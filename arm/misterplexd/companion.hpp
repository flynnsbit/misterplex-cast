#pragma once
// Plex GDM + companion HTTP for MiSTerPlex Phase 2/4.
// Lessons from mistercast-linux: prePlayHold, castBound, play-queue bind,
// async playMedia ACK, viewOffset ms, resume-dialog hold after stop.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libmisterplex/ddr_frame_layout.hpp"
#include "log_redact.hpp"

namespace misterplex {

// reject High/non-Baseline SPS (profile_idc != 66) or CABAC PPS (entropy_coding_mode_flag=1); Baseline CAVLC returns false.
bool spsRejectHigh(const std::vector<uint8_t>& nalu);

// Phase 1 (baseline FPGA): true when an Annex-B AU contains High profile_idc 100 or a CABAC PPS.
// Main (77) is not this probe. MPX_HIGH_LIBAV_TRANSCODE=0 turns the restart off; unset means on.
bool annexbNeedsBaselineTranscode(const uint8_t* p, size_t n);
bool highLibavTranscodeEnabled();

// 104: remote PMS still emitted CAVLC after decision extra=cabac=1 and directPlay=0.
// MPX_HIGH_HOST_CABAC=0 keeps the refuse. Unset means the box ffmpeg re-encodes
// that AU to Main CABAC (one ref, no B, no 8x8) before the High core sees it.
bool highHostCabacTranscodeEnabled();
bool baselineHostTranscodeEnabled();
std::vector<std::string> highCabacFfmpegArgs(const std::string& ffmpegPath);
std::vector<std::string> baselineHostFfmpegArgs(const std::string& ffmpegPath);
// Pull complete Annex-B access units out of a growing buffer. An AU ends at the
// next start code after a VCL NAL. Returns how many AUs were moved to out.
size_t extractCompleteAnnexbAus(std::vector<uint8_t>& buf,
                                std::vector<std::vector<uint8_t>>& out);
bool annexbContainsIdr(const uint8_t* p, size_t n);

// Owns one ffmpeg libx264 process. push() feeds CAVLC Annex-B. pop() returns
// one CABAC Annex-B AU when the encoder has finished a frame.
class HighCabacHostTranscode {
public:
    enum class Pop { Got, NeedMore, Failed };
    HighCabacHostTranscode();
    ~HighCabacHostTranscode();
    HighCabacHostTranscode(const HighCabacHostTranscode&) = delete;
    HighCabacHostTranscode& operator=(const HighCabacHostTranscode&) = delete;
    // baselineOut rewrites a High AU to Baseline CAVLC. The default is Main CABAC.
    bool start(const std::string& ffmpegPath, bool baselineOut = false);
    bool active() const;
    bool push(const uint8_t* p, size_t n);
    // Close the encoder's stdin so a short input can finish probing and flush.
    void endInput();
    Pop pop(std::vector<uint8_t>& out, int timeoutMs);
    // Bytes read from the encoder stdout, and the first error lines from stderr.
    size_t bytesOut() const;
    std::string stderrText() const;
    void stop();
private:
    struct Impl;
    Impl* impl_ = nullptr;
};

enum class TitleDirectPolicy { Direct, Proxy };

struct TitleStreamFacts {
    std::string codec;
    std::string profile;
    int level = 0;
    int refFrames = 0;
    int width = 0;
    int height = 0;
    bool interlaced = false;
};

TitleDirectPolicy decideTitleDirectPolicy(const TitleStreamFacts& facts, const char** why = nullptr);
// FPGA-budget direct vs proxy for a raw H.264 stream (Baseline 66, CAVLC, ref1,
// no B, level <= 30). Shared by 240p/480p/480i. level 21 is not the FPGA budget.
inline bool directStreamPolicy(int profile_idc, bool has_cavlc, int max_ref_frames, bool has_bframe,
                               int level) {
    if (profile_idc != 66)
        return false;
    if (!has_cavlc)
        return false;
    if (max_ref_frames != 1)
        return false;
    if (has_bframe)
        return false;
    if (level <= 0 || level > 30)
        return false;
    return true;
}
struct PlayRequest;
PlayRequest parsePlayRequest(const std::string& req);

struct PlayRequest {
    std::string key;
    std::string containerKey;
    std::string playQueueId;
    std::string playQueueItemId;
    std::string playQueueVersion;
    std::string ratingKey;
    TitleDirectPolicy directPolicy = TitleDirectPolicy::Proxy;
    std::string address;
    std::string protocol;
    std::string port;
    std::string token;
    std::string serverMachineId;
    int64_t offsetMs = 0;
    bool offsetPresent = false;
    uint64_t dispatchGeneration = 0;
    // MPX_BROWSE_V2 browse plays of a helper "ladder" row: the ladder input the helper chose
    // (WeakLadder::ladderSrcOverride) or asked for ("server-transcode":
    // WeakLadder::ladderServerTranscode), for the item key ladderSrcFor only. Empty otherwise.
    std::string ladderSrc;
    std::string ladderInput;
    std::string ladderSrcFor;
    // The start ladderSrc was made for, and the helper ticket it came from: the helper's
    // server-transcode relay carries its offset in its path, so a play at another offset (a
    // seek, a restart) re-fetches the ticket there (contract §9.3). -1 / empty otherwise.
    int64_t ladderSrcOffsetMs = -1;
    std::string ladderTicketUrl;
    // SPS-gate layer 3 (internal: never parsed from or written to HTTP, so every cast play starts clean, B6). The
    // gate fallback play of the SAME item: resolve takes the ladder helper even when no conf term selects it
    // (forceLadder), the play is the item's one fallback (fallbackUsed: an OUT is then an error, never a second
    // fallback), and gateDetail is the reason resolve prints after "gate fallback: ". tryQueueStep resets them for
    // a different item; a same-item restart keeps them.
    bool forceLadder = false;
    bool fallbackUsed = false;
    // Set by the seek path (seekStart) only: this play is a seekTo replay of the committed item.
    bool seekReplay = false;
    // NO-REQUIRED-PROFILE: this play is the item's one lowered PMS retry (gate row R6p).
    bool pmsLowered = false;
    std::string gateDetail;
    // Letterbox / crop / active area for 240p, 480p, and 480i. Filled by
    // applyRasterLetterbox from G2 streamRasterPlacementN. crop* are luma samples
    // (0 if unknown). is480i is set only by apply480iLetterbox.
    bool is480i = false;
    bool letterbox = false;
    int active_height = 0;
    int pic_height = 0;
    int crop_height = 0;
    int cropLeft = 0;
    int cropRight = 0;
    int cropTop = 0;
    int cropBottom = 0;
};

// Session-start DIRECT vs PROXY from the playMedia key (not PlayRequest.protocol).
// Matches resolvePlayTarget pass-through: testsrc / URL / local path DIRECT;
// /library and /playQueues PROXY until stream facts exist.
std::string titlePlayPolicy(const PlayRequest& pr);

// Shared G2 letterbox/crop into PlayRequest. rasterLines is 240 or 480.
// codedWidth/codedHeight <= 0 leaves letterbox/active_height at 0.
void applyRasterLetterbox(PlayRequest& pr, int rasterLines, int codedWidth, int codedHeight,
                          int cropLeft = 0, int cropRight = 0, int cropTop = 0, int cropBottom = 0);
void apply240pLetterbox(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft = 0,
                        int cropRight = 0, int cropTop = 0, int cropBottom = 0);
void apply480pLetterbox(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft = 0,
                        int cropRight = 0, int cropTop = 0, int cropBottom = 0);
void apply480iLetterbox(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft = 0,
                        int cropRight = 0, int cropTop = 0, int cropBottom = 0);
// pic_height = codedHeight, crop_height = cropTop+cropBottom, active_height =
// display height capped at rasterCap (240 or 480). Unknown coded size stays 0.
void applyRasterActiveHeight(PlayRequest& pr, int rasterCap, int codedWidth, int codedHeight,
                             int cropLeft = 0, int cropRight = 0, int cropTop = 0,
                             int cropBottom = 0);
void apply240pActiveHeight(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft = 0,
                           int cropRight = 0, int cropTop = 0, int cropBottom = 0);

// Token a cast request may install as the playing server's auth (empty: keep the current one).
std::string castControlRefreshToken(const std::string& req);

enum class TransportCommand { Pause, Resume, Stop, Seek, Previous };

struct TransportRequest {
    uint64_t generation = 0;
    uint64_t originGeneration = 0;
    uint64_t seekGeneration = 0;
    uint64_t epoch = 0;
    int64_t positionMs = 0;
    PlayRequest media;
};

class Companion {
public:
    using LogFn = std::function<void(const std::string&)>;
    using PlayFn = std::function<void(const PlayRequest&)>;
    using PlayQueuedFn = std::function<uint64_t()>;
    using CtrlFn = std::function<void()>;
    using TransportFn = std::function<void(const TransportRequest&)>;
    using TransportQueuedFn = std::function<TransportRequest(TransportCommand)>;
    using StepFn = std::function<void(int64_t deltaMs, const TransportRequest&)>;
    // Guards execute under mu_; they must not acquire locks or re-enter Companion.
    using CurrentFn = std::function<bool()>;

