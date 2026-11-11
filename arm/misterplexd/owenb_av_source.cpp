// OWENB-P1B T5: the OB play route's libav input (owenb_av_source.hpp).
#ifndef MPX_HAVE_LIBAV
#error "owenb_av_source.cpp requires -DMPX_HAVE_LIBAV"
#endif

#include "owenb_av_source.hpp"
#include "av_log_redact.hpp"
#include "av_open_opts.hpp"
#include "ca_bundle.hpp"
#include "http_range_avio.hpp"
#include "gcast/dash_seek.hpp"
#include "gcast/yt_open_budget.hpp"
#include "libmisterplex/net_resume.hpp"
#include "libmisterplex/embedded_text_map.hpp"
#include "libmisterplex/embedded_text_part.hpp"
#include "libmisterplex/soft_subs.hpp"
#include "libmisterplex/subtitle_cues.hpp"
#include "spelunkr/header_cache.hpp"
#include "spelunkr/header_cache_avio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace misterplex::owenb {
namespace {

// GATE7-1018: the codec-probe packet budget (libav default 2500); see AvSource::open.
constexpr const char* kMaxProbePackets = "64";
// Backpressure for a bsf/decoder flush. read() drains ready_ before the next pull.
constexpr size_t kReadyByteCap = 4u * 1024u * 1024u;

std::string avErr(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, b, sizeof b);
    return b;
}

class AvSource final : public IMediaSource {
public:
    ~AvSource() override {
        avcodec_free_context(&sdec_);
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        av_packet_free(&out_);
        av_bsf_free(&bsf_);
        avcodec_free_context(&adec_);
        swr_free(&swr_);
        av_packet_free(&vHold_);
        av_packet_free(&aHold_);
        if (fmtA_)
            avformat_close_input(&fmtA_);
        if (fmt_) {
            if (rangeIo_.attached() || headerIo_.attached())
                fmt_->pb = nullptr;
            avformat_close_input(&fmt_);
        }
        rangeIo_.reset();
        headerIo_.reset();
    }

    void cancel() override { abort_.store(true); }
    void openDone() { openDeadlineMs_.store(0); }   // the attempt cap covers the open only
    std::function<bool()> cancelled_;
    std::atomic<int64_t> openDeadlineMs_{0};
    bool openStopped_ = false;
    bool openCapped_ = false;
    int64_t startPts90k() const override { return startPts90k_; }
    bool containerAspect(int64_t& num, int64_t& den) const override {
        if (darNum_ <= 0 || darDen_ <= 0)
            return false;
        num = darNum_;
        den = darDen_;
        return true;
    }
    VideoFormat videoFormat() const override {
        if (!fmt_ || vidx_ < 0)
            return {};
        const AVStream *vs = fmt_->streams[vidx_];
        const AVRational rate = av_guess_frame_rate(fmt_, fmt_->streams[vidx_], nullptr);
        const auto order = vs->codecpar->field_order;
        return {vs->codecpar->width, vs->codecpar->height, rate.num, rate.den,
                order != AV_FIELD_UNKNOWN && order != AV_FIELD_PROGRESSIVE};
    }

    StreamSwitch selectAudioStream(int index, std::string& err) override {
        if (!fmt_) {
            err = "no input";
            return StreamSwitch::Failed;
        }
        if (fmtA_) {
            err = "separate audio input cannot switch in place";
            return StreamSwitch::Failed;
        }
        if (index == aidx_ && adec_)
            return StreamSwitch::Applied;
        if (index < 0 || index >= static_cast<int>(fmt_->nb_streams) ||
            fmt_->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            err = "no audio stream at index " + std::to_string(index);
            return StreamSwitch::Unsupported;
        }
        // Keep the open decoder until the new one succeeds, then fall back to it.
        swr_free(&swr_);
        AVCodecContext* previous = adec_;
        adec_ = nullptr;
        const int previousIndex = aidx_;
        if (previousIndex >= 0 && previousIndex < static_cast<int>(fmt_->nb_streams))
            fmt_->streams[previousIndex]->discard = AVDISCARD_ALL;
        aidx_ = index;
        fmt_->streams[aidx_]->discard = AVDISCARD_DEFAULT;
        audioPending_ = false;
        if (!openAudioDecoder()) {
            avcodec_free_context(&adec_);
            aidx_ = previousIndex;
            adec_ = previous;
            if (previousIndex >= 0 && previousIndex < static_cast<int>(fmt_->nb_streams))
                fmt_->streams[previousIndex]->discard = AVDISCARD_DEFAULT;
            if (index >= 0 && index < static_cast<int>(fmt_->nb_streams))
                fmt_->streams[index]->discard = AVDISCARD_ALL;
            err = "audio decoder did not open; kept the previous stream";
            std::fprintf(stderr, "misterplexd: %s (index %d)\n", err.c_str(), index);
            return StreamSwitch::Unsupported;
        }
        avcodec_free_context(&previous);
        return StreamSwitch::Applied;
    }

    // SOFT_SUBS off: a positive index is unsupported (PMS burn / skip). Off (-1)
    // is a no-op success: there is no text decoder, and the audio decoder stays.
    // SOFT_SUBS on: index is a container stream. -1 closes only the text decoder.
    //
    // Audio stops when adec_ is released. The selected index can stay, so an
    // index check alone still looks healthy. closeTextSubtitle() frees sdec_
    // and discards that stream. It does not touch adec_, aidx_, or swr_.
    StreamSwitch selectSubtitleStream(int index, std::string& err) override {
#ifdef MUT_SUB_OFF_DROPS_AUDIO
        if (index < 0) {
            // Index stays selected. PCM stops because the decoder is gone.
            avcodec_free_context(&adec_);
            swr_free(&swr_);
        }
#endif
        if (!misterplex::packetTextDecodeAllowedForCodec(textHint_.codec)) {
            if (index < 0) {
                closeTextSubtitle();
                return StreamSwitch::Applied;
            }
            err = "subtitles are burned by PMS; this input does not decode them";
            return StreamSwitch::Unsupported;
        }
        if (!fmt_) {
            if (index < 0)
                return StreamSwitch::Applied;
            err = "no input";
            return StreamSwitch::Failed;
        }
        if (index < 0) {
            closeTextSubtitle();
            return StreamSwitch::Applied;
        }
        const int use = resolvedTextIndex(index);
        if (use < 0) {
            err = "opened input has no embedded text track";
            return StreamSwitch::Unsupported;
        }
        if (!armTextSubtitle(use, err))
            return StreamSwitch::Unsupported;
        return StreamSwitch::Applied;
    }

