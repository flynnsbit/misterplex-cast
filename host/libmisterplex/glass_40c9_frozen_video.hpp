// GBE host regression for glass 40c9feb4 (2026-11-22 16:36 CT).
//
// Glass fact: SOURCE_ALIGNED=0 DECODER_ONLY=3 AMBIGUOUS=1 GATE_FAIL.
// Holes after t=5: 5.040 s audio_underrun, 18.507 s and 35.410 s
// video_stall_respawn. Cover log=1 (first-success on gen=7, soak-window 0).
// video_stall at remux after_ms=229402 (crypto Unable), ffmpeg respawn
// spawns=2. 17 libav "Immediate exit requested" lines (cover-wake interrupts,
// not product SourceError). Session lived 615 s, SourceError=0, zaps 6/6,
// muxed 1062 OK.
//
// Frozen video: media frames last increment 6628→6629 then stick at 6629;
// cumulative vfps 33.70→11.60; HDMI caps t=361–602 bit-identical (frozen
// Ebglyss frame). Audio holes end at t=286; video stays frozen.
//
// Why v3 (d004-arm-cover0 + burst e2e + e875-34 + fac2 parked) missed this:
// those gates judge burst-gap over-cover and parked-socket sticky-exit.
// 40c9feb4's wall-debt gate greens them, then still returns 1 / AVERROR_EXIT
// from onInterrupt when debt is true (a real underrun or post-stall quiet).
// lavf flushes the in-flight video PES, frames stop, and the 3 HDMI holes
// remain. The 17 Immediate exits cluster around the freeze.
//
// MUT_LIVE_ACCEPT_40C9_FROZEN_VIDEO must FAIL.
#pragma once

#include "libmisterplex/live_disc_rebase.hpp"

#include <cstdint>
#include <vector>

namespace misterplex {
namespace live_disc {

struct Glass40c9Hole {
    int64_t startMs = 0;
    int64_t durMs = 0;
};

inline constexpr int kGlass40c9DecoderOnlyCount = 3;
inline constexpr int64_t kGlass40c9LongestMs = 35410;
inline constexpr int kGlass40c9SoakCoverLogCount = 1;  // first-success, full soak
inline constexpr int kGlass40c9ImmediateExitCount = 17;
inline constexpr int kGlass40c9FrozenFrameCount = 6629;
inline constexpr int64_t kGlass40c9VideoStallAfterMs = 229402;
// Independent extract: C-1066-gate-v2-correlate.json (DECODER_ONLY only).
inline constexpr Glass40c9Hole kGlass40c9Holes[] = {
    {190350,  5040},
    {206344, 18507},
    {250544, 35410},
};

struct Glass40c9FrozenResult {
    std::vector<SegGap> holes;
    bool frozenVideo = false;
    int immediateExits = 0;
};

// abortInflightVideoOnWake: onInterrupt returns 1 / AVERROR_EXIT for a cover
//     wake (40c9feb4, and the d004/abfe/fac2 abort-wake family).
// recoversVideoAfterStall: product restarts video after a crypto/video_stall
//     wedge so frames keep advancing (absent on 40c9feb4).
// Wall-debt is not sufficient: 40c9feb4 has it and still freezes.
inline Glass40c9FrozenResult simulateGlass40c9FrozenVideo(bool abortInflightVideoOnWake,
                                                         bool recoversVideoAfterStall) {
    Glass40c9FrozenResult r;
    const bool ok = !abortInflightVideoOnWake || recoversVideoAfterStall;
    if (ok)
        return r;
    r.frozenVideo = true;
    r.immediateExits = kGlass40c9ImmediateExitCount;
    for (const auto& h : kGlass40c9Holes) {
        if (h.durMs * 90 >= kSoakGapGateTicks90k) {
            SegGap g;
            g.startTicks = h.startMs * 90;
            g.durTicks = h.durMs * 90;
            r.holes.push_back(g);
        }
    }
    return r;
}

}  // namespace live_disc
}  // namespace misterplex