    void setName(std::string n) { name_ = std::move(n); }
    void setMachineId(std::string id) { machineId_ = std::move(id); }
    void setPort(uint16_t p) { port_ = p; }
    void setLog(LogFn f) { log_ = std::move(f); }
    void setPlay(PlayFn f) { onPlay_ = std::move(f); }
    // Fired on the HTTP thread as soon as playMedia plants scrubber state (before
    // the async onPlay_ thread). Its generation follows that exact request into
    // the detached handler so an older thread can never promote itself later.
    void setPlayQueued(PlayQueuedFn f) { onPlayQueued_ = std::move(f); }
    // Cast may present a fresher X-Plex-Token / token= on later player requests
    // (seek, second playMedia, some polls). Forward to PMS timeline session.
    using TokenFn = std::function<void(const std::string& token)>;
    void setTokenUpdate(TokenFn f) { onTokenUpdate_ = std::move(f); }
    // Called under the state lock; the hook may perform only nonblocking atomic work.
    void setTransportQueued(TransportQueuedFn f) { onTransportQueued_ = std::move(f); }
    void setPause(TransportFn f) { onPause_ = std::move(f); }
    void setResume(TransportFn f) { onResume_ = std::move(f); }
    void setStop(TransportFn f) { onStop_ = std::move(f); }
    // When this returns true, HTTP stop is ACKed and does not clear the session.
    void setDeferStop(std::function<bool()> f) { deferStop_ = std::move(f); }
    void armResumeAfterRespawn(int64_t ms) { std::lock_guard<std::mutex> lock(mu_); resumeAfterRespawn_ = ms > 0 ? ms : 0; }
    void setSeek(TransportFn f) { onSeek_ = std::move(f); }
    // OWENB-P1B T5: /player/playback/setParameters?volume=N (0..100). The timeline echoes the volume.
    using VolumeFn = std::function<void(int volume)>;
    void setVolume(VolumeFn f) { onVolume_ = std::move(f); }
    int volume() const { return volume_.load(); }
    // Relative scrubber step (stepForward/stepBack); deltaMs may be negative.
    void setStep(StepFn f) { onStep_ = std::move(f); }
    void setSkipNext(CtrlFn f) { onSkipNext_ = std::move(f); }
    void setSkipPrevious(TransportFn f) { onSkipPrevious_ = std::move(f); }
    using PlexUiNavFn = std::function<void(const std::string& op)>;
    void setPlexUiNavEnabled(bool on) { plexUiNavEnabled_ = on; }
    void setPlexUiNav(PlexUiNavFn f) { onPlexUiNav_ = std::move(f); }
    void setPlexTvEnabled(bool) {}
    void setPlexTvToken(const std::string&) {}
    void setPlexTvLinkPath(const std::string&) {}
    void setPlexTvPersist(std::function<void(const std::string&)>) {}
    // A stop that arrived while resolve was still in flight (179 holds the generation).
    // A play that never reaches playing must not leave the scrubber on fullScreenVideo.
    void noteStopDuringResolve();
    void closedStop(int64_t timeMs, int64_t durationMs, bool terminal = false) {
        setState("stopped", timeMs, durationMs, terminal);
    }

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    bool httpReady() const { return httpReady_.load(); }
    // Best-effort non-loopback IPv4 for FRIENDLY_NAME / cast UI.
    std::string localLanIp() const { return lanIp(); }

