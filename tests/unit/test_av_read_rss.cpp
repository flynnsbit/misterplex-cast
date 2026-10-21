// Repeated MPEG-TS demux, the same calls Owenb AvSource makes (open, find_stream_info,
// h264_mp4toannexb, AAC decode, swr). RSS after the last close must stay near the RSS
// after the first close. MUT_AV_KEEP_PACKETS retains every video AU and must trip the bound.
// ASan's default quarantine also looks like growth; this binary is built without ASan.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

static long rssKb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0)
            kb = std::atol(line + 6);
    }
    std::fclose(f);
    return kb;
}

static int demuxOnce(const char* url, std::vector<std::vector<uint8_t>>& kept) {
    AVFormatContext* fmt = avformat_alloc_context();
    AVDictionary* o = nullptr;
    av_dict_set(&o, "rw_timeout", "20000000", 0);
    if (avformat_open_input(&fmt, url, nullptr, &o) < 0) {
        av_dict_free(&o);
        std::fprintf(stderr, "FAIL open\n");
        return 1;
    }
    av_dict_free(&o);
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::fprintf(stderr, "FAIL stream info\n");
        return 1;
    }
    const int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int a = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, v, nullptr, 0);
    if (v < 0) {
        std::fprintf(stderr, "FAIL no video\n");
        return 1;
    }
    AVBSFContext* bsf = nullptr;
    const AVBitStreamFilter* filt = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filt || av_bsf_alloc(filt, &bsf) < 0 ||
        avcodec_parameters_copy(bsf->par_in, fmt->streams[v]->codecpar) < 0) {
        std::fprintf(stderr, "FAIL bsf\n");
        return 1;
    }
    bsf->time_base_in = fmt->streams[v]->time_base;
    if (av_bsf_init(bsf) < 0) {
        std::fprintf(stderr, "FAIL bsf init\n");
        return 1;
    }
    AVCodecContext* adec = nullptr;
    if (a >= 0) {
        const AVCodec* c = avcodec_find_decoder(fmt->streams[a]->codecpar->codec_id);
        adec = avcodec_alloc_context3(c);
        if (adec && avcodec_parameters_to_context(adec, fmt->streams[a]->codecpar) >= 0)
            avcodec_open2(adec, c, nullptr);
    }
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVPacket* out = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == v && av_bsf_send_packet(bsf, pkt) == 0) {
            while (av_bsf_receive_packet(bsf, out) == 0) {
#ifdef MUT_AV_KEEP_PACKETS
                kept.emplace_back(out->data, out->data + out->size);
#else
                (void)kept;
#endif
                av_packet_unref(out);
            }
        } else if (adec && pkt->stream_index == a && avcodec_send_packet(adec, pkt) == 0) {
            while (avcodec_receive_frame(adec, frame) == 0) {
                if (!swr) {
                    AVChannelLayout outL = AV_CHANNEL_LAYOUT_STEREO;
                    AVChannelLayout inL;
                    if (av_channel_layout_copy(&inL, &frame->ch_layout) == 0) {
                        swr_alloc_set_opts2(&swr, &outL, AV_SAMPLE_FMT_S16, 48000, &inL,
                                            static_cast<AVSampleFormat>(frame->format),
                                            frame->sample_rate, 0, nullptr);
                        av_channel_layout_uninit(&inL);
                        swr_init(swr);
                    }
                }
                if (swr) {
                    const int outMax = swr_get_out_samples(swr, frame->nb_samples);
                    std::vector<uint8_t> pcm(outMax > 0 ? static_cast<size_t>(outMax) * 4u : 0u);
                    uint8_t* dst = pcm.data();
                    if (outMax > 0)
                        swr_convert(swr, &dst, outMax,
                                    const_cast<const uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&frame);
    av_packet_free(&out);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&adec);
    av_bsf_free(&bsf);
    avformat_close_input(&fmt);
    return 0;
}

int main() {
    const char* path = "/tmp/misterplex-av-rss.ts";
    const std::string gen =
        std::string("ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 "
                    "-f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 -c:v libx264 -preset ultrafast "
                    "-b:v 4000k -c:a aac -b:a 96k -f mpegts ") +
        path;
    if (std::system(gen.c_str()) != 0) {
        std::fprintf(stderr, "FAIL ffmpeg generate\n");
        return 1;
    }
    std::vector<std::vector<uint8_t>> kept;
    if (demuxOnce(path, kept) != 0)
        return 1;
    const long base = rssKb();
    for (int i = 0; i < 3; ++i) {
        if (demuxOnce(path, kept) != 0)
            return 1;
    }
    const long end = rssKb();
    const long delta = end - base;
    std::printf("test_av_read_rss: base_kb %ld end_kb %ld delta_kb %ld kept %zu\n", base, end, delta,
                kept.size());
    constexpr long kMaxGrowthKb = 8 * 1024;
    if (delta > kMaxGrowthKb) {
        std::fprintf(stderr, "FAIL rss grew %ld kb across repeated demux (limit %ld)\n", delta,
                     kMaxGrowthKb);
        return 1;
    }
    return 0;
}
