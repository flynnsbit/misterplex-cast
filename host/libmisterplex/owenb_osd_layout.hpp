#pragma once
// OWENB-P1B T6: the ARM overlay renderer's layout and page planner (ARM-RENDERER-SPEC-1017 §1, §2, §4).
// Pure functions over the core's raster (h264ring_raster, h264ring.h:337-357) and fb0's size.
//
// The core's contract (owenb v1.1.0 hps/ + h264/, read for the interface only):
//   * one canvas pixel is one raster pixel, the canvas centred on the raster (h264/ovl_plane.sv:7-8,
//     250-251, 287-288): a canvas of the raster's own W x H makes the centring a no-op;
//   * only the band's lines are fetched (ovl_plane.sv:19-24; h264ring.h:284-285);
//   * the descriptor is refused unless base % 8 == 0, stride % 8 == 0 and >= W x bpp, the band
//     inside the canvas and the canvas inside [FB_BYTE, FB_BYTE + FB_SIZE) = 0x22000000 + 8 MiB
//     (ovl_plane.sv:141-147; h264/ring_iface.svh:51-52; hps/lib/h264ring.c:766-785);
//   * 8888 pixels are bytes B, G, R, A with straight alpha; the core premultiplies
//     (ovl_plane.sv:446-467; h264ring.h:282-284).
// Pages live in /dev/fb0 (h264ring_fb_map, h264ring_linux.c:193-236), never /dev/mem: the idle
// framebuffer at 0x31600000 is outside the overlay's region and is never touched here.

#include "h264ring.h"

#include <algorithm>
#include <cstdint>

