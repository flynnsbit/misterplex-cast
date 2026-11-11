#pragma once
// E9: per-tube overscan / safe-area calibration (MisterZine "set the picture
// edges once"). Distinct from GH#46, which is one symmetric UI_SAFE_MARGIN
// percent plus UI_SIZE. This file does not change that pair.
//
// OVERSCAN_LEFT / RIGHT / TOP / BOTTOM are percents of that axis, 0-25.
// Blank or absent is unset. Unset leaves the fixed title-safe (and the #46
// floor) exactly as the tip draws it. A set edge replaces the fixed inset
// on that side only. The Program menu "Safe area" screen writes all four.

#include "libmisterplex/ui_safe_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace misterplex {

constexpr int kOverscanUnset = -1;
constexpr int kOverscanMinPct = 0;
constexpr int kOverscanMaxPct = 25;

inline constexpr const char* kOverscanLeftKey = "OVERSCAN_LEFT";
inline constexpr const char* kOverscanRightKey = "OVERSCAN_RIGHT";
inline constexpr const char* kOverscanTopKey = "OVERSCAN_TOP";
inline constexpr const char* kOverscanBottomKey = "OVERSCAN_BOTTOM";

struct OverscanEdges {
    int left = kOverscanUnset;
    int right = kOverscanUnset;
    int top = kOverscanUnset;
    int bottom = kOverscanUnset;
    bool any() const { return left >= 0 || right >= 0 || top >= 0 || bottom >= 0; }
    bool operator==(const OverscanEdges& o) const {
        return left == o.left && right == o.right && top == o.top && bottom == o.bottom;
    }
};

inline int clampOverscanPct(int pct) {
    return std::max(kOverscanMinPct, std::min(kOverscanMaxPct, pct));
}

// Empty, junk, or a leading minus → unset. "0" is a real edge (flush).
// "12" and "12%" are 12. Above 25 clamps.
inline int parseOverscanEdgePct(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isspace(c) || c == '%')
            continue;
        s.push_back(static_cast<char>(c));
    }
    if (s.empty())
        return kOverscanUnset;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || (end && *end))
        return kOverscanUnset;
    if (v < 0)
        return kOverscanUnset;
    return clampOverscanPct(static_cast<int>(v));
}

inline int overscanInsetPx(int dim, int pct) {
    if (dim <= 0 || pct < 0)
        return 0;
    return (dim * clampOverscanPct(pct)) / 100;
}

// Nearest percent. Used to open the screen on the current title-safe box.
inline int pctFromPx(int px, int dim) {
    if (dim <= 0 || px <= 0)
        return 0;
    return clampOverscanPct((px * 100 + dim / 2) / dim);
}

inline std::atomic<int>& overscanLeftStore() {
    static std::atomic<int> v{kOverscanUnset};
    return v;
}
inline std::atomic<int>& overscanRightStore() {
    static std::atomic<int> v{kOverscanUnset};
    return v;
}
inline std::atomic<int>& overscanTopStore() {
    static std::atomic<int> v{kOverscanUnset};
    return v;
}
inline std::atomic<int>& overscanBottomStore() {
    static std::atomic<int> v{kOverscanUnset};
    return v;
}

inline OverscanEdges overscanEdges() {
    return {overscanLeftStore().load(std::memory_order_relaxed),
            overscanRightStore().load(std::memory_order_relaxed),
            overscanTopStore().load(std::memory_order_relaxed),
            overscanBottomStore().load(std::memory_order_relaxed)};
}

inline int storedOverscanPct(int v) {
    if (v < 0)
        return kOverscanUnset;
    return clampOverscanPct(v);
}

inline void setOverscanEdges(OverscanEdges e) {
    overscanLeftStore().store(storedOverscanPct(e.left), std::memory_order_relaxed);
    overscanRightStore().store(storedOverscanPct(e.right), std::memory_order_relaxed);
    overscanTopStore().store(storedOverscanPct(e.top), std::memory_order_relaxed);
    overscanBottomStore().store(storedOverscanPct(e.bottom), std::memory_order_relaxed);
}

inline void clearOverscanEdges() { setOverscanEdges({}); }

// One axis of the safe rect. A set percent replaces `fallback`. Unset keeps it.
// Interlaced tops and bottoms are nudged to an even line.
struct OverscanPx {
    int left = 0, right = 0, top = 0, bottom = 0;
};

