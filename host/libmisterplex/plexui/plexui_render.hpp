#pragma once

#include "plexui_font.hpp"
#include "plexui_images.hpp"
#include "plexui_types.hpp"
#include "libmisterplex/overscan_safe.hpp"
#include "libmisterplex/ui_safe_layout.hpp"
#include "libmisterplex/watched_badges.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace misterplex {
namespace plexui {

// Render geometry (OB port). R1 is the 640x480 default with s = 1, where every helper below
// returns the historic constant (byte-identical R1 output, tests/test_plexui_geom.cpp). The OB
// overlay sets the canvas to the core's raster (e.g. 1280x720 -> s = 1.5): layout scales with
// the height, the grid gains columns with the width (16:9). Owned by the controller worker thread.
struct ScreenGeom {
    int w = 640;
    int h = 480;
    float s = 1.f;
};
ScreenGeom& screenGeom();
// s = h / 480; w and h are clamped to [160, 1920] x [120, 1080].
void setScreenGeom(int w, int h);
inline bool isR1Geom() { return screenGeom().w == 640 && screenGeom().h == 480; }
inline bool is240pGeom() { return screenGeom().w == 320 && screenGeom().h == 240; }
inline float uiScale() { return screenGeom().s; }
// Raster scale (h/480) times UI_SIZE. Default normal keeps P(32)==32 on R1.
inline float uiLayoutScale() { return screenGeom().s * uiSizeScale(uiSafePrefs().size); }
inline int P(int v) { return static_cast<int>(std::lround(static_cast<float>(v) * uiLayoutScale())); }
inline float PF(float v) { return v * uiLayoutScale(); }
// FONT-2X (11-24): on the PlexOB canvases (HDMI Native 960x480, 480p 720x480, 1280x720 ...)
// the browse text was 11-18 px on a 480-line raster, unreadable once the overlay is stretched to
// the TV. There every text size is ~2x (body and labels exactly 2x, headings 1.4-1.8x, the logo 2x)
// and each text band (rows, captions, chips, cards, hint bar) is refit around the larger glyphs.
// Glyphs stay whole-pixel stb_truetype rasters at the new size (no bitmap upscale, so no blur).
// The legacy R1 640x480 raster (test_plexui_geom byte goldens) and the dedicated 240p rail keep
// the historic sizes: every XP/XPF/TF below returns the old P()/PF() value there.
inline bool uiText2x() {
#if defined(MUT_FONT2X_OFF)
    return false;
#else
    return !isR1Geom() && !is240pGeom();
#endif
}
// Text pixel height for a historic size v: <= 14 px doubles, larger headings grow less
// (16 -> 30, 18 -> 31, 28 -> 40) so a page title still fits one line. Whole pixels at s = 1.
inline float TF(float v) {
    if (!uiText2x())
        return PF(v);
    const float t = v <= 14.f ? v * 2.f : 28.f + (v - 14.f) * 6.f / 7.f;
    return PF(std::round(t));
}
// Logo text: exactly 2x (20 -> 40).
inline float kLogoTextPx() { return PF(uiText2x() ? 40.f : 20.f); }
// A text-driven band: the historic value r1, or x2 on the 2x-text canvases.
inline int XP(int r1, int x2) { return P(uiText2x() ? x2 : r1); }
inline float XPF(float r1, float x2) { return PF(uiText2x() ? x2 : r1); }
// Vertical frame origin for the top-of-page text. R1 used kSafeMargin() (the left inset) for
// y; with overscan set that is the horizontal percent of the width (105 px at 960x480 ov11),
// so the 2x canvases use the real top inset. Identical on R1 (no overscan there).
inline int kSafeTopTextY();

inline int kScreenW() { return screenGeom().w; }
inline int kScreenH() { return screenGeom().h; }
// 240p CRT: P() and 5% both drop under 24 px and clip header/sidebar glyphs.
// Floor the uiSafe* legacy argument; 480p/480i/R1 still use P(32).
inline int kSafe240pFloor() { return 24; }
// Floor at the configured percent; never smaller than the historic P(32)/R1-72
// so UI_SIZE=normal + UI_SAFE_MARGIN=5 keeps test_plexui_geom goldens.
// E9: OVERSCAN_LEFT replaces this when set. Unset is the line above.
inline int kSafeMarginUncalibrated() {
    return uiSafeLeftPx(kScreenW(), uiSafePrefs().marginPct,
                        is240pGeom() ? kSafe240pFloor() : P(32));
}
inline int kSafeMargin() {
    return overscanEdgeOr(overscanEdges().left, kScreenW(), kSafeMarginUncalibrated());
}
// Vertical frame. Unset matches kSafeMarginUncalibrated(), which is today's kSafeMargin().
inline int kSafeTopInset() {
    return overscanEdgeOr(overscanEdges().top, kScreenH(), kSafeMarginUncalibrated());
}
inline int kSafeBottomInset() {
    return overscanEdgeOr(overscanEdges().bottom, kScreenH(), kSafeMarginUncalibrated());
}
inline int kSafeTopTextY() { return uiText2x() ? kSafeTopInset() : kSafeMargin(); }
//  lab HDMI path scales the 640 raster ~1.2x and crops ~40 px on the right (glass 10-18): extra
// right inset. The OB overlay is drawn 1:1 on the raster: a plain scaled margin is enough there.
inline int kSafeRight() {
    const int legacy = uiSafeRightPx(kScreenW(), uiSafePrefs().marginPct, isR1Geom() ? 72 : P(40));
    return overscanEdgeOr(overscanEdges().right, kScreenW(), legacy);
}
inline int kFocusRingPad() { return P(5); }
// Server-list status: P(440) at default 5%/normal (R1 golden). Large / a
// taller percent lifts the line so it stays inside title-safe.
inline int kStatusLineY() {
    const int yPct = uiSafeTopPx(kScreenH(), uiSafePrefs().marginPct,
                                 is240pGeom() ? kSafe240pFloor() : 0);
    const int bottom = overscanEdgeOr(overscanEdges().bottom, kScreenH(), yPct);
    const int floorY = kScreenH() - bottom - XP(16, 36);
    return std::min(P(440), floorY);
}
// Left edge of the sidebar content (focus pill, icons, labels). The pill historically sat at P(8): 1.1% of a 720-wide
// raster and 0.9% of 1280, outside the UI_SAFE_MARGIN frame (glass #53, 480p/480i). Content now starts at the
// configured safe margin. Exactly R1 (640x480, normal, default 5%) is the
// golden path of test_plexui_geom and stays at P(8).
inline int kSidebarFocusX() {
    const int legacy = P(8);
#if defined(MUT_SIDEBAR_NO_SAFE_LEFT)
    return legacy;
#endif
    if (overscanEdges().left >= 0)
        return std::max(legacy, overscanInsetPx(kScreenW(), overscanEdges().left));
    const UiSafePrefs pr = uiSafePrefs();
    if (isR1Geom() && pr.size == UiSize::Normal && pr.marginPct == kUiSafeMarginDefaultPct)
        return legacy;
    return std::max(legacy, uiSafeLeftPx(kScreenW(), pr.marginPct,
                                         is240pGeom() ? kSafe240pFloor() : 0));
}
// How far the pre-fix layout (pill at P(8), icon at P(12), label at P(40)) moves right.
inline int kSidebarShiftX() { return kSidebarFocusX() - P(8); }
// Moving labels into title-safe must not consume their width (F2196).
// The dedicated 240p rail already has extra room beyond its scaled width.
inline int kSidebarW() { return is240pGeom() ? 128 : XP(200, 240) + kSidebarShiftX(); }
// Library-grid page title / item-count x. Full-bleed when the drawer is closed;
// when the rail is open, sit to the right of kSidebarW so "MiSTerPlex" and the
// library title do not share kSafeMargin() (glass-1026).
inline int kGridTitleX(bool drawerOpen) {
    return kSafeMargin() + (drawerOpen ? kSidebarW() : 0);
}
inline int kSidebarRowH() { return XP(44, 72); }
inline int kTextRightInset() { return P(8); }
// MiSTerPlex logo (drawPlexLogo): orange chevron of width P(7) sits at kSafeMargin(),
// fully left of the "MiSTerPlex" text with a P(4) gap. The mark is vertically centred
// on the text's cap height (M). Text at kSafeMargin()+P(7)+P(4), y = kSafeMargin()-P(2).
inline int kLogoTextY() { return kSafeTopTextY() - P(2); }
inline int kLogoMarkW() { return XP(7, 14); }
inline int kLogoMarkHalfH() { return XP(6, 12); }
inline int kLogoMarkGap() { return XP(4, 8); }
inline int kLogoMarkX() { return kSafeMargin(); }
// firstGlyphXoff is the M's xoff from measureGlyph; a negative bearing shifts the text right
// so the ink still clears the mark by kLogoMarkGap().
inline int kLogoTextX(int firstGlyphXoff = 0) {
    return kLogoMarkX() + kLogoMarkW() + kLogoMarkGap() + std::max(-firstGlyphXoff, 0);
}
inline int kLogoMarkCYFallback() { return kSafeTopTextY() + XP(10, 20); }
inline int kLogoMarkCY(int capCenterY = -1) {
    return capCenterY >= 0 ? capCenterY : kLogoMarkCYFallback();
}
inline int logoCapCenterY(int glyphYoff, int glyphH, float pixelHeight) {
    const float baseline = static_cast<float>(kLogoTextY()) + pixelHeight * 0.8f;
    return static_cast<int>(std::lround(baseline + static_cast<float>(glyphYoff) +
                                        static_cast<float>(glyphH) * 0.5f));
}
// Half-open pixel box of the painted chevron [x0, x1) x [y0, y1).
inline void logoMarkPixelBox(int& x0, int& y0, int& x1, int& y1, int capCenterY = -1) {
    const int cy = kLogoMarkCY(capCenterY);
    x0 = kLogoMarkX();
    x1 = x0 + kLogoMarkW();
    y0 = cy - kLogoMarkHalfH();
    y1 = cy + kLogoMarkHalfH() + 1;
}
inline int sidebarLogoBottom(int capCenterY = -1) {
    const int markBottom = kLogoMarkCY(capCenterY) + kLogoMarkHalfH();
    const int textBottom = kLogoTextY() + XP(22, 44);
    return std::max(markBottom, textBottom);
}
// FONT-2X: the rail list stops above the hint bar band too, and shows whole rows only.
inline int sidebarListBottom() {
    return kScreenH() - (uiText2x() ? std::max(kSafeBottomInset(), XP(20, 40)) : kSafeBottomInset());
}
inline int sidebarListTop() { return std::max(P(64), sidebarLogoBottom() + XP(8, 16)); }
inline int kSidebarTextX() { return kSidebarFocusX() + XP(4, 8); }
// FONT-2X: the rail scrolls by whole rows (2x rows are tall; a half row would clip its text).
// Returns the scroll that keeps focusRow (0 = Home) visible, moving as little as possible.
inline int sidebarWholeRowScroll(int focusRow, int scrollY, int totalRows) {
    const int rowH = std::max(1, kSidebarRowH());
    const int vis = std::max(1, (sidebarListBottom() - sidebarListTop()) / rowH);
    int first = scrollY / rowH;
    if (focusRow < first)
        first = focusRow;
    else if (focusRow >= first + vis)
        first = focusRow - vis + 1;
    first = std::max(0, std::min(first, std::max(0, totalRows - vis)));
    return first * rowH;
}
inline bool sidebarHomeShowsFocusBar(int sidebarFocus) {
    return sidebarFocus == kSidebarHomeFocus;
}
inline bool sidebarHomeShowsPageCue(int sidebarFocus) { return sidebarFocus <= 0; }
inline int kTextMaxX() { return kScreenW() - kSafeRight() - kTextRightInset(); }
inline int kGridChipRowH() { return XP(30, 52); }
inline float kGridChipPadX() { return XPF(10.f, 18.f); }
inline float kGridChipH() { return XPF(22.f, 40.f); }
inline float kGridChipGap() { return XPF(8.f, 12.f); }
// Home hub rows (one place for the 96 / 144 / 12 / 64 / 52 / 26 render and controller repeated).
// HOME-UX r3: the poster is no longer a fixed P(96) x P(144). It is the largest 2:3 poster
// that keeps the same number of whole hub rows on screen as the P(96) x P(144) base did
// (so Home never drops to one row), capped at 1.5x the base and at the content width. The
// tile count is then however many of those whole posters fit side by side (hubVisibleTileCount):
// device HDMI Native canvas 960x480 + overscan 11/11/12/12 -> 3 x 106x159 (was 4 x 96x144),
// 480p 720x480 + ov11 -> 2 x 106x159 (the poster also keeps the last row above the hint bar).
// The leftover width is then spread as wider equal gaps + equal side margins (hubSpreadPart):
// 960x480 ov11 gap 38, margins 26/27; 720x480 ov11 gap 32, margins 20/21. Every input is the canvas, UI_SIZE, UI_SAFE_MARGIN and
// overscan, so any output-size or safe-area change recomputes it.
inline int kHubPosterBaseW() { return P(96); }
inline int kHubPosterBaseH() { return P(144); }
inline int kHubBaseGap() { return P(12); }   // minimum gap; the fit count uses it
// The legacy-core R1 path (exactly 640x480, normal, default 5%, no overscan) is the golden
// path of test_plexui_geom: fixed 96 x 144 posters, P(12) gaps, row flush left.
inline bool hubIsR1Golden() {
    const UiSafePrefs pr = uiSafePrefs();
    return isR1Geom() && pr.size == UiSize::Normal && pr.marginPct == kUiSafeMarginDefaultPct &&
           !overscanEdges().any();
}
inline int kHubTop() {
    const int legacy = P(64);
    if (overscanEdges().top < 0)
        return legacy;
    return std::max(legacy, overscanInsetPx(kScreenH(), overscanEdges().top));
}
inline int kHubPosterOffY() { return XP(26, 44); }   // row title band above the poster
inline int kHubRowExtraH() { return XP(52, 60); }   // row title + caption gap below the poster
inline int kHubBottomReserve() { return XP(20, 40); }   // browse hint bar band
// FONT-2X: the focused tile's title/year line under the row (P(8) gap + one TF(14) line).
inline int kHubCaptionH() { return XP(22, 40); }
// Whole hub rows that fit for a poster of height posterH (same rule as homeVisibleHubRows).
inline int hubRowsForPosterH(int posterH) {
    int n = 0;
    while (kHubTop() + n * (posterH + kHubRowExtraH()) + kHubPosterOffY() + posterH <= kScreenH())
        ++n;
    return std::max(1, n);
}
// Home hub rows: whole tiles only, inside [kHubContentLeft, kHubContentRight].
// kHubFocusPad keeps the focused poster's 8% grow + ring inside the safe area.
inline int kHubFocusPad() { return P(6); }
// FONT-2X: the wider 2x rail already sits inside the safe margin (kSidebarShiftX), so the
// row starts a fixed gutter after it instead of a second full safe margin.
inline int kHubContentLeft() { return kSidebarW() + (uiText2x() ? P(24) : kSafeMargin()); }
inline int kHubContentRight() { return kScreenW() - kSafeRight() - kHubFocusPad(); }
inline int kHubContentWidth() { return std::max(0, kHubContentRight() - kHubContentLeft()); }
inline int kHubPosterW() {
    const int baseW = kHubPosterBaseW(), baseH = kHubPosterBaseH();
    // The legacy-core R1 path (exactly 640x480, normal, default 5%, no overscan) is the
    // byte-identical golden path of test_plexui_geom and keeps its 96 x 144 posters, as
    // kSidebarFocusX does. PlexOB canvases (960x480 / 720x480 / 1280x720 ...) grow.
    if (hubIsR1Golden())
        return baseW;
    if (uiText2x()) {
        // FONT-2X: the 2x row title and caption leave room for one whole row on a 480-line
        // canvas (two only if both still get at least the base 96x144 poster). The poster is
        // the largest 2:3 size that keeps the row(s) and the focused caption above the hint
        // bar, capped at 1.5x the base (144x216) and at the content width.
        const int limit = kScreenH() - kHubBottomReserve() - kHubTop() - kHubCaptionH();
        const int two = (limit - 2 * kHubPosterOffY() - kHubRowExtraH()) / 2;
        const int maxH = two >= baseH ? two : limit - kHubPosterOffY();
        int w = std::min(maxH * 2 / 3, baseW * 3 / 2);
        w = std::min(w, kHubContentWidth());
        return std::max(P(48), w);
    }
    const int rows = hubRowsForPosterH(baseH);
    // A grown poster also keeps the last row above the browse hint bar (drawBrowseHint:
    // P(16) line + P(4) pad at the bottom); the base size keeps its historic rows.
    const int room = kScreenH() - kHubBottomReserve() - kHubTop() - kHubPosterOffY() -
                     (rows - 1) * kHubRowExtraH();
    const int maxH = std::max(baseH, room / rows);
    int w = std::min(maxH * 2 / 3, baseW * 3 / 2);
    w = std::min(w, kHubContentWidth());
    return std::max(baseW, w);
}
inline int kHubPosterH() {
    const int w = kHubPosterW();
    return w == kHubPosterBaseW() ? kHubPosterBaseH() : (w * 3) / 2;
}
inline int kHubRowAdvance() { return kHubPosterH() + kHubRowExtraH(); }
// How many whole posters fit side by side (with the minimum gap) in the content width; at least 1.
inline int hubVisibleTileCount() {
    const int fit = (kHubContentWidth() + kHubBaseGap()) / (kHubPosterW() + kHubBaseGap());
    return std::max(1, fit);
}
// HOME-UX r3b (reviewer): the width the n whole tiles leave over is not left on the right. It is
// split into n + 1 equal parts: each gap grows by one part and the row gets one part of margin
// on each side, so the row is centred and balanced in [kHubContentLeft, kHubContentRight]
// (any odd pixels go half to each margin). R1 golden path: no spread.
inline int hubRowSlack() {
    const int n = hubVisibleTileCount();
    return std::max(0, kHubContentWidth() - n * kHubPosterW() - (n - 1) * kHubBaseGap());
}
inline int hubSpreadPart() {
#if defined(MUT_HUX_NO_SPREAD)
    return 0;
#endif
    return hubIsR1Golden() ? 0 : hubRowSlack() / (hubVisibleTileCount() + 1);
}
inline int kHubGap() { return kHubBaseGap() + hubSpreadPart(); }
inline int hubRowMarginL() {
    if (hubIsR1Golden())
        return 0;
    const int n = hubVisibleTileCount();
    const int rest = hubRowSlack() - hubSpreadPart() * (n + 1);   // 0..n leftover pixels
    return hubSpreadPart() + rest / 2;
}
inline int hubRowMarginR() {
    const int n = hubVisibleTileCount();
    return kHubContentWidth() - hubRowMarginL() - n * kHubPosterW() - (n - 1) * kHubGap();
}
inline int kHubTileStep() { return kHubPosterW() + kHubGap(); }
inline int hubTileX(int col, int scrollX) {
    return kHubContentLeft() + hubRowMarginL() - scrollX + col * kHubTileStep();
}
// r3c (reviewer): hub row title starts on the first poster's left edge (the balanced margin);
// R1 golden has margin 0 so the title stays at the content left there.
inline int hubRowTitleX() {
#if defined(MUT_HUX_TITLE_AT_CONTENT_LEFT)
    return kHubContentLeft();
#else
    return hubTileX(0, 0);
#endif
}
// First visible column for a row of itemCount with focus on focusCol: keeps focus on screen,
// never scrolls past the last tile (stateless: focus rides the right edge once the row scrolls).
inline int hubFirstVisibleCol(int itemCount, int focusCol) {
    const int n = hubVisibleTileCount();
    if (itemCount <= n)
        return 0;
    const int f = std::max(0, std::min(focusCol, itemCount - 1));
    return std::max(0, std::min(f - n + 1, itemCount - n));
}
// Tile-aligned horizontal scroll: always a whole multiple of kHubTileStep.
inline int hubRowScrollXForCol(int itemCount, int focusCol) {
    if (itemCount <= 0)
        return 0;
    return hubFirstVisibleCol(itemCount, focusCol) * kHubTileStep();
}
// True when tile col is drawn for this scroll (whole tile inside the content band).
// Degenerate band (extreme overscan: not even one whole poster fits): still show exactly one
// tile, the focused one, at the band's left edge rather than an empty row.
inline bool hubTileVisible(int col, int scrollX) {
    const int x = hubTileX(col, scrollX);
    if (kHubContentWidth() < kHubPosterW())
        return x == kHubContentLeft();
    return x >= kHubContentLeft() && x + kHubPosterW() <= kHubContentRight();
}
// #206 glass: Home had no vertical scroll. Rows past the screen (row 2+ on  lab) were never drawn,
// so focus moved off screen. The first drawn hub row keeps the focused row fully on screen.
inline int homeVisibleHubRows() {
    // FONT-2X: a row counts only with its poster and the focused caption above the hint bar.
    const int limit = uiText2x() ? kScreenH() - kHubBottomReserve() - kHubCaptionH() : kScreenH();
    int n = 0;
    while (kHubTop() + n * kHubRowAdvance() + kHubPosterOffY() + kHubPosterH() <= limit)
        ++n;
    return std::max(1, n);
}
inline int homeFirstHubRow(int focusRow, int hubCount) {
#ifdef MUT_HOME_NO_VSCROLL
    (void)focusRow; (void)hubCount;
    return 0;
#else
    if (hubCount <= 0 || focusRow <= 0)
        return 0;
    const int row = std::min(focusRow, hubCount - 1);
    return std::max(0, row - (homeVisibleHubRows() - 1));
#endif
}
// Y of hub row 0 after the vertical scroll (rows above the first drawn row are negative).
inline int homeRowY0(int focusRow, int hubCount) {
    return kHubTop() - homeFirstHubRow(focusRow, hubCount) * kHubRowAdvance();
}
// Shared Home row origin: Y of hub row `row` after the vertical scroll. Every Home render,
// patch, shift and dirty-rect site uses this so the scroll math lives in one place.
inline int homeRowTopY(int focusRow, int hubCount, int row) {
    return homeRowY0(focusRow, hubCount) + row * kHubRowAdvance();
}
// False for rows scrolled above the hub band: draw/patch/shift paths skip them so they never
// overdraw the header band (drawHome and the dynamic-redraw paths alike).
inline bool homeRowDrawable(int focusRow, int hubCount, int row) {
#ifdef MUT_HOME_PATCH_NO_SKIP
    (void)focusRow; (void)hubCount; (void)row;
    return true;
#else
    const int top = homeRowTopY(focusRow, hubCount, row);
    if (top < kHubTop())
        return false;
    // FONT-2X: rows below the visible band are not drawn either (no title or clipped posters
    // under the hint bar); the vertical scroll brings them up whole.
    if (uiText2x() && hubCount > 0 &&
        top - kHubTop() >= homeVisibleHubRows() * kHubRowAdvance())
        return false;
    return true;
#endif
}
// Grid columns: 5 on the R1 raster; wider canvases fit more posters of the same scaled size.
inline int kGridCols() {
    if (isR1Geom())
        return 5;
    const ScreenGeom& g = screenGeom();
    const float logicalW = static_cast<float>(g.w) / g.s;
    const int cols = static_cast<int>((logicalW - 32.f - 40.f + 16.f) / (94.f + 16.f));
    return std::max(5, std::min(9, cols));
}

// Grid header bands below the top inset: count / A-Z line, then the chip row.
inline int gridCountLineDY() { return XP(30, 48); }
inline int gridChipLineDY() { return XP(14, 36); }
inline int gridCaptionBandH() { return XP(28, 40); }
// Height of the one-line browse hint bar (drawBrowseHint) on the 2x canvases.
inline int gridHintBandH() { return uiText2x() ? XP(20, 40) : 0; }

struct GridLayout {
    int cols = 5;
    int gap = 16;
    int posterW = 0;
    int posterH = 0;
    int x0 = 0;
    int y0 = 0;
    int rowStep = 0;
};

inline GridLayout computeGridLayout(bool categoryRow = false) {
    GridLayout g;
    g.cols = kGridCols();
    g.gap = P(16);
    g.x0 = kSafeMargin();
    const int avail = kScreenW() - kSafeMargin() - kSafeRight();
    g.posterW = (avail - (g.cols - 1) * g.gap) / g.cols;
    g.posterH = g.posterW * 3 / 2;
    const int countY = kSafeTopInset() + gridCountLineDY();
    const int chipY = countY + gridChipLineDY();
    g.y0 = categoryRow ? chipY + kGridChipRowH() + P(6) : countY + gridChipLineDY() + P(12);
    g.rowStep = g.posterH + gridCaptionBandH();  // one clipped caption line under every tile
    if (uiText2x()) {
        // FONT-2X: the poster is the largest 2:3 size that keeps one whole row plus its 2x
        // caption above the bottom inset, at most 1.5x the R1 94 px poster; as many columns
        // as fit (at least 2), the spare width spread into the gaps.
        const int bottom = kScreenH() - std::max(kSafeBottomInset(), gridHintBandH());
        const int maxH = std::max(P(60), bottom - g.y0 - gridCaptionBandH());
        int w = std::min(maxH * 2 / 3, P(141));
        g.cols = std::max(2, (avail + g.gap) / (w + g.gap));
        w = std::min(w, (avail - (g.cols - 1) * g.gap) / g.cols);
        // Very wide gaps (16:9 canvases): one more column if the poster shrinks <= 15%.
        if (avail - g.cols * w > 3 * g.gap * (g.cols - 1)) {
            const int w1 = (avail - g.cols * g.gap) / (g.cols + 1);
            if (w1 * 100 >= w * 85) {
                ++g.cols;
                w = w1;
            }
        }
        g.posterW = w;
        g.posterH = w * 3 / 2;
        if (g.cols > 1)
            g.gap = (avail - g.cols * w) / (g.cols - 1);
        g.rowStep = g.posterH + gridCaptionBandH();
    }
    return g;
}

// True when a decoded poster is empty or a near-white miss (PMS placeholder JPEG).
inline bool artLooksBlank(const DecodedImage& img) {
    if (img.rgba.empty() || img.w <= 0 || img.h <= 0)
        return true;
    const std::size_t n = static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h);
    if (img.rgba.size() < n * 4u)
        return true;
    const std::size_t step = n > 64 ? n / 64 : 1;
    int bright = 0, samples = 0;
    for (std::size_t i = 0; i < n; i += step) {
        const std::uint8_t* p = img.rgba.data() + i * 4;
        const int y = (static_cast<int>(p[0]) * 3 + static_cast<int>(p[1]) * 6 + static_cast<int>(p[2])) / 10;
        if (y >= 230)
            ++bright;
        ++samples;
    }
    return samples > 0 && bright * 10 >= samples * 8;
}

