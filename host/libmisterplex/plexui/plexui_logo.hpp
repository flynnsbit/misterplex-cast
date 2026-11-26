#pragma once
// Shared MiSTerPlex wordmark + orange chevron (the plexui logo).
// Used by plexui::Renderer::drawPlexLogo and the Library Browse shell header.

#include "plexui_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace misterplex {
namespace plexui {

// Same accent as kAccentR/G/B in plexui_render.hpp.
constexpr std::uint8_t kPlexLogoAccentR = 0xE5;
constexpr std::uint8_t kPlexLogoAccentG = 0xA0;
constexpr std::uint8_t kPlexLogoAccentB = 0x0D;

struct PlexLogoPlace {
    int textX = 0;
    int textY = 0;
    int markX = 0;
    int markCY = 0;
    int markW = 0;
    int markHalfH = 0;
    int right = 0; // exclusive right edge of the "MiSTerPlex" wordmark
};

inline PlexLogoPlace placePlexLogo(const FontSet& fonts, int markX, int textY, float textPx,
                                   int markW, int markHalfH, int markGap, int fallbackCY) {
    PlexLogoPlace p;
    p.markX = markX;
    p.textY = textY;
    p.markW = markW;
    p.markHalfH = markHalfH;
    int xoff = 0;
    int capCY = -1;
    if (fonts.ready()) {
        const FontSet::GlyphBox g = fonts.measureGlyph(static_cast<int>('M'), textPx, true);
        xoff = g.xoff;
        if (g.h > 0) {
            const float baseline = static_cast<float>(textY) + textPx * 0.8f;
            capCY = static_cast<int>(std::lround(baseline + static_cast<float>(g.yoff) +
                                                 static_cast<float>(g.h) * 0.5f));
        }
    }
    p.textX = markX + markW + markGap + std::max(-xoff, 0);
    p.markCY = capCY >= 0 ? capCY : fallbackCY;
    const float tw = fonts.ready() ? fonts.measureText("MiSTerPlex", textPx, true) : textPx * 9.f;
    p.right = p.textX + static_cast<int>(std::lround(tw));
    return p;
}

inline void drawPlexLogoMarkRgba(std::uint8_t* rgba, int stride, int w, int h, const PlexLogoPlace& p,
                                 std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a,
                                 int clipX0 = 0, int clipY0 = 0,
                                 int clipX1 = std::numeric_limits<int>::max(),
                                 int clipY1 = std::numeric_limits<int>::max()) {
    if (!rgba)
        return;
    for (int dy = -p.markHalfH; dy <= p.markHalfH; ++dy) {
        for (int dx = 0; dx < p.markW; ++dx) {
            const int px = p.markX + dx;
            const int py = p.markCY + dy;
            if (px < 0 || py < 0 || px >= w || py >= h)
                continue;
            if (px < clipX0 || py < clipY0 || px >= clipX1 || py >= clipY1)
                continue;
            if (std::abs(dy) <= p.markHalfH - dx) {
                std::uint8_t* dst = rgba + static_cast<std::size_t>(py) * static_cast<std::size_t>(stride) +
                                    static_cast<std::size_t>(px) * 4u;
                const int inv = 255 - static_cast<int>(a);
                dst[0] = static_cast<std::uint8_t>((r * a + dst[0] * inv) / 255);
                dst[1] = static_cast<std::uint8_t>((g * a + dst[1] * inv) / 255);
                dst[2] = static_cast<std::uint8_t>((b * a + dst[2] * inv) / 255);
                dst[3] = static_cast<std::uint8_t>(std::min(255, dst[3] + static_cast<int>(a)));
            }
        }
    }
}

inline void drawPlexLogoMarkRgb(std::uint8_t* rgb, int w, int h, const PlexLogoPlace& p,
                                std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a,
                                int clipX0 = 0, int clipY0 = 0,
                                int clipX1 = std::numeric_limits<int>::max(),
                                int clipY1 = std::numeric_limits<int>::max()) {
    if (!rgb)
        return;
    for (int dy = -p.markHalfH; dy <= p.markHalfH; ++dy) {
        for (int dx = 0; dx < p.markW; ++dx) {
            const int px = p.markX + dx;
            const int py = p.markCY + dy;
            if (px < 0 || py < 0 || px >= w || py >= h)
                continue;
            if (px < clipX0 || py < clipY0 || px >= clipX1 || py >= clipY1)
                continue;
            if (std::abs(dy) <= p.markHalfH - dx) {
                std::uint8_t* dst = rgb + (static_cast<std::size_t>(py) * static_cast<std::size_t>(w) +
                                           static_cast<std::size_t>(px)) * 3u;
                const int inv = 255 - static_cast<int>(a);
                dst[0] = static_cast<std::uint8_t>((r * a + dst[0] * inv) / 255);
                dst[1] = static_cast<std::uint8_t>((g * a + dst[1] * inv) / 255);
                dst[2] = static_cast<std::uint8_t>((b * a + dst[2] * inv) / 255);
            }
        }
    }
}

} // namespace plexui
} // namespace misterplex
