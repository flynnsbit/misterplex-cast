#pragma once
// Idle / screensaver frame renderer for the Plex core.
//
// Why this exists: the FPGA frame store (F1) and /dev/fb0 both latch the LAST
// frame written. When a session ends nothing overwrites them, so the final frame
// of the last video stays on screen indefinitely — on a CRT that is a burn-in
// risk, and it looks broken. The daemon therefore paints an explicit idle frame
// at session end and keeps a slow animation running while idle.
//
// Pure math + pixel fill so it can be unit-tested without a framebuffer.

#include <cstddef>
#include <cstdint>

namespace misterplex {

enum class IdleMode {
    Logo = 0,       // static centred chevron on a dark field
    Black = 1,      // flat black
    Screensaver = 2,// chevron slowly drifting to avoid CRT burn-in
    LastFrame = 3,  // leave whatever was on screen (legacy behaviour)
};

inline IdleMode idleModeFromBits(unsigned bits) {
    switch (bits & 3u) {
    case 1: return IdleMode::Black;
    case 2: return IdleMode::Screensaver;
    case 3: return IdleMode::LastFrame;
    default: return IdleMode::Logo;
    }
}

// Plex-ish palette: near-black background, amber mark.
constexpr uint8_t kIdleBgR = 0x1F, kIdleBgG = 0x23, kIdleBgB = 0x26;
constexpr uint8_t kIdleFgR = 0xE5, kIdleFgG = 0xA0, kIdleFgB = 0x0D;

// Screensaver drift: full cycle in this many phase steps. The mark never gets
// closer than kIdleMargin px to an edge, so it cannot be cropped by overscan.
constexpr int kIdlePhasePeriod = 1200;
constexpr int kIdleMargin = 8;

// Triangular wave in [0, span] — a drift that reverses instead of wrapping, so
// the mark never jumps across the screen.
inline int idleDrift(int phase, int span) {
    if (span <= 0)
        return 0;
    const int p = ((phase % kIdlePhasePeriod) + kIdlePhasePeriod) % kIdlePhasePeriod;
    const int half = kIdlePhasePeriod / 2;
    const int tri = p < half ? p : (kIdlePhasePeriod - p);
    return (tri * span) / half;
}

struct IdleChevronExtent {
    int half = 0;
    int stroke = 0;
    int glyphW = 0;
    int glyphH = 0;
};

inline IdleChevronExtent idleChevronExtent(int size) {
    IdleChevronExtent e{};
    if (size <= 0)
        return e;
    e.half = size / 2;
    e.stroke = size / 5 > 0 ? size / 5 : 1;
    e.glyphW = e.half + e.stroke;
    e.glyphH = size;
    return e;
}

// Is (x,y) inside the chevron mark whose design box starts at (ox,oy)?
// The painted ">" is narrower than that box; idleChevronExtent is the shared
// contract used to center the actual glyph rather than its empty right side.
inline bool idleChevronHit(int x, int y, int ox, int oy, int size) {
    if (size <= 0)
        return false;
    const int lx = x - ox;
    const int ly = y - oy;
    if (lx < 0 || ly < 0 || lx >= size || ly >= size)
        return false;
    const IdleChevronExtent e = idleChevronExtent(size);
    // Distance from the two 45-degree arms, in "diagonal" units.
    const int d = ly <= e.half ? (lx - ly) : (lx - (size - 1 - ly));
    return d >= 0 && d < e.stroke;
}

// Coverage 0..256 (8.8 fixed) for anti-aliased chevron stroke.
// Hard binary edges + 4:2:0 + 1280→1920 scale produce multi-colour "dot crawl"
// on the silhouette (visible on VGA/HDMI). Feather ~3px so scaled glass has no
// single-pixel chroma ticks. Solid (256) only where idleChevronHit is true so
// overscan margin / binary tests stay honest; skirt never invents full FG.
inline int idleChevronCover256(int x, int y, int ox, int oy, int size) {
    if (size <= 0)
        return 0;
    if (idleChevronHit(x, y, ox, oy, size))
        return 256;
    const int lx = x - ox;
    const int ly = y - oy;
    // Soft skirt just outside the stroke (box ±3). No full coverage out here.
    if (lx < -3 || ly < -3 || lx > size + 2 || ly > size + 2)
        return 0;
    const IdleChevronExtent e = idleChevronExtent(size);
    // Clamp ly into the diagonal domain so out-of-box samples don't fake a hit.
    const int lyC = ly < 0 ? 0 : (ly >= size ? size - 1 : ly);
    const int d = lyC <= e.half ? (lx - lyC) : (lx - (size - 1 - lyC));
    int outside = 0;
    if (d < 0)
        outside = -d;
    else if (d >= e.stroke)
        outside = d - (e.stroke - 1);
    // Also push away when outside the box vertically/horizontally.
    if (lx < 0)
        outside += -lx;
    else if (lx >= size)
        outside += lx - (size - 1);
    if (ly < 0)
        outside += -ly;
    else if (ly >= size)
        outside += ly - (size - 1);
    if (outside <= 0)
        return 0; // should be unreachable (hit handled above)
    if (outside == 1)
        return 192;
    if (outside == 2)
        return 64;
    if (outside == 3)
        return 16;
    return 0;
}

struct IdleRenderState {
    bool blank = false;
    int size = 0;
    int ox = 0;
    int oy = 0;
};

inline IdleRenderState idleRenderState(int w, int h, IdleMode mode, int phase,
                                       int visibleX, int visibleW) {
    IdleRenderState s{};
    if (w <= 0 || h <= 0 || visibleX < 0 || visibleW <= 0 || visibleX + visibleW > w) {
        s.blank = true;
        return s;
    }
    s.blank = (mode == IdleMode::Black);
    s.size = (visibleW < h ? visibleW : h) / 3;
    if (s.size < 4)
        s.size = 4;
    const IdleChevronExtent e = idleChevronExtent(s.size);
    s.ox = visibleX + (visibleW - e.glyphW) / 2;
    s.oy = (h - e.glyphH) / 2;
    if (mode == IdleMode::Screensaver) {
        const int spanX = visibleW - e.glyphW - 2 * kIdleMargin;
        const int spanY = h - e.glyphH - 2 * kIdleMargin;
        s.ox = visibleX + kIdleMargin + idleDrift(phase, spanX);
        // Quarter-period offset so the drift traces a path, not a diagonal line.
        s.oy = kIdleMargin + idleDrift(phase + kIdlePhasePeriod / 4, spanY);
    }
    return s;
}

inline IdleRenderState idleRenderState(int w, int h, IdleMode mode, int phase) {
    return idleRenderState(w, h, mode, phase, 0, w);
}

inline void idlePixelRgb(int x, int y, const IdleRenderState& s,
                         uint8_t& r, uint8_t& g, uint8_t& b) {
    if (s.blank) {
        r = g = b = 0;
        return;
    }
    const int cov = idleChevronCover256(x, y, s.ox, s.oy, s.size);
    if (cov <= 0) {
        r = kIdleBgR;
        g = kIdleBgG;
        b = kIdleBgB;
        return;
    }
    if (cov >= 256) {
        r = kIdleFgR;
        g = kIdleFgG;
        b = kIdleFgB;
        return;
    }
    // Blend fg over bg (cov is 0..256).
    r = static_cast<uint8_t>((kIdleFgR * cov + kIdleBgR * (256 - cov) + 128) >> 8);
    g = static_cast<uint8_t>((kIdleFgG * cov + kIdleBgG * (256 - cov) + 128) >> 8);
    b = static_cast<uint8_t>((kIdleFgB * cov + kIdleBgB * (256 - cov) + 128) >> 8);
}

inline uint8_t idleClamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline uint8_t idleRgbToY(int r, int g, int b) {
    return idleClamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}

inline uint8_t idleRgbToU(int r, int g, int b) {
    return idleClamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}

inline uint8_t idleRgbToV(int r, int g, int b) {
    return idleClamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

// Fill a packed RGB24 buffer with the idle image.
// `phase` advances one step per repaint; ignored unless mode is Screensaver.
// LastFrame is a no-op by design (caller must not repaint).
inline void renderIdleRgb24(uint8_t* rgb, int w, int h, IdleMode mode, int phase) {
    if (!rgb || w <= 0 || h <= 0 || mode == IdleMode::LastFrame)
        return;

    const IdleRenderState state = idleRenderState(w, h, mode, phase);
    uint8_t* p = rgb;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            idlePixelRgb(x, y, state, r, g, b);
            *p++ = r;
            *p++ = g;
            *p++ = b;
        }
    }
}

