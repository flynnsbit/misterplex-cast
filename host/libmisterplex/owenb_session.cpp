// OWENB-P1B T5: the OB play loop (owenb_session.hpp). Writes the owenb ring only (OwenbRing); never
// touches the idle framebuffer (owenb_session.hpp) and never paints DDR.
#include "owenb_session.hpp"
#include "ob_decode_envelope.hpp"
#include "soft_subs.hpp"
#include "../../arm/misterplexd/gcast/open_retry.hpp"
#include "../../arm/misterplexd/gcast/yt_403.hpp"
#include "../../arm/misterplexd/log_redact.hpp"
#include "../../arm/misterplexd/transcode_offset.hpp"
#include "p720_transcode_vf.hpp"
#include "http_range_resume.hpp"
#include <algorithm>
#include <chrono>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <utility>

extern "C" {
#include "h264ring.h"
}

namespace misterplex::owenb {

using h264stream::ControlResult;
using h264stream::PushResult;

const char* toString(EndReason r) {
    switch (r) {
    case EndReason::None: return "None";
    case EndReason::Eof: return "Eof";
    case EndReason::Stopped: return "Stopped";
    case EndReason::NeedsTranscode: return "NeedsTranscode";
    case EndReason::CoreLost: return "CoreLost";
    case EndReason::RingError: return "RingError";
    case EndReason::SourceError: return "SourceError";
    }
    return "?";
}

unsigned attenuationForVolume(int volume) {
    if (volume <= 0)
        return 255u;
    if (volume >= 100)
        return 0u;
    const long a = std::lround(-40.0 * std::log10(static_cast<double>(volume) / 100.0));
    return static_cast<unsigned>(a > 254 ? 254 : (a < 0 ? 0 : a));
}

namespace {

// The RBSP of one NAL (emulation-prevention bytes removed), at most max bytes.
std::vector<uint8_t> rbsp(const uint8_t* p, size_t n, size_t max) {
    std::vector<uint8_t> out;
    unsigned zeros = 0;
    for (size_t i = 0; i < n && out.size() < max; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}

struct Bits {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool bad = false;
    int bit() {
        if (pos >= b.size() * 8) {
            bad = true;
            return 0;
        }
        const int v = (b[pos / 8] >> (7 - pos % 8)) & 1;
        ++pos;
        return v;
    }
    uint32_t ue() {
        int lz = 0;
        while (!bad && bit() == 0)
            if (++lz > 31) {
                bad = true;
                return 0;
            }
        uint32_t v = 0;
        for (int i = 0; i < lz; ++i)
            v = (v << 1) | static_cast<uint32_t>(bit());
        return (1u << lz) - 1u + v;
    }
};

} // namespace

int ppsEntropyCabac(const uint8_t* au, size_t len) {
    size_t i = 0;
    while (i + 3 < len) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const size_t s = i + 3;
            size_t e = s;
            while (e + 2 < len && !(au[e] == 0 && au[e + 1] == 0 && (au[e + 2] == 1 || au[e + 2] == 0)))
                ++e;
            if (e + 2 >= len)
                e = len;
            if (s < e && (au[s] & 0x1F) == 8) {
                const std::vector<uint8_t> r = rbsp(au + s + 1, e - s - 1, 16);
                Bits b{r};
                b.ue();   // pic_parameter_set_id
                b.ue();   // seq_parameter_set_id
                const int f = b.bit();
                if (!b.bad)
                    return f;
            }
            i = e;
        } else {
            ++i;
        }
    }
    return -1;
}

OwenbPlaySession::OwenbPlaySession(OwenbRing& ring, SourceFactory factory, SessionConfig cfg)
    : ring_(ring), factory_(std::move(factory)), cfg_(std::move(cfg)) {}

void OwenbPlaySession::logLine(const std::string& s) const {
    if (cfg_.log)
        cfg_.log("[owenb-play] " + misterplex::logUrl(s));
}

void OwenbPlaySession::sleep(unsigned us) {
    if (cfg_.sleepUs)
        cfg_.sleepUs(us);
}