    // Update playback clock (ms) + state for timeline polls.
    void setState(const std::string& state, int64_t timeMs, int64_t durationMs,
                  bool terminal = false, const CurrentFn& current = {});

    // Bind media identity for scrubber (call after resolve / on playMedia).
    // Returns false if session already stopped (late async playMedia after stop)
    // or if a newer cast already planted a different pendingKey (stale resolve).
    bool bindMedia(const PlayRequest& req, int64_t durationMs);

    // 480i: 480-line frame / 240-line field. 480p/240p keep their own active heights.
    static constexpr int k480iActiveHeight = 480;
    static constexpr int k480iPicHeight = 240;
    void setActiveHeight(int activeHeight, int picHeight);
    void setRasterMode(const std::string& mode);

    // Plant scrubber bind for a queue step / skipNext without waiting for resolve.
    // Ensures bindMedia key-match accepts the upcoming doPlay for this key.
    bool stagePlay(const PlayRequest& req, const CurrentFn& current = {});

    // Align plant + displayed clock to the demux start about to begin (e.g. PMS
    // viewOffset when cast omitted offset=). Keeps seek-hold semantics: early
    // demux restart behind this target still pins; live time ahead adopts.
    // Port of 0abee0b6 — without this, Web scrubber freezes at 0:00.
    // current (optional) runs under the state lock, as for setState: a seek bumps the play
    // generation under this lock (acceptSeekLocked), so a superseded doPlay cannot overwrite
    // that seek's plant, which the seek's replay reads back as its start.
    void seedPlaybackPosition(int64_t timeMs, int64_t durationMs, const CurrentFn& current = {});

