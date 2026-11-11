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
    const int floorY = kScreenH() - bottom - P(16);
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
inline int kSidebarW() { return is240pGeom() ? 128 : P(200) + kSidebarShiftX(); }
// Library-grid page title / item-count x. Full-bleed when the drawer is closed;
// when the rail is open, sit to the right of kSidebarW so "MiSTerPlex" and the
// library title do not share kSafeMargin() (glass-1026).
inline int kGridTitleX(bool drawerOpen) {
    return kSafeMargin() + (drawerOpen ? kSidebarW() : 0);
}
inline int kSidebarRowH() { return P(44); }
inline int kSidebarIconSize() { return P(22); }
inline int kTextRightInset() { return P(8); }
inline int kTextMaxX() { return kScreenW() - kSafeRight() - kTextRightInset(); }
inline int kGridChipRowH() { return P(30); }
inline float kGridChipPadX() { return PF(10.f); }
inline float kGridChipH() { return PF(22.f); }
inline float kGridChipGap() { return PF(8.f); }
// Home hub rows (one place for the 96 / 144 / 12 / 64 / 52 / 26 render and controller repeated).
inline int kHubPosterW() { return P(96); }
inline int kHubPosterH() { return P(144); }
inline int kHubGap() { return P(12); }
inline int kHubTop() {
    const int legacy = P(64);
    if (overscanEdges().top < 0)
        return legacy;
    return std::max(legacy, overscanInsetPx(kScreenH(), overscanEdges().top));
}
inline int kHubRowAdvance() { return kHubPosterH() + P(52); }
inline int kHubPosterOffY() { return P(26); }
// #206 glass: Home had no vertical scroll. Rows past the screen (row 2+ on  lab) were never drawn,
// so focus moved off screen. The first drawn hub row keeps the focused row fully on screen.
inline int homeVisibleHubRows() {
    int n = 0;
    while (kHubTop() + n * kHubRowAdvance() + kHubPosterOffY() + kHubPosterH() <= kScreenH())
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
    return homeRowTopY(focusRow, hubCount, row) >= kHubTop();
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
    const int countY = kSafeTopInset() + P(30);
    const int chipY = countY + P(14);
    g.y0 = categoryRow ? chipY + kGridChipRowH() + P(6) : countY + P(14) + P(12);
    g.rowStep = g.posterH + P(28);  // one clipped caption line under every tile
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
    return fonts.measureText(text, PF(12.f), false) + kGridChipPadX() * 2.f;
}

// Season posters need a second caption line (episode count) under the title.
inline GridLayout seasonGridLayout() {
    GridLayout g = computeGridLayout(false);
    g.rowStep = g.posterH + P(46);
    return g;
}

inline int episodeRowH() { return P(52); }
inline int episodeListTop() { return kSafeTopInset() + P(36); }
inline int episodeListBottom() { return kScreenH() - kSafeBottomInset() - P(64); }
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
    void drawSidebarIcon(int x, int y, const std::string& sectionType, bool focused);
    void drawHouseIcon(int x, int y, bool focused);
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
