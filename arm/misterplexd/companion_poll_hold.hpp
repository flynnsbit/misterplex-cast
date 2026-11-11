#pragma once
// /player/timeline/poll?wait=1 long-hold (playbar no-proxy, commit b).
//
// The PMS relays Plex Web's poll to the player and waits for the reply. Open-source companions
// (plex-mpv-shim 30 s, PHT 10 s, PKC 20 s) hold the reply for a new timeline; the daemon slept a
// fixed 400 ms. The companion accept loop is SINGLE-THREADED, so a hold also stalls every other
// request: the hold is bounded (default 1000 ms), ends the moment the timeline changes or another
// connection is waiting, and MPX_COMPANION_POLL_HOLD_MS tunes it (0 = reply now; max 30000).
#include <cstdlib>
#include <string>

namespace misterplex {

constexpr int kPollHoldDefaultMs = 1000;
constexpr int kPollHoldMaxMs = 30000;
constexpr int kPollHoldSliceMs = 25;

// Only wait=1 holds. envVal is getenv("MPX_COMPANION_POLL_HOLD_MS").
inline int pollHoldMs(const std::string& waitParam, const char* envVal) {
    if (waitParam != "1")
        return 0;
    if (envVal && envVal[0]) {
        char* end = nullptr;
        const long v = std::strtol(envVal, &end, 10);
        if (end && *end == '\0' && v >= 0 && v <= kPollHoldMaxMs)
            return static_cast<int>(v);
    }
    return kPollHoldDefaultMs;
}

// Waits up to holdMs in slices; true when released early by changed(). Clock is injected for tests.
template <class Changed, class NowMs, class SleepMs>
bool holdPoll(int holdMs, Changed changed, NowMs nowMs, SleepMs sleepMs) {
    if (holdMs <= 0)
        return false;
    const long long start = nowMs();
    while (true) {
        if (changed())
            return true;
        const long long left = holdMs - (nowMs() - start);
        if (left <= 0)
            return false;
        sleepMs(left < kPollHoldSliceMs ? static_cast<int>(left) : kPollHoldSliceMs);
    }
}

} // namespace misterplex
