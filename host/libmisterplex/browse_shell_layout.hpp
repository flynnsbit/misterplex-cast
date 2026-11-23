#pragma once
// Per-resolution constant tables for the Library Browse shell.
// 240p (320x240), 480p (640x480), HDMI (1280x720, s = h/480). One code path.

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
    L.headerH = 36;
    L.footerH = 28;
    L.rowH = 24;
    L.listRows = 15;
    L.fontSm = 12;
    L.fontMd = 14;
    L.fontLg = 18;
    L.fontXl = 22;
    L.posterW = 140;
    L.posterH = 210;
    L.gridCols = 5;
    L.gridRows = 2;
    L.tileW = 100;
    L.tileH = 148;
    L.tileGap = 10;
    L.titleUnder = true;
    L.focusRing = 2;
    L.btnH = 32;
    L.btnW = 140;
    L.kbCell = 28;
    browseBakeLayout(L);
    return L;
}

inline BrowseShellLayout browseLayoutHdmi() {
    BrowseShellLayout L;
    L.w = 1280;
    L.h = 720;
    L.margin = 28;
    L.headerH = 48;
    L.footerH = 36;
    L.rowH = 28;
    L.listRows = 18;
    L.fontSm = 16;
    L.fontMd = 18;
    L.fontLg = 24;
    L.fontXl = 30;
    L.posterW = 180;
    L.posterH = 270;
    L.gridCols = 7;
    L.gridRows = 3;
    L.tileW = 112;
    L.tileH = 168;
    L.tileGap = 12;
    L.titleUnder = true;
    L.focusRing = 2;
    L.btnH = 40;
    L.btnW = 180;
    L.kbCell = 36;
    browseBakeLayout(L);
    return L;
}

// Pick the table from the paint canvas. 15 kHz / 240p uses the compact table.
inline BrowseShellLayout browseLayoutFor(int w, int h, bool interlaced = false) {
    BrowseShellLayout L = interlaced || h <= 240 || w <= 320 ? browseLayout240()
                             : h <= 480 ? browseLayout480() : browseLayoutHdmi();
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
