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
#include "libmisterplex/live_disc_rebase.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
// First-spawn split A/V gate. 0 disables it (today's immediate popFirstVideo).
// The 1500 ms total is the bound for a short lead / pass once audio is present.
// A lead over ~200 ms (kSoakGapGateTicks90k) must not be presented on that bound:
// keep demuxing for a skip_gop key up to kLiveAvLongLeadWaitMs from the first key,
// then reopen ("av start long lead") instead of releasing the original key.
// MPX_LIVE_AV_GATE_ALLOW_LONG_LEAD=1 restores today's G6: finish("timeout") at
// 1500 ms and present the early key. Unset / empty / invalid = do not present.
// No-audio is a separate bound: MPX_LIVE_AV_GATE_NO_AUDIO_MS.
//   unset / empty / invalid → 7000 ms from the first key, do not present a silent
//   picture, then fail the read with "av start no audio" so prefetch takes the
//   existing Pluto live-open retry (plutoPrefetchFailTransient). No player.stop().
//   1200 → today's bound: finish("timeout") and present the key.
// Other positive values wait that long (capped) and still do not present.
constexpr int64_t kLiveAvStartGateDefaultMs = 1200;
constexpr int64_t kLiveAvStartGateTotalMs = 1500;
constexpr int64_t kLiveAvLongLeadWaitMs = 8000;
constexpr int64_t kLiveAvNoAudioWaitDefaultMs = 7000;
constexpr int64_t kLiveAvNoAudioLegacyMs = 1200;
constexpr int64_t kLiveAvNoAudioWaitCapMs = 8000;
// Needles for plutoPrefetchFailTransient / tryPlutoLiveOpenRetry.
constexpr const char* kLiveAvStartNoAudioReopen = "av start no audio";
constexpr const char* kLiveAvStartLongLeadReopen = "av start long lead";

int64_t liveAvStartGateMsFromEnv() {
    const char* e = std::getenv("MPX_LIVE_AV_START_GATE_MS");
    if (!e || !*e)
        return kLiveAvStartGateDefaultMs;
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || *end != '\0')
        return kLiveAvStartGateDefaultMs;
    if (v <= 0)
        return 0;
    return static_cast<int64_t>(v);
}

struct LiveAvNoAudioPolicy {
    int64_t waitMs = kLiveAvNoAudioWaitDefaultMs;
    bool presentOnTimeout = false;
};

LiveAvNoAudioPolicy liveAvNoAudioPolicyFromEnv() {
#ifdef MUT_LIVE_AV_START_PRESENT_ON_NO_AUDIO_TIMEOUT
    // Today's bug: 1200 ms, then present the silent key. New cases must fail.
    return {kLiveAvNoAudioLegacyMs, true};
#else
    const char* e = std::getenv("MPX_LIVE_AV_GATE_NO_AUDIO_MS");
    if (!e || !*e)
        return {};
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(e, &end, 10);
    if (end == e || !end || *end != '\0' || errno == ERANGE || v <= 0)
        return {};
    if (v == kLiveAvNoAudioLegacyMs)
        return {kLiveAvNoAudioLegacyMs, true};
    const long capped = v > kLiveAvNoAudioWaitCapMs ? kLiveAvNoAudioWaitCapMs : v;
    return {capped, false};
#endif
}

struct LiveAvLongLeadPolicy {
    bool presentOnTimeout = false;
    int64_t waitMs = kLiveAvLongLeadWaitMs;
};

// Unset / empty / invalid: do not present a lead over ~200 ms.
// MPX_LIVE_AV_GATE_ALLOW_LONG_LEAD=1: today's G6, finish("timeout") at 1500 ms.
// MUT_LIVE_AV_START_PRESENT_ON_LONG_LEAD_TIMEOUT restores that present.
LiveAvLongLeadPolicy liveAvLongLeadPolicyFromEnv() {
#ifdef MUT_LIVE_AV_START_PRESENT_ON_LONG_LEAD_TIMEOUT
    return {true, kLiveAvStartGateTotalMs};
#else
    const char* e = std::getenv("MPX_LIVE_AV_GATE_ALLOW_LONG_LEAD");
    if (e && e[0] == '1' && e[1] == '\0')
        return {true, kLiveAvStartGateTotalMs};
    return {};
#endif
}

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
    std::atomic<int64_t> fillDeadlineMs_{0};
    std::shared_ptr<std::atomic<int64_t>> openExtraBudgetMs_;
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
    bool liveRemux() const override { return liveRemux_; }
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
        openExtraBudgetMs_ = req.attemptExtraBudgetMs;
        if (req.attemptTimeoutMs > 0)
            openDeadlineMs_.store(stallNow() + req.attemptTimeoutMs);
        liveRemux_ = req.url.find("/yt-live.ts") != std::string::npos;
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
#ifndef MUT_LIVE_NO_CLIENT_RECONNECT
            // Glass 47abfbbb: client_write closed /yt-live.ts and the player never came back.
            // libav reconnect_streamed issues a new GET; LiveRelay.accept starts a fresh ffmpeg.
            if (liveRemux_) {
                av_dict_set(dst, "reconnect", "1", 0);
                av_dict_set(dst, "reconnect_streamed", "1", 0);
                av_dict_set(dst, "reconnect_on_network_error", "1", 0);
                av_dict_set(dst, "reconnect_delay_max", "4", 0);
            }
