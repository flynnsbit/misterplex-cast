#include <chrono>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define private public
#include "companion.hpp"
#undef private
#ifndef MPX_LEGACY_EOF
#include "natural_eof.hpp"
#ifndef MPX_LEGACY_CONTROL
#include "playback_controls.hpp"
#endif
#endif
#ifndef MPX_LEGACY_EOF
#include "pms_timeline.hpp"
#endif

static void require(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct HostCompanion {
    misterplex::Companion companion;
    std::atomic<uint64_t> generation{0}, seekGeneration{0}, epoch{1};
    std::atomic<bool> inFlight{false};

    HostCompanion() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "loopback socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "ephemeral loopback bind");
        socklen_t size = sizeof(address);
        require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0 &&
                listen(fd, 8) == 0, "loopback listen");
        companion.port_ = ntohs(address.sin_port);
        companion.httpListenFd_ = fd;
        companion.running_.store(true);
        companion.setPlayQueued([this] { return ++generation; });
#if defined(MPX_LEGACY_EOF) || defined(MPX_LEGACY_CONTROL)
        companion.setSeek([this](int64_t) { ++seekGeneration; });
#else
        companion.setTransportQueued([this](misterplex::TransportCommand command) {
            misterplex::TransportGenerations generations{generation, seekGeneration};
            return generations.accept(command, epoch.load());
        });
        companion.setSeek([](const misterplex::TransportRequest&) {});
#endif
        companion.httpThr_ = std::thread([this] { companion.httpLoop(); });
    }
    ~HostCompanion() { companion.stop(); }

    std::string get(const std::string& path) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "request socket");
        timeval timeout{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(companion.port_);
        require(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "request connect");
        const std::string request = "GET " + path +
            " HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        require(send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(request.size()), "request send");
        shutdown(fd, SHUT_WR);
        std::string response;
        char buffer[4096];
        ssize_t count;
        while ((count = recv(fd, buffer, sizeof(buffer), 0)) > 0)
            response.append(buffer, static_cast<size_t>(count));
        close(fd);
        require(count == 0 && response.find("200 OK") != std::string::npos,
                "request completed successfully");
        return response;
    }
    std::string poll() { return get("/player/timeline/poll?wait=0"); }
    std::string mirror(int key = 146) {
        return get("/player/timeline/mirror?key=%2Flibrary%2Fmetadata%2F" +
                   std::to_string(key));
    }
    void play(int key = 146, int64_t offset = 0) {
        get("/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F" +
            std::to_string(key) + "&offset=" + std::to_string(offset));
        misterplex::PlayRequest request;
        request.key = "/library/metadata/" + std::to_string(key);
        require(companion.bindMedia(request, 16036), "bind staged request");
    }
    void playing(int64_t time = 15974) {
        companion.setState("playing", time, 16036);
    }
    void expect(const std::string& state, int64_t time) {
        const auto response = poll();
        require(response.find("state=\"" + state + "\"") != std::string::npos,
                "expected " + state + " timeline");
        require(response.find("time=\"" + std::to_string(time) + "\"") != std::string::npos,
                "original timeline position");
    }
#ifndef MPX_LEGACY_EOF
    auto current() {
        return [this, g = generation.load(), s = seekGeneration.load(), e = epoch.load()] {
            return generation.load() == g && seekGeneration.load() == s && epoch.load() == e;
        };
    }
    template<class Advance>
    std::function<void()> eof(Advance advance, int64_t time = 15974) {
        std::function<void()> pending;
        misterplex::handleNaturalEof(companion, inFlight, time, 16036, current(),
            advance, [&](auto callback) { pending = std::move(callback); });
        return pending;
    }
#endif
};

