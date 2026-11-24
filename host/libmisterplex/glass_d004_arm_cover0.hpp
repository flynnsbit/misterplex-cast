// GBE host regression for glass d0045771 (2026-11-22 15:27 CT).
//
// Glass fact: SOURCE_ALIGNED=0 DECODER_ONLY=6 GATE_FAIL, longest 95.351 s,
// video live, zaps 6/6 audible, muxed 1062 OK, SourceError=0, Immediate
// exit=0 (sticky-exit death is fixed; the session lived 615 s). Soak-window
// extinf PCM cover log=0, fill=0, recover=0, wedge wake=0. Interiors are
// the HDMI capture floor (peak 12–32, RMS ~2, no |s|>64).
//
// ARM-vs-host-libav gap the xhigh4 RCA prompt names (and reviewer 3b01a5bc
// reproduced on a real openAvSource / HttpRangeReader fixture):
//
//   Host parked-socket / blocked-read models (quiet grows, no PES) predicted
//   1018 covers / 0 holes. On ARM the Pluto relay keeps flowing: lavf / the
//   custom AVIO returns video (and later audio) in bursts with >=100 ms of
//   quiet between them. lastByteMs_ ages across those burst gaps, so
//   armExtinfCoverWake / audioEmitWallQuiet fire on a live stream. Each
//   wake emits 200 ms of extra PCM and the late-PES snap restamps real
//   audio onto that inflated sample clock — audio PTS runs tens of seconds
//   ahead of video (host burst e2e on d0045771: ~1.9x PCM, +43.7 s lead in
//   10 s of media). The 6 HDMI holes are that desync, not a never-armed
//   blocked read. The soak needle is 0 because extinfCoverLogged_ is
//   first-only and the first fire is outside the soak window.
//
// A wall-clock cover is only right when emitted PCM is behind wall time
// (a real HDMI underrun). MUT_LIVE_ACCEPT_D004_ARM_STILL_HOLES must FAIL.
#pragma once

#include "libmisterplex/live_disc_rebase.hpp"

#include <cstdint>
#include <vector>

namespace misterplex {
namespace live_disc {

struct GlassD004ArmHole {
    int peak = 0;
    int rms = 0;
    int64_t startMs = 0;
    int64_t durMs = 0;
};

inline constexpr int kGlassD004ArmHoleCount = 6;
inline constexpr int64_t kGlassD004ArmLongestMs = 95351;
inline constexpr int kGlassD004ArmSoakCoverLogCount = 0;
// Burst fixture used by test_1066_wedge_wake_e2e (400 ms quiet, flowing PES).
inline constexpr int64_t kD004BurstGapMs = 400;
inline constexpr int64_t kD004BurstWakeQuietMs = 100;
inline constexpr int64_t kD004CoverEmitMs = 200;
// 10 s of media in the burst e2e; d0045771 ran ~+43.7 s of audio PTS lead.
inline constexpr int64_t kD004BurstWindowMs = 10000;
inline constexpr int64_t kD004BurstLeadFailMs = 2 * 1000;

// Independent extract: C-1066-gate-v2-correlate.json start/dur (ms rounded)
// + full-window peak/RMS from ev/C-1066-soak.wav (48 kHz s16le stereo).
inline constexpr GlassD004ArmHole kGlassD004ArmHoles[] = {
    {31, 2, 120893, 72882},
    {29, 2, 202798,  1102},
    {14, 2, 203901, 79988},
    {32, 2, 291303, 87713},
    {32, 2, 385064, 92532},
    {32, 2, 482145, 95351},
};

// Flowing-relay model: PES keeps arriving in bursts; quiet between bursts
// is kD004BurstGapMs. lastByteMs_ ages, so a lastByte-only wake arms every
// gap and emits kD004CoverEmitMs of extra PCM (sample clock inflates).
// wallDebtGate: product only arms when emitted PCM is behind wall time.
// Returns residual HDMI holes (>=0.2 s) of the glass 6-hole class. leadMs
// is the audio-PTS lead accumulated over kD004BurstWindowMs of flowing PES.
inline std::vector<SegGap> simulateGlassD004ArmBurstGaps(bool wallDebtGate,
                                                         int& covers,
                                                         int64_t& leadMs) {
    covers = 0;
    leadMs = 0;
    if (!wallDebtGate) {
        const int64_t gaps = kD004BurstWindowMs / (kD004BurstGapMs + 1);
        covers = static_cast<int>(gaps);
        leadMs = gaps * kD004CoverEmitMs;
    }
    std::vector<SegGap> out;
    if (wallDebtGate)
        return out;
    for (const auto& h : kGlassD004ArmHoles) {
        if (h.durMs * 90 >= kSoakGapGateTicks90k) {
            SegGap g;
            g.startTicks = h.startMs * 90;
            g.durTicks = h.durMs * 90;
            out.push_back(g);
        }
    }
    return out;
}

}  // namespace live_disc
}  // namespace misterplex
