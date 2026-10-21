#pragma once
// Native-overlay publication and presentMu_ (rel-480i-costall-1004, GLASS-A6).
//
// The overlay publisher (media_player.cpp startOverlay, the third "mpx-play" thread)
// sends a 576-byte packet every 200 ms through FpgaSpi::sendFileTx. Like every SPI
// transaction it first parks Main_MiSTer (spi_window.hpp): usually 5-11 ms, but
// 40-125 ms when Main does not stop at once, burning ~40% CPU in the /proc polls.
// The send held presentMu_ all along, and the video ACK read (video-ack) and the
// audio clock read (audio-clock) take presentMu_ every ~2 ms for DDR-mailbox loads
// that never touch SPI: both stalled for the whole window (A6: ~0.18/min ~105 ms
// co-stalls, every one with an overlay hold of the same length).
//
// MPX_OVERLAY_DECOUPLE=1:
//   - the publisher sends without presentMu_. SPI stays serialised (SpiExclusive:
//     spiMutex + flock + Main window). The session cannot end under a send: quiesce
//     (finishFpgaPlayback) stops and joins the publisher before release takes
//     presentMu_ to hide the plane and reset. Liveness is checked just before it;
//   - the DDR hot paths update FpgaSpi's error text only when spiMutex is free
//     (FpgaSpi::setHotPathErrorsNonBlocking), else they would wait for the window
//     while holding presentMu_ (video-ack, video-push) and stall audio anyway.
// Unset or 0: unchanged.
//
// MPX_OVERLAY_ONCHANGE=1 (rel overlay-onchange 1004): every send still parks Main
// (SIGSTOP), 5 times a second, though the plane is usually hidden and unchanged. The
// publisher keeps its 200 ms tick but sends only when the drawn content changes
// (overlayContentKey), plus:
//   - one confirming repeat on the next tick after new content (also after a forced
//     send, and after the retry that delivers either): the in-session receiver only
//     delays a colliding packet by a field (ioctl_wait), but it silently discards one
//     it cannot take (no valid session), and the periodic send used to repair any
//     such loss within a tick;
//   - a keepalive every MPX_OVERLAY_KEEPALIVE_MS (default 5000) so a lost or reset
//     plane comes back;
//   - a forced send after show or skip feedback (MediaPlayer::showPlaybackOverlay,
//     flashPlaybackSkip). MediaPlayer::onCoreReturned forces one too, for the core
//     gate's coreReturned hook; nothing calls it on a lineage without the core gate,
//     where a returned core always means a new session, whose first tick sends;
//   - a retry on the next tick after any failed (SPI) send.
// It composes with MPX_OVERLAY_DECOUPLE. Unset or 0: a send every tick, unchanged.
//
// MPX_OVERLAY_SKIP_NO_PLANE=1 (rel 10:22, review-d52-shift-rca2-1004 suspect 3):
// on a core built without the playback overlay plane (overlayPlaneAbsent: the 480p
// PLEX_480P_LEAN builds) the session starts no publisher and release sends no hide, so
// a play makes no index-6 download and no overlay Main SIGSTOP at all. Cores with the
// plane are unaffected. Unset or 0: every core gets the publisher, unchanged.

#include "fpga_playback_overlay.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace misterplex {

constexpr const char* kOverlayDecoupleEnv = "MPX_OVERLAY_DECOUPLE";

enum class OverlayDecoupleSetting { Off, On, Invalid };

inline OverlayDecoupleSetting parseOverlayDecouple(const char* raw) {
    if (!raw || !*raw || std::strcmp(raw, "0") == 0)
        return OverlayDecoupleSetting::Off;
    if (std::strcmp(raw, "1") == 0)
        return OverlayDecoupleSetting::On;
    return OverlayDecoupleSetting::Invalid;
}

inline OverlayDecoupleSetting overlayDecoupleFromEnvironment() {
    return parseOverlayDecouple(std::getenv(kOverlayDecoupleEnv));
}

// "" when unset or 0 (nothing to say).
inline std::string formatOverlayDecoupleNotice(OverlayDecoupleSetting s, const char* raw) {
    if (s == OverlayDecoupleSetting::On)
        return "media: LAB MPX_OVERLAY_DECOUPLE=1 -> overlay SPI send outside presentMu_, "
               "hot-path SPI error text non-blocking";
    if (s == OverlayDecoupleSetting::Invalid) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_DECOUPLE='%.32s' invalid (want 0 or 1) -> off",
                      raw ? raw : "");
        return buf;
    }
    return std::string();
}