int runNaturalEofTests() {
    HostCompanion host;
    host.mirror();
    host.play();
    host.playing();
#ifdef MPX_LEGACY_EOF
    // Exact no-next state calls from the preserved main.cpp, then observed mirror.
    host.companion.setState("buffering", 15974, 16036);
    host.companion.setState("stopped", 15974, 16036);
    host.mirror();
    host.expect("stopped", 15974);
#else
    auto noNext = host.eof([] { return false; });
    require(static_cast<bool>(noNext), "EOF dispatch");
    host.expect("buffering", 15974);
    noNext();
    host.expect("stopped", 15974);
    require(!host.inFlight.load(), "EOF worker retired");
    host.mirror();
    host.mirror(139);
    host.expect("stopped", 15974);
    for (const auto* state : {"paused", "playing", "buffering"}) {
        host.companion.setState(state, 0, 16036);
        host.expect("stopped", 15974);
    }
    require(host.poll().find("16036") != std::string::npos, "duration retained, not padded PTS");
    host.get("/player/playback/pause");
    host.get("/player/playback/play");
    host.expect("stopped", 15974);
    host.get("/player/playback/stop");
    host.mirror();
    host.expect("stopped", 0);

    host.play(139);
    host.expect("buffering", 0);
    host.mirror(146);
    require(host.poll().find("key=\"/library/metadata/139\"") != std::string::npos,
            "old mirror cannot replace loading new title");
    host.playing(5000);
    host.get("/player/playback/pause");
    host.expect("paused", 5000);
    host.mirror(146);
    host.expect("paused", 5000);
    host.get("/player/playback/play");
    host.expect("playing", 5000);

    noNext = host.eof([] { return false; });
    host.play(146);
    noNext();
    host.expect("buffering", 0);
    host.playing();
    noNext = host.eof([] { return false; });
    host.play(146); // same key still has a new dispatch generation
    host.playing(700);
    noNext();
    host.expect("playing", 700);

    host.playing();
    noNext = host.eof([] { return false; });
    host.get("/player/playback/seekTo?offset=8000");
    noNext();
    host.expect("buffering", 8000);
    host.companion.setState("playing", 0, 16036);
    host.expect("playing", 8000);
    host.playing(8400);
    host.expect("playing", 8400);
    host.playing(15974);
    host.eof([] { return false; })();
    host.expect("stopped", 15974);

    host.play(146, 15900);
    // EOF can legitimately arrive inside the 400ms seek catch-up window.
    host.eof([] { return false; })();
    host.expect("stopped", 15974);
    host.play();
    host.playing(700);
    host.eof([] { return false; }, 1234)();
    host.expect("stopped", 1234);

    host.play();
    host.playing();
    auto advance = host.eof([&] {
        misterplex::PlayRequest next;
        next.key = "/library/metadata/139";
        ++host.generation;
        ++host.epoch;
        return host.companion.stagePlay(next);
    });
    advance();
    host.expect("buffering", 0);
    host.playing(1200);
    host.mirror(146);
    host.expect("playing", 1200);

    noNext = host.eof([] { throw std::runtime_error("queue failure"); return false; });
    noNext();
    host.expect("stopped", 15974);
    host.play();
    host.playing();
    noNext = host.eof([&] {
        host.play(139); // cast arrived while the queue lookup was in flight
        return false;
    });
    noNext();
    host.expect("buffering", 0);
    host.playing();
    noNext = host.eof([] { return false; });
    ++host.epoch; // worker replaced without a new cast request
    noNext();
    host.expect("buffering", 15974);

    host.inFlight.store(true);
    require(!host.eof([] { return false; }), "busy queue owner not replaced");
    host.expect("stopped", 15974);
    require(host.inFlight.load(), "busy owner retains its reservation");
    host.inFlight.store(false);
    const auto stale = host.current();
    host.play(139);
    misterplex::handleNaturalEof(host.companion, host.inFlight, 15974, 16036, stale,
        [] { return false; }, [](auto) { require(false, "stale EOF scheduled"); });
    host.expect("buffering", 0);
    misterplex::PlayRequest staleNext;
    staleNext.key = "/library/metadata/146";
    require(!host.companion.stagePlay(staleNext, stale), "stale queue result rejected atomically");
    host.expect("buffering", 0);
    bool launchFailed = false;
    try {
        misterplex::handleNaturalEof(host.companion, host.inFlight, 1234, 16036,
            host.current(), [] { return false; },
            [](auto) { throw std::runtime_error("host launch failure"); });
    } catch (const std::runtime_error&) {
        launchFailed = true;
    }
    require(launchFailed && !host.inFlight.load(), "failed dispatch retires reservation");
    host.expect("stopped", 1234);
    host.play(139);
    host.get("/player/playback/stop");
    require(host.poll().find("location=\"navigation\"") != std::string::npos,
            "explicit stop keeps navigation semantics");
    host.mirror();
    host.expect("buffering", 0);
#endif
    std::cout << "test_companion_natural_eof: OK (production EOF callback + loopback HTTP; no "
                 "player/device)\n";
    return 0;
}

