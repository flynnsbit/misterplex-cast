// GCAST AirPlay audio: RAOP decode (ALAC / AAC-LC round trip through host libavcodec, AAC-ELD open),
// 44.1k -> 48k resample, volume, flush, PcmFanout cap, WAV header, raop.wav cast routing.
// Mutants: MUT_RA_VOL_IGNORE MUT_RA_FANOUT_NO_CAP MUT_RA_NO_FLUSH MUT_RA_NO_RESAMPLE MUT_CR_RAOP_ANYWAV
#include <cmath>
#include <cstdio>
#include <vector>

#include "gcast/cast_resolve.hpp"
#include "gcast/raop_audio.hpp"

using namespace misterplex::gcast;
static int fails = 0, checks = 0;
#define CHECK(c, what) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s\n", what); } } while (0)

static double rms(const std::vector<uint8_t>& pcm, size_t skipBytes = 0) {
    const int16_t* s = reinterpret_cast<const int16_t*>(pcm.data());
    size_t n = pcm.size() / 2, k = skipBytes / 2;
    double acc = 0;
    for (size_t i = k; i < n; ++i) acc += double(s[i]) * s[i];
    return n > k ? std::sqrt(acc / double(n - k)) : 0;
}

// Encode 1 s of a 1 kHz sine (amp 8000) at 44.1k stereo with a host encoder into raw packets.
static std::vector<std::vector<uint8_t>> encodeSine(AVCodecID id, int frameSize) {
    std::vector<std::vector<uint8_t>> pkts;
    const AVCodec* enc = avcodec_find_encoder(id);
    if (!enc) return pkts;
    AVCodecContext* c = avcodec_alloc_context3(enc);
    c->sample_rate = 44100;
    av_channel_layout_default(&c->ch_layout, 2);
    c->sample_fmt = id == AV_CODEC_ID_ALAC ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_FLTP;
    if (id == AV_CODEC_ID_ALAC) c->frame_size = frameSize;
    c->bit_rate = 128000;
    if (avcodec_open2(c, enc, nullptr) < 0) { avcodec_free_context(&c); return pkts; }
    const int fs = c->frame_size > 0 ? c->frame_size : frameSize;
    AVFrame* f = av_frame_alloc();
    AVPacket* p = av_packet_alloc();
    long t = 0;
    for (int fr = 0; fr * fs < 44100 + 4 * fs; ++fr) {
        f->nb_samples = fs; f->format = c->sample_fmt; f->sample_rate = 44100;
        av_channel_layout_copy(&f->ch_layout, &c->ch_layout);
        av_frame_get_buffer(f, 0);
        for (int i = 0; i < fs; ++i, ++t) {
            const double v = 8000.0 * std::sin(2 * M_PI * 1000.0 * double(t) / 44100.0);
            for (int ch = 0; ch < 2; ++ch) {
                if (c->sample_fmt == AV_SAMPLE_FMT_FLTP) reinterpret_cast<float*>(f->data[ch])[i] = float(v / 32768.0);
                else reinterpret_cast<int16_t*>(f->data[ch])[i] = int16_t(v);
            }
        }
        avcodec_send_frame(c, f);
        av_frame_unref(f);
        while (avcodec_receive_packet(c, p) == 0) { pkts.emplace_back(p->data, p->data + p->size); av_packet_unref(p); }
    }
    av_frame_free(&f); av_packet_free(&p); avcodec_free_context(&c);
    return pkts;
}

// ALAC as iOS frames it: 352 samples/packet. FFmpeg's encoder is fixed at 4096, so build ALAC escape
// (uncompressed) CPE frames by hand: tag CPE, instance 0, 12 unused, has_size 0, shift 0, not-compressed 1,
// 352 x (L16, R16), then ID_END.
static std::vector<std::vector<uint8_t>> alacSine() {
    std::vector<std::vector<uint8_t>> pkts;
    long t = 0;
    for (int fr = 0; fr * 352 < 44100 + 4 * 352; ++fr) {
        std::vector<uint8_t> b;
        uint64_t acc = 0; int nb = 0;
        auto put = [&](uint32_t v, int bits) {
            for (int i = bits - 1; i >= 0; --i) { acc = (acc << 1) | ((v >> i) & 1u); if (++nb == 8) { b.push_back(uint8_t(acc)); acc = 0; nb = 0; } }
        };
        put(1, 3); put(0, 4); put(0, 12); put(0, 1); put(0, 2); put(1, 1);
        for (int i = 0; i < 352; ++i, ++t) {
            const int16_t v = int16_t(8000.0 * std::sin(2 * M_PI * 1000.0 * double(t) / 44100.0));
            put(uint16_t(v), 16); put(uint16_t(v), 16);
        }
        put(7, 3);
        while (nb) put(0, 1);
        pkts.push_back(b);
    }
    return pkts;
}