    void setEmbeddedTextIdentity(const misterplex::PlexTextIdentity& id) override { textHint_ = id; }

    std::string activeSubtitle(int64_t positionMs) override {
        if (!sdec_ || cues_.empty())
            return {};
        for (auto it = cues_.rbegin(); it != cues_.rend(); ++it) {
            if (positionMs >= it->startMs && positionMs < it->endMs) {
                if (it->line2.empty())
                    return it->line1;
                return it->line1 + "\n" + it->line2;
            }
        }
        return {};
    }

    int activeAudioIndex() const override { return aidx_; }
    bool audioDecoderOpen() const override { return adec_ != nullptr; }
    int lastAvErr() const override { return lastAvErr_; }
    int lastHttpStatus() const override { return lastHttp_; }
    void setStallWatch(misterplex::netresume::StallWatch w) override {
        stall_ = std::move(w);
        lastByteMs_ = stallNow();
        stallShown_ = false;
        stallAbort_ = false;
    }

    bool open(const OpenRequest& req, std::string& err) {
        av_log_set_level(AV_LOG_ERROR);
        misterplex::installRedactingAvLog();
        fmt_ = avformat_alloc_context();
        pkt_ = av_packet_alloc();
        out_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!fmt_ || !pkt_ || !out_ || !frame_) {
            err = "allocation failed";
            return false;
        }
        fmt_->interrupt_callback.callback = [](void* o) -> int {
            return static_cast<AvSource*>(o)->onInterrupt();
        };
        fmt_->interrupt_callback.opaque = this;
        cancelled_ = req.cancelled;
        if (req.attemptTimeoutMs > 0)
            openDeadlineMs_.store(stallNow() + req.attemptTimeoutMs);
        // GATE7-1018: bound the demuxer's codec probe. MPEG-TS probes the AAC stream (request_probe) and buffers
        // every packet until it settles; each buffered video packet pins a whole 200 KiB PES buffer while the
        // buffer limit counts only payload bytes. With the ARM static-safe libav (no ADTS demuxer to settle the
        // probe) a GCAST yt.ts open held ~350 MB of heap after avformat_find_stream_info (2500 probe packets;
        // OOM in the rc3 G7 soak). 64 packets settle every stream the OB plays (same codecpar as 2500).
        probePackets_ = 64;
        auto applyOpenOpts = [&](AVDictionary** dst) {
            if (!req.headers.empty()) {
                std::string h = req.headers;
                if (h.size() < 2 || h.compare(h.size() - 2, 2, "\r\n") != 0)
                    h += "\r\n";
                av_dict_set(dst, "headers", h.c_str(), 0);
            }
            // Remote PMS can be slow to the first byte; a helper /yt.ts first byte waits on a yt-dlp run
            // (gcast/yt_open_budget.hpp, fr-yt110-1025).
            av_dict_set(dst, "rw_timeout", std::to_string(misterplex::gcast::openRwTimeoutUs(req.url)).c_str(), 0);
            av_dict_set(dst, "max_probe_packets", kMaxProbePackets, 0);
#ifndef MUT_AV_NO_PROBE_LIMITS
            // Payload and analyze caps next to the packet cap. fflags=nobuffer is not set:
            // it discards the packets the probe already read, so the first second and the
            // keyframe of every play are lost.
            av_dict_set(dst, "probesize", "500000", 0);
            av_dict_set(dst, "analyzeduration", "1000000", 0);
            av_dict_set(dst, "max_delay", "0", 0);
#endif
            av_dict_set(dst, "protocol_whitelist", misterplex::kAvProtocolWhitelist, 0);
            av_dict_set(dst, "tls_verify", "1", 0);
            const std::string ca = mpx::caBundlePath();
            if (!ca.empty())
                av_dict_set(dst, "ca_file", ca.c_str(), 0);
        };
        AVDictionary* o = nullptr;
        applyOpenOpts(&o);
        // P2 header (moov) cache: Spelunkr-only (req.headerCacheSize >= 0; Plex opens never set
        // it, so rangeIo_ keeps owning http:// direct Parts as before). archive.org is https with
        // a hostname, outside HttpRangeReader's http://+numeric-IP scope, so this is a parallel
        // custom AVIO keyed on (url, size, etag) rather than a reuse of rangeIo_ itself.
        const bool wantHeaderCache = req.headerCacheSize >= 0;
        bool usedHeaderIo = false;
        misterplex::spelunkr::HeaderCacheKey headerKey;
        const auto openT0 = std::chrono::steady_clock::now();
        if (wantHeaderCache) {
            if (!headerStore_)
                headerStore_ = std::make_unique<misterplex::spelunkr::HeaderCacheStore>();
            headerKey = {req.url, req.headerCacheSize, req.headerCacheEtag};
            usedHeaderIo = headerIo_.attach(fmt_, headerKey, req.headers, *headerStore_,
                                            [this] { return onInterrupt() != 0; });
        }
        // #223: a direct Part over http:// uses custom AVIO so a short
        // Content-Length close resumes with Range: bytes=<offset>-.
        // Custom AVIO never runs fmt->interrupt_callback. Poll onInterrupt so
        // NET_RESUME stall show/abort and a user stop still fire.
        const std::string openName =
            usedHeaderIo ? misterplex::httprange::probeName(req.url)
            : rangeIo_.attach(fmt_, req.url, req.headers, [this] { return onInterrupt() != 0; })
                ? misterplex::httprange::probeName(req.url)
                : req.url;
        const int rc = avformat_open_input(&fmt_, openName.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (usedHeaderIo) {
            const auto& hs = headerIo_.stats();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openT0).count();
            std::fprintf(stderr,
                         "misterplexd: spelunkr header-cache %s moov_offset=%lld moov_size=%lld "
                         "bytes_read=%lld cached_bytes=%lld live_bytes=%lld open_ms=%.1f rc=%d\n",
                         hs.cacheHit ? "hit" : "miss", (long long)hs.moovOffset, (long long)hs.moovSize,
                         (long long)(fmt_ && fmt_->pb ? fmt_->pb->bytes_read : 0),
                         (long long)hs.cachedBytesServed, (long long)hs.liveBytesRead, ms, rc);
        }
        if (rc < 0) {
            if (usedHeaderIo && headerIo_.stats().cacheHit && headerStore_)
                // P2-2: open itself failed on a cache hit (a stale/poisoned entry, or a hit
                // whose live gap read failed). Delete it so the next open is a clean miss.
                headerStore_->remove(headerKey);
            err = openStopped_ ? std::string("open: stopped")
                  : openCapped_ ? "open: Connection timed out (attempt cap " + std::to_string(req.attemptTimeoutMs) + " ms)"
                                : "open: " + avErr(rc);
            return false;
        }
        // Flag off: discard subtitle streams before the probe so the new text
        // decoders are not opened during find_stream_info. Flag on keeps the
        // selected text stream. Audio and video are unchanged.
        textHint_.plexIndex = req.textSubtitleIndex;
        textHint_.codec = req.textCodec;
        textHint_.language = req.textLanguage;
        textHint_.title = req.textTitle;
        textHint_.forced = req.textForced;
        textHint_.textOrdinal = req.textOrdinal;
        {
            // Keep subtitle streams until find_stream_info; the PMS index is
            // remapped onto this file afterwards.
            const bool keepText = misterplex::packetTextDecodeAllowedForCodec(req.textCodec) &&
                                  req.textSubtitleIndex >= 0;
            for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
                AVStream* st = fmt_->streams[i];
                if (!st->codecpar || st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
                    continue;
                st->discard = keepText ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
            }
        }
        int r = avformat_find_stream_info(fmt_, nullptr);
        if (usedHeaderIo) {
            // Persist whatever header bytes were actually touched (a miss, or a hit with a gap)
            // before any seek, so the next open of this (url, size, etag) is a full hit --
            // unless find_stream_info itself failed (P2-2: a failed probe's bytes are not a
            // good header, and a cache hit that failed is deleted rather than persisted).
            headerIo_.finalizeHeader(r >= 0);
            const auto& hs = headerIo_.stats();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openT0).count();
            std::fprintf(stderr,
                         "misterplexd: spelunkr header-cache find_stream_info moov_offset=%lld "
                         "bytes_read=%lld elapsed_ms=%.1f r=%d\n",
                         (long long)hs.moovOffset, (long long)(fmt_ && fmt_->pb ? fmt_->pb->bytes_read : 0), ms, r);
            if (req.headerCacheStatsOut) {
                req.headerCacheStatsOut->cacheHit = hs.cacheHit;
                req.headerCacheStatsOut->cachedBytesServed = hs.cachedBytesServed;
                req.headerCacheStatsOut->liveBytesRead = hs.liveBytesRead;
            }
        }
        if (r < 0) {
            err = "stream info: " + avErr(r);
            return false;
        }
        vidx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidx_ < 0) {
            err = "no video stream";
            return false;
        }
        AVStream* vs = fmt_->streams[vidx_];
        // Media time 0. fmt start_time is AV_TIME_BASE; a raw .264 leaves it unset and the
        // session keeps the requested startMs (synthesised PTS restart at 0 after a seek).
        startPts90k_ = h264stream::kNoPts90k;
        // A negative start_time is a B-frame delay, not "unknown". AV_NOPTS_VALUE stays unknown.
        if (fmt_->start_time != AV_NOPTS_VALUE)
            startPts90k_ = originPts90k(fmt_->start_time, 1, AV_TIME_BASE);
        else if (vs->start_time != AV_NOPTS_VALUE && vs->time_base.num > 0 && vs->time_base.den > 0)
            startPts90k_ = originPts90k(vs->start_time, static_cast<uint32_t>(vs->time_base.num),
                                        static_cast<uint32_t>(vs->time_base.den));
        if (vs->codecpar->codec_id != AV_CODEC_ID_H264) {
            err = std::string("OB plays H.264 only (the input is ") + avcodec_get_name(vs->codecpar->codec_id) + ")";
            return false;
        }
        const AVBitStreamFilter* f = av_bsf_get_by_name("h264_mp4toannexb");
        if (!f || av_bsf_alloc(f, &bsf_) < 0 || avcodec_parameters_copy(bsf_->par_in, vs->codecpar) < 0) {
            err = "h264_mp4toannexb unavailable";
            return false;
        }
        bsf_->time_base_in = vs->time_base;
        if ((r = av_bsf_init(bsf_)) < 0) {
            err = "h264_mp4toannexb: " + avErr(r);
            return false;
        }
        // 0008: the container's display aspect (MP4 pasp, MKV display size), not the codec's VUI: the
        // daemon compares it with a VUI-less SPS (owenb_aspect.hpp owenbStreamAspect)
        if (vs->sample_aspect_ratio.num > 0 && vs->sample_aspect_ratio.den > 0 && vs->codecpar->width > 0 &&
            vs->codecpar->height > 0) {
            darNum_ = int64_t(vs->codecpar->width) * vs->sample_aspect_ratio.num;
            darDen_ = int64_t(vs->codecpar->height) * vs->sample_aspect_ratio.den;
        }
        AVRational fr = av_guess_frame_rate(fmt_, vs, nullptr);
        if (fr.num > 0 && fr.den > 0)
            frameTicks_ = av_rescale(90000, fr.den, fr.num);
        if (!req.audioUrl.empty()) {
            // Separate DASH audio. Open the URL as given and time-seek it.
            // Do not append a byte range: itag 140 from a byte offset is not a container.
            const gcast::DashSeekPlan plan = gcast::planDashSeek(
                gcast::DashOpen{req.url, req.audioUrl, req.startMs});
            if (plan.byteSeek || plan.audioUrl != req.audioUrl) {
                err = "audio seek would use a raw byte offset";
                return false;
            }
            AVDictionary* ao = nullptr;
            applyOpenOpts(&ao);
            fmtA_ = avformat_alloc_context();
            if (!fmtA_) {
                err = "allocation failed";
                av_dict_free(&ao);
                return false;
            }
            fmtA_->interrupt_callback = fmt_->interrupt_callback;
            const int arc = avformat_open_input(&fmtA_, plan.audioUrl.c_str(), nullptr, &ao);
            av_dict_free(&ao);
            if (arc < 0) {
                err = "open audio: " + avErr(arc);
                return false;
            }
            if ((r = avformat_find_stream_info(fmtA_, nullptr)) < 0) {
                err = "audio stream info: " + avErr(r);
                return false;
            }
            vHold_ = av_packet_alloc();
            aHold_ = av_packet_alloc();
            if (!vHold_ || !aHold_) {
                err = "allocation failed";
                return false;
            }
        }
        // A preferred index names a stream in the muxed file. Separate DASH audio
        // is its own input, so the preference does not apply there.
        openAudio(fmtA_ ? -1 : req.audioStreamIndex);
        // Plex id on subtitleStreamIndex is not a container index. Embedded text
        // uses textSubtitleIndex (SOFT_SUBS, or mov_text overlay #260).
        (void)req.subtitleStreamIndex;