inline float measureCategoryChipWidth(const FontSet& fonts, const std::string& text) {
    if (text.empty())
        return 0.f;
    return fonts.measureText(text, TF(12.f), false) + kGridChipPadX() * 2.f;
}

// Season posters need a second caption line (episode count) under the title.
inline GridLayout seasonGridLayout() {
    GridLayout g = computeGridLayout(false);
    g.rowStep = g.posterH + XP(46, 70);
    if (uiText2x()) {
        // FONT-2X: the second caption line comes out of the poster so the row still clears
        // the bottom inset; same columns, spare width back into the gaps.
        const int avail = kScreenW() - kSafeMargin() - kSafeRight();
        const int w = std::max(P(40), (g.posterH - (XP(46, 70) - gridCaptionBandH())) * 2 / 3);
        g.posterW = std::min(g.posterW, w);
        g.posterH = g.posterW * 3 / 2;
        if (g.cols > 1)
            g.gap = (avail - g.cols * g.posterW) / (g.cols - 1);
        g.rowStep = g.posterH + XP(46, 70);
    }
    return g;
}

inline int episodeRowH() { return XP(52, 68); }
inline int episodeListTop() { return kSafeTopInset() + XP(36, 56); }
inline int episodeListBottom() { return kScreenH() - kSafeBottomInset() - XP(64, 84); }
inline int episodeVisibleRows() {
    const int span = episodeListBottom() - episodeListTop();
    const int n = span / episodeRowH();
    return n > 1 ? n : 1;
}

