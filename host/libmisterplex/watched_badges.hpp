#pragma once
// FEATURE-GAP E18: a watched badge on a library poster.
//
// WATCHED_BADGES (misterplex.conf) / MPX_WATCHED_BADGES (env wins). Default
// off. Unset, empty, 0, off, false, and no stay off.
//
// Off: stampWatchedBadge writes nothing. Posters, the episode "Watched"
// line, and progress bars stay on the tip path.
// On: a finished title (viewCount > 0 and no in-progress viewOffset) gets
// an orange disc and a dark check in the poster's top-right corner. An
// in-progress title and an unwatched title do not. The badge stays inside
// the poster rect.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace misterplex {

inline constexpr std::uint8_t kWatchedBadgeR = 0xE5;
inline constexpr std::uint8_t kWatchedBadgeG = 0xA0;
inline constexpr std::uint8_t kWatchedBadgeB = 0x0D;
inline constexpr std::uint8_t kWatchedCheckR = 0x14;
inline constexpr std::uint8_t kWatchedCheckG = 0x14;
inline constexpr std::uint8_t kWatchedCheckB = 0x16;

inline bool watchedBadgesEnvOn(const char* e) {
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

// Default off. MPX_WATCHED_BADGES wins over WATCHED_BADGES when it is set.
inline bool watchedBadgesEnabled() {
#ifdef MUT_WBADGE_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_WATCHED_BADGES"); e && e[0])
        return watchedBadgesEnvOn(e);
    return watchedBadgesEnvOn(std::getenv("WATCHED_BADGES"));
#endif
}

struct WatchedBadge {
    bool show = false;
    int x = 0;
    int y = 0;
    int size = 0;
};

// Finished titles only. viewOffset > 0 is in progress (the existing bar).
inline bool watchedBadgeMark(int viewCount, int64_t viewOffsetMs) {
#ifdef MUT_WBADGE_NEVER
    (void)viewCount;
    (void)viewOffsetMs;
    return false;
#elif defined(MUT_WBADGE_UNWATCHED)
    (void)viewCount;
    (void)viewOffsetMs;
    return true;
#elif defined(MUT_WBADGE_PROGRESS)
    return viewCount > 0 || viewOffsetMs > 0;
#else
    return viewOffsetMs <= 0 && viewCount > 0;
#endif
}

// Empty when the flag is off, the title is not finished, or the poster is
// too small to hold an 8 px disc inside its rect.
inline WatchedBadge watchedBadgeBox(int viewCount, int64_t viewOffsetMs, int posterX, int posterY, int posterW,
                                    int posterH) {
    WatchedBadge b;
#ifdef MUT_WBADGE_SHOW_WHEN_OFF
    const bool on = true;
#else
    const bool on = watchedBadgesEnabled();
#endif
    if (!on || posterW < 16 || posterH < 16)
        return b;
    if (!watchedBadgeMark(viewCount, viewOffsetMs))
        return b;
    int size = posterW / 5;
    if (size < 8)
        size = 8;
    if (size > 24)
        size = 24;
    size &= ~1;
    if (size < 8)
        size = 8;
    if (size + 2 >= posterW || size + 2 >= posterH)
        return b;
    b.size = size;
    b.x = posterX + posterW - size - 2;
    b.y = posterY + 2;
    b.y &= ~1;
    if (b.x < posterX || b.y < posterY)
        return {};
    if (b.x + b.size > posterX + posterW || b.y + b.size > posterY + posterH)
        return {};
    b.show = true;
    return b;
}

inline void paintWatchedBadge(std::uint8_t* rgba, int canvasW, int canvasH, const WatchedBadge& box) {
    if (!rgba || canvasW <= 0 || canvasH <= 0 || canvasW > 8192 || canvasH > 8192)
        return;
    if (!box.show || box.size < 8 || box.size > 64 || box.x < 0 || box.y < 0) {
#ifdef MUT_WBADGE_PAINT_WHEN_OFF
        rgba[0] = kWatchedBadgeR;
        rgba[1] = kWatchedBadgeG;
        rgba[2] = kWatchedBadgeB;
        rgba[3] = 255;
#endif
        return;
    }
    const int s = box.size;
    const int limit = (s * s);
    auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x < 0 || y < 0 || x >= canvasW || y >= canvasH)
            return;
        std::uint8_t* p =
            rgba + (static_cast<std::size_t>(y) * static_cast<std::size_t>(canvasW) + static_cast<std::size_t>(x)) * 4;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    const int cx = s - 1;
    for (int row = 0; row < s; ++row) {
        for (int col = 0; col < s; ++col) {
            const int dx = col * 2 - cx;
            const int dy = row * 2 - cx;
            if (dx * dx + dy * dy > limit)
                continue;
            put(box.x + col, box.y + row, kWatchedBadgeR, kWatchedBadgeG, kWatchedBadgeB);
        }
    }
    auto line = [&](int x0, int y0, int x1, int y1) {
        const int adx = std::abs(x1 - x0);
        const int ady = std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int err = adx - ady;
        int x = x0;
        int y = y0;
        for (int guard = 0; guard < 256; ++guard) {
            for (int oy = -1; oy <= 0; ++oy) {
                for (int ox = -1; ox <= 0; ++ox)
                    put(box.x + x + ox, box.y + y + oy, kWatchedCheckR, kWatchedCheckG, kWatchedCheckB);
            }
            if (x == x1 && y == y1)
                break;
            const int e2 = err * 2;
            if (e2 > -ady) {
                err -= ady;
                x += sx;
            }
            if (e2 < adx) {
                err += adx;
                y += sy;
            }
        }
    };
    line(s * 2 / 8, s * 5 / 8, s * 3 / 8, s * 6 / 8);
    line(s * 3 / 8, s * 6 / 8, s * 6 / 8, s * 2 / 8);
}

// Flag off, or not a finished title: the buffer is not written.
inline void stampWatchedBadge(std::uint8_t* rgba, int canvasW, int canvasH, int viewCount, int64_t viewOffsetMs,
                              int posterX, int posterY, int posterW, int posterH) {
    const WatchedBadge box = watchedBadgeBox(viewCount, viewOffsetMs, posterX, posterY, posterW, posterH);
    paintWatchedBadge(rgba, canvasW, canvasH, box);
}

} // namespace misterplex
