// Unit tests for present cadence (24→60 3:2, 30→60 2:2, identity).
// Run: make unit (from the repo root)

#include "../../host/libmisterplex/cadence.hpp"

#include <cstdio>
#include <cstdlib>

static int fails = 0;

#define EXPECT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

int main() {
    using misterplex::should_advance_unique;
    using misterplex::unique_frames_in;

    // 60 content on 60 display: always advance
    EXPECT(should_advance_unique(0, 60, 60), "60@60 tick0 advance");
    EXPECT(unique_frames_in(60, 60, 60) == 60, "60@60 → 60 unique/sec");

    // 30 on 60: content_index = floor(n/2) → advance on 0,2,4,…
    EXPECT(unique_frames_in(60, 30, 60) == 30, "30@60 → 30 unique");
    EXPECT(should_advance_unique(0, 30, 60), "30@60 n=0 advance");
    EXPECT(!should_advance_unique(1, 30, 60), "30@60 n=1 hold");
    EXPECT(should_advance_unique(2, 30, 60), "30@60 n=2 advance");
    EXPECT(!should_advance_unique(3, 30, 60), "30@60 n=3 hold");

    // 24 on 60: floor(n*24/60) → 24 unique per 60 display
    EXPECT(unique_frames_in(60, 24, 60) == 24, "24@60 → 24 unique");
    // First 5 ticks (n=0..4): content indices 0,0,0,1,1 → advances at 0 and 3 → 2
    int adv5 = unique_frames_in(5, 24, 60);
    EXPECT(adv5 == 2, "24@60 first 5 ticks → 2 advances (3:2 group)");
    EXPECT(should_advance_unique(0, 24, 60), "24@60 n=0 advance");
    EXPECT(!should_advance_unique(1, 24, 60), "24@60 n=1 hold");
    EXPECT(!should_advance_unique(2, 24, 60), "24@60 n=2 hold");
    EXPECT(should_advance_unique(3, 24, 60), "24@60 n=3 advance");

    // 12 on 60
    EXPECT(unique_frames_in(60, 12, 60) == 12, "12@60 → 12 unique");

    // 25 on 50 (PAL film-ish)
    EXPECT(unique_frames_in(50, 25, 50) == 25, "25@50 → 25 unique");

    // 24 on 50 ≈ 24 unique
    EXPECT(unique_frames_in(50, 24, 50) == 24, "24@50 → 24 unique");

    // Degenerate: cf=0 treated as always advance in RTL; match host
    EXPECT(should_advance_unique(3, 0, 60), "cf=0 always advance");

    using misterplex::cadenceMailboxFps;
    EXPECT(cadenceMailboxFps(24000, 1001) == 24, "24000/1001 mailbox 24");
    EXPECT(cadenceMailboxFps(24, 1) == 24, "24/1 mailbox 24");
    EXPECT(cadenceMailboxFps(30000, 1001) == 30, "30000/1001 mailbox 30");
    EXPECT(cadenceMailboxFps(30, 1) == 30, "30/1 mailbox 30");
    EXPECT(cadenceMailboxFps(60, 1) == 30, "60/1 mailbox 30");
    EXPECT(cadenceMailboxFps(60000, 1001) == 30, "60000/1001 mailbox 30");
    EXPECT(cadenceMailboxFps(0, 1) == 0, "0 mailbox 0");
    EXPECT(unique_frames_in(60, cadenceMailboxFps(24000, 1001), 60) == 24,
           "NTSC film clock mailbox 3:2 vs 60");
    EXPECT(unique_frames_in(60, cadenceMailboxFps(30000, 1001), 60) == 30,
           "NTSC 29.97 clock mailbox 2:2 vs 60");

    if (fails) {
        std::fprintf(stderr, "%d cadence test(s) failed\n", fails);
        return 1;
    }
    std::printf("test_cadence: OK\n");
    return 0;
}
