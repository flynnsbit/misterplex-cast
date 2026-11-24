// Host / product-flow gate for GBE residual Risk #2 on tip 80a265c8:
//   After escape, silentBumperActive_=false and holdPts cleared; the next
//   in-band silent AU re-arms → ~1.5 s drop / short fill repeating under
//   continuous silence. Each cycle yields a ≥0.2 s HDMI hole (GATE-V2 slack)
//   that can trip DECODER_ONLY even though the 261 s stuck-bumper class is
//   escaped. Suggested harden: latch escape until a non-silent AU.
//
// Suite-red on 80a265c8 product (no silentBumperEscapedLatch_). Mutant
// MUT_LIVE_ACCEPT_SILENT_BUMPER_REARM must FAIL. Tip d81e4826 (latch) should
// turn this gate green when product source is overlaid.
#pragma once

#include "libmisterplex/live_disc_rebase.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace misterplex::gate1066_v2_rearm {

inline constexpr int64_t kRearmCycleHoleDurTicks90k =
    misterplex::live_disc::kSilentBumperEscapeTicks90k;  // 1.5 s
inline constexpr int64_t kAuStepTicks90k = 21 * 90;       // ~21 ms AU cadence
inline constexpr int64_t kSimHorizonTicks90k = 4500 * 90; // 4.5 s continuous silence

struct HdmiHole {
    int64_t startTicks = 0;
    int64_t durTicks = 0;
};

struct ProductBumperState {
    bool active = false;
    bool escapedLatch = false;
    int64_t holdPts = misterplex::live_disc::kNoPts90k;
    bool hasEnergetic = true;  // primed (lastEnergeticPcm_ non-empty)
    int64_t lastEmitPts = misterplex::live_disc::kNoPts90k;
    int escapeCount = 0;
    int rearmCount = 0;
};

// Product-faithful takeAudio silent-AU path (owenb_av_source.cpp on 80a / d81e).
// latchEscape=true mirrors d81e silentBumperEscapedLatch_ harden.
inline bool takeSilentAu(ProductBumperState& s, int64_t pts, bool latchEscape) {
    using namespace misterplex::live_disc;
    if (latchEscape && s.escapedLatch) {
        // Latched: keep substituting energetic; do not re-arm / hold.
        s.lastEmitPts = pts;
        return true;  // emit energetic substitute
    }
    const bool wasActive = s.active;
    s.active = true;
    if (s.holdPts == kNoPts90k)
        s.holdPts = pts;
    if (!wasActive && s.escapeCount > 0)
        ++s.rearmCount;  // re-armed after a prior escape
    const int64_t held = (s.holdPts != kNoPts90k) ? (pts - s.holdPts) : 0;
    if (silentBumperHoldExpired(held) && energeticPcmFillRepeatsAcrossStarve() &&
        s.hasEnergetic) {
        s.active = false;
        s.holdPts = kNoPts90k;
        if (latchEscape)
            s.escapedLatch = true;
        ++s.escapeCount;
        s.lastEmitPts = pts;
        return true;  // emit energetic substitute
    }
    return false;  // drop silent AU
}

// Product-faithful takeVideo hold while bumper active (pre-escape).
inline bool takeVideoWhileBumper(ProductBumperState& s, int64_t pts, bool latchEscape) {
    using namespace misterplex::live_disc;
    if (!(s.active) )
        return true;  // emit video
    if (latchEscape && s.escapedLatch)
        return true;  // d81e: do not re-hold while latched
    const int64_t held = (s.holdPts != kNoPts90k) ? (pts - s.holdPts) : 0;
    if (!silentBumperHoldExpired(held))
        return false;  // drop video (continue)
    s.active = false;
    s.holdPts = kNoPts90k;
    if (latchEscape)
        s.escapedLatch = true;
    ++s.escapeCount;
    return true;
}

// Continuous silence after energetic prime: walk AU+V at ~21 ms steps.
// Only post-escape holes count (the first 1.5 s title-change hold is intentional;
// GBE Risk #2 is the re-arm cycle after escape clears holdPts).
inline std::vector<HdmiHole> simulateProductRearmFlow(bool latchEscape) {
    using namespace misterplex::live_disc;
    ProductBumperState s;
    s.hasEnergetic = true;
    s.lastEmitPts = 0;
    std::vector<HdmiHole> holes;
    int64_t holeStart = kNoPts90k;
    int escapesAtHoleOpen = 0;
    auto closeHole = [&](int64_t now) {
        if (holeStart != kNoPts90k && now > holeStart) {
            // Keep only holes that began after at least one escape (re-arm class).
            if (escapesAtHoleOpen >= 1) {
                HdmiHole h;
                h.startTicks = holeStart;
                h.durTicks = now - holeStart;
                holes.push_back(h);
            }
            holeStart = kNoPts90k;
        }
    };
    for (int64_t t = kAuStepTicks90k; t <= kSimHorizonTicks90k; t += kAuStepTicks90k) {
        const int escapesBefore = s.escapeCount;
        const bool audioEmitted = takeSilentAu(s, t, latchEscape);
        const bool videoEmitted = takeVideoWhileBumper(s, t, latchEscape);
        const bool gap = !audioEmitted || !videoEmitted;
        if (gap) {
            if (holeStart == kNoPts90k) {
                holeStart = t;
                // If this step just escaped, the drop is pre-escape hold ending;
                // attribute open to escapesBefore so post-escape re-arm is distinct.
                escapesAtHoleOpen = escapesBefore;
            }
        } else {
            closeHole(t);
        }
    }
    closeHole(kSimHorizonTicks90k + kAuStepTicks90k);
    (void)s.rearmCount;
    return holes;
}

inline bool hasSoakFailHole(const std::vector<HdmiHole>& holes) {
    using namespace misterplex::live_disc;
    for (const auto& h : holes) {
        if (h.durTicks + 1e-9 >= kSoakGapGateTicks90k)
            return true;
    }
    return false;
}

// True when product owenb source latches escape (d81e+). Suite-red false on 80a.
inline bool productEscapeLatchesUntilEnergetic(const std::string& owenbAvSource) {
#ifdef MUT_LIVE_ACCEPT_SILENT_BUMPER_REARM
    (void)owenbAvSource;
    // Bad path: claim latch present / re-arm acceptable.
    return true;
#else
    return owenbAvSource.find("silentBumperEscapedLatch_") != std::string::npos &&
           owenbAvSource.find("silentBumperEscapeLatchesUntilEnergetic") !=
               std::string::npos;
#endif
}

// GATE: PASS only when product latches AND product-flow leaves no ≥0.2 s hole.
inline bool rearmGatePasses(const std::string& owenbAvSource) {
#ifdef MUT_LIVE_ACCEPT_SILENT_BUMPER_REARM
    (void)owenbAvSource;
    // Mutant accepts the re-arm cycle as PASS — suite must FAIL.
    return true;
#else
    const bool latch = productEscapeLatchesUntilEnergetic(owenbAvSource);
    const auto holes = simulateProductRearmFlow(latch);
    return latch && !hasSoakFailHole(holes);
#endif
}

}  // namespace misterplex::gate1066_v2_rearm
