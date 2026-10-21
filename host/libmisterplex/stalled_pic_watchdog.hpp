#pragma once
#include <cstdint>

namespace misterplex {

// PAUSE-WATCHDOG (v2-combined): paused time is not a picture stall. The gap is counted from the
// later of the last delivered picture and the last resume; while paused there is no stall.
// Ticks are steady_clock ticks; ticksPerMs converts. Before the first picture there is no stall.
inline bool stalledPicGapExceeded(bool videoStarted, bool paused, int64_t nowTicks, int64_t lastPicTicks,
                                  int64_t resumeTicks, int64_t ticksPerMs, int timeoutMs) {
    if (!videoStarted)
        return false;
#ifndef MUT_SPW_COUNT_PAUSED
    if (paused)
        return false;
    const int64_t ref = resumeTicks > lastPicTicks ? resumeTicks : lastPicTicks;
#else
    (void)paused;
    (void)resumeTicks;
    const int64_t ref = lastPicTicks;
#endif
    return (nowTicks - ref) / ticksPerMs >= timeoutMs;
}

inline bool stalledPicShouldReopen(bool stalled, int hits, int maxHits) {
    return stalled && hits < maxHits;
}

inline bool stalledPicShouldFail(bool stalled, int hits, int maxHits) {
    return stalled && hits >= maxHits;
}

}