inline int gridVisibleRowCount(const GridLayout& g) {
    int rows = 0;
    for (;;) {
        const int py = g.y0 + rows * g.rowStep;
        if (py + g.posterH > kScreenH() - kSafeBottomInset())
            break;
        ++rows;
    }
    return rows > 0 ? rows : 1;
}
constexpr std::uint8_t kAccentR = 0xE5;
constexpr std::uint8_t kAccentG = 0xA0;
constexpr std::uint8_t kAccentB = 0x0D;

struct RenderState {
    ViewId view = ViewId::ServerList;
    std::vector<PlexServer> servers;
    int serverIndex = 0;
    PlexServer server{};
    bool haveServer = false;
    std::vector<PlexSection> sections;
    std::vector<SidebarLibraryEntry> sidebarEntries;
    int sidebarScrollY = 0;
    std::vector<PlexHubRow> hubs;
    std::unordered_map<std::string, int> hubItemServerIndex;
    std::vector<PlexItem> grid;
    PlexItem detail;
    UiFocus focus{};
    int focusAnim = 3;
    std::string status;
    std::string queueLine;
    std::string backdropUrl;
    ImageCacheKey backdropKey{};
    bool backdropKeyValid = false;
    std::string gridSectionTitle;
    std::string gridFilterChipTitle;
    int gridItemCount = 0;
    std::vector<GridCategoryChip> gridChips;
    int gridActiveChip = 0;
    int gridChipScrollX = 0;
    int gridScrollRow = 0;
    // AZ_JUMP applied filter. 0 keeps the section title line as it is.
    char gridLetterFilter = 0;
    // FILTER_4_3 toggle. False while the flag is off, so the title line is unchanged.
    bool filter43On = false;
    bool detailBackdrop = false;
    uint64_t dataGeneration = 0;
    std::vector<PlexItem> seasons;
    std::vector<PlexItem> episodes;
    // Matches the controller's list generation. Paint skips the season and episode copy when it matches.
    uint64_t listSnap = 0;
    int seasonScrollRow = 0;
    int episodeScrollRow = 0;
    std::string listTitle;
    PlexItem upNext;
    int upNextSeconds = 0;
    std::string linkCode;
    int linkSeconds = 0;
    std::vector<SidebarLibraryEntry> homeRows;
    std::string homePin;
    int homePinSlot = 0;
    std::string homePinTitle;
};

