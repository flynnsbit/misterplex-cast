#include "plexui_render.hpp"

#include "plexui_data.hpp"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb_image_resize2.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace misterplex {
namespace plexui {

namespace {

void blendPx(std::uint8_t* dst, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb,
             std::uint8_t sa) {
    const int inv = 255 - sa;
    dst[0] = static_cast<std::uint8_t>((sr * sa + dst[0] * inv) / 255);
    dst[1] = static_cast<std::uint8_t>((sg * sa + dst[1] * inv) / 255);
    dst[2] = static_cast<std::uint8_t>((sb * sa + dst[2] * inv) / 255);
    dst[3] = static_cast<std::uint8_t>(std::min(255, dst[3] + sa));
}

void boxBlurRgb(std::uint8_t* data, int w, int h, int stride, std::uint8_t* tmp) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= h)
                    continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= w)
                        continue;
                    const std::uint8_t* p = data + (yy * stride + xx * 3);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++n;
                }
            }
            std::uint8_t* o = tmp + (y * stride + x * 3);
            o[0] = static_cast<std::uint8_t>(r / n);
            o[1] = static_cast<std::uint8_t>(g / n);
            o[2] = static_cast<std::uint8_t>(b / n);
        }
    }
    std::memcpy(data, tmp, static_cast<std::size_t>(stride) * h);
}

float easeOut(int frame, int total) {
    const float t = static_cast<float>(frame) / static_cast<float>(std::max(1, total));
    return 1.f - (1.f - t) * (1.f - t);
}

const PlexServer& serverForHubItemRender(const RenderState& st, const PlexItem& it) {
    const auto sit = st.hubItemServerIndex.find(it.ratingKey);
    if (sit != st.hubItemServerIndex.end() && sit->second >= 0 &&
        sit->second < static_cast<int>(st.servers.size()))
        return st.servers[static_cast<std::size_t>(sit->second)];
    return st.server;
}

ImageCacheKey hubThumbKey(const RenderState& st, const PlexItem& it, int w, int h) {
    const PlexServer& srv = serverForHubItemRender(st, it);
    return thumbCacheKey(srv.machineIdentifier, it.ratingKey, w, h, itemThumbRevision(it));
}

int hubRowScrollX(const RenderState& st, int hubRow, int focusCol) {
    const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
    const int sideW = kSidebarW();
    const int step = posterW + gap;
    const int viewW = kScreenW() - sideW;
    if (hubRow < 0 || hubRow >= static_cast<int>(st.hubs.size()))
        return 0;
    const auto& hub = st.hubs[static_cast<std::size_t>(hubRow)];
    if (hub.items.empty())
        return 0;
    int scrollX = focusCol * step;
    const int totalW = static_cast<int>(hub.items.size()) * step - gap;
    const int maxScroll = std::max(0, totalW - viewW + posterW / 2);
    scrollX = std::min(scrollX, maxScroll);
    const int focusCenter = focusCol * step + posterW / 2;
    if (focusCenter - scrollX > viewW - posterW / 2)
        scrollX = focusCenter - viewW + posterW / 2;
    return std::max(0, scrollX);
}

constexpr int kFocusAnimSteps = 3;
constexpr float kFocusScale = 1.08f;

void posterDirtyRect(int x, int y, int w, int h, int& x0, int& y0, int& x1, int& y1) {
    const int sw = static_cast<int>(w * kFocusScale);
    const int sh = static_cast<int>(h * kFocusScale);
    const int ox = x - (sw - w) / 2;
    const int oy = y - (sh - h) / 2;
    x0 = ox - P(8);
    y0 = oy - P(8);
    x1 = ox + sw + P(8);
    y1 = oy + sh + P(40);
}

bool roundedCornerSkip(int cx, int cy, int w, int h, int radius);

void paintSidebarFocus(std::uint8_t* rgba, int sideW, int rowY, int rowH) {
    const int x = P(8);
    const int y = rowY + P(4);
    const int w = sideW - P(16);
    const int h = rowH - P(8);
    if (w < 4 || h < 4)
        return;
    const int rad = std::min(P(8), h / 2);
    const int thick = std::max(1, P(2));
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if (roundedCornerSkip(col, row, w, h, rad))
                continue;
            const int px = x + col;
            const int py = y + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            const bool edge = row < thick || col < thick || row >= h - thick || col >= w - thick;
            std::uint8_t* dst = rgba + (static_cast<std::size_t>(py) * kScreenW() + px) * 4;
            if (edge)
                blendPx(dst, kAccentR, kAccentG, kAccentB, 230);
            else
                blendPx(dst, 36, 32, 18, 220);
        }
    }
}

ImageCacheKey cachedDetailThumb(ImageCache* cache, const RenderState& st, int posterW, int posterH) {
    ImageCacheKey none{};
    if (!st.haveServer || st.detail.thumb.empty() || st.detail.ratingKey.empty())
        return none;
    const std::string rev = itemThumbRevision(st.detail);
    const std::string& mid = st.server.machineIdentifier;
    ImageCacheKey detail = thumbCacheKey(mid, st.detail.ratingKey, posterW, posterH, rev);
    if (!cache)
        return detail;
    if (cache->get(detail))
        return detail;
    const GridLayout gl = computeGridLayout(true);
    ImageCacheKey grid = thumbCacheKey(mid, st.detail.ratingKey, gl.posterW, gl.posterH, rev);
    if (cache->get(grid))
        return grid;
    ImageCacheKey hub = thumbCacheKey(mid, st.detail.ratingKey, kHubPosterW(), kHubPosterH(), rev);
    if (cache->get(hub))
        return hub;
    return detail;
}

bool roundedCornerSkip(int cx, int cy, int w, int h, int radius) {
    if (cx >= radius && cx < w - radius)
        return false;
    if (cy >= radius && cy < h - radius)
        return false;
    int rcx = cx, rcy = cy;
    if (cx < radius)
        rcx = radius;
    else if (cx >= w - radius)
        rcx = w - radius - 1;
    if (cy < radius)
        rcy = radius;
    else if (cy >= h - radius)
        rcy = h - radius - 1;
    const int dx = cx - rcx;
    const int dy = cy - rcy;
    return dx * dx + dy * dy > radius * radius;
}

} // namespace

ScreenGeom& screenGeom() {
    static ScreenGeom g;
    return g;
}

void setScreenGeom(int w, int h) {
    ScreenGeom& g = screenGeom();
    g.w = std::max(160, std::min(1920, w));
    g.h = std::max(120, std::min(1080, h));
    g.s = static_cast<float>(g.h) / 480.f;
}

bool Renderer::initFonts(const std::string& fontDir, std::string& err) {
    return fonts_.load(fontDir, err);
}

void Renderer::ensureScratch() {
    if (geomW_ != kScreenW() || geomH_ != kScreenH()) {   // OB port: the canvas changed size
        geomW_ = kScreenW();
        geomH_ = kScreenH();
        staticValid_ = false;
        staticKey_.clear();
        cornerMasks_.clear();
        prescaleKey_ = {};
        lastBackdropUrl_.clear();
        lastView_ = ViewId::ServerList;
        lastFocusRow_ = lastFocusCol_ = lastFocusSidebar_ = lastGridChip_ = -1;
        lastSidebarScrollY_ = 0;
        rgba_.clear();
        staticRgba_.clear();
        backdropRgba_.clear();
        snaps_.clear();
    }
    const std::size_t n = static_cast<std::size_t>(kScreenW()) * kScreenH() * 4;
    if (rgba_.size() != n)
        rgba_.assign(n, 0);
    if (staticRgba_.size() != n)
        staticRgba_.assign(n, 0);
    if (backdropRgba_.size() != n)
        backdropRgba_.assign(n, 0);
    blurTmp_.resize(static_cast<std::size_t>(kScreenW()) * kScreenH() * 3);
}

void Renderer::markImageReady(const ImageCacheKey& key) {
    if (key.ratingKey.empty())
        return;
    for (const auto& k : dirtyImageKeys_) {
        if (k == key)
            return;
    }
    dirtyImageKeys_.push_back(key);
    for (auto& sn : snaps_)
        if (std::find(sn.pending.begin(), sn.pending.end(), key) == sn.pending.end())
            sn.pending.push_back(key);
}

std::size_t Renderer::layerCacheBytes() const {
    std::size_t b = 0;
    for (const auto& sn : snaps_)
        b += sn.stat.capacity() + sn.back.capacity();
    return b;
}

void Renderer::setLayerCacheCap(std::size_t bytes) {
    layerCapBytes_ = bytes;
    trimLayerSnaps();
}

void Renderer::trimLayerSnaps() {
    while (layerCapBytes_ > 0 && !snaps_.empty() && layerCacheBytes() > layerCapBytes_)
        snaps_.erase(snaps_.begin());
}

// The static key changes: keep the layer being left; restore the new key's layer when cached.
bool Renderer::layerSaveAndRestore(const std::string& key) {
    if (!staticValid_ || staticKey_.empty() || staticKey_ == key)
        return false;
    // Home and grid only: a detail page is per item (rarely returned to) and would evict Home.
    if (lastView_ == ViewId::Home || lastView_ == ViewId::LibraryGrid) {
        LayerSnap sn;
        sn.key = staticKey_; sn.backdropUrl = lastBackdropUrl_;
        sn.stat = staticRgba_; sn.back = backdropRgba_;
        sn.gridScrollRow = lastGridScrollRow_; sn.hubScrollX = lastHubScrollX_;
        sn.fRow = lastFocusRow_; sn.fCol = lastFocusCol_; sn.fSide = lastFocusSidebar_;
        sn.gChip = lastGridChip_; sn.gOnChips = lastGridOnChips_; sn.view = lastView_;
        sn.pending = dirtyImageKeys_;   // not yet patched into this layer
        snaps_.erase(std::remove_if(snaps_.begin(), snaps_.end(),
                                    [&](const LayerSnap& o) { return o.key == sn.key; }), snaps_.end());
        snaps_.push_back(std::move(sn));
        if (snaps_.size() > kLayerSnaps)
            snaps_.erase(snaps_.begin());
        trimLayerSnaps();
    }
    for (auto it = snaps_.begin(); it != snaps_.end(); ++it) {
        if (it->key != key || it->stat.size() != staticRgba_.size())
            continue;
        LayerSnap sn = std::move(*it);
        snaps_.erase(it);
        staticRgba_ = std::move(sn.stat); backdropRgba_ = std::move(sn.back);
        rgba_.assign(staticRgba_.begin(), staticRgba_.end());
        staticKey_ = key; lastBackdropUrl_ = sn.backdropUrl;
        lastGridScrollRow_ = sn.gridScrollRow; lastHubScrollX_ = sn.hubScrollX;
        lastFocusRow_ = sn.fRow; lastFocusCol_ = sn.fCol; lastFocusSidebar_ = sn.fSide;
        lastGridChip_ = sn.gChip; lastGridOnChips_ = sn.gOnChips; lastView_ = sn.view;
        for (const auto& k : sn.pending)
            markImageReady(k);
        prescaleKey_ = {};
        return true;
    }
    return false;
}

