#pragma once
// FEATURE-GAP E23: ambient / screensaver library art on the idle CRT.
//
// AMBIENT (misterplex.conf) / MPX_AMBIENT (env wins). Default off.
// Unset, empty, 0, off, false, and no stay off.
//
// Off: the OwenB idle field stays the black Ready card. IDLE_SCREEN
// (logo, black, screensaver, last) is unchanged. No library GET.
// On: while the OwenB idle card is up and nothing is playing, the daemon
// reads /library/recentlyAdded and paints those art (or thumb) frames on
// the idle field, one after another. The Ready card stays on top. A menu,
// the library page, an error, and PLAY-PENDING keep the existing field.
// Paths stored here are /library/... with no X-Plex-Token. The token is
// added only on the GET the daemon already uses for PMS, and it is not logged.

#include "library_browser.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace misterplex {

inline constexpr int64_t kAmbientDwellMs = 12000;
inline constexpr int64_t kAmbientRefreshMs = 10 * 60 * 1000;
inline constexpr int64_t kAmbientRetryMs = 30 * 1000;
inline constexpr int kAmbientMaxCards = 8;
inline constexpr int kAmbientArtW = 160;
inline constexpr int kAmbientArtH = 120;
inline constexpr std::size_t kAmbientByteCap = 512u * 1024u;

