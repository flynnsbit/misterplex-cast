#pragma once
// Finished zone for the daemon's own "resume where I stopped" memory (Companion::clearMedia -> lastGood*). PMS marks an
// item watched at >= ~90 % or in its last 5 s and drops the viewOffset; a stop there must not become the resume point of
// the next cast (glass 5259d53 leg 2 C: a stop at 588917 ms of 600000 restarted at 588917 and hit Eof 11 s later).
// Unknown duration (<= 0) or a negative position: no claim, the stop is remembered as before.
#include <cstdint>

namespace misterplex {

inline bool inFinishedZone(int64_t posMs, int64_t durationMs) {
#ifdef MUT_RZ_NO_FINISHED_ZONE
    (void)posMs;
    (void)durationMs;
    return false;
#else
    if (durationMs <= 0 || posMs < 0)
        return false;
#ifdef MUT_RZ_NO_TAIL_5S
    return posMs * 100 >= durationMs * 90;
#else
    return posMs * 100 >= durationMs * 90 || durationMs - posMs <= 5000;
#endif
#endif
}

}  // namespace misterplex