enum class OverlayPublish { Sent, Failed, Stopped };

// One publication. lockPresent() returns the presentMu_ guard; live() is "session
// current and publisher not stopped"; send() is the SPI transfer. Default: all
// three under presentMu_, exactly as before. Decoupled: live() then send(), no
// presentMu_.
template <class LockPresent, class Live, class Send>
OverlayPublish publishOverlay(bool decoupled, LockPresent&& lockPresent, Live&& live,
                              Send&& send) {
    if (decoupled) {
        if (!live())
            return OverlayPublish::Stopped;
        return send() ? OverlayPublish::Sent : OverlayPublish::Failed;
    }
    auto present = lockPresent();
    if (!live())
        return OverlayPublish::Stopped;
    return send() ? OverlayPublish::Sent : OverlayPublish::Failed;
}

// ---------------------------------------------------------------- publish on change

constexpr const char* kOverlayOnChangeEnv = "MPX_OVERLAY_ONCHANGE";
constexpr const char* kOverlayKeepaliveEnv = "MPX_OVERLAY_KEEPALIVE_MS";
constexpr int64_t kOverlayKeepaliveDefaultMs = 5000;
constexpr int64_t kOverlayKeepaliveMinMs = 200;
constexpr int64_t kOverlayKeepaliveMaxMs = 600000;

struct OverlayOnChangeSettings {
    bool on = false;
    bool onInvalid = false;       // MPX_OVERLAY_ONCHANGE neither unset, 0 nor 1: off
    int64_t keepaliveMs = kOverlayKeepaliveDefaultMs;
    bool keepaliveInvalid = false; // not digits in range: the default
};

inline OverlayOnChangeSettings parseOverlayOnChange(const char* onRaw, const char* keepaliveRaw) {
    OverlayOnChangeSettings s;
    if (onRaw && *onRaw && std::strcmp(onRaw, "0") != 0) {
        if (std::strcmp(onRaw, "1") == 0)
            s.on = true;
        else
            s.onInvalid = true;
    }
    if (keepaliveRaw && *keepaliveRaw) {
        int64_t v = 0;
        bool ok = std::strlen(keepaliveRaw) <= 7;
        for (const char* c = keepaliveRaw; ok && *c; ++c) {
            if (*c < '0' || *c > '9')
                ok = false;
            else
                v = v * 10 + (*c - '0');
        }
        if (ok && v >= kOverlayKeepaliveMinMs && v <= kOverlayKeepaliveMaxMs)
            s.keepaliveMs = v;
        else
            s.keepaliveInvalid = true;
    }
    return s;
}

inline OverlayOnChangeSettings overlayOnChangeFromEnvironment() {
    return parseOverlayOnChange(std::getenv(kOverlayOnChangeEnv), std::getenv(kOverlayKeepaliveEnv));
}

// "" when MPX_OVERLAY_ONCHANGE is unset or 0 and the keepalive is not set wrong.
inline std::string formatOverlayOnChangeNotice(const OverlayOnChangeSettings& s,
                                               const char* onRaw, const char* keepaliveRaw) {
    std::string out;
    char buf[240];
    if (s.on) {
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_ONCHANGE=1 -> overlay sent on change (+1 confirm), "
                      "keepalive %lld ms, forced on show/skip feedback, retried after a "
                      "failed send",
                      static_cast<long long>(s.keepaliveMs));
        out = buf;
    } else if (s.onInvalid) {
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_ONCHANGE='%.32s' invalid (want 0 or 1) -> off",
                      onRaw ? onRaw : "");
        out = buf;
    }
    if (s.keepaliveInvalid) {
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_KEEPALIVE_MS='%.32s' invalid (want %lld..%lld) -> %lld",
                      keepaliveRaw ? keepaliveRaw : "",
                      static_cast<long long>(kOverlayKeepaliveMinMs),
                      static_cast<long long>(kOverlayKeepaliveMaxMs),
                      static_cast<long long>(s.keepaliveMs));
        if (!out.empty())
            out += '\n';
        out += buf;
    }
    return out;
}

// What the publisher reads each tick (media_player.cpp overlay thread), as one snapshot.
struct OverlayInputs {
    bool presented = false; // the first picture is on screen
    bool paused = false;
    bool visible = false;   // PlaybackOverlay::visible(): shown or skip-flashed recently
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    std::string title;      // "" -> "MiSTerPlex"
    std::string transport;  // skip feedback while it lasts, else ""
};

