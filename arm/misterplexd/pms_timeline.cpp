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

// The PMS base for this session: baseUrl, or the IPv4 fallback when baseUrl is an unroutable IPv6 host.
std::string selectBase(const PmsTimelineSession& session) {
    std::string base = normalizePlexBase(session.baseUrl);
    if (base.empty())
        return base;
    if (isUnreachableIpv6Host(base)) {
        std::string v6;
        const std::string alt = normalizePlexBase(session.fallbackBase);
        if (!alt.empty() && !ipv6FromPmsHost(alt, v6))
            base = alt;
    }
    return base;
}

} // namespace

bool buildPmsTranscodeRequest(const PmsTimelineSession& session, const std::string& verb,
                              PmsTimelineHttpRequest& out) {
    out = {};
    if ((verb != "ping" && verb != "stop") || session.token.empty() || session.transcodeSessionId.empty())
        return false;
#ifdef MUT_F4_LOCAL_SKIPS   // test-only mutant: ping/stop only for a remote (https) server
    if (selectBase(session).rfind("https", 0) != 0)
        return false;
#endif
#ifdef MUT_HB_LOCAL_BASE
    const std::string base = "http://127.0.0.1:32400";
#else
    const std::string base = selectBase(session);
#endif
    if (base.empty())
        return false;
    out.url = base + "/video/:/transcode/universal/" + verb +
              "?session=" + urlEncodeQuery(session.transcodeSessionId);
    out.kind = verb;
    // The stream's identity block, from the one place decision/start/FFmpeg take it. The profile name is
    // this play's (the same X-Plex-Client-Profile-Name the start used). Token and session are explicit.
    const std::string block = plexFfmpegHeaders(session.transcodeSessionId, session.token);
    size_t i = 0;
    while (i < block.size()) {
        size_t e = block.find("\r\n", i);
        if (e == std::string::npos)
            e = block.size();
        const std::string line = block.substr(i, e - i);
        i = e + 2;
        const size_t c = line.find(": ");
        if (c == std::string::npos)
            continue;
        const std::string name = line.substr(0, c);
        if (name.rfind("X-Plex-Client-Profile", 0) == 0 || name == "X-Plex-Client-Capabilities")
            continue;
        out.headers.push_back({name, line.substr(c + 2)});
    }
    if (!session.clientProfileName.empty())
        out.headers.push_back({"X-Plex-Client-Profile-Name", session.clientProfileName});
    out.headers.push_back({"Accept", "application/xml"});
#ifdef MUT_HB_PLAYER_IDENTITY
    for (auto& h : out.headers)
        if (h.first == "X-Plex-Client-Identifier")
            h.second = session.clientIdentifier;
#endif
    return true;
}

bool buildPmsTimelineHttpRequest(const PmsTimelineSession& session, const std::string& state,
                                 int64_t timeMs, int64_t durationMs,
                                 PmsTimelineHttpRequest& out, bool continuing) {
    out = {};
    if (session.token.empty() || session.ratingKey.empty() || !validState(state))
        return false;

    const std::string base = selectBase(session);
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
    // hasMDE=1 + playbackTime + server machineIdentifier are what official
    // players send; without them PMS records viewOffset (resume works) but
    // Plex Web's live scrubber (session websocket) stays at 0:00.
    // viewOffset= is the same millisecond clock as time=. GET stays the
    // contract (HTTP 2xx + cast token). PMS session viewOffset follows time=;
    // the query alias is what a client that reads viewOffset= updates from.
#ifdef MUT_RC1036_FREEZE_OFFSET
    const std::string viewOff = "0";
#else
    const std::string viewOff = std::to_string(timeMs);
#endif
    out.url = base + "/:/timeline?ratingKey=" + urlEncodeQuery(session.ratingKey) +
              "&key=" + urlEncodeQuery(key) + "&state=" + urlEncodeQuery(state) +
              "&time=" + urlEncodeQuery(std::to_string(timeMs)) +
              "&viewOffset=" + urlEncodeQuery(viewOff) +
              "&playbackTime=" + urlEncodeQuery(std::to_string(timeMs)) +
              "&duration=" + urlEncodeQuery(std::to_string(durationMs)) +
              "&type=video&identifier=com.plexapp.plugins.library&hasMDE=1";
    if (!session.serverMachineId.empty())
        out.url += "&machineIdentifier=" + urlEncodeQuery(session.serverMachineId);
    if (!session.playQueueItemId.empty())
        out.url += "&playQueueItemID=" + urlEncodeQuery(session.playQueueItemId);
    if (!session.containerKey.empty())
        out.url += "&containerKey=" + urlEncodeQuery(session.containerKey);
    if (session.streamsKnown) {
        const int audio = session.audioStreamId < 0 ? 0 : session.audioStreamId;
        const int sub = session.subtitleStreamId < 0 ? 0 : session.subtitleStreamId;
        out.url += "&audioStreamID=" + urlEncodeQuery(std::to_string(audio)) +
                   "&subtitleStreamID=" + urlEncodeQuery(std::to_string(sub));
    }
    if (continuing && state == "stopped")
        out.url += "&continuing=1";

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
    // The PMS keys a client's session on X-Plex-Session-Identifier; open-source companions send it.
    if (!session.sessionIdentifier.empty())
        out.headers.push_back({"X-Plex-Session-Identifier", session.sessionIdentifier});
    return true;
}