#ifndef MUT_AV_NO_PROBE_LIMITS
        for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
            if (static_cast<int>(i) == vidx_ || (!fmtA_ && static_cast<int>(i) == aidx_))
                continue;
            if (misterplex::packetTextDecodeAllowedForCodec(req.textCodec) &&
                static_cast<int>(i) == resolvedTextIndex(req.textSubtitleIndex))
                continue;
            fmt_->streams[i]->discard = AVDISCARD_ALL;
        }
        if (fmtA_) {
            for (unsigned i = 0; i < fmtA_->nb_streams; ++i) {
                if (static_cast<int>(i) == aidx_)
                    continue;
                fmtA_->streams[i]->discard = AVDISCARD_ALL;
            }
        }
#endif
        if (misterplex::packetTextDecodeAllowedForCodec(req.textCodec) && req.textSubtitleIndex >= 0) {
            const int textAt = resolvedTextIndex(req.textSubtitleIndex);
            std::string subErr;
            if (textAt < 0)
                std::fprintf(stderr,
                             "misterplexd: opened input has no embedded text track (plex index %d)\n",
                             req.textSubtitleIndex);
            else if (!armTextSubtitle(textAt, subErr))
                std::fprintf(stderr, "misterplexd: soft subtitle not opened: %s\n", subErr.c_str());
        }
        if (req.containerSeek && req.startMs > 0) {
            // Absolute container time, not media time. seekInputToMs uses AV_TIME_BASE
            // and AVSEEK_FLAG_BACKWARD, so a start_time near 0 lands on the keyframe
            // the session rig models.
            const int64_t bytesBeforeSeek = (usedHeaderIo && fmt_ && fmt_->pb) ? fmt_->pb->bytes_read : 0;
            const auto seekT0 = std::chrono::steady_clock::now();
            if ((r = gcast::seekInputToMs(fmt_, req.startMs)) < 0) {
                err = "seek: " + avErr(r);
                return false;
            }
            if (usedHeaderIo) {
                // On a warm cache this is the single range read at the keyframe offset: the
                // header bytes themselves were already served from disk, so only the seek
                // itself touches the network.
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - seekT0).count();
                const int64_t bytesAfterSeek = fmt_ && fmt_->pb ? fmt_->pb->bytes_read : 0;
                std::fprintf(stderr,
                             "misterplexd: spelunkr header-cache seek start_ms=%lld "
                             "bytes_read=%lld seek_bytes=%lld elapsed_ms=%.1f\n",
                             (long long)req.startMs, (long long)bytesAfterSeek,
                             (long long)(bytesAfterSeek - bytesBeforeSeek), ms);
            }
            if (fmtA_ && (r = gcast::seekInputToMs(fmtA_, req.startMs)) < 0) {
                err = "audio seek: " + avErr(r);
                return false;
            }
        }
        return true;
    }

    size_t readyDepth() const override { return readyDepth_.load(); }
    unsigned probePackets() const override { return probePackets_; }

    SourceRead read(MediaPacket& p, std::string& err) override {
        for (;;) {
            if (!ready_.empty()) {
                p = std::move(ready_.front());
                readyBytes_ -= std::min(readyBytes_, p.data.size());
                ready_.pop_front();
                readyDepth_.store(ready_.size());
                return SourceRead::Packet;
            }
            if (videoPending_) {
                takeVideo();
                if (!ready_.empty())
                    continue;
            }
            if (audioPending_) {
                takeAudio();
                if (!ready_.empty())
                    continue;
            }
            if (drained_)
                return SourceRead::Eof;
            if (abort_.load() && !stallAbort_) {
                err = "cancelled";
                return SourceRead::Error;
            }
            if (fmtA_) {
                if (!pullDash(err))
                    return SourceRead::Error;
                continue;
            }
            const int r = av_read_frame(fmt_, pkt_);
            if (r == AVERROR_EOF) {
                av_bsf_send_packet(bsf_, nullptr);
                takeVideo();
                if (adec_) {
                    avcodec_send_packet(adec_, nullptr);
                    takeAudio();
                }
                drained_ = true;
                continue;
            }
            if (r < 0) {
                if (stallAbort_) {
                    stallAbort_ = false;
                    lastAvErr_ = misterplex::netresume::kStallAbortAv;
                    lastHttp_ = 0;
                    err = "stall";
                    return SourceRead::Error;
                }
                lastAvErr_ = r;
                err = abort_.load() ? "cancelled" : "read: " + avErr(r);
                lastHttp_ = httpStatusOf(r, err);
                return SourceRead::Error;
            }
            noteByte();
            if (pkt_->stream_index == vidx_) {
                if (av_bsf_send_packet(bsf_, pkt_) == 0)
                    takeVideo();
            } else if (adec_ && pkt_->stream_index == aidx_) {
                if (avcodec_send_packet(adec_, pkt_) == 0)
                    takeAudio();
            } else if (sdec_ && pkt_->stream_index == sidx_) {
                takeSubtitle(pkt_);
            }
            av_packet_unref(pkt_);
        }
    }