    // Accept Stop and invalidate queued work before clearing the bind.
    // The caller retires the player only after this state lock is released.
    TransportRequest clearMedia();
    bool seekTo(int64_t ms, const CurrentFn& current = {});

    // Pixel bars for companion timeline OSD. Zero = full-frame (omit attrs).
    void setLetterboxBars(int barTop, int barBot, int barLeft = 0, int barRight = 0);

    // True while a playMedia session is live (false after stop/clearMedia).
    bool wantPlay() const {
        std::lock_guard<std::mutex> lock(mu_);
        return wantPlay_;
    }

    bool acceptsPlayRequest(const PlayRequest& req) const {
        std::lock_guard<std::mutex> lock(mu_);
        return wantPlay_ &&
               (pendingKey_.empty() || req.key.empty() || pendingKey_ == req.key);
    }

    // Current scrubber timeline position (ms). Used by doPlay to honor seeks
    // that happen while async resolve is still in flight.
    int64_t timelineTimeMs() const {
        std::lock_guard<std::mutex> lock(mu_);
        return timeMs_;
    }

private:
    struct TimelineSubscriber {
        std::string id;
        std::string host;
        std::string protocol;
        std::string commandId;
        uint16_t port = 0;
        unsigned failures = 0;
    };

    struct ControllerCommand {
        std::string id;
        std::string host;
        std::string commandId;
    };

    void gdmLoop();
    void httpLoop();
    void timelinePushLoop();
    void requestTimelinePush(bool immediate);
    // Bounded buffering@navigation hold (holdIdle RCA, review H1-H4). Caller holds mu_.
    static int holdMsFromEnv(const char* name, int fallback);
    void armHoldLocked(int ms);
    void disarmHoldLocked();
    void subscribeTimeline(const std::string& id, const std::string& host,
                           const std::string& protocol, uint16_t port,
                           const std::string& commandId);
    void updateTimelineCommand(const std::string& id, const std::string& host,
                               const std::string& commandId);
    std::string timelineCommandFor(const std::string& id, const std::string& host,
                                   const std::string& commandId);
    // The controller of the active playMedia. A request or subscriber that never
    // carried a commandID echoes this owner's commandID instead of 0 (a relay that
    // strips the browser's id/commandID must not make Plex Web drop every timeline).
    void setCastOwner(const std::string& id, const std::string& host);
    std::string pushCommandFor(const TimelineSubscriber& subscriber);
    bool unsubscribeTimeline(const std::string& id, const std::string& host);
    bool postTimeline(const TimelineSubscriber& subscriber,
                      const std::string& xml) const;
    std::string gdmPayload() const;
    std::string resourcesXml() const;
    std::string timelineXml(const std::string& commandId) const;
    std::string lanIp() const;
    void log(const std::string& s) const;
    mutable SyncedLogAssembler logAsm_;
    TransportRequest transportRequestLocked(TransportCommand command);
    bool acceptSeekLocked(int64_t ms, TransportRequest& request);
    bool acceptPauseResumeLocked(bool pause, TransportRequest& request);
    static std::string xmlEsc(const std::string& s);

