// GBE host regression for glass 82b99128 (2026-11-22 17:12 CT).
//
// Glass fact: GATE_V2_NO_HOST_REF (kit hang, correlator unscorable) but the
// product log is readable. Soak 615 s, 26 HDMI gaps after t=5 totaling
// 586.479 s (longest 30.960 s), vfps 212.6→1.5 (last media frames=932),
// video_stall at soak t=+23 (after_ms=52867, sent=7214688, empty err),
// Immediate exit=0, cover=1 / wake=1 (first-success at soak t=−23,
// audio_quiet_ms=102 byte_quiet_ms=101), pts_jump rewind emit_pts=895166,
// ready_q=0, RSS ~34 MB (not an ARM memory ceiling). Zaps 6/6 audible,
// muxed 1062 OK.
//
// Mechanism (not just the symptom): armExtinfCoverWake stored
// fillDeadlineMs_ = now (already due). demuxOne saw fillDeadlineHit() and
// returned FillTimeout BEFORE av_read_frame, so libav never ran and there
// are zero Immediate-exit lines. The video PID went idle; the 30 s
// watchdog killed ffmpeg; the respawn rewound; the decoder starved for
// the rest of the soak (932 frames vs ~18450 at 30 fps).
//
// 40c9feb4 has the wall-debt gate so a caught-up stream does not arm, and
// held ~30 fps until a real crypto failure. It still stored now, so the
// post-stall respawn was aborted (different class: frozen frames=6629).
// 82b99128 is worse: no debt gate, abort starts at first picture.
//
// A passing product (1) refuses to arm while emitted PCM is caught up
// (wall-debt) and (2) stores the deadline one interrupt poll in the
// future so a ready PES returns. MUT_LIVE_ACCEPT_82B_DECODER_STARVE
// must FAIL.
#pragma once

#include "libmisterplex/live_disc_rebase.hpp"

#include <cstdint>
#include <vector>

namespace misterplex {
namespace live_disc {

inline constexpr int kGlass82bHdmiGapCountAfterT5 = 26;
inline constexpr int64_t kGlass82bHdmiGapTotalMs = 586479;
inline constexpr int64_t kGlass82bLongestGapMs = 30960;
inline constexpr int kGlass82bFramesEnd = 932;
inline constexpr int64_t kGlass82bSoakMs = 615000;
inline constexpr double kGlass82bVfpsFirst = 212.6;  // startup spike, glass frames-full
inline constexpr double kGlass82bVfpsLast = 1.5;     // 932 frames / ~615 s wall
// 212.6/1.5 ≈ 141.7. A last vfps below first/10 is the collapse signature.
inline constexpr double kGlass82bVfpsCollapseRatio = 10.0;
inline constexpr int64_t kGlass82bVideoStallAfterMs = 52867;
inline constexpr int64_t kGlass82bVideoStallSentBytes = 7214688;
inline constexpr int kGlass82bImmediateExitCount = 0;
inline constexpr int kGlass82bCoverLogCount = 1;
inline constexpr int kGlass82bWakeLogCount = 1;
inline constexpr int kGlass82bHealthyPolls = 300;
inline constexpr int kGlass82bRespawnPolls = 50;
inline constexpr int64_t kGlass82bWatchdogPolls = 300;  // 30 s / 100 ms
inline constexpr int64_t kWakeArmLeadMs = 100;          // one interrupt slice

struct Glass82bStarveResult {
    int healthyBytes = 0;
    int respawnBytes = 0;
    int frames = 0;
    bool videoStall = false;
    int64_t holeMs = 0;
    double vfpsFirst = kGlass82bVfpsFirst;
    double vfpsLast = 0;
};

inline bool vfpsCollapsed(double first, double last) {
    return last > 0 && first / last >= kGlass82bVfpsCollapseRatio;
}

// requireDebt: wall-debt gate (caught-up PCM does not arm).
// armLeadMs: 0 means fillDeadline = now (FillTimeout before av_read_frame);
//            100 means a ready PES returns on this poll.
// 82b99128 is requireDebt=false, armLeadMs=0.
inline Glass82bStarveResult simulateGlass82bDecoderStarve(bool requireDebt,
                                                         int64_t armLeadMs) {
    Glass82bStarveResult r;
    const bool preempt = (armLeadMs <= 0);
    // Healthy phase: video PES ready, PCM caught up, audio quiet 102 ms
    // (the 82 first-picture wake).
    if (requireDebt) {
        r.healthyBytes = kGlass82bHealthyPolls;
    } else if (preempt) {
        r.healthyBytes = 0;
        r.videoStall = true;
        r.holeMs = kGlass82bHdmiGapTotalMs;
        // fall through so vfpsLast is the 212.6→1.5 collapse
    } else {
        r.healthyBytes = kGlass82bHealthyPolls;
    }
    // Respawn phase: wall debt, PES ready again (the 40 post-crypto child).
    if (preempt) {
        r.respawnBytes = 0;
        r.holeMs = kGlass82bHdmiGapTotalMs;
    } else {
        r.respawnBytes = kGlass82bRespawnPolls;
    }
    r.frames = r.healthyBytes + r.respawnBytes;
    if (r.videoStall)
        r.holeMs = kGlass82bHdmiGapTotalMs;
    // 82 glass: first-picture starve leaves 932 frames over the 615 s soak (vfps 1.5).
    // A yield keeps frames advancing (model 30 fps). The 40 post-stall class is
    // not this 212.6→1.5 signature.
    if (r.videoStall)
        r.vfpsLast = (static_cast<double>(kGlass82bFramesEnd) * 1000.0) /
                     static_cast<double>(kGlass82bSoakMs);
    else if (r.respawnBytes == 0 && r.holeMs > 0)
        r.vfpsLast = 0;  // 40-class respawn starve; not the 82 first-picture ratio
    else
        r.vfpsLast = 30.0;
    return r;
}

}  // namespace live_disc
}  // namespace misterplex
