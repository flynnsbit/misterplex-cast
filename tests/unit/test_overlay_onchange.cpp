// The native-overlay publisher's lab flags (host/libmisterplex/overlay_publish.hpp). The
// same test runs on every daemon line (480i avmaster, 480p avsync 5D45 and 5D52, 240p):
//   MPX_OVERLAY_ONCHANGE=1       send on change (+1 confirm), keepalive, forced, retry
//   MPX_OVERLAY_DECOUPLE=1       the overlay SPI send outside presentMu_, hot-path SPI
//                                error text non-blocking (rel-480i-costall-1004)
//   MPX_OVERLAY_SKIP_NO_PLANE=1  no publisher and no hide on a core without the plane
// Every overlay send is one SPI download to index 6, and each one SIGSTOPs Main_MiSTer.
//
// The replay is a typical 10-minute play through the real publisher pieces
// (PlaybackOverlay, buildOverlayModel, fpga_overlay::encode/send, overlayContentKey,
// OverlayPublishGate, publishOverlay, overlayPublisherWanted), ticked every 200 ms as
// runOverlayPublisher is. A skip ends the session as seekMs does: the publisher exits at
// its next wake, release sends the hide (next sequence), the session ends, and
// playInternal shows the overlay and starts a new session (new epoch and nonce,
// sequence from 1, a new gate) after a reconnect, with a second of buffering.
// The receiver follows playback_overlay_plane.sv: a packet is taken only with a valid
// session identity and a newer sequence (fpga_overlay::validate); a session change or
// plane reset fences the plane (blocked, then cleared) until a frame edge; a packet
// staged before a frame edge commits at it; a packet arriving while the plane is not
// ready waits (an HPS that honours ioctl_wait) or is rejected (one that does not:
// FpgaSpi::spiWriteBytes writes without an ACK, so both receivers are run).
// The pixels at every displayed frame are the committed packet's drawn bytes (all but
// the sequence fields when Visible is set, nothing otherwise) hashed with std::hash, not
// overlayContentKey, so the comparison does not trust the gate's key. Every run ticks
// on the same 200 ms grid (the daemon sleeps 200 ms after a send, so flag off really
// ticks every ~208 ms and flag on every 200 ms while it skips: never later).
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/fpga_terminal.hpp"
#include "libmisterplex/overlay_publish.hpp"
#include "libmisterplex/spi_window.hpp"

#include "fpga_spi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace misterplex;

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

static int64_t monoUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// FpgaSpi::sendFileTx as fpga_overlay::send calls it: every overlay SPI download.
struct CountingSender {
    int index6 = 0, other = 0;
    bool fail = false;
    fpga_overlay::Packet last{};
    bool sendFileTx(const uint8_t* data, size_t len, uint8_t index) {
        (index == fpga_overlay::kIoctlIndex ? index6 : other)++;
        if (fail)
            return false;
        if (len == last.size())
            std::memcpy(last.data(), data, len);
        return true;
    }
};

// ---------------------------------------------------------------- the replay

constexpr int64_t kBaseMs = 10000000;    // monotonic ms at play start
constexpr int64_t kTickMs = 200;         // the publisher's sleep
constexpr int64_t kSendMs = 8;           // one SPI window (Main parked, 576 bytes)
constexpr int64_t kPlayMs = 600000;      // 10 minutes
constexpr int64_t kDurationMs = 1800000; // a 30-minute title
constexpr int64_t kStartMs = 60000;      // resumed at 1:00
constexpr int64_t kCapsMs = 5;           // session begin -> startOverlay (first tick)
constexpr int64_t kBufferMs = 1000;      // session begin -> first picture
constexpr int64_t kEndMs = 30;           // release hide -> endBitstreamSession
constexpr int64_t kShowMs = 40;          // release -> playInternal's show (next session)
constexpr int64_t kPauseMs = 120000, kResumeMs = 130000;
constexpr int64_t kOkShowMs[] = {60000, 400000}; // OK presses
constexpr uint64_t kEpoch = 0x1234567890abcdefull, kNonce = 0x0fedcba987654321ull;
constexpr uint32_t kPlaneBuild = 0x4801F40Cu; // 480i: FPGA_VIDEO_320, plane built
constexpr uint32_t kLeanBuild = 0x48005D52u;  // 480p PLEX_480P_LEAN: no plane

struct Skip {
    int64_t ms, deltaMs;
};
constexpr Skip kSkips[] = {{200000, 30000}, {540000, -10000}};

enum class Fault { RejectAt, FailAt, ResetAt, ResetOkAt };
enum class Receiver { Waits, Rejects };

struct FaultEvent {
    Fault kind;
    int64_t ms;
};

struct ReplayCfg {
    bool onChange = false;
    bool decoupled = false;
    int64_t keepaliveMs = kOverlayKeepaliveDefaultMs;
    Receiver receiver = Receiver::Waits;
    int64_t restartMs = 300; // release -> the next session's begin (reconnect, CAPS)
    int64_t playMs = kPlayMs;
    OverlaySkipNoPlaneSetting skipNoPlane = OverlaySkipNoPlaneSetting::Off;
    uint32_t buildId = kPlaneBuild;
    std::vector<FaultEvent> faults;
};

struct TickRec {
    int64_t ms;
    int session;
    int state; // 0 hidden, 1 visible playing, 2 paused, 3 buffering
    int64_t posS;
};

struct ReplayOut {
    std::vector<size_t> frames;  // per displayed frame: std::hash of the drawn bytes, 0 = none
    std::vector<int64_t> sendMs; // publisher send attempts, by tick time
    std::vector<int64_t> hideMs; // release hides
    std::vector<int64_t> beginMs;
    std::vector<TickRec> ticks;
    OverlayPublishCounts counts;
    int rejected = 0, waited = 0, lockedSends = 0, unlockedSends = 0, failedSends = 0;
    std::vector<int64_t> rejectedUs, waitedUs; // arrival times of rejected / waiting packets
    int index6 = 0;     // overlay SPI downloads (CountingSender)
    int publishers = 0; // sessions that started the publisher
};

static size_t drawnHash(const fpga_overlay::Packet& p) {
    if (!(fpga_overlay::get16(p.data() + 28) & fpga_overlay::Visible))
        return 0;
    std::string drawn(reinterpret_cast<const char*>(p.data()) + 28, 32);
    drawn.append(reinterpret_cast<const char*>(p.data()) + 64, p.size() - 64);
    const size_t h = std::hash<std::string>{}(drawn);
    return h ? h : 1;
}

// The first pending fault time at or before t is used up.
static bool consume(std::vector<int64_t>& at, int64_t t) {
    for (auto it = at.begin(); it != at.end(); ++it)
        if (*it <= t) {
            at.erase(it);
            return true;
        }
    return false;
}

struct ReplayArrival {
    int64_t us;
    fpga_overlay::Packet packet;
    bool reject;
};

// The plane's session identity (session_active, epoch, nonce) over time, and resets.
struct ReplayIdent {
    int64_t us;
    bool active;
    uint64_t epoch, nonce;
    bool reset;
};

static void runReceiver(const ReplayCfg& cfg, std::vector<ReplayArrival>& arrivals,
                        std::vector<ReplayIdent>& idents, ReplayOut& out);