static void roundTrip(unsigned ct, AVCodecID id, int fs, const char* name) {
    auto pkts = id == AV_CODEC_ID_ALAC ? alacSine() : encodeSine(id, fs);
    char w[128];
    std::snprintf(w, sizeof w, "%s: host encoder made packets (%zu)", name, pkts.size());
    CHECK(pkts.size() > 20, w);
    if (pkts.size() <= 20) return;
    RaopDecoder d;
    std::vector<uint8_t> out;
    int ok = 0;
    for (auto& p : pkts) ok += d.decode(ct, p.data(), int(p.size()), out) ? 1 : 0;
    std::snprintf(w, sizeof w, "%s: decoder codec %s", name, raopCodecName(d.codec()));
    CHECK(d.codec() == raopCodecFromCt(ct), w);
    std::snprintf(w, sizeof w, "%s: most packets decode (%d/%zu)", name, ok, pkts.size());
    CHECK(ok >= int(pkts.size()) - 3, w);
    const double secs = double(out.size()) / kRaopBytesPerSec, want = double(pkts.size()) * fs / 44100.0;
    std::snprintf(w, sizeof w, "%s: resampled to 48k (%.3f s of %.3f)", name, secs, want);
    CHECK(std::fabs(secs - want) < 0.08, w);
    const double r = rms(out, kRaopBytesPerSec / 5);
    std::snprintf(w, sizeof w, "%s: sine survives (rms %.0f, want ~5657)", name, r);
    CHECK(r > 4500 && r < 6800, w);
    d.setVolumeDb(-6.0206f);   // -6 dB halves the amplitude
    std::vector<uint8_t> half;
    for (size_t i = pkts.size() / 2; i < pkts.size() - 2; ++i) d.decode(ct, pkts[i].data(), int(pkts[i].size()), half);
    const double rh = rms(half, half.size() / 4);
    std::snprintf(w, sizeof w, "%s: -6 dB volume (rms %.0f)", name, rh);
    CHECK(rh > 2200 && rh < 3400, w);
    d.setVolumeDb(-144.0f);
    std::vector<uint8_t> mute;
    for (size_t i = 10; i < 18; ++i) d.decode(ct, pkts[i].data(), int(pkts[i].size()), mute);
    std::snprintf(w, sizeof w, "%s: mute is silent", name);
    CHECK(!mute.empty() && rms(mute) < 1.0, w);
    d.flush();
    std::vector<uint8_t> again;
    d.setVolumeDb(0);
    for (size_t i = 0; i < 10; ++i) d.decode(ct, pkts[i].data(), int(pkts[i].size()), again);
    std::snprintf(w, sizeof w, "%s: decodes after flush", name);
    CHECK(!again.empty(), w);
}

int main() {
    CHECK(raopCodecFromCt(2) == RaopCodec::Alac && raopCodecFromCt(4) == RaopCodec::AacLc &&
          raopCodecFromCt(8) == RaopCodec::AacEld && raopCodecFromCt(1) == RaopCodec::None, "ct map");
    CHECK(raopExtradata(RaopCodec::Alac).size() == 36 && raopExtradata(RaopCodec::AacEld).size() == 4, "extradata sizes");
    CHECK(raopGainFromDb(0) == 1.0f && raopGainFromDb(-144) == 0.0f && raopGainFromDb(-30) == 0.0f, "gain ends");
    CHECK(std::fabs(raopGainFromDb(-20) - 0.1f) < 1e-4, "gain -20 dB");
    const std::string h = wavStreamHeader();
    CHECK(h.size() == 44 && h.compare(0, 4, "RIFF") == 0 && h.compare(36, 4, "data") == 0 &&
          uint8_t(h[24]) == 0x80 && uint8_t(h[25]) == 0xBB && h[22] == 2 && h[34] == 16, "wav header 48k s16 stereo");

    roundTrip(4, AV_CODEC_ID_AAC, 1024, "aac-lc");
    roundTrip(2, AV_CODEC_ID_ALAC, 352, "alac");
    {   // AAC-ELD: iOS ASC opens the decoder; garbage counts an error, never crashes
        RaopDecoder d;
        std::vector<uint8_t> out;
        uint8_t junk[64];
        for (int i = 0; i < 64; ++i) junk[i] = uint8_t(i * 37);
        d.decode(8, junk, sizeof junk, out);
        CHECK(d.codec() == RaopCodec::AacEld, "aac-eld decoder opens with F8E85000");
        CHECK(!d.decode(9, junk, 4, out) && !d.decode(4, nullptr, 0, out), "bad ct / empty rejected");
    }
    {   // fanout: cap drops oldest, flush clears, closed listener ends
        PcmFanout f(4000);
        auto a = f.attach(), b = f.attach();
        std::vector<uint8_t> blk(1000, 7);
        for (int i = 0; i < 10; ++i) f.push(blk.data(), blk.size());
        std::vector<uint8_t> out(20000);
        const long n = f.pop(a, out.data(), out.size(), 10);
        CHECK(n == 4000 && a->dropped == 6000, "fanout cap 4000 (oldest dropped)");
        f.flush();
        CHECK(f.pop(b, out.data(), out.size(), 10) == 0, "flush empties listener queue");
        f.detach(b);
        CHECK(f.listeners() == 1, "detach");
        f.closeAll();
        CHECK(f.pop(a, out.data(), out.size(), 10) == -1, "closed + empty = -1");
    }
    CHECK(raopPcmSource("http://192.0.2.228:7002/raop.wav"), "raop.wav is the RAOP source");
    CHECK(!raopPcmSource("http://x/song.wav") && !raopPcmSource("file:///raop.wav") &&
          !raopPcmSource("http://x/master.m3u8"), "other urls are not RAOP");
    std::printf("test_raop_audio: %d/%d %s\n", checks - fails, checks, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