void Renderer::restoreBackdropRect(int x0, int y0, int x1, int y1) {
    if (backdropRgba_.empty())
        return;
    copyRgbaRect(backdropRgba_.data(), staticRgba_.data(), x0, y0, x1, y1);
    copyRgbaRect(backdropRgba_.data(), rgba_.data(), x0, y0, x1, y1);
}

void Renderer::blitPosterRgba(int x, int y, int w, int h, const std::uint8_t* src, int srcW,
                                int srcH, std::uint8_t* target) {
    if (!src || srcW != w || srcH != h)
        return;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const std::uint8_t* s = src + (static_cast<std::size_t>(row) * w + col) * 4;
            const int px = x + col;
            const int py = y + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (s[3] == 0)
                continue;
            blendPx(target + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, s[0], s[1], s[2],
                    s[3]);
        }
    }
}

void Renderer::copyRgbaRect(const std::uint8_t* src, std::uint8_t* dst, int x0, int y0, int x1,
                            int y1) const {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(kScreenW(), x1);
    y1 = std::min(kScreenH(), y1);
    for (int y = y0; y < y1; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * kScreenW() * 4;
        std::memcpy(dst + row + static_cast<std::size_t>(x0) * 4,
                    src + row + static_cast<std::size_t>(x0) * 4,
                    static_cast<std::size_t>(x1 - x0) * 4);
    }
}

const std::vector<std::uint8_t>& Renderer::cornerMask(int w, int h) {
    const MaskKey key{w, h};
    const auto it = cornerMasks_.find(key);
    if (it != cornerMasks_.end())
        return it->second;
    const int radius = P(8);
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(w) * h, 255);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if (roundedCornerSkip(col, row, w, h, radius))
                mask[static_cast<std::size_t>(row) * w + col] = 0;
        }
    }
    cornerMasks_[key] = std::move(mask);
    return cornerMasks_[key];
}

void Renderer::ensurePosterPrescale(const ImageCacheKey& key, int w, int h) {
    const int sw = static_cast<int>(w * kFocusScale);
    const int sh = static_cast<int>(h * kFocusScale);
    if (key == prescaleKey_ && prescaleW_ == sw && prescaleH_ == sh && !prescaleImg_.rgba.empty())
        return;
    prescaleKey_ = key;
    prescaleW_ = sw;
    prescaleH_ = sh;
    prescaleImg_.w = 0;
    prescaleImg_.h = 0;
    prescaleImg_.rgba.clear();
    if (!cache_ || key.ratingKey.empty())
        return;
    const auto src = cache_->get(key);
    if (!src || src->w < 1 || src->h < 1)
        return;
    if (src->focusW == sw && src->focusH == sh && !src->focusRgba.empty()) {
        prescaleImg_.w = sw;
        prescaleImg_.h = sh;
        prescaleImg_.rgba = src->focusRgba;
        return;
    }
    prescaleImg_.w = sw;
    prescaleImg_.h = sh;
    prescaleImg_.rgba.resize(static_cast<std::size_t>(sw) * sh * 4);
    stbir_resize_uint8_linear(src->rgba.data(), src->w, src->h, 0, prescaleImg_.rgba.data(), sw, sh,
                              0, STBIR_RGBA);
}

void Renderer::clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (int y = 0; y < kScreenH(); ++y) {
        for (int x = 0; x < kScreenW(); ++x) {
            std::uint8_t* p = rgba_.data() + (static_cast<std::size_t>(y) * kScreenW() + x) * 4;
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    }
}

void Renderer::paintBackdrop(const ImageCacheKey& key, bool keyValid, bool detailView,
                             std::uint8_t* rgbaTarget) {
    std::uint8_t* out = rgbaTarget ? rgbaTarget : rgba_.data();
    for (int y = 0; y < kScreenH(); ++y) {
        for (int x = 0; x < kScreenW(); ++x) {
            std::uint8_t* p = out + (static_cast<std::size_t>(y) * kScreenW() + x) * 4;
            p[0] = 0x08;
            p[1] = 0x08;
            p[2] = 0x0a;
            p[3] = 255;
        }
    }
    if (!cache_ || !keyValid) {
        if (!rgbaTarget)
            backdropRgba_.assign(out, out + static_cast<std::size_t>(kScreenW()) * kScreenH() * 4);
        return;
    }
    const auto img = cache_->get(key);
    if (!img || img->w < 4 || img->h < 4)
        return;
    if (!detailView && !img->backdropRgba.empty() &&
        img->backdropRgba.size() == static_cast<std::size_t>(kScreenW()) * kScreenH() * 4) {
        std::memcpy(out, img->backdropRgba.data(), img->backdropRgba.size());
        if (!rgbaTarget)
            backdropRgba_.assign(out, out + static_cast<std::size_t>(kScreenW()) * kScreenH() * 4);
        return;
    }
    constexpr int bw = 160;
    constexpr int bh = 90;
    std::vector<std::uint8_t> small(static_cast<std::size_t>(bw) * bh * 3);
    stbir_resize_uint8_linear(img->rgba.data(), img->w, img->h, 0, small.data(), bw, bh, 0,
                              STBIR_RGB);
    const int blurPasses = detailView ? 4 : 3;
    for (int pass = 0; pass < blurPasses; ++pass)
        boxBlurRgb(small.data(), bw, bh, bw * 3, blurTmp_.data());
    std::vector<std::uint8_t> full(static_cast<std::size_t>(kScreenW()) * kScreenH() * 3);
    stbir_resize_uint8_linear(small.data(), bw, bh, 0, full.data(), kScreenW(), kScreenH(), 0,
                              STBIR_RGB);
    for (int y = 0; y < kScreenH(); ++y) {
        const float vGrad = static_cast<float>(y) / static_cast<float>(kScreenH());
        const float bottomDark = 0.25f + 0.75f * vGrad;
        for (int x = 0; x < kScreenW(); ++x) {
            const std::uint8_t* s = full.data() + (static_cast<std::size_t>(y) * kScreenW() + x) * 3;
            std::uint8_t* d = out + (static_cast<std::size_t>(y) * kScreenW() + x) * 4;
            float dim = 0.62f * bottomDark;
            if (detailView && x < P(280)) {
                const float t = static_cast<float>(x) / PF(280.f);
                dim *= (0.15f + 0.85f * t);
            }
            d[0] = static_cast<std::uint8_t>(s[0] * dim);
            d[1] = static_cast<std::uint8_t>(s[1] * dim);
            d[2] = static_cast<std::uint8_t>(s[2] * dim);
            d[3] = 255;
        }
    }
    if (!rgbaTarget)
        backdropRgba_.assign(out, out + static_cast<std::size_t>(kScreenW()) * kScreenH() * 4);
}

void Renderer::drawPlexLogo() {
    fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), kSafeMargin(), kSafeMargin() - P(2),
                    "MiSTerPlex", PF(20.f), true, 255, 255, 255);
    const int cx = kSafeMargin() + P(2);
    const int cy = kSafeMargin() + P(10);
    for (int dy = -P(6); dy <= P(6); ++dy) {
        for (int dx = -P(2); dx <= P(8); ++dx) {
            const int px = cx + dx;
            const int py = cy + dy;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (dx >= 0 && dx < P(7) && std::abs(dy) <= P(6) - dx)
                blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4,
                        kAccentR, kAccentG, kAccentB, 230);
        }
    }
}

void Renderer::drawChip(float x, float y, const std::string& text) {
    drawCategoryChip(x, y, text, false, false);
}

void Renderer::drawCategoryChip(float x, float y, const std::string& text, bool focused,
                                bool selected) {
    if (text.empty())
        return;
    const float padX = kGridChipPadX();
    const float h = kGridChipH();
    const float w = fonts_.measureText(text, PF(12.), false) + padX * 2.f;
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);
    const int iw = static_cast<int>(w);
    const int ih = static_cast<int>(h);
    const std::uint8_t br = focused ? kAccentR : 255;
    const std::uint8_t bg = focused ? kAccentG : 255;
    const std::uint8_t bb = focused ? kAccentB : 255;
    const int fillA = focused ? 255 : 34;
    for (int row = 0; row < ih; ++row) {
        for (int col = 0; col < iw; ++col) {
            const int px = ix + col;
            const int py = iy + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (roundedCornerSkip(col, row, iw, ih, P(8)))
                continue;
            blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, br, bg, bb,
                    fillA);
        }
    }
    if (selected && !focused) {
        for (int row = 0; row < ih; ++row) {
            for (int col = 0; col < iw; ++col) {
                const int px = ix + col;
                const int py = iy + row;
                if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                    continue;
                const bool edge = col == 0 || col == iw - 1 || row == 0 || row == ih - 1;
                if (!edge)
                    continue;
                if (roundedCornerSkip(col, row, iw, ih, P(8)) &&
                    !(col == 0 || col == iw - 1 || row == 0 || row == ih - 1))
                    continue;
                blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, 255, 255,
                        255, 200);
            }
        }
    }
    const std::uint8_t tr = focused ? 0 : 230;
    const std::uint8_t tg = focused ? 0 : 230;
    const std::uint8_t tb = focused ? 0 : 230;
    fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), x + padX, y + PF(4.f), text, PF(12.f),
                    focused, tr, tg, tb);
}