inline OverscanPx overscanPxFor(int w, int h, OverscanEdges e, bool even, int fallL, int fallR, int fallT,
                                int fallB) {
    OverscanPx p{fallL, fallR, fallT, fallB};
    if (!e.any()) {
#ifdef MUT_OVERSCAN_UNSET_SHIFTS
        p.left += 8;
        p.right += 8;
        p.top += 8;
        p.bottom += 8;
#endif
        return p;
    }
#ifdef MUT_OVERSCAN_IGNORED
    (void)w;
    (void)h;
    (void)even;
    return p;
#else
    if (e.left >= 0)
        p.left = overscanInsetPx(w, e.left);
    if (e.right >= 0)
        p.right = overscanInsetPx(w, e.right);
    if (e.top >= 0)
        p.top = overscanInsetPx(h, e.top);
    if (e.bottom >= 0)
        p.bottom = overscanInsetPx(h, e.bottom);
    if (even) {
        p.top += p.top & 1;
        p.bottom += p.bottom & 1;
    }
    if (w > 16 && p.left + p.right >= w - 16)
        p.left = fallL, p.right = fallR;
    if (h > 16 && p.top + p.bottom >= h - 16)
        p.top = fallT, p.bottom = fallB;
    return p;
#endif
}

// PlexUI / ready-line inset. Unset returns `fallback` (the tip's number).
inline int overscanEdgeOr(int pct, int dim, int fallback) {
#ifndef MUT_OVERSCAN_PLEX_IGNORES
    if (pct >= 0)
        return overscanInsetPx(dim, pct);
#else
    (void)pct;
    (void)dim;
#endif
    return fallback;
}

enum class OverscanFocus : std::uint8_t { Left = 0, Right = 1, Top = 2, Bottom = 3 };

struct OverscanCal {
    bool open = false;
    OverscanFocus focus = OverscanFocus::Left;
    int left = 0, right = 0, top = 0, bottom = 0;
    int edgePct() const {
        switch (focus) {
        case OverscanFocus::Right: return right;
        case OverscanFocus::Top: return top;
        case OverscanFocus::Bottom: return bottom;
        case OverscanFocus::Left: return left;
        }
        return left;
    }
    int& edgePctRef() {
        switch (focus) {
        case OverscanFocus::Right: return right;
        case OverscanFocus::Top: return top;
        case OverscanFocus::Bottom: return bottom;
        case OverscanFocus::Left: return left;
        }
        return left;
    }
};

inline const char* overscanFocusName(OverscanFocus f) {
    switch (f) {
    case OverscanFocus::Right: return "RIGHT";
    case OverscanFocus::Top: return "TOP";
    case OverscanFocus::Bottom: return "BOTTOM";
    case OverscanFocus::Left: return "LEFT";
    }
    return "LEFT";
}

inline OverscanCal beginOverscanCal(const OverscanEdges& saved, int seedL, int seedR, int seedT, int seedB) {
    OverscanCal c;
    c.open = true;
    auto pick = [](int savedEdge, int seed) {
        return savedEdge >= 0 ? clampOverscanPct(savedEdge) : clampOverscanPct(seed);
    };
    c.left = pick(saved.left, seedL);
    c.right = pick(saved.right, seedR);
    c.top = pick(saved.top, seedT);
    c.bottom = pick(saved.bottom, seedB);
    return c;
}

inline void overscanNudge(OverscanCal& c, int delta) {
    c.edgePctRef() = clampOverscanPct(c.edgePct() + delta);
}

inline void overscanCycle(OverscanCal& c, int dir) {
    int f = static_cast<int>(c.focus) + dir;
    f = (f % 4 + 4) % 4;
    c.focus = static_cast<OverscanFocus>(f);
}

inline OverscanEdges edgesOf(const OverscanCal& c) {
    return {clampOverscanPct(c.left), clampOverscanPct(c.right), clampOverscanPct(c.top),
            clampOverscanPct(c.bottom)};
}

inline std::string overscanConfValue(int pct) {
    if (pct < 0)
        return {};
    return std::to_string(clampOverscanPct(pct));
}

inline std::string writeOverscanConfText(const std::string& body, const OverscanEdges& e) {
    std::string o = upsertConfText(body, kOverscanLeftKey, overscanConfValue(e.left));
    o = upsertConfText(o, kOverscanRightKey, overscanConfValue(e.right));
    o = upsertConfText(o, kOverscanTopKey, overscanConfValue(e.top));
    o = upsertConfText(o, kOverscanBottomKey, overscanConfValue(e.bottom));
    return o;
}

inline bool persistOverscanConf(const std::string& path, const OverscanEdges& e) {
    if (path.empty())
        return false;
    std::string body;
    {
        std::ifstream in(path);
        if (in)
            body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    body = writeOverscanConfText(body, e);
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
