#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace misterplex {

struct PmsTimelineSession {
    std::string baseUrl;
    std::string token;
    std::string key;
    std::string ratingKey;
    std::string playQueueItemId;
    std::string containerKey;
    std::string clientIdentifier = "misterplex";
    std::string product = "Plex Web";
    std::string version = "4.125.0";
    std::string deviceName = "Chrome";
    // X-Plex-Client-Profile-Name on /:/timeline (and the transcode ping): the fixed name of the
    // loaded core's family (MiSTerPlex / MiSTerPlex-High), never a variant.
    std::string clientProfileName = "MiSTerPlex";
};

struct PmsTimelineHttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Sink result: ok must reflect real HTTP 2xx (not body non-empty).
struct PmsTimelineSinkResult {
    bool ok = false;
    int httpStatus = 0; // 0 = transport/unknown; else HTTP status
};

bool buildPmsTimelineHttpRequest(const PmsTimelineSession& session, const std::string& state,
                                 int64_t timeMs, int64_t durationMs,
                                 PmsTimelineHttpRequest& out);

class PmsTimelineReporter {
public:
    using HttpSink = std::function<PmsTimelineSinkResult(const PmsTimelineHttpRequest&)>;
    using LogFn = std::function<void(const std::string&)>;

    // 1s cadence so Plex Web scrubber (PMS-bound) tracks companion progress.
    static constexpr std::chrono::seconds kPlayingCadence{1};

    explicit PmsTimelineReporter(HttpSink sink = {}, bool async = true);
    ~PmsTimelineReporter();

    PmsTimelineReporter(const PmsTimelineReporter&) = delete;
    PmsTimelineReporter& operator=(const PmsTimelineReporter&) = delete;

    void setLog(LogFn log) { log_ = std::move(log); }

    void beginSession(const PmsTimelineSession& session, int64_t timeMs, int64_t durationMs);
    void reportState(const std::string& state, int64_t timeMs, int64_t durationMs);
    void endSession(int64_t timeMs, int64_t durationMs);
    // Refresh auth for the active session (cast may send a newer token on later
    // playMedia/seek/poll). No-op when inactive or token empty.
    void updateToken(const std::string& token);
    void stopAndFlush();

private:
    struct Pending {
        PmsTimelineHttpRequest request;
        std::string state;
    };

    bool shouldSendLocked(const std::string& state);
    void enqueue(PmsTimelineHttpRequest request, const std::string& state);
    void workerLoop();
    bool send(const Pending& pending);

    HttpSink sink_;
    bool async_ = true;
    LogFn log_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Pending> queue_;
    bool stopping_ = false;
    bool workerStarted_ = false;
    std::thread worker_;

    bool active_ = false;
    PmsTimelineSession session_;
    std::string lastSentState_;
    std::string lastOkLogState_;
    std::chrono::steady_clock::time_point lastPlayingSent_{};
};

} // namespace misterplex