std::string pmsTimelineSessionIdentifier(const std::string& resolvedSessionId,
                                         const std::string& clientIdentifier, uint64_t playGen) {
    if (!resolvedSessionId.empty())
        return resolvedSessionId;
    return "mpx-" + (clientIdentifier.empty() ? std::string("misterplex") : clientIdentifier) + "-" +
           std::to_string(playGen);
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
    std::vector<Pending> pre;   // closes of the play this begin supersedes
    bool ok = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string newId = session.transcodeSessionId;
        // The previous play: a different item/server gets its final stopped timeline; a different transcode
        // session gets its stop now. The SAME session id (a resume/restart reusing it) is left running and
        // cancels a pending stop.
        // Order matters (glass rc-1033 legs, lab PMS): a stopped timeline ENDS the transcode session on the PMS, so
        // a stop sent after it answers 404 (4/4) while a stop sent first answers 200 (8/8). Stop first, then the
        // old item's final stopped timeline.
        if (active_) {
            PmsTimelineHttpRequest stop, stopped;
#ifdef MUT_HB_NO_SUPERSEDE
            if (false) {
#else
            if (!session_.transcodeSessionId.empty() && session_.transcodeSessionId != session.transcodeSessionId) {
#endif
                if (buildPmsTranscodeRequest(session_, "stop", stop))
                    pre.push_back({std::move(stop), "stop"});
            }
            if ((session_.ratingKey != session.ratingKey ||
                 normalizePlexBase(session_.baseUrl) != normalizePlexBase(session.baseUrl)) &&
                buildPmsTimelineHttpRequest(session_, "stopped", lastTimeMs_, lastDurationMs_, stopped))
                pre.push_back({std::move(stopped), "stopped"});
        }
        active_ = true;
        session_ = session;
        lastSentState_.clear();
        lastOkLogState_.clear();
        lastPlayingSent_ = {};
        lastTimeMs_ = timeMs < 0 ? 0 : timeMs;
        lastDurationMs_ = durationMs < 0 ? 0 : durationMs;
        lastTimelineSent_ = now;
        lastPing_ = now;
        ok = buildPmsTimelineHttpRequest(session_, "buffering", timeMs, durationMs, req);
        if (!ok) {
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
        } else {
            lastSentState_ = "buffering";
        }
    }
    for (auto& p : pre)
        enqueue(std::move(p.request), p.state);
    if (ok)
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
               now - lastPlayingSent_ >= kPlayingCadence - kPlayingJitterSlack;
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
        if (!active_)
            return;
#ifndef MUT_F3_REPORTER_ZERO
        // F3: a 0/0 report mid-play (a buffering wind-down) is "unknown", not "the play is at 0 of 0": keep the
        // last known position and the duration, which does not change within a play.
        if (validState(state) && durationMs <= 0 && lastDurationMs_ > 0) {
            if (timeMs <= 0 && lastTimeMs_ > 0)
                timeMs = lastTimeMs_;
            durationMs = lastDurationMs_;
        }
#endif
        if (validState(state)) {
            lastTimeMs_ = timeMs < 0 ? 0 : timeMs;
            lastDurationMs_ = durationMs < 0 ? 0 : durationMs;
        }
        if (!shouldSendLocked(state))
            return;
        if (!buildPmsTimelineHttpRequest(session_, state, timeMs, durationMs, req))
            return;
        lastSentState_ = state;
        lastTimelineSent_ = std::chrono::steady_clock::now();
        if (state == "playing")
            lastPlayingSent_ = lastTimelineSent_;
    }
    enqueue(std::move(req), state);
}

