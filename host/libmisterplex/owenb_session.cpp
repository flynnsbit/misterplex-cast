// OWENB-P1B T5: the OB play loop (owenb_session.hpp). Writes the owenb ring only (OwenbRing); never
// touches the idle framebuffer (owenb_session.hpp) and never paints DDR.
#include "owenb_session.hpp"
#include "../../arm/misterplexd/gcast/open_retry.hpp"
#include "../../arm/misterplexd/gcast/yt_403.hpp"
#include "../../arm/misterplexd/log_redact.hpp"
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
        cfg_.log("[owenb-play] " + misterplex::redactUrlTokens(s));
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
    std::lock_guard<std::mutex> g(cmdMu_);
    cmds_.push_back(Cmd{Cmd::K::Seek, req, 0});
    ++pendingCmds_;
}

void OwenbPlaySession::setVolumePercent(int volume) {
    std::lock_guard<std::mutex> g(cmdMu_);
    cmds_.push_back(Cmd{Cmd::K::Volume, {}, volume});
    ++pendingCmds_;
}

void OwenbPlaySession::stop() {
    stopReq_ = true;
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
    auto once = [&](const OpenRequest& r, std::string& err) {
        err.clear();
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
    if (!req.retryTransientOpen) {
        std::string err;
        auto s = onceWith403(req, err);
        if (!s) {
            endWith(EndReason::SourceError,
                    "open " + misterplex::redactUrlTokens(req.url) + ": " + (err.empty() ? "failed" : err));
            return false;
        }
        if (playedMs)
            *playedMs = req.startMs;
        return adopt(std::move(s));
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
    return adopt(std::move(held));
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
            if (!applySeek(c.req))
                return false;
            break;
        case Cmd::K::Stop:
            endWith(EndReason::Stopped, "");
            return false;
        case Cmd::K::Volume:
            volume_ = c.volume;
            ring_.setAttenuation(sid_, attenuationForVolume(volume_));
            break;
        }
    }
    return true;
}

bool OwenbPlaySession::applySeek(const OpenRequest& req) {
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
    baseMs_ = playedMs;
    positionMs_ = playedMs;
    needKey_ = true;
#ifndef MUT_OWENB_SESS_SEEK_NO_FLUSH
    flushBeforeKey_ = true;
#else
    flushBeforeKey_ = false;   // mutant: the old stream's queue is not dropped
#endif
    inputEof_ = false;
    drainStartUs_ = 0;
    baseMs_ = req.startMs;
    basePts_ = -1;
    rateStartPts_ = -1;
    rateBytes_ = 0;
    rewriteNs_ = 0;
    rwFirstPts_ = rwLastPts_ = -1;
    positionMs_ = req.startMs;
    return true;
}

void OwenbPlaySession::updatePosition() {
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
    if (!(cs.live & H264RING_L_STC_RUN) || cs.stc90k < 0 || basePts_ < 0)
        return;
    constexpr int64_t kMask = (int64_t{1} << 33) - 1;
    int64_t d = (cs.stc90k - basePts_) & kMask;
    if (d > (int64_t{1} << 32))
        d = 0;   // the clock is just behind the first picture's PTS
    positionMs_ = baseMs_ + d / 90;
    if (!started_) {
        started_ = true;
        logLine("first picture (STC running)");
        if (onFirstPicture)
            onFirstPicture();
    }
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
            const bool overLoad = true;   // mutant: the nominal rate alone ends the play (a lab MiSTer bug 1)
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
        inputEof_ = true;
        return true;
    case SourceRead::Error:
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
    if (p.kind == MediaPacket::Kind::Video) {
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
    if (basePts_ < 0 && p.pts90k >= 0)
        basePts_ = p.pts90k;
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
    if (!corePaused_)
        drainStartUs_ += cfg_.idleUs;
    if (drainStartUs_ >= static_cast<uint64_t>(cfg_.drainTimeoutMs) * 1000u) {
        logLine("end of input: the core did not take the tail in time; ending anyway");
        return true;
    }
    sleep(cfg_.idleUs);
    return false;
}

void OwenbPlaySession::finish() {
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
    corePaused_ = false;
    entropyCabac_ = -1;
    cavlcOverRateLogged_ = false;
    rateStartPts_ = -1;
    rateBytes_ = 0;
    rewriteNs_ = 0;
    rwFirstPts_ = rwLastPts_ = -1;
    baseMs_ = req.startMs;
    basePts_ = -1;
    drainStartUs_ = 0;
    positionMs_ = req.startMs;
    started_ = false;
    userPaused_ = false;
    { std::lock_guard<std::mutex> g(statsMu_); stats_ = SessionStats{}; }

    int64_t playedMs = req.startMs;
    if (!openSource(req, &playedMs)) {
        running_ = false;
        return end_;
    }
    baseMs_ = playedMs;
    positionMs_ = playedMs;
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
    logLine(std::string("session ended: ") + toString(end_));
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
