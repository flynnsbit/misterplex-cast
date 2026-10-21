// MPX OWENB-P1B T5: the OB play route end to end with the real libav input (arm/misterplexd/owenb_av_source.cpp):
// mp4 demux -> h264_mp4toannexb AUs with 90 kHz PTS -> OwenbPlaySession -> OwenbRing -> the paced fake core,
// and AAC decoded + resampled to the core's 48 kHz s16le stereo, in PTS'd records. Fixtures
// (tests/fixtures/owenb_t5): a libx264 Main (CABAC) clip with AAC at 44.1 kHz (resampled), the
// Constrained Baseline (CAVLC) avsync cut with AAC at 48 kHz (rewritten to CABAC by the library), and a raw
// .264 (no container PTS: synthesised from the frame rate). Built only when libav is present (MPX_HAVE_LIBAV).
#include "owenb_av_source.hpp"
#include "fake_owenb_core.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex::owenb;
using fakeob::Record;

namespace {
int g_fails = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

std::string fx(const char* name) { return std::string(MPX_SOURCE_ROOT) + "/tests/fixtures/owenb_t5/" + name; }

bool hasIdr(const std::vector<uint8_t>& p) {
    for (size_t i = 0; i + 3 < p.size(); ++i)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 5)
            return true;
    return false;
}

struct Rig {
    explicit Rig(SessionConfig cfg = {}) : win(256u * 1024u), core(win), ring(win, 250) {
        cfg.sleepUs = [this](unsigned us) { win.sleepUs(us); };
        cfg.onIteration = [this] {
            if (onIter)
                onIter();
        };
        session.reset(new OwenbPlaySession(
            ring,
            [this](const OpenRequest& q, std::string& err) {
                ++opens;
                return openAvSource(q, err);
            },
            cfg));
    }
    EndReason play(const std::string& url) {
        OpenRequest q;
        q.url = url;
        q.containerSeek = true;
        return session->run(q, 11);
    }
    std::vector<Record> of(size_t sess, unsigned type) const {
        std::vector<Record> v;
        if (sess < core.sessions.size())
            for (const auto& r : core.sessions[sess])
                if (r.type == type)
                    v.push_back(r);
        return v;
    }
    misterplex::owenb::HeapDdrWindow win;
    fakeob::PacedCore core;
    OwenbRing ring;
    std::unique_ptr<OwenbPlaySession> session;
    std::function<void()> onIter;
    int opens = 0;
};

uint64_t pcmBytes(const std::vector<Record>& a, bool& ptsOk, bool& recOk) {
    uint64_t n = 0;
    ptsOk = !a.empty();
    recOk = true;
    for (const auto& r : a) {
        n += r.len;
        ptsOk = ptsOk && r.pts >= 0;
        recOk = recOk && r.len <= 3840 && r.len % 4 == 0;
    }
    return n;
}

// A1-A3: mp4 (CABAC + AAC 44.1 kHz): 48 AUs, an IDR first, PTS rising by one frame, END_OF_STREAM last;
// about 2 s of 48 kHz PCM in <= 20 ms PTS'd records.
void testMp4Cabac() {
    Rig r;
    const EndReason e = r.play(fx("cabac_aac44k_2s.mp4"));
    CHECK(e == EndReason::Eof, "A1 the mp4 plays to the end (%s: %s)", toString(e), r.session->error().c_str());
    const std::vector<Record> v = r.of(0, H264RING_T_VIDEO_AU);
    bool rising = !v.empty() && hasIdr(v[0].payload);
    for (size_t i = 1; i < v.size(); ++i)
        rising = rising && v[i].pts - v[i - 1].pts >= 3740 && v[i].pts - v[i - 1].pts <= 3760;
    CHECK(v.size() == 48 && rising, "A1 48 AUs, IDR first, PTS +3750 each (%zu)", v.size());
    CHECK(!v.empty() && (v.back().flags & H264RING_F_END_OF_STREAM), "A1 the last AU carries END_OF_STREAM");
    bool ptsOk = false, recOk = false;
    const uint64_t n = pcmBytes(r.of(0, H264RING_T_AUDIO), ptsOk, recOk);
    CHECK(n >= 360000 && n <= 400000, "A2 ~2 s of 48 kHz stereo PCM (44.1 kHz resampled): %llu bytes",
          static_cast<unsigned long long>(n));
    CHECK(ptsOk && recOk, "A2 PCM records carry PTS and are <= 20 ms");
    CHECK(r.session->positionMs() >= 1800, "A3 position at the end %lld ms", static_cast<long long>(r.session->positionMs()));
}