void PmsTimelineReporter::endSession(int64_t timeMs, int64_t durationMs, bool continuing) {
    PmsTimelineHttpRequest req, stop;
    bool haveStop = false;
    bool haveTimeline = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!active_)
            return;
        haveTimeline = buildPmsTimelineHttpRequest(session_, "stopped", timeMs, durationMs, req, continuing);
        // The transcode stop goes BEFORE the final stopped timeline: the stopped timeline ends the PMS transcode
        // session, so a stop after it is a 404 (glass legs rc-1033: stop-first 200 x8, stopped-first 404 x4).
        // Both are best effort; a failure of either is logged and never fatal.
#ifndef MUT_HB_NO_STOP
        if (!session_.transcodeSessionId.empty())
            haveStop = buildPmsTranscodeRequest(session_, "stop", stop);
#endif
        active_ = false;
        if (haveTimeline)
            lastSentState_ = "stopped";
    }
#ifdef MUT_HB_STOP_AFTER_TIMELINE
    if (haveTimeline)
        enqueue(std::move(req), "stopped");
    if (haveStop)
        enqueue(std::move(stop), "stop");
#else
    if (haveStop)
        enqueue(std::move(stop), "stop");
    if (haveTimeline)
        enqueue(std::move(req), "stopped");
#endif
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
            auto it = std::find_if(queue_.begin(), queue_.end(), [](const Pending& p) {
                return p.state != "stopped" && p.state != "stop";
            });
            if (it != queue_.end())
                queue_.erase(it);
            else
                queue_.pop_front();
        }
        queue_.push_back(std::move(pending));
    }
    cv_.notify_one();
}

std::string PmsTimelineReporter::activeBaseUrl() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_)
        return {};
#ifdef MUT_355_TL_GATE_BASEURL
    return session_.baseUrl;   // 66f1b89: gated the ULA, sent to the conf fallback
#else
    return selectBase(session_);
#endif
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

bool PmsTimelineReporter::firstAuthFailure(const std::string& kind, int status, const std::string& url) {
    if (status != 401 && status != 403)
        return true;   // not an auth failure: the caller's normal logging applies
#ifdef MUT_F1_AUTH_EVERY_TIME
    return true;
#else
    const size_t at = url.find("://");
    const size_t from = at == std::string::npos ? 0 : at + 3;
    const std::string host = url.substr(from, url.find('/', from) - from);
    const std::string key = kind + "|" + std::to_string(status) + "|" + host;
    std::lock_guard<std::mutex> lock(authLogMu_);
    for (const auto& k : authFailLogged_)
        if (k == key)
            return false;
    if (authFailLogged_.size() < 32)
        authFailLogged_.push_back(key);
    return true;
#endif
}

void PmsTimelineReporter::logTranscodeResult(const Pending& pending, const PmsTimelineSinkResult& result) {
    if (!log_)
        return;
    const size_t slot = pending.state == "stop" ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    if (result.ok) {
        // A ping repeats every 10 s: log the first ok (and the first ok after a failure), never each one.
        if (pending.state == "ping" && pingOkLogged_)
            return;
        if (pending.state == "ping")
            pingOkLogged_ = true;
        line = "pms transcode " + pending.state + ": ok http=" + std::to_string(result.httpStatus);
    } else {
        pingOkLogged_ = false;
#ifndef MUT_F2_STOP_404_FAILED
        // F2: PMS answers 404/410 to a stop for a session it no longer has. It tears the transcode down on the
        // final stopped timeline, which precedes the stop by the 3 s grace. The stop was still sent.
        if (pending.state == "stop" && (result.httpStatus == 404 || result.httpStatus == 410)) {
            log_(redactSensitive("pms transcode stop: session already ended http=" +
                                 std::to_string(result.httpStatus) +
                                 " (the server had already torn it down; nothing left to stop) url=" +
                                 pending.request.url));
            return;
        }
#endif
        if (!firstAuthFailure(pending.state, result.httpStatus, pending.request.url))
            return;
#ifdef MUT_HB_NO_RATELIMIT
        if (false &&
#else
        if (lastFailLog_[slot] != std::chrono::steady_clock::time_point{} &&
#endif

            now - lastFailLog_[slot] < std::chrono::seconds(15)) {
            ++suppressedFails_[slot];
            return;
        }
        lastFailLog_[slot] = now;
        line = "pms transcode " + pending.state + ": failed http=" + std::to_string(result.httpStatus) +
               (pending.state == "stop" ? " (non-fatal, the stop is not retried"
                                                : " (non-fatal, retried on the next cadence");
        if (result.httpStatus == 401 || result.httpStatus == 403)
            line += "; further " + std::to_string(result.httpStatus) + " from this host are not logged";
        if (suppressedFails_[slot])
            line += "; " + std::to_string(suppressedFails_[slot]) + " similar suppressed";
        line += ")";
        suppressedFails_[slot] = 0;
    }
    log_(redactSensitive(line + " url=" + pending.request.url));
}

void PmsTimelineReporter::noteStreams(int audioStreamId, int subtitleStreamId) {
    PmsTimelineHttpRequest req;
    std::string state;
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_.audioStreamId = audioStreamId < 0 ? 0 : audioStreamId;
        session_.subtitleStreamId = subtitleStreamId < 0 ? 0 : subtitleStreamId;
        session_.streamsKnown = true;
        if (!active_)
            return;
        state = lastSentState_.empty() ? "playing" : lastSentState_;
        if (state == "ended")
            state = "stopped";
        if (!buildPmsTimelineHttpRequest(session_, state, lastTimeMs_, lastDurationMs_, req))
            return;
    }
    enqueue(std::move(req), state);
}