static ReplayOut runReplay(const ReplayCfg& cfg) {
    ReplayOut out;
    PlaybackOverlay overlay;
    uint64_t forceGen = 0;
    std::string transport;
    int64_t transportUntil = -1;
    std::vector<ReplayArrival> arrivals;
    std::vector<ReplayIdent> idents;
    std::vector<int64_t> rejectAt, failAt, okAt;
    for (const auto& f : cfg.faults) {
        if (f.kind == Fault::RejectAt)
            rejectAt.push_back(f.ms);
        else if (f.kind == Fault::FailAt)
            failAt.push_back(f.ms);
        else {
            idents.push_back({f.ms * 1000, false, 0, 0, true});
            if (f.kind == Fault::ResetOkAt)
                okAt.push_back(f.ms);
        }
    }
    struct Session {
        int64_t beginMs, pubStartMs, presentMs, startPosMs;
        uint64_t epoch, nonce;
        bool publisher;
    };
    std::vector<Session> sessions;
    auto positionAt = [](const Session& s, int64_t t) {
        if (t < s.presentMs)
            return s.startPosMs;
        int64_t played = t - s.presentMs;
        const int64_t a = std::max(s.presentMs, kPauseMs), b = std::min(t, kResumeMs);
        if (b > a)
            played -= b - a;
        return s.startPosMs + played;
    };
    CountingSender spi;
    OverlayPublishGate gate(cfg.onChange, cfg.keepaliveMs * 1000);
    uint32_t sequence = 0;
    bool lockHeld = false; // presentMu_
    struct StubLock {
        bool& held;
        explicit StubLock(bool& h) : held(h) { held = true; }
        ~StubLock() { held = false; }
    };
    int cur = -1;
    int64_t stopAt = -1, nextShowMs = -1, nextBeginMs = -1, nextPosMs = 0, skipDelta = 0;
    size_t skipIdx = 0;
    // fpgaThreadMain: the session begins; the publisher starts at the first AU unless
    // MPX_OVERLAY_SKIP_NO_PLANE rules it out for this core (overlayPublisherWanted).
    auto beginSession = [&](int64_t t, int64_t startPos) {
        Session s;
        s.beginMs = t;
        s.pubStartMs = t + kCapsMs;
        s.presentMs = t + kBufferMs;
        s.startPosMs = startPos;
        s.epoch = kEpoch + sessions.size();
        s.nonce = kNonce + 0x101 * sessions.size();
        s.publisher = overlayPublisherWanted(cfg.skipNoPlane, cfg.buildId);
        sessions.push_back(s);
        idents.push_back({t * 1000, true, s.epoch, s.nonce, false});
        out.beginMs.push_back(t);
        out.publishers += s.publisher ? 1 : 0;
        gate = OverlayPublishGate(cfg.onChange, cfg.keepaliveMs * 1000);
        sequence = 0;
        cur = static_cast<int>(sessions.size()) - 1;
    };
    auto show = [&](PlaybackOverlayState st, int64_t pos, int64_t t) {
        overlay.showAt(st, pos, kDurationMs, kBaseMs + t);
        ++forceGen; // MediaPlayer::showPlaybackOverlay
    };
    // Play start: playInternal shows the overlay, then the thread begins the session.
    show(PlaybackOverlayState::Playing, kStartMs, 0);
    beginSession(0, kStartMs);
    for (int64_t t = 0; t < cfg.playMs; ++t) {
        // MediaPlayer events at t come before a publisher tick at t.
        const int64_t pos = cur >= 0 ? positionAt(sessions[cur], t) : nextPosMs;
        for (int64_t ok : kOkShowMs)
            if (t == ok)
                show(PlaybackOverlayState::Playing, pos, t);
        if (t == kPauseMs)
            show(PlaybackOverlayState::Paused, pos, t);
        if (t == kResumeMs)
            show(PlaybackOverlayState::Playing, pos, t);
        if (consume(okAt, t)) // an OK press: MediaPlayer::showPlaybackOverlay, current state
            show(t >= kPauseMs && t < kResumeMs ? PlaybackOverlayState::Paused
                                                : PlaybackOverlayState::Playing,
                 pos, t);
        if (skipIdx < std::size(kSkips) && t == kSkips[skipIdx].ms && cur >= 0) {
            // seekMs: flashPlaybackSkip (feedback + force), then playInternal stops it.
            skipDelta = kSkips[skipIdx++].deltaMs;
            overlay.flashSkipAt(skipDelta, pos, kDurationMs, kBaseMs + t);
            transport = std::string(skipDelta >= 0 ? ">> " : "<< ") +
                        std::to_string(std::llabs(skipDelta) / 1000) + "S";
            transportUntil = t + PlaybackOverlay::kSkipVisibleMs;
            ++forceGen;
            stopAt = t;
        }
        if (cur < 0) {
            if (t == nextShowMs)
                show(PlaybackOverlayState::Playing, nextPosMs, t);
            if (t == nextBeginMs)
                beginSession(t, nextPosMs);
            continue;
        }
        const Session& s = sessions[static_cast<size_t>(cur)];
        const bool tick = s.publisher && t >= s.pubStartMs && (t - s.pubStartMs) % kTickMs == 0;
        if (stopAt >= 0 && (!s.publisher || tick)) {
            // finishFpgaPlayback: quiesce (the sleeping publisher exits at this wake),
            // then release: the hide under presentMu_ with the next sequence (only if a
            // publisher ran: overlayNonce), then the session ends.
            if (s.publisher) {
                StubLock present(lockHeld);
                if (fpga_overlay::send(spi, s.epoch, s.nonce, sequence, hiddenOverlayModel()))
                    arrivals.push_back({(t + kSendMs) * 1000, spi.last, false});
                out.hideMs.push_back(t);
            }
            idents.push_back({(t + kEndMs) * 1000, false, 0, 0, false});
            nextPosMs = positionAt(s, stopAt) + skipDelta;
            nextShowMs = t + kShowMs;
            nextBeginMs = t + cfg.restartMs;
            stopAt = -1;
            cur = -1;
            continue;
        }
        if (!tick)
            continue;
        OverlayInputs in;
        in.presented = t >= s.presentMs;
        in.paused = t >= kPauseMs && t < kResumeMs;
        in.visible = overlay.visibleAt(kBaseMs + t);
        in.positionMs = positionAt(s, t);
        in.durationMs = kDurationMs;
        in.title = "Big Buck Bunny (2008)";
        if (t < transportUntil)
            in.transport = transport;
        const fpga_overlay::Model model = buildOverlayModel(in);
        out.ticks.push_back({t, cur,
                             !in.presented ? 3
                             : in.paused   ? 2
                             : model.state == fpga_overlay::State::Hidden ? 0 : 1,
                             in.positionMs / 1000});
        const uint64_t key =
            gate.onChange()
                ? overlayContentKey(fpga_overlay::encode(s.epoch, s.nonce, sequence + 1, model))
                : 0;
        const OverlayPublishReason why = gate.decide(key, forceGen, (kBaseMs + t) * 1000);
        out.counts.note(why);
        if (why == OverlayPublishReason::None)
            continue;
        out.sendMs.push_back(t);
        const bool fail = consume(failAt, t), reject = consume(rejectAt, t);
        const OverlayPublish r = publishOverlay(
            cfg.decoupled, [&] { return StubLock(lockHeld); }, [] { return true; },
            [&] {
                (lockHeld ? out.lockedSends : out.unlockedSends)++;
                spi.fail = fail; // the SPI window failed: nothing reached the core
                const bool ok = fpga_overlay::send(spi, s.epoch, s.nonce, sequence, model);
                spi.fail = false;
                if (ok)
                    arrivals.push_back({(t + kSendMs) * 1000, spi.last, reject});
                return ok;
            });
        if (r != OverlayPublish::Sent) {
            ++out.failedSends;
            ++out.counts.failed;
        }
        gate.sent(why, key, forceGen, r == OverlayPublish::Sent, (kBaseMs + t + kSendMs) * 1000);
    }
    out.index6 = spi.index6;
    runReceiver(cfg, arrivals, idents, out);
    return out;
}

// playback_overlay_plane.sv, frame by frame (edges every 1001/30 ms).
static void runReceiver(const ReplayCfg& cfg, std::vector<ReplayArrival>& arrivals,
                        std::vector<ReplayIdent>& idents, ReplayOut& out) {
    std::stable_sort(idents.begin(), idents.end(),
                     [](const ReplayIdent& a, const ReplayIdent& b) { return a.us < b.us; });
    bool active = false, fence = false;
    uint64_t epoch = 0, nonce = 0;
    uint32_t accepted = 0;
    std::optional<fpga_overlay::Packet> staged, applied, waiting;
    auto take = [&](const fpga_overlay::Packet& p) {
        // valid_header: this session's identity and a newer sequence.
        if (!fpga_overlay::validate(p.data(), p.size(), epoch, nonce, accepted)) {
            ++out.rejected;
            out.rejectedUs.push_back(-1);
            return;
        }
        staged = p;
        accepted = fpga_overlay::get32(p.data() + 24);
    };
    constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
    size_t ai = 0, ii = 0;
    for (int64_t k = 0;; ++k) {
        const int64_t edgeUs = k * 1001000 / 30;
        if (edgeUs >= cfg.playMs * 1000)
            break;
        for (;;) {
            const int64_t a = ai < arrivals.size() ? arrivals[ai].us : kNever;
            const int64_t i = ii < idents.size() ? idents[ii].us : kNever;
            if (std::min(a, i) >= edgeUs)
                break;
            if (i <= a) {
                const ReplayIdent& e = idents[ii++];
                if (e.reset) { // the plane's reset: fence registers and sequence cleared
                    accepted = 0;
                    fence = true;
                    staged.reset();
                    applied.reset();
                } else if (e.active != active || e.epoch != epoch || e.nonce != nonce) {
                    if (e.epoch != epoch || e.nonce != nonce)
                        accepted = 0;
                    active = e.active;
                    epoch = e.epoch;
                    nonce = e.nonce;
                    fence = true; // identity_changed: blocked until the clear commits
                }
                continue;
            }
            const ReplayArrival& in = arrivals[ai++];
            if (!(active && epoch && nonce) || in.reject) { // nothing waits, or malformed
                ++out.rejected;
                out.rejectedUs.push_back(in.us);
                continue;
            }
            if (staged || fence) { // not ready: ioctl_wait
                if (cfg.receiver == Receiver::Waits && !waiting) {
                    waiting = in.packet;
                    ++out.waited;
                    out.waitedUs.push_back(in.us);
                } else {
                    ++out.rejected;
                    out.rejectedUs.push_back(in.us);
                }
                continue;
            }
            take(in.packet);
        }
        // The frame edge: the pixel side takes the staged packet; a fence's clear goes
        // once nothing is pending and commits at an edge (one later after a packet).
        const bool committed = staged.has_value();
        if (committed) {
            applied = staged;
            staged.reset();
        }
        if (fence && !committed) {
            applied.reset();
            fence = false;
        }
        if (waiting && !fence) { // the waiting transfer goes on once the plane is ready
            const fpga_overlay::Packet p = *waiting;
            waiting.reset();
            if (active && epoch && nonce) {
                take(p);
            } else {
                ++out.rejected;
                out.rejectedUs.push_back(-1);
            }
        }
        out.frames.push_back(!active || fence || !applied ? 0 : drawnHash(*applied));
    }
}

static int mismatchedFrames(const ReplayOut& a, const ReplayOut& b, int64_t* firstMs = nullptr,
                            int64_t* lastMs = nullptr) {
    int n = 0;
    for (size_t i = 0; i < a.frames.size() && i < b.frames.size(); ++i)
        if (a.frames[i] != b.frames[i]) {
            const int64_t ms = static_cast<int64_t>(i) * 1001 / 30;
            if (!n && firstMs)
                *firstMs = ms;
            if (lastMs)
                *lastMs = ms;
            ++n;
        }
    const size_t extra = a.frames.size() > b.frames.size() ? a.frames.size() - b.frames.size()
                                                           : b.frames.size() - a.frames.size();
    return n + static_cast<int>(extra);
}