#ifndef MPX_LEGACY_EOF
namespace {
std::mutex hookMutex;
std::function<void()> sendHook, unlockHook;
std::atomic<pthread_mutex_t*> unlockTarget{nullptr};
std::atomic<unsigned> unlockRemaining{0};

void atSend(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(hookMutex);
    sendHook = std::move(hook);
}

void atUnlock(std::mutex& target, std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(hookMutex);
    unlockHook = std::move(hook);
    // The HTTP loop updates castBound before accepting the transport control.
    unlockRemaining.store(2);
    unlockTarget.store(target.native_handle());
}
} // namespace

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);

extern "C" ssize_t __wrap_send(int fd, const void* data, size_t length, int flags) {
    std::function<void()> hook;
    if (length >= 5 && std::string(static_cast<const char*>(data), 5) == "HTTP/") {
        std::lock_guard<std::mutex> lock(hookMutex);
        hook = std::move(sendHook);
        sendHook = {};
    }
    if (hook)
        hook();
    return __real_send(fd, data, length, flags);
}

extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    const int result = __real_pthread_mutex_unlock(mutex);
    auto* expected = mutex;
    if (unlockTarget.load() == mutex && unlockRemaining.fetch_sub(1) == 1 &&
        unlockTarget.compare_exchange_strong(expected, nullptr)) {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(hookMutex);
            hook = std::move(unlockHook);
            unlockHook = {};
        }
        if (hook)
            hook();
    }
    return result;
}

struct PlayerControlDouble {
    explicit PlayerControlDouble(std::atomic<uint64_t>& value) : epoch(value) {}
    std::atomic<uint64_t>& epoch;
    std::atomic<bool> live{false};
    std::atomic<int64_t> position{0}, duration{0};
    std::atomic<int> pauses{0}, resumes{0}, stops{0};
    std::atomic<int> seeks{0}, sessions{0};
    std::function<void(const std::string&, int64_t, int64_t)> progress;
    std::function<uint64_t()> reportingGeneration;
    struct Binding {
        uint64_t generation = 0;
        uint64_t epoch = 0;
    };
    struct Media {
        std::string url;
        std::string headers;
        Binding binding;
    };
    mutable std::mutex mediaMutex;
    Media installed;
    struct Position {
        int64_t timeMs, durationMs;
    };
    uint64_t playbackEpoch() const { return epoch.load(); }
    bool playing() const { return live.load(); }
    Binding startedPlayback() const {
        std::lock_guard<std::mutex> lock(mediaMutex);
        return installed.url.empty() || installed.binding.epoch != epoch.load()
            ? Binding{} : installed.binding;
    }
    Media media() const {
        std::lock_guard<std::mutex> lock(mediaMutex);
        return installed;
    }
    void install(uint64_t generation, const std::string& url, const std::string& headers,
                 int64_t offset = 5000) {
        std::lock_guard<std::mutex> lock(mediaMutex);
        ++epoch;
        ++sessions;
        installed = {url, headers, {generation, epoch.load()}};
        position.store(offset);
        duration.store(16036);
        live.store(true);
    }
    void loading(const std::string& url = {}, const std::string& headers = {}) {
        std::lock_guard<std::mutex> lock(mediaMutex);
        ++epoch;
        installed = {url, headers, {}};
        live.store(false);
    }
    Position stop() {
        ++epoch;
        ++stops;
        const Position final{position.load() + 7, duration.load()};
        if (progress)
            progress("stopped", final.timeMs, final.durationMs);
        live.store(false);
        position.store(0);
        duration.store(0);
        { std::lock_guard<std::mutex> lock(mediaMutex); installed = {}; }
        return final;
    }
    void pause() {
        if (!live.load())
            return;
        ++pauses;
        if (progress)
            progress("paused", position.load(), duration.load());
    }
    void resume() {
        if (!live.load())
            return;
        ++resumes;
        if (progress)
            progress("playing", position.load(), duration.load());
    }
    void seekMs(int64_t offset, const std::function<void()>& started, uint64_t generation = 0) {
        ++seeks;
        bool samePosition = false;
        {
            std::lock_guard<std::mutex> lock(mediaMutex);
            if (installed.url.empty())
                return;
            samePosition = live.load() && position.load() == offset;
            if (!samePosition) {
                ++epoch;
                ++sessions;
            }
        }
        started();
        {
            std::lock_guard<std::mutex> lock(mediaMutex);
            installed.binding = {generation ? generation :
                reportingGeneration ? reportingGeneration() : 0, epoch.load()};
        }
        position.store(offset);
        live.store(true);
        if (progress && !samePosition)
            progress("playing", offset, duration.load());
    }
};