void Renderer::drawPlayPill(int x, int y, int w, int h, const std::string& label, bool focused) {
    const std::uint8_t br = focused ? kAccentR : static_cast<std::uint8_t>(kAccentR * 0.85f);
    const std::uint8_t bg = focused ? kAccentG : static_cast<std::uint8_t>(kAccentG * 0.85f);
    const std::uint8_t bb = focused ? kAccentB : static_cast<std::uint8_t>(kAccentB * 0.85f);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const int px = x + col;
            const int py = y + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (roundedCornerSkip(col, row, w, h, h / 2))
                continue;
            blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, br, bg, bb,
                    255);
        }
    }
    const float tw = fonts_.measureText(label, PF(14.), true);
    fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(),
                    static_cast<float>(x) + (w - tw) / 2.f + PF(8.f), static_cast<float>(y) + PF(10.f),
                    label, PF(14.f), true, 0, 0, 0);
    const int tx = x + P(18);
    const int ty = y + h / 2;
    for (int dy = -P(5); dy <= P(5); ++dy) {
        for (int dx = 0; dx < P(8); ++dx) {
            if (std::abs(dy) > P(5) - dx / 2)
                continue;
            const int px = tx + dx;
            const int py = ty + dy;
            if (px >= 0 && py >= 0 && px < kScreenW() && py < kScreenH())
                blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, 0, 0, 0,
                        255);
        }
    }
}

void Renderer::drawPoster(int x, int y, int w, int h, const ImageCacheKey& imgKey,
                          const std::string& url, const std::string& title, bool focused,
                          int animFrame, std::uint8_t* target) {
    (void)url;
    if (focused && animFrame >= 0) {
        ensurePosterPrescale(imgKey, w, h);
        if (!prescaleImg_.rgba.empty()) {
            drawPosterScaled(x, y, w, h, prescaleImg_, title, true, animFrame, target);
            return;
        }
    }
    const float scale =
        focused ? (1.f + (kFocusScale - 1.f) * easeOut(std::min(animFrame, kFocusAnimSteps),
                                                        kFocusAnimSteps))
                : 1.f;
    const int sw = static_cast<int>(w * scale);
    const int sh = static_cast<int>(h * scale);
    const int ox = x - (sw - w) / 2;
    const int oy = y - (sh - h) / 2;
    std::uint8_t* const pxBase = target ? target : rgba_.data();

    for (int sy = -P(6); sy < sh + P(9); ++sy) {
        for (int sx = -P(6); sx < sw + P(9); ++sx) {
            const int px = ox + sx;
            const int py = oy + sy + P(3);
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (sx >= 0 && sy >= 0 && sx < sw && sy < sh)
                continue;
            const int dist = std::max(std::abs(sx - sw / 2), std::abs(sy - sh / 2));
            if (dist > sw / 2 + P(6))
                continue;
            const int alpha = std::max(0, 80 - static_cast<int>(static_cast<float>((std::abs(sx - sw / 2) + std::abs(sy - sh / 2)) * 3) / uiScale()));
            blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, 0, 0, 0,
                    static_cast<std::uint8_t>(alpha));
        }
    }

    const auto img = cache_ && !imgKey.ratingKey.empty() ? cache_->get(imgKey)
                                                         : std::shared_ptr<const DecodedImage>{};
    const bool have = img && !img->rgba.empty();
    bool paintedFast = false;
    if (have && !focused && scale == 1.f && img->w == w && img->h == h) {
        blitPosterRgba(ox, oy, w, h, img->rgba.data(), img->w, img->h, pxBase);
        paintedFast = true;
    }
    if (!paintedFast) {
    const auto& mask = cornerMask(sw, sh);
    for (int row = 0; row < sh; ++row) {
        for (int col = 0; col < sw; ++col) {
            const int px = ox + col;
            const int py = oy + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (!mask[static_cast<std::size_t>(row) * sw + col])
                continue;
            std::uint8_t r = 0x2b, g = 0x2b, b = 0x2b;
            if (have) {
                const int ix = std::min(img->w - 1, static_cast<int>(static_cast<float>(col) / sw * img->w));
                const int iy = std::min(img->h - 1, static_cast<int>(static_cast<float>(row) / sh * img->h));
                const std::uint8_t* s =
                    img->rgba.data() + (static_cast<std::size_t>(iy) * img->w + ix) * 4;
                r = s[0];
                g = s[1];
                b = s[2];
            } else {
                const float t = sh > 1 ? static_cast<float>(row) / static_cast<float>(sh - 1) : 0.f;
                r = static_cast<std::uint8_t>(0x2b + (0x1a - 0x2b) * t);
                g = r;
                b = r;
            }
            blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, r, g, b, 255);
        }
    }
    }
    if (!have && !title.empty()) {
        std::string line = title;
        const float tw = fonts_.measureText(line, PF(11.), false);
        const float tx = static_cast<float>(ox) + (sw - tw) / 2.f;
        const float ty = static_cast<float>(oy + sh) - PF(22.f);
        fonts_.drawText(pxBase, kScreenW() * 4, kScreenW(), kScreenH(), tx, ty, line, PF(11.f), false, 150, 150,
                        155);
    }
    if (focused) {
        for (int row = -P(2); row < sh + P(2); ++row) {
            for (int col = -P(2); col < sw + P(2); ++col) {
                const bool edge = row < 0 || col < 0 || row >= sh || col >= sw;
                if (!edge)
                    continue;
                const int px = ox + col;
                const int py = oy + row;
                if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                    continue;
                blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, kAccentR,
                        kAccentG, kAccentB, 220);
            }
        }
        for (int row = -P(5); row < sh + P(5); ++row) {
            for (int col = -P(5); col < sw + P(5); ++col) {
                const bool edge = row < -P(2) || col < -P(2) || row >= sh + P(2) || col >= sw + P(2);
                if (!edge)
                    continue;
                const int px = ox + col;
                const int py = oy + row;
                if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                    continue;
                blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, kAccentR,
                        kAccentG, kAccentB, 35);
            }
        }
    }
}

void Renderer::drawPosterScaled(int x, int y, int w, int h, const DecodedImage& prescaled,
                                const std::string& title, bool haveImg, int animFrame,
                                std::uint8_t* target) {
    const float scale =
        1.f + (kFocusScale - 1.f) * easeOut(std::min(animFrame, kFocusAnimSteps), kFocusAnimSteps);
    const int sw = static_cast<int>(w * scale);
    const int sh = static_cast<int>(h * scale);
    const int ox = x - (sw - w) / 2;
    const int oy = y - (sh - h) / 2;
    std::uint8_t* const pxBase = target ? target : rgba_.data();
    const int imgW = prescaled.w;
    const int imgH = prescaled.h;
    const auto& mask = cornerMask(sw, sh);
    for (int sy = -P(6); sy < sh + P(9); ++sy) {
        for (int sx = -P(6); sx < sw + P(9); ++sx) {
            const int px = ox + sx;
            const int py = oy + sy + P(3);
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (sx >= 0 && sy >= 0 && sx < sw && sy < sh)
                continue;
            const int dist = std::max(std::abs(sx - sw / 2), std::abs(sy - sh / 2));
            if (dist > sw / 2 + P(6))
                continue;
            const int alpha = std::max(0, 80 - static_cast<int>(static_cast<float>((std::abs(sx - sw / 2) + std::abs(sy - sh / 2)) * 3) / uiScale()));
            blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, 0, 0, 0,
                    static_cast<std::uint8_t>(alpha));
        }
    }
    for (int row = 0; row < sh; ++row) {
        for (int col = 0; col < sw; ++col) {
            const int px = ox + col;
            const int py = oy + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            if (!mask[static_cast<std::size_t>(row) * sw + col])
                continue;
            std::uint8_t r = 0x2b, g = 0x2b, b = 0x2b;
            if (haveImg && imgW > 0 && imgH > 0) {
                const int ix =
                    std::min(imgW - 1, static_cast<int>(static_cast<float>(col) / sw * imgW));
                const int iy =
                    std::min(imgH - 1, static_cast<int>(static_cast<float>(row) / sh * imgH));
                const std::uint8_t* s =
                    prescaled.rgba.data() + (static_cast<std::size_t>(iy) * imgW + ix) * 4;
                r = s[0];
                g = s[1];
                b = s[2];
            }
            blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, r, g, b, 255);
        }
    }
    if (!haveImg && !title.empty()) {
        std::string line = title;
        const float tw = fonts_.measureText(line, PF(11.), false);
        const float tx = static_cast<float>(ox) + (sw - tw) / 2.f;
        const float ty = static_cast<float>(oy + sh) - PF(22.f);
        fonts_.drawText(pxBase, kScreenW() * 4, kScreenW(), kScreenH(), tx, ty, line, PF(11.f), false, 150, 150,
                        155);
    }
    for (int row = -P(2); row < sh + P(2); ++row) {
        for (int col = -P(2); col < sw + P(2); ++col) {
            const bool edge = row < 0 || col < 0 || row >= sh || col >= sw;
            if (!edge)
                continue;
            const int px = ox + col;
            const int py = oy + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, kAccentR, kAccentG,
                    kAccentB, 220);
        }
    }
    for (int row = -P(5); row < sh + P(5); ++row) {
        for (int col = -P(5); col < sw + P(5); ++col) {
            const bool edge = row < -P(2) || col < -P(2) || row >= sh + P(2) || col >= sw + P(2);
            if (!edge)
                continue;
            const int px = ox + col;
            const int py = oy + row;
            if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                continue;
            blendPx(pxBase + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, kAccentR, kAccentG,
                    kAccentB, 35);
        }
    }
}

