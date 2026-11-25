#pragma once
// Per-resolution constant tables for the Library Browse shell.
// 240p (320x240), 480p (640x480), Native 480 (960x480), HDMI (1280x720). One code path.

#include <algorithm>
#include <array>

namespace misterplex {

enum class BrowseViewMode : int { List = 0, Grid = 1 };

struct BrowseShellLayout {
    int w = 640;
    int h = 480;
    int margin = 16;
    int headerH = 36;
    int footerH = 28;
    int rowH = 24;
    int listRows = 15;
    int fontSm = 12;
    int fontMd = 14;
    int fontLg = 18;
    int fontXl = 22;
    int posterW = 140;
    int posterH = 210;
    int gridCols = 5;
    int gridRows = 2;
    int tileW = 100;
    int tileH = 148;
    int tileGap = 10;
    bool titleUnder = true;
    int focusRing = 2;
    int btnH = 32;
    int btnW = 140;
    int kbCell = 28;
    bool compact = false; // 240p: no list poster, title in footer for grid
    // 2x MiSTerPlex logo bar (plexui kLogoTextPx): 40px on 480p, 60px on 1280x720.
    // 0 on 240p / compact: no logo, historic header.
    int logoPx = 0;
    int logoMarkW = 0;
    int logoMarkHalfH = 0;
    int logoMarkGap = 0;
    std::array<int, 24> rowY{};
    std::array<int, 21> tileX{}, tileY{};
    std::array<int, 40> keyX{}, keyY{};
    int searchResultsY = 0;
};

inline void browseBakeLayout(BrowseShellLayout& L) {
    const int top = L.headerH + 4;
    for (std::size_t i = 0; i < L.rowY.size(); ++i)
        L.rowY[i] = top + static_cast<int>(i) * L.rowH;
    const int gridW = L.gridCols * L.tileW + (L.gridCols - 1) * L.tileGap;
    for (std::size_t i = 0; i < L.tileX.size(); ++i) {
        L.tileX[i] = (L.w - gridW) / 2 + static_cast<int>(i) % L.gridCols * (L.tileW + L.tileGap);
        L.tileY[i] = top + (L.compact ? 4 : 8) + static_cast<int>(i) / L.gridCols *
                    (L.tileH + L.tileGap + (L.titleUnder ? L.fontSm + 4 : 0));
    }
    const int cols = std::max(1, (L.w - 2 * L.margin + 3) / (L.kbCell + 3));
    const int ky = top + 4 + L.rowH + (L.compact ? 2 : 4) + (L.compact ? 4 : 8);
    for (std::size_t i = 0; i < L.keyX.size(); ++i) {
        L.keyX[i] = L.margin + static_cast<int>(i) % cols * (L.kbCell + 3);
        L.keyY[i] = ky + static_cast<int>(i) / cols * (L.kbCell + 3);
    }
    L.searchResultsY = ky + ((static_cast<int>(L.keyX.size()) + cols - 1) / cols) * (L.kbCell + 3) + 4;
}

inline BrowseShellLayout browseLayout240() {
    BrowseShellLayout L;
    L.w = 320;
    L.h = 240;
    L.margin = 8;
    L.headerH = 22;
    L.footerH = 18;
    L.rowH = 18;
    L.listRows = 8;
    L.fontSm = 12;
    L.fontMd = 14;
    L.fontLg = 16;
    L.fontXl = 18;
    L.posterW = 0;
    L.posterH = 0;
    L.gridCols = 3;
    L.gridRows = 2;
    L.tileW = 56;
    L.tileH = 84;
    L.tileGap = 6;
    L.titleUnder = false;
    L.focusRing = 2;
    L.btnH = 22;
    L.btnW = 90;
    L.kbCell = 18;
    L.compact = true;
    browseBakeLayout(L);
    return L;
}

inline BrowseShellLayout browseLayout480() {
    BrowseShellLayout L;
    L.w = 640;
    L.h = 480;
    L.margin = 16;
    L.headerH = 60;
    L.footerH = 34;
    L.rowH = 36;
    L.listRows = 10;
    L.fontSm = 20;
    L.fontMd = 24;
    L.fontLg = 30;
    L.fontXl = 36;
    L.logoPx = 40;
    L.logoMarkW = 14;
    L.logoMarkHalfH = 12;
    L.logoMarkGap = 8;
    L.posterW = 140;
    L.posterH = 210;
    L.gridCols = 5;
    L.gridRows = 2;
    L.tileW = 104;
    L.tileH = 156;
    L.tileGap = 10;
    L.titleUnder = true;
    L.focusRing = 2;
    L.btnH = 40;
    L.btnW = 170;
    L.kbCell = 36;
    browseBakeLayout(L);
    return L;
}

inline BrowseShellLayout browseLayoutHdmi() {
    BrowseShellLayout L;
    L.w = 1280;
    L.h = 720;
    L.margin = 28;
    L.headerH = 76;
    L.footerH = 44;
    L.rowH = 50;
    L.listRows = 11;
    L.fontSm = 28;
    L.fontMd = 36;
    L.fontLg = 42;
    L.fontXl = 48;
    L.logoPx = 60;
    L.logoMarkW = 21;
    L.logoMarkHalfH = 18;
    L.logoMarkGap = 12;
    L.posterW = 180;
    L.posterH = 270;
    L.gridCols = 5;
    L.gridRows = 2;
    L.tileW = 168;
    L.tileH = 252;
    L.tileGap = 16;
    L.titleUnder = true;
    L.focusRing = 2;
    L.btnH = 54;
    L.btnW = 240;
    L.kbCell = 56;
    browseBakeLayout(L);
    return L;
}

// HDMI Native 960x480 (and any >=896-wide 480-line raster): same 2x fonts/logo
// as 480p, but the grid gets as many 104x156 columns as fit instead of a
// 640-wide island in the middle of the screen.
inline BrowseShellLayout browseLayoutNative480(int w = 960) {
    BrowseShellLayout L = browseLayout480();
    L.w = w;
    L.margin = 24;
    L.posterW = 160;
    L.posterH = 240;
    L.btnW = 200;
    const int fit = (w - 2 * L.margin + L.tileGap) / (L.tileW + L.tileGap);
#ifdef MUT_BROWSE_NATIVE_5COL
    L.gridCols = 5;
#else
    L.gridCols = std::clamp(fit, 5, 10);
#endif
    browseBakeLayout(L);
    return L;
}

// Pick the table from the paint canvas. Only a true 15 kHz / 240-line raster
// uses the compact table; 480i on a 480-line canvas keeps the 2x 480 table.
inline BrowseShellLayout browseLayoutFor(int w, int h, bool interlaced = false) {
#ifdef MUT_BROWSE_INTERLACED_240
    const bool compact = interlaced || h <= 288 || w <= 360;
#else
    const bool compact = h <= 288 || w <= 360;
#endif
    // 576i (720x576) is still a 15 kHz CRT: 480 table, not the 1280x720 one.
    const bool sd = h <= 480 || (interlaced && h <= 600);
    BrowseShellLayout L = compact ? browseLayout240()
                          : sd ? (w >= 896 && h <= 480 ? browseLayoutNative480(w) : browseLayout480())
                               : browseLayoutHdmi();
    L.w = w;
    L.h = h;
    L.tileW = std::min(L.tileW, std::max(16, (w - 2 * L.margin -
                                       (L.gridCols - 1) * L.tileGap) / L.gridCols));
    const int under = L.titleUnder ? L.fontSm + 4 : 0;
    L.tileH = std::min(L.tileH, std::max(24, (h - L.headerH - L.footerH - 16 -
                                       (L.gridRows - 1) * L.tileGap) / L.gridRows - under));
    L.listRows = std::min(L.listRows, std::max(1, (h - L.headerH - L.footerH - 8) / L.rowH));
    browseBakeLayout(L);
    return L;
}

inline BrowseViewMode browseDefaultView(const BrowseShellLayout& L) {
    return L.compact ? BrowseViewMode::List : BrowseViewMode::Grid;
}

inline int browseViewportCount(const BrowseShellLayout& L, BrowseViewMode mode) {
    if (mode == BrowseViewMode::List)
        return std::max(1, L.listRows);
    return std::max(1, L.gridCols) * std::max(1, L.gridRows);
}

inline int browsePageDelta(const BrowseShellLayout& L, BrowseViewMode mode) {
#ifdef MUT_BROWSE_SHELL_PAGE_HALF
    const int v = browseViewportCount(L, mode);
    return std::max(1, v / 2);
#else
    return browseViewportCount(L, mode);
#endif
}

inline int browseClampIndex(int i, int n) {
    if (n <= 0)
        return 0;
    if (i < 0)
        return 0;
    if (i >= n)
        return n - 1;
    return i;
}

// Keep focus inside the visible window. scroll is the first visible index.
inline int browseScrollForFocus(int focus, int scroll, int viewport, int count) {
    if (count <= 0 || viewport <= 0)
        return 0;
    focus = browseClampIndex(focus, count);
    scroll = std::max(0, std::min(scroll, std::max(0, count - viewport)));
    if (focus < scroll)
        return focus;
    if (focus >= scroll + viewport)
        return focus - viewport + 1;
    const int maxScroll = std::max(0, count - viewport);
    if (scroll > maxScroll)
        return maxScroll;
    if (scroll < 0)
        return 0;
    return scroll;
}

} // namespace misterplex
