// Flags-off SPI-sequence replay of the native-overlay publisher (release 12:24 10/4, item 2).
//
// gen_harness.py builds one program per git ref from this template. The four placeholders
// (BLOCK, CALLSITE, QUIESCE, RELEASE, each between at-signs) become that ref's own
// arm/misterplexd/media_player.cpp text:
// fpgaThreadMain's overlay publisher block, its first-AU call site, and the overlay steps
// of finishFpgaPlayback's quiesce and release lambdas. Two substitutions are applied, the
// same for every ref, so the publisher runs in lock-step on a virtual clock:
//   std::this_thread::sleep_for(std::chrono::milliseconds(200)) -> harness::sleepMs(200)
//   std::chrono::steady_clock::now()                           -> harness::steadyNow()
// The rest of fpgaThreadMain's text is the same in the base and the candidate
// (run_overlay_spi_replay.sh checks that), so the overlay SPI traffic is what can differ.
//
// The driver plays the same session script on every ref and records, in order: each SPI
// transaction (one FpgaSpi::sendFileTx = one SpiExclusive window = one Main SIGSTOP
// window) with index, length, decoded header, an FNV-1a hash of the 576-byte payload, the
// calling thread and the presentMu_ owner at that moment; the driver's own control
// windows under presentMu_ (pause/resume); every log line and failFpgaSession call; and
// the session lifecycle (begin, end, quiesce/join, release, exit). stdout is the record
// to diff; stderr says which ref and which publisher ran.
#include "libmisterplex/fpga_playback_overlay.hpp"
#include "libmisterplex/playback_overlay.hpp"
#if HARNESS_HAS_PUBLISH_HPP
#include "libmisterplex/overlay_publish.hpp"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <pthread.h>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define MPX_AV_TRACE(...)

using namespace misterplex;

namespace harness {

std::mutex mu;
std::condition_variable cv;
std::atomic<int64_t> vnowMs{0};
constexpr int64_t kSteadyBaseMs = 1000000000;
constexpr int64_t kOverlayBaseMs = 5000000;
bool sleeping = false, released = false, exitedFlag = false;
int64_t wakeMs = 0;
std::function<bool()> wakePred;
thread_local bool isDriver = false;
int driverTid = 0, publisherTid = 0;
std::mutex* presentMu = nullptr;

int tid() { return static_cast<int>(::syscall(SYS_gettid)); }

std::mutex recMu;
std::vector<std::string> lines;
void rec(const std::string& s) {
    std::lock_guard<std::mutex> l(recMu);
    lines.push_back("t=" + std::to_string(vnowMs.load()) + " " + s);
}

struct PublisherReg {
    PublisherReg() {
        std::lock_guard<std::mutex> l(mu);
        publisherTid = tid();
    }
    ~PublisherReg() {
        std::lock_guard<std::mutex> l(mu);
        exitedFlag = true;
        cv.notify_all();
    }
};
void reg() {
    if (!isDriver) {
        thread_local PublisherReg r;
        (void)r;
    }
}

std::chrono::steady_clock::time_point steadyNow() {
    return std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(kSteadyBaseMs + vnowMs.load()));
}

// The publisher's 200 ms sleep: back to the driver until it releases this tick, or until
// the quiesce stop predicate holds (the real sleep would end and the loop would exit).
void sleepMs(int64_t ms) {
    reg();
    std::unique_lock<std::mutex> l(mu);
    wakeMs = vnowMs.load() + ms;
    sleeping = true;
    released = false;
    cv.notify_all();
    while (!released && !(wakePred && wakePred()))
        cv.wait_for(l, std::chrono::microseconds(200));
    sleeping = false;
    cv.notify_all();
}

// Driver side: wait (real time) until the publisher is back asleep or gone.
void waitSettled(std::unique_lock<std::mutex>& l) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!((sleeping && !released) || exitedFlag)) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "harness: publisher did not settle (t=%lld)\n",
                         static_cast<long long>(vnowMs.load()));
            std::_Exit(3);
        }
        cv.wait_for(l, std::chrono::microseconds(200));
    }
}