#endif
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
        const auto openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openT0).count();
        if (usedHeaderIo) {
            const auto& hs = headerIo_.stats();
            std::fprintf(stderr,
                         "misterplexd: spelunkr header-cache %s moov_offset=%lld moov_size=%lld "
                         "bytes_read=%lld cached_bytes=%lld live_bytes=%lld open_ms=%.1f "
                         "live_open_ms=%lld live_seek_ms=%lld cache_read_ms=%lld live_opens=%d "
                         "live_seeks=%d reconnects=%d rc=%d\n",
                         hs.cacheHit ? "hit" : "miss", (long long)hs.moovOffset, (long long)hs.moovSize,
                         (long long)(fmt_ && fmt_->pb ? fmt_->pb->bytes_read : 0),
                         (long long)hs.cachedBytesServed, (long long)hs.liveBytesRead, openMs,
                         (long long)hs.liveOpenMs, (long long)hs.liveSeekMs, (long long)hs.cacheReadMs,
                         hs.liveOpens, hs.liveSeeks, hs.liveReconnects, rc);
        }
        if (rc < 0) {
            if (usedHeaderIo && headerIo_.stats().cacheHit && headerStore_)
                // P2-2: open itself failed on a cache hit (a stale/poisoned entry, or a hit
                // whose live gap read failed). Delete it so the next open is a clean miss.
                headerStore_->remove(headerKey);
            if (usedHeaderIo)
                // browse-lane P2-A: for MP4/MOV the moov atom is parsed inside avformat_open_input
                // itself (mov_read_header), not avformat_find_stream_info -- this is the path
                // a dial-500-sized moov actually takes on a cancel/prefetch-timeout, and
                // without this call every byte already read here was discarded before today.
                // openStopped_/openCapped_ distinguish a cancel/timeout from a genuine
                // protocol error, so only the former persists a resumable partial.
                headerIo_.finalizeHeader(false, openStopped_ || openCapped_);
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
        const auto parseT0 = std::chrono::steady_clock::now();
        int r = avformat_find_stream_info(fmt_, nullptr);
        const auto movParseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - parseT0)
                                    .count();
        if (usedHeaderIo) {
            // Persist whatever header bytes were actually touched (a miss, or a hit with a gap)
            // before any seek, so the next open of this (url, size, etag) is a full hit --
            // unless find_stream_info itself failed (P2-2: a failed probe's bytes are not a
            // good header, and a cache hit that failed is deleted rather than persisted).
            // browse-lane P2-A: on a find_stream_info failure, openStopped_/openCapped_ (cancel / zap
            // window / prefetch-slice timeout, not a genuine protocol error) persists a
            // resumable partial of what was read instead of dropping it.
            headerIo_.finalizeHeader(r >= 0, openStopped_ || openCapped_);
            const auto& hs = headerIo_.stats();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openT0).count();
            std::fprintf(stderr,
                         "misterplexd: spelunkr header-cache find_stream_info moov_offset=%lld "
                         "bytes_read=%lld elapsed_ms=%.1f mov_parse_ms=%lld r=%d\n",
                         (long long)hs.moovOffset, (long long)(fmt_ && fmt_->pb ? fmt_->pb->bytes_read : 0), ms,
                         (long long)movParseMs, r);
            if (req.headerCacheStatsOut) {
                req.headerCacheStatsOut->cacheHit = hs.cacheHit;
                req.headerCacheStatsOut->cachedBytesServed = hs.cachedBytesServed;
                req.headerCacheStatsOut->liveBytesRead = hs.liveBytesRead;
                req.headerCacheStatsOut->liveOpenMs = hs.liveOpenMs;
                req.headerCacheStatsOut->liveSeekMs = hs.liveSeekMs;
                req.headerCacheStatsOut->cacheReadMs = hs.cacheReadMs;
                req.headerCacheStatsOut->movParseMs = movParseMs;
                req.headerCacheStatsOut->liveOpens = hs.liveOpens;
                req.headerCacheStatsOut->liveSeeks = hs.liveSeeks;
                req.headerCacheStatsOut->liveReconnects = hs.liveReconnects;
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
        // Live remux was set at open() start so reconnect flags and openAudio
        // (split MPEG-TS AAC in a second program; related=vidx_ used to stick in aidx_)
        // both see it.
        // A preferred index names a stream in the muxed file. Separate DASH audio
        // is its own input, so the preference does not apply there.
        openAudio(fmtA_ ? -1 : req.audioStreamIndex);
        // Pace only split-rendition live (Vevo two-HLS remux). /yt-live.ts alone is
        // muxed Pluto too; VOD never sets liveRemux_. E 9b4c55a7 REVIEW REJECT.
        splitLivePace_ = liveRemux_ && (req.splitLiveRendition ||
                                        misterplex::live_disc::splitLiveRemuxActive().load() ||
                                        audioOutsideVideoProgram());
        avStartGateMs_ = liveAvStartGateMsFromEnv();
        {
            const LiveAvNoAudioPolicy noAudio = liveAvNoAudioPolicyFromEnv();
            noAudioWaitMs_ = noAudio.waitMs;
            presentSilentNoAudio_ = noAudio.presentOnTimeout;
        }
        {
            const LiveAvLongLeadPolicy longLead = liveAvLongLeadPolicyFromEnv();
            longLeadWaitMs_ = longLead.waitMs;
            presentLongLead_ = longLead.presentOnTimeout;
        }
        if (liveRemux_) {
            const char* codec = "none";
            int rate = 0;
            const int logAidx = audioIndexOrNone();
            if (logAidx >= 0 && audioFmt() && audioFmt()->streams[logAidx] &&
                audioFmt()->streams[logAidx]->codecpar) {
                codec = avcodec_get_name(audioFmt()->streams[logAidx]->codecpar->codec_id);
                rate = audioFmt()->streams[logAidx]->codecpar->sample_rate;
            }
            std::fprintf(stderr,
                         "misterplexd: live remux demux: video_idx=%d audio_idx=%d codec=%s rate=%d decoder=%d\n",
                         vidx_, logAidx, codec, rate, adec_ ? 1 : 0);
        }
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
            // Live remux: if the GATE7 probe missed AAC (Hybrik muxed MPEG-TS, audio
            // packets after the 64-packet cap), keep audio PIDs so a later PMT/ADTS
            // can still open the decoder. UNKNOWN: a split PMT can advertise the PID
            // before codecpar settles; discarding it made VEVO HDMI sit at the floor.
            if (liveRemux_ && aidx_ < 0 && fmt_->streams[i]->codecpar) {
                const AVMediaType t = fmt_->streams[i]->codecpar->codec_type;
                if (t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_UNKNOWN)
                    continue;
            }
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
            if (splitLivePace_ && !firstSpawnPaced_) {
                if (!fillFirstSpawnPace(err))
                    return SourceRead::Error;
#ifndef MUT_LIVE_PACE_HOLD_AUDIO
                // Prefix only (SPS/AUD/IDR) so needKey_ clears. Glass G kept
                // popping every later video AU while PCM sat in ready_: the
                // ring presented video, then the 1.5 s dump was AUD_LATE and
                // HDMI sat at idle floor from t=0 (glass G 8.99 s / −81.9 dB)
                // even though audio_bytes=290816. After the first key, FIFO
                // A+V so paced PCM lands with the video it belongs to.
                if (!firstVideoDelivered_) {
#ifndef MUT_LIVE_AV_START_NO_GATE
                    // No-audio reopen must not fall through to popFirstVideo.
                    // That was Finding 4: packet 1 presented, packet 2 blocked
                    // in popReadyByPts because videoLeadAllowed(pts, kNoPts90k)
                    // is false until audio exists.
                    if (avGateLongLeadFailed_) {
                        err = kLiveAvStartLongLeadReopen;
                        return SourceRead::Error;
                    }
                    if (avGateNoAudioFailed_) {
                        err = kLiveAvStartNoAudioReopen;
                        return SourceRead::Error;
                    }
                    if (!avGateDone_ && avStartGateMs_ > 0) {
                        const DemuxStep g = avStartGate(err);
                        if (g == DemuxStep::Error)
                            return SourceRead::Error;
                    }
#endif
                    if (popFirstVideo(p))
                        return SourceRead::Packet;
                }
#else
                // Mutant: G video-only watermark. Tests must FAIL.
                if (popFirstVideo(p))
                    return SourceRead::Packet;
#endif
            }
            if (!splitLivePace_ || firstSpawnPaced_
#ifndef MUT_LIVE_PACE_HOLD_AUDIO
                || firstVideoDelivered_
#endif
            ) {
#ifndef MUT_LIVE_PACE_HOLD_AUDIO
                // After the first key, deliver by PTS so a video-first ready_
                // queue cannot dump 1.5 s of video before the paced PCM.
                if (splitLivePace_ && firstVideoDelivered_ && popReadyByPts(p))
                    return SourceRead::Packet;
#endif
                if (!ready_.empty()) {
#ifndef MUT_LIVE_NO_VIDEO_LEAD_CAP
                    // Do not leak a leading video AU through the FIFO fallback
                    // while popReadyByPts is holding for audio (H 3.42 s / 2.64 s).
                    if (!(splitLivePace_ && firstVideoDelivered_ &&
                          ready_.front().kind == MediaPacket::Kind::Video &&
                          !misterplex::live_disc::videoLeadAllowed(ready_.front().pts90k,
                                                                   lastAudioEmitPts_)))
#endif
                    {
                    p = std::move(ready_.front());
                    readyBytes_ -= std::min(readyBytes_, p.data.size());
                    ready_.pop_front();
                    readyDepth_.store(ready_.size());
                    noteEmitted(p);
                    return SourceRead::Packet;
                    }
                }
            }
            const DemuxStep step = demuxOne(err);
            if (step == DemuxStep::Error)
                return SourceRead::Error;
            if (step == DemuxStep::FillTimeout) {
                // Bound the stuck av_read_frame only. Do not release a 0-byte
                // watermark (glass F 1066 audio_bytes=0 need=288000).
                // libav latches AVERROR_EXIT onto the AVIO context. Leave it
                // and the re-armed fill deadline treats that latch as another
                // timeout without reading the socket.
                clearAvioExitLatch();
                fillDeadlineMs_.store(0);
                continue;
            }
            if (step == DemuxStep::Eof) {
                if (splitLivePace_ && !firstSpawnPaced_)
                    logFirstSpawnPace(misterplex::live_disc::firstSpawnPaceAudioBytes());
                firstSpawnPaced_ = true;
#ifndef MUT_LIVE_PACE_HOLD_AUDIO
                if (splitLivePace_ && firstVideoDelivered_ && popReadyByPts(p))
                    return SourceRead::Packet;
                if (splitLivePace_ && !firstVideoDelivered_ && popFirstVideo(p))
                    return SourceRead::Packet;
#else
                if (splitLivePace_ && popFirstVideo(p))
                    return SourceRead::Packet;
#endif
                if (!ready_.empty()) {
#ifndef MUT_LIVE_NO_VIDEO_LEAD_CAP
                    // Do not leak a leading video AU through the FIFO fallback
                    // while popReadyByPts is holding for audio (H 3.42 s / 2.64 s).
                    if (!(splitLivePace_ && firstVideoDelivered_ &&
                          ready_.front().kind == MediaPacket::Kind::Video &&
                          !misterplex::live_disc::videoLeadAllowed(ready_.front().pts90k,
                                                                   lastAudioEmitPts_)))
#endif
                    {
                    p = std::move(ready_.front());
                    readyBytes_ -= std::min(readyBytes_, p.data.size());
                    ready_.pop_front();
                    readyDepth_.store(ready_.size());
                    noteEmitted(p);
                    return SourceRead::Packet;
                    }
                }
                return SourceRead::Eof;
            }
        }
    }

