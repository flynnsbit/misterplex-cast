#pragma once

#include <cstdint>

namespace misterplex {

// MPEG 33-bit PTS wrap. Instance state (not thread_local) so a new play or
// seek does not inherit the previous title's correction.
struct PtsWrapGuard {
    int64_t lastPts = 0;
    int64_t correction = 0;
    bool initialized = false;

    int64_t apply(int64_t pts) {
        if (!initialized) {
            lastPts = pts;
            initialized = true;
            return pts;
        }
        const int64_t delta = pts - lastPts;
        if (delta < -(1LL << 32))
            correction += (1LL << 33);
        lastPts = pts;
        return pts + correction;
    }

    void reset() {
        lastPts = 0;
        correction = 0;
        initialized = false;
    }
};

} // namespace misterplex
