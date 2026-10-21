#include "libmisterplex/pts_jump_clamp.hpp"
#include <cstdint>
#include <cstdio>
static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)

static int64_t applyCallSite(int64_t pts, int64_t& last, int64_t fd, uint32_t tbNum, uint32_t tbDen) {
    if (misterplex::ptsJumpIsAcceptedNFrames(pts, last, tbNum, tbDen)) {
        last = pts;
        return pts;
    }
    return misterplex::clampPtsJump(pts, last, fd);
}

int main() {
    using misterplex::clampPtsJump;
    using misterplex::frameDurationTicks;
    using misterplex::ptsJumpIsAcceptedNFrames;
    CHECK(frameDurationTicks(1, 90000, 30, 1) == 3000);
    CHECK(frameDurationTicks(1, 90000, 24, 1) == 3750);
    const int64_t fd = 3000;
    int64_t last = INT64_MIN;
    CHECK(clampPtsJump(0, last, fd) == 0);
    CHECK(last == 0);
    CHECK(clampPtsJump(3000, last, fd) == 3000);
    CHECK(last == 3000);
    // 1s forward jump must clamp to last+1 frame in TICKS (6000), not microseconds (66666).
    CHECK(clampPtsJump(90000, last, fd) == 6000);
    CHECK(last == 6000);
    // one-frame backward (−3000 ticks ~= −33 ms) must clamp; -50000 us hysteresis would miss it.
    last = 6000;
    CHECK(clampPtsJump(3000, last, fd) == 9000);
    CHECK(last == 9000);
    // 2-frame forward is allowed.
    last = 0;
    CHECK(clampPtsJump(0, last, fd) == 0);
    CHECK(clampPtsJump(6000, last, fd) == 6000);

    // c782 f3: demux-accepted N-frame jumps must pass through. Naive clampPtsJump
    // of a 5-frame 90 kHz / 30 fps skip collapses to last+1 then runaways.
    CHECK(ptsJumpIsAcceptedNFrames(15000, 0, 1, 90000));
    CHECK(ptsJumpIsAcceptedNFrames(6000, 0, 1, 90000));
    CHECK(!ptsJumpIsAcceptedNFrames(3000, 0, 1, 90000));
    CHECK(!ptsJumpIsAcceptedNFrames(15000, INT64_MIN, 1, 90000));
    CHECK(!ptsJumpIsAcceptedNFrames(12345, 0, 1, 90000));
    last = 0;
    CHECK(clampPtsJump(15000, last, fd) == 3000);
    CHECK(clampPtsJump(18000, last, fd) == 6000);
    last = INT64_MIN;
    CHECK(applyCallSite(0, last, fd, 1, 90000) == 0);
    CHECK(last == 0);
    CHECK(applyCallSite(15000, last, fd, 1, 90000) == 15000);
    CHECK(last == 15000);
    CHECK(applyCallSite(18000, last, fd, 1, 90000) == 18000);
    CHECK(last == 18000);
    // Backward is still clamped by the helper (demux already fails delta<=0).
    last = 6000;
    CHECK(!ptsJumpIsAcceptedNFrames(3000, last, 1, 90000));
    CHECK(applyCallSite(3000, last, fd, 1, 90000) == 9000);

    if (fails) { std::fprintf(stderr, "%d FAIL\n", fails); return 1; }
    return 0;
}