int runControlWiringTests(const std::string& selected) {
    HostCompanion host;
    PlayerControlDouble player{host.epoch};
    std::atomic<uint64_t> activePlay{0};
    player.reportingGeneration = [&] { return activePlay.load(); };
    std::mutex handoff, seekMutex, pendingMutex, reportMutex, sessionMutex;
    std::vector<std::function<void()>> pending;
    std::vector<misterplex::PlayRequest> starts;
    std::vector<misterplex::PmsTimelineHttpRequest> reports;
    misterplex::PlayRequest lastPlay;
    int cleared = 0;
    misterplex::PmsTimelineReporter reporter(
        [&](const auto& request) {
            std::lock_guard<std::mutex> lock(reportMutex);
            reports.push_back(request);
            return misterplex::PmsTimelineSinkResult{true, 200};
        },
        false);
    auto post = [&](auto callback) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pending.push_back(std::move(callback));
    };
    auto drain = [&] {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            callbacks.swap(pending);
        }
        for (auto& callback : callbacks)
            callback();
    };
    bool failStart = false;
    auto start = [&](const misterplex::PlayRequest& request) {
        if (failStart)
            throw std::runtime_error("controlled start failure");
        starts.push_back(request);
    };
    auto clear = [&](uint64_t = 0) {
        std::lock_guard<std::mutex> lock(sessionMutex);
        lastPlay = {};
        ++cleared;
    };
    player.progress = [&](const std::string& state, int64_t t, int64_t d) {
        if (activePlay.load() != host.generation.load())
            return;
        if (state == "stopped")
            reporter.endSession(t, d);
        host.companion.setState(state, t, d);
    };
#ifdef MPX_LEGACY_CONTROL
    host.companion.setPause([&] { player.pause(); });
    host.companion.setResume([&] { player.resume(); });
    host.companion.setStop([&] {
        std::lock_guard<std::mutex> lock(handoff);
        ++host.generation;
        player.stop();
        clear();
    });
    host.companion.setSeek([&](int64_t offset) {
        const uint64_t generation = ++host.seekGeneration;
        post([&, offset, generation] {
            std::lock_guard<std::mutex> lock(seekMutex);
            if (generation != host.seekGeneration.load())
                return;
            misterplex::PlayRequest request;
            {
                std::lock_guard<std::mutex> session(sessionMutex);
                request = lastPlay;
            }
            request.offsetMs = offset;
            request.offsetPresent = true;
            request.dispatchGeneration = 0;
            start(request);
        });
    });
#else
    misterplex::TransportGenerations generations{host.generation, host.seekGeneration};
    misterplex::wirePlaybackControls(host.companion, player, reporter, generations, activePlay,
                                     handoff, seekMutex, start, clear, post);
