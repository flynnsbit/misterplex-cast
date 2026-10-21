// Pins parent-measured 720p24 CPU-copy budget. RED if someone "fits" serial path
// by editing constants without new hardware evidence.
#include "libmisterplex/p720_e2e_budget.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                     \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

int main() {
    using namespace misterplex::p720_budget;
    int fails = 0;

    CHECK(kFrameBytes720pI420 == 1382400u, "720p I420 byte count");
    CHECK(std::fabs(kDeadline24Ms - 41.666666) < 0.001, "24fps deadline ~41.667");

    // Archive Sweep 9 pins must remain the named measured copy cost.
    CHECK(std::fabs(kCpuCopyMsPerFrame - 14.978) < 1e-9, "Sweep9 copy ms pin");
    CHECK(std::fabs(kDecodeOnlyMsPerFrameSweep116 - 32.705) < 1e-9, "Sweep116 decode ms pin");

    const double serial =
        serialDecodePlusCopyMs(kDecodeOnlyMsPerFrameSweep116, kCpuCopyMsPerFrame);
    std::printf("measured_serial_ms=%.3f deadline_ms=%.3f headroom_ms=%.3f\n", serial,
                kDeadline24Ms, headroomAfterDecodeMs(kDecodeOnlyMsPerFrameSweep116));
    std::printf("copy_ms=%.3f decode_ms=%.3f\n", kCpuCopyMsPerFrame,
                kDecodeOnlyMsPerFrameSweep116);

    // Primary: serial CPU path does NOT meet 24 fps with measured pins.
    CHECK(!kSerialSweep116Meets24, "serial Sweep116+copy must NOT meet 24fps");
    CHECK(kWcSweep116Meets24, "WC /dev/mplex_ddr copy_us=1.917 + Sweep116 meets 24");
    CHECK(kWcCopyMsPerFrame < 3.0, "WC copy is not Sweep-9 15ms /dev/mem");
    CHECK(serial > kDeadline24Ms, "serial sum exceeds deadline");
    CHECK(kCopyExceedsSweep116Headroom, "copy exceeds decode-only headroom");

    // Headroom arithmetic: 41.667 - 32.705 = 8.962 < 14.978
    const double head = headroomAfterDecodeMs(kDecodeOnlyMsPerFrameSweep116);
    CHECK(head < kCpuCopyMsPerFrame, "8.962 headroom < 14.978 copy");
    CHECK(std::fabs(head - 8.962) < 0.01, "headroom ~8.962");

    // Negative (naive wrong impl would pass if only decode checked):
    // decode-only alone DOES meet 24 — so a test that only checked decode
    // would be green while E2E is red. We require the copy term.
    CHECK(kDecodeOnlyMsPerFrameSweep116 < kDeadline24Ms, "decode-only fits (trap)");
    CHECK(!serialCpuPathMeets24(kDecodeOnlyMsPerFrameSweep116, kCpuCopyMsPerFrame),
          "decode-only fit must not imply serial fit");

    // Counterfactual: if copy were free, serial would meet — proves the copy
    // pin is load-bearing (not a tautology that always fails).
    CHECK(serialCpuPathMeets24(kDecodeOnlyMsPerFrameSweep116, 0.0),
          "counterfactual zero-copy would meet 24");
    CHECK(serialCpuPathMeets24(kDecodeOnlyMsPerFrameSweep116, 8.0),
          "counterfactual copy<=headroom would meet 24");
    CHECK(!serialCpuPathMeets24(kDecodeOnlyMsPerFrameSweep116, 9.0),
          "copy just above headroom fails");

    // Legacy 35.94 decode is even worse serially.
    CHECK(!serialCpuPathMeets24(kDecodeOnlyMsPerFrameLegacy, kCpuCopyMsPerFrame),
          "legacy decode+copy serial fail");

    // Sweep 118: named serial deficit pin (parent correction arithmetic).
    CHECK(std::fabs(kSerialDeficitSweep118Ms - 6.016) < 0.01, "Sweep118 deficit ~6.016 ms");
    CHECK(kSerialDeficitSweep118Ms > 0.0, "serial path still short of 24fps");
    CHECK(std::fabs(kPayloadRate720p24MBps - 33.1776) < 0.001, "R_req ~33.18 MB/s");

    // rd-duck blocking: idle% is at-rest, not concurrent with decode.
    CHECK(std::fabs(kIdlePctSweep116AtRest - 49.0) < 1e-9, "idle-at-rest pin 49%");
    CHECK(!kIdlePctSweep116IsConcurrentWithDecode, "49% must NOT mean free core in decode");
    CHECK(!kMayBudgetFreeCoreDuringDecode, "forbid free-core-during-decode budget");
    CHECK(!kDecodeCopyOverlapProven, "overlap unproven");

    // DMA scope: publication memcpy only — not "ARM never touches pixels".
    CHECK(kDmaRetiresPublicationMemcpyOnly, "DMA retires publication memcpy");
    CHECK(!kDmaMeansArmNeverTouchesPixels, "forbid never-touches-pixels claim");
    CHECK(kStrategicPublishPreference == PreferredPublishPath::FabricDirectReader,
          "prefer fabric direct reader");
    CHECK(!kSourceToBankMoverPreferred, "source→bank mover dispreferred");

    // Fit release: BOTH blockers unless parent resolves.
    CHECK(kFitBlockerNostubReclaim && kFitBlockerOsd720pRealReader2090Stalled,
          "both fit blockers armed");
    CHECK(kFitReleaseBlockerCount == 2, "two fit blockers");

    // rd-duck: DPB one-byte fetch is a hard 720p fabric blocker (not entropy-only).
    CHECK(kDpbFetchBytesPerPartition == 603, "603 B/partition (441Y+81U+81V)");
    CHECK(!kDpbPartWhNarrowsFetch, "part_w/h must not claim to narrow fetch");
    CHECK(kMbCount720p == 3600, "720p MB grid 80x45");
    CHECK(kDpbP16x16RefFetchCycles720p == 2170800LL, "603*3600 cycles");
    CHECK(std::fabs(kDpbP16x16RefFetchMs720pAt20MHz - 108.54) < 0.01, "108.54 ms @20MHz");
    CHECK(std::fabs(kI420WriteMs720pAt20MHz - 69.12) < 0.01, "I420 write 69.12 ms @20MHz");
    CHECK(!kDpbOneByteFetchMeets24At20MHz, "DPB fetch alone misses 24fps @20MHz");
    CHECK(!kRemainingWorkIsEntropyFrontendOnly, "forbid entropy-only remaining-work framing");
    CHECK(!kStubDpbMemProves720p, "stub 640x480 dpb_mem does not prove 720p");

    if (fails) {
        std::printf("test_p720_e2e_budget: %d FAIL(s)\n", fails);
        return 1;
    }
    std::printf("test_p720_e2e_budget: OK serial=%.3f deficit=%.3f idle=at_rest "
                "dma=pub_only fit_blockers=%d dpb_fetch=%.2fms@20MHz\n",
                serial, kSerialDeficitSweep118Ms, kFitReleaseBlockerCount,
                kDpbP16x16RefFetchMs720pAt20MHz);
    return 0;
}