inline fpga_overlay::Model buildOverlayModel(const OverlayInputs& in) {
    fpga_overlay::Model model;
    model.state = in.paused ? fpga_overlay::State::Paused
                            : in.presented ? fpga_overlay::State::Playing
                                           : fpga_overlay::State::Buffering;
    model.position_ms = in.presented ? in.positionMs : 0;
    model.duration_ms = in.durationMs;
    model.title = in.title.empty() ? "MiSTerPlex" : in.title;
    model.transport = in.transport;
    if (in.presented && !in.paused && !in.visible) {
        model.state = fpga_overlay::State::Hidden;
        model.flags = 0;
    }
    return model;
}

// What the receiver draws from a packet, as a 64-bit FNV-1a key. The sequence fields
// (bytes 24..27 and 60..63) never count. A packet without the Visible flag draws
// nothing, so its text, progress and placement do not count either: they still carry
// the playback clock, which would otherwise "change" every second while hidden.
inline uint64_t overlayContentKey(const fpga_overlay::Packet& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    mix(0, 24); // magic, version, length, epoch, nonce
    if (!(fpga_overlay::get16(p.data() + 28) & fpga_overlay::Visible)) {
        mix(28, 31); // flags and state (scale, byte 31, is not drawn either)
        return h ^ 0x48494444454eull; // "HIDDEN": never equal to a drawn packet's key
    }
    mix(28, 60);
    mix(64, p.size());
    return h;
}

enum class OverlayPublishReason : uint8_t {
    None, Periodic, First, Changed, Confirm, Keepalive, Forced, Retry, Count
};

inline const char* overlayPublishReasonName(OverlayPublishReason r) {
    static const char* const names[] = {"none", "periodic", "first", "changed", "confirm",
                                        "keepalive", "forced", "retry"};
    const size_t i = static_cast<size_t>(r);
    return i < sizeof names / sizeof names[0] ? names[i] : "?";
}

// One decision per publisher tick. With on-change off every tick is Periodic (the
// unchanged behaviour). forceGen: MediaPlayer's force counter; a new value forces a send.
class OverlayPublishGate {
public:
    OverlayPublishGate(bool onChange, int64_t keepaliveUs)
        : onChange_(onChange), keepaliveUs_(keepaliveUs) {}

    OverlayPublishReason decide(uint64_t key, uint64_t forceGen, int64_t nowUs) const {
        if (!onChange_)
            return OverlayPublishReason::Periodic;
        if (!haveSent_)
            return OverlayPublishReason::First;
        if (failed_)
            return OverlayPublishReason::Retry;
        if (forceGen != forceSeen_)
            return OverlayPublishReason::Forced;
        if (key != lastKey_)
            return OverlayPublishReason::Changed;
        if (confirmPending_)
            return OverlayPublishReason::Confirm;
        if (nowUs - lastSendUs_ >= keepaliveUs_)
            return OverlayPublishReason::Keepalive;
        return OverlayPublishReason::None;
    }

    // The outcome of a send decide() asked for. A failed send is retried on the next
    // tick whatever changes; new content (or a force, also one that arrived while a
    // send was failing) earns one confirming repeat once it is delivered, also when
    // that takes a retry.
    void sent(OverlayPublishReason reason, uint64_t key, uint64_t forceGen, bool ok,
              int64_t nowUs) {
        const bool fresh = !haveSent_ || key != lastKey_ || reason == OverlayPublishReason::Forced ||
                           reason == OverlayPublishReason::First || forceGen != forceSeen_ ||
                           retryFresh_;
        if (!ok) {
            failed_ = true;
            retryFresh_ = fresh;
            return;
        }
        failed_ = false;
        retryFresh_ = false;
        haveSent_ = true;
        lastKey_ = key;
        lastSendUs_ = nowUs;
        forceSeen_ = forceGen;
        confirmPending_ = onChange_ && fresh && reason != OverlayPublishReason::Confirm;
    }

    bool onChange() const { return onChange_; }
    int64_t keepaliveUs() const { return keepaliveUs_; }

private:
    bool onChange_;
    int64_t keepaliveUs_;
    bool haveSent_ = false;
    bool failed_ = false;
    bool retryFresh_ = false; // the failed send carried new content or a force
    bool confirmPending_ = false;
    uint64_t lastKey_ = 0;
    uint64_t forceSeen_ = 0;
    int64_t lastSendUs_ = 0;
};

