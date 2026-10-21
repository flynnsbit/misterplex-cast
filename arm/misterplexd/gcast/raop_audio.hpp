// GCAST AirPlay audio: RAOP (AirTunes) RTP audio -> 48 kHz s16le stereo PCM.
// UxPlay hands audio_process() decrypted RTP payloads with ct = 2 ALAC, 4 AAC-LC, 8 AAC-ELD (44.1 kHz,
// 352 / 1024 / 480 samples per frame). RaopDecoder decodes with libavcodec (aac + alac, already in the
// static ffmpeg-arm prefix), resamples to the 48 kHz stereo s16 the MrAudio/HDMI path and gcast-xcode
// use, and applies the sender volume. PcmFanout keeps a bounded per-listener queue for the live
// /raop.wav stream that the AirPlay cast plays through the normal gcast-xcode/Plex path.
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef MPX_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

namespace misterplex {
namespace gcast {

enum class RaopCodec { None, Alac, AacLc, AacEld };

constexpr int kRaopOutRate = 48000;
constexpr int kRaopOutCh = 2;
constexpr int kRaopBytesPerSec = kRaopOutRate * kRaopOutCh * 2;

inline RaopCodec raopCodecFromCt(unsigned ct) {
    switch (ct) {
    case 2: return RaopCodec::Alac;
    case 4: return RaopCodec::AacLc;
    case 8: return RaopCodec::AacEld;
    default: return RaopCodec::None;
    }
}
inline const char* raopCodecName(RaopCodec c) {
    switch (c) {
    case RaopCodec::Alac: return "alac";
    case RaopCodec::AacLc: return "aac-lc";
    case RaopCodec::AacEld: return "aac-eld";
    default: return "none";
    }
}

// Decoder extradata for the fixed UxPlay/iOS stream formats (44.1 kHz stereo).
// AAC AudioSpecificConfig: LC 0x12 0x10; ELD (AOT 39 escape, 44.1k, stereo, 480 spf) F8 E8 50 00.
// ALAC: 36-byte 'alac' atom = size + tag + version + 24-byte ALACSpecificConfig
// (fmtp 96 352 0 16 40 10 14 2 255 0 0 44100).
inline std::vector<uint8_t> raopExtradata(RaopCodec c) {
    switch (c) {
    case RaopCodec::AacLc: return {0x12, 0x10};
    case RaopCodec::AacEld: return {0xF8, 0xE8, 0x50, 0x00};
    case RaopCodec::Alac:
        return {0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x01, 0x60,   // frameLength 352
                0x00, 0x10,               // compatibleVersion, bitDepth 16
                0x28, 0x0A, 0x0E, 0x02,   // pb mb kb, channels 2
                0x00, 0xFF,               // maxRun 255
                0x00, 0x00, 0x00, 0x00,   // maxFrameBytes
                0x00, 0x00, 0x00, 0x00,   // avgBitRate
                0x00, 0x00, 0xAC, 0x44};  // sampleRate 44100
    default: return {};
    }
}

// AirPlay volume is dB: 0 = full, -30 = quietest, -144 = mute. Linear gain in [0, 1].
inline float raopGainFromDb(float db) {
#ifndef MUT_RA_VOL_IGNORE
    if (!(db > -30.0f))   // also -144 mute and NaN
        return 0.0f;
    if (db >= 0.0f)
        return 1.0f;
    return std::pow(10.0f, db / 20.0f);
#else
    (void)db;
    return 1.0f;
#endif
}

inline void raopApplyGain(int16_t* s, size_t n, float g) {
    if (g >= 0.9999f)
        return;
    for (size_t i = 0; i < n; ++i)
        s[i] = static_cast<int16_t>(std::lrint(static_cast<float>(s[i]) * g));
}

// 44-byte RIFF/WAVE header for an endless 48k s16 stereo stream (sizes 0xFFFFFFFF = unknown).
inline std::string wavStreamHeader() {
    std::string h(44, '\0');
    auto le32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o + i] = static_cast<char>(v >> (8 * i)); };
    auto le16 = [&](size_t o, uint16_t v) { h[o] = static_cast<char>(v); h[o + 1] = static_cast<char>(v >> 8); };
    std::memcpy(&h[0], "RIFF", 4); le32(4, 0xFFFFFFFFu); std::memcpy(&h[8], "WAVEfmt ", 8);
    le32(16, 16); le16(20, 1); le16(22, kRaopOutCh); le32(24, kRaopOutRate); le32(28, kRaopBytesPerSec);
    le16(32, kRaopOutCh * 2); le16(34, 16); std::memcpy(&h[36], "data", 4); le32(40, 0xFFFFFFFFu);
    return h;
}