namespace {

void strokeRoundedRectOutline(std::uint8_t* rgba, int stride, int screenW, int screenH, int x, int y,
                              int w, int h, int radius, std::uint8_t r, std::uint8_t g,
                              std::uint8_t b, int thickness = 2) {
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const bool inside = !roundedCornerSkip(col, row, w, h, radius);
            if (!inside)
                continue;
            const bool nearEdge = col < thickness || col >= w - thickness || row < thickness ||
                                  row >= h - thickness;
            if (!nearEdge)
                continue;
            const int px = x + col;
            const int py = y + row;
            if (px < 0 || py < 0 || px >= screenW || py >= screenH)
                continue;
            blendPx(rgba + (static_cast<std::size_t>(py) * screenW + px) * 4, r, g, b, 255);
        }
    }
}

} // namespace

void Renderer::drawHouseIcon(int x, int y, bool focused) {
    const std::uint8_t r = focused ? kAccentR : 0x9a;
    const std::uint8_t g = focused ? kAccentG : 0x9a;
    const std::uint8_t b = focused ? kAccentB : 0x9a;
    const int sz = kSidebarIconSize();
    strokeRoundedRectOutline(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), x, y, sz, sz, P(4), r, g, b,
                             std::max(1, P(1)));
    const int cx = x + sz / 2;
    const int roofY = y + P(5);
    for (int dy = 0; dy < P(8); ++dy) {
        const int halfW = P(7) - dy;
        for (int dx = -halfW; dx <= halfW; ++dx) {
            const int px = cx + dx;
            const int py = roofY + dy;
            if (px >= 0 && py >= 0 && px < kScreenW() && py < kScreenH())
                blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, r, g, b,
                        255);
        }
    }
    const int bodyY = y + P(12);
    strokeRoundedRectOutline(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), x + P(5), bodyY, sz - P(10),
                             sz - P(14), P(3), r, g, b, P(2));
}

void Renderer::drawSidebarIcon(int x, int y, const std::string& sectionType, bool focused) {
    const std::uint8_t r = focused ? kAccentR : 0x9a;
    const std::uint8_t g = focused ? kAccentG : 0x9a;
    const std::uint8_t b = focused ? kAccentB : 0x9a;
    const int sz = kSidebarIconSize();
    const bool movie = sectionType == "movie";
    strokeRoundedRectOutline(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), x, y, sz, sz, P(5), r, g,
                             b, P(2));
    if (movie) {
        for (int i = 0; i < 4; ++i) {
            const int px = x + P(6) + P(i * 3);
            const int py0 = y + P(5);
            const int py1 = y + sz - P(6);
            for (int py = py0; py <= py1; ++py)
                if (px >= 0 && py >= 0 && px < kScreenW() && py < kScreenH())
                    blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, r, g,
                            b, 200);
        }
    } else {
        const int mx = x + P(5);
        const int my = y + P(7);
        const int mw = sz - P(10);
        const int mh = sz - P(12);
        strokeRoundedRectOutline(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), mx, my, mw, mh, P(3),
                                 r, g, b, P(2));
        const int standW = P(8);
        const int standX = x + (sz - standW) / 2;
        const int standY = y + sz - P(5);
        for (int col = 0; col < standW; ++col)
            if (standX + col >= 0 && standY >= 0 && standX + col < kScreenW() && standY < kScreenH())
                blendPx(rgba_.data() + (static_cast<std::size_t>(standY) * kScreenW() + standX + col) *
                                           4,
                        r, g, b, 255);
    }
}

void Renderer::drawServerList(const RenderState& st) {
    drawPlexLogo();
    const int cardW = std::min(P(420), kScreenW() - 2 * kSafeMargin());
    const int cardX = std::max(kSafeMargin(), (kScreenW() - cardW) / 2);
    int y = P(88);
    for (size_t i = 0; i < st.servers.size(); ++i) {
        const bool sel = static_cast<int>(i) == st.focus.row;
        const int cardH = P(56);
        if (sel) {
            for (int row = 0; row < cardH; ++row) {
                for (int col = 0; col < cardW; ++col) {
                    const int px = cardX + col;
                    const int py = y + row;
                    if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                        continue;
                    if (roundedCornerSkip(col, row, cardW, cardH, P(10)))
                        continue;
                    blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4, 30,
                            30, 34, 200);
                }
            }
            for (int row = 0; row < cardH; ++row) {
                for (int col = 0; col < cardW; ++col) {
                    const bool edge = row < P(2) || col < P(2) || row >= cardH - P(2) || col >= cardW - P(2);
                    if (!edge)
                        continue;
                    const int px = cardX + col;
                    const int py = y + row;
                    if (px < 0 || py < 0 || px >= kScreenW() || py >= kScreenH())
                        continue;
                    blendPx(rgba_.data() + (static_cast<std::size_t>(py) * kScreenW() + px) * 4,
                            kAccentR, kAccentG, kAccentB, 200);
                }
            }
        }
        const float nameX = static_cast<float>(std::min(cardX + P(16), kTextMaxX() - P(8)));
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), nameX,
                        static_cast<float>(y + P(10)), st.servers[i].name, PF(18.f), true,
                        st.servers[i].offline ? 110 : 255, st.servers[i].offline ? 110 : 255,
                        st.servers[i].offline ? 115 : 255);   // 0011: greyed when offline
        std::string sub = st.servers[i].owned ? "You" : "Shared";
        sub += " | ";
        sub += serverLinkLabel(st.servers[i]);
        const bool down = st.servers[i].offline;
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), nameX,
                        static_cast<float>(y + P(32)), sub, PF(13.f), false, down ? 210 : 140,
                        down ? 150 : 140, down ? 90 : 145);
        y += cardH + P(12);
    }
    if (!st.status.empty())
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), kSafeMargin(), P(440),
                        st.status, PF(14.f), false, 180, 180, 180);
    else if (!st.servers.empty()) {
        const int idx = std::max(0, std::min(static_cast<int>(st.servers.size()) - 1, st.focus.row));
        if (st.servers[static_cast<std::size_t>(idx)].offline)
            fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), kSafeMargin(), P(440),
                            "This server is offline", PF(14.f), false, 210, 150, 90);
    }
}

void Renderer::drawSidebar(const RenderState& st) {
    const int sideW = kSidebarW();
    for (int y = 0; y < kScreenH(); ++y) {
        for (int x = 0; x < sideW; ++x)
            blendPx(rgba_.data() + (static_cast<std::size_t>(y) * kScreenW() + x) * 4, 0, 0, 0, 153);
    }
    drawPlexLogo();
    const int listTop = P(64);
    const int listBottom = kScreenH() - kSafeMargin();
    const int iconX = P(12);
    const int textX = P(40);
    const int rowH = kSidebarRowH();
    int rowIndex = 0;
    const int homeRowY = listTop - st.sidebarScrollY;
    if (homeRowY + rowH > listTop - P(8) && homeRowY < listBottom) {
        const bool homeSel = st.focus.sidebar == 0;
        if (homeSel)
            paintSidebarFocus(rgba_.data(), sideW, homeRowY, rowH);
        drawHouseIcon(iconX, homeRowY + P(10), homeSel);
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), static_cast<float>(textX),
                        static_cast<float>(homeRowY + P(12)), "Home", PF(16.f), homeSel,
                        homeSel ? kAccentR : 220, homeSel ? kAccentG : 220,
                        homeSel ? kAccentB : 220);
    }
    ++rowIndex;
    for (size_t i = 0; i < st.sidebarEntries.size(); ++i) {
        const int rowY = listTop + rowIndex * rowH - st.sidebarScrollY;
        ++rowIndex;
        if (rowY + rowH < listTop || rowY >= listBottom)
            continue;
        const bool sel = static_cast<int>(i) + 1 == st.focus.sidebar;
        const auto& ent = st.sidebarEntries[i];
        if (sel)
            paintSidebarFocus(rgba_.data(), sideW, rowY, rowH);
        drawSidebarIcon(iconX, rowY + P(10), ent.type, sel);
        const float maxW = static_cast<float>(sideW - textX - P(6));
        const std::string line = fonts_.ellipsize(ent.title, PF(16.f), sel, maxW);
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), static_cast<float>(textX),
                        static_cast<float>(rowY + P(6)), line, PF(16.f), sel, sel ? kAccentR : 220,
                        sel ? kAccentG : 220, sel ? kAccentB : 220);
        const std::string sub = fonts_.ellipsize(ent.subtitle, PF(12.f), false, maxW);
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), static_cast<float>(textX),
                        static_cast<float>(rowY + P(24)), sub, PF(12.f), false, 0x9a, 0x9a, 0x9a);
    }
}

void Renderer::patchSidebar(const RenderState& st) {
    const int sideW = kSidebarW();
    if (sideW <= 0 || staticRgba_.empty() ||
        staticRgba_.size() != static_cast<std::size_t>(kScreenW()) * kScreenH() * 4u)
        return;
    rgba_.assign(staticRgba_.begin(), staticRgba_.end());
    const std::size_t rowBytes = static_cast<std::size_t>(kScreenW()) * 4u;
    const std::size_t sideBytes = static_cast<std::size_t>(sideW) * 4u;
    if (backdropRgba_.size() == rgba_.size()) {
        for (int y = 0; y < kScreenH(); ++y)
            std::memcpy(rgba_.data() + static_cast<std::size_t>(y) * rowBytes,
                        backdropRgba_.data() + static_cast<std::size_t>(y) * rowBytes, sideBytes);
    }
    drawSidebar(st);
    for (int y = 0; y < kScreenH(); ++y)
        std::memcpy(staticRgba_.data() + static_cast<std::size_t>(y) * rowBytes,
                    rgba_.data() + static_cast<std::size_t>(y) * rowBytes, sideBytes);
}

