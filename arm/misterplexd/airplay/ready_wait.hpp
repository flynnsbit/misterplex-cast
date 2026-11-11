#pragma once
#include "airplay/ns.hpp"
// KI-5: after a hook restart the on-card helper is not listening yet. The first
// Ready line used to be published immediately, so it read "Ready: Plex | YouTube"
// and the card still lacked AirPlay a few seconds later. Wait for the helper
// /probe self-test, bounded, then publish whatever the probe says.

namespace misterplex {
namespace airplay {

constexpr int kAirplayReadyWaitMs = 10000;
constexpr int kAirplayReadyPollMs = 250;

// 0: publish the Ready line now (probe passed, or the bound ran out).
// Otherwise: milliseconds to sleep before probing again.
inline int airplayFirstReadyDelayMs(bool probeOk, int elapsedMs) {
    if (probeOk || elapsedMs < 0 || elapsedMs >= kAirplayReadyWaitMs)
        return 0;
    const int left = kAirplayReadyWaitMs - elapsedMs;
    return left < kAirplayReadyPollMs ? left : kAirplayReadyPollMs;
}

}  // namespace airplay
}  // namespace misterplex
