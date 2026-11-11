#pragma once
#include "airplay/ns.hpp"
// First-byte budget of a helper /yt.ts open (fr-yt110-1025).
//
// The helper (tools/gcast/gcast_xcode.py) answers /yt.ts by running `yt-dlp -o - | ffmpeg -c copy`, so the
// first TS byte arrives only after a full yt-dlp extraction (the daemon's own notes: 19-22 s on the ARM, and
// the helper allows 90 s per resolve attempt). libav's http rw_timeout used to be a flat 20 s for every open,
// so a cold /yt.ts open ended in "open ...: Connection timed out" (a SourceError, not retried) although the
// helper was healthy. Only the helper /yt.ts URL gets the longer budget; every other source keeps 20 s.
#include <cstdint>
#include <string>

namespace misterplex {
namespace airplay {

inline constexpr int64_t kOpenRwTimeoutUs = 20000000;
inline constexpr int64_t kYtTsOpenRwTimeoutUs = 75000000;

inline bool isHelperYtTsUrl(const std::string& url) {
    return url.find("/yt.ts") != std::string::npos;
}

inline int64_t openRwTimeoutUs(const std::string& url) {
#if defined(MUT_YB_FLAT)
    (void)url;
    return kOpenRwTimeoutUs;
#elif defined(MUT_YB_ALL)
    (void)url;
    return kYtTsOpenRwTimeoutUs;
#elif defined(MUT_YB_SHORT)
    return isHelperYtTsUrl(url) ? 30000000 : kOpenRwTimeoutUs;
#else
    return isHelperYtTsUrl(url) ? kYtTsOpenRwTimeoutUs : kOpenRwTimeoutUs;
#endif
}

} // namespace airplay
} // namespace misterplex