// Per-reason tallies for the session-end summary line.
struct OverlayPublishCounts {
    uint64_t byReason[static_cast<size_t>(OverlayPublishReason::Count)] = {};
    uint64_t failed = 0;
    void note(OverlayPublishReason r) { ++byReason[static_cast<size_t>(r)]; }
    uint64_t of(OverlayPublishReason r) const { return byReason[static_cast<size_t>(r)]; }
    uint64_t sends() const {
        uint64_t n = 0;
        for (size_t i = 1; i < static_cast<size_t>(OverlayPublishReason::Count); ++i)
            n += byReason[i];
        return n;
    }
};

inline std::string formatOverlayPublishSummary(const OverlayPublishGate& gate,
                                               const OverlayPublishCounts& c) {
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "media: overlay publishes onchange=%d keepalive_ms=%lld sends=%llu failed=%llu "
                  "skipped=%llu periodic=%llu first=%llu changed=%llu confirm=%llu "
                  "keepalive=%llu forced=%llu retry=%llu",
                  gate.onChange() ? 1 : 0, static_cast<long long>(gate.keepaliveUs() / 1000),
                  static_cast<unsigned long long>(c.sends()),
                  static_cast<unsigned long long>(c.failed),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::None)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::Periodic)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::First)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::Changed)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::Confirm)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::Keepalive)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::Forced)),
                  static_cast<unsigned long long>(c.of(OverlayPublishReason::Retry)));
    return buf;
}

// ---------------------------------------------------------------- the publisher loop

// The overlay publisher thread's loop (media_player.cpp startOverlay), here so the tests
// drive the code the daemon runs. Each tick: one input snapshot, the model, the gate's
// decision, then (unless the tick is skipped) one publication through `publish`
// (presentMu_ + liveness check + SPI send, or the decoupled order). It ends when live()
// is false, publish() reports Stopped, or 5 sends in a row failed (failSession).
struct OverlayPublisherHooks {
    std::function<bool()> live;
    std::function<OverlayInputs()> inputs;
    std::function<uint64_t(const fpga_overlay::Model&)> contentKey; // on-change only
    std::function<uint64_t()> forceGen;
    std::function<int64_t()> nowUs;
    std::function<OverlayPublish(OverlayPublishReason, const fpga_overlay::Model&)> publish;
    std::function<void()> tick;                 // the 200 ms sleep
    std::function<void(bool sent)> counted;     // after each send attempt
    std::function<void()> skipped;              // a tick that sent nothing
    std::function<void(const char*)> failSession;
};

inline void runOverlayPublisher(const OverlayPublisherHooks& h, OverlayPublishGate& gate,
                                OverlayPublishCounts& counts) {
    unsigned failedTransfers = 0;
    while (h.live()) {
        // AutoFit measures native DE in FPGA; never substitute decoded/HDMI geometry.
        const fpga_overlay::Model model = buildOverlayModel(h.inputs());
        const uint64_t key = gate.onChange() ? h.contentKey(model) : 0;
        const uint64_t forceGen = h.forceGen();
        const OverlayPublishReason why = gate.decide(key, forceGen, h.nowUs());
        if (why == OverlayPublishReason::None) {
            counts.note(why);
            h.skipped();
            h.tick();
            continue;
        }
        const OverlayPublish published = h.publish(why, model);
        if (published == OverlayPublish::Stopped)
            break; // stopped before the send: not an attempt
        counts.note(why);
        const bool sent = published == OverlayPublish::Sent;
        h.counted(sent);
        gate.sent(why, key, forceGen, sent, h.nowUs());
        if (sent) {
            failedTransfers = 0;
        } else {
            ++counts.failed;
            if (++failedTransfers >= 5) {
                h.failSession("native overlay SPI transport failed repeatedly");
                break;
            }
        }
        h.tick();
    }
}

// The publisher thread. start() runs runOverlayPublisher until the session ends or
// quiesce(); an exception ends it through failSession. quiesce() stops and joins it:
// finishFpgaPlayback's quiesce step calls it before release sends the hide packet under
// presentMu_, so no publisher send can follow the hide, and the shared sequence counter
// is the release step's alone from then on.
class OverlayPublisherThread {
public:
    using Done = std::function<void(const OverlayPublishGate&, const OverlayPublishCounts&)>;
    OverlayPublisherThread() = default;
    OverlayPublisherThread(const OverlayPublisherThread&) = delete;
    OverlayPublisherThread& operator=(const OverlayPublisherThread&) = delete;
    ~OverlayPublisherThread() { quiesce(); }