#endif
    auto prepare = [&](int key = 146, int64_t position = 5000) {
        host.play(key);
        host.playing(position);
        player.install(host.generation.load(), "resolved-library-" + std::to_string(key),
                       "library-headers", position);
        activePlay.store(host.generation.load());
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            lastPlay = {};
            lastPlay.key = "/library/metadata/" + std::to_string(key);
        }
        misterplex::PmsTimelineSession session;
        session.baseUrl = "http://127.0.0.1:1";
        session.token = "offline-fixture";
        session.key = "/library/metadata/" + std::to_string(key);
        session.ratingKey = std::to_string(key);
        reporter.beginSession(session, position, 16036);
        {
            std::lock_guard<std::mutex> lock(reportMutex);
            reports.clear();
        }
        starts.clear();
    };
    if (selected.empty() || selected == "stop") {
        prepare();
        host.get("/player/playback/stop");
        std::lock_guard<std::mutex> lock(reportMutex);
        require(reports.size() == 1 && reports[0].url.find("state=stopped") != std::string::npos &&
                    reports[0].url.find("ratingKey=146") != std::string::npos &&
                    reports[0].url.find("time=5007") != std::string::npos &&
                    reports[0].url.find("duration=16036") != std::string::npos,
                "explicit Stop retires exactly the outgoing PMS session at "
                "post-join position");
        require(player.position.load() == 0 && player.duration.load() == 0 && cleared == 1,
                "retirement does not read cleared player values");
        host.expect("buffering", 0);
    }
    if (selected.empty() || selected == "seek" || selected == "step") {
        for (const std::string path :
             {"/player/playback/seekTo?offset=7000", "/player/playback/seek?time=7000",
              "/player/playback/seekTo?viewOffset=7000", "/player/playback/stepForward?offset=2000",
              "/player/playback/stepBack?offset=2000"}) {
            if (selected == "step" && path.find("step") == std::string::npos)
                continue;
            prepare();
            if (path.find("stepBack") != std::string::npos) {
                host.playing(9000);
                player.position.store(9000);
            }
            const auto current = host.current();
            bool oldQueueAdvanced = false;
            atSend([&] {
                if (!current())
                    return;
                oldQueueAdvanced = true;
                misterplex::PlayRequest next;
                next.key = "/library/metadata/139";
                host.companion.stagePlay(next);
                std::lock_guard<std::mutex> lock(sessionMutex);
                lastPlay = next;
            });
            host.get(path);
            drain();
            require(!oldQueueAdvanced && starts.size() == 1 &&
                        starts[0].key == "/library/metadata/146" && starts[0].offsetMs == 7000 &&
                        starts[0].dispatchGeneration == host.generation.load(),
                    "accepted seek blocks old queue before response and retains its "
                    "identity");
        }
    }
    if (selected.empty() || selected == "pause" || selected == "resume") {
        for (const std::string path : {"/player/playback/pause", "/player/playback/play"}) {
            if (selected == "resume" && path != "/player/playback/play")
                continue;
            prepare();
            if (path == "/player/playback/play")
                host.companion.setState("paused", 5000, 16036);
            atUnlock(host.companion.mu_, [&] {
#ifndef MPX_LEGACY_CONTROL
                require(host.companion.state_ ==
                            (path == "/player/playback/pause" ? "paused" : "playing"),
                        "control state is published in the eligibility critical section");
#endif
                player.live.store(false);
                auto terminal = host.eof([] { return false; }, 5000);
                require(static_cast<bool>(terminal), "interleaved EOF queued");
                terminal();
            });
            host.get(path);
            host.expect("stopped", 5000);
            require(player.pauses.load() == 0 && player.resumes.load() == 0,
                    "ended player receives no deferred pause/resume");
        }
    }