// Run the publisher ticks due before `target`, then set the clock to `target` (a tick due
// at exactly `target` runs after the driver's events at `target`).
void advanceTo(int64_t target, const std::function<void(int64_t)>& state) {
    for (;;) {
        std::unique_lock<std::mutex> l(mu);
        if (sleeping && !released && !exitedFlag && wakeMs < target) {
            const int64_t t = wakeMs;
            l.unlock();
            vnowMs = t;
            state(t);
            l.lock();
            released = true;
            cv.notify_all();
            waitSettled(l);
            continue;
        }
        l.unlock();
        if (target > vnowMs.load())
            vnowMs = target;
        state(vnowMs.load());
        return;
    }
}

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int windows = 0;
struct Inject {
    int64_t fromMs, toMs;
    char kind; // 'F' the transfer fails, 'X' it throws std::bad_alloc
};
std::vector<Inject> injects;

const char* ownerName(int owner) {
    if (!owner)
        return "none";
    if (owner == driverTid)
        return "driver";
    if (owner == publisherTid)
        return "publisher";
    return "other";
}

int presentOwner() {
    // glibc records the owner tid of a locked mutex (all mutex types).
    return presentMu ? presentMu->native_handle()->__data.__owner : 0;
}

void ctrlWindow(const char* name) {
    ++windows;
    char buf[160];
    std::snprintf(buf, sizeof buf, "W%d CTRL %s thr=driver presentMu=%s", windows, name,
                  ownerName(presentOwner()));
    rec(buf);
}

bool recordSend(const uint8_t* d, size_t len, uint8_t index) {
    reg();
    ++windows;
    const char* who = ownerName(presentOwner());
    char buf[360];
    if (len == fpga_overlay::kPacketBytes)
        std::snprintf(buf, sizeof buf,
                      "W%d SPI sendFileTx idx=%u len=%zu seq=%u epoch=%016llx nonce=%016llx "
                      "flags=0x%04x state=%u progress=%u hash=%016llx thr=%s presentMu=%s",
                      windows, index, len, fpga_overlay::get32(d + 24),
                      static_cast<unsigned long long>(fpga_overlay::get64(d + 8)),
                      static_cast<unsigned long long>(fpga_overlay::get64(d + 16)),
                      fpga_overlay::get16(d + 28), d[30], fpga_overlay::get16(d + 36),
                      static_cast<unsigned long long>(fnv(d, len)),
                      isDriver ? "driver" : "publisher", who);
    else
        std::snprintf(buf, sizeof buf,
                      "W%d SPI sendFileTx idx=%u len=%zu hash=%016llx thr=%s presentMu=%s", windows,
                      index, len, static_cast<unsigned long long>(fnv(d, len)),
                      isDriver ? "driver" : "publisher", who);
    rec(buf);
    const int64_t now = vnowMs.load();
    for (const Inject& i : injects)
        if (now >= i.fromMs && now <= i.toMs) {
            if (i.kind == 'X') {
                rec("  -> injected throw (std::bad_alloc)");
                throw std::bad_alloc();
            }
            rec("  -> injected SPI failure");
            return false;
        }
    return true;
}

} // namespace harness

// The FpgaSpi the extracted code names: the sender fpga_overlay::send calls, and the
// process-wide hot-path switch (22e50556 sets it once per session).
struct FpgaSpi {
    bool sendFileTx(const uint8_t* data, size_t len, uint8_t index) {
        return harness::recordSend(data, len, index);
    }
    static void setHotPathErrorsNonBlocking(bool on) {
        if (on)
            harness::rec("FpgaSpi::setHotPathErrorsNonBlocking(true)");
    }
};

struct VirtualOverlay {
    PlaybackOverlay po;
    bool visible() const { return po.visibleAt(harness::kOverlayBaseMs + harness::vnowMs.load()); }
};

