#pragma once

#include "owenb_session.hpp"
#include "remote_h264_transcode.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace misterplex::owenb {

struct PrefetchLimits {
#ifdef MUT_OWENB_PREFETCH_HARD_TIMEOUT
    int64_t timeoutMs = 5000;
#else
    int64_t timeoutMs = 15000;
#endif
    size_t maxBytes = 4u * 1024u * 1024u;
    unsigned maxPackets = 512;
    bool requireCoreCaps = false; // Spelunkr only; other callers retain their existing policy.
    uint64_t logGen = 0;
    int logAttempt = 0;
    // browse-lane #663 follow-up: a live, externally-bumped extension to this call's own deadline.
    // The caller's cancelled() can restart its own zap window "in place" while a productive
    // remainder fetch is mid-open instead of aborting it; without this, prefetchInput's own
    // deadline (fixed at call time below) would still fire on its own clock and cancel the
    // same open regardless of what cancelled() returns. nullptr (the default) is a no-op --
    // every existing caller is unaffected.
    std::shared_ptr<std::atomic<int64_t>> extraBudgetMs;
    // browse-lane #663 follow-up (P2-2 dial500): tried once this call's own deadline (baseDeadline +
    // extraBudgetMs, above) is reached, before declaring a timeout -- a chance for the caller
    // to restart its zap window in place (the same decision cancelled() makes on its own,
    // later, window expiry) without waiting for that later expiry to arrive, which would
    // otherwise cost a productive open its life a second early. A successful call is expected
    // to have already bumped extraBudgetMs. nullptr (the default) is a no-op -- every existing
    // caller is unaffected.
    std::function<bool()> extendOnProgress;
};

// Glass on  lab device must set MPX_SPELUNKR_STAGE=1 (or SPELUNKR_STAGE=1 / MPX_DEBUG=1)
// so the prefetch open_ms / idr_ms split is logged. Default is silent (browse-lane #647 P2-6).
inline bool spelunkrStageLogWanted() {
    auto on = [](const char* name) {
        const char* e = std::getenv(name);
        if (!e || !*e)
            return false;
        return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' ||
               e[0] == 't' || e[0] == 'T' ||
               std::strcmp(e, "on") == 0 || std::strcmp(e, "ON") == 0;
    };
    return on("MPX_SPELUNKR_STAGE") || on("SPELUNKR_STAGE") || on("MPX_DEBUG");
}

inline bool spelunkrFormatKnown(const IMediaSource::VideoFormat& f) {
    return f.width > 0 && f.height > 0 && f.fpsNum > 0 && f.fpsDen > 0;
}

inline bool spelunkrCoreFits(const IMediaSource::VideoFormat& f) {
    if (!spelunkrFormatKnown(f))
        return false;
    const int64_t mbs = ((int64_t(f.width) + 15) / 16) * ((int64_t(f.height) + 15) / 16);
    if (mbs > kRemoteDirectMaxMbs)
        return false;
    return mbs * f.fpsNum <= int64_t(kRemoteDirectMaxMbps) * f.fpsDen;
}

// Glass 85629316 zap4: remux SIGKILL with codecpar still 0x0/fps 0 labeled the
// stall "unsupported resolution 0x0 fps=0/0", which main treated as a hard
// caps reject (NO NEXT, no Pluto remux retry). Unknown format is a remux
// stall. Real oversize (1920x1080 …) stays "unsupported resolution".
// MUT_LIVE_NO_NONEXT_RETRY: keep the old 0x0 caps-reject string (tests FAIL).
inline std::string spelunkrCapsRejectWhy(const IMediaSource::VideoFormat& f) {
#ifdef MUT_LIVE_NO_NONEXT_RETRY
    return "unsupported resolution " + std::to_string(f.width) + "x" + std::to_string(f.height) +
           " fps=" + std::to_string(f.fpsNum) + "/" + std::to_string(f.fpsDen);
#else
    if (!spelunkrFormatKnown(f))
        return "unknown resolution (remux stall)";
    return "unsupported resolution " + std::to_string(f.width) + "x" + std::to_string(f.height) +
           " fps=" + std::to_string(f.fpsNum) + "/" + std::to_string(f.fpsDen);
#endif
}

