#pragma once

#include "plexui_font.hpp"
#include "plexui_images.hpp"
#include "plexui_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
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
inline float uiScale() { return screenGeom().s; }
inline int P(int v) { return static_cast<int>(std::lround(static_cast<float>(v) * screenGeom().s)); }
inline float PF(float v) { return v * screenGeom().s; }

inline int kScreenW() { return screenGeom().w; }
inline int kScreenH() { return screenGeom().h; }
inline int kSafeMargin() { return P(32); }
// a lab MiSTer HDMI path scales the 640 raster ~1.2x and crops ~40 px on the right (glass 10-18): extra
// right inset. The OB overlay is drawn 1:1 on the raster: a plain scaled margin is enough there.
inline int kSafeRight() { return isR1Geom() ? 72 : P(40); }
inline int kSidebarW() { return P(200); }
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
inline int kHubTop() { return P(64); }
inline int kHubRowAdvance() { return kHubPosterH() + P(52); }
inline int kHubPosterOffY() { return P(26); }
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
    const int countY = kSafeMargin() + P(30);
    const int chipY = countY + P(14);
    g.y0 = categoryRow ? chipY + kGridChipRowH() + P(6) : countY + P(14) + P(12);
    g.rowStep = g.posterH + P(34);  // room for the focused title under a scaled poster
    return g;
}

inline float measureCategoryChipWidth(const FontSet& fonts, const std::string& text) {
    if (text.empty())
        return 0.f;
    return fonts.measureText(text, PF(12.f), false) + kGridChipPadX() * 2.f;
}

inline int gridVisibleRowCount(const GridLayout& g) {
    int rows = 0;
    for (;;) {
        const int py = g.y0 + rows * g.rowStep;
        if (py + g.posterH > kScreenH() - kSafeMargin())
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
    bool detailBackdrop = false;
    uint64_t dataGeneration = 0;
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
                    std::uint8_t* target = nullptr);
    void drawPosterScaled(int x, int y, int w, int h, const DecodedImage& prescaled,
                          const std::string& title, bool haveImg, int animFrame,
                          std::uint8_t* target = nullptr);
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
    void paintUnfocusedHubPoster(const RenderState& st, int row, int col);
    void drawHome(const RenderState& st, bool omitFocusedPoster = false);
    void drawGrid(const RenderState& st, bool omitFocusedPoster = false);
    void drawDetail(const RenderState& st);
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
    ViewId lastView_ = ViewId::ServerList;
    int lastGridScrollRow_ = 0;
    int lastHubScrollX_ = 0;
    std::string lastBackdropUrl_;
    std::vector<ImageCacheKey> dirtyImageKeys_;
    // View-layer cache: the static layer of the last views left (Home/grid/detail), so Back restores
    // instead of a 100-220 ms rebuild. pending = images that arrived while away (patched on restore).
    struct LayerSnap {
        std::string key, backdropUrl;
        std::vector<std::uint8_t> stat, back;
        int gridScrollRow = 0, hubScrollX = 0, fRow = -1, fCol = -1, fSide = -1, gChip = -1;
        bool gOnChips = false;
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