private:
    enum class DemuxStep { Ok, Eof, Error, FillTimeout };

    int64_t audioBytesInReady() const {
        int64_t n = 0;
        for (const auto& m : ready_) {
            if (m.kind == MediaPacket::Kind::Audio)
                n += static_cast<int64_t>(m.data.size());
        }
        return n;
    }

    bool fillDeadlineHit() const {
        const int64_t dl = fillDeadlineMs_.load();
        return dl > 0 && stallNow() >= dl;
    }

    // Split-rendition live: audio is not in the video MPEG-TS program (two-program
    // remux / test fixture). Muxed Pluto is one program with both A and V.
    bool audioOutsideVideoProgram() const {
        if (!fmt_ || vidx_ < 0 || fmt_->nb_programs < 2)
            return false;
        int vProg = -1, aProg = -1;
        for (unsigned p = 0; p < fmt_->nb_programs; ++p) {
            const AVProgram* pr = fmt_->programs[p];
            if (!pr)
                continue;
            for (unsigned i = 0; i < pr->nb_stream_indexes; ++i) {
                const int si = static_cast<int>(pr->stream_index[i]);
                if (si == vidx_)
                    vProg = static_cast<int>(p);
                if (aidx_ >= 0 && si == aidx_)
                    aProg = static_cast<int>(p);
            }
        }
        if (vProg < 0)
            return false;
        if (aidx_ < 0)
            return true;
        return aProg != vProg;
    }

    bool popFirstVideo(MediaPacket& p) {
        // Stream order: SPS/AUD before IDR. F popped the first key from the middle
        // and then blocked, so the FPGA saw an IDR without the prefix and waited
        // out the fill (zap first_picture vs B).
        for (auto it = ready_.begin(); it != ready_.end(); ++it) {
            if (it->kind != MediaPacket::Kind::Video)
                continue;
            p = std::move(*it);
            readyBytes_ -= std::min(readyBytes_, p.data.size());
            ready_.erase(it);
            readyDepth_.store(ready_.size());
            if (p.keyframe)
                firstVideoDelivered_ = true;
            noteEmitted(p);
            return true;
        }
        return false;
    }

    void noteEmitted(const MediaPacket& p) {
        if (p.kind == MediaPacket::Kind::Audio && p.pts90k != misterplex::live_disc::kNoPts90k)
            lastAudioEmitPts_ = p.pts90k;
        if (p.kind == MediaPacket::Kind::Video && p.pts90k != misterplex::live_disc::kNoPts90k)
            lastVideoEmitPts_ = p.pts90k;
    }

    // Glass H r3: after the first key, popReadyByPts dumped 3.42 s of video
    // (idle-floor HDMI) while 1.5 s of PCM sat in ready_ at a later PTS, and
    // a mid-soak pts_jump left video moving with no audio. Cap video lead at
    // the 0.2 s soak gate; snap the first post-key (or post-disc) audio AU
    // onto the last video emit so paced PCM starts now, not 3.42 s later.
    bool popReadyByPts(MediaPacket& p) {
        if (ready_.empty())
            return false;
        auto bestAudio = ready_.end();
        auto best = ready_.begin();
        for (auto it = ready_.begin(); it != ready_.end(); ++it) {
            if (it->kind == MediaPacket::Kind::Audio &&
                (bestAudio == ready_.end() ||
                 (it->pts90k >= 0 && (bestAudio->pts90k < 0 || it->pts90k < bestAudio->pts90k))))
                bestAudio = it;
            if (it->pts90k < 0)
                continue;
            if (best->pts90k < 0 || it->pts90k < best->pts90k)
                best = it;
        }
#ifndef MUT_LIVE_NO_VIDEO_LEAD_CAP
        if (splitLivePace_ && firstVideoDelivered_) {
            const bool videoLeads = best->kind == MediaPacket::Kind::Video &&
                !misterplex::live_disc::videoLeadAllowed(best->pts90k, lastAudioEmitPts_);
            if (videoLeads) {
                if (bestAudio == ready_.end())
                    return false;  // demux more; do not emit a ≥0.2 s video-only lead
                best = bestAudio;
            }
        }
#endif
        p = std::move(*best);
        readyBytes_ -= std::min(readyBytes_, p.data.size());
        ready_.erase(best);
        readyDepth_.store(ready_.size());
        if (p.kind == MediaPacket::Kind::Video && p.keyframe)
            firstVideoDelivered_ = true;
#ifndef MUT_LIVE_NO_VIDEO_LEAD_CAP
        // First audio after the key (or after a pts_jump hold): play now with
        // the picture already on screen, not at a late independent PTS origin.
        if (p.kind == MediaPacket::Kind::Audio &&
            lastAudioEmitPts_ == misterplex::live_disc::kNoPts90k &&
            lastVideoEmitPts_ != misterplex::live_disc::kNoPts90k)
            p.pts90k = lastVideoEmitPts_;
#endif
        noteEmitted(p);
        return true;
    }

    void logFirstSpawnPace(int64_t need) {
        if (paceLogged_)
            return;
        paceLogged_ = true;
        std::fprintf(stderr,
                     "misterplexd: live remux first-spawn pace audio_bytes=%lld need=%lld\n",
                     static_cast<long long>(pacedAudioBytes_), static_cast<long long>(need));
    }

    // Split-rendition live only. Decode 1.5 s of PCM (post-first-key-AU, not
    // FPGA first_picture) so the H264 ring absorbs a 1.12 s (D t~31 s /
    // remux ~70 s) or 1.39 s (F t=221.8 s) audio-EXTINF fetch hole. After the
    // first key, A+V FIFO into the ring (glass G held PCM while video played:
    // HDMI idle floor). Fill deadline only interrupts a stuck av_read_frame.
    // Mutant 0 skips the 1.5 s; MUT_LIVE_PACE_HOLD_AUDIO restores G's hold.
    bool fillFirstSpawnPace(std::string& err) {
        (void)err;
        const int64_t need = misterplex::live_disc::firstSpawnPaceAudioBytes();
        if (need <= 0) {
            firstSpawnPaced_ = true;
            return true;
        }
        if (fillDeadlineMs_.load() == 0)
            fillDeadlineMs_.store(stallNow() + misterplex::live_disc::kFirstSpawnPaceFillMs);
        // Cumulative produced PCM: FIFO after the first key drains ready_, so
        // audioBytesInReady() is not the watermark (glass G logged 290816 of
        // held bytes that never reached HDMI).
        if (pacedAudioBytes_ >= need || audioBytesInReady() >= need) {
            logFirstSpawnPace(need);
            fillDeadlineMs_.store(0);
            firstSpawnPaced_ = true;
        }
        return true;
    }

    void clearAvioExitLatch() {
        if (fmt_ && fmt_->pb) {
            fmt_->pb->error = 0;
            fmt_->pb->eof_reached = 0;
        }
    }

    static bool validPts(int64_t pts) { return pts != misterplex::live_disc::kNoPts90k; }

    int64_t firstQueuedVideoKeyPts() const {
        for (const auto& m : ready_) {
            if (m.kind == MediaPacket::Kind::Video && m.keyframe && validPts(m.pts90k))
                return m.pts90k;
        }
        return misterplex::live_disc::kNoPts90k;
    }

    int64_t firstQueuedAudioPts() const {
        int64_t best = misterplex::live_disc::kNoPts90k;
        for (const auto& m : ready_) {
            if (m.kind != MediaPacket::Kind::Audio || !validPts(m.pts90k))
                continue;
            if (!validPts(best) || m.pts90k < best)
                best = m.pts90k;
        }
        return best;
    }

    int64_t qualifyingKeyPts(int64_t minPts) const {
        for (const auto& m : ready_) {
            if (m.kind == MediaPacket::Kind::Video && m.keyframe && validPts(m.pts90k) && m.pts90k >= minPts)
                return m.pts90k;
        }
        return misterplex::live_disc::kNoPts90k;
    }