// B1-B2: the CAVLC (Constrained Baseline) mp4: the library rewrites it, the core sees a CABAC PPS; audio at
// 48 kHz passes through the resampler unchanged in rate.
void testMp4Cavlc() {
    Rig r;
    const EndReason e = r.play(fx("cavlc_aac48k_2s.mp4"));
    CHECK(e == EndReason::Eof, "B1 the CAVLC mp4 plays (%s: %s)", toString(e), r.session->error().c_str());
    const std::vector<Record> v = r.of(0, H264RING_T_VIDEO_AU);
    CHECK(!v.empty() && ppsEntropyCabac(v[0].payload.data(), v[0].payload.size()) == 1 &&
              r.session->stats().cavlcAus == v.size(),
          "B1 %zu AUs rewritten to CABAC", v.size());
    bool ptsOk = false, recOk = false;
    const uint64_t n = pcmBytes(r.of(0, H264RING_T_AUDIO), ptsOk, recOk);
    CHECK(n >= 360000 && ptsOk && recOk, "B2 48 kHz PCM in PTS'd records (%llu bytes)", static_cast<unsigned long long>(n));
}

// R1: a raw .264 (no container timestamps): AUs with a 24 fps clock.
void testRaw() {
    Rig r;
    const EndReason e = r.play(fx("cabac_160x96_2s.264"));
    CHECK(e == EndReason::Eof, "R1 a raw .264 plays (%s: %s)", toString(e), r.session->error().c_str());
    const std::vector<Record> v = r.of(0, H264RING_T_VIDEO_AU);
    bool ok = v.size() == 48;
    for (size_t i = 1; ok && i < v.size(); ++i)
        ok = v[i].pts > v[i - 1].pts;
    CHECK(ok && r.of(0, H264RING_T_AUDIO).empty(), "R1 48 AUs with rising PTS, no audio (%zu)", v.size());
}

// K1-K3: a held seek in the container (containerSeek), a pause and a stop on the libav input.
void testControls() {
    {
        Rig r;
        bool seeked = false;
        r.onIter = [&] {
            if (!seeked && r.session->started() && r.session->positionMs() >= 200) {
                seeked = true;
                OpenRequest q;
                q.url = fx("cabac_aac44k_2s.mp4");
                q.startMs = 1000;
                q.containerSeek = true;
                r.session->seek(q);
            }
        };
        const EndReason e = r.play(fx("cabac_aac44k_2s.mp4"));
        const std::vector<Record> v = r.of(1, H264RING_T_VIDEO_AU);
        CHECK(e == EndReason::Eof && r.opens == 2 && r.core.sessions.size() == 2, "K1 seek: reopened and flushed (%s)",
              toString(e));
        CHECK(!v.empty() && hasIdr(v[0].payload) && v[0].pts >= 85000 && v[0].pts <= 90000,
              "K1 the new stream starts on the IDR at 1 s (pts %lld)", v.empty() ? -1LL : static_cast<long long>(v[0].pts));
        CHECK(r.session->positionMs() >= 1800, "K1 position after the seek %lld", static_cast<long long>(r.session->positionMs()));
    }
    {
        Rig r;
        int phase = 0, idle = 0;
        r.onIter = [&] {
            if (phase == 0 && r.session->started() && r.session->positionMs() >= 300) {
                r.session->pause();
                phase = 1;
            } else if (phase == 1 && r.session->paused() && ++idle == 50) {
                r.session->resume();
                phase = 2;
            }
        };
        const EndReason e = r.play(fx("cavlc_aac48k_2s.mp4"));
        CHECK(e == EndReason::Eof && r.session->stats().pauses == 1 && r.core.pausedPolls > 0,
              "K2 pause / resume on the libav input (%s)", toString(e));
    }
    {
        Rig r;
        r.onIter = [&] {
            if (r.session->started() && r.session->positionMs() >= 500)
                r.session->stop();
        };
        const EndReason e = r.play(fx("cabac_aac44k_2s.mp4"));
        CHECK(e == EndReason::Stopped && r.core.paused() && !r.ring.status().active,
              "K3 stop holds the picture and releases (%s)", toString(e));
    }
    {
        Rig r;
        const EndReason e = r.play(fx("does_not_exist.mp4"));
        CHECK(e == EndReason::SourceError && r.core.sessions.empty(), "K4 a missing input -> SourceError, no session");
    }
}

} // namespace

int main() {
    testMp4Cabac();
    testMp4Cavlc();
    testRaw();
    testControls();
    std::printf("test_owenb_av_e2e: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