    // started: runs first on the new thread; done: runs last, with the totals.
    void start(OverlayPublisherHooks hooks, OverlayPublishGate gate, std::function<void()> started,
               Done done) {
        quiesce();
        stop_.store(false);
        thread_ = std::thread([this, hooks = std::move(hooks), gate, started = std::move(started),
                               done = std::move(done)]() mutable {
            if (started)
                started();
            OverlayPublishCounts counts;
            OverlayPublisherHooks h = hooks;
            h.live = [this, live = hooks.live] { return !stop_.load() && live(); };
            try {
                runOverlayPublisher(h, gate, counts);
            } catch (...) {
                if (h.failSession)
                    h.failSession("native overlay publisher failed");
            }
            if (done) {
                try {
                    done(gate, counts);
                } catch (...) {
                }
            }
        });
    }
    // True once quiesce() began: the publish step's liveness check reads it.
    bool stopping() const { return stop_.load(); }
    void quiesce() {
        stop_.store(true);
        if (thread_.joinable())
            thread_.join();
    }

private:
    std::atomic<bool> stop_{true};
    std::thread thread_;
};

// The packet release sends after quiesce: the plane hidden.
inline fpga_overlay::Model hiddenOverlayModel() {
    fpga_overlay::Model hidden;
    hidden.state = fpga_overlay::State::Hidden;
    hidden.flags = 0;
    return hidden;
}

// ---------------------------------------------------------------- no overlay plane

constexpr const char* kOverlaySkipNoPlaneEnv = "MPX_OVERLAY_SKIP_NO_PLANE";

enum class OverlaySkipNoPlaneSetting { Off, On, Invalid };

inline OverlaySkipNoPlaneSetting parseOverlaySkipNoPlane(const char* raw) {
    if (!raw || !*raw || std::strcmp(raw, "0") == 0)
        return OverlaySkipNoPlaneSetting::Off;
    if (std::strcmp(raw, "1") == 0)
        return OverlaySkipNoPlaneSetting::On;
    return OverlaySkipNoPlaneSetting::Invalid;
}

inline OverlaySkipNoPlaneSetting overlaySkipNoPlaneFromEnvironment() {
    return parseOverlaySkipNoPlane(std::getenv(kOverlaySkipNoPlaneEnv));
}

// The BUILD_IDs whose core has no playback overlay plane. Each was fitted with
// PLEX_480P_LEAN=1 (Plex_480p.qsf), and its Plex.sv then defines
// PLEX_NO_PLAYBACK_OVERLAY (`ifdef PLEX_480P_LEAN, Plex.sv:1828 in the 5D40/5D45/5D50/5D52
// trees, :1822 in 4800F36A's): no playback_overlay_plane, overlay_ioctl_wait tied 0, so
// an index-6 download has no consumer. CAPS carries no overlay bit, so the ID decides.
// R1 4800A1FB (8ffb2335 Plex_480p_r1.qsf:217) and 4800A152 (45ff6daf Plex_480p_r1.qsf:223)
// are LEAN too: Plex.sv:1915-1916 in both trees; so is 48009E36 (ea4ca1e0 Plex_480p_r1.qsf:229), and so
// is 48002731 (9801dad2 Plex_480p_r1.qsf:237), and so is 48003ECB (f32947c5 Plex_480p_r1.qsf:242).
// 4D57 (7d3e4b5b), 4D40 (47cb9fb4), F40A (8c4c83f8), 6A7E (cb60f749): Plex_480p.qsf ID
// :173/:174, PLEX_480P_LEAN=1 :194/:195; Plex.sv :1828-1829 LEAN -> NO_PLAYBACK_OVERLAY,
// :1834 guards the plane. 240p 2400F40B, 240p 24003997 (F1 ship, 240p v3 + F1: Plex_240p.qsf, no
// LEAN), 240p 2400F36A (96a5c60e Plex_240p.qsf:105) and
// 480i 4801F40A/4801F40C (FPGA_VIDEO_320 without LEAN) keep the plane; any ID not listed
// is assumed to have it.
inline bool overlayPlaneAbsent(uint32_t buildId) {
    switch (buildId) {
    case 0x4800F36Au:
    case 0x48004D57u:
    case 0x48004D40u:
    case 0x48005D40u:
    case 0x48005D45u:
    case 0x48005D50u:
    case 0x48005D52u:
    case 0x4800A1FBu:
    case 0x4800A152u:
    case 0x48009E36u:
    case 0x48002731u:
    case 0x4800C88Fu:
    case 0x48003ECBu:
    case 0x480058BCu:
    case 0x480094DDu:
    case 0x48018AE0u:
    case 0x48032F84u:
    case 0x48038D9Au:
    case 0x48FF65F2u:
    case 0x487A1BD1u: // 48FF65F2 + analog VGA restored
    case 0x48C5385Cu: // 487A1BD1 + sps_parser NVENC relax (analog v10)
    case 0x48C51FF1u: // 48C5385C + pairing overlay both BUILD_IDs (analog v11)
    case 0x48CFEE31u: // 48C51FF1 + HDMI FB_FORCE_BLANK gate (analog v12)
    case 0x48110015u: // 487A1BD1 + sps_parser NVENC relax (same LEAN qsf)
    case 0x4800F40Au:
    case 0x48006A7Eu:
        return true;
    default:
        return false;
    }
}

