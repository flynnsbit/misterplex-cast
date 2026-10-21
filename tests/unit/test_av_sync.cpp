// avsync45 (release 19:22 10/3): the 45/50 MHz A/V machinery (host/libmisterplex/av_sync.hpp)
// against a model of the FPGA path, the resampler, and the build rows.
//
// The model runs the real PcmRateNudger, AudioContentMap and AvSyncController:
// - audio: the pump keeps about 100 ms of PCM queued; the hardware consumes output frames at
//   48 kHz; the content is a 1 kHz tone through the nudger at the controller's ratio;
// - video: 24000/1001 fps content, each AU released when the consumed clock reaches its PTS
//   (media_player.cpp), shown at the first 60 Hz vsync after a fixed latency; the core freezes
//   consumed at that pickup (MVPS word 5), which is all the daemon measures;
// - HDMI (what the soak measures) is computed from the audio actually played (the nudger's own
//   output->input record), plus a fixed downstream term, plus soak A's -0.87 ms/min drift
//   placed AFTER the pickup, plus +-8 ms of jitter. The drift is invisible to the daemon, so
//   only the row's open-loop compensation (-14.6 ppm) can cancel it;
// - excursions: the shown content shifts (+ = audio late), persistent or passing.
// Pass needs, feature on (row trim -45 ms, compensation, resync): soak A held flat with p50 in
// 0..+10 ms and no window over 40 ms; a persistent +66 ms excursion pulled back by one logged
// resync; passing glitches (+60 ms for 5 s, video 1 s late for 3 s) leave no offset behind; a
// pull the measurement does not follow is undone. Feature off (negative control): soak A's
// -0.87 ms/min drift comes back.
#include "libmisterplex/av_sync.hpp"
#include "libmisterplex/fpga_build_ids.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace av = misterplex::av_sync;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL av sync: %s\n", msg.c_str());
        ++fails;
    }
}

std::string ms(double us) {
    char b[32];
    std::snprintf(b, sizeof b, "%+.1f ms", us / 1000.0);
    return b;
}

// ---------------------------------------------------------------- resampler

std::vector<uint8_t> tone(size_t frames, double hz, double amp, double& phase) {
    std::vector<uint8_t> out(frames * av::kFrameBytes);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t v = static_cast<int16_t>(std::lround(amp * std::sin(phase)));
        phase += 2.0 * M_PI * hz / av::kRateHz;
        const int16_t f[2] = {v, static_cast<int16_t>(-v)};
        std::memcpy(out.data() + i * av::kFrameBytes, f, av::kFrameBytes);
    }
    return out;
}

int16_t left(const std::vector<uint8_t>& pcm, size_t frame) {
    int16_t v;
    std::memcpy(&v, pcm.data() + frame * av::kFrameBytes, 2);
    return v;
}

void testPassthrough() {
    std::mt19937 rng(7);
    std::vector<uint8_t> in(4 * 50000);
    for (auto& b : in)
        b = static_cast<uint8_t>(rng());
    av::PcmRateNudger n;
    std::vector<uint8_t> out;
    size_t at = 0;
    const size_t chunks[] = {1, 3, 4, 7, 960 * 4, 3839, 5, 4096};
    for (size_t k = 0; at < in.size(); ++k) {
        const size_t len = std::min(chunks[k % 8], in.size() - at);
        n.process(in.data() + at, len, 1.0, out);
        at += len;
    }
    expect(out.size() == in.size() - 2 * av::kFrameBytes,
           "ratio 1 passthrough size " + std::to_string(out.size()));
    expect(std::memcmp(out.data(), in.data(), out.size()) == 0,
           "ratio 1 is a bit-exact passthrough across odd chunk sizes");
    expect(n.appliedUs() == 0, "ratio 1 moves nothing");
    std::printf("resampler: ratio 1 is bit-exact across odd chunk sizes (%zu frames)\n",
                out.size() / av::kFrameBytes);
}