namespace misterplex::owenb_osd {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

inline Rect intersect(const Rect& a, const Rect& b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

inline bool inside(const Rect& in, const Rect& out) {
    return !in.empty() && in.x >= out.x && in.y >= out.y && in.right() <= out.right() &&
           in.bottom() <= out.bottom();
}

// The output modes of the spec's §4 table.
enum class OutMode : uint8_t { NativeHd, NativeSd, Progressive, Interlaced };

inline const char* modeName(OutMode m) {
    switch (m) {
    case OutMode::NativeHd: return "native-hd";
    case OutMode::NativeSd: return "native-sd";
    case OutMode::Progressive: return "progressive";
    case OutMode::Interlaced: return "interlaced";
    }
    return "?";
}

// What the renderer reads from the status block, decoded (h264ring_raster + h264ring_video_out).
struct Raster {
    unsigned w = 720, h = 480;
    bool interlaced = false;
    unsigned arX = 0, arY = 0;       // word 47: the raster is shown arX : arY wide on HDMI
    bool lb = false;                 // A': letterboxed inside the raster (bars are the raster's)
    int picX0 = 0, picY0 = 0;        // the picture's rectangle on the raster (negative under Crop)
    unsigned picW = 0, picH = 0;     // 0 while no picture is fitted
    unsigned videoOut = H264RING_VOUT_PROGRESSIVE;
    bool operator==(const Raster& o) const {
        return w == o.w && h == o.h && interlaced == o.interlaced && arX == o.arX && arY == o.arY &&
               lb == o.lb && picX0 == o.picX0 && picY0 == o.picY0 && picW == o.picW && picH == o.picH &&
               videoOut == o.videoOut;
    }
    bool operator!=(const Raster& o) const { return !(*this == o); }
};

inline Raster fromCore(const struct ::h264ring_raster& r, unsigned videoOut) {
    Raster o;
    o.w = r.w ? r.w : 720u;
    o.h = r.h ? r.h : 480u;
    o.interlaced = r.interlaced != 0;
    o.arX = r.ar_x;
    o.arY = r.ar_y;
    o.lb = r.lb != 0;
    o.picX0 = r.pic_x0;
    o.picY0 = r.pic_y0;
    o.picW = r.pic_w;
    o.picH = r.pic_h;
    o.videoOut = videoOut;
    return o;
}

inline OutMode modeOf(const Raster& r) {
    if (r.interlaced)
        return OutMode::Interlaced;
    if (r.videoOut == H264RING_VOUT_NATIVE)
        return r.h >= 720 ? OutMode::NativeHd : OutMode::NativeSd;
    return OutMode::Progressive;
}

// The geometry one redraw uses: the canvas (= the raster), the safe rect, the picture, the bar.
struct Layout {
    OutMode mode = OutMode::Progressive;
    unsigned W = 720, H = 480;
    Rect canvas, safe, pic, bar;
    unsigned bandH = 80;
    double par = 1.0;          // the pixel aspect (ui.c:464): glyph columns are sy / par wide
    bool evenLines = false;    // 480i / 576i: even y, strokes >= 2 lines (the fabric's (1,2,1) stays on)
    bool operator==(const Layout& o) const {
        return mode == o.mode && W == o.W && H == o.H && safe == o.safe && pic == o.pic && bar == o.bar &&
               par == o.par && evenLines == o.evenLines;
    }
    bool operator!=(const Layout& o) const { return !(*this == o); }
};

inline Layout layoutFor(const Raster& r) {
    Layout L;
    L.mode = modeOf(r);
    L.W = std::min<unsigned>(std::max(1u, r.w), H264RING_OVL_MAX_W);
    L.H = std::min<unsigned>(std::max(1u, r.h), H264RING_OVL_MAX_H);
    L.canvas = {0, 0, static_cast<int>(L.W), static_cast<int>(L.H)};
    int mx, my;
    switch (L.mode) {
    case OutMode::NativeHd: mx = 40; my = 28; L.bandH = 96; break;
    case OutMode::NativeSd: mx = static_cast<int>(L.W / 10); my = static_cast<int>(L.H / 10); L.bandH = 80; break;
    default: mx = 72; my = L.H >= 576 ? 57 : 48; L.bandH = 80; break;   // 480p / 480i / 576
    }
    L.evenLines = L.mode == OutMode::Interlaced;
    if (L.evenLines)
        my += my & 1;
    L.safe = {mx, my, static_cast<int>(L.W) - 2 * mx, static_cast<int>(L.H) - 2 * my};
    const double aspect = (!r.interlaced && r.arX && r.arY) ? double(r.arX) / double(r.arY) : 4.0 / 3.0;
    L.par = aspect * double(L.H) / double(L.W);
    // The picture (clamped to the raster: a Crop rect has x0 / y0 < 0); no picture yet = the raster.
    L.pic = (r.picW && r.picH)
                ? intersect({r.picX0, r.picY0, static_cast<int>(r.picW), static_cast<int>(r.picH)}, L.canvas)
                : L.canvas;
    if (L.pic.empty())
        L.pic = L.canvas;
    Rect area = intersect(L.pic, L.safe);
    if (area.empty())
        area = L.safe;
    // The bar: the bottom of (picture ∩ safe) -- inside the active picture, never in the bars (§4).
    int h = std::min<int>(static_cast<int>(L.bandH), area.h);
    int y = area.bottom() - h;
    if (L.evenLines) {
        h &= ~1;
        y = (area.bottom() - h) & ~1;
        if (y < area.y) {
            y += 2;
            h -= 2;
        }
    }
    L.bar = {area.x, y, area.w, h};
    return L;
}

// Where the pages lie in fb0 (§1, Decisions 2-3): double-buffered 8888 when fb0's length allows,
// else ONE page (hide, wait, draw, show: one blank frame per redraw). Planned from smem_len, never
// from the 8 MiB region (a page past fb0's end is memory we did not map).
enum class Buffering : uint8_t { None, Single, Double };

struct PagePlan {
    Buffering buf = Buffering::None;
    bool compact = false;          // the band trick: page 1 at phys + bandH x stride (§1)
    unsigned W = 0, H = 0;
    uint32_t stride = 0;
    uint32_t base[2] = {0, 0};
    uint16_t bandFirst = 0, bandLast = 0;
    bool operator==(const PagePlan& o) const {
        return buf == o.buf && compact == o.compact && W == o.W && H == o.H && stride == o.stride &&
               base[0] == o.base[0] && base[1] == o.base[1] && bandFirst == o.bandFirst && bandLast == o.bandLast;
    }
    bool operator!=(const PagePlan& o) const { return !(*this == o); }
    int pages() const { return buf == Buffering::Double ? 2 : buf == Buffering::Single ? 1 : 0; }
};

inline const char* bufferingName(Buffering b) {
    return b == Buffering::Double ? "double" : b == Buffering::Single ? "single" : "none";
}

inline struct ::h264ring_overlay descriptorFor(const PagePlan& p, int page) {
    struct ::h264ring_overlay o{};
    o.base = p.base[page & 1];
    o.width = static_cast<uint16_t>(p.W);
    o.height = static_cast<uint16_t>(p.H);
    o.stride = static_cast<uint16_t>(p.stride);
    o.format = H264RING_OVL_FMT_8888;
    o.band_first = p.bandFirst;
    o.band_last = p.bandLast;
    return o;
}

inline uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }

// One page at `base` is usable when the core would take it (h264ring_overlay_check == 0: the
// whole CANVAS inside the 8 MiB region) and every byte we write -- the band's lines -- lies in
// fb0's mapping [phys, phys + smemLen).
inline bool pageFits(uint32_t base, const PagePlan& p, uint32_t phys, uint32_t smemLen) {
    struct ::h264ring_overlay o = descriptorFor(p, 0);
    o.base = base;
    if (h264ring_overlay_check(&o) != 0)
        return false;
    const uint64_t bandLo = uint64_t(base) + uint64_t(p.bandFirst) * p.stride;
    const uint64_t bandHi = uint64_t(base) + uint64_t(p.bandLast) * p.stride + uint64_t(p.W) * 4u;
    return bandLo >= phys && bandHi <= uint64_t(phys) + smemLen;
}

// Plan the pages for a W x H canvas whose drawn lines are [bandFirst, bandLast], in fb0 at `phys`
// of `smemLen` bytes. Preference: double (canvas-disjoint pages), double compact (page 1 at
// phys + bandH x stride: its canvas overlaps page 0's but its band does not, §1), single (the
// runtime fallback of Decision 3), none. allowDouble = false forces the single path
// (MPX_OB_OSD_SINGLE=1, the lab switch for "8 MiB fails on glass").
inline PagePlan planPages(uint32_t phys, uint32_t smemLen, unsigned W, unsigned H, unsigned bandFirst,
                          unsigned bandLast, bool allowDouble = true) {
#ifdef MUT_OSD_8MB_NOT_SMEM
    // mutant: plan from the 8 MiB region instead of fb0's own length
    smemLen = (phys >= H264RING_FB_BYTE && phys < H264RING_FB_BYTE + H264RING_FB_SIZE)
                  ? H264RING_FB_BYTE + H264RING_FB_SIZE - phys
                  : smemLen;
#endif
    PagePlan p;
    p.W = W;
    p.H = H;
    p.stride = align8(W * 4u);
    p.bandFirst = static_cast<uint16_t>(std::min(bandFirst, H ? H - 1 : 0));
    p.bandLast = static_cast<uint16_t>(std::min(std::max(bandLast, unsigned(p.bandFirst)), H ? H - 1 : 0));
    if (W == 0 || H == 0 || W > H264RING_OVL_MAX_W || H > H264RING_OVL_MAX_H || (phys & 7u))
        return PagePlan{};
    const uint32_t bandBytes = uint32_t(p.bandLast - p.bandFirst + 1u) * p.stride;
    if (allowDouble) {
        p.buf = Buffering::Double;
        p.base[0] = phys;
        p.base[1] = phys + H * p.stride;
        if (pageFits(p.base[0], p, phys, smemLen) && pageFits(p.base[1], p, phys, smemLen))
            return p;
        p.compact = true;
        p.base[1] = phys + bandBytes;   // page 1's band starts where page 0's ends
        if (pageFits(p.base[0], p, phys, smemLen) && pageFits(p.base[1], p, phys, smemLen))
            return p;
        p.compact = false;
    }
    p.buf = Buffering::Single;
    p.base[0] = p.base[1] = phys;
    if (pageFits(p.base[0], p, phys, smemLen))
        return p;
    return PagePlan{};
}

} // namespace misterplex::owenb_osd
