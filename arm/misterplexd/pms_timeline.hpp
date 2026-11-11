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

#include "libmisterplex/plex_web_client.hpp"

namespace misterplex {

struct PmsTimelineSession {
    std::string baseUrl;
    // IPv4/LAN base for this PMS (the conf PLEX_BASE). Used instead of baseUrl when baseUrl is an IPv6
    // ULA/link-local host the MiSTer cannot route to. Empty: baseUrl is posted as is.
    std::string fallbackBase;
    std::string token;
    std::string key;
    std::string ratingKey;
    std::string playQueueItemId;
    std::string containerKey;
    // PMS machineIdentifier of the playing server (not the player). Official
    // clients send this on /:/timeline so Web can bind the session.
    std::string serverMachineId;
    std::string clientIdentifier = "misterplex";
    // X-Plex-Session-Identifier on /:/timeline (empty = header omitted). Same id as the transcode session.
    std::string sessionIdentifier;
    std::string product = "Plex Web";
    std::string version = kPlexWebVersion;
    std::string deviceName = "Chrome";
    // X-Plex-Client-Profile-Name on /:/timeline (and the transcode ping): the fixed name of the
    // loaded core's family (MiSTerPlex / MiSTerPlex-High), never a variant.
    std::string clientProfileName = "MiSTerPlex";
    // The PMS transcode session id (session= on /video/:/transcode/universal/start). Empty for a direct play: no
    // ping and no stop are sent. When set, the reporter pings the session every kKeepaliveCadence and stops it
    // after the final stopped timeline (PMS kills an un-pinged transcode after ~180 s idle).
    std::string transcodeSessionId;
    // Active part stream ids. plex-mpv-shim / OpenPHT send the same
    // audioStreamID/subtitleStreamID on /:/timeline as on companion poll.
    // Omitted until the player names them (0 = subtitles off).
    bool streamsKnown = false;
    int audioStreamId = 0;
    int subtitleStreamId = 0;
};

struct PmsTimelineHttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string kind = "timeline";   // "timeline" | "ping" | "stop" (sinks and tests tell them apart)
};

// Sink result: ok must reflect real HTTP 2xx (not body non-empty).
struct PmsTimelineSinkResult {
    bool ok = false;
    int httpStatus = 0; // 0 = transport/unknown; else HTTP status
};

// The session id for /:/timeline: the resolve's transcode session when it had one, else a stable
// per-play id (direct play) built from the client identifier and the play generation. Never a token.
std::string pmsTimelineSessionIdentifier(const std::string& resolvedSessionId,
                                         const std::string& clientIdentifier, uint64_t playGen);

bool buildPmsTimelineHttpRequest(const PmsTimelineSession& session, const std::string& state,
                                 int64_t timeMs, int64_t durationMs,
                                 PmsTimelineHttpRequest& out, bool continuing = false);

// GET {base}/video/:/transcode/universal/{ping|stop}?session=<id>. verb is "ping" or "stop". Same server (base,
// IPv6 fallback) and token as the timeline; the identity headers are the STREAM's (what decision/start/FFmpeg
// send: misterplex / Plex Web), not the Companion player's, because PMS owns the transcode under that client.
// False when the token, transcodeSessionId or base is empty/invalid, or the verb is not ping/stop.
bool buildPmsTranscodeRequest(const PmsTimelineSession& session, const std::string& verb,
                              PmsTimelineHttpRequest& out);

class PmsTimelineReporter {
public:
    using HttpSink = std::function<PmsTimelineSinkResult(const PmsTimelineHttpRequest&)>;
    using LogFn = std::function<void(const std::string&)>;

    // 1s cadence so Plex Web scrubber (PMS-bound) tracks companion progress.
    static constexpr std::chrono::seconds kPlayingCadence{1};
    // The progress tick is nominally 1 Hz but jitters; a tick landing under 1.000 s after the last send used to
    // be skipped, doubling the effective cadence to ~2 s. A tick this much early still counts as due.
    static constexpr std::chrono::milliseconds kPlayingJitterSlack{150};

    // PMS wants a timeline "in a regular fashion (generally every 10 seconds)" in every state, and a
    // transcode ping well inside its 180 s idle kill. Both ride this cadence.
    static constexpr std::chrono::seconds kKeepaliveCadence{10};
    // rc-1034: the transcode stop is sent BEFORE the final stopped timeline (no grace window): the stopped
    // timeline ends the PMS session, so a later stop is a 404.

    explicit PmsTimelineReporter(HttpSink sink = {}, bool async = true);
    ~PmsTimelineReporter();

    PmsTimelineReporter(const PmsTimelineReporter&) = delete;
    PmsTimelineReporter& operator=(const PmsTimelineReporter&) = delete;

    void setLog(LogFn log) { log_ = std::move(log); }

    void beginSession(const PmsTimelineSession& session, int64_t timeMs, int64_t durationMs);
    void reportState(const std::string& state, int64_t timeMs, int64_t durationMs);
    void endSession(int64_t timeMs, int64_t durationMs, bool continuing = false);
    // Refresh auth for the active session (cast may send a newer token on later
    // playMedia/seek/poll). No-op when inactive or token empty.
    void updateToken(const std::string& token);
    // #355: the base the active session's requests actually go to (selectBase: baseUrl, or the IPv4
    // fallback for an unroutable IPv6 baseUrl); "" when inactive. Gate a new token against THIS host.
    std::string activeBaseUrl();
    // Player opened or setStreams changed the active part's streams. Updates
    // the session and immediately reports so PMS /status/sessions has the ids
    // before Plex Web opens the in-play stream menu.
    void noteStreams(int audioStreamId, int subtitleStreamId);
    void stopAndFlush();
    // The keepalive step. The worker calls it when something is due; tests call it with a synthetic clock.
    // Sends: the pending transcode stop once its grace has passed; a transcode ping when kKeepaliveCadence has
    // passed since the last one; the last timeline state again (same time/duration) when kKeepaliveCadence has
    // passed since the last timeline of ANY state (paused/buffering used to be sent once, then silence).
    void keepaliveTick(std::chrono::steady_clock::time_point now);

private:
    struct Pending {
        PmsTimelineHttpRequest request;
        std::string state;
    };

    bool shouldSendLocked(const std::string& state);
    void enqueue(PmsTimelineHttpRequest request, const std::string& state);
    void workerLoop();
    bool send(const Pending& pending);
    std::chrono::steady_clock::time_point nextDueLocked() const;
    void logTranscodeResult(const Pending& pending, const PmsTimelineSinkResult& result);

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

    // Keepalive state (under mu_).
    int64_t lastTimeMs_ = 0;
    int64_t lastDurationMs_ = 0;
    std::chrono::steady_clock::time_point lastTimelineSent_{};
    std::chrono::steady_clock::time_point lastPing_{};
    // Transcode ping/stop log policy: the first ok per kind, every state change, and failures at most
    // once per 15 s per kind (with the suppressed count).
    bool pingOkLogged_ = false;
    // fr-glass-1032-1025: a 401/403 from one server repeats for every request; log it once per (kind,status,host).
    std::mutex authLogMu_;
    std::vector<std::string> authFailLogged_;
    bool firstAuthFailure(const std::string& kind, int status, const std::string& url);
    std::chrono::steady_clock::time_point lastFailLog_[2]{};
    unsigned suppressedFails_[2] = {0, 0};
};

} // namespace misterplex