void Renderer::paintUnfocusedHubPoster(const RenderState& st, int row, int col) {
    if (st.view != ViewId::Home || row < 0 || row >= static_cast<int>(st.hubs.size()))
        return;
    const auto& hub = st.hubs[static_cast<std::size_t>(row)];
    if (col < 0 || col >= static_cast<int>(hub.items.size()))
        return;
    const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
    const int sideW = kSidebarW();
    const int step = posterW + gap;
    const int scrollX = hubRowScrollX(st, row, col);
    const int px = sideW + kSafeMargin() - scrollX + col * step;
    const int py = kHubTop() + row * kHubRowAdvance() + kHubPosterOffY();
    ImageCacheKey imgKey{};
    std::string url;
    const auto& it = hub.items[static_cast<std::size_t>(col)];
    if (st.haveServer && !it.thumb.empty()) {
        const PlexServer& thumbSrv = serverForHubItemRender(st, it);
        imgKey = thumbCacheKey(thumbSrv.machineIdentifier, it.ratingKey, posterW, posterH,
                               itemThumbRevision(it));
        url = photoTranscodeUrl(thumbSrv.baseUrl, it.thumb, thumbSrv.accessToken, posterW, posterH);
    }
    drawPoster(px, py, posterW, posterH, imgKey, url, it.title, false, kFocusAnimSteps,
               staticRgba_.data());
}

void Renderer::drawHome(const RenderState& st, bool omitFocusedPoster) {
    drawSidebar(st);

    const int posterW = kHubPosterW();
    const int posterH = kHubPosterH();
    const int gap = kHubGap();
    const int sideW = kSidebarW();
    const int step = posterW + gap;

    int rowY = kHubTop();
    for (size_t ri = 0; ri < st.hubs.size(); ++ri) {
        const auto& hub = st.hubs[ri];
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(),
                        static_cast<float>(sideW + kSafeMargin()), static_cast<float>(rowY), hub.title,
                        PF(18.f), true, 255, 255, 255);
        const int py = rowY + kHubPosterOffY();
        const bool rowFocus = static_cast<int>(ri) == st.focus.row && st.focus.sidebar == 0;
        const int scrollX =
            rowFocus ? hubRowScrollX(st, static_cast<int>(ri), st.focus.col) : 0;
        int px = sideW + kSafeMargin() - scrollX;
        for (size_t ci = 0; ci < hub.items.size(); ++ci) {
            if (px > kScreenW())
                break;
            if (px < sideW + kSafeMargin() - gap) {  // scrolled out to the left: never over the sidebar
                px += step;
                continue;
            }
            const bool foc =
                !omitFocusedPoster && rowFocus && static_cast<int>(ci) == st.focus.col;
            ImageCacheKey imgKey{};
            std::string url;
            if (st.haveServer && !hub.items[ci].thumb.empty()) {
                const PlexServer& thumbSrv = serverForHubItemRender(st, hub.items[ci]);
                imgKey = thumbCacheKey(thumbSrv.machineIdentifier, hub.items[ci].ratingKey, posterW,
                                       posterH, itemThumbRevision(hub.items[ci]));
                url = photoTranscodeUrl(thumbSrv.baseUrl, hub.items[ci].thumb, thumbSrv.accessToken,
                                        posterW, posterH);
            }
            drawPoster(px, py, posterW, posterH, imgKey, url, hub.items[ci].title, foc,
                       st.focusAnim);
            px += step;
        }
        if (!omitFocusedPoster && rowFocus && st.focus.col >= 0 &&
            st.focus.col < static_cast<int>(hub.items.size())) {
            const auto& it = hub.items[static_cast<std::size_t>(st.focus.col)];
            std::string meta = it.title;
            if (!it.year.empty())
                meta += "  " + it.year;
            const float metaW = fonts_.measureText(meta, PF(14.), false);
            const float metaX =
                std::min(static_cast<float>(sideW + kSafeMargin()),
                         static_cast<float>(kTextMaxX()) - metaW);
            fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), metaX,
                            static_cast<float>(py + posterH + P(8)), meta, PF(14.f), false, 230, 230, 230);
        }
        rowY += kHubRowAdvance();
        if (rowY > kScreenH() - P(40))
            break;
    }
}

void Renderer::drawGrid(const RenderState& st, bool omitFocusedPoster) {
    const bool categoryRow = !st.gridChips.empty();
    const GridLayout gl = computeGridLayout(categoryRow);
    const int cols = gl.cols;
    const int posterW = gl.posterW;
    const int posterH = gl.posterH;
    const int gap = gl.gap;
    const int x0 = gl.x0;
    const int y0 = gl.y0;
    const int rowStep = gl.rowStep;
    const std::string sectionName =
        st.gridSectionTitle.empty() ? "Library" : st.gridSectionTitle;
    const std::string chipLabel =
        st.gridFilterChipTitle.empty() ? "All" : st.gridFilterChipTitle;
    const std::string title = sectionName + " \xC2\xB7 " + chipLabel;
    fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), kSafeMargin(), kSafeMargin(), title,
                    PF(28.f), true, 255, 255, 255);
    const std::string count =
        std::to_string(st.gridItemCount > 0 ? st.gridItemCount : static_cast<int>(st.grid.size())) +
        " items";
    fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), kSafeMargin(), kSafeMargin() + P(30),
                    count, PF(14.f), false, 140, 140, 145);
    if (categoryRow) {
        const int chipY = kSafeMargin() + P(30) + P(14);
        float cx = static_cast<float>(kSafeMargin() - st.gridChipScrollX);
        for (size_t i = 0; i < st.gridChips.size(); ++i) {
            const auto& chip = st.gridChips[i];
            const float cw = measureCategoryChipWidth(fonts_, chip.title);
            const int right = static_cast<int>(cx + cw);
            if (right >= kSafeMargin() && static_cast<int>(cx) <= kScreenW() - kSafeRight()) {
                const bool foc = st.focus.gridOnChips && static_cast<int>(i) == st.focus.gridChip;
                const bool sel = static_cast<int>(i) == st.gridActiveChip;
                drawCategoryChip(cx, static_cast<float>(chipY), chip.title, foc, sel && !foc);
            }
            cx += cw + kGridChipGap();
        }
    }
    const PlexItem* focused = nullptr;
    int focPx = 0, focPy = 0;
    const int topRow = st.gridScrollRow;
    for (size_t i = 0; i < st.grid.size(); ++i) {
        const int c = static_cast<int>(i % cols);
        const int r = static_cast<int>(i / cols);
        if (r < topRow)
            continue;
        const int visRow = r - topRow;
        const int px = x0 + c * (posterW + gap);
        const int py = y0 + visRow * rowStep;
        if (py + posterH > kScreenH() - kSafeMargin())
            break;
        const bool foc = !omitFocusedPoster && !st.focus.gridOnChips && r == st.focus.row &&
                         c == st.focus.col;
        ImageCacheKey imgKey{};
        std::string url;
        if (st.haveServer && !st.grid[i].thumb.empty()) {
            imgKey = thumbCacheKey(st.server.machineIdentifier, st.grid[i].ratingKey, posterW,
                                   posterH, itemThumbRevision(st.grid[i]));
            url = photoTranscodeUrl(st.server.baseUrl, st.grid[i].thumb, st.server.accessToken,
                                    posterW, posterH);
        }
        drawPoster(px, py, posterW, posterH, imgKey, url, st.grid[i].title, foc, st.focusAnim);
        if (foc) {
            focused = &st.grid[i];
            focPx = px;
            focPy = py;
        }
    }
    // Focused title in the row gap directly under the focused poster (below its 1.08 scale
    // growth); skipped when the gap would run past the safe area.
    const int ty = focPy + posterH + P(10);
    if (focused && ty + P(16) <= kScreenH() - kSafeMargin() + P(8)) {
        const std::string t = fonts_.ellipsize(focused->title, PF(13.f), false, 2.f * posterW);
        const float tw = fonts_.measureText(t, PF(13.), false);
        float tx = static_cast<float>(focPx) + (posterW - tw) / 2.f;
        tx = std::max(static_cast<float>(kSafeMargin()), std::min(tx, static_cast<float>(kTextMaxX()) - tw));
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), tx,
                        static_cast<float>(ty), t, PF(13.f), false, 230, 230, 230);
    }
}

void Renderer::drawDetail(const RenderState& st) {
    for (int y = 0; y < kScreenH(); ++y) {
        for (int x = 0; x < P(320); ++x) {
            const float t = static_cast<float>(x) / PF(320.f);
            const int a = static_cast<int>(200 * (1.f - t));
            blendPx(rgba_.data() + (static_cast<std::size_t>(y) * kScreenW() + x) * 4, 0, 0, 0, a);
        }
    }
    const int posterW = P(150);
    const int posterH = P(225);
    std::string purl;
    if (st.haveServer && !st.detail.thumb.empty())
        purl = photoTranscodeUrl(st.server.baseUrl, st.detail.thumb, st.server.accessToken,
                                 posterW, posterH);
    const int detailTop = kSafeMargin() + P(24);
    const ImageCacheKey detailKey = cachedDetailThumb(cache_, st, posterW, posterH);
    drawPoster(kSafeMargin(), detailTop, posterW, posterH, detailKey, purl, st.detail.title, false,
               kFocusAnimSteps);

    const float tx = static_cast<float>(kSafeMargin() + posterW + P(20));
    const float textMaxW = static_cast<float>(kTextMaxX()) - tx;
    fonts_.drawTextWrapped(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), tx,
                           static_cast<float>(detailTop), textMaxW, st.detail.title, PF(28.f), true, 255,
                           255, 255, 2);

    const float titleBlockH = fonts_.measureTextWrapped(st.detail.title, PF(28.), true, textMaxW, 2);
    float chipX = tx;
    const float chipY = static_cast<float>(detailTop) + titleBlockH + PF(8.f);
    if (!st.detail.year.empty()) {
        drawChip(chipX, chipY, st.detail.year);
        chipX += fonts_.measureText(st.detail.year, PF(12.), false) + PF(28.f);
    }
    if (st.detail.runtimeMs > 0) {
        const int m = (st.detail.runtimeMs / 60000) % 60;
        const int h = st.detail.runtimeMs / 3600000;
        const std::string rt = std::to_string(h) + "h " + std::to_string(m) + "m";
        drawChip(chipX, chipY, rt);
        chipX += fonts_.measureText(rt, PF(12.), false) + PF(28.f);
    }
    if (!st.detail.contentRating.empty()) {
        drawChip(chipX, chipY, st.detail.contentRating);
        chipX += fonts_.measureText(st.detail.contentRating, PF(12.), false) + PF(28.f);
    }
    const float score = st.detail.audienceRating > 0.f ? st.detail.audienceRating : st.detail.rating;
    if (score > 0.f) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", score);
        drawChip(chipX, chipY, buf);
    }
    float textY = chipY + PF(30.f);
    if (!st.detail.tagline.empty()) {
        fonts_.drawTextWrapped(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), tx, textY, textMaxW,
                               st.detail.tagline, PF(13.f), false, 190, 190, 195, 2);
        textY += fonts_.measureTextWrapped(st.detail.tagline, PF(13.), false, textMaxW, 2) + PF(6.f);
    }
    const float summaryY = textY;
    fonts_.drawTextWrapped(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), tx, summaryY, textMaxW,
                           st.detail.summary, PF(14.f), false, 210, 210, 210, 6);
    const float summaryH =
        fonts_.measureTextWrapped(st.detail.summary, PF(14.), false, textMaxW, 6);
    textY = summaryY + summaryH + PF(8.f);
    if (!st.detail.genres.empty()) {
        fonts_.drawTextWrapped(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), tx, textY, textMaxW,
                               st.detail.genres, PF(13.f), false, 140, 140, 145, 1);
        textY += PF(18.f);
    }
    const int btnX = static_cast<int>(tx);
    const int btnY = static_cast<int>(textY + PF(16.f));
    const bool playFoc = st.focus.detailButton == 0;
    const std::string playLabel = st.detail.viewOffsetMs > 0 ? "Resume" : "Play";
    drawPlayPill(btnX, btnY, P(120), P(36), playLabel, playFoc);
    if (st.detail.viewOffsetMs > 0) {
        const bool rf = st.focus.detailButton == 1;
        fonts_.drawText(rgba_.data(), kScreenW() * 4, kScreenW(), kScreenH(), btnX + P(140),
                        static_cast<float>(btnY + P(10)), "Play from start", PF(14.f), rf, rf ? kAccentR : 200,
                        rf ? kAccentG : 200, rf ? kAccentB : 200);
    }
}

