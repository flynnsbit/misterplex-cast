#pragma once
// GH#193 / #235: short generated pad on plexui Home while nothing is playing.
//
// THEME_MUSIC (misterplex.conf) / MPX_THEME_MUSIC (env wins). Default off.
// Unset, empty, 0, off, false, and no stay off. On: one 0.5 s stereo pad via
// pushOneShotPcm when Home is visible. Show Detail GETs theme bytes with the
// token as a header (never in the URL) and decodes in theme_decode.hpp.

#include "libmisterplex/mraudio_oneshot.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {

inline constexpr int kThemeHomeFrames = 24000; // 0.5 s @ 48 kHz

inline bool themeEnvOn(const char* e) {
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

inline bool themeMusicEnabled() {
#ifdef MUT_THEME_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_THEME_MUSIC"); e && e[0])
        return themeEnvOn(e);
    return themeEnvOn(std::getenv("THEME_MUSIC"));
#endif
}

inline bool themeShouldPlay(bool enabled, bool plexVisible, bool home, bool playing) {
#ifdef MUT_THEME_ALWAYS
    (void)enabled;
    (void)plexVisible;
    (void)home;
    (void)playing;
    return true;
#else
    return enabled && plexVisible && home && !playing;
#endif
}

inline std::vector<uint8_t> themeHomePcm() {
#ifdef MUT_THEME_SILENT
    return std::vector<uint8_t>(static_cast<size_t>(kThemeHomeFrames) * 4, 0);
#else
    std::vector<uint8_t> out(static_cast<size_t>(kThemeHomeFrames) * 4);
    for (int i = 0; i < kThemeHomeFrames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kThemeHomeFrames);
        const float env = t < 0.15f ? t / 0.15f : (t > 0.85f ? (1.f - t) / 0.15f : 1.f);
        const float s =
            env * 0.08f *
            std::sin(2.f * 3.14159265f * 220.f * static_cast<float>(i) / 48000.f);
        const auto v = static_cast<int16_t>(s * 32767.f);
        const auto lo = static_cast<uint8_t>(v & 0xff);
        const auto hi = static_cast<uint8_t>((v >> 8) & 0xff);
        const size_t o = static_cast<size_t>(i) * 4;
        out[o] = lo;
        out[o + 1] = hi;
        out[o + 2] = lo;
        out[o + 3] = hi;
    }
    return out;
#endif
}

inline bool themeRatingKeyOk(const std::string& rk) {
    if (rk.empty() || rk.size() > 32)
        return false;
    for (unsigned char c : rk) {
        if (!std::isdigit(c))
            return false;
    }
    return true;
}

// Library path only. A theme attribute that already carries a token is refused.
inline std::string themeLibraryPath(const std::string& ratingKey, const std::string& themeAttr) {
    if (themeAttr.find("X-Plex-Token") != std::string::npos)
        return {};
    std::string p = themeAttr;
    const auto q = p.find('?');
    if (q != std::string::npos)
        p.resize(q);
    if (p.rfind("/library/metadata/", 0) == 0)
        return p;
    if (!themeRatingKeyOk(ratingKey))
        return {};
    return "/library/metadata/" + ratingKey + "/theme";
}

inline std::string themeFetchUrl(const std::string& baseUrl, const std::string& path) {
#ifdef MUT_THEME_TOKEN_IN_URL
    return baseUrl + path + "?X-Plex-Token=secret";
#else
    if (baseUrl.empty() || path.empty() || path[0] != '/' || path.find("X-Plex-Token") != std::string::npos)
        return {};
    std::string b = baseUrl;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    return b + path;
#endif
}

inline std::vector<std::pair<std::string, std::string>> themeFetchHeaders(const std::string& token) {
    std::vector<std::pair<std::string, std::string>> h;
    if (!token.empty())
        h.emplace_back("X-Plex-Token", token);
    return h;
}

struct ThemeShowFetch {
    bool fetch = false;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Show Detail only. Token is a header. Empty plan if the URL would carry it.
inline ThemeShowFetch themeShowFetchPlan(bool enabled, bool plexVisible, bool detail, bool isShow,
                                         bool playing, const std::string& baseUrl,
                                         const std::string& ratingKey, const std::string& themeAttr,
                                         const std::string& token) {
#ifdef MUT_THEME_SHOW_SKIP
    (void)enabled;
    (void)plexVisible;
    (void)detail;
    (void)isShow;
    (void)playing;
    (void)baseUrl;
    (void)ratingKey;
    (void)themeAttr;
    (void)token;
    return {};
#else
    ThemeShowFetch p;
    if (!enabled || !plexVisible || !detail || !isShow || playing)
        return p;
    const std::string path = themeLibraryPath(ratingKey, themeAttr);
    p.url = themeFetchUrl(baseUrl, path);
    if (p.url.empty())
        return {};
#ifdef MUT_THEME_SHOW_TOKEN_QUERY
    if (!token.empty())
        p.url += std::string(p.url.find('?') == std::string::npos ? "?" : "&") + "X-Plex-Token=" + token;
#else
    p.headers = themeFetchHeaders(token);
#endif
    p.fetch = true;
    return p;
#endif
}

} // namespace misterplex
