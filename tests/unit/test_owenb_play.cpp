// MPX OWENB-P1B T5: the daemon's OB play worker (host/libmisterplex/owenb_play.cpp) on its own thread, with
// a HeapDdrWindow + the paced fake core, and source rows on the daemon wiring (media_player.cpp, companion.cpp,
// playback_controls.hpp, main.cpp): the OB route touches no legacy FPGA/fb path and never the idle FB.
#include "libmisterplex/owenb_play.hpp"
#include "fake_owenb_core.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex::owenb;

namespace {
int g_fails = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

std::string readFile(const std::string& rel) {
    std::ifstream f(std::string(MPX_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A looping synthetic stream: an AUD-only "IDR" every 12 AUs (the fake core does not decode), 24 fps PTS,
// 20 ms of PCM a frame. frames < 0: endless.
struct LoopSource final : IMediaSource {
    explicit LoopSource(int frames, int64_t startMs) : left(frames), n(startMs * 24 / 1000) {}
    int left;
    int64_t n;
    bool audioNext = false;
    SourceRead read(MediaPacket& p, std::string&) override {
        if (left == 0 && !audioNext)
            return SourceRead::Eof;
        if (audioNext) {
            audioNext = false;
            p.kind = MediaPacket::Kind::Audio;
            p.data.assign(3840, 0x11);
            p.pts90k = (n - 1) * 3750;
            return SourceRead::Packet;
        }
        p.kind = MediaPacket::Kind::Video;
        p.data = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x0C, 0xFF, 0xFF, 0x80};
        p.keyframe = n % 12 == 0;
        p.pts90k = n * 3750;
        ++n;
        if (left > 0)
            --left;
        audioNext = true;
        return SourceRead::Packet;
    }
};

bool waitFor(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f();
}

// The player owns (and drops) its window at stop; the test keeps the memory and the fake core alive behind it.
struct ProxyWindow final : OwenbDdrWindow {
    explicit ProxyWindow(HeapDdrWindow& h) : w(h) {}
    HeapDdrWindow& w;
    volatile uint64_t* ctrl() override { return w.ctrl(); }
    volatile uint64_t* ring() override { return w.ring(); }
    uint32_t ringSize() const override { return w.ringSize(); }
    bool mapped() const override { return true; }
    void sleepUs(unsigned us) override { w.sleepUs(us); }
    uint64_t nowUs() override { return w.nowUs(); }
};

struct Rig {
    Rig() {
        player = std::make_unique<OwenbPlayer>(
            [this](std::string& err) -> std::unique_ptr<OwenbDdrWindow> {
                if (failWindow) {
                    err = "scripted window failure";
                    return nullptr;
                }
                core.reset();
                heap = std::make_unique<HeapDdrWindow>(128u * 1024u);
                core = std::make_unique<fakeob::PacedCore>(*heap);
                core->alive = !deadCore;
                if (deadCore)
                    heap->ctrlWords()[0] = 0;
                ++windows;
                return std::make_unique<ProxyWindow>(*heap);
            },
            [this](const OpenRequest& q, std::string&) -> std::unique_ptr<IMediaSource> {
                ++opens;
                return std::make_unique<LoopSource>(frames, q.startMs);
            });
    }
    bool start(std::string& err) {
        OpenRequest q;
        q.url = "loop";
        SessionConfig cfg;
        cfg.idleUs = 1000;
        // The play thread's sleep: a real 100 us, then one fake-core poll (through the window).
        cfg.sleepUs = [this](unsigned) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (core)
                core->win.sleepUs(100);
        };
        OwenbPlayCallbacks cb;
        cb.started = [this] { ++startedCalls; };
        cb.ended = [this](EndReason r, const std::string&, int64_t) {
            std::lock_guard<std::mutex> g(endMu);
            ++endedCalls;
            endedWhy = r;
        };
        return player->start(q, 5, cfg, cb, err);
    }
    EndReason why() {
        std::lock_guard<std::mutex> g(endMu);
        return endedWhy;
    }
    std::unique_ptr<HeapDdrWindow> heap;
    std::unique_ptr<fakeob::PacedCore> core;
    std::unique_ptr<OwenbPlayer> player;   // destroyed first: its thread stops before the memory goes
    bool failWindow = false, deadCore = false;
    int frames = -1, windows = 0;
    std::atomic<int> opens{0};
    std::atomic<int> startedCalls{0};
    std::mutex endMu;
    int endedCalls = 0;
    EndReason endedWhy = EndReason::None;
};

// W1-W2: a window that does not open, or a core that is not alive: start fails, no thread, no callback.
void testStartRefusals() {
    {
        Rig r;
        r.failWindow = true;
        std::string err;
        CHECK(!r.start(err) && err.find("scripted") != std::string::npos && !r.player->active(),
              "W1 no window -> refused (%s)", err.c_str());
    }
    {
        Rig r;
        r.deadCore = true;
        std::string err;
        CHECK(!r.start(err) && err.find("not alive") != std::string::npos && r.endedCalls == 0,
              "W2 no live core -> refused (%s)", err.c_str());
    }
}

// W3-W5: a play on its own thread: the first picture, pause / resume, a held seek, volume, then stop() joins it:
// one ended(Stopped), the picture held (pause word set), the window released. W6: the end of input -> ended(Eof).
void testThreadedPlay() {
    Rig r;
    r.player->setVolumePercent(50);
    std::string err;
    CHECK(r.start(err) && r.player->active(), "W3 started (%s)", err.c_str());
    CHECK(waitFor([&] { return r.player->started(); }) && r.startedCalls.load() == 1, "W3 the first picture");
    r.player->pause();
    CHECK(waitFor([&] { return r.player->paused(); }), "W4 paused");
    r.player->resume();
    CHECK(waitFor([&] { return !r.player->paused(); }), "W4 resumed");
    OpenRequest q;
    q.url = "loop";
    q.startMs = 5000;
    r.player->seek(q);
    CHECK(waitFor([&] { return r.player->positionMs() >= 5000 && r.opens.load() == 2; }), "W4 held seek: position %lld",
          static_cast<long long>(r.player->positionMs()));
    r.player->stop();
    CHECK(!r.player->active() && r.endedCalls == 1 && r.why() == EndReason::Stopped, "W5 stop joins: ended(Stopped) once");
    CHECK(r.core && r.core->sessions.size() == 2 && r.core->paused() && r.core->atten == 12,
          "W5 a flush at the seek, the picture held at stop, volume 50 -> atten 12 (%zu, %u)",
          r.core ? r.core->sessions.size() : 0, r.core ? r.core->atten : 0);
    r.player->stop();   // idempotent
    CHECK(r.endedCalls == 1, "W5 a second stop is a no-op");

    Rig e;
    e.frames = 30;
    CHECK(e.start(err), "W6 started");
    CHECK(waitFor([&] { return !e.player->active(); }) && e.why() == EndReason::Eof, "W6 the end of input -> Eof (%s)",
          toString(e.why()));
    e.player->stop();
    CHECK(e.endedCalls == 1 && e.core && e.core->ended, "W6 one ended(Eof); the core took END_OF_STREAM");
}

std::string body(const std::string& src, const char* head) {
    const size_t a = src.find(head);
    if (a == std::string::npos)
        return {};
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

bool before(const std::string& s, const char* a, const char* b) {
    const size_t i = s.find(a), j = s.find(b);
    return i != std::string::npos && j != std::string::npos && i < j;
}

bool has(const std::string& s, const char* a) { return s.find(a) != std::string::npos; }

// D1-D8: the daemon wiring, read from the sources.
void testSources() {
    const std::string mp = readFile("arm/misterplexd/media_player.cpp");
    const std::string route = body(mp, "bool MediaPlayer::playOwenbRoute(");
    CHECK(!route.empty() && !has(route, "fpga_.") && !has(route, "fb_.") && !has(route, "paint") &&
              !has(route, "0x31600000") && !has(route, "kIdleFbPhys"),
          "D1 the OB route touches no legacy FPGA/fb path and never the idle FB");
    const std::string ens = body(mp, "void MediaPlayer::ensureOwenbPlayerLocked(");   // T7: shared with the idle session
    CHECK(!has(ens, "fpga_.") && !has(ens, "fb_.") && !has(ens, "0x31600000") && !has(ens, "kIdleFbPhys"),
          "D1 the shared OB player setup touches no legacy FPGA/fb path and never the idle FB");
    {   // FIELD-1018 F6: pads/keys/OSD during a play that no idle session preceded (a lab MiSTer Xbox pad)
        const size_t st = route.find("ob->start(");
        const size_t in = route.find("startObInput()", st == std::string::npos ? 0 : st);
        CHECK(st != std::string::npos && in != std::string::npos,
              "D9 the OB play path starts the input thread itself (not only the idle arm)");
    }
    CHECK(has(route, "ensureOwenbPlayerLocked()") && has(ens, "owenb::DevMemDdrWindow") && has(ens, "owenb::openAvSource") && has(route, "ob->start(") &&
              has(route, "owenbCavlcMaxKbpsFromEnv()") && has(route, "lastFrameHoldEnabled_"),
          "D1 the OB route opens the ring window, the libav input, the CAVLC cap and IDLE_SCREEN");
    for (const char* f : {"host/libmisterplex/owenb_play.cpp", "host/libmisterplex/owenb_session.cpp",
                          "arm/misterplexd/owenb_av_source.cpp"}) {
        const std::string s = readFile(f);
        CHECK(!s.empty() && !has(s, "0x316") && !has(s, "kIdleFbPhys") && !has(s, "fpga_") && !has(s, "FpgaSpi"),
              "D2 %s: no idle FB, no legacy FPGA path", f);
    }
    const std::string pause = body(mp, "void MediaPlayer::pause() {");
    const std::string resume = body(mp, "void MediaPlayer::resume() {");
    CHECK(before(pause, "if (owenbPlayActive()) {", "fpgaH264Backend()") &&
              before(resume, "if (owenbPlayActive()) {", "fpgaH264Backend()"),
          "D3 pause / resume take the OB branch before the legacy path");
    const std::string seek = body(mp, "void MediaPlayer::seekMs(");
    CHECK(before(seek, "owenbSeek(ms, started, generation)", "playInternal("), "D4 seekMs: the held OB seek first");
    const std::string stop = body(mp, "MediaPlayer::StoppedPosition MediaPlayer::stop() {");
    const std::string shut = body(mp, "void MediaPlayer::shutdown() {");
    CHECK(before(stop, "stopOwenbPlay();", "thr_.join()") && before(shut, "stopOwenbPlay();", "thr_.join()"),
          "D5 stop / shutdown join the OB play");
    const std::string ended = body(mp, "void MediaPlayer::owenbPlayEnded(");
    CHECK(has(ended, "EndReason::NeedsTranscode") && has(ended, "GateRow::R6p") && has(ended, "gateFallbackHandler_(f)"),
          "D6 NeedsTranscode hands off one lowered PMS request (R6p)");
    const std::string comp = readFile("arm/misterplexd/companion.cpp");
    CHECK(has(comp, "playback/setParameters") && has(comp, "onVolume_(vol)") && !has(comp, "volume=\\\"100\\\"") &&
              has(comp, "<< volume_.load() <<"),
          "D7 companion: setParameters volume, echoed in the timeline");
    const std::string mainSrc = readFile("arm/misterplexd/main.cpp");
    const std::string pc = readFile("arm/misterplexd/playback_controls.hpp");
    CHECK(has(mainSrc, "comp.setVolume([&player](int volume) { player.setVolumePercent(volume); });") &&
              before(pc, "owenbPlayActiveOf(player, 0)", "media.key.rfind(\"/library\", 0) == 0"),
          "D8 main wires the volume; a cast seek on OB stays in the running play");
    int reloadGuards = 0;
    const std::string guard = "legacyMenuReloadApplies(player.sessionBackend())";
    for (std::size_t at = 0; (at = mainSrc.find(guard, at)) != std::string::npos; at += guard.size())
        ++reloadGuards;
    CHECK(reloadGuards == 2, "D9 doPlay skips the Utility reload while the session is OwenbRing");
    int hostSplits = 0;
    const std::string split = "splitPlexHostPort(";
    for (std::size_t at = 0; (at = comp.find(split, at)) != std::string::npos; at += split.size())
        ++hostSplits;
    CHECK(hostSplits == 4, "D10 a port already in the cast address is the timeline port");
    int queueBumps = 0;
    const std::string bump = "playQueueVersionAfterReplace(";
    for (std::size_t at = 0; (at = comp.find(bump, at)) != std::string::npos; at += bump.size())
        ++queueBumps;
    CHECK(queueBumps == 2, "D11 a replaced play queue bumps the version Web already saw");
    CHECK(has(mainSrc, "lastBase = misterplex::joinPlexHostPort(req.protocol, req.address, req.port);") &&
              !has(mainSrc, "lastBase = proto + \"://\" + req.address + \":\" + port;"),
          "D12 a port already in the cast address is not appended again");
    const std::string resolveSrc = readFile("arm/misterplexd/plex_resolve.cpp");
    CHECK(has(resolveSrc, "ladderForPlaySession(") && has(resolveSrc, "sessionVideoBackend(") &&
              has(mainSrc, "player.fpgaH264Backend() && !owenbSession"),
          "D13 a PlexOB session resolves the Owenb ladder, not the FPGA 624 baseline");
    const char* early = "if (CoreGate::process().ownsIdleInFabric())\n        return false;";
    const std::string issue = body(mp, "bool issueMenuReloadForPair(");
    const std::string targetFn = body(mp, "bool MediaPlayer::tryMenuReloadForTargetBuild(");
    CHECK(before(issue, early, "utilityRbfForBuild(") && before(targetFn, early, "fpgaH264Backend()"),
          "D14 both reload functions return while the PlexOB core is already in the fabric");
    // D16 (a lab MiSTer bug 1): OB_CAVLC_MAX_LOAD_PCT reaches the session config.
    CHECK(has(body(mp, "bool MediaPlayer::playOwenbRoute("), "cfg.cavlcMaxLoadPct = owenbCavlcMaxLoadPctFromEnv();"),
          "D16 the CAVLC guard's load budget comes from OB_CAVLC_MAX_LOAD_PCT");
    // D17 (a lab MiSTer bug 3): the input loop logs an OSD action or key the mapping does not take.
    const std::string inLoop = body(mp, "void MediaPlayer::obInputLoop(");
    CHECK(has(inLoop, "unknown OSD action") && has(inLoop, "unmapped key"),
          "D17 the OB input loop logs unknown OSD actions and unmapped keys");
}

} // namespace

int main() {
    testStartRefusals();
    testThreadedPlay();
    testSources();
    std::printf("test_owenb_play: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