void testClicksAndPitch() {
    const double amp = 20000.0;
    const double maxStep = 2.0 * M_PI * 1010.0 / av::kRateHz * amp * 1.01 + 4.0;
    for (double target : {1.01, 0.99, 1.0 - 14.6e-6}) {
        av::PcmRateNudger n;
        std::vector<uint8_t> out;
        double phase = 0.0;
        for (int c = 0; c < 400; ++c) {
            const double r = (c >= 100 && c < 300) ? target : 1.0;
            const auto in = tone(960, 1000.0, amp, phase);
            n.process(in.data(), in.size(), r, out);
        }
        const size_t frames = out.size() / av::kFrameBytes;
        double worst = 0.0, peak = 0.0;
        for (size_t i = 1; i < frames; ++i) {
            worst = std::max(worst, std::fabs(static_cast<double>(left(out, i)) - left(out, i - 1)));
            peak = std::max(peak, std::fabs(static_cast<double>(left(out, i))));
        }
        expect(worst <= maxStep, "ratio " + std::to_string(target) + ": a sample step of " +
                                     std::to_string(worst) + " (click) over " + std::to_string(maxStep));
        expect(peak <= amp + 64.0, "ratio " + std::to_string(target) + ": overshoot " +
                                       std::to_string(peak));
        int crossings = 0;
        const size_t from = static_cast<size_t>(3.5 * av::kRateHz);
        for (size_t i = from + 1; i < from + av::kRateHz; ++i)
            crossings += left(out, i - 1) < 0 && left(out, i) >= 0;
        const double want = 1000.0 * target;
        expect(std::fabs(crossings - want) <= 2.0,
               "ratio " + std::to_string(target) + ": " + std::to_string(crossings) + " Hz, want " +
                   std::to_string(want));
        std::printf("resampler: ratio %.6f slewed in and out: largest step %.0f (click bound %.0f), "
                    "peak %.0f, %d Hz, moved %s\n",
                    target, worst, maxStep, peak, crossings, ms(n.appliedUs()).c_str());
    }
}

// ---------------------------------------------------------------- the model

struct Step {
    double fromS, toS, shiftMs;  // toS < 0: to the end; + shift = audio late
};

struct Plant {
    double downstreamDriftMsPerMin = -0.87;  // after the pickup: HDMI only, the daemon never sees it
    std::vector<Step> steps;
    double seconds = 1920.0;
    bool feature = true;
};

struct Run {
    std::vector<double> hdmiUs, tS;
    std::vector<av::AvSyncEvent> events;
};