static std::vector<int> perMinute(const ReplayOut& o) {
    std::vector<int> m(static_cast<size_t>(kPlayMs / 60000), 0);
    for (int64_t t : o.sendMs)
        if (t / 60000 < static_cast<int64_t>(m.size()))
            ++m[static_cast<size_t>(t / 60000)];
    return m;
}

static std::string minuteList(const std::vector<int>& m) {
    std::string s;
    for (size_t i = 0; i < m.size(); ++i)
        s += (i ? " " : "") + std::to_string(m[i]);
    return s;
}

// Sends per second while the overlay is in `state` (tick states), and the seconds.
static double sendsPerSecond(const ReplayOut& o, int state, double* seconds = nullptr) {
    std::map<int64_t, int> stateAt;
    int ticks = 0, sends = 0;
    for (const TickRec& r : o.ticks) {
        stateAt[r.ms] = r.state;
        if (r.state == state)
            ++ticks;
    }
    for (int64_t t : o.sendMs)
        if (stateAt.count(t) && stateAt[t] == state)
            ++sends;
    const double secs = ticks * kTickMs / 1000.0;
    if (seconds)
        *seconds = secs;
    return secs > 0 ? sends / secs : 0.0;
}

// The overlay publisher of one session through the real thread (OverlayPublisherThread
// + runOverlayPublisher) and finishFpgaPlayback, wired as fpgaThreadMain does: the
// decision at the first AU (overlayPublisherWanted), startOverlay, quiesce, release.
struct ThreadPlay {
    int index6 = 0, ticks = 0, hides = 0;
    int64_t quiesceUs = 0; // finishFpgaPlayback's quiesce: stop and join the publisher
};

// The publisher loop's own paths through the real OverlayPublisherThread: a scripted
// publish result per call ('S' sent, 'F' failed, 'X' throws, 'T' stopped before the
// send; 'S' after the script), until `attempts` publish calls were made.
struct LoopRun {
    int publishes = 0, counted = 0, countedSent = 0, failSessions = 0;
    std::string failWhy;
    bool doneRan = false;
    OverlayPublishCounts counts;
    std::vector<OverlayPublishReason> reasons; // per publish call
};

static LoopRun runLoop(const std::string& script, bool onChange, int attempts) {
    LoopRun r;
    std::mutex mu;
    std::atomic<int> calls{0};
    std::atomic<bool> finished{false};
    const int64_t untilUs = monoUs() + 3000000;
    OverlayPublisherThread t;
    OverlayPublisherHooks h;
    h.live = [&] { return calls.load() < attempts && monoUs() < untilUs; };
    h.inputs = [&] {
        OverlayInputs in;
        in.presented = true;
        in.visible = true; // the clock shows: new content after every publish call
        in.positionMs = 1000 * static_cast<int64_t>(calls.load());
        in.durationMs = kDurationMs;
        in.title = "T";
        return in;
    };
    h.contentKey = [](const fpga_overlay::Model& m) {
        return overlayContentKey(fpga_overlay::encode(kEpoch, kNonce, 1, m));
    };
    h.forceGen = [] { return uint64_t(0); };
    h.nowUs = [] { return monoUs(); };
    h.publish = [&](OverlayPublishReason why, const fpga_overlay::Model&) {
        const int i = calls.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(mu);
            r.reasons.push_back(why);
        }
        const char c = i < static_cast<int>(script.size()) ? script[static_cast<size_t>(i)] : 'S';
        if (c == 'X')
            throw 42; // e.g. std::bad_alloc in the model or the SPI path
        return c == 'S' ? OverlayPublish::Sent
               : c == 'T' ? OverlayPublish::Stopped
                          : OverlayPublish::Failed;
    };
    h.tick = [] {};
    h.counted = [&](bool sent) {
        std::lock_guard<std::mutex> lock(mu);
        ++r.counted;
        r.countedSent += sent ? 1 : 0;
    };
    h.skipped = [] {};
    h.failSession = [&](const char* why) {
        std::lock_guard<std::mutex> lock(mu);
        ++r.failSessions;
        r.failWhy = why;
    };
    t.start(std::move(h), OverlayPublishGate(onChange, 5000000), nullptr,
            [&](const OverlayPublishGate&, const OverlayPublishCounts& c) {
                std::lock_guard<std::mutex> lock(mu);
                r.counts = c;
                r.doneRan = true;
                finished = true;
            });
    while (!finished.load() && monoUs() < untilUs + 1000000)
        std::this_thread::yield();
    t.quiesce();
    r.publishes = calls.load();
    return r;
}