std::string Renderer::staticLayerKey(const RenderState& st) const {
    std::string bd;
    // Home/grid backdrops follow focus; those repaint via backdropUrl patch, not static key.
    if (st.backdropKeyValid && st.view != ViewId::Home && st.view != ViewId::LibraryGrid)
        bd = imageCacheKeyString(st.backdropKey);
    return std::to_string(static_cast<int>(st.view)) + "|" + std::to_string(st.dataGeneration) +
           "|" + bd;
}

namespace {

void logStaticKeyDiff(const std::string& oldKey, const std::string& newKey) {
    if (oldKey == newKey)
        return;
    const auto field = [](const std::string& k, int i) -> std::string {
        std::size_t start = 0;
        for (int f = 0; f < i; ++f) {
            const std::size_t p = k.find('|', start);
            if (p == std::string::npos)
                return {};
            start = p + 1;
        }
        const std::size_t end = k.find('|', start);
        return end == std::string::npos ? k.substr(start) : k.substr(start, end - start);
    };
    static const char* names[] = {"view", "data_gen", "backdrop_key"};
    for (int i = 0; i < 3; ++i) {
        const std::string a = field(oldKey, i);
        const std::string b = field(newKey, i);
        if (a != b)
            std::fprintf(stderr, "plexui: static_key_diff %s=%s->%s\n", names[i], a.c_str(),
                         b.c_str());
    }
}

} // namespace

void Renderer::patchStaticPoster(const RenderState& st, int row, int col, bool focused) {
    if (st.view == ViewId::LibraryGrid && !st.grid.empty()) {
        const GridLayout gl = computeGridLayout(!st.gridChips.empty());
        const int idx = row * gl.cols + col;
        if (idx < 0 || idx >= static_cast<int>(st.grid.size()))
            return;
        const int c = idx % gl.cols;
        const int r = idx / gl.cols;
        const int visRow = r - st.gridScrollRow;
        const int px = gl.x0 + c * (gl.posterW + gl.gap);
        const int py = gl.y0 + visRow * gl.rowStep;
        ImageCacheKey imgKey{};
        std::string url;
        if (st.haveServer && !st.grid[static_cast<std::size_t>(idx)].thumb.empty()) {
            imgKey = thumbCacheKey(st.server.machineIdentifier,
                                   st.grid[static_cast<std::size_t>(idx)].ratingKey, gl.posterW,
                                   gl.posterH,
                                   itemThumbRevision(st.grid[static_cast<std::size_t>(idx)]));
            url = photoTranscodeUrl(st.server.baseUrl, st.grid[static_cast<std::size_t>(idx)].thumb,
                                    st.server.accessToken, gl.posterW, gl.posterH);
        }
        drawPoster(px, py, gl.posterW, gl.posterH, imgKey, url,
                   st.grid[static_cast<std::size_t>(idx)].title, focused, kFocusAnimSteps,
                   staticRgba_.data());
        return;
    }
    if (st.view == ViewId::Home && st.focus.sidebar == 0 && row >= 0 &&
        row < static_cast<int>(st.hubs.size())) {
        const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
        const int sideW = kSidebarW();
        const int step = posterW + gap;
        const auto& hub = st.hubs[static_cast<std::size_t>(row)];
        if (col < 0 || col >= static_cast<int>(hub.items.size()))
            return;
        const int scrollX = hubRowScrollX(st, row, col);
        const int px = sideW + kSafeMargin() - scrollX + col * step;
        const int py = kHubTop() + row * kHubRowAdvance() + kHubPosterOffY();
        ImageCacheKey imgKey{};
        std::string url;
        if (st.haveServer && !hub.items[static_cast<std::size_t>(col)].thumb.empty()) {
            const PlexServer& thumbSrv =
                serverForHubItemRender(st, hub.items[static_cast<std::size_t>(col)]);
            imgKey = thumbCacheKey(thumbSrv.machineIdentifier,
                                   hub.items[static_cast<std::size_t>(col)].ratingKey, posterW,
                                   posterH,
                                   itemThumbRevision(hub.items[static_cast<std::size_t>(col)]));
            url = photoTranscodeUrl(thumbSrv.baseUrl, hub.items[static_cast<std::size_t>(col)].thumb,
                                    thumbSrv.accessToken, posterW, posterH);
        }
        drawPoster(px, py, posterW, posterH, imgKey, url,
                   hub.items[static_cast<std::size_t>(col)].title, focused, kFocusAnimSteps,
                   staticRgba_.data());
    }
}

void Renderer::shiftStaticGridRows(const RenderState& st, int deltaRows) {
    if (deltaRows == 0)
        return;
    const GridLayout gl = computeGridLayout(!st.gridChips.empty());
    const int y0 = gl.y0;
    const int y1 = kScreenH() - kSafeMargin();
    const int h = y1 - y0;
    const int dy = -deltaRows * gl.rowStep;
    if (h <= 0 || std::abs(dy) >= h)
        return;
    const std::size_t rowBytes = static_cast<std::size_t>(kScreenW()) * 4;
    auto shiftBuf = [&](std::uint8_t* buf) {
        if (dy < 0) {
            std::memmove(buf + static_cast<std::size_t>(y0) * rowBytes,
                         buf + static_cast<std::size_t>(y0 - dy) * rowBytes,
                         static_cast<std::size_t>(h + dy) * rowBytes);
        } else {
            std::memmove(buf + static_cast<std::size_t>(y0 + dy) * rowBytes,
                         buf + static_cast<std::size_t>(y0) * rowBytes,
                         static_cast<std::size_t>(h - dy) * rowBytes);
        }
    };
    shiftBuf(staticRgba_.data());
    shiftBuf(rgba_.data());
    const int exposedY0 = dy < 0 ? y1 + dy : y0;
    const int exposedY1 = dy < 0 ? y1 : y0 + dy;
    copyRgbaRect(backdropRgba_.data(), staticRgba_.data(), 0, exposedY0, kScreenW(), exposedY1);
    copyRgbaRect(backdropRgba_.data(), rgba_.data(), 0, exposedY0, kScreenW(), exposedY1);
    RenderState draw = st;
    draw.focusAnim = kFocusAnimSteps;
    const int newRow = deltaRows > 0 ? st.gridScrollRow + gridVisibleRowCount(gl)
                                     : st.gridScrollRow - 1;
    if (newRow >= 0) {
        for (int c = 0; c < gl.cols; ++c) {
            const int idx = newRow * gl.cols + c;
            if (idx >= static_cast<int>(st.grid.size()))
                break;
            patchStaticPoster(draw, newRow, c, false);
        }
    }
    rgba_.assign(staticRgba_.begin(), staticRgba_.end());
}

void Renderer::shiftStaticHomeColumns(const RenderState& st, int hubRow, int deltaPx) {
    if (deltaPx == 0 || hubRow < 0 || hubRow >= static_cast<int>(st.hubs.size()))
        return;
    const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
    int rowY = kHubTop();
    for (int ri = 0; ri < hubRow; ++ri)
        rowY += kHubRowAdvance();
    const int y0 = rowY + kHubPosterOffY() - P(8);
    const int y1 = rowY + kHubPosterOffY() + posterH + P(40);
    const int x0 = kSidebarW() + kSafeMargin() - gap;
    const int x1 = kScreenW();
    const int w = x1 - x0;
    if (w <= 0 || std::abs(deltaPx) >= w)
        return;
    const std::size_t rowBytes = static_cast<std::size_t>(kScreenW()) * 4;
    auto shiftBuf = [&](std::uint8_t* buf) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* row = buf + static_cast<std::size_t>(y) * rowBytes;
            if (deltaPx > 0)
                std::memmove(row + static_cast<std::size_t>(x0 + deltaPx) * 4,
                             row + static_cast<std::size_t>(x0) * 4,
                             static_cast<std::size_t>(w - deltaPx) * 4);
            else
                std::memmove(row + static_cast<std::size_t>(x0) * 4,
                             row + static_cast<std::size_t>(x0 - deltaPx) * 4,
                             static_cast<std::size_t>(w + deltaPx) * 4);
        }
    };
    shiftBuf(staticRgba_.data());
    shiftBuf(rgba_.data());
    const int restoreX0 = deltaPx > 0 ? x0 : x1 + deltaPx;
    const int restoreX1 = deltaPx > 0 ? x0 + deltaPx : x1;
    restoreBackdropRect(restoreX0, y0, restoreX1, y1);
    rgba_.assign(staticRgba_.begin(), staticRgba_.end());
}