Run simulate(const Plant& p) {
    Run run;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-8.0, 8.0);
    std::uniform_real_distribution<double> pace(0.0, 0.002);
    const misterplex::FpgaBuildInfo* row = misterplex::recognisedFpgaBuild(0x48005D45u);
    const double delayS = misterplex::fpgaAudioDelayMs(50, row) / 1000.0;  // 50 - 45 = 5 ms
    const double latency = 0.080, vsync = 1.0 / 60.0, phase = 0.0073;
    const double frameS = 1001.0 / 24000.0;
    // Start boundary offset ~ 5 - 80 - 8.3 - 1 = -84.3 ms; +91 puts HDMI at about +6.7 ms,
    // where soak A's start (+52.7 ms at 50) lands with the -45 ms trim.
    const double downstreamMs = 91.0;

    av::PcmRateNudger nudger;
    av::AudioContentMap map;
    map.reset(0);
    av::AvSyncController ctl;
    if (p.feature)
        ctl.setBaseRatio(1.0 + row->avDriftPpm * 1e-6);
    // The audio actually played: the nudger's own output -> input record, kept apart from the
    // daemon's map.
    std::vector<std::pair<uint64_t, double>> played{{0, 0.0}};
    auto playedContentS = [&](uint64_t n) {
        auto hi = std::lower_bound(played.begin(), played.end(), n,
                                   [](const std::pair<uint64_t, double>& a, uint64_t v) {
                                       return a.first < v;
                                   });
        if (hi == played.begin())
            return hi->second / av::kRateHz;
        const auto lo = std::prev(hi);
        const double f = static_cast<double>(n - lo->first) /
                         static_cast<double>(hi->first - lo->first);
        return (lo->second + f * (hi->second - lo->second)) / av::kRateHz;
    };
    double tonePhase = 0.0;
    uint64_t written = 0;
    const double tPresent0 = 0.5, tAudio0 = tPresent0 + delayS;
    auto consumed = [&](double t) -> uint64_t {
        if (t <= tAudio0)
            return 0;
        return std::min<uint64_t>(written, static_cast<uint64_t>((t - tAudio0) * av::kRateHz));
    };
    std::vector<uint8_t> out;
    auto pump = [&](double t) {
        while (written < consumed(t) + 4800) {
            const auto in = tone(960, 1000.0, 12000.0, tonePhase);
            out.clear();
            const double r = p.feature ? ctl.targetRatio() : 1.0;
            written += nudger.process(in.data(), in.size(), r, out);
            map.add(nudger.outputFrames(), nudger.inputPosition());
            played.push_back({nudger.outputFrames(), nudger.inputPosition()});
            if (p.feature)
                ctl.noteApplied(nudger.appliedUs(), static_cast<int64_t>(t * 1e6));
        }
    };
    pump(tAudio0);
    double nextPump = tAudio0;
    const int frames = static_cast<int>(p.seconds / frameS);
    for (int k = 1; k < frames; ++k) {
        const double v = k * frameS;
        const double tSubmit = tPresent0 + v + pace(rng);
        const double tAck = phase + std::ceil((tSubmit + latency - phase) / vsync) * vsync;
        while (nextPump <= tAck) {
            pump(nextPump);
            nextPump += 0.020;
        }
        double shiftS = 0.0;
        for (const Step& s : p.steps)
            if (tAck >= s.fromS && (s.toS < 0 || tAck < s.toS))
                shiftS += s.shiftMs / 1000.0;
        const double shown = v + shiftS;  // content on screen (+ shift = audio late)
        const uint64_t nAck = consumed(tAck);
        int64_t contentUs = 0;
        if (!map.contentUs(nAck, contentUs))
            continue;
        if (p.feature) {
            ctl.addMeasurement(static_cast<int64_t>(shown * 1e6) - contentUs,
                               static_cast<int64_t>((tAck + 0.003) * 1e6));
            for (const auto& e : ctl.takeEvents())
                run.events.push_back(e);
        }
        const double hdmiMs = (shown - playedContentS(nAck)) * 1000.0 + downstreamMs +
                              p.downstreamDriftMsPerMin * (tAck / 60.0) + jitter(rng);
        run.hdmiUs.push_back(hdmiMs * 1000.0);
        run.tS.push_back(tAck);
    }
    return run;
}

struct Stats {
    std::vector<double> windowMedMs, windowT;
    double p5 = 0, p50 = 0, p95 = 0, slopeMsPerMin = 0, lastTenMinMs = 0;
    int over40 = 0;
};

double pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(q * (v.size() - 1))];
}

Stats stats(const Run& r) {
    Stats s;
    std::vector<double> all, win, tail;
    for (double h : r.hdmiUs)
        all.push_back(h / 1000.0);
    s.p5 = pct(all, 0.05);
    s.p50 = pct(all, 0.50);
    s.p95 = pct(all, 0.95);
    double winStart = 0.0;
    const double end = r.tS.empty() ? 0.0 : r.tS.back();
    for (size_t i = 0; i < r.tS.size(); ++i) {
        while (r.tS[i] >= winStart + 10.0) {
            if (!win.empty()) {
                const double m = pct(win, 0.5);
                s.windowMedMs.push_back(m);
                s.windowT.push_back(winStart);
                s.over40 += std::fabs(m) > 40.0;
            }
            win.clear();
            winStart += 10.0;
        }
        win.push_back(r.hdmiUs[i] / 1000.0);
        if (r.tS[i] >= end - 600.0)
            tail.push_back(r.hdmiUs[i] / 1000.0);
    }
    s.lastTenMinMs = tail.empty() ? 0.0 : pct(tail, 0.5);
    std::vector<double> xs, ys;
    for (size_t i = 0; i < s.windowMedMs.size(); ++i) {
        if (s.windowT[i] >= 120.0) {
            xs.push_back((s.windowT[i] + 5.0) / 60.0);
            ys.push_back(s.windowMedMs[i]);
        }
    }
    double mx = 0, my = 0;
    for (size_t i = 0; i < xs.size(); ++i) { mx += xs[i]; my += ys[i]; }
    mx /= xs.size(); my /= ys.size();
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        sxy += (xs[i] - mx) * (ys[i] - my);
        sxx += (xs[i] - mx) * (xs[i] - mx);
    }
    s.slopeMsPerMin = sxx > 0 ? sxy / sxx : 0.0;
    return s;
}

