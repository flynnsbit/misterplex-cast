// OB port: the PlexUI full-screen surface in the OB overlay renderer (owenb_osd_renderer.hpp).
//   S1 720p raster, fb0 3686400 (one page = all of smem): single page shown once, then updated
//      in place, dirty rows only, never hidden between frames (no blank frame per nav step).
//   S2 720x480 raster: double-buffered; the shown page always holds the latest frame.
//   S3 a surface larger than the canvas is refused (logged), the OSD stays.
//   S4 closing the surface hands back to the idle card; a play (presented) never shows it.
#include "libmisterplex/owenb_osd_renderer.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace misterplex::owenb_osd;
static int g_checks = 0, g_fail = 0;
#define CHECK(c, msg) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

struct Port final : IOverlayPort {
    Raster raster;
    uint32_t writtenGen = 0, gen = 0;
    bool wEnable = false;
    unsigned state = H264RING_OVL_S_OFF;
    struct ::h264ring_overlay wDesc{}, shown{};
    std::vector<std::string> ops;
    int show(const struct ::h264ring_overlay& o) override {
        if (h264ring_overlay_check(&o)) return -EINVAL;
        wDesc = o; wEnable = true; ops.push_back("show"); return int(++writtenGen);
    }
    int hide() override { wEnable = false; ops.push_back("hide"); return int(++writtenGen); }
    OvlStatus status() override {
        OvlStatus s; s.valid = true; s.gen = gen; s.state = state; s.raster = raster; s.session = true; return s;
    }
    int requestSettings(unsigned, const unsigned*) override { return 1; }
    int clearSettings() override { return 0; }
    void vblank() {
        if (writtenGen == gen) return;
        gen = writtenGen;
        state = wEnable ? H264RING_OVL_S_ACCEPTED : H264RING_OVL_S_OFF;
        if (wEnable) shown = wDesc;
    }
};

struct Rig {
    Rig(unsigned rw, unsigned rh, uint32_t len) : mem(len, 0), fb{mem.data(), H264RING_FB_BYTE, len},
        r(port, fb, true, [this](const std::string& s) { logs.push_back(s); }) {
        port.raster.w = rw; port.raster.h = rh; port.raster.videoOut = H264RING_VOUT_NATIVE;
        r.setOnSurfaceShown([this](uint64_t g) { shownGens.push_back(g); });
    }
    std::vector<uint8_t> mem;
    FbMap fb;
    Port port;
    std::vector<std::string> logs;
    std::vector<uint64_t> shownGens;
    Renderer r;
    int64_t now = 0, next = 0;
    UiState ui;
    // like OsdThread: a UiBus update wakes the renderer at once (tests set next = now on a change)
    void run(int64_t ms) {
        for (int64_t end = now + ms; now < end; ++now) {
            if (now % 16 == 0) port.vblank();
            if (now >= next) next = now + std::max<int64_t>(1, r.tick(now, ui));
        }
    }
    // the page the core reads now equals frame f
    bool shownEquals(const FullSurface& f) const {
        if (!port.wEnable || port.gen != port.writtenGen) return false;
        const uint8_t* p = mem.data() + (port.shown.base - H264RING_FB_BYTE);
        for (unsigned y = 0; y < f.h; ++y)
            if (std::memcmp(p + size_t(y) * port.shown.stride, f.bgra.data() + size_t(y) * f.w * 4, f.w * 4)) return false;
        return port.shown.width == f.w && port.shown.height == f.h;
    }
};

static std::shared_ptr<FullSurface> frame(unsigned w, unsigned h, uint64_t gen, unsigned focusRow) {
    auto f = std::make_shared<FullSurface>();
    f->w = w; f->h = h; f->gen = gen; f->bgra.assign(size_t(w) * h * 4, 0);
    for (size_t i = 0; i < f->bgra.size(); i += 4) { f->bgra[i] = 0x10; f->bgra[i + 1] = 0x10; f->bgra[i + 2] = 0x12; f->bgra[i + 3] = 255; }
    for (unsigned y = focusRow; y < focusRow + 40 && y < h; ++y)   // a "focused poster" band
        for (unsigned x = 100; x < 300 && x < w; ++x) { uint8_t* p = &f->bgra[(size_t(y) * w + x) * 4]; p[0] = 0x0D; p[1] = 0xA0; p[2] = 0xE5; }
    return f;
}

