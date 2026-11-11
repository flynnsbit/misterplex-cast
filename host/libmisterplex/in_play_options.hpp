#pragma once
// FEATURE-GAP E5: in-play options on the Program menu.
//
// IN_PLAY_OPTIONS (misterplex.conf) / MPX_IN_PLAY_OPTIONS (env wins).
// Default off. Unset, empty, 0, off, false, and no stay off.
//
// Off: the Program menu matches the tip. There is no Options row, no
// picture-size line, and Play from start is not offered, so a play is
// not seeked to 0 from this path.
// On, while a title is playing: the menu gains Options. That page shows
// the open picture size (or "Picture size pending" before the first
// frame) and Play from start, which seeks to 0 with the existing seek.
// Idle and the library do not grow the row.

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {

inline bool inPlayOptionsEnvOn(const char* e) {
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

// Default off. MPX_IN_PLAY_OPTIONS wins over IN_PLAY_OPTIONS when it is set.
inline bool inPlayOptionsEnabled() {
#ifdef MUT_INPLAY_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_IN_PLAY_OPTIONS"); e && e[0])
        return inPlayOptionsEnvOn(e);
    return inPlayOptionsEnvOn(std::getenv("IN_PLAY_OPTIONS"));
#endif
}

// Flag off is no line, even when a size is known. Flag on with no size yet
// says the picture has not reported. Otherwise "WxH".
inline std::string inPlayInfoText(bool on, int w, int h) {
#ifdef MUT_INPLAY_DROP_INFO
    (void)on;
    (void)w;
    (void)h;
    return {};
#elif defined(MUT_INPLAY_INFO_WHEN_OFF)
    (void)on;
    if (w <= 0 || h <= 0)
        return "Picture size pending";
    return std::to_string(w) + "x" + std::to_string(h);
#else
    if (!on)
        return {};
    if (w <= 0 || h <= 0)
        return "Picture size pending";
    return std::to_string(w) + "x" + std::to_string(h);
#endif
}

// Play from start is a seek only when the flag is on and the row was chosen.
inline bool inPlayAcceptRestart(bool on, bool chose) {
#ifdef MUT_INPLAY_SEEK_WHEN_OFF
    (void)on;
    return chose;
#elif defined(MUT_INPLAY_IGNORE_START)
    (void)on;
    (void)chose;
    return false;
#else
    return on && chose;
#endif
}

} // namespace misterplex
