// Host model for GATE-1066-V2 glass REJECT of tip 684967fa:
//   21 DECODER_ONLY holes; longest 353.589–615.000 s (261.411 s) silent State Farm
//   bumper while host-HLS stayed loud. Energetic PCM fill logged once (bytes=4096)
//   and did NOT cover that stall. Zaps 6/6; muxed 1062 OK; host-ref present.
//
// Prior tip 684967fa already holds energetic PCM for the short GATE-V2 r2 cluster
// class (simulateGateV2DecoderOnlyHoles). This model gates the *long* stuck-bumper
// class (≥60 s DECODER_ONLY while host has audio) that energetic-fill-once missed.
//
// Suite-red on 684967fa / 6a5fbcc4: recoversStuckDecoderOnlyBumper() defaults false.
// Mutant MUT_LIVE_ACCEPT_LONG_DECODER_ONLY_STALL must FAIL.
#pragma once

#include "libmisterplex/live_disc_rebase.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace misterplex::gate1066_v2_stall {

inline constexpr double kLongDecoderOnlyFailSec = 60.0;
inline constexpr double kGlass684StallStartSec = 353.589;
inline constexpr double kGlass684StallEndSec = 615.000;
inline constexpr double kGlass684StallDurSec = 261.411;
inline constexpr int kGlass684EnergeticFillCount = 1;
inline constexpr int kGlass684EnergeticFillBytes = 4096;

struct DecoderOnlyHole {
    double startSec = 0;
    double endSec = 0;
    double durSec = 0;
    bool hostHasAudio = true;
    int energeticFillCount = 0;
    int energeticFillBytes = 0;
};

// Product tip that clears the 261 s stuck-bumper class must recover / re-tune /
// drop the silent bumper so no DECODER_ONLY hole ≥60 s remains while host loud.
inline bool recoversStuckDecoderOnlyBumper() {
#ifdef MUT_LIVE_ACCEPT_LONG_DECODER_ONLY_STALL
    // Bad path: claim recover while longStallGatePasses also falsely PASSes.
    return true;
#else
    // Wired to product predicates (GBE 80a265c8 review): escape + repeating
    // energetic fill + latch-until-energetic (closes 1.5 s re-arm cycle).
    return misterplex::live_disc::escapeSilentBumperAfterSoakGate() &&
           misterplex::live_disc::energeticPcmFillRepeatsAcrossStarve() &&
           misterplex::live_disc::silentBumperEscapeLatchesUntilEnergetic();
#endif
}

// Glass signature: one long DECODER_ONLY hole despite energetic fill once.
inline DecoderOnlyHole simulateGlass684967faLongStall(bool recoverStuck) {
    DecoderOnlyHole h;
    h.hostHasAudio = true;
    h.energeticFillCount = kGlass684EnergeticFillCount;
    h.energeticFillBytes = kGlass684EnergeticFillBytes;
    if (recoverStuck) {
        h.startSec = 0;
        h.endSec = 0;
        h.durSec = 0;
        return h;
    }
    h.startSec = kGlass684StallStartSec;
    h.endSec = kGlass684StallEndSec;
    h.durSec = kGlass684StallDurSec;
    return h;
}

// Same class on 6a5fbcc4 r2 (65 DECODER_ONLY; host loud). Prefer still RED.
inline DecoderOnlyHole simulateGlass6a5fbcc4LongClass(bool recoverStuck) {
    DecoderOnlyHole h;
    h.hostHasAudio = true;
    h.energeticFillCount = 0;
    h.energeticFillBytes = 0;
    if (recoverStuck) {
        return h;
    }
    // Representative long hole from r2 (not the full 65 count).
    h.startSec = 40.0;
    h.endSec = 120.0;
    h.durSec = 80.0;
    return h;
}

inline bool isLongDecoderOnlyFail(const DecoderOnlyHole& h) {
    return h.hostHasAudio && h.durSec + 1e-9 >= kLongDecoderOnlyFailSec;
}

// GATE-V2 long-stall gate: FAIL when any DECODER_ONLY ≥60 s while host has audio.
inline bool longStallGatePasses(bool recoverStuck) {
#ifdef MUT_LIVE_ACCEPT_LONG_DECODER_ONLY_STALL
    (void)recoverStuck;
    // Mutant accepts the glass long stall as PASS — suite must FAIL.
    return true;
#else
    const auto h684 = simulateGlass684967faLongStall(recoverStuck);
    const auto h6a = simulateGlass6a5fbcc4LongClass(recoverStuck);
    if (isLongDecoderOnlyFail(h684))
        return false;
    if (isLongDecoderOnlyFail(h6a))
        return false;
    return true;
#endif
}

}  // namespace misterplex::gate1066_v2_stall