#ifdef MUT_LIVE_AV_START_DROP_WHILE_SKIPPING
    // Pre-fix per-pass drop. A timeout while skipping has already discarded the
    // original key, so the first video read() returns is not that key.
    int dropVideoBeforePts(int64_t minPts) {
        int n = 0;
        for (auto it = ready_.begin(); it != ready_.end();) {
            if (it->kind == MediaPacket::Kind::Video && validPts(it->pts90k) && it->pts90k < minPts) {
                readyBytes_ -= std::min(readyBytes_, it->data.size());
                it = ready_.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        if (n)
            readyDepth_.store(ready_.size());
        return n;
    }
#endif

    // Erase every queued video packet that sits before the first keyframe at or
    // after minPts. Deque erase invalidates iterators, so rescan after each drop.
    int eraseVideoBeforeKey(int64_t minPts) {
        int n = 0;
        for (;;) {
            auto key = ready_.end();
            for (auto it = ready_.begin(); it != ready_.end(); ++it) {
                if (it->kind == MediaPacket::Kind::Video && it->keyframe && validPts(it->pts90k) &&
                    it->pts90k >= minPts) {
                    key = it;
                    break;
                }
            }
            if (key == ready_.end())
                break;
            auto victim = ready_.end();
            for (auto it = ready_.begin(); it != key; ++it) {
                if (it->kind == MediaPacket::Kind::Video) {
                    victim = it;
                    break;
                }
            }
            if (victim == ready_.end())
                break;
            readyBytes_ -= std::min(readyBytes_, victim->data.size());
            ready_.erase(victim);
            ++n;
        }
        if (n)
            readyDepth_.store(ready_.size());
        return n;
    }

    void eraseAudioBefore(int64_t minPts) {
        bool dropped = false;
        for (auto it = ready_.begin(); it != ready_.end();) {
            if (it->kind == MediaPacket::Kind::Audio && validPts(it->pts90k) && it->pts90k < minPts) {
                readyBytes_ -= std::min(readyBytes_, it->data.size());
                it = ready_.erase(it);
                dropped = true;
            } else {
                ++it;
            }
        }
        if (dropped)
            readyDepth_.store(ready_.size());
    }

    // Split live, first spawn only. Hold the first video key until audio shows
    // up, and if audio starts more than 0.2 s later, release the key at that
    // audio instead of the opening GOP. Video is dropped only at the skip_gop
    // decision. A lead over ~200 ms keeps demuxing until a qualifying key or
    // kLiveAvLongLeadWaitMs from the first key, then reopens ("av start long
    // lead") instead of presenting the original key. The 1500 ms total stays
    // the short-lead / pass bound and the kill-switch present
    // (MPX_LIVE_AV_GATE_ALLOW_LONG_LEAD=1, G6 today).
    // With no audio, the default waits kLiveAvNoAudioWaitDefaultMs from the
    // first key and then returns Error (no silent present). The kill switch
    // MPX_LIVE_AV_GATE_NO_AUDIO_MS=1200 still finish("timeout") and presents.
    // MUT_LIVE_AV_START_GATE_UNBOUNDED is the build with no deadline.
    // MUT_LIVE_AV_START_DROP_WHILE_SKIPPING restores the per-pass drop (G6 fails).
    // MUT_LIVE_AV_START_PRESENT_ON_NO_AUDIO_TIMEOUT presents at 1200 ms.
    // MUT_LIVE_AV_START_PRESENT_ON_LONG_LEAD_TIMEOUT presents the 5 s-early key.
    DemuxStep avStartGate(std::string& err) {
        struct ClearSkipGopProgressGuard {
            ~ClearSkipGopProgressGuard() { misterplex::spelunkr::clearLiveRemuxSkipGopProgress(); }
        } clearSkipGopProgressGuard;
        int dropped = 0;
        int64_t gateStart = -1;
        const int64_t noPts = misterplex::live_disc::kNoPts90k;
        int64_t firstKeyPts = noPts;
        int64_t firstAudioPts = noPts;
        bool skipping = false;
        const int64_t soak = misterplex::live_disc::kSoakGapGateTicks90k;

        auto finish = [&](const char* action, int64_t leadMs) -> DemuxStep {
            const int64_t wait = gateStart >= 0 ? std::max<int64_t>(0, stallNow() - gateStart) : 0;
            std::fprintf(stderr,
                         "misterplexd: live remux av start gate lead_ms=%lld action=%s dropped_v=%d wait_ms=%lld\n",
                         static_cast<long long>(leadMs), action, dropped, static_cast<long long>(wait));
            avGateDone_ = true;
            gateDeadlineMs_.store(0);
            gateWake_ = false;
            return DemuxStep::Ok;
        };
        // Do not set avGateDone_: the next read() must keep returning Error
        // instead of popFirstVideo on the silent key.
        auto reopenNoAudio = [&](int64_t waited) -> DemuxStep {
            if (presentSilentNoAudio_)
                return finish("timeout", -1);
            err = kLiveAvStartNoAudioReopen;
            avGateNoAudioFailed_ = true;
            const int64_t wait = waited >= 0 ? waited
                                             : (gateStart >= 0 ? std::max<int64_t>(0, stallNow() - gateStart) : 0);
            std::fprintf(stderr,
                         "misterplexd: live remux av start gate lead_ms=-1 action=reopen dropped_v=%d wait_ms=%lld\n",
                         dropped, static_cast<long long>(wait));
            gateDeadlineMs_.store(0);
            gateWake_ = false;
            return DemuxStep::Error;
        };
        // Same as no-audio: do not set avGateDone_, so popFirstVideo cannot
        // release the key that leads audio by more than ~200 ms.
        auto reopenLongLead = [&](int64_t leadMs, int64_t waited) -> DemuxStep {
            if (presentLongLead_)
                return finish("timeout", leadMs);
            err = kLiveAvStartLongLeadReopen;
            avGateLongLeadFailed_ = true;
            const int64_t wait = waited >= 0 ? waited
                                             : (gateStart >= 0 ? std::max<int64_t>(0, stallNow() - gateStart) : 0);
            std::fprintf(stderr,
                         "misterplexd: live remux av start gate lead_ms=%lld action=reopen dropped_v=%d wait_ms=%lld\n",
                         static_cast<long long>(leadMs), dropped, static_cast<long long>(wait));
            gateDeadlineMs_.store(0);
            gateWake_ = false;
            return DemuxStep::Error;
        };

        for (;;) {
            if (firstKeyPts == noPts) {
                firstKeyPts = firstQueuedVideoKeyPts();
                if (firstKeyPts != noPts && gateStart < 0)
                    gateStart = stallNow();
            }
            if (gateStart >= 0 && firstAudioPts == noPts)
                firstAudioPts = firstQueuedAudioPts();

            if (gateStart >= 0 && firstAudioPts != noPts && !skipping) {
                const int64_t lead = firstAudioPts - firstKeyPts;
                if (lead <= soak)
                    return finish("pass", lead / 90);
                skipping = true;
            }

            if (skipping) {
                const int64_t target = firstAudioPts - soak;
                const int64_t keyPts = qualifyingKeyPts(target);
                if (keyPts != noPts) {
                    dropped += eraseVideoBeforeKey(target);
                    eraseAudioBefore(keyPts - soak);
                    return finish("skip_gop", (firstAudioPts - firstKeyPts) / 90);
                }
#ifdef MUT_LIVE_AV_START_DROP_WHILE_SKIPPING
                // Pre-fix: every wait while skipping discarded pts < target, so a
                // timeout had already thrown away the original key and popFirstVideo
                // emitted a non-key. G6 must fail.
                dropped += dropVideoBeforePts(target);
#endif
            }

#ifndef MUT_LIVE_AV_START_GATE_UNBOUNDED
            if (gateStart >= 0) {
                const int64_t waited = stallNow() - gateStart;
                const bool noAudio = firstAudioPts == noPts;
                if (noAudio) {
                    if (waited >= noAudioWaitMs_)
                        return reopenNoAudio(waited);
                    gateDeadlineMs_.store(gateStart + noAudioWaitMs_);
                } else if (skipping) {
                    // 1500 ms must not present a lead over ~200 ms. Keep demuxing
                    // for a skip_gop key (enough to reach a ~5 s audio start),
                    // then reopen. The kill switch presents at 1500 ms.
                    const int64_t bound = presentLongLead_ ? kLiveAvStartGateTotalMs : longLeadWaitMs_;
                    const int64_t leadMs = (firstAudioPts - firstKeyPts) / 90;
                    if (waited >= bound)
                        return reopenLongLead(leadMs, waited);
                    gateDeadlineMs_.store(gateStart + bound);
                } else if (waited >= kLiveAvStartGateTotalMs) {
                    const int64_t leadMs = firstKeyPts == noPts ? -1 : (firstAudioPts - firstKeyPts) / 90;
                    if (firstKeyPts != noPts && firstAudioPts != noPts &&
                        (firstAudioPts - firstKeyPts) > soak)
                        return reopenLongLead(leadMs, waited);
                    return finish("timeout", leadMs);
                } else {
                    gateDeadlineMs_.store(gateStart + kLiveAvStartGateTotalMs);
                }
            }
#endif
            const DemuxStep step = demuxOne(err);
            if (step == DemuxStep::Ok && skipping)
                misterplex::spelunkr::noteLiveRemuxSkipGopProgress();
            if (step == DemuxStep::Error) {
                gateDeadlineMs_.store(0);
                gateWake_ = false;
                return DemuxStep::Error;
            }
            if (step == DemuxStep::FillTimeout) {
                clearAvioExitLatch();
                fillDeadlineMs_.store(0);
                continue;
            }
            if (step == DemuxStep::Eof) {
                if (firstKeyPts == noPts) {
                    firstKeyPts = firstQueuedVideoKeyPts();
                    if (firstKeyPts != noPts && gateStart < 0)
                        gateStart = stallNow();
                }
                if (firstAudioPts == noPts)
                    firstAudioPts = firstQueuedAudioPts();
                if (firstAudioPts != noPts && firstKeyPts != noPts) {
                    const int64_t lead = firstAudioPts - firstKeyPts;
                    if (lead <= soak)
                        return finish("pass", lead / 90);
                    const int64_t target = firstAudioPts - soak;
                    const int64_t keyPts = qualifyingKeyPts(target);
                    if (keyPts != noPts) {
                        dropped += eraseVideoBeforeKey(target);
                        eraseAudioBefore(keyPts - soak);
                        return finish("skip_gop", lead / 90);
                    }
                    return reopenLongLead(lead / 90, gateStart >= 0 ? stallNow() - gateStart : -1);
                }
                if (firstAudioPts == noPts)
                    return reopenNoAudio(gateStart >= 0 ? stallNow() - gateStart : -1);
                const int64_t leadMs = firstKeyPts == noPts ? -1 : (firstAudioPts - firstKeyPts) / 90;
                return finish("timeout", leadMs);
            }
        }
    }

    DemuxStep demuxOne(std::string& err) {
        // When ready_ is empty this is the original one-packet pull. During the
        // post-picture fill, ready_ already holds video; still drain codecs then
        // read so audio can catch up.
        if (videoPending_)
            takeVideo();
        if (audioPending_)
            takeAudio();
        if (ready_.empty()) {
            if (videoPending_ || audioPending_)
                return DemuxStep::Ok;
        }
        if (drained_)
            return DemuxStep::Eof;
        if (abort_.load() && !stallAbort_) {
            err = "cancelled";
            return DemuxStep::Error;
        }
#ifndef MUT_LIVE_FILL_SWALLOWS_CANCEL
        // Cancel is polled from onInterrupt, which is not in this loop. If a
        // prefetch/zap sets cancelled_() while the first-spawn fill deadline is
        // armed, the next demux must return Error here. Otherwise read() treats
        // the interrupt as a fill timeout, clears the deadline, re-arms it, and
        // never leaves this loop.
        if (cancelled_ && cancelled_()) {
            openStopped_ = true;
            err = "cancelled";
            return DemuxStep::Error;
        }
#endif
        if (fillDeadlineHit())
            return DemuxStep::FillTimeout;
        if (fmtA_) {
            if (!pullDash(err))
                return DemuxStep::Error;
            return DemuxStep::Ok;
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
            return DemuxStep::Eof;
        }
        if (r < 0) {
#ifdef MUT_LIVE_FILL_SWALLOWS_CANCEL
            // Old classification: any AVERROR_EXIT while the fill deadline is
            // armed is a fill timeout, including cancel and stall. read() then
            // re-arms and livelocks. Tests must FAIL.
            if (fillDeadlineHit() || (r == AVERROR_EXIT && fillDeadlineMs_.load() > 0))
                return DemuxStep::FillTimeout;
#else
            // AVERROR_EXIT is a fill timeout only when the interrupt was the
            // fill deadline. Cancel, the open cap, and a stall abort are errors
            // even while that deadline is armed (a cancel used to be swallowed
            // and the prefetch read livelocked).
            if (abort_.load() || openCapped_ || (cancelled_ && cancelled_())) {
                if (cancelled_ && cancelled_())
                    openStopped_ = true;
                err = "cancelled";
                return DemuxStep::Error;
            }
            if (stallAbort_) {
                stallAbort_ = false;
                lastAvErr_ = misterplex::netresume::kStallAbortAv;
                lastHttp_ = 0;
                err = "stall";
                return DemuxStep::Error;
            }
            if (gateWake_ && !fillDeadlineHit()) {
                gateWake_ = false;
                clearAvioExitLatch();
                return DemuxStep::Ok;
            }
            if (fillDeadlineHit() || (r == AVERROR_EXIT && fillDeadlineMs_.load() > 0))
                return DemuxStep::FillTimeout;
#endif
            if (stallAbort_) {
                stallAbort_ = false;
                lastAvErr_ = misterplex::netresume::kStallAbortAv;
                lastHttp_ = 0;
                err = "stall";
                return DemuxStep::Error;
            }
            lastAvErr_ = r;
            err = abort_.load() ? "cancelled" : "read: " + avErr(r);
            lastHttp_ = httpStatusOf(r, err);
            return DemuxStep::Error;
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
        } else if (liveRemux_ && !adec_ && !fmtA_) {
            // MPEG-TS can publish an audio PID after find_stream_info. Retry once
            // the stream list grows or codecpar settles.
            openAudio(-1);
            if (adec_ && pkt_->stream_index == aidx_ && avcodec_send_packet(adec_, pkt_) == 0)
                takeAudio();
        }
        av_packet_unref(pkt_);
        return DemuxStep::Ok;
    }

    AVFormatContext* audioFmt() const { return fmtA_ ? fmtA_ : fmt_; }

    static bool isAudioStream(AVFormatContext* af, int i) {
        return af && i >= 0 && i < static_cast<int>(af->nb_streams) && af->streams[i] &&
               af->streams[i]->codecpar && af->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
    }

    static int firstAudioIndex(AVFormatContext* af) {
        if (!af)
            return -1;
        for (unsigned i = 0; i < af->nb_streams; ++i) {
            if (isAudioStream(af, static_cast<int>(i)))
                return static_cast<int>(i);
        }
        return -1;
    }

    int audioIndexOrNone() const {
        return isAudioStream(audioFmt(), aidx_) ? aidx_ : -1;
    }

    void openAudio(int preferred) {
        aidx_ = -1;
#ifdef MUT_LIVE_SPLIT_NO_AIDX
        (void)preferred;
        // glass2 VEVO: skip the split/fmtA_ assignment and leave av_find_best_stream's
        // AVERROR in aidx_ on the muxed-related search (audio_idx=-1381258232 decoder=0).
        if (fmtA_)
            return;
        AVFormatContext* af = audioFmt();
        if (!af)
            return;
        aidx_ = av_find_best_stream(af, AVMEDIA_TYPE_AUDIO, -1, vidx_, nullptr, 0);
        if (aidx_ < 0)
            return;
#else
        AVFormatContext* af = audioFmt();
        if (!af)
            return;
        if (!fmtA_ && isAudioStream(af, preferred))
            aidx_ = preferred;
        else {
            // Live remux and separate-audio opens: do not restrict to the video program.
            // Split ffmpeg (-map 1:a:0) can put AAC in a second MPEG-TS program; related=vidx_
            // then returns AVERROR and used to leave that code in aidx_ (glass2 VEVO/CMT).
            const int related = (liveRemux_ || fmtA_) ? -1 : vidx_;
            const int found = av_find_best_stream(af, AVMEDIA_TYPE_AUDIO, -1, related, nullptr, 0);
            aidx_ = isAudioStream(af, found) ? found : firstAudioIndex(af);
        }
        if (!isAudioStream(af, aidx_)) {
            aidx_ = -1;
            return;
        }
#endif
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

    // Push SWR leftover PCM into ready_ before a tagged-disc flush so the
    // ring reserve is not thrown away with swr_free (H mid-soak hole).
    void drainSwrResidual() {
        if (!swr_ || !splitLivePace_)
            return;
        const int leftover = swr_get_out_samples(swr_, 0);
        if (leftover <= 0)
            return;
        MediaPacket m;
        m.kind = MediaPacket::Kind::Audio;
        m.data.resize(static_cast<size_t>(leftover) * 4u);
        uint8_t* dst = m.data.data();
        const int got = swr_convert(swr_, &dst, leftover, nullptr, 0);
        if (got <= 0)
            return;
        m.data.resize(static_cast<size_t>(got) * 4u);
        m.pts90k = (lastAudioEmitPts_ != misterplex::live_disc::kNoPts90k)
                       ? lastAudioEmitPts_
                       : disc_.lastEmit;
        if (liveRemux_)
            misterplex::live_disc::notePcmBytes(disc_, m.data.size());
        if (splitLivePace_)
            pacedAudioBytes_ += static_cast<int64_t>(m.data.size());
        pushReady(std::move(m));
    }

    // AD-REC A: live remux (#1066 split HLS) rebases audio/PTS at every discontinuity
    // the way a zap/tune reopen does, then keeps playing (play-all splice).
    void applyLiveDisc(MediaPacket& m, bool discFlag) {
        if (!liveRemux_ || m.pts90k < 0)
            return;
        const auto playlist = discFlag ? misterplex::live_disc::DiscKind::DiscontinuityTag
                                       : misterplex::live_disc::DiscKind::None;
        const auto kind = misterplex::live_disc::notePacket(disc_, m.pts90k, playlist,
                                                            m.kind == MediaPacket::Kind::Audio);
        // lastEmit is already clampHdmiEmit()'d so the ring never sees a
        // non-monotonic / oversized jump (glass 47abfbbb client_write).
        m.pts90k = disc_.lastEmit;
#ifndef MUT_LIVE_NO_DISC_REBASE
        if (kind == misterplex::live_disc::DiscKind::None)
            return;
        m.discontinuity = true;
        // Glass H r3: flushing adec + freeing SWR on pts_jump dropped residual
        // PCM while interleave-0 video kept moving (2.64 s HDMI hole @58.38,
        // vfps≈31). Keep the decoder/SWR on a PTS-only jump (same codec);
        // drain residual before a tagged discontinuity flush. Re-arm the
        // video-lead cap so the next audio AU starts with the picture.
#ifndef MUT_LIVE_FLUSH_PCM_ON_DISC
        if (kind == misterplex::live_disc::DiscKind::PtsJump &&
            misterplex::live_disc::keepPcmOnPtsJump()) {
            lastAudioEmitPts_ = misterplex::live_disc::kNoPts90k;
        } else {
            drainSwrResidual();
            if (adec_)
                avcodec_flush_buffers(adec_);
            swr_free(&swr_);
            lastAudioEmitPts_ = misterplex::live_disc::kNoPts90k;
        }
#else
        if (adec_)
            avcodec_flush_buffers(adec_);
        swr_free(&swr_);
        lastAudioEmitPts_ = misterplex::live_disc::kNoPts90k;
#endif
        std::fprintf(stderr,
                     "misterplexd: live remux disc rebase kind=%s splices=%d emit_pts=%lld\n",
                     misterplex::live_disc::discKindName(kind), disc_.splices,
                     static_cast<long long>(m.pts90k));
#else
        (void)kind;
#endif
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
#ifdef AV_PKT_FLAG_DISCONTINUITY
            const bool discFlag = (out_->flags & AV_PKT_FLAG_DISCONTINUITY) != 0;
#else
            const bool discFlag = false;
#endif
            ++videoFrames_;
            av_packet_unref(out_);
            if (!m.data.empty()) {
                applyLiveDisc(m, discFlag);
                pushReady(std::move(m));
            }
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
            applyLiveDisc(m, false);
            if (liveRemux_)
                misterplex::live_disc::notePcmBytes(disc_, m.data.size());
            if (splitLivePace_)
                pacedAudioBytes_ += static_cast<int64_t>(m.data.size());
            pushReady(std::move(m));
        }
        audioPending_ = readyBytes_ >= kReadyByteCap && !ready_.empty();
    }

    bool openSwr() {
        // Late AAC (Hybrik muxed / GATE7 probe): the first frame can arrive with
        // sample_rate 0. Do not kill the decoder; a later ADTS frame settles it.
        // Split live remux (glass F 1066): codecpar.rate=0 at open made every
        // fill frame fail here, so first-spawn pace logged audio_bytes=0.
        if (frame_->sample_rate <= 0) {
            int rate = 0;
            if (adec_ && adec_->sample_rate > 0)
                rate = adec_->sample_rate;
            else if (audioFmt() && aidx_ >= 0 && audioFmt()->streams[aidx_] &&
                     audioFmt()->streams[aidx_]->codecpar)
                rate = audioFmt()->streams[aidx_]->codecpar->sample_rate;
            if (rate <= 0 && splitLivePace_)
                rate = 48000;
            if (rate <= 0)
                return false;
            frame_->sample_rate = rate;
        }
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
            if (liveRemux_)
                return false;   // keep AAC decoder; retry on the next frame
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
            if (!adec_)
                openAudio(-1);
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
        const int64_t fillDl = fillDeadlineMs_.load();
        if (fillDl > 0 && stallNow() >= fillDl)
            return 1;
#ifndef MUT_LIVE_AV_START_GATE_UNBOUNDED
        const int64_t gateDl = gateDeadlineMs_.load();
        if (gateDl > 0 && stallNow() >= gateDl) {
            gateWake_ = true;
            return 1;
        }
#endif
        const int64_t dl = openDeadlineMs_.load();
        if (dl > 0) {
            const int64_t extra = openExtraBudgetMs_
                                      ? std::max<int64_t>(0, openExtraBudgetMs_->load(std::memory_order_relaxed))
                                      : 0;
            const int64_t now = stallNow();
            if (now >= dl && now - dl >= extra) {
                openCapped_ = true;
                return 1;
            }
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
    misterplex::live_disc::RebaseState disc_{};
    int64_t darNum_ = 0, darDen_ = 0;
    int64_t frameTicks_ = 0, videoFrames_ = 0;
    bool drained_ = false;
    bool videoPending_ = false;
    bool audioPending_ = false;
    bool haveV_ = false, haveA_ = false, vEof_ = false, aEof_ = false;
    bool liveRemux_ = false;
    bool splitLivePace_ = false;
    bool firstVideoDelivered_ = false;
    bool firstSpawnPaced_ = false;
    bool paceLogged_ = false;
    bool avGateDone_ = false;
    bool avGateNoAudioFailed_ = false;
    bool avGateLongLeadFailed_ = false;
    bool gateWake_ = false;
    int64_t avStartGateMs_ = kLiveAvStartGateDefaultMs;
    int64_t noAudioWaitMs_ = kLiveAvNoAudioWaitDefaultMs;
    int64_t longLeadWaitMs_ = kLiveAvLongLeadWaitMs;
    bool presentSilentNoAudio_ = false;
    bool presentLongLead_ = false;
    std::atomic<int64_t> gateDeadlineMs_{0};
    int64_t pacedAudioBytes_ = 0;
    int64_t lastAudioEmitPts_ = misterplex::live_disc::kNoPts90k;
    int64_t lastVideoEmitPts_ = misterplex::live_disc::kNoPts90k;
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
