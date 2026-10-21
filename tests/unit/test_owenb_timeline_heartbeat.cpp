// BUG2-1017: OwenbPlayer progress heartbeat -> PMS /:/timeline + companion poll time.
#include "libmisterplex/owenb_play.hpp"
#include "fake_owenb_core.hpp"
#include "pms_timeline.hpp"
#include "plex_resolve.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <sys/wait.h>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex::owenb;

namespace {

template<class T, class = void>
struct HasProgress : std::false_type {};
template<class T>
struct HasProgress<T, std::void_t<decltype(std::declval<T&>().progress)>> : std::true_type {};

int g_fails = 0, g_checks = 0;
#define ROW(name, cond, ...)                                                                       \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s: ", name);                                                        \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        } else {                                                                                   \
            std::printf("PASS %s\n", name);                                                        \
        }                                                                                          \
    } while (0)

std::string readFile(const std::string& rel) {
    std::ifstream f(std::string(MPX_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string body(const std::string& src, const char* head) {
    const size_t a = src.find(head);
    if (a == std::string::npos)
        return {};
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

std::string qp(const std::map<std::string, std::string>& p, const char* k) {
    const auto it = p.find(k);
    return it == p.end() ? std::string() : it->second;
}

std::map<std::string, std::string> queryParams(const std::string& url) {
    std::map<std::string, std::string> out;
    auto q = url.find('?');
    if (q == std::string::npos)
        return out;
    ++q;
    while (q < url.size()) {
        auto amp = url.find('&', q);
        auto part = url.substr(q, amp == std::string::npos ? std::string::npos : amp - q);
        auto eq = part.find('=');
        if (eq != std::string::npos)
            out[part.substr(0, eq)] = misterplex::urlDecode(part.substr(eq + 1));
        if (amp == std::string::npos)
            break;
        q = amp + 1;
    }
    return out;
}

std::string header(const misterplex::PmsTimelineHttpRequest& req, const std::string& name) {
    for (const auto& h : req.headers) {
        if (h.first == name)
            return h.second;
    }
    return {};
}

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

// Delays video until the core has been stepped enough times (first picture late).
struct DelayedVideoSource final : IMediaSource {
    DelayedVideoSource(int delaySteps, int64_t startMs, fakeob::PacedCore* core)
        : delay(delaySteps), inner(-1, startMs), core_(core) {}
    int delay;
    LoopSource inner;
    fakeob::PacedCore* core_;
    SourceRead read(MediaPacket& p, std::string& err) override {
        if (delay > 0 && core_ && core_->sessions.empty()) {
            --delay;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            p.kind = MediaPacket::Kind::Audio;
            p.data.assign(3840, 0x22);
            p.pts90k = 0;
            return SourceRead::Packet;
        }
        return inner.read(p, err);
    }
};

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

struct ProgressEvent {
    int64_t ms = 0;
    bool paused = false;
    std::chrono::steady_clock::time_point at{};
};

struct ProgressFeatureTag {};
struct NoProgressFeatureTag {};

using OwenbProgressTag =
    std::conditional_t<HasProgress<OwenbPlayCallbacks>::value, ProgressFeatureTag, NoProgressFeatureTag>;

template<class Rig, class Cb>
void wireDefaultProgress(Rig* self, Cb& cb, ProgressFeatureTag) {
    cb.progress = [self](int64_t ms, bool paused) {
        if (!self->startedFired.load())
            ++self->progressBeforeStarted;
        ProgressEvent ev{ms, paused, std::chrono::steady_clock::now()};
        {
            std::lock_guard<std::mutex> g(self->progressMu);
            self->progress.push_back(ev);
        }
        self->reporter->reportState(paused ? "paused" : "playing", ms, 2731712);
    };
}

template<class Rig, class Cb>
void wireDefaultProgress(Rig*, Cb&, NoProgressFeatureTag) {}

struct HeartbeatRig {
    std::unique_ptr<HeapDdrWindow> heap;
    std::unique_ptr<fakeob::PacedCore> core;
    std::unique_ptr<OwenbPlayer> player;
    std::vector<misterplex::PmsTimelineHttpRequest> sent;
    std::mutex sentMu;
    std::vector<ProgressEvent> progress;
    std::mutex progressMu;
    std::atomic<bool> startedFired{false};
    std::chrono::steady_clock::time_point startedAt{};
    std::unique_ptr<misterplex::PmsTimelineReporter> reporter;
    int progressBeforeStarted = 0;

    std::vector<misterplex::PmsTimelineHttpRequest> sentCopy() {
        std::lock_guard<std::mutex> g(sentMu);
        return sent;
    }
    void clearSent() {
        std::lock_guard<std::mutex> g(sentMu);
        sent.clear();
    }

    HeartbeatRig() {
        reporter = std::make_unique<misterplex::PmsTimelineReporter>(
            [this](const misterplex::PmsTimelineHttpRequest& req) {
                std::lock_guard<std::mutex> g(sentMu);
                sent.push_back(req);
                return misterplex::PmsTimelineSinkResult{true, 200};
            },
            false);
        player = std::make_unique<OwenbPlayer>(
            [this](std::string& err) -> std::unique_ptr<OwenbDdrWindow> {
                core.reset();
                heap = std::make_unique<HeapDdrWindow>(128u * 1024u);
                core = std::make_unique<fakeob::PacedCore>(*heap);
                return std::make_unique<ProxyWindow>(*heap);
            },
            [this](const OpenRequest& q, std::string&) -> std::unique_ptr<IMediaSource> {
                return std::make_unique<LoopSource>(-1, q.startMs);
            });
    }

    void wireCallbacks(int64_t startMs) {
        misterplex::PmsTimelineSession s;
        s.baseUrl = "http://127.0.0.1:32400";
        s.token = "hb-unit-token";
        s.ratingKey = "40780";
        s.key = "/library/metadata/40780";
        reporter->beginSession(s, startMs, 2731712);

        OwenbPlayCallbacks cb;
        cb.started = [this] {
            startedFired = true;
            startedAt = std::chrono::steady_clock::now();
        };
        wireDefaultProgress(this, cb, OwenbProgressTag{});
        callbacks_ = std::move(cb);
        startMs_ = startMs;
    }

    bool start(std::string& err) {
        OpenRequest q;
        q.url = "loop";
        q.startMs = startMs_;
        SessionConfig cfg;
        cfg.idleUs = 1000;
        cfg.sleepUs = [this](unsigned) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (core)
                core->win.sleepUs(100);
        };
        return player->start(q, 7, cfg, callbacks_, err);
    }

    OwenbPlayCallbacks callbacks_{};
    int64_t startMs_ = 3387;
};

void failNoProgressRows() {
    ROW("H1", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("H2", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("H3", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("H4", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("H5", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("H6", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("S1", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("S2", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
    ROW("S3", false,
        "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
}

void testH1() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        ROW("H1", false,
            "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
        return;
    }
    HeartbeatRig r;
    r.wireCallbacks(3387);
    std::string err;
    ROW("H1", r.start(err), "start failed: %s", err.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    r.player->stop();

    std::vector<misterplex::PmsTimelineHttpRequest> playingAfter;
    for (const auto& req : r.sentCopy()) {
        const auto p = queryParams(req.url);
        if (qp(p, "state") == "playing")
            playingAfter.push_back(req);
    }
    ROW("H1", playingAfter.size() >= 3, "need >=3 playing timeline URLs after first picture, got %zu",
        playingAfter.size());

    bool timesOk = playingAfter.size() >= 2;
    int64_t lastTime = -1;
    for (const auto& req : playingAfter) {
        const auto p = queryParams(req.url);
        const int64_t t = std::stoll(qp(p, "time"));
        if (lastTime >= 0 && t <= lastTime)
            timesOk = false;
        lastTime = t;
        if (qp(p, "duration") != "2731712" || qp(p, "ratingKey") != "40780")
            timesOk = false;
        if (req.url.find("%2Flibrary%2Fmetadata%2F40780") == std::string::npos)
            timesOk = false;
        if (header(req, "X-Plex-Token") != "hb-unit-token" ||
            header(req, "X-Plex-Client-Identifier") != "misterplex")
            timesOk = false;
    }
    ROW("H1", timesOk && lastTime >= 3387 + 2000, "playing times/fields monotonic, last=%lld", (long long)lastTime);
    if (!playingAfter.empty()) {
        const int64_t firstUrlT = std::stoll(qp(queryParams(playingAfter.front().url), "time"));
        ROW("H1", firstUrlT >= 3387, "first PMS playing time>=start offset 3387, got %lld", (long long)firstUrlT);
    }

    std::vector<ProgressEvent> prog;
    {
        std::lock_guard<std::mutex> g(r.progressMu);
        prog = r.progress;
    }
    bool cadenceOk = prog.size() >= 2;
    for (size_t i = 1; i < prog.size(); ++i) {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(prog[i].at - prog[i - 1].at);
        if (gap.count() < 900)
            cadenceOk = false;
    }
    ROW("H1", cadenceOk, "progress cadence >=900ms between calls (%zu events)", prog.size());
    if (!prog.empty())
        ROW("H1", prog.front().ms >= 3387, "first progress ms>=start offset 3387, got %lld",
            (long long)prog.front().ms);
}

void testH2() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        ROW("H2", false,
            "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
        return;
    }
    HeartbeatRig r;
    r.player = std::make_unique<OwenbPlayer>(
        [&r](std::string&) -> std::unique_ptr<OwenbDdrWindow> {
            r.core.reset();
            r.heap = std::make_unique<HeapDdrWindow>(128u * 1024u);
            r.core = std::make_unique<fakeob::PacedCore>(*r.heap);
            r.core->perStep = 1;
            return std::make_unique<ProxyWindow>(*r.heap);
        },
        [&r](const OpenRequest& q, std::string&) -> std::unique_ptr<IMediaSource> {
            return std::make_unique<DelayedVideoSource>(80, q.startMs, r.core.get());
        });
    r.wireCallbacks(1000);
    std::string err;
    ROW("H2", r.start(err), "start failed: %s", err.c_str());
    for (int i = 0; i < 500 && !r.startedFired.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ROW("H2", r.progressBeforeStarted == 0, "no progress before first picture (%d early)",
        r.progressBeforeStarted);
    if (r.startedFired.load()) {
        std::vector<ProgressEvent> prog;
        {
            std::lock_guard<std::mutex> g(r.progressMu);
            prog = r.progress;
        }
        ROW("H2", prog.empty() || prog.front().at >= r.startedAt, "first progress after started");
    }
    r.player->stop();
}

void testH3() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        ROW("H3", false,
            "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
        return;
    }
    HeartbeatRig r;
    r.wireCallbacks(3387);
    std::string err;
    ROW("H3", r.start(err), "start failed: %s", err.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    while (!r.startedFired.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    r.player->stop();
    const size_t nAtStop = r.progress.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ROW("H3", r.progress.size() == nAtStop, "no progress after stop (%zu -> %zu)", nAtStop, r.progress.size());
}

void testH4() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        ROW("H4", false,
            "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
        return;
    }
    HeartbeatRig r;
    r.wireCallbacks(5000);
    std::string err;
    ROW("H4", r.start(err), "start failed: %s", err.c_str());
    while (!r.player->started())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto sentBeforePauseSnap = r.sentCopy();
    const size_t sentBeforePause = sentBeforePauseSnap.size();
    size_t progBeforePause = 0;
    {
        std::lock_guard<std::mutex> g(r.progressMu);
        progBeforePause = r.progress.size();
    }
    r.player->pause();
    while (!r.player->paused())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));

    std::vector<ProgressEvent> pauseProg;
    {
        std::lock_guard<std::mutex> g(r.progressMu);
        for (size_t i = progBeforePause; i < r.progress.size(); ++i)
            pauseProg.push_back(r.progress[i]);
    }
    bool pauseProgOk = !pauseProg.empty();
    int64_t pausePos = pauseProg.empty() ? -1 : pauseProg.front().ms;
    for (const auto& e : pauseProg) {
        if (!e.paused || e.ms != pausePos)
            pauseProgOk = false;
    }
    ROW("H4", pauseProgOk, "while paused: progress paused=true and constant t (%zu events)", pauseProg.size());

    int pausedInWindow = 0, playingInPauseWindow = 0;
    const auto sentAtPause = r.sentCopy();
    for (size_t i = sentBeforePause; i < sentAtPause.size(); ++i) {
        const auto p = queryParams(sentAtPause[i].url);
        const std::string st = qp(p, "state");
        if (st == "paused")
            ++pausedInWindow;
        else if (st == "playing")
            ++playingInPauseWindow;
    }
    ROW("H4", pausedInWindow == 1, "reporter sent state=paused once while paused (%d)", pausedInWindow);
    ROW("H4", playingInPauseWindow == 0, "no state=playing timeline while paused (%d)", playingInPauseWindow);

    const auto sentBeforeResumeSnap = r.sentCopy();
    const size_t sentBeforeResume = sentBeforeResumeSnap.size();
    r.player->resume();
    while (r.player->paused())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Two playing reports at the 1 s cadence, plus slack so the second is not still in flight.
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));

    std::vector<int64_t> resumePlayingTimes;
    const auto sentAtResume = r.sentCopy();
    for (size_t i = sentBeforeResume; i < sentAtResume.size(); ++i) {
        const auto p = queryParams(sentAtResume[i].url);
        if (qp(p, "state") == "playing")
            resumePlayingTimes.push_back(std::stoll(qp(p, "time")));
    }
    bool resumeOk = resumePlayingTimes.size() >= 2;
    for (size_t i = 1; i < resumePlayingTimes.size(); ++i) {
        if (resumePlayingTimes[i] <= resumePlayingTimes[i - 1])
            resumeOk = false;
    }
    ROW("H4", resumeOk, "after resume: playing timeline times increase (%zu reports)",
        resumePlayingTimes.size());

    std::vector<ProgressEvent> resumeProg;
    {
        std::lock_guard<std::mutex> g(r.progressMu);
        for (size_t i = progBeforePause + pauseProg.size(); i < r.progress.size(); ++i)
            resumeProg.push_back(r.progress[i]);
    }
    bool resumeProgOk = resumeProg.size() >= 2;
    for (size_t i = 1; i < resumeProg.size(); ++i) {
        if (resumeProg[i].paused || resumeProg[i].ms <= resumeProg[i - 1].ms)
            resumeProgOk = false;
    }
    ROW("H4", resumeProgOk, "after resume: progress playing with increasing t (%zu events)", resumeProg.size());
    r.player->stop();
}

template<class Rig>
void wireSlowProgress(Rig* r, std::atomic<bool>* stopReturned, std::atomic<int>* progressAfterStop,
                      ProgressFeatureTag) {
    r->callbacks_.progress = [r, stopReturned, progressAfterStop](int64_t ms, bool paused) {
        if (stopReturned->load()) {
            ++(*progressAfterStop);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (stopReturned->load()) {
            ++(*progressAfterStop);
            return;
        }
        ProgressEvent ev{ms, paused, std::chrono::steady_clock::now()};
        std::lock_guard<std::mutex> g(r->progressMu);
        r->progress.push_back(ev);
        r->reporter->reportState(paused ? "paused" : "playing", ms, 2731712);
    };
}

template<class Rig>
void wireSlowProgress(Rig*, std::atomic<bool>*, std::atomic<int>*, NoProgressFeatureTag) {}

void testH5a() {
    HeartbeatRig r;
    r.wireCallbacks(2000);
    std::atomic<bool> stopReturned{false};
    std::atomic<int> progressAfterStop{0};
    wireSlowProgress(&r, &stopReturned, &progressAfterStop, OwenbProgressTag{});
    std::string err;
    ROW("H5", r.start(err), "start failed: %s", err.c_str());
    while (!r.player->started())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto fut = std::async(std::launch::async, [&] {
        r.player->stop();
        stopReturned.store(true);
    });
    if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        std::printf("FAIL H5: stop() deadlocked (>3s) with slow progress callback\n");
        std::_Exit(1);
    }
    fut.get();
    ROW("H5", progressAfterStop == 0, "no progress callback started after stop returned (%d)",
        progressAfterStop.load());
}

template<class Rig>
void wireLockOrderProgress(Rig* r, std::mutex* companionMu, std::atomic<uint64_t>* playEpoch, uint64_t epoch,
                           std::mt19937* rng, std::atomic<bool>* stalePost, ProgressFeatureTag) {
    r->callbacks_.progress = [r, companionMu, playEpoch, epoch, rng, stalePost](int64_t ms, bool paused) {
        std::lock_guard<std::mutex> g(*companionMu);
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + ((*rng)() % 40)));
        if (playEpoch->load() != epoch)
            return;
        ProgressEvent ev{ms, paused, std::chrono::steady_clock::now()};
        {
            std::lock_guard<std::mutex> pg(r->progressMu);
            r->progress.push_back(ev);
        }
        if (playEpoch->load() != epoch)
            stalePost->store(true);
        else
            r->reporter->reportState(paused ? "paused" : "playing", ms, 2731712);
    };
}

template<class Rig>
void wireLockOrderProgress(Rig*, std::mutex*, std::atomic<uint64_t>*, uint64_t, std::mt19937*, std::atomic<bool>*,
                           NoProgressFeatureTag) {}

void testH5b() {
    std::mutex companionMu;
    std::atomic<uint64_t> playEpoch{1};
    std::atomic<bool> stalePost{false};
    std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

    for (int trial = 0; trial < 20; ++trial) {
        HeartbeatRig r;
        const uint64_t epoch = playEpoch.load();
        r.wireCallbacks(1500);
        wireLockOrderProgress(&r, &companionMu, &playEpoch, epoch, &rng, &stalePost, OwenbProgressTag{});
        std::string err;
        if (!r.start(err))
            continue;
        while (!r.player->started())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(10 + (rng() % 80)));
        auto fut = std::async(std::launch::async, [&] {
            {
                std::lock_guard<std::mutex> g(companionMu);
                playEpoch.fetch_add(1, std::memory_order_acq_rel);
            }
            r.player->stop();
        });
        if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
            std::printf("FAIL H5: stop() deadlocked (>3s) trial %d (daemon lock order)\n", trial);
            std::_Exit(1);
        }
        fut.get();
    }
    ROW("H5", !stalePost.load(), "epoch guard: no timeline post after epoch bump");
}

void testH5() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        ROW("H5", false,
            "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
        return;
    }
    testH5a();
    testH5b();
}

void testH6() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        ROW("H6", false,
            "OwenbPlayCallbacks has no progress heartbeat (PMS/companion time frozen at playMedia offset)");
        return;
    }
    HeartbeatRig r;
    r.wireCallbacks(3387);
    std::string err;
    ROW("H6", r.start(err), "play A start failed: %s", err.c_str());
    while (!r.player->started())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    r.player->stop();

    r.clearSent();
    {
        std::lock_guard<std::mutex> g(r.progressMu);
        r.progress.clear();
    }
    r.startedFired = false;
    r.wireCallbacks(60000);
    ROW("H6", r.start(err), "play B start failed: %s", err.c_str());
    while (!r.player->started())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));

    bool positionsOk = true;
    {
        std::lock_guard<std::mutex> g(r.progressMu);
        for (const auto& e : r.progress) {
            if (e.ms < 60000)
                positionsOk = false;
        }
    }
    ROW("H6", positionsOk && !r.progress.empty(), "play B progress all >=60000 (%zu events)", r.progress.size());
    r.player->stop();
}