struct Counts {
    int starts = 0, dones = 0, undos = 0, disabled = 0;
};

Counts report(const char* name, const Stats& s, const Run& r) {
    double lo = 1e9, hi = -1e9;
    for (double m : s.windowMedMs) { lo = std::min(lo, m); hi = std::max(hi, m); }
    std::printf("%s: p5/p50/p95 %+.1f/%+.1f/%+.1f ms, %zu windows %+.1f..%+.1f ms, |med|>40: %d, "
                "drift %+.3f ms/min, last 10 min %+.1f ms\n",
                name, s.p5, s.p50, s.p95, s.windowMedMs.size(), lo, hi, s.over40, s.slopeMsPerMin,
                s.lastTenMinMs);
    Counts c;
    for (const auto& e : r.events) {
        switch (e.kind) {
        case av::AvSyncEvent::ResyncStart:
            ++c.starts;
            std::printf("  resync start: %s vs reference\n", ms(e.offsetUs).c_str());
            break;
        case av::AvSyncEvent::ResyncDone:
            ++c.dones;
            std::printf("  resync: offset before=%s after=%s, audio moved %s in %.1f s\n",
                        ms(e.beforeUs).c_str(), ms(e.afterUs).c_str(), ms(e.movedUs).c_str(),
                        e.durationUs / 1e6);
            break;
        case av::AvSyncEvent::UndoStart: ++c.undos; break;
        case av::AvSyncEvent::Disabled: ++c.disabled; break;
        default: break;
        }
    }
    return c;
}

void testModel() {
    // 1. Soak A as measured: the downstream drift only.
    Plant a;
    const Run aOn = simulate(a);
    const Stats sa = stats(aOn);
    const Counts ca = report("soak A model, on ", sa, aOn);
    expect(sa.over40 == 0, "soak A on: " + std::to_string(sa.over40) + " windows over 40 ms");
    expect(sa.p50 >= 0.0 && sa.p50 <= 10.0, "soak A on: p50 " + std::to_string(sa.p50));
    expect(std::fabs(sa.slopeMsPerMin) < 0.1, "soak A on: drift left " +
                                                  std::to_string(sa.slopeMsPerMin));
    expect(ca.starts == 0 && ca.undos == 0, "soak A on: drift alone never resyncs");
    a.feature = false;
    const Run aOff = simulate(a);
    const Stats sb = stats(aOff);
    report("soak A model, off", sb, aOff);
    expect(std::fabs(sb.slopeMsPerMin + 0.87) < 0.05,
           "negative control: the -0.87 ms/min drift comes back: " + std::to_string(sb.slopeMsPerMin));

    // 2. A persistent excursion to about +66 ms at 65 s, with the drift.
    Plant b;
    b.steps.push_back({65.0, -1.0, 60.0});
    const Run bOn = simulate(b);
    const Stats so = stats(bOn);
    const Counts cb = report("+66 ms persistent, on ", so, bOn);
    expect(cb.starts == 1 && cb.dones == 1 && cb.undos == 0,
           "one resync for one persistent excursion");
    for (const auto& e : bOn.events)
        if (e.kind == av::AvSyncEvent::ResyncDone)
            expect(e.beforeUs > 54000 && std::llabs(e.afterUs) <= 5000,
                   "resync before " + ms(e.beforeUs) + " after " + ms(e.afterUs));
    expect(so.over40 == 0, "+66 ms on: " + std::to_string(so.over40) + " windows over 40 ms");
    double worstAfter = 0.0;
    for (size_t i = 0; i < bOn.tS.size(); ++i)
        if (bOn.tS[i] >= 72.0)
            worstAfter = std::max(worstAfter, std::fabs(bOn.hdmiUs[i] / 1000.0));
    expect(worstAfter <= 40.0, "+66 ms on: a frame at " + std::to_string(worstAfter) +
                                   " ms 7 s after the excursion");
    expect(so.p50 >= 0.0 && so.p50 <= 10.0 && std::fabs(so.slopeMsPerMin) < 0.1,
           "+66 ms on: held near the start, no drift");
    b.feature = false;
    const Run bOff = simulate(b);
    const Stats sf = stats(bOff);
    report("+66 ms persistent, off", sf, bOff);
    expect(sf.over40 >= 100, "+66 ms off fails the 40 ms bar: " + std::to_string(sf.over40));

    // 3. Passing glitches leave nothing behind: +60 ms for 5 s, and video 1 s late for 3 s.
    const Step glitches[] = {{65.0, 70.0, 60.0}, {65.0, 68.0, -1000.0}};
    const char* names[] = {"+60 ms for 5 s, on    ", "video 1 s late 3 s, on"};
    for (int g = 0; g < 2; ++g) {
        Plant c;
        c.steps.push_back(glitches[g]);
        const Run cOn = simulate(c);
        const Stats sc = stats(cOn);
        const Counts cc = report(names[g], sc, cOn);
        expect(cc.undos == 0 && cc.disabled == 0, std::string(names[g]) + ": no undo/disable");
        expect(std::fabs(sc.lastTenMinMs - sa.lastTenMinMs) < 5.0,  // the 5 ms stop band
               std::string(names[g]) + ": the last 10 min sit at " +
                   std::to_string(sc.lastTenMinMs) + " ms, the clean run at " +
                   std::to_string(sa.lastTenMinMs));
        int over = 0;
        for (size_t i = 0; i < sc.windowMedMs.size(); ++i)
            over += sc.windowT[i] >= 80.0 && std::fabs(sc.windowMedMs[i]) > 40.0;
        expect(over == 0, std::string(names[g]) + ": windows over 40 ms after the glitch");
    }
}