static ThreadPlay playThroughThread(OverlaySkipNoPlaneSetting setting, uint32_t buildId,
                                    bool onChange, int wantTicks, bool constantContent = false) {
    CountingSender spi;
    std::mutex spiMu; // FpgaSpi's lock: the publisher thread and release share the sender
    uint64_t overlayNonce = 0;
    uint32_t sequence = 0;
    std::atomic<int> ticks{0};
    OverlayPublisherThread publisher;
    auto startOverlay = [&](uint64_t nonce) {
        overlayNonce = nonce;
        OverlayPublisherHooks h;
        const int64_t liveUntilUs = monoUs() + 5000000; // a mutant that ignores stop ends
        h.live = [liveUntilUs] { return monoUs() < liveUntilUs; };
        h.inputs = [&] {
            OverlayInputs in;
            in.presented = true;
            in.visible = true; // the clock moves every tick: content changes every tick
            in.positionMs = constantContent ? 61000 : 1000 * static_cast<int64_t>(ticks.load());
            in.durationMs = kDurationMs;
            in.title = "T";
            return in;
        };
        h.contentKey = [&, nonce](const fpga_overlay::Model& m) {
            return overlayContentKey(fpga_overlay::encode(kEpoch, nonce, sequence + 1, m));
        };
        h.forceGen = [] { return uint64_t(0); };
        h.nowUs = [] { return monoUs(); };
        h.publish = [&, nonce](OverlayPublishReason, const fpga_overlay::Model& m) {
            std::lock_guard<std::mutex> lock(spiMu);
            return fpga_overlay::send(spi, kEpoch, nonce, sequence, m) ? OverlayPublish::Sent
                                                                       : OverlayPublish::Failed;
        };
        h.tick = [&] {
            ticks.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        h.counted = [](bool) {};
        h.skipped = [] {};
        h.failSession = [](const char*) {};
        publisher.start(std::move(h), OverlayPublishGate(onChange, 5000000), nullptr, nullptr);
    };
    // The first AU (media_player.cpp): log the notice, then maybe start the publisher.
    if (overlayPublisherWanted(setting, buildId))
        startOverlay(kNonce);
    const int64_t until = monoUs() + 3000000;
    while (ticks.load() < wantTicks && overlayNonce && monoUs() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!overlayNonce)
        std::this_thread::sleep_for(std::chrono::milliseconds(wantTicks)); // the same play
    ThreadPlay r;
    finishFpgaPlayback(
        FpgaTerminalInput{true, true, true, true}, [] { return std::string(); },
        [&] {
            const int64_t t0 = monoUs();
            publisher.quiesce();
            r.quiesceUs = monoUs() - t0;
        },
        [&] {
            if (overlayNonce) { // release: the hide only after a publisher ran
                std::lock_guard<std::mutex> lock(spiMu);
                ++r.hides;
                fpga_overlay::send(spi, kEpoch, overlayNonce, sequence, hiddenOverlayModel());
            }
            return true;
        },
        [] { return false; }, [] { return true; }, [](const FpgaTerminalReceipt&) {});
    r.index6 = spi.index6;
    r.ticks = ticks.load();
    return r;
}

int main() {
    // ------------------------------------------------------------ settings
    {
        OverlayOnChangeSettings s = parseOverlayOnChange(nullptr, nullptr);
        CHECK(!s.on && !s.onInvalid && s.keepaliveMs == 5000 && !s.keepaliveInvalid);
        CHECK(!parseOverlayOnChange("0", nullptr).on && parseOverlayOnChange("1", nullptr).on);
        for (const char* bad : {"2", "yes", " 1", "true"}) {
            s = parseOverlayOnChange(bad, nullptr);
            CHECK(!s.on && s.onInvalid);
        }
        CHECK(parseOverlayOnChange("1", "2500").keepaliveMs == 2500);
        CHECK(parseOverlayOnChange("1", "200").keepaliveMs == 200);
        CHECK(parseOverlayOnChange("1", "600000").keepaliveMs == 600000);
        for (const char* bad : {"199", "600001", "abc", "-5", "5e3", "99999999"}) {
            s = parseOverlayOnChange("1", bad);
            CHECK(s.keepaliveInvalid && s.keepaliveMs == 5000);
        }
        CHECK(formatOverlayOnChangeNotice(parseOverlayOnChange(nullptr, nullptr), nullptr, nullptr).empty());
        CHECK(formatOverlayOnChangeNotice(parseOverlayOnChange("1", nullptr), "1", nullptr) ==
              "media: LAB MPX_OVERLAY_ONCHANGE=1 -> overlay sent on change (+1 confirm), keepalive "
              "5000 ms, forced on show/skip feedback, retried after a failed send");
        CHECK(formatOverlayOnChangeNotice(parseOverlayOnChange("yes", "9"), "yes", "9") ==
              "media: LAB MPX_OVERLAY_ONCHANGE='yes' invalid (want 0 or 1) -> off\n"
              "media: LAB MPX_OVERLAY_KEEPALIVE_MS='9' invalid (want 200..600000) -> 5000");
        unsetenv(kOverlayOnChangeEnv);
        unsetenv(kOverlayKeepaliveEnv);
        CHECK(!overlayOnChangeFromEnvironment().on);
        setenv(kOverlayOnChangeEnv, "1", 1);
        setenv(kOverlayKeepaliveEnv, "3000", 1);
        CHECK(overlayOnChangeFromEnvironment().on && overlayOnChangeFromEnvironment().keepaliveMs == 3000);
        unsetenv(kOverlayOnChangeEnv);
        unsetenv(kOverlayKeepaliveEnv);
        using D = OverlayDecoupleSetting;
        CHECK(parseOverlayDecouple(nullptr) == D::Off && parseOverlayDecouple("0") == D::Off &&
              parseOverlayDecouple("1") == D::On && parseOverlayDecouple("on") == D::Invalid);
        using N = OverlaySkipNoPlaneSetting;
        CHECK(parseOverlaySkipNoPlane(nullptr) == N::Off && parseOverlaySkipNoPlane("") == N::Off &&
              parseOverlaySkipNoPlane("0") == N::Off && parseOverlaySkipNoPlane("1") == N::On);
        for (const char* bad : {"2", "yes", " 1", "true", "01"})
            CHECK(parseOverlaySkipNoPlane(bad) == N::Invalid);
        unsetenv(kOverlaySkipNoPlaneEnv);
        CHECK(overlaySkipNoPlaneFromEnvironment() == N::Off);
        setenv(kOverlaySkipNoPlaneEnv, "1", 1);
        CHECK(overlaySkipNoPlaneFromEnvironment() == N::On);
        unsetenv(kOverlaySkipNoPlaneEnv);
        CHECK(std::strcmp(kOverlaySkipNoPlaneEnv, "MPX_OVERLAY_SKIP_NO_PLANE") == 0);
    }

    // ------------------------------------------------------------ lab publisher: fails closed
    // Only a flag set to exactly "1" selects the shared publisher; every other value of
    // every flag, in any mix, keeps the prod publisher (rel bug-nits 1004).
    {
        const char* offs[] = {nullptr, "", "0", "yes", "2", " 1", "1 ", "true", "on",
                              "01", "-1", "1\n", "0x1", "１"};
        int combos = 0;
        for (const char* d : offs)
            for (const char* c : offs)
                for (const char* n : offs) {
                    CHECK(!overlayLabPublisherSelected(parseOverlayDecouple(d),
                                                       parseOverlayOnChange(c, "yes"),
                                                       parseOverlaySkipNoPlane(n)));
                    ++combos;
                }
        CHECK(combos == 14 * 14 * 14);
        for (const char* other : offs) {
            CHECK(overlayLabPublisherSelected(parseOverlayDecouple("1"), parseOverlayOnChange(other, nullptr),
                                              parseOverlaySkipNoPlane(other)));
            CHECK(overlayLabPublisherSelected(parseOverlayDecouple(other), parseOverlayOnChange("1", other),
                                              parseOverlaySkipNoPlane(other)));
            CHECK(overlayLabPublisherSelected(parseOverlayDecouple(other), parseOverlayOnChange(other, nullptr),
                                              parseOverlaySkipNoPlane("1")));
        }
        // Through the environment, as media_player.cpp reads it.
        const char* envs[] = {kOverlayDecoupleEnv, kOverlayOnChangeEnv, kOverlaySkipNoPlaneEnv};
        auto fromEnv = [] {
            return overlayLabPublisherSelected(overlayDecoupleFromEnvironment(),
                                               overlayOnChangeFromEnvironment(),
                                               overlaySkipNoPlaneFromEnvironment());
        };
        for (const char* e : envs)
            unsetenv(e);
        CHECK(!fromEnv());
        for (const char* e : envs) {
            for (const char* bad : {"", "yes", "2", " 1"}) {
                setenv(e, bad, 1);
                CHECK(!fromEnv());
                unsetenv(e);
            }
            setenv(e, "1", 1);
            CHECK(fromEnv());
            unsetenv(e);
        }
        setenv(kOverlayKeepaliveEnv, "3000", 1);
        CHECK(!fromEnv());
        unsetenv(kOverlayKeepaliveEnv);
    }

    // ------------------------------------------------------------ the content key
    {
        OverlayInputs in;
        in.presented = true;
        in.durationMs = kDurationMs;
        in.title = "T";
        in.positionMs = 61000;
        auto keyOf = [&](const OverlayInputs& x, uint32_t seq) {
            return overlayContentKey(fpga_overlay::encode(kEpoch, kNonce, seq, buildOverlayModel(x)));
        };
        // Hidden: the clock and title in the packet do not count; the sequence never does.
        const uint64_t hidden = keyOf(in, 1);
        OverlayInputs later = in;
        later.positionMs = 62000;
        later.title = "Other";
        CHECK(keyOf(later, 2) == hidden);
        // Visible: the clock, the transport text and the state count; the sequence does not.
        OverlayInputs vis = in;
        vis.visible = true;
        const uint64_t visible = keyOf(vis, 1);
        CHECK(visible != hidden && keyOf(vis, 77) == visible);
        OverlayInputs tick = vis;
        tick.positionMs = 62000;
        CHECK(keyOf(tick, 2) != visible);
        OverlayInputs within = vis;
        within.positionMs = 61400; // same second, same progress pixel: nothing drawn changes
        CHECK(keyOf(within, 2) == visible);
        OverlayInputs feedback = vis;
        feedback.transport = ">> 30S";
        CHECK(keyOf(feedback, 2) != visible);
        OverlayInputs paused = in;
        paused.paused = true; // paused shows even when not "visible"
        CHECK(keyOf(paused, 2) != hidden && keyOf(paused, 2) != visible);
        // Another session: a different key even when hidden.
        CHECK(overlayContentKey(fpga_overlay::encode(kEpoch + 1, kNonce, 1, buildOverlayModel(in))) != hidden);
        // Placement, scale, colours, width and the progress bar are drawn: each counts
        // when visible, none when hidden.
        auto modelKey = [](const fpga_overlay::Model& m) {
            return overlayContentKey(fpga_overlay::encode(kEpoch, kNonce, 1, m));
        };
        const fpga_overlay::Model vm = buildOverlayModel(vis), hm = buildOverlayModel(in);
        CHECK(modelKey(vm) == visible && modelKey(hm) == hidden);
        const std::function<void(fpga_overlay::Model&)> drawnChanges[] = {
            [](fpga_overlay::Model& m) { m.x = 16; },
            [](fpga_overlay::Model& m) { m.y = 180; },
            [](fpga_overlay::Model& m) { m.scale = 2; },
            [](fpga_overlay::Model& m) { m.foreground = 0xffffff; },
            [](fpga_overlay::Model& m) { m.background = 0x000000; },
            [](fpga_overlay::Model& m) { m.accent = 0x00ff00; },
            [](fpga_overlay::Model& m) { m.flags &= ~fpga_overlay::AutoFit; m.width = 200; },
            [](fpga_overlay::Model& m) { m.flags &= ~fpga_overlay::Progress; },
        };
        for (const auto& change : drawnChanges) {
            fpga_overlay::Model v = vm, h = hm;
            change(v);
            change(h);
            CHECK(modelKey(v) != modelKey(vm));
            if (!(h.flags & fpga_overlay::Visible))
                CHECK(modelKey(h) == hidden);
        }
        // The model builder matches the daemon's rules.
        CHECK(buildOverlayModel(in).state == fpga_overlay::State::Hidden && buildOverlayModel(in).flags == 0);
        OverlayInputs buffering;
        CHECK(buildOverlayModel(buffering).state == fpga_overlay::State::Buffering &&
              buildOverlayModel(buffering).position_ms == 0 && buildOverlayModel(buffering).title == "MiSTerPlex");
        CHECK(buildOverlayModel(paused).state == fpga_overlay::State::Paused);
        CHECK(hiddenOverlayModel().state == fpga_overlay::State::Hidden && hiddenOverlayModel().flags == 0);
    }

    // ------------------------------------------------------------ the gate
    {
        const int64_t s = 1000000;
        OverlayPublishGate off(false, 5000000);
        CHECK(off.decide(1, 0, s) == OverlayPublishReason::Periodic);
        off.sent(OverlayPublishReason::Periodic, 1, 0, true, s);
        CHECK(off.decide(1, 0, s + 200000) == OverlayPublishReason::Periodic);

        OverlayPublishGate g(true, 5000000);
        CHECK(g.decide(1, 0, s) == OverlayPublishReason::First);
        g.sent(OverlayPublishReason::First, 1, 0, true, s);
        CHECK(g.decide(1, 0, s + 200000) == OverlayPublishReason::Confirm);
        g.sent(OverlayPublishReason::Confirm, 1, 0, true, s + 200000);
        CHECK(g.decide(1, 0, s + 400000) == OverlayPublishReason::None);
        CHECK(g.decide(1, 0, s + 5199999) == OverlayPublishReason::None);
        CHECK(g.decide(1, 0, s + 5200000) == OverlayPublishReason::Keepalive);
        g.sent(OverlayPublishReason::Keepalive, 1, 0, true, s + 5200000);
        CHECK(g.decide(1, 0, s + 5400000) == OverlayPublishReason::None); // no confirm after a keepalive
        CHECK(g.decide(2, 0, s + 5400000) == OverlayPublishReason::Changed);
        g.sent(OverlayPublishReason::Changed, 2, 0, true, s + 5400000);
        CHECK(g.decide(3, 0, s + 5600000) == OverlayPublishReason::Changed); // a change beats the confirm
        g.sent(OverlayPublishReason::Changed, 3, 0, true, s + 5600000);
        CHECK(g.decide(3, 0, s + 5800000) == OverlayPublishReason::Confirm);
        g.sent(OverlayPublishReason::Confirm, 3, 0, true, s + 5800000);
        CHECK(g.decide(3, 1, s + 6000000) == OverlayPublishReason::Forced);
        g.sent(OverlayPublishReason::Forced, 3, 1, true, s + 6000000);
        CHECK(g.decide(3, 1, s + 6200000) == OverlayPublishReason::Confirm);
        g.sent(OverlayPublishReason::Confirm, 3, 1, true, s + 6200000);
        CHECK(g.decide(4, 1, s + 6400000) == OverlayPublishReason::Changed);
        g.sent(OverlayPublishReason::Changed, 4, 1, false, s + 6400000); // SPI error
        CHECK(g.decide(4, 1, s + 6600000) == OverlayPublishReason::Retry);
        CHECK(g.decide(3, 2, s + 6600000) == OverlayPublishReason::Retry); // retry first
        g.sent(OverlayPublishReason::Retry, 4, 1, true, s + 6600000);
        CHECK(g.decide(4, 1, s + 6800000) == OverlayPublishReason::Confirm); // the retry was new content
        CHECK(std::strcmp(overlayPublishReasonName(OverlayPublishReason::Keepalive), "keepalive") == 0);
        // A failed forced send (same content) keeps its confirm through the retry.
        OverlayPublishGate f(true, 5000000);
        f.sent(OverlayPublishReason::First, 1, 0, true, s);
        f.sent(OverlayPublishReason::Confirm, 1, 0, true, s + 200000);
        CHECK(f.decide(1, 1, s + 400000) == OverlayPublishReason::Forced);
        f.sent(OverlayPublishReason::Forced, 1, 1, false, s + 400000);
        CHECK(f.decide(1, 1, s + 600000) == OverlayPublishReason::Retry);
        f.sent(OverlayPublishReason::Retry, 1, 1, true, s + 600000);
        CHECK(f.decide(1, 1, s + 800000) == OverlayPublishReason::Confirm);
        f.sent(OverlayPublishReason::Confirm, 1, 1, true, s + 800000);
        CHECK(f.decide(1, 1, s + 1000000) == OverlayPublishReason::None);
        // A failed keepalive (nothing new) is retried, without a confirm.
        CHECK(f.decide(1, 1, s + 5800000) == OverlayPublishReason::Keepalive);
        f.sent(OverlayPublishReason::Keepalive, 1, 1, false, s + 5800000);
        CHECK(f.decide(1, 1, s + 6000000) == OverlayPublishReason::Retry);
        f.sent(OverlayPublishReason::Retry, 1, 1, true, s + 6000000);
        CHECK(f.decide(1, 1, s + 6200000) == OverlayPublishReason::None);
        // A force that arrives while an unchanged keepalive is failing: the retry
        // delivers it, and it still earns its confirm.
        OverlayPublishGate k(true, 5000000);
        k.sent(OverlayPublishReason::First, 1, 0, true, s);
        k.sent(OverlayPublishReason::Confirm, 1, 0, true, s + 200000);
        CHECK(k.decide(1, 0, s + 5200000) == OverlayPublishReason::Keepalive);
        k.sent(OverlayPublishReason::Keepalive, 1, 0, false, s + 5200000);
        CHECK(k.decide(1, 1, s + 5400000) == OverlayPublishReason::Retry); // force pending
        k.sent(OverlayPublishReason::Retry, 1, 1, true, s + 5400000);
        CHECK(k.decide(1, 1, s + 5600000) == OverlayPublishReason::Confirm);
        k.sent(OverlayPublishReason::Confirm, 1, 1, true, s + 5600000);
        CHECK(k.decide(1, 1, s + 5800000) == OverlayPublishReason::None);
        // New content B fails, then the content reverts to A (the last delivered) before
        // the retry. A transfer that reports failure may still have landed (the host
        // sequence did not advance, so the receiver then rejects the retry as not
        // newer): the retry still earns a confirm, which repairs it on the next tick.
        OverlayPublishGate v(true, 5000000);
        v.sent(OverlayPublishReason::First, 1, 0, true, s);
        v.sent(OverlayPublishReason::Confirm, 1, 0, true, s + 200000);
        CHECK(v.decide(2, 0, s + 400000) == OverlayPublishReason::Changed);
        v.sent(OverlayPublishReason::Changed, 2, 0, false, s + 400000);
        CHECK(v.decide(1, 0, s + 600000) == OverlayPublishReason::Retry);
        v.sent(OverlayPublishReason::Retry, 1, 0, true, s + 600000);
        CHECK(v.decide(1, 0, s + 800000) == OverlayPublishReason::Confirm);
        // A failed first send stays First until delivered, then confirms.
        OverlayPublishGate first(true, 5000000);
        first.sent(OverlayPublishReason::First, 7, 0, false, s);
        CHECK(first.decide(7, 0, s + 200000) == OverlayPublishReason::First);
        first.sent(OverlayPublishReason::First, 7, 0, true, s + 200000);
        CHECK(first.decide(7, 0, s + 400000) == OverlayPublishReason::Confirm);
    }

    // ------------------------------------------------------------ cores without the plane
    {
        using N = OverlaySkipNoPlaneSetting;
        // Default path (MPX_OVERLAY_SKIP_NO_PLANE unset or 0, or invalid): every recognised row
        // starts the publisher and unset/0 prints no notice, whatever overlayPlaneAbsent says.
        size_t defaultRows = 0;
        for (const misterplex::FpgaBuildInfo& b : misterplex::kRecognisedFpgaBuilds) {
            ++defaultRows;
            CHECK(overlayPublisherWanted(N::Off, b.id) && overlayPublisherWanted(N::Invalid, b.id) &&
                  formatOverlaySkipNoPlaneNotice(N::Off, nullptr, b.id).empty() &&
                  formatOverlaySkipNoPlaneNotice(N::Off, "0", b.id).empty());
        }
        std::printf("overlay default path: %zu recognised rows publish, no notice\n", defaultRows);
        // LEAN (no plane): 4D57 7d3e4b5b, 4D40 47cb9fb4, F40A 8c4c83f8, 6A7E cb60f749 carry
        // PLEX_480P_LEAN=1 (Plex_480p.qsf :194/:195), so Plex.sv :1828-1834 drops the plane.
        // R1 A1FB (8ffb2335) and A152 (45ff6daf): Plex_480p_r1.qsf :217/:223 PLEX_480P_LEAN=1,
        // Plex.sv :1915-1916; the r1 D-PP1 line (94DD, 48018AE0) and the ascal fold 48032F84
        // (6b3832ac3, :255) the same, and the E-port rebake 48038D9A (99fe93e1e, :255) the same.
        for (uint32_t id : {0x48005D52u, 0x48005D50u, 0x48005D45u, 0x48005D40u, 0x4800F36Au,
                            0x48004D57u, 0x48004D40u, 0x4800F40Au, 0x48006A7Eu, 0x4800A1FBu,
                            0x4800A152u, 0x48009E36u, 0x48002731u,
                            0x4800C88Fu, 0x48003ECBu, 0x480058BCu, 0x480094DDu, 0x48018AE0u,
                            0x48032F84u, 0x48038D9Au, 0x48FF65F2u, 0x487A1BD1u, 0x48C5385Cu,
                            0x48C51FF1u, 0x48CFEE31u, 0x48110015u})
            CHECK(overlayPlaneAbsent(id) && !overlayPublisherWanted(N::On, id) &&
                  overlayPublisherWanted(N::Off, id) && overlayPublisherWanted(N::Invalid, id));
        // 2400F36A keeps the plane: 96a5c60e Plex_240p.qsf:105 FPGA_VIDEO_320=1, no LEAN.
        // F1 ship 24003997 too: dev/f1-ship-240p-c4188b00 Plex_240p.qsf FPGA_VIDEO_320=1, no LEAN.
        for (uint32_t id : {0x4801F40Cu, 0x4801F40Au, 0x48F14A0Bu, 0x48C078E2u, 0x2400F40Bu, 0x24003997u, 0x24F1A2C4u, 0x24A10026u, 0x48A64AEDu,
                            0x2400F36Au, 0x24C07821u, 0x999800CFu, 0x48005D51u, 0x48005D53u, 0u})
            CHECK(!overlayPlaneAbsent(id) && overlayPublisherWanted(N::On, id));
        CHECK(formatOverlaySkipNoPlaneNotice(N::Off, nullptr, kLeanBuild).empty());
        CHECK(formatOverlaySkipNoPlaneNotice(N::On, "1", kLeanBuild) ==
              "media: LAB MPX_OVERLAY_SKIP_NO_PLANE=1 -> BUILD_ID 0x48005D52 has no overlay plane "
              "(PLEX_480P_LEAN): no overlay publisher, no index-6 sends this session");
        CHECK(formatOverlaySkipNoPlaneNotice(N::On, "1", kPlaneBuild) ==
              "media: LAB MPX_OVERLAY_SKIP_NO_PLANE=1 -> BUILD_ID 0x4801F40C has the overlay plane: "
              "publisher unchanged");
        CHECK(formatOverlaySkipNoPlaneNotice(N::Invalid, "yes", kLeanBuild) ==
              "media: LAB MPX_OVERLAY_SKIP_NO_PLANE='yes' invalid (want 0 or 1) -> off");
        CHECK(formatOverlaySessionSends(false, 0, 0, kLeanBuild) ==
              "media: overlay session index6_sends=0 publisher=off publisher_sends=0 hide_sends=0 "
              "build=0x48005D52");
        CHECK(formatOverlaySessionSends(true, 2999, 1, kPlaneBuild) ==
              "media: overlay session index6_sends=3000 publisher=on publisher_sends=2999 "
              "hide_sends=1 build=0x4801F40C");
    }

    // ------------------------------------------------------------ a typical play, off vs on
    ReplayCfg offCfg, onCfg, bothCfg;
    onCfg.onChange = true;
    bothCfg.onChange = true;
    bothCfg.decoupled = true;
    const ReplayOut off = runReplay(offCfg);
    const ReplayOut on = runReplay(onCfg);
    const ReplayOut both = runReplay(bothCfg);
    const auto offMin = perMinute(off), onMin = perMinute(on);
    std::printf("test_overlay_onchange: typical 10 min play (shows at 0/60/400 s, pause 120-130 s, "
                "skips at 200/540 s = 3 sessions), %zu displayed frames\n", off.frames.size());
    std::printf("  publisher sends (= SPI windows = Main SIGSTOP rounds) per minute, + %zu release hides\n",
                off.hideMs.size());
    std::printf("    flag off: %s  total %zu (%.1f/min)\n", minuteList(offMin).c_str(), off.sendMs.size(),
                off.sendMs.size() / (kPlayMs / 60000.0));
    std::printf("    flag on : %s  total %zu (%.1f/min)\n", minuteList(onMin).c_str(), on.sendMs.size(),
                on.sendMs.size() / (kPlayMs / 60000.0));
    std::printf("  flag on reasons: first %llu changed %llu confirm %llu keepalive %llu forced %llu retry %llu, skipped ticks %llu\n",
                (unsigned long long)on.counts.of(OverlayPublishReason::First),
                (unsigned long long)on.counts.of(OverlayPublishReason::Changed),
                (unsigned long long)on.counts.of(OverlayPublishReason::Confirm),
                (unsigned long long)on.counts.of(OverlayPublishReason::Keepalive),
                (unsigned long long)on.counts.of(OverlayPublishReason::Forced),
                (unsigned long long)on.counts.of(OverlayPublishReason::Retry),
                (unsigned long long)on.counts.of(OverlayPublishReason::None));
    double hiddenS = 0, visibleS = 0, pausedS = 0;
    const double hiddenRate = sendsPerSecond(on, 0, &hiddenS), visibleRate = sendsPerSecond(on, 1, &visibleS),
                 pausedRate = sendsPerSecond(on, 2, &pausedS);
    std::printf("  flag on sends/s: hidden %.2f (%.0f s), visible playing %.2f (%.0f s), paused %.2f (%.0f s); "
                "flag off 5.00 everywhere\n", hiddenRate, hiddenS, visibleRate, visibleS, pausedRate, pausedS);
    ReplayCfg offRej = offCfg, onRej = onCfg;
    offRej.receiver = onRej.receiver = Receiver::Rejects;
    const ReplayOut offR = runReplay(offRej), onR = runReplay(onRej);
    const int mismatch = mismatchedFrames(off, on), mismatchBoth = mismatchedFrames(off, both),
              mismatchRej = mismatchedFrames(offR, onR);
    size_t drawn = 0;
    for (size_t h : off.frames)
        drawn += h != 0;
    std::printf("  displayed frames different from flag off: flag on %d, flag on + MPX_OVERLAY_DECOUPLE %d, "
                "flag on with a receiver that rejects instead of waiting %d (%zu frames show the overlay)\n",
                mismatch, mismatchBoth, mismatchRej, drawn);
    CHECK(off.beginMs.size() == 3 && off.hideMs.size() == 2 && off.publishers == 3);
    CHECK(off.counts.of(OverlayPublishReason::Periodic) == off.sendMs.size());
    CHECK(off.counts.of(OverlayPublishReason::None) == 0 && off.sendMs.size() == off.ticks.size());
    CHECK(off.index6 == static_cast<int>(off.sendMs.size() + off.hideMs.size()));
    CHECK(on.index6 == static_cast<int>(on.sendMs.size() + on.hideMs.size()));
    CHECK(mismatch == 0 && mismatchBoth == 0 && mismatchRej == 0 && drawn > 600);
    CHECK(on.sendMs.size() * 10 < off.sendMs.size());          // at least 10x fewer Main stops
    CHECK(onMin[5] <= 13 && onMin[8] <= 13);                    // all-hidden minutes: the keepalive
    CHECK(hiddenRate <= 0.25 && visibleRate >= 1.0 && pausedRate < 1.0);
    CHECK(off.rejected == 0 && on.rejected == 0);
    CHECK(off.lockedSends == static_cast<int>(off.sendMs.size()) && off.unlockedSends == 0);
    CHECK(both.unlockedSends == static_cast<int>(both.sendMs.size()) && both.lockedSends == 0);
    CHECK(both.sendMs == on.sendMs);
    // Every tick where the visible clock moved, a send went out on that tick.
    int clockTicks = 0;
    for (size_t i = 1; i < on.ticks.size(); ++i) {
        const TickRec &a = on.ticks[i - 1], &b = on.ticks[i];
        if (a.session != b.session || a.state != 1 || b.state != 1 || a.posS == b.posS)
            continue;
        ++clockTicks;
        CHECK(std::find(on.sendMs.begin(), on.sendMs.end(), b.ms) != on.sendMs.end());
    }
    std::printf("  visible clock changes: %d, each sent on its tick\n", clockTicks);
    CHECK(clockTicks >= 10);

    // ------------------------------------------------------------ faults
    auto pairRun = [&](const char* name, std::vector<FaultEvent> faults, int64_t* onFirst = nullptr,
                       int64_t* onLast = nullptr) {
        ReplayCfg a = offCfg, b = onCfg;
        a.faults = b.faults = faults;
        const ReplayOut fo = runReplay(a), fn = runReplay(b);
        int64_t ofFirst = -1, ofLast = -1, nFirst = -1, nLast = -1;
        const int offVsIdeal = mismatchedFrames(off, fo, &ofFirst, &ofLast);
        const int onVsIdeal = mismatchedFrames(off, fn, &nFirst, &nLast);
        const int onVsOff = mismatchedFrames(fo, fn);
        std::printf("  fault %-44s frames off the ideal: flag off %3d (%.1f s), flag on %3d (%.1f s); on vs off %d\n",
                    name, offVsIdeal, offVsIdeal ? (ofLast - ofFirst) / 1000.0 + 0.033 : 0.0, onVsIdeal,
                    onVsIdeal ? (nLast - nFirst) / 1000.0 + 0.033 : 0.0, onVsOff);
        if (onFirst)
            *onFirst = nFirst;
        if (onLast)
            *onLast = nLast;
        return std::make_pair(onVsOff, std::make_pair(offVsIdeal, onVsIdeal));
    };
    // The core rejects the hide after the 60 s show (a malformed transfer): the confirm
    // repeats it on the next tick, as the periodic send did.
    auto rejectHide = pairRun("core rejects the hide (malformed, 63 s)", {{Fault::RejectAt, 63000}});
    CHECK(rejectHide.first == 0 && rejectHide.second.second > 0 && rejectHide.second.second <= 7);
    // Two failed SPI sends at the pause: retried on each tick, delivered with flag off.
    auto failPause = pairRun("2 failed SPI sends at the pause (120 s)",
                             {{Fault::FailAt, kPauseMs}, {Fault::FailAt, kPauseMs + kTickMs}});
    CHECK(failPause.first == 0 && failPause.second.second <= 14);
    // The plane resets while paused (nothing changes): the keepalive restores it.
    int64_t first = -1, last = -1;
    auto reset = pairRun("plane reset while paused (121 s)", {{Fault::ResetAt, 121000}}, &first, &last);
    CHECK(reset.second.first <= 7);                                       // flag off: next tick
    CHECK(reset.second.second > 0 && last - first <= kOverlayKeepaliveDefaultMs + 2 * kTickMs);
    // The same reset, then an OK press (a show forces a send): the next tick, as flag off.
    auto forced = pairRun("plane reset + an OK press (126 s)", {{Fault::ResetOkAt, 126000}});
    CHECK(forced.first == 0 && forced.second.second <= 7);
    // A shorter keepalive bounds the reset gap accordingly.
    {
        ReplayCfg k = onCfg;
        k.keepaliveMs = 1000;
        k.faults = {{Fault::ResetAt, 121000}};
        int64_t f = -1, l = -1;
        const int n = mismatchedFrames(off, runReplay(k), &f, &l);
        CHECK(n > 0 && l - f <= 1000 + 2 * kTickMs);
    }

    // ------------------------------------------------------------ session begin vs the fence
    // A new session's identity fences the plane until a frame edge; its first packet
    // follows the begin by ~13 ms, so by phase it can meet the fence. A receiver that
    // rejects it then needs the next tick: the confirm, as the periodic send did.
    {
        int rejectedRuns = 0, waitedRuns = 0;
        for (int64_t restart = 300; restart < 335; ++restart) {
            for (Receiver rx : {Receiver::Rejects, Receiver::Waits}) {
                ReplayCfg a = offCfg, b = onCfg;
                a.receiver = b.receiver = rx;
                a.restartMs = b.restartMs = restart;
                a.playMs = b.playMs = 206000;
                const ReplayOut ra = runReplay(a), rb = runReplay(b);
                CHECK(mismatchedFrames(ra, rb) == 0 && ra.beginMs.size() == 2);
                CHECK(rx == Receiver::Waits || ra.rejected == rb.rejected);
                // The swept session is the second one: count its first packet only.
                const int64_t s1Us = ra.beginMs.back() * 1000;
                auto after = [&](const std::vector<int64_t>& v) {
                    return std::count_if(v.begin(), v.end(), [&](int64_t us) { return us >= s1Us; });
                };
                if (rx == Receiver::Rejects && after(ra.rejectedUs))
                    ++rejectedRuns;
                if (rx == Receiver::Waits && after(ra.waitedUs))
                    ++waitedRuns;
            }
        }
        std::printf("  session begin vs the fence, 35 begin phases: first packet rejected in %d (rejecting "
                    "receiver), waited in %d (waiting receiver); flag on == flag off in all\n",
                    rejectedRuns, waitedRuns);
        CHECK(rejectedRuns > 0 && rejectedRuns < 35 && waitedRuns == rejectedRuns);
    }

    // ------------------------------------------------------------ no overlay plane: the count
    {
        using N = OverlaySkipNoPlaneSetting;
        ReplayCfg lean = offCfg, leanOn = offCfg, leanAll = onCfg, prodOn = offCfg, planeOn = offCfg;
        lean.buildId = leanOn.buildId = leanAll.buildId = kLeanBuild;
        prodOn.buildId = 0x48005D45u; // prod 480p (rbf 534cb8e7), also PLEX_480P_LEAN
        leanOn.skipNoPlane = leanAll.skipNoPlane = prodOn.skipNoPlane = planeOn.skipNoPlane = N::On;
        leanAll.decoupled = true;
        const ReplayOut rl = runReplay(lean), rlOn = runReplay(leanOn), rlAll = runReplay(leanAll),
                        rProd = runReplay(prodOn), rpOn = runReplay(planeOn);
        std::printf("  MPX_OVERLAY_SKIP_NO_PLANE, 10 min play, overlay SPI downloads (index 6):\n"
                    "    BUILD_ID 0x48005D52 switch off %d (%.1f/min: %zu publisher + %zu hides), switch on %d "
                    "(publishers started %d of %zu sessions); with ONCHANGE+DECOUPLE too %d\n"
                    "    BUILD_ID 0x48005D45 switch on %d; BUILD_ID 0x4801F40C (plane) switch on %d = off %d\n",
                    rl.index6, rl.index6 / (kPlayMs / 60000.0), rl.sendMs.size(), rl.hideMs.size(),
                    rlOn.index6, rlOn.publishers, rlOn.beginMs.size(), rlAll.index6, rProd.index6,
                    rpOn.index6, off.index6);
        CHECK(rl.sendMs == off.sendMs && rl.index6 == off.index6 && rl.index6 > 2900); // off: unchanged
        CHECK(rlOn.index6 == 0 && rlOn.sendMs.empty() && rlOn.hideMs.empty() && rlOn.publishers == 0);
        CHECK(rlOn.beginMs.size() == 3 && rlOn.ticks.empty());
        CHECK(rlAll.index6 == 0 && rProd.index6 == 0);
        CHECK(rpOn.sendMs == off.sendMs && rpOn.index6 == off.index6 && mismatchedFrames(rpOn, off) == 0);
        // The same through the real publisher thread and finishFpgaPlayback.
        const ThreadPlay tOff = playThroughThread(N::Off, kLeanBuild, false, 40);
        const ThreadPlay tOn = playThroughThread(N::On, kLeanBuild, false, 40);
        const ThreadPlay tOnChange = playThroughThread(N::On, kLeanBuild, true, 40);
        const ThreadPlay tPlane = playThroughThread(N::On, kPlaneBuild, false, 40);
        // On change with nothing changing: every tick after first + confirm is skipped, so
        // only the thread's own stop check (not a send's liveness check) ends it.
        const ThreadPlay tSkip = playThroughThread(N::Off, kPlaneBuild, true, 40, true);
        std::printf("    real publisher thread, 40 ticks: 0x48005D52 off %d sends (%d ticks + %d hide), on %d "
                    "(+ONCHANGE %d); 0x4801F40C on %d; ONCHANGE with nothing changing %d (quiesce %.1f ms)\n",
                    tOff.index6, tOff.ticks, tOff.hides, tOn.index6, tOnChange.index6, tPlane.index6,
                    tSkip.index6, tSkip.quiesceUs / 1000.0);
        CHECK(tOff.index6 >= 41 && tOff.hides == 1 && tOff.index6 >= tOff.ticks);
        CHECK(tOn.index6 == 0 && tOn.ticks == 0 && tOn.hides == 0 && tOnChange.index6 == 0);
        CHECK(tPlane.index6 >= 41 && tPlane.hides == 1);
        CHECK(tSkip.index6 == 3 && tSkip.hides == 1 && tSkip.ticks >= 40); // first, confirm, hide
        for (const ThreadPlay* p : {&tOff, &tOn, &tOnChange, &tPlane, &tSkip})
            CHECK(p->quiesceUs < 500000);
    }

    // ------------------------------------------------------------ the publisher loop's own paths
    // runOverlayPublisher through the real OverlayPublisherThread (the flag-off surface
    // too): 5 failed sends in a row end the session, not 4; a delivered send resets the
    // count; on change a failed send is retried on every tick; an exception ends it
    // through failSession; a publish stopped before its send is not an attempt; the
    // done hook (summary line, trace writer) runs on every path.
    {
        using R = OverlayPublishReason;
        const LoopRun five = runLoop("FFFFF", false, 100);
        CHECK(five.publishes == 5 && five.failSessions == 1 &&
              five.failWhy == "native overlay SPI transport failed repeatedly");
        CHECK(five.counted == 5 && five.countedSent == 0 && five.counts.failed == 5 &&
              five.counts.of(R::Periodic) == 5 && five.doneRan);
        const LoopRun resets = runLoop("FFFFSFFFFSFFFF", false, 20);
        CHECK(resets.publishes == 20 && resets.failSessions == 0 && resets.counts.failed == 12 &&
              resets.countedSent == 8 && resets.doneRan);
        const LoopRun thrown = runLoop("FFFFX", false, 100);
        CHECK(thrown.publishes == 5 && thrown.failSessions == 1 &&
              thrown.failWhy == "native overlay publisher failed" && thrown.counted == 4 &&
              thrown.doneRan);
        const LoopRun stopped = runLoop("SST", false, 100);
        CHECK(stopped.publishes == 3 && stopped.counted == 2 && stopped.failSessions == 0 &&
              stopped.counts.sends() == 2 && stopped.counts.failed == 0 && stopped.doneRan);
        const LoopRun retry = runLoop("SSFFS", true, 6);
        CHECK(retry.reasons ==
              std::vector<R>({R::First, R::Changed, R::Changed, R::Retry, R::Retry, R::Changed}));
        CHECK(retry.counts.failed == 2 && retry.countedSent == 4 && retry.failSessions == 0 &&
              retry.doneRan);
        std::printf("  publisher loop (real OverlayPublisherThread): 5 failed sends end the session after %d "
                    "attempts; F4 S F4 S F4: %d session failures; a throw: \"%s\"; stopped before the send: "
                    "%llu of %d calls counted; on change a failed send is retried every tick\n",
                    five.publishes, resets.failSessions, thrown.failWhy.c_str(),
                    static_cast<unsigned long long>(stopped.counts.sends()), stopped.publishes);
    }

    // ------------------------------------------------------------ quiesce, then the hide
    // fpgaThreadMain's order through the real pieces: finishFpgaPlayback calls quiesce
    // (OverlayPublisherThread::quiesce: stop, join; an in-flight send completes) and only
    // then release, which sends the hide under presentMu_ with the next sequence.
    for (bool decoupled : {false, true}) {
        for (bool onChange : {false, true}) {
            std::mutex presentMu, recMu;
            struct Rec {
                int64_t beginUs, endUs;
                uint32_t seq;
                bool hide;
            };
            std::vector<Rec> recs;
            CountingSender spi;
            uint32_t sequence = 0;
            std::atomic<int> sends{0};
            std::atomic<bool> inFlight{false}, doneRan{false};
            // Event tickets (seq_cst), so "before" is exact, not microsecond-coarse.
            std::atomic<int64_t> ticket{0}, quiesceAskedUs{0};
            auto stamp = [&] { return ticket.fetch_add(1) + 1; };
            const int64_t deadlineUs = monoUs() + 3000000;
            OverlayPublisherThread publisher;
            OverlayPublisherHooks h;
            h.live = [&] { return monoUs() < deadlineUs; };
            h.inputs = [&] {
                OverlayInputs in;
                in.presented = true;
                in.visible = true;
                in.positionMs = 1000 * static_cast<int64_t>(sends.load()); // new content each send
                in.durationMs = kDurationMs;
                in.title = "T";
                return in;
            };
            h.contentKey = [&](const fpga_overlay::Model& m) {
                return overlayContentKey(fpga_overlay::encode(kEpoch, kNonce, sequence + 1, m));
            };
            h.forceGen = [] { return uint64_t(0); };
            h.nowUs = [] { return monoUs(); };
            h.publish = [&](OverlayPublishReason, const fpga_overlay::Model& m) {
                return publishOverlay(
                    decoupled, [&] { return std::unique_lock<std::mutex>(presentMu); },
                    [&] { return !publisher.stopping(); },
                    [&] {
                        const int64_t b = stamp();
                        if (sends.load() == 4) {
                            // The fifth send is in flight when quiesce starts.
                            inFlight = true;
                            const int64_t giveUpUs = monoUs() + 2000000;
                            while (!publisher.stopping() && monoUs() < giveUpUs)
                                std::this_thread::yield();
                            std::this_thread::sleep_for(std::chrono::milliseconds(3));
                        }
                        const bool ok = fpga_overlay::send(spi, kEpoch, kNonce, sequence, m);
                        {
                            std::lock_guard<std::mutex> lock(recMu);
                            recs.push_back({b, stamp(), sequence, false});
                        }
                        sends.fetch_add(1);
                        return ok;
                    });
            };
            h.tick = [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
            h.counted = [](bool) {};
            h.skipped = [] {};
            h.failSession = [](const char*) {};
            publisher.start(std::move(h), OverlayPublishGate(onChange, 5000000), nullptr,
                            [&](const OverlayPublishGate&, const OverlayPublishCounts&) { doneRan = true; });
            while (!inFlight.load() && monoUs() < deadlineUs)
                std::this_thread::yield();
            std::vector<std::string> order;
            int64_t hideBeginUs = 0;
            finishFpgaPlayback(
                FpgaTerminalInput{true, false, false, true}, [] { return std::string("snapshot"); },
                [&] {
                    quiesceAskedUs = stamp();
                    publisher.quiesce();
                    order.push_back(doneRan ? "quiesced" : "quiesce-returned-early");
                },
                [&] {
                    std::lock_guard<std::mutex> present(presentMu);
                    order.push_back("release");
                    hideBeginUs = stamp();
                    const bool ok = fpga_overlay::send(spi, kEpoch, kNonce, sequence, hiddenOverlayModel());
                    std::lock_guard<std::mutex> lock(recMu);
                    recs.push_back({hideBeginUs, stamp(), sequence, true});
                    return ok;
                },
                [] { return false; }, [] { return true; }, [](const FpgaTerminalReceipt&) {});
            CHECK(order.size() == 2 && order[0] == "quiesced" && order[1] == "release");
            CHECK(recs.size() == 6 && recs.back().hide && spi.index6 == 6);
            if (recs.size() == 6) {
                CHECK(recs[4].seq == 5 && recs[5].seq == 6); // the hide takes the next sequence
                CHECK(recs[4].beginUs < quiesceAskedUs && recs[4].endUs <= hideBeginUs);
                for (size_t i = 0; i + 1 < recs.size(); ++i)
                    CHECK(recs[i].seq + 1 == recs[i + 1].seq && recs[i].endUs <= recs[i + 1].beginUs);
                for (const Rec& r : recs)
                    CHECK(r.hide || r.beginUs < quiesceAskedUs); // no send starts after quiesce
            }
            // The receiver takes the hide: this session, the next sequence, nothing drawn.
            CHECK(fpga_overlay::validate(spi.last.data(), spi.last.size(), kEpoch, kNonce, 5));
            CHECK(!(fpga_overlay::get16(spi.last.data() + 28) & fpga_overlay::Visible));
        }
    }
    std::printf("  quiesce then hide (real OverlayPublisherThread + finishFpgaPlayback, 4 flag combinations): "
                "an in-flight send completes, no send after quiesce, the hide is last with the next sequence\n");

    // ------------------------------------------------------------ presentMu_ holders
    // Pause/resume (media_player.cpp pause/resume: pauseBitstreamSession then the audio
    // control), setSourceAspect / video-aspect and session-begin hold presentMu_ across
    // several SPI windows: a control record, then waitBitstreamControl's ACK poll (a
    // status read, 0.5 ms sleep, until the ACK or 500 ms). Coupled, an overlay send waits
    // for presentMu_; decoupled it may land between two of the holder's windows, never
    // inside one (FpgaSpi's lock), and it only delays the next poll: the poll still sees
    // the ACK (the deadline is checked after each read, never instead of one).
    for (bool decoupled : {false, true}) {
        std::mutex presentMu, recMu;
        using Iv = std::pair<int64_t, int64_t>;
        std::vector<Iv> overlayWins; // the overlay thread's SPI windows (under recMu)
        auto spiWindow = [&](std::vector<Iv>* into, int64_t bodyUs) {
            int64_t end = 0;
            FpgaSpi::withSpiLockForTest([&] {
                const int64_t b = monoUs();
                while (monoUs() - b < bodyUs) {
                }
                end = monoUs();
                std::lock_guard<std::mutex> lock(recMu);
                into->push_back({b, end});
            });
            return end;
        };
        std::atomic<bool> stop{false};
        std::thread overlayThread([&] {
            while (!stop.load()) {
                publishOverlay(
                    decoupled, [&] { return std::unique_lock<std::mutex>(presentMu); },
                    [&] { return !stop.load(); },
                    [&] {
                        spiWindow(&overlayWins, 1500);
                        return true;
                    });
                std::this_thread::sleep_for(std::chrono::microseconds(700));
            }
        });
        struct Span {
            int64_t lockedUs = 0, unlockedUs = 0;
            bool acked = false;
            std::vector<Iv> wins;
        };
        std::vector<Span> spans;
        int between = 0;
        for (int i = 0; i < 400 && (i < 60 || (decoupled && between < 3)); ++i) {
            Span sp;
            {
                std::unique_lock<std::mutex> present(presentMu);
                sp.lockedUs = monoUs();
                spiWindow(&sp.wins, 300); // the control record
                const int64_t ackAt = monoUs() + 2000, deadline = monoUs() + 500000;
                do {
                    if (spiWindow(&sp.wins, 200) >= ackAt) { // readBitstreamStatus
                        sp.acked = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                } while (monoUs() < deadline);
                sp.unlockedUs = monoUs();
            }
            {
                // Overlay windows after this holder's first SPI window and before its last.
                std::lock_guard<std::mutex> lock(recMu);
                for (const Iv& o : overlayWins)
                    between += o.first >= sp.wins.front().second && o.second <= sp.wins.back().first;
            }
            spans.push_back(std::move(sp));
            std::this_thread::sleep_for(std::chrono::microseconds(800));
        }
        stop = true;
        overlayThread.join();
        std::vector<Iv> all = overlayWins;
        for (const Span& s : spans)
            all.insert(all.end(), s.wins.begin(), s.wins.end());
        std::sort(all.begin(), all.end());
        int overlaps = 0, inside = 0, acked = 0;
        int64_t longestUs = 0;
        for (size_t i = 1; i < all.size(); ++i)
            overlaps += all[i].first < all[i - 1].second;
        for (const Iv& o : overlayWins)
            for (const Span& s : spans)
                inside += o.first >= s.lockedUs && o.second <= s.unlockedUs;
        for (const Span& s : spans) {
            acked += s.acked;
            longestUs = std::max(longestUs, s.unlockedUs - s.lockedUs);
        }
        std::printf("  presentMu_ holder (control record + ACK poll) x%zu, overlay %s: %zu overlay windows, %d inside "
                    "a holder's presentMu_ span, %d between two of its SPI windows, overlapping windows %d, "
                    "ACK seen %d/%zu, longest span %.1f ms\n",
                    spans.size(), decoupled ? "decoupled" : "coupled", overlayWins.size(), inside, between,
                    overlaps, acked, spans.size(), longestUs / 1000.0);
        CHECK(overlaps == 0 && !overlayWins.empty());
        CHECK(acked == static_cast<int>(spans.size()));
        if (decoupled)
            CHECK(between > 0);
        else
            CHECK(inside == 0 && between == 0);
    }

    // ------------------------------------------------------------ hot-path error text
    // MPX_OVERLAY_DECOUPLE also makes the DDR hot paths' error text non-blocking: the
    // presentation read (video-ack, under presentMu_) no longer waits for an SPI window.
    {
        struct SpiHolder {
            std::atomic<bool> held{false};
            std::thread t;
            explicit SpiHolder(int ms)
                : t([this, ms] {
                      FpgaSpi::withSpiLockForTest([&] {
                          held = true;
                          std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                      });
                  }) {
                while (!held.load())
                    std::this_thread::yield();
            }
            ~SpiHolder() { t.join(); }
        };
        FpgaSpi spi; // never opened
        FpgaSpi::VideoPresentation picture;
        const std::string stale = "readVideoPresentation: stale or inactive session";
        FpgaSpi::setHotPathErrorsNonBlocking(false);
        CHECK(!spi.readVideoPresentation(7, picture) && spi.lastError() == stale);
        spi.clearErrorForTest();
        int64_t waitedOff = 0, waitedOn = 0;
        {
            SpiHolder window(40);
            const int64_t t0 = monoUs();
            (void)spi.readVideoPresentation(7, picture);
            waitedOff = monoUs() - t0;
        }
        CHECK(waitedOff >= 30000 && spi.lastError() == stale);
        FpgaSpi::setHotPathErrorsNonBlocking(true);
        spi.clearErrorForTest();
        const uint64_t skips0 = FpgaSpi::hotPathErrorsSkipped();
        {
            SpiHolder window(40);
            const int64_t t0 = monoUs();
            (void)spi.readVideoPresentation(7, picture);
            waitedOn = monoUs() - t0;
        }
        CHECK(waitedOn < 10000 && FpgaSpi::hotPathErrorsSkipped() == skips0 + 1 && spi.lastError().empty());
        CHECK(!spi.readVideoPresentation(7, picture) && spi.lastError() == stale); // lock free: as before
        // A clear (a successful read clears the text) during a window: skipped, text kept.
        int64_t clearUs = 0;
        {
            SpiHolder window(30);
            const int64_t t0 = monoUs();
            spi.clearErrorForTest();
            clearUs = monoUs() - t0;
        }
        CHECK(clearUs < 10000 && spi.lastError() == stale && FpgaSpi::hotPathErrorsSkipped() == skips0 + 2);
        spi.clearErrorForTest(); // lock free: cleared
        CHECK(spi.lastError().empty());
        FpgaSpi::setHotPathErrorsNonBlocking(false);
        spi.clearErrorForTest();
        std::printf("  presentation read during a 40 ms SPI window: %.1f ms (default), %.1f ms (decoupled)\n",
                    waitedOff / 1000.0, waitedOn / 1000.0);
    }

    if (fails) {
        std::fprintf(stderr, "test_overlay_onchange: %d FAIL\n", fails);
        return 1;
    }
    std::printf("test_overlay_onchange: OK\n");
    return 0;
}