// The render target's pixel format: R1 idle FB (RGB24) or the OB overlay page (fbdev 8888:
// bytes B, G, R, A; opaque).
enum class PixelFormat : std::uint8_t { RGB24, BGRA8888 };
inline int bytesPerPixel(PixelFormat f) { return f == PixelFormat::RGB24 ? 3 : 4; }

class Renderer {
public:
    bool initFonts(const std::string& fontDir, std::string& err);

    void setImageCache(ImageCache* cache) { cache_ = cache; }

    void markStaticDirty() { staticValid_ = false; }
    // #219: called from the ImageCache worker threads while copyFrame renders on the paint
    // thread. Thread-safe: it only queues the key; render() applies it on the render thread.
    void markImageReady(const ImageCacheKey& key);

    struct RenderPerf {
        long long renderMs = 0;
        long long staticMs = 0;
        long long blitMs = 0;
        long long focusMs = 0;
        int rebuild = 0;
        std::string reason;
    };
    // Renders at screenGeom() (kScreenW() x kScreenH()), tightly packed, in `fmt`.
    void render(const RenderState& st, std::vector<std::uint8_t>& out, RenderPerf* perf = nullptr,
                PixelFormat fmt = PixelFormat::RGB24);

    FontSet& fonts() { return fonts_; }

private:
    void ensureScratch();
    void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void paintBackdrop(const ImageCacheKey& key, bool keyValid, bool detailView,
                       std::uint8_t* rgbaTarget = nullptr);
    void restoreBackdropRect(int x0, int y0, int x1, int y1);
    void patchStaticPoster(const RenderState& st, int row, int col, bool focused);
    void syncStaticAfterFocusMove(const RenderState& st, int prevRow, int prevCol, int prevGridScrollRow,
                                  int prevHubScrollX);
    void shiftStaticGridRows(const RenderState& st, int deltaRows);
    void shiftStaticHomeColumns(const RenderState& st, int hubRow, int deltaPx);
    void drawPoster(int x, int y, int w, int h, const ImageCacheKey& imgKey, const std::string& url,
                    const std::string& title, bool focused, int animFrame,
                    std::uint8_t* target = nullptr, int viewCount = 0, int64_t viewOffsetMs = 0);
    void drawHubItemDecor(int x, int y, int w, int h, const PlexItem& it, bool focused,
                          int animFrame, std::uint8_t* target);
    void drawHubItemPoster(int x, int y, int w, int h, const ImageCacheKey& imgKey,
                           const std::string& url, const PlexItem& it, bool focused, int animFrame,
                           std::uint8_t* target = nullptr);
    void drawPosterScaled(int x, int y, int w, int h, const DecodedImage& prescaled,
                          const std::string& title, bool haveImg, int animFrame,
                          std::uint8_t* target = nullptr, int viewCount = 0, int64_t viewOffsetMs = 0);
    void blitPosterRgba(int x, int y, int w, int h, const std::uint8_t* src, int srcW, int srcH,
                        std::uint8_t* target);
    void drawPlexLogo();
    void drawChip(float x, float y, const std::string& text);
    void drawCategoryChip(float x, float y, const std::string& text, bool focused, bool selected);
    void drawPlayPill(int x, int y, int w, int h, const std::string& label, bool focused);
    void drawServerList(const RenderState& st);
    void drawSidebar(const RenderState& st);
    void patchSidebar(const RenderState& st);
    void patchChipRow(const RenderState& st);
    void paintUnfocusedHubPoster(const RenderState& st, int row, int col);
    void drawHome(const RenderState& st, bool omitFocusedPoster = false);
    void drawGrid(const RenderState& st, bool omitFocusedPoster = false);
    void drawDetail(const RenderState& st);
    void drawSeasonList(const RenderState& st);
    void drawEpisodeList(const RenderState& st);
    void drawUpNext(const RenderState& st);
    void drawLinkSignIn(const RenderState& st);
    void drawHomeUsers(const RenderState& st);
    void drawHomePin(const RenderState& st);
    void drawSubtitlePicker(const RenderState& st);
    void drawAudioPicker(const RenderState& st);
    void drawBrowseHint(const std::string& queueLine);
    void buildStaticLayer(const RenderState& st);
    std::string staticLayerKey(const RenderState& st) const;
    void copyRgbaRect(const std::uint8_t* src, std::uint8_t* dst, int x0, int y0, int x1,
                      int y1) const;
    void ensurePosterPrescale(const ImageCacheKey& key, int w, int h);
    const std::vector<std::uint8_t>& cornerMask(int w, int h);

