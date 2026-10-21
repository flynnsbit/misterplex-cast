// Split DASH: video and audio are opened as themselves and seeked by time.
// The audio URL must not grow a raw byte offset.
#include "gcast/dash_seek.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

using misterplex::gcast::DashOpen;
using misterplex::gcast::planDashSeek;
using misterplex::gcast::seekInputToMs;
using misterplex::gcast::splitYtAv;
using misterplex::gcast::ytAvPlayable;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (c)                                                                                         \
            ++g_pass;                                                                                  \
        else {                                                                                         \
            ++g_fail;                                                                                  \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                    \
        }                                                                                              \
    } while (0)

static std::string slurp(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int64_t packetMs(AVFormatContext* fmt, AVPacket* pkt) {
    if (pkt->pts == AV_NOPTS_VALUE)
        return -1;
    return av_rescale_q(pkt->pts, fmt->streams[pkt->stream_index]->time_base, AVRational{1, 1000});
}

int main() {
    const std::string video = "https://example.invalid/videoplayback?id=v";
    const std::string audio = "https://example.invalid/videoplayback?id=a";
    const auto plan = planDashSeek(DashOpen{video, audio, 180000});
    CHECK(plan.videoUrl == video && plan.audioUrl == audio);
    CHECK(!plan.byteSeek);
    CHECK(plan.seekTs == 180000LL * 1000);
    CHECK(plan.audioUrl.find("bytes=") == std::string::npos);
    std::string v, a;
    CHECK(splitYtAv(ytAvPlayable(video, audio), v, a) && v == video && a == audio);

    const std::string srcRoot = MPX_SOURCE_ROOT;
    const std::string av = slurp(srcRoot + "/arm/misterplexd/owenb_av_source.cpp");
    const std::string seek = slurp(srcRoot + "/arm/misterplexd/gcast/dash_seek.hpp");
    CHECK(av.find("seekInputToMs") != std::string::npos);
    CHECK(av.find("AVSEEK_FLAG_BYTE") == std::string::npos);
    CHECK(seek.find("avformat_seek_file(fmt, -1, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD)") !=
          std::string::npos);

    char dir[] = "/tmp/dash-seek-XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string root = dir;
    const std::string vpath = root + "/v.mp4";
    const std::string apath = root + "/a.m4a";
    const std::string vcmd =
        "ffmpeg -y -loglevel error -f lavfi -i testsrc=size=160x120:rate=25:duration=2 "
        "-c:v libx264 -pix_fmt yuv420p -g 25 -an '" +
        vpath + "'";
    const std::string acmd =
        "ffmpeg -y -loglevel error -f lavfi -i sine=frequency=440:sample_rate=48000:duration=2 "
        "-c:a aac '" +
        apath + "'";
    CHECK(std::system(vcmd.c_str()) == 0);
    CHECK(std::system(acmd.c_str()) == 0);

    const int64_t startMs = 1000;
    const auto local = planDashSeek(DashOpen{vpath, apath, startMs});
    CHECK(local.videoUrl == vpath && local.audioUrl == apath && !local.byteSeek);

    AVFormatContext* vf = nullptr;
    AVFormatContext* af = nullptr;
    CHECK(avformat_open_input(&vf, local.videoUrl.c_str(), nullptr, nullptr) == 0);
    CHECK(avformat_open_input(&af, local.audioUrl.c_str(), nullptr, nullptr) == 0);
    CHECK(vf && af);
    if (vf)
        avformat_find_stream_info(vf, nullptr);
    if (af)
        avformat_find_stream_info(af, nullptr);
    CHECK(seekInputToMs(vf, startMs) >= 0);
    CHECK(seekInputToMs(af, startMs) >= 0);
    AVPacket* pkt = av_packet_alloc();
    CHECK(pkt != nullptr);
    if (vf && pkt && av_read_frame(vf, pkt) == 0) {
        const int64_t ms = packetMs(vf, pkt);
        CHECK(ms >= 0 && ms <= startMs);
        av_packet_unref(pkt);
    } else {
        CHECK(false);
    }
    if (af && pkt && av_read_frame(af, pkt) == 0) {
        const int64_t ms = packetMs(af, pkt);
        CHECK(ms >= 0 && ms <= startMs + 200);
        av_packet_unref(pkt);
    } else {
        CHECK(false);
    }
    av_packet_free(&pkt);
    if (vf)
        avformat_close_input(&vf);
    if (af)
        avformat_close_input(&af);

    std::printf("test_dash_seek: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
