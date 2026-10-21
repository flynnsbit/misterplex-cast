#pragma once
#include <cstdint>
#include <climits>
#include "libmisterplex/pts_cadence.hpp"

namespace misterplex {

inline int64_t clampPtsJump(int64_t pts, int64_t& last_ok_pts, int64_t frame_duration_ticks) {
    if (frame_duration_ticks <= 0)
        return pts;
    if (last_ok_pts == INT64_MIN) {
        last_ok_pts = pts;
        return pts;
    }
    const int64_t delta = pts - last_ok_pts;
    if (delta < 0 || delta > 2 * frame_duration_ticks)
        pts = last_ok_pts + frame_duration_ticks;
    last_ok_pts = pts;
    return pts;
}

// Gate A already accepts integer N-frame jumps (N<=48) after oversize AU skips.
// Those must pass through: clamping them to last+1 frame leaves later AUs
// N-ahead of last_ok_pts_ so every following picture also trips (c782 f3).
inline bool ptsJumpIsAcceptedNFrames(int64_t pts, int64_t last_ok_pts,
                                     uint32_t tbNum, uint32_t tbDen) {
    if (last_ok_pts == INT64_MIN || tbNum == 0 || tbDen == 0)
        return false;
    const long double ticks =
        (static_cast<long double>(pts) - last_ok_pts) * tbNum;
    return pts_delta_is_n_frames(ticks, tbNum, tbDen, 48);
}

inline int64_t frameDurationTicks(uint32_t tbNum, uint32_t tbDen, int fpsNum, int fpsDen) {
    if (tbNum == 0 || tbDen == 0 || fpsNum <= 0 || fpsDen <= 0)
        return 0;
    return static_cast<int64_t>(
        static_cast<long double>(tbDen) * fpsDen /
        (static_cast<long double>(tbNum) * fpsNum));
}

}  // namespace misterplex