private:
    AVFormatContext* audioFmt() const { return fmtA_ ? fmtA_ : fmt_; }

    void openAudio(int preferred) {
        aidx_ = -1;
        AVFormatContext* af = audioFmt();
        if (!af)
            return;
        if (!fmtA_ && preferred >= 0 && preferred < static_cast<int>(af->nb_streams) &&
            af->streams[preferred]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            aidx_ = preferred;
        else
            aidx_ = av_find_best_stream(af, AVMEDIA_TYPE_AUDIO, -1, fmtA_ ? -1 : vidx_, nullptr, 0);
        if (aidx_ < 0)
            return;
        af->streams[aidx_]->discard = AVDISCARD_DEFAULT;
        if (!openAudioDecoder())
            aidx_ = -1;
    }

    bool openAudioDecoder() {
        if (aidx_ < 0)
            return false;
        AVStream* as = audioFmt()->streams[aidx_];
        const AVCodec* c = avcodec_find_decoder(as->codecpar->codec_id);
        if (!c || !(adec_ = avcodec_alloc_context3(c)) ||
            avcodec_parameters_to_context(adec_, as->codecpar) < 0 || avcodec_open2(adec_, c, nullptr) < 0) {
            avcodec_free_context(&adec_);   // video only: the core plays silence
            return false;
        }
        return true;
    }

    // One packet is always admitted so a single large AU cannot stall the bsf.
    // Further packets wait in the bsf/decoder until read() drains ready_.
    bool readyRoom() const {
#ifdef MUT_AV_READY_UNBOUNDED
        return true;
#else
        return ready_.empty() || readyBytes_ < kReadyByteCap;
#endif
    }

    void pushReady(MediaPacket m) {
        readyBytes_ += m.data.size();
        ready_.push_back(std::move(m));
        readyDepth_.store(ready_.size());
    }

    void takeVideo() {
        const AVRational tb = fmt_->streams[vidx_]->time_base;
        while (readyRoom() && av_bsf_receive_packet(bsf_, out_) == 0) {
            MediaPacket m;
            m.kind = MediaPacket::Kind::Video;
            m.data.assign(out_->data, out_->data + out_->size);
            m.keyframe = (out_->flags & AV_PKT_FLAG_KEY) != 0;
            const int64_t ts = out_->pts != AV_NOPTS_VALUE ? out_->pts : out_->dts;
            if (ts != AV_NOPTS_VALUE)
                m.pts90k = toPts90k(ts, static_cast<uint32_t>(tb.num), static_cast<uint32_t>(tb.den));
            if (m.pts90k < 0 && frameTicks_ > 0)   // a raw .264: the frame rate gives the clock
                m.pts90k = (videoFrames_ * frameTicks_) & ((int64_t{1} << 33) - 1);
            ++videoFrames_;
            av_packet_unref(out_);
            if (!m.data.empty())
                pushReady(std::move(m));
        }
        videoPending_ = readyBytes_ >= kReadyByteCap && !ready_.empty();
    }

    void takeAudio() {
        const AVRational tb = audioFmt()->streams[aidx_]->time_base;
        while (readyRoom() && adec_ && avcodec_receive_frame(adec_, frame_) == 0) {
            if (!swr_ && !openSwr())
                return;
            const int64_t ts = frame_->best_effort_timestamp;
            const int64_t pts = ts != AV_NOPTS_VALUE
                                    ? toPts90k(ts, static_cast<uint32_t>(tb.num), static_cast<uint32_t>(tb.den))
                                    : h264stream::kNoPts90k;
            const int outMax = swr_get_out_samples(swr_, frame_->nb_samples);
            MediaPacket m;
            m.kind = MediaPacket::Kind::Audio;
            m.data.resize(static_cast<size_t>(outMax > 0 ? outMax : 0) * 4u);
            uint8_t* dst = m.data.data();
            const int got = outMax > 0 ? swr_convert(swr_, &dst, outMax,
                                                     const_cast<const uint8_t**>(frame_->extended_data),
                                                     frame_->nb_samples)
                                       : 0;
            av_frame_unref(frame_);
            if (got <= 0)
                continue;
            m.data.resize(static_cast<size_t>(got) * 4u);
            m.pts90k = pts;
            pushReady(std::move(m));
        }
        audioPending_ = readyBytes_ >= kReadyByteCap && !ready_.empty();
    }

    bool openSwr() {
        AVChannelLayout outL = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout inL;
        if (frame_->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&inL, &frame_->ch_layout);
        else
            av_channel_layout_default(&inL, adec_->ch_layout.nb_channels > 0 ? adec_->ch_layout.nb_channels : 2);
        const int rc = swr_alloc_set_opts2(&swr_, &outL, AV_SAMPLE_FMT_S16, 48000, &inL,
                                           static_cast<AVSampleFormat>(frame_->format), frame_->sample_rate, 0,
                                           nullptr);
        av_channel_layout_uninit(&inL);
        if (rc < 0 || swr_init(swr_) < 0) {
            swr_free(&swr_);
            avcodec_free_context(&adec_);   // undecodable audio: play the video alone
            aidx_ = -1;
            return false;
        }
        return true;
    }

    // One demux step of a split DASH pair. Video and audio stay on the URLs
    // they were opened with; the earlier PTS is delivered next.
    bool pullDash(std::string& err) {
        auto pull = [&](AVFormatContext* fmt, AVPacket* hold, bool& have, bool& eof) -> bool {
            if (have || eof)
                return true;
            const int r = av_read_frame(fmt, hold);
            if (r == AVERROR_EOF) {
                eof = true;
                return true;
            }
            if (r < 0) {
                if (stallAbort_) {
                    stallAbort_ = false;
                    lastAvErr_ = misterplex::netresume::kStallAbortAv;
                    lastHttp_ = 0;
                    err = "stall";
                    return false;
                }
                lastAvErr_ = r;
                err = abort_.load() ? "cancelled" : "read: " + avErr(r);
                lastHttp_ = httpStatusOf(r, err);
                return false;
            }
            noteByte();
            have = true;
            return true;
        };
        if (!pull(fmt_, vHold_, haveV_, vEof_) || !pull(fmtA_, aHold_, haveA_, aEof_))
            return false;
        if (!haveV_ && !haveA_) {
            av_bsf_send_packet(bsf_, nullptr);
            takeVideo();
            if (adec_) {
                avcodec_send_packet(adec_, nullptr);
                takeAudio();
            }
            drained_ = true;
            return true;
        }
        auto ptsUs = [](AVFormatContext* fmt, const AVPacket* p) -> int64_t {
            if (p->pts == AV_NOPTS_VALUE)
                return INT64_MAX;
            return av_rescale_q(p->pts, fmt->streams[p->stream_index]->time_base, AV_TIME_BASE_Q);
        };
        const bool takeV = haveV_ && (!haveA_ || ptsUs(fmt_, vHold_) <= ptsUs(fmtA_, aHold_));
        if (takeV) {
            av_packet_move_ref(pkt_, vHold_);
            haveV_ = false;
            if (pkt_->stream_index == vidx_ && av_bsf_send_packet(bsf_, pkt_) == 0)
                takeVideo();
        } else {
            av_packet_move_ref(pkt_, aHold_);
            haveA_ = false;
            if (adec_ && pkt_->stream_index == aidx_ && avcodec_send_packet(adec_, pkt_) == 0)
                takeAudio();
        }
        av_packet_unref(pkt_);
        return true;
    }

    int64_t stallNow() const {
        if (stall_.nowMs)
            return stall_.nowMs();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void noteByte() {
        lastByteMs_ = stallNow();
        if (stallShown_) {
            stallShown_ = false;
            if (stall_.onClear)
                stall_.onClear();
        }
    }

    int onInterrupt() {
        if (abort_.load())
            return 1;
        // #231 Run H: a stop during the open (not in the session's src_ yet) and the per-attempt cap.
        if (cancelled_ && cancelled_()) {
            openStopped_ = true;
            return 1;
        }
        const int64_t dl = openDeadlineMs_.load();
        if (dl > 0 && stallNow() >= dl) {
            openCapped_ = true;
            return 1;
        }
        if (!stall_.enabled)
            return 0;
        const bool armed = stall_.armed && stall_.armed();
        const int64_t quiet = stallNow() - lastByteMs_;
        const auto tick = misterplex::netresume::stallTick(armed, quiet, stall_.showMs, stall_.abortMs);
        if (tick == misterplex::netresume::StallTick::Wait) {
            stallShown_ = false;
            return 0;
        }
        if (tick == misterplex::netresume::StallTick::Show) {
            if (!stallShown_) {
                stallShown_ = true;
                if (stall_.onShow)
                    stall_.onShow();
            }
            return 0;
        }
        stallAbort_ = true;
        return 1;
    }

    static int httpStatusOf(int avErr, const std::string& text) {
        int http = misterplex::netresume::httpStatusFromText(text);
        if (http)
            return http;
#ifdef AVERROR_HTTP_UNAUTHORIZED
        if (avErr == AVERROR_HTTP_UNAUTHORIZED)
            return 401;
#endif
#ifdef AVERROR_HTTP_FORBIDDEN
        if (avErr == AVERROR_HTTP_FORBIDDEN)
            return 403;
#endif
#ifdef AVERROR_HTTP_NOT_FOUND
        if (avErr == AVERROR_HTTP_NOT_FOUND)
            return 404;
#endif
#ifdef AVERROR_HTTP_SERVER_ERROR
        if (avErr == AVERROR_HTTP_SERVER_ERROR)
            return 500;
#endif
        (void)avErr;
        return 0;
    }

    struct SoftCue {
        int64_t startMs = 0;
        int64_t endMs = 0;
        std::string line1;
        std::string line2;
    };

    static bool textSubtitleCodec(AVCodecID id) {
        switch (id) {
        case AV_CODEC_ID_SUBRIP:
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SSA:
        case AV_CODEC_ID_MOV_TEXT:
        case AV_CODEC_ID_WEBVTT:
        case AV_CODEC_ID_TEXT:
        case AV_CODEC_ID_SRT:
            return true;
        default:
            return false;
        }
    }

    void closeTextSubtitle() {
        if (fmt_ && sidx_ >= 0 && sidx_ < static_cast<int>(fmt_->nb_streams))
            fmt_->streams[sidx_]->discard = AVDISCARD_ALL;
        avcodec_free_context(&sdec_);
        sidx_ = -1;
        cues_.clear();
    }

    bool armTextSubtitle(int index, std::string& err) {
        closeTextSubtitle();
        if (!fmt_ || index < 0 || index >= static_cast<int>(fmt_->nb_streams)) {
            err = "no text subtitle at index " + std::to_string(index);
            return false;
        }
        AVStream* st = fmt_->streams[index];
        if (!st->codecpar || st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE ||
            !textSubtitleCodec(st->codecpar->codec_id)) {
            err = "not a text subtitle stream";
            return false;
        }
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        if (!c) {
            err = "text subtitle decoder is not in this libav";
            st->discard = AVDISCARD_ALL;
            return false;
        }
        sdec_ = avcodec_alloc_context3(c);
        if (!sdec_ || avcodec_parameters_to_context(sdec_, st->codecpar) < 0) {
            avcodec_free_context(&sdec_);
            err = "subtitle decoder context failed";
            st->discard = AVDISCARD_ALL;
            return false;
        }
        sdec_->pkt_timebase = st->time_base;
        if (avcodec_open2(sdec_, c, nullptr) < 0) {
            avcodec_free_context(&sdec_);
            err = "subtitle decoder did not open";
            st->discard = AVDISCARD_ALL;
            return false;
        }
        st->discard = AVDISCARD_DEFAULT;
        sidx_ = index;
        std::fprintf(stderr, "misterplexd: soft subtitle index=%d codec=%s\n", index,
                     avcodec_get_name(st->codecpar->codec_id));
        return true;
    }

    void pushSoftCue(SoftCue c) {
        constexpr size_t kCap = 256;
        if (cues_.size() >= 64) {
            const int64_t horizon = c.startMs - 30000;
            auto end = std::remove_if(cues_.begin(), cues_.end(), [&](const SoftCue& old) {
                return old.endMs < horizon;
            });
            cues_.erase(end, cues_.end());
        }
        while (cues_.size() >= kCap)
            cues_.erase(cues_.begin());
        cues_.push_back(std::move(c));
    }

    void takeSubtitle(const AVPacket* pkt) {
#ifdef MUT_SOFTSUBS_DISCARD
        (void)pkt;
        return;
#else
        if (!sdec_ || !pkt)
            return;
        AVSubtitle sub;
        std::memset(&sub, 0, sizeof sub);
        int got = 0;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        const int rc = avcodec_decode_subtitle2(sdec_, &sub, &got, pkt);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (rc < 0 || !got) {
            if (got)
                avsubtitle_free(&sub);
            return;
        }
        const AVStream* st = fmt_->streams[sidx_];
        const AVRational tb = st->time_base;
        int64_t packetMs = 0;
        bool have = false;
        const int64_t origin = startPts90k_ == h264stream::kNoPts90k ? 0 : startPts90k_;
        if (pkt->pts != AV_NOPTS_VALUE && tb.num > 0 && tb.den > 0) {
            const int64_t p90 = toPts90k(pkt->pts, static_cast<uint32_t>(tb.num), static_cast<uint32_t>(tb.den));
            if (p90 >= 0) {
                packetMs = mediaMsFromPts90k(p90, origin);
                have = true;
            }
        }
        if (!have && sub.pts != AV_NOPTS_VALUE) {
            const int64_t p90 = toPts90k(sub.pts, 1, AV_TIME_BASE);
            if (p90 >= 0) {
                packetMs = mediaMsFromPts90k(p90, origin);
                have = true;
            }
        }
        int64_t start = packetMs + static_cast<int64_t>(sub.start_display_time);
        int64_t end = packetMs + static_cast<int64_t>(sub.end_display_time);
        if (end <= start)
            end = start + 4000;
        if (start < 0)
            start = 0;
        std::string text;
        for (unsigned i = 0; i < sub.num_rects; ++i) {
            AVSubtitleRect* r = sub.rects[i];
            if (!r || r->type == SUBTITLE_BITMAP)
                continue;
            const char* raw = nullptr;
            if (r->type == SUBTITLE_TEXT && r->text)
                raw = r->text;
            else if (r->ass)
                raw = r->ass;
            else if (r->text)
                raw = r->text;
            if (!raw || !raw[0])
                continue;
            std::string piece = misterplex::flattenDecodedSubtitleText(raw);
            if (piece.empty())
                continue;
            if (!text.empty())
                text.push_back('\n');
            text += piece;
        }
        avsubtitle_free(&sub);
        if (text.empty())
            return;
        SoftCue cue;
        cue.startMs = start;
        cue.endMs = end;
        misterplex::splitCueLines(text, cue.line1, cue.line2);
        if (cue.line1.empty() && cue.line2.empty())
            return;
        pushSoftCue(std::move(cue));
#endif
    }

    AVFormatContext* fmt_ = nullptr;
    AVFormatContext* fmtA_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVPacket* vHold_ = nullptr;
    AVPacket* aHold_ = nullptr;
    AVPacket* out_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVBSFContext* bsf_ = nullptr;
    AVCodecContext* adec_ = nullptr;
    AVCodecContext* sdec_ = nullptr;
    SwrContext* swr_ = nullptr;
    int vidx_ = -1, aidx_ = -1, sidx_ = -1;
    misterplex::PlexTextIdentity textHint_{};

    std::vector<misterplex::OpenedTextStream> openedTextStreams() const {
        std::vector<misterplex::OpenedTextStream> out;
        if (!fmt_)
            return out;
        for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
            AVStream* st = fmt_->streams[i];
            if (!st->codecpar || st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
                continue;
            if (!textSubtitleCodec(st->codecpar->codec_id))
                continue;
            misterplex::OpenedTextStream s;
            s.index = static_cast<int>(i);
            s.codec = avcodec_get_name(st->codecpar->codec_id);
            if (const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0))
                s.language = lang->value;
            if (const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0))
                s.title = title->value;
            s.forced = (st->disposition & AV_DISPOSITION_FORCED) != 0;
            out.push_back(std::move(s));
        }
        return out;
    }

    int resolvedTextIndex(int plexIndex) const {
        misterplex::PlexTextIdentity id = textHint_;
        id.plexIndex = plexIndex;
        return misterplex::mapEmbeddedTextIndex(id, openedTextStreams());
    }
    std::vector<SoftCue> cues_;
    int64_t startPts90k_ = h264stream::kNoPts90k;
    int64_t darNum_ = 0, darDen_ = 0;
    int64_t frameTicks_ = 0, videoFrames_ = 0;
    bool drained_ = false;
    bool videoPending_ = false;
    bool audioPending_ = false;
    bool haveV_ = false, haveA_ = false, vEof_ = false, aEof_ = false;
    std::deque<MediaPacket> ready_;
    std::atomic<size_t> readyDepth_{0};
    unsigned probePackets_ = 0;
    size_t readyBytes_ = 0;
    std::atomic<bool> abort_{false};
    misterplex::netresume::StallWatch stall_{};
    int64_t lastByteMs_ = 0;
    int lastAvErr_ = 0;
    int lastHttp_ = 0;
    bool stallShown_ = false;
    bool stallAbort_ = false;
    misterplex::httprange::AvioSession rangeIo_{};
    misterplex::spelunkr::HeaderCacheAvio headerIo_{};
    std::unique_ptr<misterplex::spelunkr::HeaderCacheStore> headerStore_;
};