int main() {
    {   // S1: 720p, fb0 = one page: in place after the first show, no hide between frames
        Rig g(1280, 720, 3686400u);
        g.ui.idle = true; g.run(300);
        CHECK(g.port.wEnable, "S1 idle card up first");
        auto f1 = frame(1280, 720, 1, 100);
        g.ui.surface = f1; g.run(200);
        CHECK(g.r.surfaceMode() && g.shownEquals(*f1), "S1 surface 1 shown at 1280x720");
        const long hides0 = std::count(g.port.ops.begin(), g.port.ops.end(), std::string("hide"));
        const long shows0 = std::count(g.port.ops.begin(), g.port.ops.end(), std::string("show"));
        const uint64_t bytes0 = g.r.stats().surfBytes;
        for (uint64_t k = 2; k <= 12; ++k) {
            auto f = frame(1280, 720, k, 100 + unsigned(k) * 37);
            g.ui.surface = f; g.next = g.now; g.run(40);
            CHECK(g.shownEquals(*f), "S1 the core's page holds the latest frame");
        }
        CHECK(std::count(g.port.ops.begin(), g.port.ops.end(), std::string("hide")) == hides0,
              "S1 no hide (blank frame) between surface frames");
        CHECK(std::count(g.port.ops.begin(), g.port.ops.end(), std::string("show")) == shows0,
              "S1 in place: no new descriptor per frame");
        const uint64_t perFrame = (g.r.stats().surfBytes - bytes0) / 11;
        CHECK(perFrame <= 80u * 200u * 4u, "S1 dirty rows only (two 40-row bands of 200 px)");
        CHECK(g.r.stats().surfInPlace == 11, "S1 11 in-place updates");
        CHECK(g.shownGens.size() >= 12 && g.shownGens.back() == 12, "S1 onSurfaceShown reports every frame");
        bool logged = false; for (auto& l : g.logs) logged |= l.find("plexui single-buffered") != std::string::npos;
        CHECK(logged, "S1 plan logged");
    }
    {   // S2: 720x480 canvas, fb0 3686400 holds two pages: double-buffered, latest frame on screen
        Rig g(720, 480, 3686400u);
        for (uint64_t k = 1; k <= 9; ++k) {
            auto f = frame(720, 480, k, 20 + unsigned(k) * 41);
            g.ui.surface = f; g.next = g.now; g.run(60);
            CHECK(g.shownEquals(*f), "S2 shown page = latest frame (page shadows correct)");
        }
        CHECK(g.r.plan().buf == Buffering::Double && g.r.stats().surfInPlace == 0, "S2 double, never in place");
        CHECK(std::count(g.port.ops.begin(), g.port.ops.end(), std::string("hide")) == 0, "S2 no hide");
        CHECK(g.shownGens.size() == 9, "S2 shown reported after ovl_gen");
    }
    {   // S3: a 1280x720 surface on a 720x480 canvas is refused; nothing drawn from it
        Rig g(720, 480, 3686400u);
        g.ui.surface = frame(1280, 720, 1, 0); g.run(200);
        CHECK(!g.port.wEnable && g.r.stats().surfTooBig > 0, "S3 refused");
        bool logged = false; for (auto& l : g.logs) logged |= l.find("does not fit the canvas") != std::string::npos;
        CHECK(logged, "S3 logged");
    }
    {   // S4: close -> idle card back; presented (play) -> surface never resolved
        Rig g(1280, 720, 3686400u);
        g.ui.idle = true; g.ui.surface = frame(1280, 720, 1, 0); g.run(200);
        CHECK(g.r.surfaceMode(), "S4 surface up");
        g.ui.surface.reset(); g.run(300);
        CHECK(!g.r.surfaceMode() && g.port.wEnable && g.port.shown.width == 1280, "S4 idle card drawn again");
        UiModel m; m.idle = true; m.surface = frame(8, 8, 1, 0);
        CHECK(m.resolve(0).surface != nullptr, "S4 idle: surface resolved");
        m.presented = true;
        CHECK(m.resolve(0).surface == nullptr, "S4 a play hides it");
        UiBus bus; bus.noteCanvas(1280, 720);
        CHECK(bus.canvasW() == 1280 && bus.canvasH() == 720, "S4 canvas published");
    }
    std::printf("test_owenb_osd_plexui: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