#ifndef MPX_LEGACY_CONTROL
    prepare();
    host.get("/player/playback/pause");
    host.expect("paused", 5000);
    host.get("/player/playback/play");
    host.expect("playing", 5000);
    require(player.pauses.load() == 1 && player.resumes.load() == 1, "active controls retained");

    prepare();
    host.get("/player/playback/seekTo?offset=7000");
    host.get("/player/playback/seekTo?offset=9000");
    drain();
    require(starts.size() == 1 && starts[0].offsetMs == 9000,
            "older accepted offset cannot acquire newer seek identity");
    prepare();
    failStart = true;
    host.get("/player/playback/seekTo?offset=7000");
    drain();
    require(starts.empty(), "failed start did not report success");
    failStart = false;
    host.get("/player/playback/seekTo?offset=9000");
    drain();
    require(starts.size() == 1 && starts[0].offsetMs == 9000,
            "seek serialization is released after an exception");
    prepare();
    host.get("/player/playback/seekTo?offset=7000");
    host.play(139);
    drain();
    require(starts.empty(), "old seek cannot adopt a newer cast");
    host.get("/player/playback/seekTo?offset=8000");
    drain();
    require(starts.size() == 1 && starts[0].key == "/library/metadata/139",
            "seek uses accepted loading title, not the stale lastPlay cache");

    prepare();
    auto oldPause = host.companion.onPause_;
    const auto accepted =
        generations.accept(misterplex::TransportCommand::Pause, player.playbackEpoch());
    host.play(139);
    const int pauseCount = player.pauses.load();
    oldPause(accepted);
    require(player.pauses.load() == pauseCount, "stale control cannot pause newer title");
    auto oldStop = host.companion.onStop_;
    const auto staleStop = host.companion.clearMedia();
    prepare(139);
    oldStop(staleStop);
    {
        std::lock_guard<std::mutex> lock(reportMutex);
        require(reports.empty(), "stale Stop cannot retire newer PMS session");
    }
    reporter.endSession(1200, 16036);
    host.get("/player/playback/stop");
    {
        std::lock_guard<std::mutex> lock(reportMutex);
        require(reports.size() == 1, "already-ended outgoing PMS session is not duplicated");
    }

    prepare();
    host.companion.setSkipPrevious([&](const misterplex::TransportRequest& request) {
        host.companion.seekTo(0, [&] { return generations.current(request); });
    });
    const auto oldQueue = host.current();
    host.get("/player/playback/skipPrevious");
    require(!oldQueue(), "restart entry invalidates old EOF ownership");
    drain();
    require(starts.size() == 1 && starts[0].offsetMs == 0, "accepted restart retains title");

    prepare();
    misterplex::PlayRequest local;
    local.key = "local-fixture";
    host.companion.stagePlay(local);
    host.companion.setState("playing", 5000, 16036);
    host.get("/player/playback/seekTo?offset=6000");
    drain();
    require(starts.size() == 1 && starts[0].key == "local-fixture" &&
            starts[0].offsetMs == 6000,
            "unstarted non-library identity uses its captured request");
#endif
    std::cout << "test_companion_control_wiring: OK (real control wiring/PMS "
                 "reporter/HTTP; player double)\n";
    return 0;
}
#endif

#if !defined(MPX_LEGACY_EOF) && !defined(MPX_LEGACY_CONTROL)
std::string encodeFixtureQuery(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            encoded += static_cast<char>(c);
        else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    return encoded;
}

