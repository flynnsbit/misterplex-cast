#pragma once
// OWENB-P1B T7: the idle overlay's pages for a PlexOB session (replaces 2da3f48d's four 1555 pages laid
// out from H264RING_FB_BYTE regardless of fb0, T6 gaps G8).
//
// There are no fixed pages any more. The idle card, the error screens, the cast bar and the toasts are
// all drawn by owenb_osd into one 8888 canvas the size of the core's raster (centred, ovl_plane.sv:250-251).
// That canvas is double-buffered inside fb0 exactly as h264ring_fb_map gave it ([phys, phys + smem_len)
// within [0x22000000, +8 MiB), ring_iface.svh:51-52, h264ring_linux.c:193), or single-buffered with a blank
// frame when only one fits (spec Decision 3; 1920x1080 native always, review 10:08). Every page passes
// h264ring_overlay_check. 0x31600000 (the idle framebuffer) is outside the region, so it is never used.
// This header plans; it maps nothing.

#include "owenb_osd_layout.hpp"

#include <cstdint>

namespace misterplex {

struct OwenbIdleOverlay {
    owenb_osd::Layout layout;   // the canvas, title-safe, the picture, the bar band for this raster
    owenb_osd::PagePlan plan;   // buf None: no page fits fb0 (no overlay)
};

// The whole canvas as the band: the error screens are full-canvas (ARM-RENDERER-SPEC §3), so an idle
// session's pages must hold every line. A zero raster is the fitted 720x480 canvas.
inline OwenbIdleOverlay owenbIdleOverlay(const struct ::h264ring_raster& raster, unsigned videoOut, uint32_t fbPhys,
                                         uint32_t fbLen, bool allowDouble = true) {
    OwenbIdleOverlay o;
    o.layout = owenb_osd::layoutFor(owenb_osd::fromCore(raster, videoOut));
    o.plan = owenb_osd::planPages(fbPhys, fbLen, o.layout.W, o.layout.H, 0, o.layout.H - 1, allowDouble);
    return o;
}

} // namespace misterplex
