#pragma once
// Gate 8 boot: the Ready line includes YouTube only after Lounge has connected.
// A connect that fails because DNS is not up yet waits and tries again. The
// wait grows and then holds; it does not stop.

namespace misterplex {
namespace gcast {

inline bool loungeAdvertisesYouTube(bool loungeConnected) {
    return loungeConnected;
}

// failures is how many connect attempts have already failed.
// 0 -> 1s, then 2, 4, 8, 16, then 30s forever.
inline int loungeConnectBackoffMs(int failures) {
    if (failures < 0)
        failures = 0;
    int ms = 1000;
    for (int i = 0; i < failures && ms < 30000; ++i) {
        ms *= 2;
        if (ms > 30000)
            ms = 30000;
    }
    return ms;
}

}  // namespace gcast
}  // namespace misterplex