// One producer (RAOP audio thread), N listeners (live HTTP clients). Each listener has a bounded queue;
// a slow listener loses its oldest bytes past the cap (live audio, no growing backlog).
class PcmFanout {
public:
    explicit PcmFanout(size_t capBytes = kRaopBytesPerSec * 2) : cap_(capBytes & ~size_t(3)) {}
    struct Listener {
        std::deque<uint8_t> q;
        uint64_t dropped = 0;
        bool closed = false;
    };
    std::shared_ptr<Listener> attach() {
        auto l = std::make_shared<Listener>();
        std::lock_guard<std::mutex> g(mu_);
        ls_.push_back(l);
        return l;
    }
    void detach(const std::shared_ptr<Listener>& l) {
        std::lock_guard<std::mutex> g(mu_);
        ls_.erase(std::remove(ls_.begin(), ls_.end(), l), ls_.end());
    }
    void push(const uint8_t* p, size_t n) {
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& l : ls_) {
                l->q.insert(l->q.end(), p, p + n);
#ifndef MUT_RA_FANOUT_NO_CAP
                if (l->q.size() > cap_) {
                    size_t over = (l->q.size() - cap_ + 3) & ~size_t(3);   // keep s16 stereo alignment
                    over = std::min(over, l->q.size());
                    l->q.erase(l->q.begin(), l->q.begin() + static_cast<long>(over));
                    l->dropped += over;
                }
#endif
            }
        }
        cv_.notify_all();
    }
    void flush() {   // sender flush (seek/pause): drop queued audio, keep listeners
        std::lock_guard<std::mutex> g(mu_);
        for (auto& l : ls_)
#ifndef MUT_RA_NO_FLUSH
            l->q.clear();
#else
            (void)l;
#endif
    }
    void closeAll() {
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& l : ls_)
                l->closed = true;
        }
        cv_.notify_all();
    }
    // Up to n bytes (frame aligned) within waitMs. 0 = timeout, -1 = closed and empty.
    long pop(const std::shared_ptr<Listener>& l, uint8_t* out, size_t n, int waitMs) {
        std::unique_lock<std::mutex> g(mu_);
        cv_.wait_for(g, std::chrono::milliseconds(waitMs), [&] { return !l->q.empty() || l->closed; });
        if (l->q.empty())
            return l->closed ? -1 : 0;
        n = std::min(n, l->q.size()) & ~size_t(3);
        if (n == 0)
            return 0;
        std::copy(l->q.begin(), l->q.begin() + static_cast<long>(n), out);
        l->q.erase(l->q.begin(), l->q.begin() + static_cast<long>(n));
        return static_cast<long>(n);
    }
    size_t listeners() {
        std::lock_guard<std::mutex> g(mu_);
        return ls_.size();
    }

private:
    size_t cap_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Listener>> ls_;
};