// One opened demux, not a second ring writer. Ownership passes to the play thread.
class PrefetchedSource final : public IMediaSource {
public:
    PrefetchedSource(std::unique_ptr<IMediaSource> source, std::deque<MediaPacket> packets)
        : source_(std::move(source)), packets_(std::move(packets)) {}

    SourceRead read(MediaPacket& p, std::string& err) override {
        if (packets_.empty())
            return source_->read(p, err);
        p = std::move(packets_.front());
        packets_.pop_front();
        return SourceRead::Packet;
    }
    void cancel() override { source_->cancel(); }
    bool containerAspect(int64_t& n, int64_t& d) const override { return source_->containerAspect(n, d); }
    VideoFormat videoFormat() const override { return source_->videoFormat(); }
    size_t readyDepth() const override { return packets_.size() + source_->readyDepth(); }
    unsigned probePackets() const override { return source_->probePackets(); }
    int64_t startPts90k() const override { return source_->startPts90k(); }
    int activeAudioIndex() const override { return source_->activeAudioIndex(); }
    bool audioDecoderOpen() const override { return source_->audioDecoderOpen(); }
    StreamSwitch selectAudioStream(int i, std::string& e) override {
        return source_->selectAudioStream(i, e);
    }
    StreamSwitch selectSubtitleStream(int i, std::string& e) override {
        return source_->selectSubtitleStream(i, e);
    }
    std::string activeSubtitle(int64_t ms) override { return source_->activeSubtitle(ms); }
    void setEmbeddedTextIdentity(const misterplex::PlexTextIdentity& id) override {
        source_->setEmbeddedTextIdentity(id);
    }
    void setStallWatch(misterplex::netresume::StallWatch w) override { source_->setStallWatch(std::move(w)); }
    int lastAvErr() const override { return source_->lastAvErr(); }
    int lastHttpStatus() const override { return source_->lastHttpStatus(); }

private:
    std::unique_ptr<IMediaSource> source_;
    std::deque<MediaPacket> packets_;
};

struct PrefetchedInput {
    std::mutex mu;
    std::unique_ptr<IMediaSource> source;
    IMediaSource::VideoFormat format;
    int64_t darNum = 0, darDen = 0;

    std::unique_ptr<IMediaSource> take() {
        std::lock_guard<std::mutex> lock(mu);
        return std::move(source);
    }
};