int runPendingDirectSeekTests(const std::string& selected) {
    const std::vector<std::string> cases = {
        "pending", "empty", "same-url", "before-play", "startup", "not-started",
        "active", "paused", "same-position", "superseded"};
    for (const auto& scenario : cases) {
        if (!selected.empty() && selected != scenario)
            continue;
        HostCompanion host;
        PlayerControlDouble player{host.epoch};
        std::atomic<uint64_t> activePlay{0};
        player.reportingGeneration = [&] { return activePlay.load(); };
        std::mutex handoff, seekMutex, pendingMutex;
        std::vector<std::function<void()>> pending;
        std::vector<misterplex::PlayRequest> starts;
        misterplex::PmsTimelineReporter reporter(
            [](const auto&) { return misterplex::PmsTimelineSinkResult{true, 200}; }, false);
        misterplex::TransportGenerations generations{host.generation, host.seekGeneration};
        auto post = [&](auto command) {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pending.push_back(std::move(command));
        };
        auto drain = [&] {
            std::vector<std::function<void()>> commands;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                commands.swap(pending);
            }
            for (auto& command : commands)
                command();
        };
        auto headersFor = [](const misterplex::PlayRequest& request) {
            return "Fixture-Session: " + request.token;
        };
        auto start = [&](const misterplex::PlayRequest& request) {
            require(handoff.try_lock(), "captured start must run outside the handoff lock");
            std::unique_lock<std::mutex> lock(handoff, std::adopt_lock);
            if (request.dispatchGeneration != host.generation.load())
                return;
            starts.push_back(request);
            activePlay.store(request.dispatchGeneration);
            player.install(request.dispatchGeneration, request.key, headersFor(request),
                           request.offsetMs);
        };
        misterplex::wirePlaybackControls(host.companion, player, reporter, generations,
            activePlay, handoff, seekMutex, start, [](uint64_t) {}, post);
        auto stage = [&](const std::string& url, const std::string& session) {
            host.get("/player/playback/playMedia?key=" + encodeFixtureQuery(url) +
                     "&token=" + session + "&offset=0");
            misterplex::PlayRequest request;
            request.key = url;
            request.token = session;
            request.dispatchGeneration = host.generation.load();
            request.offsetPresent = true;
            require(host.companion.bindMedia(request, 16036), "bind direct fixture identity");
            return request;
        };
        const std::string aUrl = "http://media.invalid/A.ts";
        const std::string bUrl = scenario == "same-url" ? aUrl : "http://media.invalid/B.ts";
        if (scenario != "empty") {
            const auto a = stage(aUrl, "session-A");
            player.install(a.dispatchGeneration, a.key, headersFor(a), 5000);
            activePlay.store(a.dispatchGeneration);
            host.playing(5000);
        }
        if (scenario == "active" || scenario == "paused" || scenario == "same-position") {
            if (scenario == "paused")
                host.companion.setState("paused", 5000, 16036);
            if (scenario == "same-position")
                player.position.store(6000); // Companion progress can lag the player.
            const auto oldEpoch = player.playbackEpoch();
            host.get("/player/playback/seekTo?offset=6000");
            drain();
            const auto actual = player.media();
            require(starts.empty() && player.seeks.load() == 1 &&
                    actual.url == aUrl && actual.headers == "Fixture-Session: session-A" &&
                    actual.binding.generation == host.generation.load() &&
                    activePlay.load() == host.generation.load() &&
                    player.position.load() == 6000,
                    "matched started local seek retains actual URL, headers and session ownership");
            if (scenario == "same-position") {
                require(player.playbackEpoch() == oldEpoch, "same-position seek keeps its epoch");
                host.get("/player/playback/seekTo?offset=7000");
                drain();
                require(starts.empty() && player.seeks.load() == 2,
                        "same-position generation rebind remains eligible for local seek");
            }
            continue;
        }

        const auto b = stage(bUrl, "session-B"); // B is accepted/bound, but not installed.
        if (scenario == "before-play" || scenario == "startup") {
            player.loading();
            activePlay.store(b.dispatchGeneration); // Exact main-before-player.play window.
        } else if (scenario == "not-started") {
            player.loading(b.key, headersFor(b)); // URL assigned, worker not yet launched.
            activePlay.store(b.dispatchGeneration);
        }
        if (scenario == "startup") {
            std::lock_guard<std::mutex> lock(handoff);
            host.get("/player/playback/seekTo?offset=6000");
            // B finishes starting after the seek captured the previous player epoch.
            player.install(b.dispatchGeneration, b.key, headersFor(b), 0);
        } else {
            host.get("/player/playback/seekTo?offset=6000");
        }
        if (scenario == "superseded") {
            host.get("/player/playback/seekTo?offset=8000");
            const auto c = stage("http://media.invalid/C.ts", "session-C");
            drain();
            require(starts.empty() && player.seeks.load() == 0,
                    "stale non-library seeks cannot act on a newer cast");
            host.get("/player/playback/seekTo?offset=7000");
            host.get("/player/playback/seekTo?offset=9000");
            drain();
            const auto actual = player.media();
            require(starts.size() == 1 && starts[0].key == c.key &&
                    starts[0].offsetMs == 9000 && actual.url == c.key &&
                    actual.headers == headersFor(c) &&
                    actual.binding.generation == host.generation.load(),
                    "latest non-library seek starts its captured URL/header/session only");
            continue;
        }
        require(b.dispatchGeneration != host.generation.load(),
                "accepted seek supersedes pending doPlay(B)");
        drain();
        const auto actual = player.media();
        require(starts.size() == 1 && player.seeks.load() == 0 &&
                starts[0].key == b.key && starts[0].offsetMs == 6000 &&
                actual.url == b.key && actual.headers == headersFor(b) &&
                actual.binding.generation == host.generation.load() &&
                player.position.load() == 6000,
                "pending direct request must start captured B, not seek current A/empty URL");
    }
    std::cout << "test_pending_direct_seek: OK (production registration/HTTP; URL/header/session double)\n";
    return 0;
}
#endif

