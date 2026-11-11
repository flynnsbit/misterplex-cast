#pragma once
// misterplex.conf MARKERS and the Skip Intro / Up Next knobs. Env MPX_* wins.
// MARKERS defaults off until glass.

#include "marker_tracker.hpp"

#include <cstdlib>
#include <functional>
#include <string>

namespace misterplex::markers {

struct Flags {
    bool markers = false;
    Mode skipIntro = Mode::Button;
    Mode skipCredits = Mode::Button;
    bool upNext = true;
    int countdownS = 10;
    int fallbackS = 20;
    int passoutMin = 120;
};

struct FlagSources {
    std::function<std::string(const char* key)> conf;
    std::function<const char*(const char* key)> env;
};

inline Mode parseSkipMode(const std::string& v, Mode fallback) {
    if (v.empty())
        return fallback;
    if (v == "off" || v == "0" || v == "disabled" || v == "false")
        return Mode::Off;
    if (v == "auto" || v == "automatically")
        return Mode::Auto;
    if (v == "button" || v == "manual" || v == "manually" || v == "on" || v == "1" || v == "true")
        return Mode::Button;
    return fallback;
}

inline int parseNonNeg(const std::string& v, int fallback) {
    if (v.empty())
        return fallback;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str())
        return fallback;
    if (n < 0)
        return 0;
    if (n > 86400)
        return 86400;
    return static_cast<int>(n);
}

inline bool parseBoolOff(const std::string& v, bool fallback) {
    if (v.empty())
        return fallback;
    if (v == "0" || v == "off" || v == "false" || v == "no")
        return false;
    if (v == "1" || v == "on" || v == "true" || v == "yes")
        return true;
    return fallback;
}

inline std::string pick(const FlagSources& src, const char* confKey, const char* envKey) {
    if (src.env) {
        if (const char* e = src.env(envKey)) {
            if (e[0])
                return e;
        }
    }
    if (src.conf)
        return src.conf(confKey);
    return {};
}

inline Flags loadFlags(const FlagSources& src) {
    Flags f;
    const std::string master = pick(src, "MARKERS", "MPX_MARKERS");
#ifdef MUT_MARKERS_FLAG_IGNORED
    f.markers = true;
    (void)master;
#else
    f.markers = parseBoolOff(master, false);
#endif
    f.skipIntro = parseSkipMode(pick(src, "SKIP_INTRO", "MPX_SKIP_INTRO"), Mode::Button);
    f.skipCredits = parseSkipMode(pick(src, "SKIP_CREDITS", "MPX_SKIP_CREDITS"), Mode::Button);
    f.upNext = parseBoolOff(pick(src, "UPNEXT", "MPX_UPNEXT"), true);
    f.countdownS = parseNonNeg(pick(src, "UPNEXT_COUNTDOWN_S", "MPX_UPNEXT_COUNTDOWN_S"), 10);
    f.fallbackS = parseNonNeg(pick(src, "UPNEXT_FALLBACK_S", "MPX_UPNEXT_FALLBACK_S"), 20);
    f.passoutMin = parseNonNeg(pick(src, "UPNEXT_PASSOUT_MIN", "MPX_UPNEXT_PASSOUT_MIN"), 120);
    return f;
}

// MARKERS=0 forces the tracker off even when the other knobs stay at their defaults.
inline TrackerConfig toConfig(const Flags& f, bool autoNext) {
    TrackerConfig c;
    c.intro = f.markers ? f.skipIntro : Mode::Off;
    c.credits = f.markers ? f.skipCredits : Mode::Off;
    c.upNext = f.markers && f.upNext;
    c.autoNext = autoNext;
    c.countdownMs = static_cast<int64_t>(f.countdownS) * 1000;
    c.fallbackMs = static_cast<int64_t>(f.fallbackS) * 1000;
    c.passoutMs = static_cast<int64_t>(f.passoutMin) * 60 * 1000;
    return c;
}

inline const char* modeName(Mode m) {
    switch (m) {
    case Mode::Off: return "off";
    case Mode::Button: return "button";
    case Mode::Auto: return "auto";
    }
    return "off";
}

} // namespace misterplex::markers
