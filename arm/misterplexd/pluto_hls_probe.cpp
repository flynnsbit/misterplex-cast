// Pluto stitcher HLS variant probe via in-process libav (AES-128 decrypt in demuxer).
#include "spelunkr/pluto_live.hpp"

#if defined(MPX_HAVE_LIBAV) || defined(MPX_HAVE_PLUTO_HLS_PROBE_LIBAV)

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include "ca_bundle.hpp"
#include "spelunkr/broadcast.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace misterplex {
namespace spelunkr {
namespace pluto_live {

namespace {

inline constexpr char kPlutoHlsProto[] = "https,tls,tcp,crypto,hls";
inline constexpr int64_t kProbeDeadlineMs = 8000;
inline constexpr int64_t kRwTimeoutUs = 8'000'000;

struct ProbeInterrupt {
    std::function<bool()> cancelled;
    int64_t deadlineMs = 0;
};

static int interruptCb(void* opaque) {
    auto* ctx = static_cast<ProbeInterrupt*>(opaque);
    if (ctx->cancelled && ctx->cancelled())
        return 1;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    return now >= ctx->deadlineMs ? 1 : 0;
}

static float fpsFromStream(const AVStream* st) {
    if (!st)
        return 0.f;
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        return static_cast<float>(av_q2d(st->avg_frame_rate));
    if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
        return static_cast<float>(av_q2d(st->r_frame_rate));
    return 0.f;
}

static int h264ProfileIdc(int ffProfile) {
    if (ffProfile == AV_PROFILE_H264_CONSTRAINED_BASELINE || ffProfile == AV_PROFILE_H264_BASELINE)
        return 66;
    if (ffProfile == AV_PROFILE_H264_MAIN)
        return 77;
    if (ffProfile == AV_PROFILE_H264_HIGH)
        return 100;
    if (ffProfile > 0 && ffProfile <= 255)
        return ffProfile;
    return -1;
}

static std::string avc1FromProfileLevel(int profile, int level) {
    const int idc = h264ProfileIdc(profile);
    if (idc < 0 || !plutoHlsCodecProfileLevelValid(profile, level))
        return {};
    SpsInfo fake;
    fake.profile_idc = static_cast<uint8_t>(idc);
    fake.level_idc = static_cast<uint8_t>(level);
    return avc1CodecFromSps(fake);
}

static PlutoStreamProbe probeFromVideoStream(AVFormatContext* fmt) {
    PlutoStreamProbe out;
    int vidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidx = static_cast<int>(i);
            break;
        }
    }
    if (vidx < 0)
        return out;
    const AVStream* st = fmt->streams[static_cast<unsigned>(vidx)];
    const AVCodecParameters* par = st->codecpar;
    if (par->codec_id != AV_CODEC_ID_H264)
        return out;
    if (par->width <= 0 || par->height <= 0)
        return out;
    if (par->profile == AV_PROFILE_UNKNOWN || !plutoHlsCodecProfileLevelValid(par->profile, par->level))
        return out;
    out.vcodec = avc1FromProfileLevel(par->profile, par->level);
    if (out.vcodec.empty())
        return out;
    out.width = par->width;
    out.height = par->height;
    out.fps = fpsFromStream(st);
    out.ok = true;
    return out;
}

}  // namespace

PlutoStreamProbe plutoHlsProbeLibav(const PlutoHlsProbeRequest& req) {
    PlutoStreamProbe fail;
    if (!plutoPlayUrlAllowed(req.mediaUrl))
        return fail;
    ProbeInterrupt ic;
    ic.cancelled = req.cancelled;
    ic.deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count() +
                     kProbeDeadlineMs;

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt)
        return fail;
    fmt->interrupt_callback = {interruptCb, &ic};

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", kPlutoHlsProto, 0);
    av_dict_set(&opts, "probesize", "400000", 0);
    av_dict_set(&opts, "analyzeduration", "3000000", 0);
    av_dict_set(&opts, "rw_timeout", std::to_string(kRwTimeoutUs).c_str(), 0);
    av_dict_set(&opts, "tls_verify", "1", 0);
    const std::string ca = mpx::caBundlePath();
    if (!ca.empty())
        av_dict_set(&opts, "ca_file", ca.c_str(), 0);
    if (req.allowSessionBearer && plutoAttachSessionBearer(req.mediaUrl, req.sessionToken)) {
        const std::string hdr = "Authorization: Bearer " + req.sessionToken + "\r\n";
        av_dict_set(&opts, "headers", hdr.c_str(), 0);
    }

    int rc = avformat_open_input(&fmt, req.mediaUrl.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        avformat_free_context(fmt);
        return fail;
    }
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        avformat_close_input(&fmt);
        return fail;
    }
    PlutoStreamProbe out = probeFromVideoStream(fmt);
    avformat_close_input(&fmt);
    return out;
}

}  // namespace pluto_live
}  // namespace spelunkr
}  // namespace misterplex

#else

namespace misterplex {
namespace spelunkr {
namespace pluto_live {

PlutoStreamProbe plutoHlsProbeLibav(const PlutoHlsProbeRequest&) { return PlutoStreamProbe(); }

}  // namespace pluto_live
}  // namespace spelunkr
}  // namespace misterplex

#endif