// timedOut permits a normal-start fallback; cancellation and other failures must not use it.
inline std::shared_ptr<PrefetchedInput> prefetchInput(const SourceFactory& factory, OpenRequest req,
                                                      const std::function<bool()>& cancelled,
                                                      std::string& err, PrefetchLimits limits = {},
                                                      bool* timedOut = nullptr) {
    err.clear();
    if (timedOut)
        *timedOut = false;
    // browse-lane #663 follow-up: baseDeadline is fixed at call time (as before); extraBudgetMs is a
    // live atomic the caller's cancelled() can bump (via limits.extraBudgetMs) to extend it in
    // place instead of letting this same clock re-cancel a productive in-flight open.
    const auto baseDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs);
    auto extraBudgetMs = limits.extraBudgetMs;
    auto currentDeadline = [baseDeadline, extraBudgetMs] {
        const int64_t extra = extraBudgetMs ? extraBudgetMs->load(std::memory_order_relaxed) : 0;
        return baseDeadline + std::chrono::milliseconds(extra);
    };
    // browse-lane #663 follow-up (P2-2 dial500): this call's own deadline (baseDeadline, above) is
    // pinned inside the caller's wider zap window by design (the 7 s prefetch slice sits
    // about 1 s short of the caller's 8 s window) so a hung/no-progress open still dies on it
    // on schedule. A PRODUCTIVE open must not die on this same clock a full second before the
    // caller's own window-expiry restart logic (cancelled(), gated on the wider window's own
    // later expiry) ever gets a chance to run -- by the time that logic finally does restart,
    // the open underneath it has already been aborted by this earlier deadline (the "warm
    // in-place window restart" log still fires, but too late). limits.extendOnProgress -- when
    // set -- mirrors the caller's same restart decision (progress + its own restart cap) but is
    // reachable from this earlier clock too; a successful extension bumps the same
    // extraBudgetMs atom currentDeadline() above reads, moving it forward before the next poll.
    // nullptr (the default) is a no-op -- every existing caller is unaffected and this clock
    // still fires exactly as before.
    auto extendOnProgress = limits.extendOnProgress;
    auto deadlineExceeded = [currentDeadline, extendOnProgress] {
        if (std::chrono::steady_clock::now() < currentDeadline())
            return false;
        if (extendOnProgress && extendOnProgress())
            return std::chrono::steady_clock::now() >= currentDeadline();
        return true;
    };
    auto active = std::make_shared<std::atomic<bool>>(true);
    // The source retains this interrupt after handoff; disarm the job's borrowed
    // cancellation callback before returning it to the caller. deadlineExceeded() (not a bare
    // currentDeadline() compare) is what the AVIOInterruptCB deep inside avformat_open_input
    // actually polls -- the real site of the "open: stopped" abort this follows up on.
    req.cancelled = [active, cancelled, deadlineExceeded] {
        return active->load() && ((cancelled && cancelled()) || deadlineExceeded());
    };
    auto interrupted = [&] {
        if (cancelled && cancelled()) {
            err = "prefetch cancelled";
            return true;
        }
        if (deadlineExceeded()) {
            err = "prefetch timed out";
#ifndef MUT_OWENB_PREFETCH_HARD_TIMEOUT
            if (timedOut)
                *timedOut = true;
#endif
            return true;
        }
        return false;
    };
    if (interrupted())
        return {};
    req.attemptTimeoutMs = limits.timeoutMs;
    req.attemptExtraBudgetMs = limits.extraBudgetMs;
    const auto tPf = std::chrono::steady_clock::now();
    auto pfMs = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tPf).count();
    };
    auto source = factory(req, err);
    if (interrupted())
        return {};
    if (!source) {
        if (err.empty())
            err = "prefetch open failed";
        return {};
    }
    if (spelunkrStageLogWanted())
        std::fprintf(stderr, "media: prefetch gen=%llu attempt=%d open_ms=%lld\n",
                     static_cast<unsigned long long>(limits.logGen), limits.logAttempt,
                     static_cast<long long>(pfMs()));
    auto capsOk = [&] {
        if (!limits.requireCoreCaps)
            return true;
        const auto f = source->videoFormat();
        if (spelunkrCoreFits(f))
            return true;
        err = spelunkrCapsRejectWhy(f);
        return false;
    };
    if (!capsOk())
        return {};
    std::deque<MediaPacket> packets;
    size_t bytes = 0;
    unsigned video = 0;
    for (unsigned n = 0; n < limits.maxPackets;) {
        if (interrupted())
            return {};
        MediaPacket p;
        const auto read = source->read(p, err);
        if (interrupted())
            return {};
        if (read == SourceRead::Again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (read == SourceRead::Eof)
            break;
        if (read == SourceRead::Error) {
            if (err.empty())
                err = "prefetch read failed";
            return {};
        }
        ++n;
        if (p.data.size() > limits.maxBytes - bytes) {
            err = "prefetch byte budget exceeded";
            return {};
        }
        bytes += p.data.size();
        if (video == 0 && (p.kind != MediaPacket::Kind::Video || !p.keyframe))
            continue;
        if (p.kind == MediaPacket::Kind::Video)
            ++video;
        packets.push_back(std::move(p));
        // The session has one video AU of lookahead. Include its successor so
        // pushing the IDR at flip does not need another network read.
        if (video == 2)
            break;
    }
    if (video == 0) {
        err = "prefetch ended before IDR (EOF or packet budget)";
        return {};
    }
    if (spelunkrStageLogWanted())
        std::fprintf(stderr, "media: prefetch gen=%llu attempt=%d idr_ms=%lld video=%u bytes=%zu\n",
                     static_cast<unsigned long long>(limits.logGen), limits.logAttempt,
                     static_cast<long long>(pfMs()), video, bytes);
    if (interrupted())
        return {};
    if (!capsOk())
        return {};
    auto ready = std::make_shared<PrefetchedInput>();
    ready->format = source->videoFormat();
    source->containerAspect(ready->darNum, ready->darDen);
    ready->source = std::make_unique<PrefetchedSource>(std::move(source), std::move(packets));
    active->store(false);
    return ready;
}

} // namespace misterplex::owenb