// Fill a planar I420/YUV420p buffer with the same idle image. This is the DDR
// frame-store format used by the C3 core; LastFrame remains a no-op.
inline bool renderIdleYuv420p(uint8_t* yuv, int w, int h, IdleMode mode, int phase,
                              int visibleX, int visibleW) {
    if (!yuv || w <= 0 || h <= 0 || (w & 1) || (h & 1) ||
        visibleX < 0 || visibleW <= 0 || visibleX + visibleW > w ||
        mode == IdleMode::LastFrame)
        return false;

    const IdleRenderState state =
        idleRenderState(w, h, mode, phase, visibleX, visibleW);
    uint8_t* yPlane = yuv;
    uint8_t* uPlane = yPlane + static_cast<size_t>(w) * static_cast<size_t>(h);
    uint8_t* vPlane = uPlane + static_cast<size_t>(w / 2) * static_cast<size_t>(h / 2);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            idlePixelRgb(x, y, state, r, g, b);
            yPlane[static_cast<size_t>(y) * static_cast<size_t>(w) + x] = idleRgbToY(r, g, b);
        }
    }

    // Chroma 2x2: do NOT average orange with dark bg — that yields mid-Y + near-
    // neutral UV which decodes green on the right silhouette (4:2:0 classic).
    // Prefer solid FG UV whenever any sample is inside the stroke; otherwise
    // coverage-weighted blend of FG UV toward neutral (bg UV=128).
    const uint8_t uFg = idleRgbToU(kIdleFgR, kIdleFgG, kIdleFgB);
    const uint8_t vFg = idleRgbToV(kIdleFgR, kIdleFgG, kIdleFgB);
    const uint8_t uBg = idleRgbToU(kIdleBgR, kIdleBgG, kIdleBgB);
    const uint8_t vBg = idleRgbToV(kIdleBgR, kIdleBgG, kIdleBgB);
    for (int cy = 0; cy < h / 2; ++cy) {
        for (int cx = 0; cx < w / 2; ++cx) {
            int covMax = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int cov = idleChevronCover256(
                        cx * 2 + dx, cy * 2 + dy, state.ox, state.oy, state.size);
                    if (cov > covMax)
                        covMax = cov;
                }
            }
            const size_t ci = static_cast<size_t>(cy) * static_cast<size_t>(w / 2) + cx;
            if (state.blank) {
                uPlane[ci] = 128;
                vPlane[ci] = 128;
            } else if (covMax <= 0) {
                // Solid dark field — use BG chroma (not neutral 128).
                uPlane[ci] = uBg;
                vPlane[ci] = vBg;
            } else if (covMax >= 256) {
                // Any fully-inside sample → pure amber chroma (avoid green fringe).
                uPlane[ci] = uFg;
                vPlane[ci] = vFg;
            } else {
                // Partial AA: blend FG chroma toward BG chroma by coverage.
                const int t = covMax; // 1..255
                uPlane[ci] = idleClamp8((int(uFg) * t + int(uBg) * (256 - t) + 128) >> 8);
                vPlane[ci] = idleClamp8((int(vFg) * t + int(vBg) * (256 - t) + 128) >> 8);
            }
        }
    }
    return true;
}

inline bool renderIdleYuv420p(uint8_t* yuv, int w, int h, IdleMode mode, int phase) {
    return renderIdleYuv420p(yuv, w, h, mode, phase, 0, w);
}

} // namespace misterplex