struct SessionSpec {
    uint64_t epoch = 0, nonce = 0;
    uint32_t buildId = 0;
    int64_t beginMs = 0, firstAuMs = 0, presentMs = 0, startPosMs = 0;
    int64_t endMs = 0;    // stop, seek or EOF at this time
    bool eof = false;     // natural EOF: current() stays true through quiesce
    int64_t seekDeltaMs = 0;
    std::vector<int64_t> okShows;
    int64_t pauseMs = -1, resumeMs = -1;
};

struct HarnessPlayer {
    std::mutex presentMu_, mu_, pauseControlMu_;
    std::atomic<bool> paused_{false}, fpgaVideoStarted_{false};
    std::atomic<int64_t> positionMs_{0};
    std::string playbackTitle_ = "Big Buck Bunny (2008)";
    std::string fpgaOverlayTransport_;
    std::chrono::steady_clock::time_point fpgaOverlayTransportUntil_{};
    VirtualOverlay overlay_;
    FpgaSpi fpga_;
    std::atomic<uint64_t> overlayForceGen_{0};
    std::atomic<bool> sessionFailed{false};
    int64_t durationMsAll = 1800000;

    void log(const std::string& s) { harness::rec("LOG " + s); }
    void failFpgaSession(const std::string& why) {
        harness::rec("FAIL_SESSION " + why);
        sessionFailed = true;
    }
    // MediaPlayer::showPlaybackOverlay / flashPlaybackSkip on the virtual clock (the force
    // counter is 22e50556's; dbcc42fa's publisher never reads it).
    void showPlaybackOverlay(PlaybackOverlayState st, int64_t pos) {
        overlay_.po.showAt(st, pos, durationMsAll, harness::kOverlayBaseMs + harness::vnowMs.load());
        overlayForceGen_.fetch_add(1);
        harness::rec(std::string("EVENT show ") +
                     (st == PlaybackOverlayState::Paused ? "paused" : "playing") +
                     " pos=" + std::to_string(pos));
    }
    void flashPlaybackSkip(int64_t deltaMs, int64_t pos) {
        overlay_.po.flashSkipAt(deltaMs, pos, durationMsAll,
                                harness::kOverlayBaseMs + harness::vnowMs.load());
        {
            std::lock_guard<std::mutex> lock(mu_);
            const int64_t seconds = deltaMs / 1000;
            fpgaOverlayTransport_ = deltaMs >= 0 ? ">> " : "<< ";
            fpgaOverlayTransport_ += std::to_string(seconds >= 0 ? seconds : -seconds) + "S";
            fpgaOverlayTransportUntil_ =
                harness::steadyNow() + std::chrono::milliseconds(PlaybackOverlay::kSkipVisibleMs);
        }
        overlayForceGen_.fetch_add(1);
        harness::rec("EVENT skip " + std::to_string(deltaMs));
    }
    // pause()/resume(): the pause state, then presentMu_ across the control record and its
    // ACK polls (SPI windows), then the overlay show.
    void pauseResume(bool pause) {
        std::lock_guard<std::mutex> control(pauseControlMu_);
        if (pause)
            paused_ = true;
        {
            std::lock_guard<std::mutex> present(presentMu_);
            harness::ctrlWindow(pause ? "pause-record" : "resume-record");
            harness::ctrlWindow("ack-poll");
            harness::ctrlWindow("ack-poll");
        }
        if (!pause)
            paused_ = false;
        showPlaybackOverlay(pause ? PlaybackOverlayState::Paused : PlaybackOverlayState::Playing,
                            positionMs_.load());
    }

    int64_t positionAt(const SessionSpec& s, int64_t t) const {
        if (t < s.presentMs)
            return s.startPosMs;
        int64_t played = t - s.presentMs;
        if (s.pauseMs >= 0 && t > s.pauseMs) {
            const int64_t until = (s.resumeMs >= 0 && t > s.resumeMs) ? s.resumeMs : t;
            played -= until - s.pauseMs;
        }
        return s.startPosMs + played;
    }

