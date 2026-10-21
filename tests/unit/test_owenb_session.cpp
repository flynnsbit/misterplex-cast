// MPX OWENB-P1B T5: the OB play session (host/libmisterplex/owenb_session.cpp) end to end on the real
// OwenbRing + vendored owenb lib + a paced fake core (tests/unit/fake_owenb_core.hpp), with real H.264
// (tests/fixtures/owenb_t5: a libx264 CABAC clip and a Constrained Baseline CAVLC clip cut from
// assets/avsync) and synthetic 48 kHz PCM interleaved per frame.
// Mutants (each must FAIL): MUT_OWENB_SESS_NO_EOS, MUT_OWENB_SESS_SEEK_NO_FLUSH, MUT_OWENB_SESS_PAUSE_READS,
// MUT_OWENB_SESS_NO_CAVLC_GUARD, MUT_OWENB_SESS_PCM_NO_PTS,
// MUT_OWENB_SESS_UNBOUNDED_OUT, MUT_OWENB_SESS_IGNORE_SENDREADY.
#include "libmisterplex/owenb_session.hpp"
#include "fake_owenb_core.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex::owenb;
using fakeob::PacedCore;
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

constexpr uint32_t kRing = 128u * 1024u;
constexpr int64_t kFrame = 3750;          // 24 fps in 90 kHz
constexpr size_t kPcmPerFrame = 8000;     // 1/24 s of 48 kHz s16 stereo

