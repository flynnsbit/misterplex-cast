#pragma once
// CRT title-safe insets + UI size (GH#46). Pure functions: host-testable, no raster/RBF.
//
// UI_SAFE_MARGIN is a percent of each raster axis, clamped 0-15 (default 5).
// UI_SIZE is normal (scale 1) or large (scale 5/4). At the default pair, R1
// PlexUI geometry stays P(32)/72 so test_plexui_geom goldens stay green.
//
// Ready/status cards (owenb OSD) already sit in a spec title-safe that is
// larger than 5% on 480p/480i/240p; the helper is a floor (never shrinks).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace misterplex {

enum class UiSize : std::uint8_t { Normal, Large };

constexpr int kUiSafeMarginDefaultPct = 5;
constexpr int kUiSafeMarginMinPct = 0;
constexpr int kUiSafeMarginMaxPct = 15;
constexpr float kUiSizeLargeScale = 1.25f;

inline int clampUiSafeMarginPct(int pct) {
    return std::max(kUiSafeMarginMinPct, std::min(kUiSafeMarginMaxPct, pct));
}

inline const char* uiSizeName(UiSize s) { return s == UiSize::Large ? "large" : "normal"; }

inline float uiSizeScale(UiSize s) { return s == UiSize::Large ? kUiSizeLargeScale : 1.f; }

inline UiSize parseUiSize(std::string_view raw) {
#ifdef MUT_GH46_NO_LARGE
    (void)raw;
    return UiSize::Normal;
#else
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isspace(c))
            continue;
        s.push_back(static_cast<char>(std::tolower(c)));
    }
    return s == "large" ? UiSize::Large : UiSize::Normal;
#endif
}

// Empty / junk → default 5. Accepts "5", "5%", " 12 ".
inline int parseUiSafeMarginPct(std::string_view raw) {
    if (raw.empty())
        return kUiSafeMarginDefaultPct;
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isspace(c) || c == '%')
            continue;
        s.push_back(static_cast<char>(c));
    }
    if (s.empty())
        return kUiSafeMarginDefaultPct;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str())
        return kUiSafeMarginDefaultPct;
    return clampUiSafeMarginPct(static_cast<int>(v));
}

inline int uiSafeMarginPx(int dim, int pct) {
    if (dim <= 0)
        return 0;
    return (dim * clampUiSafeMarginPct(pct)) / 100;
}

struct UiSafeInsets {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return left; }  // alias: symmetric percent
};

inline UiSafeInsets uiSafeInsets(int w, int h, int pct) {
    const int mx = uiSafeMarginPx(w, pct);
    const int my = uiSafeMarginPx(h, pct);
    return {mx, my, mx, my};
}

struct UiSafeRect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < right() && py < bottom(); }
};

inline UiSafeRect uiSafeRect(int w, int h, int pct) {
    const UiSafeInsets in = uiSafeInsets(w, h, pct);
    const int rw = std::max(0, w - in.left - in.right);
    const int rh = std::max(0, h - in.top - in.bottom);
    return {in.left, in.top, rw, rh};
}

// Applied browse insets: never smaller than the historic PlexUI constants, so
// default 5% @ 640x480 stays left=32 / right=72 (R1 goldens).
inline int uiSafeLeftPx(int w, int pct, int legacyLeft) {
    return std::max(legacyLeft, uiSafeMarginPx(w, pct));
}
inline int uiSafeRightPx(int w, int pct, int legacyRight) {
    return std::max(legacyRight, uiSafeMarginPx(w, pct));
}
inline int uiSafeTopPx(int h, int pct, int legacyTop) {
    return std::max(legacyTop, uiSafeMarginPx(h, pct));
}

// Daemon-drawn caption band: bottom of title-safe, even-aligned on 480i.
struct CaptionBand {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

inline CaptionBand captionBandFor(int rasterW, int rasterH, int pct, bool evenLines) {
    const UiSafeRect s = uiSafeRect(rasterW, rasterH, pct);
    int bandH = evenLines ? 32 : 24;
    if (bandH > s.h)
        bandH = s.h;
    if (evenLines)
        bandH &= ~1;
    int y = s.bottom() - bandH;
    if (evenLines)
        y &= ~1;
    if (y < s.y)
        y = s.y;
    return {s.x, y, s.w, bandH};
}

struct UiSafePrefs {
    int marginPct = kUiSafeMarginDefaultPct;
    UiSize size = UiSize::Normal;
};

inline std::atomic<int>& uiSafeMarginPctStore() {
    static std::atomic<int> v{kUiSafeMarginDefaultPct};
    return v;
}
inline std::atomic<int>& uiSafeSizeStore() {
    static std::atomic<int> v{0};  // 0 normal, 1 large
    return v;
}

// #207 (a): clamp a framebuffer row span to [0, screenH). UI_SIZE=large (5/4) pushes the lower
// hub rows past the 480-line raster, so an unclamped span walks off the RGBA buffers.
inline void clampRowSpanToScreen(int& y0, int& y1, int screenH) {
    if (y0 < 0) y0 = 0;
    if (y1 > screenH) y1 = screenH;
    if (y1 < y0) y1 = y0;
}

inline UiSafePrefs uiSafePrefs() {
    return {uiSafeMarginPctStore().load(std::memory_order_relaxed),
            uiSafeSizeStore().load(std::memory_order_relaxed) ? UiSize::Large : UiSize::Normal};
}

inline void setUiSafePrefs(int pct, UiSize size) {
    uiSafeMarginPctStore().store(clampUiSafeMarginPct(pct), std::memory_order_relaxed);
    uiSafeSizeStore().store(size == UiSize::Large ? 1 : 0, std::memory_order_relaxed);
}

// In-memory KEY=value upsert. Preserves comments and other keys. Host-testable.
inline std::string upsertConfText(const std::string& text, const char* key, const std::string& value) {
    if (!key || !*key)
        return text;
    const std::string p = std::string(key) + "=";
    std::istringstream in(text);
    std::string line, out;
    bool found = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!found && line.rfind(p, 0) == 0) {
            out += p;
            out += value;
            out += '\n';
            found = true;
        } else {
            out += line;
            out += '\n';
        }
    }
    if (!found) {
        out += p;
        out += value;
        out += '\n';
    }
    return out;
}

inline bool persistUiSafeConf(const std::string& path, int marginPct, UiSize size) {
    if (path.empty())
        return false;
    std::string body;
    {
        std::ifstream in(path);
        if (in)
            body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    body = upsertConfText(body, "UI_SAFE_MARGIN", std::to_string(clampUiSafeMarginPct(marginPct)));
    body = upsertConfText(body, "UI_SIZE", uiSizeName(size));
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << body;
        if (!out)
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace misterplex