void testFailSafe() {
    // The measurement does not follow the audio: one pull to the cap, then it is put back and
    // the resync turns off; the drift compensation (base ratio) stays.
    av::AvSyncController c;
    c.setBaseRatio(1.0 - 14.6e-6);
    int64_t t = 0;
    double applied = 0.0;
    for (int i = 0; i < 2000; ++i, t += 41708) {
        c.addMeasurement(i < 300 ? 0 : 60000, t);
        applied += (c.targetRatio() - 1.0) * 41708.0;  // what the nudger moves meanwhile
        c.noteApplied(static_cast<int64_t>(applied), t);
    }
    bool undo = false, off = false;
    for (const auto& e : c.takeEvents()) {
        undo = undo || e.kind == av::AvSyncEvent::UndoStart;
        off = off || e.kind == av::AvSyncEvent::Disabled;
    }
    const double net = applied - (-14.6e-6) * static_cast<double>(t);  // minus the compensation
    expect(undo && off && c.disabled() && c.targetRatio() == 1.0 - 14.6e-6,
           "a pull the measurement does not follow is undone, then the resync is off");
    expect(std::fabs(net) < 2000.0, "after the undo the pull's move is gone: " + ms(net));
    std::printf("fail safe: a pull the measurement does not follow is put back (net %s) and "
                "the resync turns off; the drift compensation stays\n", ms(net).c_str());
}

// ---------------------------------------------------------------- rows

