// OWENB-P1B T5: the OB play route's libav input (owenb_av_source.hpp).
#ifndef MPX_HAVE_LIBAV
#error "owenb_av_source.cpp requires -DMPX_HAVE_LIBAV"
#endif

#include "owenb_av_source.hpp"
#include "av_open_opts.hpp"
#include "ca_bundle.hpp"
#include "gcast/dash_seek.hpp"

#include <atomic>
#include <cstring>
#include <deque>

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
        if (fmt_)
            avformat_close_input(&fmt_);
    }

    void cancel() override { abort_.store(true); }
    bool containerAspect(int64_t& num, int64_t& den) const override {
        if (darNum_ <= 0 || darDen_ <= 0)
            return false;
        num = darNum_;
        den = darDen_;
        return true;
    }

    bool open(const OpenRequest& req, std::string& err) {
        av_log_set_level(AV_LOG_ERROR);
        fmt_ = avformat_alloc_context();
        pkt_ = av_packet_alloc();
        out_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!fmt_ || !pkt_ || !out_ || !frame_) {
            err = "allocation failed";
            return false;
        }
        fmt_->interrupt_callback.callback = [](void* o) -> int {
            return static_cast<AvSource*>(o)->abort_.load() ? 1 : 0;
        };
        fmt_->interrupt_callback.opaque = this;
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
            av_dict_set(dst, "rw_timeout", "20000000", 0);   // remote PMS can be slow to the first byte
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
        const int rc = avformat_open_input(&fmt_, req.url.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (rc < 0) {
            err = "open: " + avErr(rc);
            return false;
        }
        int r = avformat_find_stream_info(fmt_, nullptr);
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
        openAudio();
#ifndef MUT_AV_NO_PROBE_LIMITS
        for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
            if (static_cast<int>(i) == vidx_ || (!fmtA_ && static_cast<int>(i) == aidx_))
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
        if (req.containerSeek && req.startMs > 0) {
            if ((r = gcast::seekInputToMs(fmt_, req.startMs)) < 0) {
                err = "seek: " + avErr(r);
                return false;
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
            if (abort_.load()) {
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
                err = abort_.load() ? "cancelled" : "read: " + avErr(r);
                return SourceRead::Error;
            }
            if (pkt_->stream_index == vidx_) {
                if (av_bsf_send_packet(bsf_, pkt_) == 0)
                    takeVideo();
            } else if (adec_ && pkt_->stream_index == aidx_) {
                if (avcodec_send_packet(adec_, pkt_) == 0)
                    takeAudio();
            }
            av_packet_unref(pkt_);
        }
    }

private:
    AVFormatContext* audioFmt() const { return fmtA_ ? fmtA_ : fmt_; }

    void openAudio() {
        AVFormatContext* af = audioFmt();
        aidx_ = av_find_best_stream(af, AVMEDIA_TYPE_AUDIO, -1, fmtA_ ? -1 : vidx_, nullptr, 0);
        if (aidx_ < 0)
            return;
        AVStream* as = audioFmt()->streams[aidx_];
        const AVCodec* c = avcodec_find_decoder(as->codecpar->codec_id);
        if (!c || !(adec_ = avcodec_alloc_context3(c)) ||
            avcodec_parameters_to_context(adec_, as->codecpar) < 0 || avcodec_open2(adec_, c, nullptr) < 0) {
            avcodec_free_context(&adec_);   // video only: the core plays silence
            aidx_ = -1;
        }
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
                err = abort_.load() ? "cancelled" : "read: " + avErr(r);
                return false;
            }
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

    AVFormatContext* fmt_ = nullptr;
    AVFormatContext* fmtA_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVPacket* vHold_ = nullptr;
    AVPacket* aHold_ = nullptr;
    AVPacket* out_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVBSFContext* bsf_ = nullptr;
    AVCodecContext* adec_ = nullptr;
    SwrContext* swr_ = nullptr;
    int vidx_ = -1, aidx_ = -1;
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
};

} // namespace

std::unique_ptr<IMediaSource> openAvSource(const OpenRequest& req, std::string& err) {
    auto s = std::make_unique<AvSource>();
    if (!s->open(req, err))
        return nullptr;
    return s;
}

} // namespace misterplex::owenb