    FontSet fonts_;
    ImageCache* cache_ = nullptr;
    std::vector<std::uint8_t> rgba_;
    std::vector<std::uint8_t> backdropRgba_;
    std::vector<std::uint8_t> staticRgba_;
    std::vector<std::uint8_t> blurTmp_;
    bool staticValid_ = false;
    std::string staticKey_;
    ImageCacheKey prescaleKey_;
    int prescaleW_ = 0;
    int prescaleH_ = 0;
    DecodedImage prescaleImg_;
    struct MaskKey {
        int w = 0;
        int h = 0;
        bool operator==(const MaskKey& o) const { return w == o.w && h == o.h; }
    };
    struct MaskKeyHash {
        std::size_t operator()(const MaskKey& k) const {
            return (static_cast<std::size_t>(k.w) << 16) |
                   static_cast<std::size_t>(static_cast<unsigned>(k.h));
        }
    };
    std::unordered_map<MaskKey, std::vector<std::uint8_t>, MaskKeyHash> cornerMasks_;
    int lastFocusAnim_ = 3;
    int lastFocusRow_ = -1;
    int lastFocusCol_ = -1;
    int lastFocusSidebar_ = -1;
    int lastSidebarScrollY_ = 0;
    bool lastGridOnChips_ = false;
    int lastGridChip_ = -1;
    bool lastGridOnLetters_ = false;
    int lastGridLetter_ = 0;
    ViewId lastView_ = ViewId::ServerList;
    int lastGridScrollRow_ = 0;
    int lastHubScrollX_ = 0;
    std::string lastBackdropUrl_;
    std::vector<ImageCacheKey> dirtyImageKeys_;
    // #219: arrivals from the image workers, drained by render() (readyMu_ guards only this).
    std::mutex readyMu_;
    std::vector<ImageCacheKey> readyInbox_;
    void applyImageReady(const ImageCacheKey& key);   // render thread only
    void drainImageReady();                            // render thread only
    // View-layer cache: the static layer of the last views left (Home/grid/detail), so Back restores
    // instead of a 100-220 ms rebuild. pending = images that arrived while away (patched on restore).
    struct LayerSnap {
        std::string key, backdropUrl;
        std::vector<std::uint8_t> stat, back;
        int gridScrollRow = 0, hubScrollX = 0, fRow = -1, fCol = -1, fSide = -1, gChip = -1;
        bool gOnChips = false;
        bool gOnLetters = false;
        int gLetter = 0;
        ViewId view = ViewId::ServerList;
        std::vector<ImageCacheKey> pending;
    };
    std::vector<LayerSnap> snaps_;   // newest last, at most kLayerSnaps
    static constexpr std::size_t kLayerSnaps = 2;
    bool layerSaveAndRestore(const std::string& key);
    std::size_t layerCapBytes_ = 0;   // 0012: 0 = count cap only (kLayerSnaps)
    void trimLayerSnaps();
public:
    // 0012: the view-layer cache tier (full-frame stat + backdrop copies per snap), for the global
    // PlexUI byte budget, and a byte cap on it (oldest snaps dropped first).
    std::size_t layerCacheBytes() const;
    void setLayerCacheCap(std::size_t bytes);
private:
    int geomW_ = 0;   // the geometry the scratch / static layer / masks were built for
    int geomH_ = 0;
};

} // namespace plexui
} // namespace misterplex