#ifndef MPX_LEGACY_EOF
// BUG2-RECAST (glass 10-18 00:24-00:26 CT): queue 148023 ended with AUTO_NEXT=0, the poll stayed
// stopped + key/containerKey/playQueueID in fullScreenVideo, and the next Plex Web cast never sent
// playMedia. The ended frame must decay to the fresh-daemon idle; a new cast must bind normally.
static void runQueueEndIdleTests() {
    setenv("MPX_COMPANION_ENDED_IDLE_MS", "300", 1);
    HostCompanion host;
    host.mirror();
    host.play();
    host.playing();
    auto noNext = host.eof([] { return false; });
    require(static_cast<bool>(noNext), "R1 EOF dispatch");
    noNext();
    std::string p = host.poll();
    require(p.find("state=\"stopped\"") != std::string::npos && p.find("key=\"/library/metadata/146\"") != std::string::npos,
            "R1 ended frame first keeps the final title bound");
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    p = host.poll();
    require(p.find("state=\"stopped\"") != std::string::npos, "R2 idle is stopped");
    require(p.find("location=\"navigation\"") != std::string::npos && p.find("fullScreenVideo") == std::string::npos,
            "R2 idle is navigation");
    require(p.find(" key=") == std::string::npos && p.find("containerKey=") == std::string::npos &&
            p.find("playQueueID=") == std::string::npos && p.find("ratingKey=") == std::string::npos,
            "R2 idle carries no old queue binding");
    require(p.find("time=\"0\"") != std::string::npos, "R2 idle time 0");
    for (const auto* state : {"paused", "playing", "buffering"}) {     // late media-thread progress stays idle
        host.companion.setState(state, 0, 16036);
        require(host.poll().find(" key=") == std::string::npos, "R3 late progress cannot rebind");
    }
    host.mirror(139);                                                    // R5 Web's mirror for the next cast
    p = host.poll();
    require(p.find("state=\"buffering\"") != std::string::npos,
            "R5 mirror after the decayed queue end arms the pre-play hold (Web keeps its new session)");
    host.play(139);                                                      // R4 the next cast binds
    p = host.poll();
    require(p.find("key=\"/library/metadata/139\"") != std::string::npos && p.find("fullScreenVideo") != std::string::npos,
            "R4 new cast after queue end binds the new title");
    host.playing(2000);
    host.expect("playing", 2000);
    unsetenv("MPX_COMPANION_ENDED_IDLE_MS");
    std::cout << "BUG2-RECAST queue-end idle: R1-R5 ok\n";
}
#endif

int main(int argc, char** argv) {
#ifdef MPX_F0_BINDING
    return runPendingDirectSeekTests(argc > 1 ? argv[1] : "");
#else
#ifdef MPX_LEGACY_CONTROL
    return runControlWiringTests(argc > 1 ? argv[1] : "");
#else
    (void)argc;
    (void)argv;
    runNaturalEofTests();
#ifndef MPX_LEGACY_EOF
    runControlWiringTests("");
    runPendingDirectSeekTests("");
    runQueueEndIdleTests();
#endif
    return 0;
#endif
#endif
}