bool PmsTimelineReporter::send(const Pending& pending) {
    PmsTimelineSinkResult result;
    try {
        result = sink_(pending.request);
    } catch (...) {
        result = {};
    }
    if (pending.request.kind == "ping" || pending.request.kind == "stop") {
        logTranscodeResult(pending, result);
        return result.ok;
    }
    if (log_) {
        // Failures always. A successful update is logged only when the state changes:
        // the 1 Hz playing cadence would otherwise write ~3600 lines/hour onto the SD card.
        bool logThis = !result.ok && firstAuthFailure("timeline", result.httpStatus, pending.request.url);
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

std::chrono::steady_clock::time_point PmsTimelineReporter::nextDueLocked() const {
    auto due = std::chrono::steady_clock::time_point::max();
    if (active_) {
        if (!session_.transcodeSessionId.empty())
            due = std::min(due, lastPing_ + kKeepaliveCadence);
        if (!lastSentState_.empty() && lastSentState_ != "stopped")
            due = std::min(due, lastTimelineSent_ + kKeepaliveCadence);
    }
    return due;
}

void PmsTimelineReporter::keepaliveTick(std::chrono::steady_clock::time_point now) {
    std::vector<Pending> out;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (active_) {
            if (!session_.transcodeSessionId.empty() && now - lastPing_ >= kKeepaliveCadence) {
                PmsTimelineHttpRequest ping;
#ifdef MUT_HB_NO_PING
                if (false)
#else
                if (buildPmsTranscodeRequest(session_, "ping", ping))
#endif
                    out.push_back({std::move(ping), "ping"});
                lastPing_ = now;
            }
#ifdef MUT_HB_NO_STATE_RESEND
            if (false) {
#else
            if (!lastSentState_.empty() && lastSentState_ != "stopped" &&
                now - lastTimelineSent_ >= kKeepaliveCadence) {
#endif
                PmsTimelineHttpRequest tl;
                if (buildPmsTimelineHttpRequest(session_, lastSentState_, lastTimeMs_, lastDurationMs_, tl))
                    out.push_back({std::move(tl), lastSentState_});
                lastTimelineSent_ = now;
            }
        }
    }
    for (auto& p : out)
        enqueue(std::move(p.request), p.state);
}

void PmsTimelineReporter::workerLoop() {
    for (;;) {
        Pending pending;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(mu_);
            const auto ready = [this] { return stopping_ || !queue_.empty(); };
            const auto due = nextDueLocked();
            if (due == std::chrono::steady_clock::time_point::max())
                cv_.wait(lock, ready);
            else
                cv_.wait_until(lock, due, ready);
            if (!queue_.empty()) {
                pending = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            } else if (stopping_) {
                break;
            }
        }
        if (have)
            send(pending);
        else
            keepaliveTick(std::chrono::steady_clock::now());
    }
}

void PmsTimelineReporter::stopAndFlush() {
    // Daemon exit: close an active play (final stopped timeline) and stop its transcode now, no grace.
    int64_t t = 0, d = 0;
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        wasActive = active_;
        t = lastTimeMs_;
        d = lastDurationMs_;
    }
#ifndef MUT_HB_NO_FLUSH_STOP
    if (wasActive)
        endSession(t, d);
#endif
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
