#pragma once
// Compressed-demux PTS cadence allow-list. Must match
// arm/misterplexd/av_inproc_decode.cpp Gate A/B.
//
// Product 8addcac1 accepted 24 and 24000/1001 only. Lab D3 also allows
// 30 and 30000/1001 with the same absolute tick tolerance style.

#include <cmath>
#include <cstdint>

namespace misterplex {

inline bool expected_fps_supported(int num, int den) {
    if (num == 0 && den == 0)
        return true;
    return (num == 24 && den == 1) || (num == 24000 && den == 1001) ||
           (num == 30 && den == 1) || (num == 30000 && den == 1001);
}

inline bool pts_delta_is_one_frame(long double ticks, long double tb_num, long double tb_den) {
    const bool film24 = std::fabs(ticks * 24 - tb_den) <= tb_num * 24;
    const bool film23976 =
        std::fabs(ticks * 24000 - tb_den * 1001) <= tb_num * 24000;
    const bool ntsc30 = std::fabs(ticks * 30 - tb_den) <= tb_num * 30;
    const bool ntsc2997 =
        std::fabs(ticks * 30000 - tb_den * 1001) <= tb_num * 30000;
    return film24 || film23976 || ntsc30 || ntsc2997;
}

inline bool pts_delta_is_n_frames(long double ticks, long double tb_num, long double tb_den,
                                  int n_max) {
    if (n_max < 2)
        return false;
    const long double frame24 = tb_den / 24.0L;
    const long double frame23976 = tb_den * 1001.0L / 24000.0L;
    const long double frame30 = tb_den / 30.0L;
    const long double frame2997 = tb_den * 1001.0L / 30000.0L;
    const long double tol = tb_num * 24;
    for (int n = 2; n <= n_max; ++n) {
        if (std::fabs(ticks - frame24 * n) <= tol || std::fabs(ticks - frame23976 * n) <= tol ||
            std::fabs(ticks - frame30 * n) <= tol || std::fabs(ticks - frame2997 * n) <= tol)
            return true;
    }
    return false;
}

// Long-run source-rate gate: tolerant of per-frame timestamp rounding/jitter,
// still rejects a wrong nominal rate (25 vs 23.976, 30 vs 29.97) over a long window.
inline bool pts_long_run_rate_ok(long double elapsedTicks, uint64_t frames, long double tb_num,
                                 long double tb_den, int fps_num, int fps_den) {
    if (fps_num <= 0 || fps_den <= 0 || tb_num <= 0 || tb_den <= 0)
        return false;
    if (frames < 120)
        return true;
    const long double nominal =
        static_cast<long double>(frames) * fps_den * tb_den / (static_cast<long double>(fps_num) * tb_num);
    const long double frameDur = static_cast<long double>(fps_den) * tb_den /
                                 (static_cast<long double>(fps_num) * tb_num);
    const long double slack = frameDur + 200e-6L * nominal;
    return std::fabs(elapsedTicks - nominal) <= slack;
}

}  // namespace misterplex
