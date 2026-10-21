// rc3 gate-7 host replay: the daemon's OB play worker (OwenbPlayer + OwenbPlaySession + the real libav AvSource)
// against the paced fake core, idle session armed, fb0 overlay thread on a heap "fb0", a PlexUI-sized
// FullSurface shown/hidden each cycle. N plays of an HTTP MPEG-TS (the yt.ts copy-remux shape), each stopped
// after HOLD ms. Prints RSS + mallinfo2 per cycle. Run under heaptrack / valgrind massif / ASan+LSan.
#include "libmisterplex/owenb_play.hpp"
#include "owenb_av_source.hpp"
#include "fake_owenb_core.hpp"
#include <malloc.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace misterplex::owenb;
static long rssKb() { std::ifstream f("/proc/self/status"); std::string k; long v = 0;
    while (f >> k) { if (k == "VmRSS:") { f >> v; return v; } f.ignore(4096, '\n'); } return 0; }
struct ProxyWindow final : OwenbDdrWindow {
    explicit ProxyWindow(HeapDdrWindow& h) : w(h) {}
    HeapDdrWindow& w;
    volatile uint64_t* ctrl() override { return w.ctrl(); }
    volatile uint64_t* ring() override { return w.ring(); }
    uint32_t ringSize() const override { return w.ringSize(); }
    bool mapped() const override { return true; }
    void sleepUs(unsigned us) override { w.sleepUs(us); }
    uint64_t nowUs() override { return w.nowUs(); }
};
int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:18999/yt.ts";
    const int n = argc > 2 ? std::atoi(argv[2]) : 10, holdMs = argc > 3 ? std::atoi(argv[3]) : 4000;
    std::unique_ptr<HeapDdrWindow> heap;
    std::unique_ptr<fakeob::PacedCore> core;
    std::mutex coreMu;
    std::vector<uint8_t> fb(1280u * 720u * 4u * 2u);
    auto player = std::make_unique<OwenbPlayer>(
        [&](std::string&) -> std::unique_ptr<OwenbDdrWindow> {
            core.reset();
            heap = std::make_unique<HeapDdrWindow>(1u << 20);
            core = std::make_unique<fakeob::PacedCore>(*heap);
            return std::make_unique<ProxyWindow>(*heap);
        },
        [](const OpenRequest& q, std::string& e) { return openAvSource(q, e); });
    OsdConfig oc;
    oc.mapFb = [&](misterplex::owenb_osd::FbMap& m, std::string&) { m.virt = fb.data(); m.phys = 0x30000000; m.len = uint32_t(fb.size()); return true; };
    oc.unmapFb = [] {};
    player->setOsd(oc);
    IdleConfig ic;
    ic.name = "MiSTerPlex-host";
    ic.log = [](const std::string&) {};
    ic.stepLocked = [&] { std::lock_guard<std::mutex> g(coreMu); if (heap) heap->sleepUs(100); if (core) core->sessions.clear(); };
    std::string err;
    if (!player->startIdle(ic, err)) { std::printf("idle: %s\n", err.c_str()); return 2; }
    std::printf("c0 rss_kb=%ld\n", rssKb());
    for (int c = 1; c <= n; ++c) {
        OpenRequest q; q.url = url;
        SessionConfig cfg; cfg.idleUs = 1000; cfg.cavlcMaxKbps = 0;
        cfg.sleepUs = [&](unsigned) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            std::lock_guard<std::mutex> g(coreMu);
            if (core) { core->win.sleepUs(300); core->sessions.clear(); }
        };
        OwenbPlayCallbacks cb; cb.log = [](const std::string&) {};
        if (!player->start(q, 100 + c, cfg, cb, err)) { std::printf("start: %s\n", err.c_str()); return 3; }
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        player->stop();
        auto s = std::make_shared<misterplex::owenb_osd::FullSurface>();   // PlexUI open/close over the idle card
        s->w = 1280; s->h = 720; s->bgra.assign(size_t(1280) * 720 * 4, 0x40); s->gen = c;
        player->ui().setSurface(std::move(s));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        player->ui().setSurface(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const struct mallinfo2 mi = mallinfo2();
        std::printf("c%d rss_kb=%ld inuse_kb=%zu arena_kb=%zu mmap_kb=%zu\n", c, rssKb(), mi.uordblks / 1024,
                    mi.arena / 1024, mi.hblkhd / 1024);
        std::fflush(stdout);
    }
    player->stopIdle();
    player.reset();
    std::printf("done rss_kb=%ld\n", rssKb());
    return 0;
}
