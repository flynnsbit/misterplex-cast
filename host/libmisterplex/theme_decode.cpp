#include "libmisterplex/theme_decode.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef MPX_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}
#endif

namespace misterplex {

#ifndef MPX_HAVE_LIBAV
std::vector<uint8_t> themeDecodePcm(const uint8_t*, std::size_t) {
    return {};
}
#else

namespace {

struct MemBuf {
    const uint8_t* p = nullptr;
    std::size_t n = 0;
    std::size_t off = 0;
};

int memRead(void* opaque, uint8_t* buf, int bufSize) {
    auto* m = static_cast<MemBuf*>(opaque);
    if (!m || bufSize <= 0 || m->off >= m->n)
        return AVERROR_EOF;
    const std::size_t take = std::min(static_cast<std::size_t>(bufSize), m->n - m->off);
    std::memcpy(buf, m->p + m->off, take);
    m->off += take;
    return static_cast<int>(take);
}

int64_t memSeek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<MemBuf*>(opaque);
    if (!m)
        return -1;
    if (whence & AVSEEK_SIZE)
        return static_cast<int64_t>(m->n);
    whence &= ~AVSEEK_FORCE;
    std::size_t base = 0;
    if (whence == SEEK_CUR)
        base = m->off;
    else if (whence == SEEK_END)
        base = m->n;
    else if (whence != SEEK_SET)
        return -1;
    const int64_t pos = static_cast<int64_t>(base) + offset;
    if (pos < 0 || static_cast<std::size_t>(pos) > m->n)
        return -1;
    m->off = static_cast<std::size_t>(pos);
    return pos;
}

} // namespace

std::vector<uint8_t> themeDecodePcm(const uint8_t* data, std::size_t n) {
#ifdef MUT_THEME_DECODE_PASSTHROUGH
    if (!data || n == 0)
        return {};
    return std::vector<uint8_t>(data, data + n);
#else
    if (!data || n < 16)
        return {};

    av_log_set_level(AV_LOG_ERROR);

    MemBuf mem{data, n, 0};
    constexpr int kIo = 4096;
    auto* ioBuf = static_cast<unsigned char*>(av_malloc(kIo));
    if (!ioBuf)
        return {};
    AVIOContext* pb = avio_alloc_context(ioBuf, kIo, 0, &mem, &memRead, nullptr, &memSeek);
    if (!pb) {
        av_free(ioBuf);
        return {};
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        return {};
    }
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    std::vector<uint8_t> pcm;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    auto cleanup = [&]() {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&dec);
        if (fmt) {
            fmt->pb = nullptr;
            avformat_close_input(&fmt);
        }
        if (pb) {
            av_freep(&pb->buffer);
            avio_context_free(&pb);
        }
    };

    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
        cleanup();
        return {};
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        cleanup();
        return {};
    }
    const int best = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (best < 0) {
        cleanup();
        return {};
    }
    AVStream* st = fmt->streams[best];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        cleanup();
        return {};
    }
    dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 || avcodec_open2(dec, codec, nullptr) < 0) {
        cleanup();
        return {};
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        cleanup();
        return {};
    }

    auto appendFrame = [&](AVFrame* fr) -> bool {
        if (!fr || fr->nb_samples <= 0)
            return true;
        int rate = fr->sample_rate > 0 ? fr->sample_rate : dec->sample_rate;
        if (rate <= 0)
            rate = 48000;
        if (!swr) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &fr->ch_layout,
                                    static_cast<AVSampleFormat>(fr->format), rate, 0, nullptr) < 0 ||
                swr_init(swr) < 0)
                return false;
        }
        const int cap = swr_get_out_samples(swr, fr->nb_samples);
        if (cap <= 0 || cap > 48000)
            return false;
        std::vector<uint8_t> chunk(static_cast<std::size_t>(cap) * 4);
        uint8_t* dst = chunk.data();
        const int samples =
            swr_convert(swr, &dst, cap, const_cast<const uint8_t**>(fr->extended_data), fr->nb_samples);
        if (samples < 0)
            return false;
        const std::size_t bytes = static_cast<std::size_t>(samples) * 4;
#ifdef MUT_THEME_DECODE_NO_CAP
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(bytes));
        return true;
#else
        const std::size_t room = kThemePcmByteCap - pcm.size();
        const std::size_t take = std::min(bytes, room);
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
        return pcm.size() < kThemePcmByteCap;
#endif
    };

    int err = 0;
    while ((err = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index != best) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec, pkt) < 0) {
            av_packet_unref(pkt);
            cleanup();
            return {};
        }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(dec, frame) == 0) {
            if (!appendFrame(frame)) {
                cleanup();
                if (pcm.size() >= kThemePcmByteCap)
                    return pcm;
                return {};
            }
            if (pcm.size() >= kThemePcmByteCap) {
                cleanup();
                return pcm;
            }
        }
    }
    avcodec_send_packet(dec, nullptr);
    while (avcodec_receive_frame(dec, frame) == 0) {
        if (!appendFrame(frame) || pcm.size() >= kThemePcmByteCap)
            break;
    }
    cleanup();
    if (pcm.size() < 4)
        return {};
    pcm.resize(pcm.size() & ~std::size_t{3});
    return pcm;
#endif
}

#endif

} // namespace misterplex