void testS1() {
    const std::string mp = readFile("arm/misterplexd/media_player.cpp");
    const std::string route = body(mp, "bool MediaPlayer::playOwenbRoute(");
    const size_t progAt = route.find("cb.progress = [this, epoch]");
    const size_t progEnd = progAt == std::string::npos ? std::string::npos : route.find("};", progAt);
    const std::string progLambda =
        progAt == std::string::npos ? std::string() : route.substr(progAt, progEnd - progAt);
    const bool wiringOk = progLambda.find("playEpoch_.load() != epoch") != std::string::npos &&
                          progLambda.find("companionTimelineTick()") != std::string::npos;
    ROW("S1", wiringOk, "cb.progress has epoch guard and companionTimelineTick");

    const std::string tickBody = body(mp, "void MediaPlayer::companionTimelineTick()");
    const bool tickOk = !tickBody.empty() && tickBody.find("onProgress_") != std::string::npos &&
                        tickBody.find("lock_guard") == std::string::npos;
    ROW("S1", tickOk, "companionTimelineTick does not hold mu_ across onProgress_");
}

void testS2() {
    const std::string mp = readFile("arm/misterplexd/media_player.cpp");
    const std::string seek = body(mp, "bool MediaPlayer::owenbSeek(");
    const bool ok = seek.find("onProgress_") != std::string::npos;
    ROW("S2", ok, "owenbSeek calls onProgress_ for immediate seek tick");
}