    int64_t runSession(const SessionSpec& sp);
};

#if HARNESS_HAS_OLD
#define HARNESS_OLD_STARTED() (overlayThread.joinable())
#define HARNESS_OLD_STOP_PRED [&] { return stopOverlay.load(); }
#else
#define HARNESS_OLD_STARTED() false
#define HARNESS_OLD_STOP_PRED nullptr
#endif
#if HARNESS_HAS_LAB
#define HARNESS_LAB_STARTED() (!overlayPublisher.stopping())
#define HARNESS_LAB_STOP_PRED [&] { return overlayPublisher.stopping(); }
#else
#define HARNESS_LAB_STARTED() false
#define HARNESS_LAB_STOP_PRED nullptr
#endif

// One fpgaThreadMain session. Returns the position at its end.
int64_t HarnessPlayer::runSession(const SessionSpec& sp) {
    using namespace harness;
    const uint64_t session = sp.epoch;
    const int64_t durationMs = durationMsAll;
    std::atomic<bool> liveFlag{true};
    auto current = [&] { return liveFlag.load(); };
    struct {
        uint64_t nonce;
        uint32_t build_id;
    } caps{sp.nonce, sp.buildId};
    sessionFailed = false;
    fpgaVideoStarted_ = false;
    paused_ = false;
    positionMs_ = sp.startPosMs;
    {
        std::lock_guard<std::mutex> l(mu);
        sleeping = released = exitedFlag = false;
        wakePred = nullptr;
    }
    auto state = [&](int64_t t) {
        fpgaVideoStarted_ = t >= sp.presentMs;
        positionMs_ = positionAt(sp, t);
    };
    advanceTo(sp.beginMs, state);
    rec("SESSION begin epoch=" + std::to_string(sp.epoch) + " nonce=" + std::to_string(sp.nonce) +
        " build=" + std::to_string(sp.buildId));
    // ------------------------------------------------ the ref's publisher block
@BLOCK@
    // ------------------------------------------------ the first AU
    advanceTo(sp.firstAuMs, state);
    {
@CALLSITE@
    }
    const bool oldRunning = HARNESS_OLD_STARTED(), labRunning = HARNESS_LAB_STARTED();
    std::fprintf(stderr, "session epoch=%llu publisher=%s\n",
                 static_cast<unsigned long long>(sp.epoch),
                 oldRunning ? "base-loop" : labRunning ? "shared-publisher" : "none");
    if (oldRunning || labRunning) {
        std::unique_lock<std::mutex> l(mu);
        waitSettled(l);
    }
    struct Ev {
        int64_t ms;
        int kind; // 0 ok-show, 1 pause, 2 resume
    };
    std::vector<Ev> evs;
    for (int64_t t : sp.okShows)
        evs.push_back({t, 0});
    if (sp.pauseMs >= 0)
        evs.push_back({sp.pauseMs, 1});
    if (sp.resumeMs >= 0)
        evs.push_back({sp.resumeMs, 2});
    for (size_t i = 0; i < evs.size(); ++i)
        for (size_t j = i + 1; j < evs.size(); ++j)
            if (evs[j].ms < evs[i].ms)
                std::swap(evs[i], evs[j]);
    for (const Ev& e : evs) {
        if (e.ms >= sp.endMs)
            break;
        advanceTo(e.ms, state);
        if (sessionFailed)
            break;
        if (e.kind == 0)
            showPlaybackOverlay(PlaybackOverlayState::Playing, positionMs_.load());
        else
            pauseResume(e.kind == 1);
    }
    if (!sessionFailed)
        advanceTo(sp.endMs, state);
    const int64_t endPos = positionAt(sp, vnowMs.load());
    if (!sessionFailed && sp.seekDeltaMs)
        flashPlaybackSkip(sp.seekDeltaMs, endPos);
    // A stop, seek or failure supersedes the session (current() false); natural EOF does
    // not. Then finishFpgaPlayback: quiesce, then release.
    if (!sp.eof || sessionFailed)
        liveFlag = false;
    rec(std::string("END ") +
        (sessionFailed ? "failed" : sp.eof ? "eof" : sp.seekDeltaMs ? "seek" : "stop"));
    {
        std::lock_guard<std::mutex> l(mu);
        wakePred = labRunning ? std::function<bool()>(HARNESS_LAB_STOP_PRED)
                              : std::function<bool()>(HARNESS_OLD_STOP_PRED);
    }
    rec("QUIESCE");
    {
@QUIESCE@
    }
    rec("JOINED");
    {
@RELEASE@
    }
    rec("RELEASED");
    {
        std::lock_guard<std::mutex> l(mu);
        wakePred = nullptr;
    }
    return endPos;
}