static_assert(AVERROR_EXIT == misterplex::netresume::kStallAbortAv, "stall abort is AVERROR_EXIT");

} // namespace

std::unique_ptr<IMediaSource> openAvSource(const OpenRequest& req, std::string& err) {
    auto s = std::make_unique<AvSource>();
    const bool ok = s->open(req, err);
    s->openDone();
    if (!ok)
        return nullptr;
    return s;
}

namespace {
struct PartTextDeadline {
    int64_t deadlineMs = 0;
    const std::function<bool()>* cancelled = nullptr;
};
int64_t partTextNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

bool extractPartTextSrt(const std::string& partUrl, const std::string& headers, int index, int timeoutMs,
                        const std::function<bool()>& cancelled, std::string& srt, std::string& err) {
    srt.clear();
    if (partUrl.find('?') != std::string::npos || misterplex::hasPlexTokenKey(partUrl)) {
        err = "part url carries a query";
        return false;
    }
    if (index < 0) {
        err = "no container index";
        return false;
    }
    av_log_set_level(AV_LOG_ERROR);
    misterplex::installRedactingAvLog();
    PartTextDeadline dl;
    dl.deadlineMs = partTextNowMs() + (timeoutMs > 0 ? timeoutMs : 30000);
    dl.cancelled = &cancelled;
    AVFormatContext* fmt = avformat_alloc_context();
    AVPacket* pkt = av_packet_alloc();
    if (!fmt || !pkt) {
        avformat_free_context(fmt);
        av_packet_free(&pkt);
        err = "allocation failed";
        return false;
    }
    fmt->interrupt_callback.callback = [](void* o) -> int {
        const auto* d = static_cast<PartTextDeadline*>(o);
        if (d->cancelled && *d->cancelled && (*d->cancelled)())
            return 1;
#ifdef MUT_334_NO_DEADLINE
        return 0;
#else
        return partTextNowMs() > d->deadlineMs ? 1 : 0;
#endif
    };
    fmt->interrupt_callback.opaque = &dl;
    AVDictionary* opts = nullptr;
    if (!headers.empty())
        av_dict_set(&opts, "headers", headers.c_str(), 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
#ifndef MUT_334_FOLLOW_REDIRECTS
    // The token rides "headers", which libav re-sends on every redirect hop: never follow
    // one from the Part read (a 3xx fails the open; the first hop is the verified server).
    av_dict_set(&opts, "max_redirects", "0", 0);
#endif
    int rc = avformat_open_input(&fmt, partUrl.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        av_packet_free(&pkt);
        char averr[64];
        av_strerror(rc, averr, sizeof averr);
        err = std::string("part open failed (") + averr + ")";
        return false;  // avformat_open_input freed fmt
    }
    bool ok = false;
    std::vector<misterplex::PartTextPacket> cues;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0 && fmt->nb_streams == 0) {
            err = "part has no streams";
            break;
        }
        if (index >= static_cast<int>(fmt->nb_streams)) {
            err = "part has no stream at the index";
            break;
        }
        AVStream* st = fmt->streams[index];
        const AVCodecID id = st->codecpar->codec_id;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE ||
            (id != AV_CODEC_ID_SUBRIP && id != AV_CODEC_ID_TEXT)) {
            err = "part index is not a subrip track";
            break;
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            fmt->streams[i]->discard = static_cast<int>(i) == index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        const AVRational ms{1, 1000};
        size_t bytes = 0;
        while ((rc = av_read_frame(fmt, pkt)) >= 0) {
            if (pkt->stream_index == index && pkt->data && pkt->size > 0 && pkt->pts != AV_NOPTS_VALUE) {
                misterplex::PartTextPacket c;
                c.startMs = av_rescale_q(pkt->pts, st->time_base, ms);
                c.durMs = av_rescale_q(pkt->duration, st->time_base, ms);
                c.text.assign(reinterpret_cast<const char*>(pkt->data), static_cast<size_t>(pkt->size));
                bytes += c.text.size();
                cues.push_back(std::move(c));
            }
            av_packet_unref(pkt);
#ifndef MUT_334_NO_SIZE_CAP
            if (bytes > misterplex::kPartTextMaxBytes)
                break;
#endif
        }
        if (cancelled && cancelled()) {
            err = "part text cancelled";
            break;
        }
        // Deadline or read error part-way: the cues read so far are kept on purpose (captions
        // for the start of a slow Part beat none); nothing read at all is a failure.
        if (rc < 0 && rc != AVERROR_EOF && cues.empty()) {
            err = "part read failed";
            break;
        }
        std::stable_sort(cues.begin(), cues.end(),
                         [](const misterplex::PartTextPacket& a, const misterplex::PartTextPacket& b) {
                             return a.startMs < b.startMs;
                         });
        srt = misterplex::srtFromPartPackets(cues);
        if (srt.empty()) {
            err = "part text track is empty";
            break;
        }
        ok = true;
    } while (false);
    avformat_close_input(&fmt);
    av_packet_free(&pkt);
    return ok;
}

} // namespace misterplex::owenb