void testS3() {
    // 970247865 is owenb v2 bugfix 1d, the substrate these heartbeat commits sit on.
    // 09788cbc0 is not in this history, so a missing object used to make the diff empty and S3 pass.
    const std::string base = "970247865df041409bba093b8cdb3c8f26128084";
    const std::string cmd = std::string("git -C ") + MPX_SOURCE_ROOT + " diff " + base + "..HEAD";
    std::string diff;
    int rc = -1;
    if (FILE* pipe = popen(cmd.c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, pipe))
            diff += buf;
        rc = pclose(pipe);
    }
    const bool gitOk = rc >= 0 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
    bool bad = !gitOk || diff.empty();
    size_t pos = 0;
    while (!bad && (pos = diff.find("namespace owenb {", pos)) != std::string::npos) {
        if (pos == 0 || diff[pos - 1] == '\n' || diff[pos - 1] == '+')
            bad = true;
        ++pos;
    }
    ROW("S3", !bad, "diff vs 970247865 adds no global namespace owenb (git_ok=%d bytes=%zu)",
        gitOk ? 1 : 0, diff.size());
}

} // namespace

int main() {
    if constexpr (!HasProgress<OwenbPlayCallbacks>::value) {
        failNoProgressRows();
        std::printf("test_owenb_timeline_heartbeat: %d checks, %d failures\n", g_checks, g_fails);
        return 1;
    }
    testH1();
    testH2();
    testH3();
    testH4();
    testH5();
    testH6();
    testS1();
    testS2();
    testS3();
    std::printf("test_owenb_timeline_heartbeat: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