inline bool ambientEnvOn(const char* e) {
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

// Default off. MPX_AMBIENT wins over AMBIENT when it is set.
inline bool ambientEnabled() {
#ifdef MUT_AMBIENT_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_AMBIENT"); e && e[0])
        return ambientEnvOn(e);
    return ambientEnvOn(std::getenv("AMBIENT"));
#endif
}

inline std::string ambientListPath() {
    return "/library/recentlyAdded?X-Plex-Container-Start=0&X-Plex-Container-Size=8";
}

inline std::string ambientStripToken(std::string url) {
    const std::string key = "X-Plex-Token=";
    const auto hash = url.find('#');
    const auto end = hash == std::string::npos ? url.size() : hash;
    const auto q = url.find('?');
    if (q == std::string::npos || q > end)
        return url;
    const std::string frag = hash == std::string::npos ? std::string() : url.substr(hash);
    const std::string query = url.substr(q + 1, end - (q + 1));
    std::string kept;
    std::size_t i = 0;
    while (i < query.size()) {
        auto amp = query.find('&', i);
        if (amp == std::string::npos)
            amp = query.size();
        const std::string part = query.substr(i, amp - i);
        if (part.rfind(key, 0) != 0 && !part.empty()) {
            if (!kept.empty())
                kept.push_back('&');
            kept += part;
        }
        i = amp + 1;
    }
    if (kept.empty())
        return url.substr(0, q) + frag;
    return url.substr(0, q) + "?" + kept + frag;
}

// A PMS library path, or empty when it is not /library/... (no public host).
inline std::string ambientLibraryPath(std::string raw) {
#ifdef MUT_AMBIENT_DROP_ART
    (void)raw;
    return {};
#else
    if (raw.empty())
        return {};
#ifndef MUT_AMBIENT_TOKEN_LEAK
    raw = ambientStripToken(std::move(raw));
#endif
    const auto hash = raw.find('#');
    if (hash != std::string::npos)
        raw.resize(hash);
    const auto scheme = raw.find("://");
    if (scheme != std::string::npos) {
        const auto slash = raw.find('/', scheme + 3);
        if (slash == std::string::npos)
            return {};
        raw = raw.substr(slash);
    }
    const auto q = raw.find('?');
    const std::string path = q == std::string::npos ? raw : raw.substr(0, q);
    if (path.rfind("/library/", 0) != 0)
        return {};
    if (path.find("..") != std::string::npos)
        return {};
#ifdef MUT_AMBIENT_TOKEN_LEAK
    if (q != std::string::npos)
        return raw;
#endif
    return path;
#endif
}

inline std::string ambientSizedPath(const std::string& path) {
    const std::string clean = ambientLibraryPath(path);
    if (clean.empty())
        return {};
    if (clean.find('?') == std::string::npos)
        return clean + "?width=320&height=240";
    return clean + "&width=320&height=240";
}

struct AmbientArt {
    std::string ratingKey;
    std::string title;
    std::string path;
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> bgra;
    std::uint64_t fingerprint = 0;
    bool visible() const {
        return w > 0 && h > 0 && bgra.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    }
};

struct AmbientDeck {
    std::vector<AmbientArt> cards;
};

inline int ambientVisibleCount(const AmbientDeck& deck) {
    int n = 0;
    for (const AmbientArt& c : deck.cards)
        if (c.visible())
            ++n;
    return n;
}

inline const AmbientArt* ambientArtAt(const AmbientDeck& deck, int slot) {
    if (slot < 0)
        return nullptr;
    int seen = 0;
    for (const AmbientArt& c : deck.cards) {
        if (!c.visible())
            continue;
        if (seen == slot)
            return &c;
        ++seen;
    }
    return nullptr;
}

inline int ambientSlot(int64_t nowMs, int64_t epochMs, int count, int64_t dwellMs = kAmbientDwellMs) {
#ifdef MUT_AMBIENT_NO_CYCLE
    (void)nowMs;
    (void)epochMs;
    (void)dwellMs;
    return count > 0 ? 0 : -1;
#else
    if (count <= 0 || dwellMs <= 0)
        return -1;
    int64_t elapsed = nowMs - epochMs;
    if (elapsed < 0)
        elapsed = 0;
    return static_cast<int>((elapsed / dwellMs) % count);
#endif
}

inline int64_t ambientNextChange(int64_t nowMs, int64_t epochMs, int count, int64_t dwellMs = kAmbientDwellMs) {
#ifdef MUT_AMBIENT_NO_CYCLE
    (void)nowMs;
    (void)epochMs;
    (void)count;
    (void)dwellMs;
    return std::numeric_limits<int64_t>::max();
#else
    if (count <= 1 || dwellMs <= 0)
        return std::numeric_limits<int64_t>::max();
    int64_t elapsed = nowMs - epochMs;
    if (elapsed < 0)
        elapsed = 0;
    const int64_t into = elapsed % dwellMs;
    int64_t step = dwellMs - into;
    if (step <= 0)
        step = dwellMs;
    if (nowMs > std::numeric_limits<int64_t>::max() - step)
        return std::numeric_limits<int64_t>::max();
    return nowMs + step;
#endif
}

// True when the idle loop should start one library fetch. The caller passes
// ambientEnabled() as `enabled`. Playing, a fetch already in flight, or no
// PMS base and token stays false.
inline bool ambientShouldFetch(bool enabled, bool idle, bool playing, bool inFlight, bool haveServer,
                                int64_t nowMs, int64_t fetchedAtMs, int cards) {
    if (!enabled || !idle || playing || inFlight || !haveServer)
        return false;
    if (fetchedAtMs <= 0)
        return true;
    const int64_t wait = cards > 0 ? kAmbientRefreshMs : kAmbientRetryMs;
    return nowMs - fetchedAtMs >= wait;
}

inline std::uint64_t ambientFingerprint(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Letterbox RGBA (stbi, 4 channels) into the fixed art card. Black bars.
// The stored path is the library path with no token.
inline AmbientArt ambientPack(const std::string& ratingKey, const std::string& title, const std::string& path,
                              const std::uint8_t* rgba, int w, int h) {
    AmbientArt a;
    a.ratingKey = ratingKey;
    a.title = title;
    a.path = ambientLibraryPath(path);
    if (!rgba || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return a;
    a.w = kAmbientArtW;
    a.h = kAmbientArtH;
    a.bgra.assign(static_cast<std::size_t>(a.w) * static_cast<std::size_t>(a.h) * 4u, 0);
    int dw = kAmbientArtW;
    int dh = kAmbientArtH;
    if (static_cast<long long>(w) * kAmbientArtH >= static_cast<long long>(h) * kAmbientArtW) {
        dw = kAmbientArtW;
        dh = std::max(1, static_cast<int>((static_cast<long long>(h) * kAmbientArtW) / w));
    } else {
        dh = kAmbientArtH;
        dw = std::max(1, static_cast<int>((static_cast<long long>(w) * kAmbientArtH) / h));
    }
    if (dw > kAmbientArtW)
        dw = kAmbientArtW;
    if (dh > kAmbientArtH)
        dh = kAmbientArtH;
    const int ox = (kAmbientArtW - dw) / 2;
    const int oy = (kAmbientArtH - dh) / 2;
    for (int dy = 0; dy < dh; ++dy) {
        const int iy = std::min(h - 1, dy * h / dh);
        for (int dx = 0; dx < dw; ++dx) {
            const int ix = std::min(w - 1, dx * w / dw);
            const std::uint8_t* s = rgba + (static_cast<std::size_t>(iy) * static_cast<std::size_t>(w) +
                                            static_cast<std::size_t>(ix)) *
                                               4u;
            std::uint8_t* d = a.bgra.data() + (static_cast<std::size_t>(oy + dy) * static_cast<std::size_t>(a.w) +
                                               static_cast<std::size_t>(ox + dx)) *
                                                  4u;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3] ? s[3] : 255;
        }
    }
    a.fingerprint = ambientFingerprint(a.bgra.data(), a.bgra.size());
    return a;
}

inline std::vector<AmbientArt> parseAmbientCards(const std::string& xml) {
    std::vector<AmbientArt> out;
    const char* opens[] = {"<Video", "<Directory", "<Photo", "<Track"};
    for (const char* open : opens) {
        const std::size_t olen = std::strlen(open);
        std::size_t i = 0;
        while (i < xml.size() && static_cast<int>(out.size()) < kAmbientMaxCards) {
            const auto p = xml.find(open, i);
            if (p == std::string::npos)
                break;
            if (p + olen < xml.size() && !xmlSpace(xml[p + olen]) && xml[p + olen] != '/' && xml[p + olen] != '>') {
                i = p + olen;
                continue;
            }
            const auto e = xmlTagEnd(xml, p);
            if (e == std::string::npos)
                break;
            const std::string tag = xml.substr(p, e - p + 1);
            std::string raw;
            for (const char* attr : {"art", "grandparentArt", "thumb", "grandparentThumb"}) {
                raw = xmlAttr(tag, attr);
                if (!raw.empty())
                    break;
            }
            AmbientArt card;
            card.ratingKey = xmlAttr(tag, "ratingKey");
            card.title = xmlAttr(tag, "title");
            card.path = ambientLibraryPath(raw);
            if (!card.path.empty())
                out.push_back(std::move(card));
            i = e + 1;
        }
    }
    return out;
}

// One field pixel. BGRA. MUT_AMBIENT_NO_PAINT keeps the black idle field.
inline void ambientSampleBgra(const AmbientArt& art, int x, int y, int fieldW, int fieldH, std::uint8_t& b,
                              std::uint8_t& g, std::uint8_t& r, std::uint8_t& a) {
#ifdef MUT_AMBIENT_NO_PAINT
    (void)art;
    (void)x;
    (void)y;
    (void)fieldW;
    (void)fieldH;
    b = g = r = 0;
    a = 255;
    return;
#else
    b = g = r = 0;
    a = 255;
    if (!art.visible() || fieldW <= 0 || fieldH <= 0)
        return;
    int sx = x * art.w / fieldW;
    int sy = y * art.h / fieldH;
    if (sx < 0)
        sx = 0;
    if (sy < 0)
        sy = 0;
    if (sx >= art.w)
        sx = art.w - 1;
    if (sy >= art.h)
        sy = art.h - 1;
    const std::uint8_t* p =
        art.bgra.data() + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(art.w) + static_cast<std::size_t>(sx)) * 4u;
    b = p[0];
    g = p[1];
    r = p[2];
    a = p[3] ? p[3] : 255;
#endif
}

} // namespace misterplex