    std::string name_ = "MiSTerPlex";
    std::string machineId_ = "misterplex-1";
    uint16_t port_ = 3005;
    LogFn log_;
    PlayFn onPlay_;
    PlayQueuedFn onPlayQueued_;
    TokenFn onTokenUpdate_;
    TransportQueuedFn onTransportQueued_;
    TransportFn onPause_;
    TransportFn onResume_;
    TransportFn onStop_;
    std::function<bool()> deferStop_;
    TransportFn onSeek_;
    StepFn onStep_;
    CtrlFn onSkipNext_;
    TransportFn onSkipPrevious_;
    VolumeFn onVolume_;
    std::atomic<int> volume_{100};   // OWENB-P1B T5: the last setParameters volume
    PlexUiNavFn onPlexUiNav_;
    bool plexUiNavEnabled_ = false;

    bool openHttpListen();

    std::atomic<bool> running_{false};
    std::atomic<bool> httpReady_{false};
    int httpListenFd_{-1};
    std::thread gdmThr_;
    std::thread httpThr_;
    std::thread timelinePushThr_;

    std::mutex subscriberMu_;
    std::vector<TimelineSubscriber> subscribers_;
    std::vector<ControllerCommand> controllerCommands_;
    // Under subscriberMu_: the active playMedia controller (see setCastOwner).
    bool castOwnerSet_ = false;
    std::string castOwnerId_;
    std::string castOwnerHost_;
    std::mutex timelinePushMu_;
    std::condition_variable timelinePushCv_;
    bool timelinePushPending_ = false;
    bool timelinePushImmediate_ = false;
    // Under timelinePushMu_: one push when the wire hold expires (re-arm moves it).
    bool holdExpiryPending_ = false;
    std::chrono::steady_clock::time_point holdExpiry_{};

    mutable std::mutex mu_;
    std::string state_ = "stopped";
    bool terminalStop_ = false;
    int64_t timeMs_ = 0;
    int64_t lastGoodPositionMs_ = 0;
    std::string lastGoodKey_;
    int64_t durationMs_ = 0;
    // After seek/step plant: pin scrubber to this target until demux playing/
    // paused/ended is within catchup. Buffering never releases (plant itself is
    // buffering@target). Async race — early/stale playing@0 must not rewind.
    // -1 = no hold.
    int64_t scrubTargetMs_ = -1;
    int64_t resumeAfterRespawn_{0};
    int barTop_ = 0;
    int barBot_ = 0;
    int barLeft_ = 0;
    int barRight_ = 0;
    bool wantPlay_ = false;
    bool stopDuringResolve_ = false;
    bool prePlayHold_ = false;
    bool castBound_ = false;
    // Wire shows buffering@navigation only while now < holdUntil_ (epoch = no hold).
    // castBound_ alone never holds; a pure poll never sets this.
    std::chrono::steady_clock::time_point holdUntil_{};
    const int stopHoldMs_ = holdMsFromEnv("MPX_COMPANION_STOP_HOLD_MS", 8000);
    // BUG2-RECAST (owner 10-18): after a queue ends with nothing to advance to, the wire keeps the ended
    // frame (stopped @ final time, key/playQueue bound) for this long, then reports the clean idle a fresh
    // daemon sends (navigation, no key/containerKey/playQueueID). Plex Web treats "stopped + old playQueue"
    // as the player still owning that session: a new cast's POST /playQueues closes it again and Web logs
    // "Player was closed or a new playback session was started; ignoring playback request" (no playMedia).
    const int endedIdleMs_ = holdMsFromEnv("MPX_COMPANION_ENDED_IDLE_MS", 5000);
    std::chrono::steady_clock::time_point endedAt_{};
    bool endedIdleLocked() const;   // BUG2-RECAST: ended frame has decayed (mu_ held)
    const int mirrorHoldMs_ = holdMsFromEnv("MPX_COMPANION_MIRROR_HOLD_MS", 30000);
    // MPX_COMPANION_POLL_DIAG=1: log each /player/ request's controller identity and
    // commandID in/out, plus the poll/playMedia reply summary (lab diagnosis only).
    const bool pollDiag_ = holdMsFromEnv("MPX_COMPANION_POLL_DIAG", 0) == 1;

    // Active / staged media for Web scrubber
    std::string pendingKey_;
    std::string pendingContainerKey_;
    std::string pendingPlayQueueId_;
    std::string pendingPlayQueueItemId_;
    std::string pendingPlayQueueVersion_;
    std::string pendingRatingKey_;
    std::string pendingToken_;
    uint64_t pendingGeneration_ = 0;
    std::string serverMachineId_;
    std::string serverHost_;
    std::string serverPort_;
    std::string serverProto_ = "http";
    std::string rasterMode_;
    int activeHeight_ = 0;
    int picHeight_ = 0;
};

} // namespace misterplex
