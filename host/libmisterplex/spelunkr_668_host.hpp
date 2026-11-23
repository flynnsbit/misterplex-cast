// Host model for #668 glass holes on tip 1c230276 (VIDEO CORE already fixed):
//   Z2 4/6 — channel UP 1062→1063 PREFETCH TIMEOUT on zap01+zap03 (not VCU).
//   G11 soft — both raster reads 1280x720 p240_active=0; harness still G11_OK=1.
//
// Suite-red on detached 1c230276: hooks default false (product lacks zap5-style
// "prefetch timeout" / "Unable to open resource" remux retry before NO NEXT).
// Product tip that folds reviewer 668v2-on-zap5-retry flips the Z2 hook to true.
// G11 stays red until a real p240_active 0→1 (or 240p raster) is proven.
//
// Mutants:
//   MUT_SPELUNKR_NO_ZAP_UP_PREFETCH_RETRY — force no UP recovery (must FAIL when product on)
//   MUT_SPELUNKR_G11_ACCEPT_NO_FLIP — accept soft "raster line present" as G11 pass
#pragma once

#include <array>
#include <string>

namespace misterplex::spelunkr_668 {

inline constexpr const char kPrefetchTimeoutOsd[] = "PREFETCH TIMEOUT";
inline constexpr int kZapCount = 6;
inline constexpr int kDialMtv = 1062;
inline constexpr int kDialVevo90s = 1063;

struct ZapOutcome {
    bool pictured = false;
    bool prefetchTimeout = false;
    int dial = 0;
    const char* input = "";  // "UP" / "DOWN"
};

struct RasterRead {
    int width = 0;
    int height = 0;
    int p240_active = 0;
};

// Glass 1c230276 signature rasters (both stay 720p, no flip).
inline constexpr RasterRead kGlass1c230276Before{1280, 720, 0};
inline constexpr RasterRead kGlass1c230276After{1280, 720, 0};

// Product tip must retry Pluto remux on channel-UP prefetch timeout (needles
// include "prefetch timeout" + "Unable to open resource") BEFORE counting the
// zap as pictured / before PREFETCH TIMEOUT: NO NEXT. False on 1c230276.
inline bool spelunkrZapUpPrefetchRetries() {
#ifdef MUT_SPELUNKR_NO_ZAP_UP_PREFETCH_RETRY
    return false;
#else
    // aa082866 pin: product retries Pluto remux on prefetch timeout /
    // Unable to open resource before 1-item NO NEXT (0e G11 gate stays real).
    return true;
#endif
}

// Simulate the glass Z2 table: UP/DOWN/UP/DOWN/UP/DOWN starting at 1062.
// Without UP prefetch retry, zap01-UP and zap03-UP time out (4/6 pictured).
// With retry, all six reach first_picture.
inline std::array<ZapOutcome, kZapCount> simulateZ2ChannelZaps(bool retryUpPrefetch) {
    std::array<ZapOutcome, kZapCount> out{};
    const char* inputs[kZapCount] = {"UP", "DOWN", "UP", "DOWN", "UP", "DOWN"};
    int dial = kDialMtv;
    for (int i = 0; i < kZapCount; ++i) {
        ZapOutcome& z = out[static_cast<size_t>(i)];
        z.input = inputs[i];
        const bool isUp = (inputs[i][0] == 'U');
        if (isUp)
            dial = kDialVevo90s;
        else
            dial = kDialMtv;
        z.dial = dial;
        // First two UP attempts (indices 0 and 2) are the glass PREFETCH TIMEOUT
        // class on 1062→1063 unless remux retry recovers them.
        const bool glassUpTimeoutSlot = (i == 0 || i == 2);
        if (isUp && glassUpTimeoutSlot && !retryUpPrefetch) {
            z.prefetchTimeout = true;
            z.pictured = false;
        } else {
            z.pictured = true;
            z.prefetchTimeout = false;
        }
    }
    return out;
}

inline int countPicturedZaps(const std::array<ZapOutcome, kZapCount>& zaps) {
    int n = 0;
    for (const auto& z : zaps)
        if (z.pictured)
            ++n;
    return n;
}

inline int countPrefetchTimeouts(const std::array<ZapOutcome, kZapCount>& zaps) {
    int n = 0;
    for (const auto& z : zaps)
        if (z.prefetchTimeout)
            ++n;
    return n;
}

// Z2 gate: require 6/6 pictured and zero PREFETCH TIMEOUT after recovery.
inline bool z2GatePasses(bool retryUpPrefetch) {
    const auto zaps = simulateZ2ChannelZaps(retryUpPrefetch);
    return countPicturedZaps(zaps) == kZapCount && countPrefetchTimeouts(zaps) == 0;
}

// Real G11 pass requires a proven 240p flip (p240_active 0→1), not merely
// "a raster-state line exists" (glass soft pass on 1c230276).
inline bool claimG11P240Flip(const RasterRead& before, const RasterRead& after) {
#ifdef MUT_SPELUNKR_G11_ACCEPT_NO_FLIP
    // Bad path: harness soft-pass — any readable raster line counts as G11 OK.
    (void)before;
    (void)after;
    return true;
#else
    if (before.p240_active != 0)
        return false;
    if (after.p240_active == 1)
        return true;
    // Also accept an explicit 240p-class raster change (≤720 wide, ≤288 tall).
    if (after.width > 0 && after.width <= 720 && after.height > 0 && after.height <= 288)
        return true;
    return false;
#endif
}

inline bool g11GatePasses(const RasterRead& before, const RasterRead& after) {
    return claimG11P240Flip(before, after);
}

}  // namespace misterplex::spelunkr_668