int main(int argc, char** argv) {
    harness::isDriver = true;
    harness::driverTid = harness::tid();
    const std::string scenario = argc > 1 ? argv[1] : "session";
    const uint32_t build =
        argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 16)) : 0x48005D45u;
    std::fprintf(stderr, "ref=%s scenario=%s build=0x%08X\n", HARNESS_REF, scenario.c_str(),
                 static_cast<unsigned>(build));
    HarnessPlayer p;
    harness::presentMu = &p.presentMu_;
    {
        // The presentMu_ owner probe must work on this libc.
        std::lock_guard<std::mutex> l(p.presentMu_);
        if (harness::presentOwner() != harness::driverTid) {
            std::fprintf(stderr, "harness: mutex owner probe unsupported\n");
            return 2;
        }
    }
    if (harness::presentOwner() != 0)
        return 2;
    SessionSpec a;
    a.epoch = 0x1111000000000001ull;
    a.nonce = 0x2222000000000001ull;
    a.buildId = build;
    a.beginMs = 0;
    a.firstAuMs = 5;
    a.presentMs = 1000;
    a.startPosMs = 60000;
    a.endMs = 200000;
    a.seekDeltaMs = 30000;
    a.okShows = {60000};
    a.pauseMs = 120000;
    a.resumeMs = 130000;
    // Play start: playInternal shows the overlay, then the session thread runs.
    p.showPlaybackOverlay(PlaybackOverlayState::Playing, a.startPosMs);
    if (scenario == "session") {
        const int64_t endPos = p.runSession(a);
        // seekMs -> playInternal: the next session shows at once, begins 300 ms later.
        harness::advanceTo(200040, [](int64_t) {});
        p.showPlaybackOverlay(PlaybackOverlayState::Playing, endPos + a.seekDeltaMs);
        SessionSpec b = a;
        b.epoch = 0x1111000000000002ull;
        b.nonce = 0x2222000000000002ull;
        b.beginMs = 200300;
        b.firstAuMs = 200305;
        b.presentMs = 201300;
        b.startPosMs = endPos + a.seekDeltaMs;
        b.endMs = 300000;
        b.seekDeltaMs = 0;
        b.okShows = {250000};
        b.pauseMs = b.resumeMs = -1;
        p.runSession(b);
    } else if (scenario == "eof") {
        a.endMs = 150000;
        a.eof = true;
        a.seekDeltaMs = 0;
        p.runSession(a);
    } else if (scenario == "failures") {
        harness::injects = {{50000, 50300, 'F'}, {90000, 91000, 'F'}};
        a.endMs = 150000;
        a.seekDeltaMs = 0;
        p.runSession(a);
    } else if (scenario == "exception") {
        harness::injects = {{40000, 40100, 'X'}};
        a.endMs = 150000;
        a.seekDeltaMs = 0;
        p.runSession(a);
    } else {
        std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
        return 2;
    }
    harness::rec("EXIT");
    int sends = 0;
    for (const std::string& l : harness::lines) {
        std::printf("%s\n", l.c_str());
        sends += l.find(" SPI sendFileTx ") != std::string::npos;
    }
    std::fprintf(stderr, "records=%zu spi_sends=%d windows=%d\n", harness::lines.size(), sends,
                 harness::windows);
    return 0;
}
