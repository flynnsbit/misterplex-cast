#pragma once

#include "owenb_session.hpp"
#include "remote_h264_transcode.hpp"

#include <chrono>
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
};

inline bool spelunkrCoreFits(const IMediaSource::VideoFormat& f) {
    if (f.width <= 0 || f.height <= 0 || f.fpsNum <= 0 || f.fpsDen <= 0)
        return false;
    const int64_t mbs = ((int64_t(f.width) + 15) / 16) * ((int64_t(f.height) + 15) / 16);
    if (mbs > kRemoteDirectMaxMbs)
        return false;
    return mbs * f.fpsNum <= int64_t(kRemoteDirectMaxMbps) * f.fpsDen;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs);
    auto active = std::make_shared<std::atomic<bool>>(true);
    // The source retains this interrupt after handoff; disarm the job's borrowed
    // cancellation callback before returning it to the caller.
    req.cancelled = [active, cancelled, deadline] {
        return active->load() && ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline);
    };
    auto interrupted = [&] {
        if (cancelled && cancelled()) {
            err = "prefetch cancelled";
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
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
    auto source = factory(req, err);
    if (interrupted())
        return {};
    if (!source) {
        if (err.empty())
            err = "prefetch open failed";
        return {};
    }
    auto capsOk = [&] {
        if (!limits.requireCoreCaps)
            return true;
        const auto f = source->videoFormat();
        if (spelunkrCoreFits(f))
            return true;
        err = "unsupported resolution " + std::to_string(f.width) + "x" + std::to_string(f.height) +
              " fps=" + std::to_string(f.fpsNum) + "/" + std::to_string(f.fpsDen);
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
