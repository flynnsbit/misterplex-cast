#include <pthread.h>
#include "pms_timeline.hpp"

#include "log_redact.hpp"
#include "plex_resolve.hpp"

#include <algorithm>
#include <cstdio>

namespace misterplex {
namespace {

constexpr size_t kMaxQueue = 4;

bool validState(const std::string& state) {
    return state == "playing" || state == "paused" || state == "stopped" ||
           state == "buffering";
}

PmsTimelineSinkResult defaultSink(const PmsTimelineHttpRequest& req) {
    // GET is the Plex client convention for /:/timeline (query carries state/time).
    const auto r = plexHttpGetNoBodyResult(req.url, req.headers, 4);
    return PmsTimelineSinkResult{r.ok, r.httpStatus};
}

} // namespace

bool buildPmsTimelineHttpRequest(const PmsTimelineSession& session, const std::string& state,
                                 int64_t timeMs, int64_t durationMs,
                                 PmsTimelineHttpRequest& out) {
    out = {};
    if (session.token.empty() || session.ratingKey.empty() || !validState(state))
        return false;

    std::string base = normalizePlexBase(session.baseUrl);
    if (base.empty())
        return false;

    if (timeMs < 0)
        timeMs = 0;
    if (durationMs < 0)
        durationMs = 0;
    if (durationMs > 0 && timeMs > durationMs)
        timeMs = durationMs;

    std::string key = session.key;
    if (key.empty())
        key = "/library/metadata/" + session.ratingKey;

    // type=video matches Plex Web cast playMedia and common player clients.
    out.url = base + "/:/timeline?ratingKey=" + urlEncodeQuery(session.ratingKey) +
              "&key=" + urlEncodeQuery(key) + "&state=" + urlEncodeQuery(state) +
              "&time=" + urlEncodeQuery(std::to_string(timeMs)) +
              "&duration=" + urlEncodeQuery(std::to_string(durationMs)) +
              "&type=video&identifier=com.plexapp.plugins.library";
    if (!session.playQueueItemId.empty())
        out.url += "&playQueueItemID=" + urlEncodeQuery(session.playQueueItemId);
    if (!session.containerKey.empty())
        out.url += "&containerKey=" + urlEncodeQuery(session.containerKey);

    out.headers = {
        {"X-Plex-Token", session.token},
        {"X-Plex-Client-Identifier", session.clientIdentifier},
        {"X-Plex-Product", session.product},
        {"X-Plex-Version", session.version},
        {"X-Plex-Device-Name", session.deviceName},
        {"X-Plex-Device", "Linux"},
        {"X-Plex-Platform", "Linux"},
        {"X-Plex-Provides", "player"},
        {"Accept", "application/xml"},
    };
    if (!session.clientProfileName.empty())
        out.headers.push_back({"X-Plex-Client-Profile-Name", session.clientProfileName});
    return true;
}

PmsTimelineReporter::PmsTimelineReporter(HttpSink sink, bool async)
    : sink_(std::move(sink)), async_(async) {
    if (!sink_)
        sink_ = [](const PmsTimelineHttpRequest& req) { return defaultSink(req); };
    if (async_) {
        workerStarted_ = true;
        worker_ = std::thread([this] {
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "mpx-timeline");
#endif
            workerLoop();
        });
    }
}

PmsTimelineReporter::~PmsTimelineReporter() { stopAndFlush(); }

void PmsTimelineReporter::beginSession(const PmsTimelineSession& session, int64_t timeMs,
                                       int64_t durationMs) {
    PmsTimelineHttpRequest req;
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_ = true;
        session_ = session;
        lastSentState_.clear();
        lastOkLogState_.clear();
        lastPlayingSent_ = {};
        if (!buildPmsTimelineHttpRequest(session_, "buffering", timeMs, durationMs, req)) {
            active_ = false;
            if (log_) {
                const char* why = "unknown";
                if (session.token.empty())
                    why = "empty token";
                else if (session.ratingKey.empty())
                    why = "empty ratingKey";
                else if (normalizePlexBase(session.baseUrl).empty())
                    why = "empty/invalid baseUrl";
                log_(redactSensitive(std::string("pms timeline: beginSession skipped (") + why +
                                     ") base=" + session.baseUrl +
                                     " ratingKey=" + session.ratingKey));
            }
            return;
        }
        lastSentState_ = "buffering";
        lastPlayingSent_ = {};
    }
    enqueue(std::move(req), "buffering");
}