#ifdef MPX_HAVE_LIBAV
// libavcodec decode + swresample to 48k s16 stereo. Re-opens when the stream codec changes.
class RaopDecoder {
public:
    ~RaopDecoder() { close(); }
    RaopCodec codec() const { return codec_; }
    void setVolumeDb(float db) { gain_ = raopGainFromDb(db); }
    float gain() const { return gain_; }
    uint64_t frames() const { return frames_; }
    uint64_t errors() const { return errors_; }
    // Decode one RTP payload, append 48k s16le stereo PCM to out. false on error / unknown ct.
    bool decode(unsigned ct, const uint8_t* data, int len, std::vector<uint8_t>& out) {
        const RaopCodec c = raopCodecFromCt(ct);
        if (c == RaopCodec::None || !data || len <= 0)
            return false;
        if (c != codec_ && !open(c))
            return false;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt)
            return false;
        // copy with padding: the bitstream readers may overread AV_INPUT_BUFFER_PADDING_SIZE bytes
        if (av_new_packet(pkt, len) < 0) {
            av_packet_free(&pkt);
            return false;
        }
        std::memcpy(pkt->data, data, static_cast<size_t>(len));
        const int r = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        if (r < 0) {
            ++errors_;
            return false;
        }
        bool any = false;
        while (avcodec_receive_frame(ctx_, frm_) == 0) {
            any = convert(out) || any;
            av_frame_unref(frm_);
        }
        return any;
    }
    void flush() {
        if (ctx_)
            avcodec_flush_buffers(ctx_);
        if (swr_)
            swr_free(&swr_);
        swrKey_ = -1;
    }
    void close() {
        if (swr_)
            swr_free(&swr_);
        if (frm_)
            av_frame_free(&frm_);
        if (ctx_)
            avcodec_free_context(&ctx_);
        codec_ = RaopCodec::None;
        swrKey_ = -1;
    }

private:
    bool open(RaopCodec c) {
        close();
        const AVCodec* dec = avcodec_find_decoder(c == RaopCodec::Alac ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC);
        if (!dec || !(ctx_ = avcodec_alloc_context3(dec)))
            return false;
        const auto ex = raopExtradata(c);
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(ex.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(ctx_->extradata, ex.data(), ex.size());
        ctx_->extradata_size = static_cast<int>(ex.size());
        ctx_->sample_rate = 44100;
        av_channel_layout_default(&ctx_->ch_layout, 2);
        if (avcodec_open2(ctx_, dec, nullptr) < 0) {
            avcodec_free_context(&ctx_);
            return false;
        }
        frm_ = av_frame_alloc();
        codec_ = c;
        return frm_ != nullptr;
    }
    bool convert(std::vector<uint8_t>& out) {
        const int inRate = frm_->sample_rate > 0 ? frm_->sample_rate : 44100;
        const int key = inRate * 64 + frm_->format * 8 + frm_->ch_layout.nb_channels;
        if (!swr_ || key != swrKey_) {
            if (swr_)
                swr_free(&swr_);
            AVChannelLayout outL;
            av_channel_layout_default(&outL, kRaopOutCh);
#ifndef MUT_RA_NO_RESAMPLE
            const int outRate = kRaopOutRate;
#else
            const int outRate = inRate;
#endif
            if (swr_alloc_set_opts2(&swr_, &outL, AV_SAMPLE_FMT_S16, outRate, &frm_->ch_layout,
                                    static_cast<AVSampleFormat>(frm_->format), inRate, 0, nullptr) < 0 ||
                swr_init(swr_) < 0) {
                swr_free(&swr_);
                ++errors_;
                return false;
            }
            swrKey_ = key;
        }
        const int maxOut = swr_get_out_samples(swr_, frm_->nb_samples) + 32;
        const size_t base = out.size();
        out.resize(base + static_cast<size_t>(maxOut) * kRaopOutCh * 2);
        uint8_t* dst = out.data() + base;
        const int got = swr_convert(swr_, &dst, maxOut, const_cast<const uint8_t**>(frm_->extended_data),
                                    frm_->nb_samples);
        if (got < 0) {
            out.resize(base);
            ++errors_;
            return false;
        }
        out.resize(base + static_cast<size_t>(got) * kRaopOutCh * 2);
        raopApplyGain(reinterpret_cast<int16_t*>(out.data() + base), static_cast<size_t>(got) * kRaopOutCh, gain_);
        ++frames_;
        return got > 0;
    }
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frm_ = nullptr;
    SwrContext* swr_ = nullptr;
    int swrKey_ = -1;
    RaopCodec codec_ = RaopCodec::None;
    float gain_ = 1.0f;
    uint64_t frames_ = 0, errors_ = 0;
};
#endif

}  // namespace gcast
}  // namespace misterplex