std::vector<uint8_t> readFile(const std::string& rel) {
    std::ifstream f(std::string(MPX_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool hasIdr(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 3 < n; ++i)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 5)
            return true;
    return false;
}

// A clip: AUs split by the library, PTS i x 3750, then one PCM packet per frame (same PTS).
std::vector<MediaPacket> clip(const std::string& rel, bool audio = true) {
    const std::vector<uint8_t> b = readFile(rel);
    std::vector<MediaPacket> out;
    const size_t n = h264ring_split_annexb(b.data(), b.size(), nullptr, 0);
    std::vector<h264ring_au> aus(n);
    h264ring_split_annexb(b.data(), b.size(), aus.data(), n);
    for (size_t i = 0; i < n; ++i) {
        MediaPacket v;
        v.kind = MediaPacket::Kind::Video;
        v.data.assign(b.begin() + aus[i].off, b.begin() + aus[i].off + aus[i].len);
        v.pts90k = static_cast<int64_t>(i) * kFrame;
        v.keyframe = hasIdr(v.data.data(), v.data.size());
        out.push_back(std::move(v));
        if (audio) {
            MediaPacket a;
            a.kind = MediaPacket::Kind::Audio;
            a.data.resize(kPcmPerFrame);
            for (size_t k = 0; k < kPcmPerFrame; ++k)
                a.data[k] = static_cast<uint8_t>(i + k);
            a.pts90k = static_cast<int64_t>(i) * kFrame;
            out.push_back(std::move(a));
        }
    }
    return out;
}

struct ScriptSource final : IMediaSource {
    std::vector<MediaPacket> pk;
    size_t i = 0;
    int* reads = nullptr;
    int* readsPaused = nullptr;              // reads while the session reports paused()
    std::function<bool()> pausedNow;
    size_t errorAt = SIZE_MAX;
    SourceRead read(MediaPacket& p, std::string& err) override {
        if (i == errorAt) {
            err = "scripted read error";
            return SourceRead::Error;
        }
        if (i >= pk.size())
            return SourceRead::Eof;
        if (reads)
            ++*reads;
        if (readsPaused && pausedNow && pausedNow())
            ++*readsPaused;
        p = pk[i++];
        return SourceRead::Packet;
    }
};

// A seek / start offset opens the clip three frames before the target, like a server that starts a
// little early: the non-keyframes before the next IDR must be dropped.
struct Rig {
    explicit Rig(std::vector<MediaPacket> pk, SessionConfig cfg = {}) : win(kRing), core(win), ring(win, 250),
                                                                         packets(std::move(pk)) {
        cfg.sleepUs = [this](unsigned us) {
            ++sleeps;
            if (onSleep)
                onSleep();
            win.sleepUs(us);
        };
        cfg.onIteration = [this] {
            ++iterations;
            if (onIter)
                onIter();
        };
        cfg.log = [this](const std::string& s) { logs += s + "\n"; };
        session.reset(new OwenbPlaySession(
            ring,
            [this](const OpenRequest& req, std::string& err) -> std::unique_ptr<IMediaSource> {
                ++opens;
                if (failOpen) {
                    err = "scripted open failure";
                    return nullptr;
                }
                auto s = std::make_unique<ScriptSource>();
                size_t start = 0;
                std::vector<size_t> vids;
                for (size_t i = 0; i < packets.size(); ++i)
                    if (packets[i].kind == MediaPacket::Kind::Video)
                        vids.push_back(i);
                for (size_t k = 0; k < vids.size(); ++k)
                    if (packets[vids[k]].pts90k >= req.startMs * 90) {
                        start = vids[k >= 3 ? k - 3 : 0];
                        break;
                    }
                s->pk.assign(packets.begin() + static_cast<long>(start), packets.end());
                s->reads = &reads;
                s->readsPaused = &readsPaused;
                s->pausedNow = [this] { return session->paused(); };
                s->errorAt = errorAt;
                return s;
            },
            cfg));
    }
    EndReason play(int64_t startMs = 0) {
        OpenRequest r;
        r.url = "fixture";
        r.startMs = startMs;
        return session->run(r, 7);
    }
    std::vector<Record> videos(size_t sess) const {
        std::vector<Record> v;
        if (sess < core.sessions.size())
            for (const auto& r : core.sessions[sess])
                if (r.type == H264RING_T_VIDEO_AU)
                    v.push_back(r);
        return v;
    }
    misterplex::owenb::HeapDdrWindow win;
    PacedCore core;
    OwenbRing ring;
    std::vector<MediaPacket> packets;
    std::unique_ptr<OwenbPlaySession> session;
    std::function<void()> onIter, onSleep;
    int opens = 0, reads = 0, readsPaused = 0, iterations = 0, sleeps = 0;
    bool failOpen = false;
    size_t errorAt = SIZE_MAX;
    std::string logs;
};

const char* kCabac = "tests/fixtures/owenb_t5/cabac_160x96_2s.264";
const char* kCavlc = "tests/fixtures/owenb_t5/cavlc_320x240_2s.264";

// E1-E4: the CABAC clip plays to the end: every AU with its PTS in order, the first an IDR, the last
// END_OF_STREAM and the session's last record; PCM in <= 20 ms records, each with its PTS, contiguous.
void testPlayToEnd() {
    Rig r(clip(kCabac));
    const EndReason e = r.play();
    CHECK(e == EndReason::Eof, "E1 the clip ends Eof (%s: %s)", toString(e), r.session->error().c_str());
    CHECK(r.core.sessions.size() == 1, "E1 one session (%zu)", r.core.sessions.size());
    const std::vector<Record> v = r.videos(0);
    CHECK(v.size() == 48, "E1 48 AUs reached the core (%zu)", v.size());
    bool ptsOk = !v.empty() && hasIdr(v[0].payload.data(), v[0].payload.size());
    for (size_t i = 0; i < v.size(); ++i)
        ptsOk = ptsOk && v[i].pts == static_cast<int64_t>(i) * kFrame;
    CHECK(ptsOk, "E1 the first AU is an IDR and every AU carries PTS i x 3750");
    const std::vector<Record> all = r.core.all();
    const bool eosLast = !all.empty() && all.back().type == H264RING_T_VIDEO_AU &&
                         (all.back().flags & H264RING_F_END_OF_STREAM);
    size_t eosCount = 0;
    for (const auto& x : all)
        eosCount += (x.type == H264RING_T_VIDEO_AU && (x.flags & H264RING_F_END_OF_STREAM)) ? 1 : 0;
    CHECK(eosLast && eosCount == 1, "E2 the last AU carries END_OF_STREAM and is the last record (%zu EOS)", eosCount);
    CHECK(r.core.ended, "E2 the core took the end (L_ENDED: the last picture stays up)");
    uint64_t bytes = 0;
    bool recOk = true, ptsCont = true;
    int64_t next = 0;
    for (const auto& x : all) {
        if (x.type != H264RING_T_AUDIO)
            continue;
        bytes += x.len;
        recOk = recOk && x.len <= 3840 && x.len % 4 == 0;
        ptsCont = ptsCont && x.pts >= 0 && x.pts >= next - 1 && x.pts <= next + 1;
        next = x.pts + static_cast<int64_t>(x.len / 4) * 15 / 8;
    }
    CHECK(bytes == 48 * kPcmPerFrame, "E3 all PCM reached the core (%llu bytes)", static_cast<unsigned long long>(bytes));
    CHECK(recOk, "E3 PCM records are <= 20 ms and whole stereo frames");
    CHECK(ptsCont, "E3 every PCM record has its PTS, contiguous at 48 kHz");
    const SessionStats st = r.session->stats();
    CHECK(st.fullWaits > 0, "E4 the 128 KiB ring filled and the session backed off (%llu waits)",
          static_cast<unsigned long long>(st.fullWaits));
    CHECK(r.session->started() && r.session->positionMs() >= 1900, "E4 started, position at the end %lld ms",
          static_cast<long long>(r.session->positionMs()));
    CHECK(!r.ring.status().active && !r.session->running(), "E4 teardown: the ring session is released");
    CHECK(!r.core.paused(), "E4 end of stream leaves the core unpaused (the hold is the core's)");
}

// C1-C3: CAVLC. The library rewrites it to CABAC (the PPS the core sees says CABAC); a CAVLC stream above
// OB_CAVLC_MAX_KBPS ends NeedsTranscode (the daemon's lowered retry), before more than the window is sent.
void testCavlc() {
    {
        Rig r(clip(kCavlc));
        const EndReason e = r.play();
        CHECK(e == EndReason::Eof, "C1 the CAVLC clip plays (%s: %s)", toString(e), r.session->error().c_str());
        const std::vector<Record> v = r.videos(0);
        const std::vector<MediaPacket> src = clip(kCavlc, false);
        CHECK(!src.empty() && ppsEntropyCabac(src[0].data.data(), src[0].data.size()) == 0,
              "C1 the fixture's PPS is CAVLC");
        CHECK(!v.empty() && ppsEntropyCabac(v[0].payload.data(), v[0].payload.size()) == 1,
              "C1 the core received a CABAC PPS (rewritten in the library)");
        CHECK(v.size() == 48 && r.session->stats().cavlcAus == 48, "C1 48 rewritten AUs (%zu)", v.size());
    }
    {
        SessionConfig cfg;
        cfg.cavlcMaxKbps = 50;   // the clip runs ~120 kbps
        cfg.cavlcWindowMs = 1000;
        cfg.cavlcMaxLoadPct = 0;  // v2-bugfix: 0 = the rate alone decides (the T5 rule)
        Rig r(clip(kCavlc), cfg);
        const EndReason e = r.play();
        CHECK(e == EndReason::NeedsTranscode, "C2 CAVLC above OB_CAVLC_MAX_KBPS -> NeedsTranscode (%s)", toString(e));
        CHECK(r.session->error().find("OB_CAVLC_MAX_KBPS") != std::string::npos, "C2 the reason names the cap: %s",
              r.session->error().c_str());
        CHECK(r.videos(0).size() < 30, "C2 the guard stopped within the window (%zu AUs)", r.videos(0).size());
    }
    {
        SessionConfig cfg;
        cfg.cavlcMaxKbps = 50;
        cfg.cavlcWindowMs = 1000;
        Rig r(clip(kCabac), cfg);   // the cap applies to CAVLC only
        CHECK(r.play() == EndReason::Eof, "C3 a CABAC stream is not capped");
    }
    const std::vector<MediaPacket> cab = clip(kCabac, false);
    CHECK(!cab.empty() && ppsEntropyCabac(cab[0].data.data(), cab[0].data.size()) == 1, "C3 libx264 main: CABAC");
    CHECK(cab.size() > 1 && ppsEntropyCabac(cab[1].data.data(), cab[1].data.size()) == -1, "C3 a P AU has no PPS");
}

// V1 (a lab MiSTer bug 1, 2026-10-17): a CAVLC stream above OB_CAVLC_MAX_KBPS whose rewrite keeps real time plays on.
// Before the fix the nominal rate alone ended it NeedsTranscode after the window (Star Trek: ~3 s of good
// picture, then "Transcoding instead").
void testCavlcKeepsUp() {
    SessionConfig cfg;
    cfg.cavlcMaxKbps = 50;   // the clip runs ~120 kbps: above the cap
    cfg.cavlcWindowMs = 1000;
    Rig r(clip(kCavlc), cfg);
    const EndReason e = r.play();
    CHECK(e == EndReason::Eof, "V1 CAVLC above the cap with a cheap rewrite plays to the end (%s: %s)", toString(e),
          r.session->error().c_str());
    CHECK(r.videos(0).size() == 48, "V1 all 48 AUs reached the core (%zu)", r.videos(0).size());
    // V2: the same stream whose rewrite does not keep real time (each CAVLC write costs 100 ms of a 40 ms frame
    // on the injected clock) still takes the lowered retry, and the reason names both limits.
    SessionConfig slow = cfg;
    int64_t fake = 0;
    slow.nowNs = [&fake] { return fake += 50000000; };   // two reads per write: 100 ms per AU
    Rig s(clip(kCavlc), slow);
    const EndReason e2 = s.play();
    CHECK(e2 == EndReason::NeedsTranscode, "V2 a rewrite above OB_CAVLC_MAX_LOAD_PCT -> NeedsTranscode (%s)",
          toString(e2));
    CHECK(s.session->error().find("OB_CAVLC_MAX_LOAD_PCT=70") != std::string::npos &&
              s.session->error().find("OB_CAVLC_MAX_KBPS=50") != std::string::npos,
          "V2 the reason names the rate and the load: %s", s.session->error().c_str());
    // V3: a cheap rewrite under the cap is untouched (C1 with the default budget), and the cap still applies to
    // CAVLC only (C3).
    SessionConfig dflt;
    CHECK(dflt.cavlcMaxLoadPct == 70 && dflt.cavlcMaxKbps == 4000 && dflt.cavlcHardMaxKbps == 7000,
          "V3 defaults: 4000 kbps, 70 %% load, 7000 kbps hard");
    // V4 (BUG1-CAP): above OB_CAVLC_HARD_MAX_KBPS the play ends even with a cheap rewrite.
    SessionConfig hard = cfg;
    hard.cavlcHardMaxKbps = 60;
    Rig h(clip(kCavlc), hard);
    const EndReason e4 = h.play();
    CHECK(e4 == EndReason::NeedsTranscode && h.session->error().find("OB_CAVLC_HARD_MAX_KBPS=60") != std::string::npos,
          "V4 above the hard cap -> NeedsTranscode (%s: %s)", toString(e4), h.session->error().c_str());
    // V6 (lab 7AE78D80): the entropy report for the per-PMS memory: CAVLC windows with kbps and load, CABAC once.
    {
        SessionConfig c = cfg;
        int cavlcReports = 0, cabacReports = 0;
        unsigned lastKbps = 0;
        c.onEntropy = [&](bool cavlc, unsigned kbps, unsigned) {
            if (cavlc) {
                ++cavlcReports;
                lastKbps = kbps;
            } else {
                ++cabacReports;
            }
        };
        Rig v(clip(kCavlc), c);
        v.play();
        CHECK(cavlcReports >= 1 && lastKbps > 50 && cabacReports == 0, "V6 CAVLC: %d window reports (last %u kbps), %d CABAC",
              cavlcReports, lastKbps, cabacReports);
        int cabacOnly = 0, cavlcOnCabac = 0;
        SessionConfig k;
        k.onEntropy = [&](bool cavlc, unsigned, unsigned) { cavlc ? ++cavlcOnCabac : ++cabacOnly; };
        Rig w(clip(kCabac), k);
        w.play();
        CHECK(cabacOnly == 1 && cavlcOnCabac == 0, "V6 CABAC: one CABAC report (%d), no CAVLC (%d)", cabacOnly, cavlcOnCabac);
    }
    // V5 (BUG1-CAP): the budget's edge on the clip's 80 ms frames: 50 ms of rewrite an AU (~62 %) plays on, 64 ms (~80 %) ends.
    for (const auto& [perReadNs, wantEof] : {std::pair<int64_t, bool>{25000000, true}, {32000000, false}}) {
        SessionConfig c = cfg;
        int64_t clk = 0;
        const int64_t step = perReadNs;
        c.nowNs = [&clk, step] { return clk += step; };
        Rig v(clip(kCavlc), c);
        const EndReason ev = v.play();
        if (std::getenv("V5LOG"))
            std::fprintf(stderr, "%s", v.logs.c_str());
        CHECK((ev == EndReason::Eof) == wantEof, "V5 %lld ms an AU: %s (%s: %s)",
              static_cast<long long>(2 * perReadNs / 1000000), wantEof ? "plays on" : "ends", toString(ev),
              v.session->error().c_str());
    }
}

// P1-P4: pause sets the core's pause word and stops reading the input; resume clears it and play ends
// normally. A pause before the first picture is ignored (cast clients pause while buffering).
void testPause() {
    Rig r(clip(kCabac));
    int readsAtPause = -1, readsAtResume = -1, pausedSleeps = 0;
    bool sawCorePaused = false, resumed = false;
    r.onIter = [&] {
        if (readsAtPause < 0 && r.session->started() && r.session->positionMs() >= 300)
            r.session->pause();
    };
    r.onSleep = [&] {
        if (!resumed && r.session->paused()) {
            if (readsAtPause < 0)
                readsAtPause = r.reads;
            sawCorePaused = sawCorePaused || r.core.paused();
            if (++pausedSleeps == 200) {
                readsAtResume = r.reads;
                resumed = true;
                r.session->resume();
            }
        }
    };
    const EndReason e = r.play();
    CHECK(e == EndReason::Eof, "P1 pause / resume, then the end (%s: %s)", toString(e), r.session->error().c_str());
    CHECK(sawCorePaused, "P1 the core's pause word was set while paused");
    CHECK(readsAtPause >= 0 && readsAtResume == readsAtPause && r.readsPaused == 0,
          "P2 no input read while paused (%d -> %d, %d while paused)", readsAtPause, readsAtResume, r.readsPaused);
    CHECK(r.core.pausedPolls >= 100, "P2 the core idled while paused (%llu polls)",
          static_cast<unsigned long long>(r.core.pausedPolls));
    const SessionStats st = r.session->stats();
    CHECK(st.pauses == 1 && st.resumes == 1, "P3 one pause, one resume (%llu, %llu)",
          static_cast<unsigned long long>(st.pauses), static_cast<unsigned long long>(st.resumes));
    CHECK(r.videos(0).size() == 48 && r.core.sessions.size() == 1, "P3 nothing lost, no flush");

    Rig b(clip(kCabac));
    b.onIter = [&] {
        if (b.iterations == 1)
            b.session->pause();
    };
    CHECK(b.play() == EndReason::Eof && b.session->stats().pauses == 0, "P4 a pause before the first picture is ignored");
}

// S1-S4: a held seek: the picture holds (pause) while the input reopens, the ring is flushed (a new
// session: the old queue is dropped), the new stream starts on an IDR at or after the target, and play
// resumes. Position rebases on the target.
void testSeek() {
    Rig r(clip(kCabac));
    bool seeked = false;
    r.onIter = [&] {
        if (!seeked && r.session->started() && r.session->positionMs() >= 200) {
            seeked = true;
            OpenRequest q;
            q.url = "fixture";
            q.startMs = 1000;
            r.session->seek(q);
        }
    };
    const EndReason e = r.play();
    CHECK(e == EndReason::Eof, "S1 seek, then the end (%s: %s)", toString(e), r.session->error().c_str());
    CHECK(r.opens == 2, "S1 the input reopened at the target (%d opens)", r.opens);
    const bool sawHeld = r.core.pausedAtStart.size() == 2 && !r.core.pausedAtStart[0] && r.core.pausedAtStart[1];
    CHECK(sawHeld, "S2 the old picture was held (paused) while the new input opened");
    CHECK(r.core.sessions.size() == 2 && r.session->stats().flushes == 1, "S2 one flush: a new session (%zu)",
          r.core.sessions.size());
    const std::vector<Record> v = r.videos(1);
    CHECK(!v.empty() && hasIdr(v[0].payload.data(), v[0].payload.size()) && v[0].pts == 24 * kFrame,
          "S3 the new stream starts on the IDR at 1000 ms (pts %lld)",
          v.empty() ? -1LL : static_cast<long long>(v[0].pts));
    CHECK(r.session->stats().droppedBeforeKey == 3 && v.size() == 24, "S3 3 AUs before the IDR dropped, 24 sent (%zu)",
          v.size());
    bool audioAfterKey = true;
    if (r.core.sessions.size() > 1)
        for (const auto& x : r.core.sessions[1])
            if (x.type == H264RING_T_AUDIO)
                audioAfterKey = audioAfterKey && x.pts >= 24 * kFrame;
    CHECK(audioAfterKey, "S3 no audio from before the new IDR");
    CHECK(!r.core.paused() && r.session->positionMs() >= 1900, "S4 resumed; position %lld ms",
          static_cast<long long>(r.session->positionMs()));
}

// T1-T2: stop. IDLE_SCREEN=last pauses and releases (the picture stays, no flush); black flushes.
void testStop() {
    for (int black = 0; black < 2; ++black) {
        SessionConfig cfg;
        cfg.holdLastPicture = !black;
        Rig r(clip(kCabac), cfg);
        r.onIter = [&] {
            if (r.session->started() && r.session->positionMs() >= 500)
                r.session->stop();
        };
        const EndReason e = r.play();
        CHECK(e == EndReason::Stopped, "T%d stop -> Stopped (%s)", 1 + black, toString(e));
        if (!black)
            CHECK(r.core.paused() && r.core.sessions.size() == 1, "T1 hold: paused, no flush (%zu sessions)",
                  r.core.sessions.size());
        else
            CHECK(!r.core.paused() && r.core.sessions.size() == 2, "T2 black: flushed (%zu sessions)",
                  r.core.sessions.size());
        CHECK(!r.ring.status().active && r.videos(0).size() < 48, "T%d the ring session is released mid-stream",
              1 + black);
    }
}

// V1-V2: the Plex volume maps to the core's attenuation (0.5 dB steps), live during play.
void testVolume() {
    CHECK(attenuationForVolume(100) == 0 && attenuationForVolume(50) == 12 && attenuationForVolume(25) == 24 &&
              attenuationForVolume(10) == 40 && attenuationForVolume(1) == 80 && attenuationForVolume(0) == 255 &&
              attenuationForVolume(150) == 0 && attenuationForVolume(-3) == 255,
          "V1 the volume curve (100:0 50:12 25:24 10:40 1:80 0:255)");
    Rig r(clip(kCabac));
    unsigned seen50 = 999, seen0 = 999;
    int phase = 0;
    r.onIter = [&] {
        if (phase == 0 && r.session->started()) {
            r.session->setVolumePercent(50);
            phase = 1;
        } else if (phase == 1 && r.core.atten == 12) {
            seen50 = r.core.atten;
            r.session->setVolumePercent(0);
            phase = 2;
        } else if (phase == 2 && r.core.atten == 255) {
            seen0 = r.core.atten;
            phase = 3;
        }
    };
    CHECK(r.play() == EndReason::Eof, "V2 volume changes during play");
    CHECK(seen50 == 12 && seen0 == 255, "V2 the core saw atten 12 (50) then 255 (0): %u %u", seen50, seen0);
}

// X1-X4: errors and teardown. An OSD Reset (another nonce) -> CoreLost; the core in ERROR -> RingError;
// an input that does not open -> SourceError and no session; a read error mid-play -> SourceError with
// the picture held.
void testErrors() {
    {
        Rig r(clip(kCabac));
        r.onIter = [&] {
            if (r.session->positionMs() >= 400)
                r.core.ack ^= 0x5A5Au;
        };
        const EndReason e = r.play();
        CHECK(e == EndReason::CoreLost, "X1 OSD Reset -> CoreLost (%s: %s)", toString(e), r.session->error().c_str());
        CHECK(!r.ring.status().active, "X1 the session is released");
    }
    {
        Rig r(clip(kCabac));
        r.onIter = [&] {
            if (r.session->positionMs() >= 400)
                r.core.error = true;
        };
        const EndReason e = r.play();
        CHECK(e == EndReason::RingError, "X2 core ERROR -> RingError (%s)", toString(e));
    }
    {
        Rig r(clip(kCabac));
        r.failOpen = true;
        const EndReason e = r.play();
        CHECK(e == EndReason::SourceError && r.core.sessions.empty() && !r.ring.status().active,
              "X3 no input -> SourceError, no ring session (%s)", toString(e));
    }
    {
        Rig r(clip(kCabac));
        r.errorAt = 40;
        const EndReason e = r.play();
        CHECK(e == EndReason::SourceError && r.session->error().find("scripted") != std::string::npos,
              "X4 a read error -> SourceError (%s)", toString(e));
        CHECK(r.core.paused() && !r.ring.status().active, "X4 the picture is held, the session released");
    }
}

// GATE7: out_ is capped, and sendReady(false)==false stops the read loop.
// UNBOUNDED_OUT keeps the lookahead hold and piles audio past the cap.
// IGNORE_SENDREADY treats a full ring as "read another packet".
void testOutBackpressure() {
    std::vector<MediaPacket> pk = clip(kCabac, false);
    CHECK(!pk.empty() && pk[0].keyframe, "Q0 the cabac clip starts on a keyframe");
    if (!pk.empty() && pk[0].keyframe) {
        MediaPacket key = pk[0];
        pk.clear();
        pk.push_back(key);
        for (int i = 0; i < 50; ++i) {
            MediaPacket a;
            a.kind = MediaPacket::Kind::Audio;
            a.data.assign(100u * 1024u, static_cast<uint8_t>(i));
            a.pts90k = static_cast<int64_t>(i + 1) * kFrame;
            pk.push_back(std::move(a));
        }
        Rig r(std::move(pk));
        size_t peak = 0;
        r.onIter = [&] {
            const size_t q = r.session->queuedBytes();
            if (q > peak)
                peak = q;
        };
        const EndReason e = r.play();
        std::printf("out_cap peak=%zu cap=%zu reads=%d end=%s\n", peak, OwenbPlaySession::kOutByteCap,
                    r.reads, toString(e));
        CHECK(e == EndReason::Eof, "Q1 the capped queue still drains (%s: %s)", toString(e),
              r.session->error().c_str());
        CHECK(peak <= OwenbPlaySession::kOutByteCap + 128u * 1024u, "Q2 out_ peak %zu over the cap", peak);
    }

    std::vector<MediaPacket> many;
    {
        const std::vector<MediaPacket> base = clip(kCabac, false);
        if (!base.empty()) {
            const MediaPacket v = base[0];
            for (int i = 0; i < 400; ++i) {
                const int64_t pts = static_cast<int64_t>(i) * kFrame;
                MediaPacket c = v;
                c.pts90k = pts;
                c.keyframe = true;
                many.push_back(std::move(c));
                MediaPacket a;
                a.kind = MediaPacket::Kind::Audio;
                a.data.assign(8u * 1024u, static_cast<uint8_t>(i));
                a.pts90k = pts;
                many.push_back(std::move(a));
            }
        }
    }
    Rig b(std::move(many));
    b.core.perStep = 0;
    int sleeps = 0;
    b.onSleep = [&] {
        b.session->setVolumePercent(40);
        if (++sleeps >= 200)
            b.session->stop();
    };
    const EndReason eb = b.play();
    const size_t au = b.packets.empty() ? 1 : b.packets[0].data.size() + 8u * 1024u;
    const int fit = static_cast<int>((128u * 1024u) / (au ? au : 1)) + 40;
    std::printf("sendready reads=%d fit=%d sleeps=%d end=%s queued=%zu\n", b.reads, fit, sleeps,
                toString(eb), b.session->queuedBytes());
    CHECK(eb == EndReason::Stopped, "Q3 a full ring stops the session on request (%s)", toString(eb));
    CHECK(b.reads < fit, "Q4 kept reading after the ring filled (%d, fit %d)", b.reads, fit);
}

} // namespace

int main() {
    testPlayToEnd();
    testCavlc();
    testCavlcKeepsUp();
    testPause();
    testSeek();
    testStop();
    testVolume();
    testErrors();
    testOutBackpressure();
    std::printf("test_owenb_session: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