void testRows() {
    using misterplex::FpgaBuildInfo;
    // 0x48005D52 (dec50 cut 4, 9bdb535b) is a 50 MHz row with the same A/V values, and so is
    // 0x4800A152 (the R1 refit on 5D52's decode/present path, 45ff6daf).
    // 0x48009E36 (the fixes refit on A152, ea4ca1e0) keeps A152's A/V values, and so does its P_PART port
    // 0x48002731 (9801dad2), and so does the deblock fit 0x4800C88F (7cb897b7), and so does the P_PART
    // ship build 0x48003ECB (f32947c5), and so does Track B 0x480058BC (C88F + fix21 + Status block, cb1e5f8b),
    // and so does D-PP1 0x480094DD (C88F + P_PART + partitioned-bS, 01d2e473), and so does
    // T-DPP1-STATUS 0x48018AE0 (the 94DD core + the 029A Status block, e7a38f4a), and so does
    // T-ASCAL-FOLD 0x48032F84 (the 94DD core + the 4-cone ascal fold, 6b3832ac3), and so does
    // T-ASCAL-FOLD-SYNC 0x48038D9A (the same fold on the E-port ascal, 99fe93e1e).
    for (uint32_t id : {0x48005D45u, 0x4800A1FBu, 0x48005D50u, 0x48005D52u, 0x4800A152u, 0x48009E36u,
                        0x48002731u, 0x4800C88Fu, 0x48003ECBu, 0x480058BCu, 0x480094DDu,
                        0x48018AE0u, 0x48032F84u, 0x48038D9Au, 0x48FF65F2u, 0x487A1BD1u,
                        0x48C5385Cu, 0x48C51FF1u, 0x48CFEE31u, 0x48110015u}) {
        const FpgaBuildInfo* b = misterplex::recognisedFpgaBuild(id);
        expect(b && b->avTrimMs == -45 && b->avResync && b->avDriftPpm == -14.6,
               "45/50 MHz row: trim -45, resync, drift -14.6 ppm");
    }
    int others = 0;
    for (const FpgaBuildInfo& b : misterplex::kRecognisedFpgaBuilds) {
        if (b.id == 0x48005D45u || b.id == 0x4800A1FBu || b.id == 0x48005D50u ||
            b.id == 0x48005D52u || b.id == 0x4800A152u || b.id == 0x48009E36u || b.id == 0x48002731u ||
            b.id == 0x4800C88Fu || b.id == 0x48003ECBu || b.id == 0x480058BCu ||
            b.id == 0x480094DDu || b.id == 0x48018AE0u || b.id == 0x48032F84u ||
            b.id == 0x48038D9Au || b.id == 0x48FF65F2u || b.id == 0x487A1BD1u || b.id == 0x48C5385Cu ||
            b.id == 0x48C51FF1u || b.id == 0x48CFEE31u || b.id == 0x48110015u)
            continue;
        ++others;
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", b.id);
        expect(b.avTrimMs == 0 && !b.avResync && b.avDriftPpm == 0.0,
               std::string(hex) + " must keep trim 0, no resync, no drift compensation");
    }
    // The rows the order names, as they were at c5334354.
    struct Row { uint32_t id; const char* raster; bool b15, b12; int conf; } const was[] = {
        {0x48004D40u, "480p", false, false, 9},  // RBF 999800cf
        {0x48005D40u, "480p", false, false, 9},  // N5
        {0x2400F36Au, "240p", false, false, 9},
        {0x2400F40Bu, "240p", true, true, 10},
        {0x4801F40Au, "480i", true, true, 10},
    };
    for (const Row& w : was) {
        const FpgaBuildInfo* b = misterplex::recognisedFpgaBuild(w.id);
        expect(b && std::strcmp(b->raster, w.raster) == 0 && b->byte15Echoed == w.b15 &&
                   b->byte12Echoed == w.b12 && b->confVersion == w.conf && b->avTrimMs == 0 &&
                   !b->avResync && b->avDriftPpm == 0.0,
               "row unchanged");
        expect(misterplex::fpgaAudioDelayMs(50, b) == 50, "conf 50 stays 50 on a row without trim");
    }
    expect(misterplex::fpgaAudioDelayMs(50, misterplex::recognisedFpgaBuild(0x48005D45u)) == 5,
           "48005D45: 50 - 45 = 5 ms");
    expect(misterplex::fpgaAudioDelayMs(0, misterplex::recognisedFpgaBuild(0x48005D45u)) == 0,
           "48005D45: a 0 conf clamps at 0");
    expect(misterplex::fpgaAudioDelayMs(50, nullptr) == 50, "no row: the conf value");
    std::printf("rows: 48005D45/48005D50 trim -45 ms (conf 50 -> 5 ms), drift -14.6 ppm, resync on; "
                "%d other rows (999800cf 48004D40, N5 48005D40, 240p, 480i, ...) unchanged: trim 0, "
                "no compensation, no resync\n",
                others);
}

}  // namespace

int main() {
    testPassthrough();
    testClicksAndPitch();
    testFailSafe();
    testRows();
    testModel();
    if (fails) {
        std::fprintf(stderr, "FAIL av sync: %d check(s)\n", fails);
        return 1;
    }
    std::printf("PASS av sync (45/50 MHz: trim + drift compensation hold soak A flat, a lasting "
                "excursion is pulled back, passing glitches leave nothing; off reproduces the drift)\n");
    return 0;
}