int64_t OwenbPlaySession::nowNs() const {
    if (cfg_.nowNs)
        return cfg_.nowNs();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t OwenbPlaySession::nowMs() const { return nowNs() / 1000000; }

void OwenbPlaySession::pause() {
    std::lock_guard<std::mutex> g(cmdMu_);
    cmds_.push_back(Cmd{Cmd::K::Pause, {}, 0});
    ++pendingCmds_;
}

void OwenbPlaySession::resume() {
    std::lock_guard<std::mutex> g(cmdMu_);
    cmds_.push_back(Cmd{Cmd::K::Resume, {}, 0});
    ++pendingCmds_;
}

void OwenbPlaySession::seek(const OpenRequest& req) {
    // Epoch and queue order are one critical section. Bumping before the lock let
    // two seeks queue in the opposite order to their epochs.
    const int64_t prev = positionMs();
    const int64_t media = mediaMs();
    uint32_t ep = 0;
    {
        std::lock_guard<std::mutex> g(cmdMu_);
        ep = seekEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        Cmd c;
        c.k = Cmd::K::Seek;
        c.req = req;
        c.epoch = ep;
        c.prevMs = prev;
        cmds_.push_back(std::move(c));
        ++pendingCmds_;
    }
    // The bar shows the target at once. The media word keeps the picture's
    // milliseconds and moves to this epoch at phase 0, so a reader can see that
    // the landing has not been committed yet.
    publishMedia(ep, 0, media);
    publishPosition(ep, 0, req.startMs);
}

void OwenbPlaySession::publishPacked(std::atomic<uint64_t>& word, uint32_t epoch, uint8_t phase, int64_t ms) {
    if (ms < 0)
        ms = 0;
    if (ms > 0x7fffffff)
        ms = 0x7fffffff;
    const uint32_t stamp = posStamp(epoch, phase);
    const uint64_t next = (static_cast<uint64_t>(stamp) << 32) | static_cast<uint32_t>(ms);
    uint64_t cur = word.load(std::memory_order_acquire);
    for (;;) {
        if (!posStampWins(static_cast<uint32_t>(cur >> 32), stamp))
            return;
        if (word.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}

void OwenbPlaySession::publishPosition(uint32_t epoch, uint8_t phase, int64_t ms) {
    publishPacked(packed_, epoch, phase, ms);
}

void OwenbPlaySession::publishMedia(uint32_t epoch, uint8_t phase, int64_t ms) {
    publishPacked(mediaPacked_, epoch, phase, ms);
}

void OwenbPlaySession::setVolumePercent(int volume) {
    std::lock_guard<std::mutex> g(cmdMu_);
    cmds_.push_back(Cmd{Cmd::K::Volume, {}, volume});
    ++pendingCmds_;
}

void OwenbPlaySession::selectStreams(int audioIndex, bool setAudio, int subtitleIndex, bool setSubtitle,
                                     int textContainerIndex, misterplex::PlexTextIdentity text) {
    Cmd c;
    c.k = Cmd::K::Streams;
    c.audioIndex = audioIndex;
    c.setAudio = setAudio;
    c.subtitleIndex = subtitleIndex;
    c.setSubtitle = setSubtitle;
    c.textContainerIndex = textContainerIndex;
    c.textId = std::move(text);
    if (c.textId.plexIndex < 0)
        c.textId.plexIndex = textContainerIndex;
    std::lock_guard<std::mutex> g(cmdMu_);
    cmds_.push_back(std::move(c));
    ++pendingCmds_;
}

void OwenbPlaySession::stop() {
    const int64_t t = nowMs();
    int64_t none = -1;
    stopAtMs_.compare_exchange_strong(none, t);
    stopReq_ = true;
    logLine("stop requested t=" + std::to_string(t));
    std::lock_guard<std::mutex> g(srcMu_);
    if (src_)
        src_->cancel();   // a blocked network read returns
}

std::string OwenbPlaySession::error() const {
    std::lock_guard<std::mutex> g(errMu_);
    return error_;
}

SessionStats OwenbPlaySession::stats() const {
    std::lock_guard<std::mutex> g(statsMu_);
    return stats_;
}

bool OwenbPlaySession::interruptPending() const { return stopReq_.load() || pendingCmds_.load() > 0; }

void OwenbPlaySession::endWith(EndReason r, const std::string& why) {
    if (end_ == EndReason::None) {
        end_ = r;
        std::lock_guard<std::mutex> g(errMu_);
        error_ = why;
    }
    logLine(std::string("end: ") + toString(r) + (why.empty() ? "" : ": " + why));
}

void OwenbPlaySession::fatalFromRing(const char* what) {
    const int e = ring_.lastErrno();
    const std::string msg = std::string(what) + ": " + ring_.lastError();
    if (e == -EOPNOTSUPP || e == -EBADMSG)
        endWith(EndReason::NeedsTranscode, "the CAVLC rewrite refused the stream (" + ring_.rewriteWhy() + "); " + msg);
    else if (e == -ENOTCONN)
        endWith(EndReason::CoreLost, "the core was taken (OSD Reset or another program); " + msg);
    else
        endWith(EndReason::RingError, msg);
}

bool OwenbPlaySession::openSource(const OpenRequest& req, int64_t* playedMs) {
    auto adopt = [&](std::unique_ptr<IMediaSource> s) -> bool {
        if (!s)
            return false;
        std::lock_guard<std::mutex> g(srcMu_);
        src_ = std::move(s);
        if (stopReq_)
            src_->cancel();
        return true;
    };
    auto notifyOpenedAudio = [&] {
        int audioIndex = -1;
        {
            std::lock_guard<std::mutex> g(srcMu_);
            if (src_)
                audioIndex = src_->activeAudioIndex();
        }
        if (cfg_.onAudioOpened)
            cfg_.onAudioOpened(audioIndex);
    };
    auto once = [&](OpenRequest r, std::string& err) {
        err.clear();
#ifndef MUT_OPEN_NO_STOP_CANCEL
        if (!r.cancelled)
            r.cancelled = [this] { return stopReq_.load(); };   // stop() cannot reach an open
#endif
        return factory_ ? factory_(r, err) : std::unique_ptr<IMediaSource>();
    };
    auto onceWith403 = [&](OpenRequest r, std::string& err) {
        auto s = once(r, err);
        if (!s && r.reresolve403 && gcast::yt403ShouldReresolve(err, 0) &&
            r.reresolve403(r.url, r.audioUrl)) {
            r.reresolve403 = nullptr;
            err.clear();
            s = once(r, err);
        }
        return s;
    };
    const bool reopen = reopenRetryArmed_.exchange(false);   // one-shot: cleared on every open
    if (!req.retryTransientOpen) {
        std::string err;
#ifdef MUT_REOPEN_RETRY_IGNORE_OFF
        const bool retryOn = true;
#else
        const bool retryOn = httprange::rangeResumeEnabled();
#endif
        const int64_t started = nowMs();
        OpenRequest rq = req;
        auto capFor = [&](OpenRequest& r) {
#ifndef MUT_REOPEN_NO_ATTEMPT_CAP
#ifdef MUT_REOPEN_CAP_IGNORE_OFF
            if (!reopen)
                return;
#else
            if (!reopen || !retryOn)   // MPX_HTTP_RANGE_RESUME=0: the old open, no attempt cap
                return;
#endif
            const int64_t left = started + cfg_.reopenRetryBudgetMs - nowMs();
            r.attemptTimeoutMs = std::min<int64_t>(cfg_.reopenAttemptCapMs, std::max<int64_t>(1000, left));
#else
            (void)r;
#endif
        };
        capFor(rq);
        auto s = onceWith403(rq, err);
#ifndef MUT_REOPEN_NO_NET_RETRY
        if (!s && reopen && retryOn) {
            // #231 Glass G: a drop's reopen gets "open: Connection refused"; retry inside the budget.
            for (int i = 0; !s && i < httprange::kReconnectRetries && !stopReq_.load() &&
                            netresume::reopenRetryable(err);
                 ++i) {
#ifdef MUT_REOPEN_NO_BUDGET
                const int64_t d = httprange::kReconnectBackoffMs[i];
#else
                const int64_t d = httprange::reconnectDelayMs(i, nowMs(), started, cfg_.reopenRetryBudgetMs);
#endif
                if (d <= 0)
                    break;
                logLine("range reopen retry at " + std::to_string(req.startMs) + " ms attempt " +
                        std::to_string(i + 1) + "/" + std::to_string(httprange::kReconnectRetries) + " in=" +
                        std::to_string(d) + "ms: " + err);
                const int64_t until = nowMs() + d;
                while (nowMs() < until && !stopReq_.load())
                    sleep(50000);
                if (stopReq_.load()) {
                    logLine("stop observed in retry wait t=" + std::to_string(nowMs()));
                    break;
                }
                err.clear();
                capFor(rq);
                s = onceWith403(rq, err);
            }
            if (!s && stopReq_.load()) {
                endWith(EndReason::Stopped, "");
                return false;
            }
        }
#endif
        if (!s) {
            endWith(EndReason::SourceError,
                    "open " + misterplex::redactUrlTokens(req.url) + ": " + (err.empty() ? "failed" : err));
            return false;
        }
        if (playedMs)
            *playedMs = req.startMs;
        if (!adopt(std::move(s)))
            return false;
        notifyOpenedAudio();
        if (req.subtitleStreamIndex >= 0)
            applySubtitleCommand(currentReq_, req.subtitleStreamIndex, true, cfg_.onSubtitleSelect);
        armStallWatch();
        return true;
    }

    std::unique_ptr<IMediaSource> held;
    std::string lastErr;
    const gcast::RetryHit hit = gcast::retryOpens(
        [&](int64_t ms, gcast::OpenClass& c) {
            if (stopReq_.load())
                return;
            OpenRequest r = req;
            r.startMs = ms;
            r.retryTransientOpen = false;
            if (r.url.find("start_ms=") != std::string::npos)
                r.url = gcast::urlWithStartMs(r.url, ms);
            std::string err;
            auto s = onceWith403(r, err);
            lastErr = err;
            c.error = err;
            if (s) {
                c.ok = true;
                c.bodyBytes = 1;
                c.httpStatus = 200;
                held = std::move(s);
            } else {
                c.ok = false;
                c.bodyBytes = 0;
                if (err.find("Server returned 5") != std::string::npos || err.find("5XX") != std::string::npos)
                    c.httpStatus = 503;
                else if (err.find("Input/output error") != std::string::npos ||
                         err.find("prematurely") != std::string::npos)
                    c.httpStatus = 200;
            }
        },
        req.startMs, req.previousMs,
        [&] { return !stopReq_.load(); },
        [&](int64_t ms) {
            logLine("gcast open retry, buffering at " + std::to_string(ms) + " ms");
            if (cfg_.onBuffering)
                cfg_.onBuffering(ms);
        },
        [&](int ms) {
            int left = ms;
            while (left > 0 && !stopReq_.load()) {
                const int n = left > 50 ? 50 : left;
                sleep(static_cast<unsigned>(n) * 1000u);
                left -= n;
            }
        });
    if (hit.superseded || stopReq_.load()) {
        held.reset();
        endWith(EndReason::Stopped, "");
        return false;
    }
    if (!hit.played || !held) {
        endWith(EndReason::SourceError,
                "open " + misterplex::redactUrlTokens(req.url) + ": " +
                    (lastErr.empty() ? "retries exhausted" : lastErr));
        return false;
    }
    if (hit.resumedPrevious)
        logLine("gcast seek failed; resumed at " + std::to_string(hit.playedMs) + " ms");
    if (playedMs)
        *playedMs = hit.playedMs;
    if (!adopt(std::move(held)))
        return false;
    notifyOpenedAudio();
    if (req.subtitleStreamIndex >= 0)
        applySubtitleCommand(currentReq_, req.subtitleStreamIndex, true, cfg_.onSubtitleSelect);
    armStallWatch();
    return true;
}

bool OwenbPlaySession::takeCommands() {
    if (stopReq_) {
        endWith(EndReason::Stopped, "");
        return false;
    }
    std::deque<Cmd> cmds;
    {
        std::lock_guard<std::mutex> g(cmdMu_);
        cmds.swap(cmds_);
        pendingCmds_ = 0;
    }
    for (Cmd& c : cmds) {
        switch (c.k) {
        case Cmd::K::Pause:
            if (!started_) {
                logLine("pause before the first picture: ignored (cast clients pause while buffering)");
                break;
            }
            if (userPaused_)
                break;
            userPaused_ = true;
            if (!corePaused_ && ring_.pause(sid_) == ControlResult::Ok)
                corePaused_ = true;
            { std::lock_guard<std::mutex> g(statsMu_); ++stats_.pauses; }
            break;
        case Cmd::K::Resume:
            if (!userPaused_)
                break;
            userPaused_ = false;
            // A held seek still waiting for its keyframe resumes when the keyframe is queued.
            if (corePaused_ && !flushBeforeKey_ && ring_.resume(sid_) == ControlResult::Ok)
                corePaused_ = false;
            { std::lock_guard<std::mutex> g(statsMu_); ++stats_.resumes; }
            break;
        case Cmd::K::Seek:
            if (!applySeek(c))
                return false;
            break;
        case Cmd::K::Stop:
            endWith(EndReason::Stopped, "");
            return false;
        case Cmd::K::Volume:
            volume_ = c.volume;
            ring_.setAttenuation(sid_, attenuationForVolume(volume_));
            break;
        case Cmd::K::Streams: {
            bool restart = false;
            if (c.setAudio) {
                const int previous = currentReq_.audioStreamIndex;
                currentReq_.audioStreamIndex = c.audioIndex;
                std::string err;
                const StreamSwitch sw = src_ ? src_->selectAudioStream(c.audioIndex, err) : StreamSwitch::Failed;
                if (sw != StreamSwitch::Applied)
                    currentReq_.audioStreamIndex = previous;
                if (sw == StreamSwitch::Failed) {
                    logLine("audio switch needs a reopen" + (err.empty() ? std::string() : ": " + err));
                    restart = true;
                } else if (sw == StreamSwitch::Unsupported) {
                    logLine("audio track not applied" + (err.empty() ? std::string() : ": " + err));
                }
                if (cfg_.onAudioSwitch)
                    cfg_.onAudioSwitch(sw == StreamSwitch::Applied ? c.audioIndex : previous, sw);
            }
            if (applySubtitleCommand(currentReq_, c.subtitleIndex, c.setSubtitle, cfg_.onSubtitleSelect)) {
                if (c.subtitleIndex < 0) {
                    shownSubtitle_.clear();
                    if (cfg_.onSubtitle)
                        cfg_.onSubtitle("");
                }
                logLine(c.subtitleIndex > 0 ? ("subtitle select stream=" + std::to_string(c.subtitleIndex))
                                            : "subtitle select off");
            }
            if (c.textContainerIndex >= 0 && !c.textId.codec.empty()) {
                currentReq_.textCodec = c.textId.codec;
                currentReq_.textLanguage = c.textId.language;
                currentReq_.textTitle = c.textId.title;
                currentReq_.textForced = c.textId.forced;
                currentReq_.textOrdinal = c.textId.textOrdinal;
                currentReq_.textSubtitleIndex = c.textContainerIndex;
            }
            if (c.textContainerIndex >= -1 &&
                (misterplex::packetTextDecodeAllowedForCodec(currentReq_.textCodec) || c.textContainerIndex == -1) &&
                src_) {
                std::string subErr;
                if (c.textContainerIndex >= 0)
                    src_->setEmbeddedTextIdentity(c.textId);
                const StreamSwitch sw = src_->selectSubtitleStream(c.textContainerIndex, subErr);
                if (sw != StreamSwitch::Applied)
                    logLine("soft subtitle not applied" + (subErr.empty() ? std::string() : ": " + subErr));
            }
            if (restart) {
                if (currentReq_.containerSeek) {
                    Cmd seek;
                    seek.k = Cmd::K::Seek;
                    seek.req = currentReq_;
                    seek.req.startMs = std::max<int64_t>(0, positionMs());
                    seek.prevMs = positionMs();
                    seek.epoch = seekEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
                    publishPosition(seek.epoch, 0, seek.req.startMs);
                    if (!applySeek(seek))
                        return false;
                } else if (cfg_.onStreamRestart) {
                    cfg_.onStreamRestart(positionMs());
                }
            }
            break;
        }
        }
    }
    return true;
}

bool OwenbPlaySession::applySeek(const Cmd& c) {
    const OpenRequest& req = c.req;
    if (c.epoch != seekEpoch_.load(std::memory_order_acquire)) {
        logLine("seek superseded");
        return true;
    }
    // A seek during a network retry becomes the new target and the backoff starts over.
    if (netEpisode_) {
        if (netBackoff_)
            netBackoff_->resetBudget();
        netEpisode_ = false;
        netSeekTook_ = true;
        stallShown_ = false;
    }
    currentReq_ = req;
    shownSubtitle_.clear();
    if (cfg_.onSubtitle)
        cfg_.onSubtitle("");
    logLine("seek (held) to " + std::to_string(req.startMs) + " ms");
    { std::lock_guard<std::mutex> g(statsMu_); ++stats_.seeks; stats_.eosSent = false; }
    // Hold the current picture while the new input opens and reaches its keyframe.
    if (!corePaused_ && ring_.pause(sid_) == ControlResult::Ok)
        corePaused_ = true;
    out_.clear();
    syncOutDepth();
    outBytes_ = 0;
    audioOffset_ = 0;
    int64_t playedMs = req.startMs;
    if (!openSource(req, &playedMs))
        return false;
    if (c.epoch != seekEpoch_.load(std::memory_order_acquire)) {
        // This source is already installed and a newer seek has not run yet.
        // Drop until a keyframe and flush, so its non-keys are not appended to
        // the old stream. The newer seek replaces the source.
        logLine("seek superseded");
        needKey_ = true;
        flushBeforeKey_ = true;
        return true;
    }
    needKey_ = true;
#ifndef MUT_OWENB_SESS_SEEK_NO_FLUSH
    flushBeforeKey_ = true;
#else
    flushBeforeKey_ = false;   // mutant: the old stream's queue is not dropped
#endif
    inputEof_ = false;
    srcBaseMs_ = playedMs;
    srcFirstPts_ = srcLastPts_ = -1;
    drainStartUs_ = 0;
    drainTotalUs_ = 0;
    drainLevel_ = UINT32_MAX;
    rateStartPts_ = -1;
    rateBytes_ = 0;
    rewriteNs_ = 0;
    rwFirstPts_ = rwLastPts_ = -1;
    // The target is already published (phase 0) by seek(). The anchor waits for the
    // first AU; until then a stale STC must not move the position.
    // A gcast retry that resumes at an earlier offset publishes that offset, not the
    // requested target. playedMs equals startMs on every other open.
    clock_.prepareSeek(c.epoch, c.prevMs, playedMs);
    int64_t origin = h264stream::kNoPts90k;
    {
        std::lock_guard<std::mutex> g(srcMu_);
        if (src_)
            origin = src_->startPts90k();
    }
    clock_.arm(c.epoch, req.containerSeek, origin);
    return true;
}

void OwenbPlaySession::updatePosition() {
    const uint32_t ep = seekEpoch_.load(std::memory_order_acquire);
    const CoreStatus cs = ring_.coreStatus();
    if (!cs.valid)
        return;
    if (!cs.sessionOk) {
        endWith(EndReason::CoreLost, "the core no longer runs this session (OSD Reset or another program)");
        return;
    }
    if (cs.state == H264RING_S_ERROR) {
        endWith(EndReason::RingError, "the core is in ERROR (err " + std::to_string(cs.err) + ", refusal " +
                                          std::to_string(cs.refusedCode) + ")");
        return;
    }
    // A seek has been queued and has not installed its anchor yet. The target seek()
    // published stays; this sample was taken against the previous picture.
    if (ep != clock_.epoch)
        return;
    const bool stcRun = (cs.live & H264RING_L_STC_RUN) != 0 && cs.stc90k >= 0;
    const int64_t now = nowMs();
    int64_t ms = 0;
    const bool got = clock_.takeSample(ep, cs.stc90k, stcRun, ms, userPaused_.load(std::memory_order_acquire), now);
    // A stalled core never accepts a sample, so the hold cap has to be checked here.
    const bool expired = clock_.pollHold(now);
    // A running clock that is not the new picture yet must still move the bar.
    int64_t reckon = 0;
    const bool moving = clock_.pollReckon(now, reckon);
    if (seekEpoch_.load(std::memory_order_acquire) != ep)
        return;
    if (!got && !expired && !moving)
        return;
    publishMedia(ep, 2, clock_.mediaMs);
    const int64_t shown = got ? ms : expired ? clock_.reportedMs() : reckon;
    publishPosition(ep, 2, shown);
    if (!picsAtOpenSet_) {
        picsAtOpen_ = cs.picsReconstructed;
        picsAtOpenSet_ = true;
    }
    if (got && !firstPicLogged_) {
        // #207: "first picture" only on real decoder output (RC_PIC moved), not on STC alone.
        if (misterplex::firstPictureReal(got, cs.picsReconstructed, picsAtOpen_)) {
            firstPicLogged_ = true;
            decodedPic_ = true;
            logLine("first picture (STC running, decoded=" +
                    std::to_string(cs.picsReconstructed - picsAtOpen_) + ")");
        } else if (!noPicWarned_) {
            noPicWarned_ = true;
            logLine("STC running but no decoded picture yet (RC_PIC unchanged)");
        }
    }
    if (got && !started_) {
        started_ = true;
        if (onFirstPicture)
            onFirstPicture();
    }
    publishSubtitle();
}

void OwenbPlaySession::publishSubtitle() {
    if (!cfg_.onCaptionClock)
        return;
    std::string packet;
    const int64_t ms = mediaMs();
    if (misterplex::packetTextDecodeAllowedForCodec(currentReq_.textCodec)) {
        std::lock_guard<std::mutex> g(srcMu_);
        if (src_)
            packet = src_->activeSubtitle(ms);
    }
    cfg_.onCaptionClock(ms, packet);
}

// The CAVLC rate guard (design §a.4): a CAVLC stream above OB_CAVLC_MAX_KBPS is not played; the
// daemon asks PMS for a lowered (re-encoded, CABAC) request instead.
bool OwenbPlaySession::checkVideoIn(const MediaPacket& p) {
    const int e = ppsEntropyCabac(p.data.data(), p.data.size());
    if (e >= 0 && e != entropyCabac_) {
        entropyCabac_ = e;
        logLine(e ? "stream: CABAC" : "stream: CAVLC (rewritten to CABAC by the library)");
#ifndef MUT_OWENB_SESS_NO_ENTROPY_REPORT
        if (e && cfg_.onEntropy)
            cfg_.onEntropy(false, 0, 0);
#endif
    }
#ifndef MUT_OWENB_SESS_NO_CAVLC_GUARD
    if (entropyCabac_ == 0 && cfg_.cavlcMaxKbps && p.pts90k >= 0) {
        if (rateStartPts_ < 0 || p.pts90k < rateStartPts_) {
            rateStartPts_ = p.pts90k;
            rateBytes_ = 0;
        }
        rateBytes_ += p.data.size();
        const int64_t span = p.pts90k - rateStartPts_;
        if (span >= static_cast<int64_t>(cfg_.cavlcWindowMs) * 90) {
            const uint64_t kbps = rateBytes_ * 8u * 90u / static_cast<uint64_t>(span);
            // v2-bugfix: the rewrite's share of one A9 over the window (rewrite ns / window ns in PTS time).
            // BUG1-CAP: the load is the rewrite time over the PTS span of the AUs it rewrote (the writes lag the
            // demux by the queue). Under half a window of writes: not known yet, no verdict on load.
            const int64_t loadSpan = (rwFirstPts_ >= 0 && rwLastPts_ > rwFirstPts_) ? rwLastPts_ - rwFirstPts_ : 0;
            const bool loadKnown = loadSpan * 2 >= span;
            const int64_t windowNs = loadSpan * 100000 / 9;
            const unsigned loadPct =
                windowNs > 0 ? static_cast<unsigned>(std::min<int64_t>(1000, rewriteNs_ * 100 / windowNs)) : 0;
#ifndef MUT_OWENB_SESS_CAVLC_RATE_ONLY
            const bool overLoad = cfg_.cavlcMaxLoadPct == 0 || (loadKnown && loadPct > cfg_.cavlcMaxLoadPct) ||
#ifndef MUT_OWENB_SESS_NO_HARD_CAP
                                  (cfg_.cavlcHardMaxKbps && kbps > cfg_.cavlcHardMaxKbps) ||
#endif
                                  false;
#else
            const bool overLoad = true;   // mutant: the nominal rate alone ends the play (the  lab bug 1)
            (void)loadKnown;
#endif
#ifndef MUT_OWENB_SESS_NO_ENTROPY_REPORT
            if (loadKnown && cfg_.onEntropy)
                cfg_.onEntropy(true, static_cast<unsigned>(kbps), loadPct);
#endif
            if (kbps > cfg_.cavlcMaxKbps && overLoad) {
                std::string why = "CAVLC at " + std::to_string(kbps) + " kbps, above OB_CAVLC_MAX_KBPS=" +
                                  std::to_string(cfg_.cavlcMaxKbps);
                if (cfg_.cavlcHardMaxKbps && kbps > cfg_.cavlcHardMaxKbps)
                    why += ", above OB_CAVLC_HARD_MAX_KBPS=" + std::to_string(cfg_.cavlcHardMaxKbps);
                else if (cfg_.cavlcMaxLoadPct)
                    why += ", rewrite load " + std::to_string(loadPct) + "% above OB_CAVLC_MAX_LOAD_PCT=" +
                           std::to_string(cfg_.cavlcMaxLoadPct);
                endWith(EndReason::NeedsTranscode, why);
                return false;
            }
            if (kbps > cfg_.cavlcMaxKbps && !cavlcOverRateLogged_) {
                cavlcOverRateLogged_ = true;
                logLine("stream: CAVLC at " + std::to_string(kbps) + " kbps (above OB_CAVLC_MAX_KBPS=" +
                        std::to_string(cfg_.cavlcMaxKbps) + "), rewrite load " + std::to_string(loadPct) +
                        "% (<= OB_CAVLC_MAX_LOAD_PCT): the play continues");
            }
            rateStartPts_ = p.pts90k;
            rateBytes_ = 0;
            rewriteNs_ = 0;
            rwFirstPts_ = rwLastPts_ = -1;
        }
    }
#endif
    return true;
}

bool OwenbPlaySession::inputNearEnd() const {
    const int64_t dur = cfg_.durationMs ? cfg_.durationMs() : 0;
    if (dur <= 0)
        return false;
    int64_t srcEnd = positionMs();
    if (srcFirstPts_ >= 0 && srcLastPts_ >= srcFirstPts_)
        srcEnd = std::max(srcEnd, srcBaseMs_ + (srcLastPts_ - srcFirstPts_) / 90);
#ifdef MUT_OWENB_ERR_NEAREND_ANY
    return true;   // mutant: any read error ends clean, even mid-clip
#endif
    return dur - srcEnd <= static_cast<int64_t>(cfg_.prematureNearEndMs);
}

OwenbPlaySession::EarlyEof OwenbPlaySession::classifyEof(bool fromError) {
    const int64_t dur = cfg_.durationMs ? cfg_.durationMs() : 0;
#ifdef MUT_OWENB_PREMATURE_OFF
    return EarlyEof::NotEarly;   // mutant: the old behaviour, every EOF is the end
#endif
    if (!cfg_.onPrematureEof || dur <= 0 || stopReq_.load() || pendingCmds_.load() > 0)
        return EarlyEof::NotEarly;
    // Where the input really ended: its video span from where it started, and the picture's
    // own position (the core can be behind the push, so the picture alone under-reports).
    int64_t srcEnd = positionMs();
    if (srcFirstPts_ >= 0 && srcLastPts_ >= srcFirstPts_)
        srcEnd = std::max(srcEnd, srcBaseMs_ + (srcLastPts_ - srcFirstPts_) / 90);
#ifndef MUT_OWENB_PREMATURE_NO_NEAR_END
    if (dur - srcEnd <= static_cast<int64_t>(cfg_.prematureNearEndMs))
        return EarlyEof::NotEarly;
#endif   // the title's end (or its last seconds)
    const int64_t resumeMs = std::max<int64_t>(0, mediaMs());
    const int64_t now = nowMs();
    while (!resumeAtMs_.empty() && now - resumeAtMs_.front() > static_cast<int64_t>(cfg_.prematureResumeWindowMs))
        resumeAtMs_.pop_front();
    const std::string where = "input ended at " + std::to_string(srcEnd) + " ms of " + std::to_string(dur) +
                              " ms (picture " + std::to_string(resumeMs) + " ms)";
#ifndef MUT_OWENB_PREMATURE_NO_CAP
    if (resumeAtMs_.size() >= cfg_.prematureResumeMax) {
        logLine("premature end: " + where + "; " + std::to_string(resumeAtMs_.size()) + " resumes in the last " +
                std::to_string(cfg_.prematureResumeWindowMs / 1000) + " s: giving up");
        endWith(EndReason::SourceError, "the stream ended early (" + where + ") and did not resume");
        return EarlyEof::Capped;
    }
#endif
    const unsigned attempt = static_cast<unsigned>(resumeAtMs_.size()) + 1;
    logLine("premature end: " + where + "; resuming at " + std::to_string(resumeMs) + " ms (attempt " +
            std::to_string(attempt) + "/" + std::to_string(cfg_.prematureResumeMax) + ")");
    if (fromError) {
#ifndef MUT_OWENB_ERR_NO_BACKOFF
        // A dropped WAN stream is usually not back at once: wait attempt * backoff, in short slices
        // so a stop is not held up, before the input is opened again.
        const int64_t until = nowMs() + static_cast<int64_t>(attempt) * cfg_.prematureErrorBackoffMs;
        while (nowMs() < until && !stopReq_.load())
            sleep(50000);
#endif
        if (stopReq_.load()) {
            logLine("stop observed in premature backoff t=" + std::to_string(nowMs()));
            return EarlyEof::NotEarly;
        }
    }
    reopenRetryArmed_.store(true);
    if (!cfg_.onPrematureEof(resumeMs, attempt)) {
        reopenRetryArmed_.store(false);
        return EarlyEof::NotEarly;
    }
    resumeAtMs_.push_back(now);
    return EarlyEof::Resumed;
}

bool OwenbPlaySession::netResumeOn() const {
#ifdef MUT_NET_RESUME_IGNORE_OFF
    return true;   // mutant: the flag is ignored and every play retries
#else
    return cfg_.netResume;
#endif
}

bool OwenbPlaySession::netResumeEligible(const OpenRequest& req) const {
    if (req.retryTransientOpen || req.reresolve403)
        return false;
    const std::string& u = req.url;
    if (u.find("googlevideo.com") != std::string::npos || u.find("/raop.wav") != std::string::npos ||
        u.find("/yt.ts") != std::string::npos)
        return false;
    return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

int64_t OwenbPlaySession::lastAuMs() const {
    if (srcFirstPts_ >= 0 && srcLastPts_ >= srcFirstPts_)
        return srcBaseMs_ + (srcLastPts_ - srcFirstPts_) / 90;
    return -1;
}

int64_t OwenbPlaySession::resumePictureMs() const {
    int64_t media = 0;
    uint32_t stamp = 0;
    readMedia(media, stamp);
    const int64_t pending = (stamp & 0xffu) < 2 ? positionMs() : -1;
    return netresume::resumeTargetMs(media, stamp, pending, lastAuMs(), 0);
}

void OwenbPlaySession::publishRecover(const RecoverStatus& st) {
    if (cfg_.onRecover)
        cfg_.onRecover(st);
}

void OwenbPlaySession::giveUpNet(int64_t goodMs, const std::string& why) {
    netEpisode_ = false;
    stallShown_ = false;
    const uint32_t ep = seekEpoch_.load(std::memory_order_acquire);
    publishMedia(ep, 2, goodMs);
    publishPosition(ep, 2, goodMs);
    RecoverStatus done;
    done.targetMs = goodMs;
    publishRecover(done);
    logLine("net resume give up: " + why);
    endWith(EndReason::SourceError, why);
}

void OwenbPlaySession::noteNetRecovered() {
    if (!netEpisode_)
        return;
    netEpisode_ = false;
    stallShown_ = false;
    if (netBackoff_)
        netBackoff_->noteRecovered(nowMs());
    RecoverStatus st;
    st.reconnected = true;
    st.targetMs = mediaMs();
    publishRecover(st);
    logLine("net resume reconnected");
}

void OwenbPlaySession::armStallWatch() {
    if (!netResumeOn() || !src_)
        return;
    netresume::StallWatch w;
    w.enabled = true;
    w.nowMs = [this] { return nowMs(); };
    w.armed = [this] {
        if (!started_.load() || userPaused_.load())
            return false;
        const CoreStatus cs = ring_.coreStatus();
        return cs.valid && cs.ringLevel == 0;
    };
    w.onShow = [this] {
        if (stallShown_ || netEpisode_)
            return;
        stallShown_ = true;
        RecoverStatus st;
        st.active = true;
        st.maxTries = netresume::Policy{}.maxTries;
        st.targetMs = resumePictureMs();
        publishRecover(st);
        logLine("net resume stall");
    };
    w.onClear = [this] {
        if (!stallShown_ || netEpisode_)
            return;
        stallShown_ = false;
        publishRecover(RecoverStatus{});
    };
    src_->setStallWatch(std::move(w));
}

bool OwenbPlaySession::waitBudget(int64_t ms) {
    int64_t left = ms * 1000;
    if (left < 0)
        left = 0;
    for (;;) {
        if (stopReq_) {
            endWith(EndReason::Stopped, "");
            return false;
        }
        if (!takeCommands())
            return false;
        if (netSeekTook_ || end_ != EndReason::None)
            return end_ == EndReason::None;
        if (left <= 0)
            return true;
        const unsigned step = left > 50000 ? 50000u : static_cast<unsigned>(left);
        sleep(step);
        left -= step;
    }
}

bool OwenbPlaySession::openQuiet(const OpenRequest& req, std::string& err) {
    err.clear();
    if (!factory_) {
        err = "no source";
        return false;
    }
    auto s = factory_(req, err);
    if (!s)
        return false;
    if (stopReq_.load()) {
        s->cancel();
        endWith(EndReason::Stopped, "");
        return false;
    }
    {
        std::lock_guard<std::mutex> g(srcMu_);
        src_ = std::move(s);
        if (stopReq_)
            src_->cancel();
    }
    int audioIndex = -1;
    {
        std::lock_guard<std::mutex> g(srcMu_);
        if (src_)
            audioIndex = src_->activeAudioIndex();
    }
    if (cfg_.onAudioOpened)
        cfg_.onAudioOpened(audioIndex);
    return true;
}

void OwenbPlaySession::finishResumeOpen(const OpenRequest& req, uint32_t epoch, int64_t prevMs) {
    currentReq_ = req;
    shownSubtitle_.clear();
    if (cfg_.onSubtitle)
        cfg_.onSubtitle("");
    needKey_ = true;
#ifndef MUT_OWENB_SESS_SEEK_NO_FLUSH
    flushBeforeKey_ = true;
#else
    flushBeforeKey_ = false;
#endif
    inputEof_ = false;
    out_.clear();
    syncOutDepth();
    outBytes_ = 0;
    audioOffset_ = 0;
    srcBaseMs_ = req.startMs;
    srcFirstPts_ = srcLastPts_ = -1;
    drainStartUs_ = 0;
    drainTotalUs_ = 0;
    drainLevel_ = UINT32_MAX;
    rateStartPts_ = -1;
    rateBytes_ = 0;
    rewriteNs_ = 0;
    rwFirstPts_ = rwLastPts_ = -1;
    clock_.prepareSeek(epoch, prevMs, req.startMs);
    int64_t origin = h264stream::kNoPts90k;
    {
        std::lock_guard<std::mutex> g(srcMu_);
        if (src_)
            origin = src_->startPts90k();
    }
    clock_.arm(epoch, req.containerSeek, origin);
    if (req.subtitleStreamIndex >= 0)
        applySubtitleCommand(currentReq_, req.subtitleStreamIndex, true, cfg_.onSubtitleSelect);
    { std::lock_guard<std::mutex> g(statsMu_); ++stats_.seeks; stats_.eosSent = false; }
    armStallWatch();
}

OwenbPlaySession::ResumeStep OwenbPlaySession::tryNetResume(bool eof, const std::string& err) {
    if (!netResumeOn())
        return ResumeStep::Off;
    if (!started_.load() || !netResumeEligible(currentReq_))
        return ResumeStep::Off;
    int avErr = 0;
    int http = 0;
    {
        std::lock_guard<std::mutex> g(srcMu_);
        if (src_) {
            avErr = src_->lastAvErr();
            http = src_->lastHttpStatus();
        }
    }
    if (avErr == 0)
        avErr = netresume::errnoFromText(err);
    if (http == 0)
        http = netresume::httpStatusFromText(err);
    const bool transcode = misterplex::isUniversalTranscodeUrl(currentReq_.url);
    const int64_t dur = cfg_.durationMs ? cfg_.durationMs() : 0;
    const int64_t picture = resumePictureMs();
    const netresume::Fault fault =
        netresume::classifyFault(avErr, http, transcode, eof, picture, dur);
    if (fault == netresume::Fault::None)
        return eof ? ResumeStep::Natural : ResumeStep::Fatal;
    if (!netresume::isRecoverable(fault))
        return ResumeStep::Fatal;

    if (!netBackoff_)
        netBackoff_.emplace(netresume::Policy{}, cfg_.netResumeSeed);
    const int64_t now = nowMs();
    if (!netEpisode_) {
        if (!netBackoff_->beginFault(now)) {
            giveUpNet(picture, netresume::lostConnectionText(picture));
            return ResumeStep::Ended;
        }
        netEpisode_ = true;
        if (!corePaused_ && ring_.pause(sid_) == h264stream::ControlResult::Ok)
            corePaused_ = true;
    }
    if (fault == netresume::Fault::EarlyEof) {
        if (netBackoff_->noteEarlyEof()) {
            giveUpNet(picture, "Server ended the stream");
            return ResumeStep::Ended;
        }
    } else {
        netBackoff_->clearEarlyEof();
    }
    const std::optional<int64_t> delay = netBackoff_->nextDelayMs(nowMs());
    if (!delay) {
        giveUpNet(picture, netresume::lostConnectionText(picture));
        return ResumeStep::Ended;
    }
    int64_t media = 0;
    uint32_t stamp = 0;
    readMedia(media, stamp);
    const int64_t pending = (stamp & 0xffu) < 2 ? positionMs() : -1;
    const int64_t target = netresume::resumeTargetMs(media, stamp, pending, lastAuMs(), netBackoff_->policy().rewindMs);
    RecoverStatus st;
    st.active = true;
    st.attempt = netBackoff_->attempt();
    st.maxTries = netBackoff_->maxTries();
    st.targetMs = target;
    publishRecover(st);
    logLine("net resume attempt " + std::to_string(st.attempt) + "/" + std::to_string(st.maxTries) + " at " +
            std::to_string(target) + " ms");
    if (!waitBudget(*delay))
        return end_ == EndReason::None ? ResumeStep::Continue : ResumeStep::Ended;
    if (netSeekTook_) {
        netSeekTook_ = false;
        return ResumeStep::Continue;
    }
    const int64_t prev = positionMs();
    const uint32_t ep = seekEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    publishMedia(ep, 0, media);
    publishPosition(ep, 0, target);
    OpenRequest q = currentReq_;
    q.startMs = target;
    q.retryTransientOpen = false;
    q.reresolve403 = nullptr;
    q.previousMs = prev;
    if (transcode) {
        q.url = misterplex::transcode_offset::withUniversalOffset(currentReq_.url, target);
        q.containerSeek = false;
    } else {
        q.containerSeek = true;
    }
    std::string oerr;
    if (!openQuiet(q, oerr)) {
        if (end_ != EndReason::None)
            return ResumeStep::Ended;
        const int openHttp = netresume::httpStatusFromText(oerr);
        const netresume::Fault of = openHttp ? netresume::classifyFault(0, openHttp, transcode, false, picture, dur)
                                             : netresume::Fault::Network;
        if (!netresume::isRecoverable(of)) {
            netEpisode_ = false;
            endWith(EndReason::SourceError, oerr.empty() ? "open failed" : oerr);
            return ResumeStep::Ended;
        }
        logLine("net resume open failed: " + (oerr.empty() ? std::string("failed") : oerr));
        return ResumeStep::Continue;
    }
    finishResumeOpen(q, ep, prev);
    return ResumeStep::Continue;
}

bool OwenbPlaySession::readOne() {
    MediaPacket p;
    std::string err;
    IMediaSource* s = src_.get();   // replaced only on this thread
    const SourceRead r = s ? s->read(p, err) : SourceRead::Error;
    switch (r) {
    case SourceRead::Again:
        sleep(cfg_.backoffUs);
        return true;
    case SourceRead::Eof:
        if (netResumeOn()) {
            switch (tryNetResume(true, err)) {
            case ResumeStep::Ended:
                return false;
            case ResumeStep::Continue:
                return true;
            case ResumeStep::Natural:
            case ResumeStep::Fatal:
                inputEof_ = true;
                return true;
            case ResumeStep::Off:
                break;
            }
        }
        switch (classifyEof()) {
        case EarlyEof::Resumed:
            return true;    // the seek is queued; takeCommands applies it next pass
        case EarlyEof::Capped:
            return false;   // end_ is set (SourceError)
        case EarlyEof::NotEarly:
            break;
        }
        inputEof_ = true;
        return true;
    case SourceRead::Error:
        if (netResumeOn() && !stopReq_.load()) {
            switch (tryNetResume(false, err)) {
            case ResumeStep::Ended:
                return false;
            case ResumeStep::Continue:
                return true;
            case ResumeStep::Fatal:
                endWith(EndReason::SourceError, err.empty() ? "read failed" : err);
                return false;
            case ResumeStep::Natural:
            case ResumeStep::Off:
                break;
            }
        }
#ifndef MUT_OWENB_ERR_NO_RESUME
        if (!stopReq_.load()) {
            // A read error far from the end of a PMS stream is a dropped connection: reopen at the
            // picture's position like an early EOF (the demuxer seeks with an HTTP Range request).
            switch (classifyEof(true)) {
            case EarlyEof::Resumed:
                return true;
            case EarlyEof::Capped:
                return false;
            case EarlyEof::NotEarly:
                break;
            }
        }
#endif
#ifndef MUT_OWENB_ERR_NEAREND_OFF
        if (!stopReq_.load() && cfg_.nearEndErrorIsEof && inputNearEnd()) {
            // The whole clip was delivered and the sender then closed the stream: libav calls that EIO.
            logLine("read error at the end of the clip (" + (err.empty() ? std::string("read failed") : err) +
                    "): clean end");
            inputEof_ = true;
            return true;
        }
#endif
        if (stopReq_)
            endWith(EndReason::Stopped, "");
        else
            endWith(EndReason::SourceError, err.empty() ? "read failed" : err);
        return false;
    case SourceRead::Packet:
        break;
    }
    if (p.data.empty())
        return true;
    if (netEpisode_)
        noteNetRecovered();
    else if (netResumeOn() && started_.load() && netBackoff_)
        netBackoff_->onCleanPlay(nowMs());
    if (p.kind == MediaPacket::Kind::Video) {
        if (p.pts90k >= 0) {
            if (srcFirstPts_ < 0)
                srcFirstPts_ = p.pts90k;
            srcLastPts_ = p.pts90k;
        }
        if (needKey_ && !p.keyframe) {
            std::lock_guard<std::mutex> g(statsMu_);
            ++stats_.droppedBeforeKey;
            return true;
        }
        needKey_ = false;
        if (!checkVideoIn(p))
            return false;
        ++videoInOut_;
    } else {
        if (needKey_) {
            std::lock_guard<std::mutex> g(statsMu_);
            ++stats_.droppedAudioBeforeKey;
            return true;
        }
        p.data.resize(p.data.size() & ~static_cast<size_t>(3));   // whole stereo frames
        if (p.data.empty())
            return true;
    }
    outBytes_ += p.data.size();
    out_.push_back(std::move(p));
    syncOutDepth();
    return true;
}

OwenbPlaySession::Push OwenbPlaySession::pushWithBackoff(const std::function<PushResult()>& f) {
    for (;;) {
        const PushResult r = f();
        if (r == PushResult::Ok)
            return Push::Ok;
        if (r == PushResult::Full) {
            { std::lock_guard<std::mutex> g(statsMu_); ++stats_.fullWaits; }
            if (interruptPending())
                return Push::Interrupted;
            sleep(cfg_.backoffUs);
            updatePosition();
            if (end_ != EndReason::None)
                return Push::Fatal;
            continue;
        }
        if (r == PushResult::Desync) {
            endWith(EndReason::RingError, "record sequence desync");
            return Push::Fatal;
        }
        fatalFromRing("write");
        return Push::Fatal;
    }
}

OwenbPlaySession::Push OwenbPlaySession::pushVideo(const MediaPacket& p, bool eos) {
    if (flushBeforeKey_) {
        // A held seek: a new stream on the ring (the core drops the old queue), then its keyframe.
        if (ring_.flush(sid_) != ControlResult::Ok) {
            fatalFromRing("flush");
            return Push::Fatal;
        }
        clock_.noteFlush(nowMs());
        flushBeforeKey_ = false;
        std::lock_guard<std::mutex> g(statsMu_);
        ++stats_.flushes;
    }
    h264stream::AccessUnitView au;
    au.session_id = sid_;
    au.annexb = p.data.data();
    au.len = p.data.size();
    au.pts90k = p.pts90k;
#ifndef MUT_OWENB_SESS_NO_EOS
    au.end_of_stream = eos;
#else
    (void)eos;
#endif
    const bool timeRewrite = entropyCabac_ == 0;   // v2-bugfix: the library rewrites this AU inside the write
    const Push r = pushWithBackoff([&] {
        au.seq = seq_;
        if (!timeRewrite)
            return ring_.pushAccessUnit(au);
        const int64_t t0 = nowNs();
        const auto res = ring_.pushAccessUnit(au);
        rewriteNs_ += nowNs() - t0;
        if (au.pts90k >= 0) {   // BUG1-CAP: the media time these writes covered
            if (rwFirstPts_ < 0 || au.pts90k < rwFirstPts_)
                rwFirstPts_ = au.pts90k;
            rwLastPts_ = std::max(rwLastPts_, au.pts90k);
        }
        return res;
    });
    if (r != Push::Ok)
        return r;
    ++seq_;
    if (clock_.basePts < 0 && p.pts90k >= 0) {
        int64_t landing = 0;
        const bool commit = clock_.noteFirstAu(p.pts90k, landing, nowMs());
        const uint32_t ep = clock_.epoch;
        if (commit && ep == seekEpoch_.load(std::memory_order_acquire)) {
            publishMedia(ep, 1, clock_.mediaMs);
            const int64_t shown = clock_.reportedMs();
            publishPosition(ep, 1, shown);
            if (clock_.holdTargetMs >= 0)
                logLine("seek clock: picture at " + std::to_string(landing) + " ms, bar held at " +
                        std::to_string(shown) + " ms");
            else
                logLine("seek clock: position follows the landing keyframe at " + std::to_string(landing) + " ms");
        }
    }
    if (corePaused_ && !userPaused_ && ring_.resume(sid_) == ControlResult::Ok)
        corePaused_ = false;   // the held seek's keyframe is queued: play on
    std::lock_guard<std::mutex> g(statsMu_);
    ++stats_.videoAus;
    if (entropyCabac_ == 0)
        ++stats_.cavlcAus;
    if (au.end_of_stream)
        stats_.eosSent = true;
    return Push::Ok;
}

// PCM in records of at most pcmRecordBytes, each with its own PTS (in-core A/V sync). Progress is
// kept in audioOffset_, so a write interrupted by a full ring continues where it stopped.
OwenbPlaySession::Push OwenbPlaySession::pushAudio(const MediaPacket& p) {
    const size_t rec = cfg_.pcmRecordBytes ? (cfg_.pcmRecordBytes & ~3u) : 3840u;
    while (audioOffset_ < p.data.size()) {
        const size_t n = std::min(rec ? rec : 4, p.data.size() - audioOffset_);
        h264stream::PcmView v;
        v.session_id = sid_;
        v.s16le = p.data.data() + audioOffset_;
        v.len = n;
#ifndef MUT_OWENB_SESS_PCM_NO_PTS
        // 48 kHz frames to 90 kHz: x 1.875 = x 15 / 8.
        v.pts90k = p.pts90k < 0 ? h264stream::kNoPts90k
                                : p.pts90k + static_cast<int64_t>(audioOffset_ / 4) * 15 / 8;
#else
        v.pts90k = h264stream::kNoPts90k;   // mutant: records follow each other, no anchor
#endif
        const Push r = pushWithBackoff([&] { return ring_.pushPcm(v); });
        if (r != Push::Ok)
            return r;
        audioOffset_ += n;
        std::lock_guard<std::mutex> g(statsMu_);
        ++stats_.audioRecords;
        stats_.audioBytes += n;
    }
    audioOffset_ = 0;
    return Push::Ok;
}

// Writes out_ in container order. Without all, the last video AU waits for the next one (or the end
// of input), so the stream's last AU can carry END_OF_STREAM. With all (end of input), the audio
// queued behind the last AU goes first, so END_OF_STREAM is the session's last record.
bool OwenbPlaySession::sendReady(bool all) {
    while (!out_.empty()) {
        if (interruptPending())
            return false;
        MediaPacket& f = out_.front();
        if (f.kind == MediaPacket::Kind::Video && videoInOut_ == 1 && !all) {
            // One video AU of lookahead so the real tail can carry END_OF_STREAM.
            // Past the byte cap that hold would pin every audio packet behind it:
            // send this AU without EOS so the queue can drain. EOF still marks the tail.
#ifndef MUT_OWENB_SESS_UNBOUNDED_OUT
            if (outBytes_ < kOutByteCap)
#endif
                return true;
        }
        if (all && f.kind == MediaPacket::Kind::Video && videoInOut_ == 1) {
            while (out_.size() > 1) {   // the trailing audio, then the last AU with END_OF_STREAM
                const size_t n = out_[1].data.size();
                const Push r = pushAudio(out_[1]);
                if (r != Push::Ok)
                    return false;
                outBytes_ = n > outBytes_ ? 0 : outBytes_ - n;
                out_.erase(out_.begin() + 1);
                syncOutDepth();
            }
        }
        MediaPacket& head = out_.front();   // re-bound: the erase above moved the deque
        const bool video = head.kind == MediaPacket::Kind::Video;
        const size_t n = head.data.size();
        const Push r = video ? pushVideo(head, all && videoInOut_ == 1) : pushAudio(head);
        if (r != Push::Ok)
            return false;
        if (video)
            --videoInOut_;
        outBytes_ = n > outBytes_ ? 0 : outBytes_ - n;
        out_.pop_front();
        syncOutDepth();
    }
    return true;
}

// End of input, everything written: true once the core has taken every record (or the drain timed
// out). A paused core does not count against the timeout.
bool OwenbPlaySession::drainToEnd() {
    const CoreStatus cs = ring_.coreStatus();
    if (!cs.valid || cs.ringLevel == 0)
        return true;
    if (!corePaused_) {
        drainStartUs_ += cfg_.idleUs;
        drainTotalUs_ += cfg_.idleUs;
    }
    // D8: the core can be tens of seconds behind the push (PMS transcode ahead of a slow core). A fixed
    // wait dropped the tail (the stopped timeline stuck at 89.9 %, so the item was never marked watched).
    // Now: the stall clock restarts whenever the core releases ring bytes (the STC is not used: a clock that keeps running after the end must not extend the drain). Only a core that
    // makes no progress for drainTimeoutMs, or a total past drainMaxMs, ends the play early.
    const bool progress = cs.ringLevel < drainLevel_;   // bytes the core released since the last poll
    drainLevel_ = cs.ringLevel;
#ifndef MUT_OWENB_DRAIN_FIXED
    if (progress)
        drainStartUs_ = 0;
#endif
    const uint64_t stallUs = static_cast<uint64_t>(cfg_.drainTimeoutMs) * 1000u;
    const uint64_t capUs = static_cast<uint64_t>(cfg_.drainMaxMs) * 1000u;
#ifdef MUT_OWENB_DRAIN_NO_STALL
    if (false)
#else
    if (drainStartUs_ >= stallUs)
#endif
    {
        logLine("end of input: the core did not take the tail in time (no progress for " +
                std::to_string(cfg_.drainTimeoutMs) + " ms); ending anyway");
        return true;
    }
#ifndef MUT_OWENB_DRAIN_NO_CAP
    if (drainTotalUs_ >= capUs) {
        logLine("end of input: the core did not take the tail in time (cap " + std::to_string(cfg_.drainMaxMs) +
                " ms); ending anyway");
        return true;
    }
#endif
    sleep(cfg_.idleUs);
    return false;
}

void OwenbPlaySession::finish() {
    // Stop and end of input drop a short-forward hold. Pause never reaches here.
    // The bar goes back to the picture so a target past the landing does not stick.
    if (clock_.holdTargetMs >= 0) {
        clock_.clearHold();
        const uint32_t ep = clock_.epoch;
        if (ep == seekEpoch_.load(std::memory_order_acquire)) {
            publishMedia(ep, 2, clock_.mediaMs);
            publishPosition(ep, 2, clock_.mediaMs);
        }
    }
    const EndReason r = end_;
    if (r == EndReason::CoreLost || r == EndReason::RingError) {
        ring_.release(sid_);   // the core is gone or in ERROR: nothing more to write
        return;
    }
    if (!cfg_.holdLastPicture) {
        ring_.flush(sid_);     // IDLE_SCREEN=black: drop the queue, black screen
        ring_.end(sid_);
        return;
    }
    if (r == EndReason::Eof) {
        ring_.end(sid_);       // the END_OF_STREAM picture stays up
        return;
    }
    // Stop, a refused stream or a source error: keep the current picture, play nothing more.
    if (!corePaused_)
        ring_.pause(sid_);
    corePaused_ = true;
    ring_.release(sid_);
}

EndReason OwenbPlaySession::run(const OpenRequest& req, uint64_t sessionId) {
    running_ = true;
    sid_ = sessionId;
    seq_ = 0;
    end_ = EndReason::None;
    out_.clear();
    syncOutDepth();
    outBytes_ = 0;
    videoInOut_ = 0;
    audioOffset_ = 0;
    needKey_ = true;
    flushBeforeKey_ = false;
    inputEof_ = false;
    srcBaseMs_ = req.startMs;
    srcFirstPts_ = srcLastPts_ = -1;
    resumeAtMs_.clear();
    netBackoff_.reset();
    netEpisode_ = false;
    netSeekTook_ = false;
    stallShown_ = false;
    corePaused_ = false;
    entropyCabac_ = -1;
    cavlcOverRateLogged_ = false;
    rateStartPts_ = -1;
    rateBytes_ = 0;
    rewriteNs_ = 0;
    rwFirstPts_ = rwLastPts_ = -1;
    drainStartUs_ = 0;
    drainTotalUs_ = 0;
    drainLevel_ = UINT32_MAX;
    seekEpoch_.store(1, std::memory_order_release);
    clock_.reset(req.startMs);
    packed_.store(0, std::memory_order_release);
    mediaPacked_.store(0, std::memory_order_release);
    publishMedia(1, 0, req.startMs);
    publishPosition(1, 0, req.startMs);
    currentReq_ = req;
    shownSubtitle_.clear();
    started_ = false;
    decodedPic_ = false;
    picsAtOpenSet_ = false;
    firstPicLogged_ = false;
    noPicWarned_ = false;
    userPaused_ = false;
    { std::lock_guard<std::mutex> g(statsMu_); stats_ = SessionStats{}; }

    int64_t playedMs = req.startMs;
    if (!openSource(req, &playedMs)) {
        running_ = false;
        return end_;
    }
    {
        int64_t origin = h264stream::kNoPts90k;
        std::lock_guard<std::mutex> g(srcMu_);
        if (src_)
            origin = src_->startPts90k();
        clock_.arm(1, req.containerSeek, origin);
    }
    if (playedMs != req.startMs) {
        clock_.baseMs = playedMs;
        publishPosition(1, 0, playedMs);
    }
    if (ring_.begin(sid_) != ControlResult::Ok) {
        fatalFromRing("begin");
        std::lock_guard<std::mutex> g(srcMu_);
        src_.reset();
        running_ = false;
        return end_;
    }
    if (volume_ != 100)
        ring_.setAttenuation(sid_, attenuationForVolume(volume_));
    logLine("playing " + req.url + " from " + std::to_string(req.startMs) + " ms");

    while (end_ == EndReason::None) {
        if (cfg_.onIteration)
            cfg_.onIteration();
        if (!takeCommands())
            break;
        updatePosition();
        if (end_ != EndReason::None)
            break;
#ifndef MUT_OWENB_SESS_PAUSE_READS
        // Paused: nothing is read or written (a held seek still reads to its keyframe).
        if (userPaused_ && !flushBeforeKey_ && !needKey_) {
            sleep(cfg_.idleUs);
            continue;
        }
#endif
        if (inputEof_) {
            if (!sendReady(true))
                continue;   // interrupted (a command) or ended
            if (drainToEnd())
                endWith(EndReason::Eof, "");
            continue;
        }
#ifndef MUT_OWENB_SESS_IGNORE_SENDREADY
        // A false return is an interrupt or a fatal push, not "keep reading".
        // Reading again is how out_ grew at the input rate during command spam.
        if (!sendReady(false)) {
            if (end_ != EndReason::None)
                break;
            sleep(cfg_.backoffUs);
            continue;
        }
#else
        sendReady(false);
#endif
#ifndef MUT_OWENB_SESS_UNBOUNDED_OUT
        if (outBytes_ >= kOutByteCap) {
            sleep(cfg_.backoffUs);
            continue;
        }
#endif
        if (!readOne())
            break;
    }
    finish();
    {
        std::lock_guard<std::mutex> g(srcMu_);
        src_.reset();
    }
    running_ = false;
    {
        const int64_t t = nowMs();
        const int64_t st = stopAtMs_.load();
        logLine(std::string("session ended: ") + toString(end_) + " ended t=" + std::to_string(t) +
                " dt_since_stop=" + (st >= 0 ? std::to_string(t - st) : std::string("-")));
    }
    return end_;
}

size_t OwenbPlaySession::readyDepth() const {
    std::lock_guard<std::mutex> g(srcMu_);
    return src_ ? src_->readyDepth() : 0;
}

unsigned OwenbPlaySession::probeBound() const {
    std::lock_guard<std::mutex> g(srcMu_);
    return src_ ? src_->probePackets() : 0;
}

} // namespace misterplex::owenb