bool PmsTimelineReporter::shouldSendLocked(const std::string& state) {
    if (state == "stopped")
        return true;
    if (state != lastSentState_)
        return true;
    if (state == "playing") {
        auto now = std::chrono::steady_clock::now();
        return lastPlayingSent_ == std::chrono::steady_clock::time_point{} ||
               now - lastPlayingSent_ >= kPlayingCadence;
    }
    return false;
}

void PmsTimelineReporter::reportState(const std::string& state, int64_t timeMs,
                                      int64_t durationMs) {
    if (state == "ended") {
        endSession(timeMs, durationMs);
        return;
    }
    PmsTimelineHttpRequest req;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!active_ || !shouldSendLocked(state))
            return;
        if (!buildPmsTimelineHttpRequest(session_, state, timeMs, durationMs, req))
            return;
        lastSentState_ = state;
        if (state == "playing")
            lastPlayingSent_ = std::chrono::steady_clock::now();
    }
    enqueue(std::move(req), state);
}

void PmsTimelineReporter::endSession(int64_t timeMs, int64_t durationMs) {
    PmsTimelineHttpRequest req;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!active_)
            return;
        if (!buildPmsTimelineHttpRequest(session_, "stopped", timeMs, durationMs, req)) {
            active_ = false;
            return;
        }
        lastSentState_ = "stopped";
        active_ = false;
    }
    enqueue(std::move(req), "stopped");
}

void PmsTimelineReporter::enqueue(PmsTimelineHttpRequest request, const std::string& state) {
    Pending pending{std::move(request), state};
    if (!async_) {
        send(pending);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.size() >= kMaxQueue) {
            auto it = std::find_if(queue_.begin(), queue_.end(),
                                   [](const Pending& p) { return p.state != "stopped"; });
            if (it != queue_.end())
                queue_.erase(it);
            else
                queue_.pop_front();
        }
        queue_.push_back(std::move(pending));
    }
    cv_.notify_one();
}

void PmsTimelineReporter::updateToken(const std::string& token) {
    if (token.empty())
        return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_)
        return;
    if (session_.token == token)
        return;
    session_.token = token;
    if (log_)
        log_("pms timeline: session token refreshed");
}

bool PmsTimelineReporter::send(const Pending& pending) {
    PmsTimelineSinkResult result;
    try {
        result = sink_(pending.request);
    } catch (...) {
        result = {};
    }
    if (log_) {
        // Failures always. A successful update is logged only when the state changes:
        // the 1 Hz playing cadence would otherwise write ~3600 lines/hour onto the SD card.
        bool logThis = !result.ok;
        if (result.ok && pending.state != lastOkLogState_) {
            lastOkLogState_ = pending.state;
            logThis = true;
        }
        if (logThis) {
            log_(redactSensitive(std::string("pms timeline: update ") +
                                 (result.ok ? "ok" : "failed") +
                                 " http=" + std::to_string(result.httpStatus) +
                                 " state=" + pending.state + " url=" + pending.request.url));
        }
    }
    return result.ok;
}

void PmsTimelineReporter::workerLoop() {
    for (;;) {
        Pending pending;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_)
                    break;
                continue;
            }
            pending = std::move(queue_.front());
            queue_.pop_front();
        }
        send(pending);
    }
}

void PmsTimelineReporter::stopAndFlush() {
    if (!async_)
        return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!workerStarted_)
            return;
        stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable())
        worker_.join();
    workerStarted_ = false;
}

} // namespace misterplex