// Whether a session on this core starts the overlay publisher (and so later sends the
// release hide): always, unless MPX_OVERLAY_SKIP_NO_PLANE=1 and the core has no plane.
inline bool overlayPublisherWanted(OverlaySkipNoPlaneSetting s, uint32_t buildId) {
    return !(s == OverlaySkipNoPlaneSetting::On && overlayPlaneAbsent(buildId));
}

// One line per session: "" when unset or 0.
inline std::string formatOverlaySkipNoPlaneNotice(OverlaySkipNoPlaneSetting s, const char* raw,
                                                  uint32_t buildId) {
    char buf[220];
    if (s == OverlaySkipNoPlaneSetting::Invalid) {
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_SKIP_NO_PLANE='%.32s' invalid (want 0 or 1) -> off",
                      raw ? raw : "");
        return buf;
    }
    if (s != OverlaySkipNoPlaneSetting::On)
        return std::string();
    if (overlayPlaneAbsent(buildId))
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_SKIP_NO_PLANE=1 -> BUILD_ID 0x%08X has no overlay "
                      "plane (PLEX_480P_LEAN): no overlay publisher, no index-6 sends this session",
                      static_cast<unsigned>(buildId));
    else
        std::snprintf(buf, sizeof buf,
                      "media: LAB MPX_OVERLAY_SKIP_NO_PLANE=1 -> BUILD_ID 0x%08X has the overlay "
                      "plane: publisher unchanged",
                      static_cast<unsigned>(buildId));
    return buf;
}

// Whether a session runs the shared lab publisher instead of the prod publisher
// (media_player.cpp overlayLabFlags). Fails closed: only a lab flag set to exactly "1"
// selects it. Unset, "", "0" and every invalid value ("yes", "2", " 1", ...) keep the
// prod publisher; an invalid MPX_OVERLAY_DECOUPLE or MPX_OVERLAY_ONCHANGE still logs its
// "invalid (want 0 or 1) -> off" notice. MPX_OVERLAY_KEEPALIVE_MS selects nothing.
inline bool overlayLabPublisherSelected(OverlayDecoupleSetting decouple,
                                        const OverlayOnChangeSettings& onChange,
                                        OverlaySkipNoPlaneSetting skipNoPlane) {
    return decouple == OverlayDecoupleSetting::On || onChange.on ||
           skipNoPlane == OverlaySkipNoPlaneSetting::On;
}

// The session's overlay SPI downloads (index 6): publisher sends and the release hide,
// for the session-end line. Logged only with a lab overlay flag set.
inline std::string formatOverlaySessionSends(bool publisherStarted, uint64_t publisherSends,
                                             uint64_t hideSends, uint32_t buildId) {
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "media: overlay session index6_sends=%llu publisher=%s publisher_sends=%llu "
                  "hide_sends=%llu build=0x%08X",
                  static_cast<unsigned long long>(publisherSends + hideSends),
                  publisherStarted ? "on" : "off",
                  static_cast<unsigned long long>(publisherSends),
                  static_cast<unsigned long long>(hideSends), static_cast<unsigned>(buildId));
    return buf;
}

} // namespace misterplex