void Renderer::syncStaticAfterFocusMove(const RenderState& st, int prevRow, int prevCol,
                                        int prevGridScrollRow, int prevHubScrollX) {
    if (st.view == ViewId::LibraryGrid && !st.focus.gridOnChips) {
        if (prevGridScrollRow != st.gridScrollRow) {
            shiftStaticGridRows(st, st.gridScrollRow - prevGridScrollRow);
            return;
        }
        patchStaticPoster(st, prevRow, prevCol, false);
        const GridLayout gl = computeGridLayout(!st.gridChips.empty());
        const int idx = st.focus.row * gl.cols + st.focus.col;
        if (idx >= 0 && idx < static_cast<int>(st.grid.size())) {
            const int c = idx % gl.cols;
            const int r = idx / gl.cols;
            const int visRow = r - st.gridScrollRow;
            const int px = gl.x0 + c * (gl.posterW + gl.gap);
            const int py = gl.y0 + visRow * gl.rowStep;
            restoreBackdropRect(px - P(4), py - P(4), px + gl.posterW + P(4), py + gl.posterH + P(4));
        }
        rgba_.assign(staticRgba_.begin(), staticRgba_.end());
        return;
    }
    if (st.view == ViewId::Home && st.focus.sidebar == 0) {
        const int hubScrollX =
            st.hubs.empty() ? 0 : hubRowScrollX(st, st.focus.row, st.focus.col);
        if (prevRow == st.focus.row && prevHubScrollX != hubScrollX) {
            shiftStaticHomeColumns(st, st.focus.row, hubScrollX - prevHubScrollX);
            const int step = kHubPosterW() + kHubGap();
            const int sideW = kSidebarW();
            const int px = sideW + kSafeMargin() - hubScrollX + st.focus.col * step;
            int rowY = kHubTop();
            for (int ri = 0; ri < st.focus.row; ++ri)
                rowY += kHubRowAdvance();
            const int py = rowY + kHubPosterOffY();
            const int newCol = hubScrollX > prevHubScrollX ? st.focus.col + 1 : st.focus.col - 1;
            if (newCol >= 0)
                patchStaticPoster(st, st.focus.row, newCol, false);
            restoreBackdropRect(px - P(4), py - P(4), px + kHubPosterW() + P(4), py + kHubPosterH() + P(4));
            rgba_.assign(staticRgba_.begin(), staticRgba_.end());
            return;
        }
        patchStaticPoster(st, prevRow, prevCol, false);
        const int posterW = kHubPosterW(), posterH = kHubPosterH();
        int rowY = kHubTop();
        for (int ri = 0; ri < st.focus.row; ++ri)
            rowY += kHubRowAdvance();
        const int py = rowY + kHubPosterOffY();
        const int sideW = kSidebarW();
        const int gap = kHubGap();
        const int step = posterW + gap;
        const int scrollX = hubRowScrollX(st, st.focus.row, st.focus.col);
        const int px = sideW + kSafeMargin() - scrollX + st.focus.col * step;
        restoreBackdropRect(px - P(4), py - P(4), px + posterW + P(4), py + posterH + P(4));
        rgba_.assign(staticRgba_.begin(), staticRgba_.end());
    }
}

void Renderer::buildStaticLayer(const RenderState& st) {
    ensureScratch();
    paintBackdrop(st.backdropKey, st.backdropKeyValid, st.detailBackdrop);
    lastBackdropUrl_ = st.backdropUrl;
    RenderState base = st;
    base.focusAnim = kFocusAnimSteps;
    switch (st.view) {
    case ViewId::ServerList:
        drawServerList(base);
        break;
    case ViewId::Home:
        drawHome(base, true);
        break;
    case ViewId::LibraryGrid:
        drawGrid(base, true);
        break;
    case ViewId::Detail:
        drawDetail(base);
        break;
    }
    staticRgba_.assign(rgba_.begin(), rgba_.end());
}

