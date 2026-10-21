// OWENB-P1B T7: the idle overlay's page plan (owenb_overlay.hpp). It replaces the four 1555 pages from
// H264RING_FB_BYTE: the pages come from fb0 as h264ring_fb_map gave it, 8888, double-buffered when two
// canvases fit (single + a blank frame otherwise; 1080 native always), never 0x31600000.
#include "libmisterplex/owenb_overlay.hpp"

#include <cstdio>

namespace {

int fails = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

struct h264ring_raster rast(unsigned w, unsigned h, bool il) {
    struct h264ring_raster r{};
    r.w = w;
    r.h = h;
    r.interlaced = il;
    r.ar_x = 4;
    r.ar_y = 3;
    return r;
}

// every page the plan uses: 8888, the core's check, the whole band inside fb0
void checkPages(const misterplex::OwenbIdleOverlay& o, uint32_t phys, uint32_t len, const char* label) {
    const int pages = o.plan.buf == misterplex::owenb_osd::Buffering::Double ? 2 : 1;
    for (int i = 0; i < pages; ++i) {
        const struct h264ring_overlay d = misterplex::owenb_osd::descriptorFor(o.plan, i);
        const uint64_t end = uint64_t(d.base) + uint64_t(d.band_last) * d.stride + uint64_t(d.width) * 4u;
        if (h264ring_overlay_check(&d) != H264RING_OVL_E_NONE || d.format != H264RING_OVL_FMT_8888 ||
            d.base < phys || end > uint64_t(phys) + len || d.band_first != 0 || d.band_last != d.height - 1) {
            std::fprintf(stderr, "FAIL %s page %d (base %#x end %#llx)\n", label, i, d.base,
                         static_cast<unsigned long long>(end));
            ++fails;
        }
    }
}

} // namespace

int main() {
    using misterplex::owenb_osd::Buffering;
    const uint32_t fb = H264RING_FB_BYTE, full = H264RING_FB_SIZE;
    const auto p480 = misterplex::owenbIdleOverlay(rast(720, 480, false), H264RING_VOUT_PROGRESSIVE, fb, full);
    check(p480.plan.buf == Buffering::Double && p480.plan.W == 720 && p480.plan.H == 480,
          "720x480 progressive: double-buffered 8888 in the full 8 MiB (Decision 2)");
    checkPages(p480, fb, full, "720x480");
    check(p480.plan.base[1] - p480.plan.base[0] >= 480u * p480.plan.stride, "the two canvases are disjoint");

    const uint32_t off = fb + 0x100000u, offLen = full - 0x100000u;   // fb0 not at the region's start
    const auto shifted = misterplex::owenbIdleOverlay(rast(720, 480, true), H264RING_VOUT_INTERLACED, off, offLen);
    check(shifted.plan.buf == Buffering::Double && shifted.plan.base[0] == off,
          "from fb.phys, not H264RING_FB_BYTE (the old plan's flaw)");
    checkPages(shifted, off, offLen, "480i at fb+1 MiB");

    const auto hd = misterplex::owenbIdleOverlay(rast(1280, 720, false), H264RING_VOUT_NATIVE, fb, full);
    check(hd.plan.buf == Buffering::Double, "1280x720 native: double");
    checkPages(hd, fb, full, "1280x720");

    const auto fhd = misterplex::owenbIdleOverlay(rast(1920, 1080, false), H264RING_VOUT_NATIVE, fb, full);
    check(fhd.plan.buf == Buffering::Single, "1920x1080 native: single + blank (accepted, review 10:08)");
    checkPages(fhd, fb, full, "1920x1080");

    const auto small = misterplex::owenbIdleOverlay(rast(720, 480, false), H264RING_VOUT_PROGRESSIVE, fb, 1382400u);
    check(small.plan.buf == Buffering::Single, "a 1.38 MB fb0: single (the runtime fallback, Decision 3)");
    checkPages(small, fb, 1382400u, "720x480 in 1.38 MB");

    const auto forced = misterplex::owenbIdleOverlay(rast(720, 480, false), H264RING_VOUT_PROGRESSIVE, fb, full, false);
    check(forced.plan.buf == Buffering::Single, "MPX_OB_OSD_SINGLE=1: single");

    const auto idleFb = misterplex::owenbIdleOverlay(rast(720, 480, false), H264RING_VOUT_PROGRESSIVE, 0x31600000u, full);
    check(idleFb.plan.buf == Buffering::None, "0x31600000 (the idle framebuffer) is never a page");

    struct h264ring_raster empty{};
    const auto fallback = misterplex::owenbIdleOverlay(empty, 0, fb, full);
    check(fallback.plan.W == 720 && fallback.plan.H == 480 && fallback.plan.buf == Buffering::Double,
          "a zero raster: the fitted 720x480 canvas");

    if (fails) {
        std::fprintf(stderr, "test_owenb_overlay: %d FAIL\n", fails);
        return 1;
    }
    std::puts("test_owenb_overlay: OK");
    return 0;
}
