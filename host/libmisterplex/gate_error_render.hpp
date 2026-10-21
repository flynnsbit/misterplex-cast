#pragma once
// SPS-gate layer 3 (pre-reg v1.4.3 b57a87fb section 4 s1/v1/n1; L3 paper v1 c1f55425): the on-screen gate error,
// rendered IN PLACE OF the idle chevron while MediaPlayer's gate-error latch is set. Pure, no daemon state.
//
// Text wraps at word boundaries to floor((textEnd(activeW) - margin) / 8) characters per line (a word longer than a
// line is split) and clips to floor((textEnd(activeH) - margin) / 16) lines, where textEnd(e) = min(e - margin,
// e * 9 / 10): the text keeps the margin on every side AND ends inside the visible 90% of the active rect. The glass
// capture of the 800x600 HDMI output is 720x576, i.e. only the first 90% of a full-width target's columns (and 96% of
// its rows): the old (activeW - 2*margin)/8 wrap gave 76-col lines at 640 wide, which ran to x = 624 and lost
// everything past x = 576 (R7 'No ladd|er', release 11:36; Track D 17:17). At 640 it now wraps at 70 cols (x < 576).
// The text origin (activeX + margin, margin) is unchanged. When text remains, the last line ends "..." (the 8x16
// font is printable ASCII only, so no U+2026; other bytes render as '?'). Like the chevron renderers (idle_screen.hpp),
// each renderer fills the WHOLE target buffer with the background, and the glyphs land only inside the active rect
// [activeX, activeX + activeW) x [0, h). Every write is bounded by the buffer's own w x h: nothing outside the buffer
// is ever touched. browse_render.hpp's putPixel bounds only against its 640-wide canvas, so it is not reused here.
#include "../../arm/misterplexd/font_8x16.hpp"
#include "libmisterplex/idle_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex::gate {

constexpr int kErrGlyphW = 8;
constexpr int kErrGlyphH = 16;
constexpr int kErrMargin = 16;
constexpr int kErrVisibleNum = 9;  // the text ends inside the visible 9/10 of the active extent (720 of 800 columns)
constexpr int kErrVisibleDen = 10;

// The right (or bottom) bound of the text block for an active extent: the margin AND the visible 90%.
inline int errTextEnd(int extent, int margin = kErrMargin) {
    return std::min(extent - margin, extent * kErrVisibleNum / kErrVisibleDen);
}

// Wraps to an explicit cols x rows grid (wrapErrorText derives them from the active rect).
inline std::vector<std::string> wrapErrorTextCols(const std::string& text, int cols, int rows) {
    std::vector<std::string> out;
    if (cols <= 0 || rows <= 0)
        return out;
    std::string s;
    s.reserve(text.size());
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        s += (u >= 0x20 && u <= 0x7E) ? c : '?';
    }
    size_t i = 0;
    bool truncated = false;
    for (;;) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        if (i >= s.size())
            break;
        if (static_cast<int>(out.size()) == rows) {
            truncated = true;
            break;
        }
        size_t end = std::min(s.size(), i + static_cast<size_t>(cols));
        if (end < s.size()) {
            const size_t sp = s.rfind(' ', end);
            if (sp != std::string::npos && sp > i)
                end = sp;
        }
        std::string line = s.substr(i, end - i);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out.push_back(std::move(line));
        i = end;
    }
    if (truncated) {
        std::string& last = out.back();
        const size_t keep = cols > 3 ? static_cast<size_t>(cols - 3) : 0;
        if (last.size() > keep)
            last.resize(keep);
        last += "...";
        if (last.size() > static_cast<size_t>(cols))
            last.resize(static_cast<size_t>(cols));
    }
    return out;
}

inline std::vector<std::string> wrapErrorText(const std::string& text, int activeW, int activeH,
                                              int margin = kErrMargin) {
    return wrapErrorTextCols(text, (errTextEnd(activeW, margin) - margin) / kErrGlyphW,
                             (errTextEnd(activeH, margin) - margin) / kErrGlyphH);
}

// Calls put(x, y) for every set pixel of the wrapped lines, laid out from (originX, originY).
template <class Put>
inline void forEachErrorGlyphPixel(const std::vector<std::string>& lines, int originX, int originY, Put put) {
    for (size_t li = 0; li < lines.size(); ++li) {
        for (size_t ci = 0; ci < lines[li].size(); ++ci) {
            unsigned char u = static_cast<unsigned char>(lines[li][ci]);
            if (u < kFont8x16First || u > kFont8x16Last)
                u = '?';
            const auto& glyph = kFont8x16[static_cast<size_t>(u - kFont8x16First)];
            for (int gy = 0; gy < kErrGlyphH; ++gy) {
                const uint8_t bits = glyph[static_cast<size_t>(gy)];
                for (int gx = 0; gx < kErrGlyphW; ++gx)
                    if (bits & (0x80u >> gx))
                        put(originX + static_cast<int>(ci) * kErrGlyphW + gx,
                            originY + static_cast<int>(li) * kErrGlyphH + gy);
            }
        }
    }
}

inline bool validErrorTarget(int w, int h, int activeX, int activeW) {
    return w > 0 && h > 0 && activeX >= 0 && activeW > 0 && activeX + activeW <= w;
}

// Packed RGB24 (stride w * 3): black background, white text.
inline bool renderGateErrorRgb24(uint8_t* rgb, int w, int h, int activeX, int activeW, const std::string& text) {
    if (!rgb || !validErrorTarget(w, h, activeX, activeW))
        return false;
    std::memset(rgb, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    const auto lines = wrapErrorText(text, activeW, h);
    forEachErrorGlyphPixel(lines, activeX + kErrMargin, kErrMargin, [&](int x, int y) {
        if (x < activeX || x >= activeX + activeW || y < 0 || y >= h)
            return;
        uint8_t* p = rgb + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
        p[0] = p[1] = p[2] = 255;
    });
    return true;
}

// Planar I420 (Y w x h, then U and V at w/2 x h/2): the same picture through the idle renderer's own RGB -> YUV
// (idleRgbToY/U/V), so the background matches the chevron's dark field convention. Text is achromatic: chroma stays
// the background's.
inline bool renderGateErrorI420(uint8_t* yuv, int w, int h, int activeX, int activeW, const std::string& text) {
    if (!yuv || (w & 1) || (h & 1) || !validErrorTarget(w, h, activeX, activeW))
        return false;
    const size_t ysz = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t csz = ysz / 4;
    std::memset(yuv, idleRgbToY(0, 0, 0), ysz);
    std::memset(yuv + ysz, idleRgbToU(0, 0, 0), csz);
    std::memset(yuv + ysz + csz, idleRgbToV(0, 0, 0), csz);
    const uint8_t fg = idleRgbToY(255, 255, 255);
    const auto lines = wrapErrorText(text, activeW, h);
    forEachErrorGlyphPixel(lines, activeX + kErrMargin, kErrMargin, [&](int x, int y) {
        if (x < activeX || x >= activeX + activeW || y < 0 || y >= h)
            return;
        yuv[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = fg;
    });
    return true;
}

} // namespace misterplex::gate