void Renderer::render(const RenderState& st, std::vector<std::uint8_t>& out, RenderPerf* perf,
                      PixelFormat fmt) {
    const auto tRender0 = std::chrono::steady_clock::now();
    ensureScratch();
    const std::string key = staticLayerKey(st);
    const bool useCache =
        st.view == ViewId::Home || st.view == ViewId::LibraryGrid || st.view == ViewId::Detail;
    long long staticMs = 0;
    int rebuild = 0;
    std::string rebuildReason;
#if !defined(MUT_PLEXUI_NO_LAYER_CACHE)
    if (useCache && layerSaveAndRestore(key))
        rebuildReason = "layer_cache";
#endif
    const int prevRow = lastFocusRow_;
    const int prevCol = lastFocusCol_;
    const ViewId prevView = lastView_;
    const bool focusChanged = st.view != lastView_ || st.focus.row != lastFocusRow_ ||
                              st.focus.col != lastFocusCol_ ||
                              st.focus.sidebar != lastFocusSidebar_ ||
                              st.focus.gridOnChips != lastGridOnChips_ ||
                              st.focus.gridChip != lastGridChip_;
    const bool keyChanged = staticKey_ != key;
    if (keyChanged && !staticKey_.empty())
        logStaticKeyDiff(staticKey_, key);
    // Sidebar highlight is painted into the static layer. Moving the highlight, including
    // Left from the first hub poster into the sidebar, often keeps the same backdrop URL
    // (that poster is also the fallback). Those frames used to be copied unchanged.
    const bool sidebarMoved = st.view == ViewId::Home &&
                              (st.focus.sidebar != lastFocusSidebar_ ||
                               st.sidebarScrollY != lastSidebarScrollY_);
    // A Home/grid backdrop follows focus. Painting the new backdrop over the cached static layer
    // erased the sidebar, titles and every card but the focused one (glass a lab MiSTer 11:42, R1 too), so
    // a backdrop change rebuilds the static layer (backdrop + chrome + cached posters).
    const bool backdropChanged =
        useCache && staticValid_ && !keyChanged && st.backdropUrl != lastBackdropUrl_;
    const bool sidebarOnly =
        sidebarMoved && staticValid_ && !keyChanged && !backdropChanged && useCache;
    if (!staticValid_ || keyChanged || backdropChanged) {
        const auto t0 = std::chrono::steady_clock::now();
        buildStaticLayer(st);
        staticMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
        staticValid_ = true;
        staticKey_ = key;
        prescaleKey_ = {};
        lastGridScrollRow_ = st.gridScrollRow;
        lastHubScrollX_ =
            st.hubs.empty() ? 0 : hubRowScrollX(st, st.focus.row, st.focus.col);
        rebuild = 1;
        if (keyChanged)
            rebuildReason = "key";
        else if (backdropChanged)
            rebuildReason = "backdrop";
        else
            rebuildReason = "invalid";
    } else if (sidebarOnly) {
        // The pill lives in the static layer. Repaint that strip only. The backdrop
        // (and every poster) is rebuilt above, when its URL actually changes.
        const auto t0 = std::chrono::steady_clock::now();
        if (lastFocusSidebar_ == 0 && st.focus.sidebar != 0)
            paintUnfocusedHubPoster(st, lastFocusRow_, lastFocusCol_);
        patchSidebar(st);
        staticMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
        rebuildReason = "sidebar";
    } else if (focusChanged && useCache && staticValid_) {
        syncStaticAfterFocusMove(st, prevRow, prevCol, lastGridScrollRow_, lastHubScrollX_);
        lastGridScrollRow_ = st.gridScrollRow;
        lastHubScrollX_ =
            st.hubs.empty() ? 0 : hubRowScrollX(st, st.focus.row, st.focus.col);
        rebuildReason = "focus_patch";
    }
    if (focusChanged)
        prescaleKey_ = {};

    const auto tBlit0 = std::chrono::steady_clock::now();
    bool detailArt = false;
    if (!dirtyImageKeys_.empty() && staticValid_ && useCache) {
        for (const auto& dkey : dirtyImageKeys_) {
            if (st.view == ViewId::LibraryGrid) {
                const GridLayout gl = computeGridLayout(!st.gridChips.empty());
                for (size_t i = 0; i < st.grid.size(); ++i) {
                    if (!st.haveServer || st.grid[i].thumb.empty())
                        continue;
                    const ImageCacheKey ik = thumbCacheKey(st.server.machineIdentifier,
                                                             st.grid[i].ratingKey, gl.posterW,
                                                             gl.posterH, itemThumbRevision(st.grid[i]));
                    if (ik != dkey)
                        continue;
                    const int r = static_cast<int>(i / gl.cols);
                    const int c = static_cast<int>(i % gl.cols);
                    if (r == st.focus.row && c == st.focus.col && !st.focus.gridOnChips)
                        continue;
                    patchStaticPoster(st, r, c, false);
                }
            } else if (st.view == ViewId::Home) {
                const int posterW = kHubPosterW(), posterH = kHubPosterH();
                for (size_t ri = 0; ri < st.hubs.size(); ++ri) {
                    const auto& hub = st.hubs[ri];
                    for (size_t ci = 0; ci < hub.items.size(); ++ci) {
                        if (!st.haveServer || hub.items[ci].thumb.empty())
                            continue;
                        const ImageCacheKey ik =
                            hubThumbKey(st, hub.items[ci], posterW, posterH);
                        if (ik != dkey)
                            continue;
                        if (static_cast<int>(ri) == st.focus.row &&
                            static_cast<int>(ci) == st.focus.col && st.focus.sidebar == 0)
                            continue;
                        patchStaticPoster(st, static_cast<int>(ri), static_cast<int>(ci), false);
                    }
                }
            } else if (st.view == ViewId::Detail && !st.detail.ratingKey.empty() &&
                       dkey.ratingKey == st.detail.ratingKey) {
                detailArt = true;
            }
        }
        dirtyImageKeys_.clear();
        if (detailArt) {
            buildStaticLayer(st);
            rebuild = 1;
            rebuildReason = "detail_art";
        }
        rgba_.assign(staticRgba_.begin(), staticRgba_.end());
    }

    long long blitMs = 0;
    const bool animFocus = useCache && st.focusAnim < kFocusAnimSteps;
    if (animFocus && staticValid_) {
        int x0 = 0, y0 = 0, x1 = kScreenW(), y1 = kScreenH();
        if (st.view == ViewId::Home || st.view == ViewId::LibraryGrid) {
            int nx0 = 0, ny0 = 0, nx1 = 0, ny1 = 0;
            if (st.view == ViewId::Home) {
                const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
                const int sideW = kSidebarW();
                const int step = posterW + gap;
                int rowY = kHubTop();
                for (size_t ri = 0; ri < st.hubs.size(); ++ri) {
                    if (static_cast<int>(ri) != st.focus.row || st.focus.sidebar != 0) {
                        rowY += kHubRowAdvance();
                        continue;
                    }
                    const auto& hub = st.hubs[ri];
                    int scrollX = st.focus.col * step;
                    const int viewW = kScreenW() - sideW;
                    const int totalW = static_cast<int>(hub.items.size()) * step - gap;
                    const int maxScroll = std::max(0, totalW - viewW + posterW / 2);
                    scrollX = std::min(scrollX, maxScroll);
                    const int px = sideW + kSafeMargin() - scrollX + st.focus.col * step;
                    const int py = rowY + kHubPosterOffY();
                    posterDirtyRect(px, py, posterW, posterH, nx0, ny0, nx1, ny1);
                    break;
                }
            } else {
                const GridLayout gl = computeGridLayout(!st.gridChips.empty());
                const int idx = st.focus.row * gl.cols + st.focus.col;
                if (idx >= 0 && idx < static_cast<int>(st.grid.size())) {
                    const int c = idx % gl.cols;
                    const int r = idx / gl.cols;
                    const int visRow = r - st.gridScrollRow;
                    const int px = gl.x0 + c * (gl.posterW + gl.gap);
                    const int py = gl.y0 + visRow * gl.rowStep;
                    posterDirtyRect(px, py, gl.posterW, gl.posterH, nx0, ny0, nx1, ny1);
                }
            }
            if (lastFocusAnim_ < kFocusAnimSteps && prevView == st.view &&
                (prevRow != st.focus.row || prevCol != st.focus.col)) {
                int ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
                if (st.view == ViewId::Home) {
                    const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
                    const int sideW = kSidebarW();
                    const int step = posterW + gap;
                    int rowY = kHubTop();
                    for (size_t ri = 0; ri < st.hubs.size(); ++ri) {
                        if (static_cast<int>(ri) != prevRow) {
                            rowY += kHubRowAdvance();
                            continue;
                        }
                        const auto& hub = st.hubs[ri];
                        int scrollX = prevCol * step;
                        const int viewW = kScreenW() - sideW;
                        const int totalW = static_cast<int>(hub.items.size()) * step - gap;
                        scrollX = std::min(scrollX, std::max(0, totalW - viewW + posterW / 2));
                        const int px = sideW + kSafeMargin() - scrollX + prevCol * step;
                        const int py = rowY + kHubPosterOffY();
                        posterDirtyRect(px, py, posterW, posterH, ox0, oy0, ox1, oy1);
                        break;
                    }
                } else {
                    const GridLayout gl = computeGridLayout(!st.gridChips.empty());
                    const int idx = prevRow * gl.cols + prevCol;
                    if (idx >= 0 && idx < static_cast<int>(st.grid.size())) {
                        const int c = idx % gl.cols;
                        const int r = idx / gl.cols;
                        const int visRow = r - st.gridScrollRow;
                        const int px = gl.x0 + c * (gl.posterW + gl.gap);
                        const int py = gl.y0 + visRow * gl.rowStep;
                        posterDirtyRect(px, py, gl.posterW, gl.posterH, ox0, oy0, ox1, oy1);
                    }
                }
                x0 = std::min(nx0, ox0);
                y0 = std::min(ny0, oy0);
                x1 = std::max(nx1, ox1);
                y1 = std::max(ny1, oy1);
            } else {
                x0 = nx0;
                y0 = ny0;
                x1 = nx1;
                y1 = ny1;
            }
        }
        copyRgbaRect(staticRgba_.data(), rgba_.data(), x0, y0, x1, y1);
    } else if (staticValid_ && useCache) {
        rgba_.assign(staticRgba_.begin(), staticRgba_.end());
    } else {
        paintBackdrop(st.backdropKey, st.backdropKeyValid, st.detailBackdrop);
        switch (st.view) {
        case ViewId::ServerList:
            drawServerList(st);
            break;
        case ViewId::Home:
            drawHome(st);
            break;
        case ViewId::LibraryGrid:
            drawGrid(st);
            break;
        case ViewId::Detail:
            drawDetail(st);
            break;
        }
    }
    blitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - tBlit0)
                 .count();

    const auto tFocus0 = std::chrono::steady_clock::now();
    if (st.view == ViewId::Home && st.focus.sidebar == 0) {
        const int posterW = kHubPosterW(), posterH = kHubPosterH(), gap = kHubGap();
        const int sideW = kSidebarW();
        const int step = posterW + gap;
        int rowY = kHubTop();
        for (size_t ri = 0; ri < st.hubs.size(); ++ri) {
            const auto& hub = st.hubs[ri];
            if (static_cast<int>(ri) != st.focus.row) {
                rowY += kHubRowAdvance();
                continue;
            }
            const int scrollX = hubRowScrollX(st, static_cast<int>(ri), st.focus.col);
            int px = sideW + kSafeMargin() - scrollX;
            const int py = rowY + kHubPosterOffY();
            for (size_t ci = 0; ci < hub.items.size(); ++ci) {
                if (px > kScreenW())
                    break;
                if (px < sideW + kSafeMargin() - gap) {
                    px += step;
                    continue;
                }
                if (static_cast<int>(ci) == st.focus.col) {
                    ImageCacheKey imgKey{};
                    std::string url;
                    if (st.haveServer && !hub.items[ci].thumb.empty()) {
                        const PlexServer& thumbSrv = serverForHubItemRender(st, hub.items[ci]);
                        imgKey = thumbCacheKey(thumbSrv.machineIdentifier, hub.items[ci].ratingKey,
                                               posterW, posterH, itemThumbRevision(hub.items[ci]));
                        url = photoTranscodeUrl(thumbSrv.baseUrl, hub.items[ci].thumb,
                                                thumbSrv.accessToken, posterW, posterH);
                    }
                    drawPoster(px, py, posterW, posterH, imgKey, url, hub.items[ci].title, true,
                               st.focusAnim);
                }
                px += step;
            }
            break;
        }
    } else if (st.view == ViewId::LibraryGrid && !st.focus.gridOnChips) {
        const GridLayout gl = computeGridLayout(!st.gridChips.empty());
        const int idx = st.focus.row * gl.cols + st.focus.col;
        if (idx >= 0 && idx < static_cast<int>(st.grid.size())) {
            const int c = idx % gl.cols;
            const int r = idx / gl.cols;
            const int visRow = r - st.gridScrollRow;
            const int px = gl.x0 + c * (gl.posterW + gl.gap);
            const int py = gl.y0 + visRow * gl.rowStep;
            ImageCacheKey imgKey{};
            std::string url;
            if (st.haveServer && !st.grid[static_cast<std::size_t>(idx)].thumb.empty()) {
                imgKey = thumbCacheKey(st.server.machineIdentifier,
                                       st.grid[static_cast<std::size_t>(idx)].ratingKey,
                                       gl.posterW, gl.posterH,
                                       itemThumbRevision(st.grid[static_cast<std::size_t>(idx)]));
                url = photoTranscodeUrl(st.server.baseUrl,
                                        st.grid[static_cast<std::size_t>(idx)].thumb,
                                        st.server.accessToken, gl.posterW, gl.posterH);
            }
            drawPoster(px, py, gl.posterW, gl.posterH, imgKey, url,
                       st.grid[static_cast<std::size_t>(idx)].title, true, st.focusAnim);
        }
    }

    lastFocusAnim_ = st.focusAnim;
    lastView_ = st.view;
    lastFocusRow_ = st.focus.row;
    lastFocusCol_ = st.focus.col;
    lastFocusSidebar_ = st.focus.sidebar;
    lastSidebarScrollY_ = st.sidebarScrollY;
    lastGridOnChips_ = st.focus.gridOnChips;
    lastGridChip_ = st.focus.gridChip;

    const long long focusMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - tFocus0)
                                  .count();
    const long long renderMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tRender0)
                                   .count();
    if (perf) {
        perf->renderMs = renderMs;
        perf->staticMs = staticMs;
        perf->blitMs = blitMs;
        perf->focusMs = focusMs;
        perf->rebuild = rebuild;
        perf->reason = rebuildReason.empty() ? "none" : rebuildReason;
    }

    const std::size_t px = static_cast<std::size_t>(kScreenW()) * kScreenH();
    out.resize(px * static_cast<std::size_t>(bytesPerPixel(fmt)));
    const std::uint8_t* src = rgba_.data();
    std::uint8_t* d = out.data();
    if (fmt == PixelFormat::RGB24) {
        for (std::size_t i = 0; i < px; ++i, src += 4, d += 3) {
            d[0] = src[0];
            d[1] = src[1];
            d[2] = src[2];
        }
    } else {
        for (std::size_t i = 0; i < px; ++i, src += 4, d += 4) {
            d[0] = src[2];
            d[1] = src[1];
            d[2] = src[0];
            d[3] = 255;
        }
    }
}

} // namespace plexui
} // namespace misterplex
