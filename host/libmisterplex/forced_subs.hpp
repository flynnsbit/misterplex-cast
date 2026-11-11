#pragma once
// FORCED_SUBS (misterplex.conf) / MPX_FORCED_SUBS (env wins). Default off.
//
// Off or unset: play and re-resolve match this tip. A PMS Stream forced="1"
// is still parsed. It is not selected unless the user picks it, a saved show
// preference asks for it, or SUBTITLE_FORCED already filtered a language match.
//
// On (1/on/yes/true): when that pick left subtitles off, and the user did not
// turn them off, the forced track is selected. Soft text uses the overlay
// already on this tip (sidecar SRT/ASS, and packet text when SOFT_SUBS is on).
// Image tracks (PGS, VobSub) use the existing PMS burn path. No PGS OCR and
// no new overlay. An explicit subtitleStreamID, including 0, still wins.
// A stored show preference that turns subtitles off, or that names a
// subtitle language, still wins.

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace misterplex {

inline bool forcedSubsEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// Default off. MPX_FORCED_SUBS wins over FORCED_SUBS when it is set. Unset is off.
inline bool forcedSubsEnabled() {
#ifdef MUT_FORCED_SUBS_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_FORCED_SUBS"); e && e[0])
        return forcedSubsEnvOn(e);
    return forcedSubsEnvOn(std::getenv("FORCED_SUBS"));
#endif
}

} // namespace misterplex
