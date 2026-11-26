#pragma once
// PLUTO_LIVE (misterplex.conf) / MPX_PLUTO_LIVE (env wins). Default off.
// On: Back When kind=pluto channels resolve to Pluto stitcher HLS (public boot API).
// Off: byte-identical to the pre-tip UNSUPPORTED Pluto placeholder path.

#include <cstdlib>
#include <cstring>

namespace misterplex {

inline bool plutoLiveEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n) {
        unsigned char c = static_cast<unsigned char>(e[n]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        buf[n] = static_cast<char>(c);
    }
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

inline bool plutoLiveEnabled() {
#ifdef MUT_PLUTO_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_PLUTO_LIVE"); e && e[0])
        return plutoLiveEnvOn(e);
    return plutoLiveEnvOn(std::getenv("PLUTO_LIVE"));
#endif
}

}  // namespace misterplex