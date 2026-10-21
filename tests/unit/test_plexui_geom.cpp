// OB port: render geometry. (1) R1 640x480 RGB24 frames stay byte-identical to the
// pre-geometry renderer (golden FNV-1a hashes recorded on 853cd5a6) across every view, focus
// moves, grid scroll, hub scroll and focus animation (static-layer patch paths included).
// (2) The OB canvas (1280x720, 720x480) renders BGRA at the canvas size with a scaled layout.
#include "plexui_render.hpp"
#include "plexui_controller.hpp"
#include "plexui_data.hpp"
#include <chrono>
#include <thread>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace misterplex::plexui;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t fnv(const std::vector<std::uint8_t>& f) {
    uint64_t h = 1469598103934665603ull;
    for (auto b : f) { h ^= b; h *= 1099511628211ull; }
    return h;
}
static PlexItem item(int i) {
    PlexItem it; it.ratingKey = std::to_string(1000 + i); it.title = "Title number " + std::to_string(i);
    it.year = "19" + std::to_string(60 + i % 40); it.thumb = "/t/" + std::to_string(i); return it;
}
// Scene script: returns the frame hashes in order.
static std::vector<uint64_t> script(Renderer& r, std::vector<std::vector<std::uint8_t>>* keep) {
    std::vector<uint64_t> out;
    RenderState st;
    PlexServer s; s.name = "Home"; s.owned = true; s.machineIdentifier = "m1"; s.baseUrl = "http://h";
    PlexServer s2 = s; s2.name = "Shared box"; s2.owned = false;
    st.servers = {s, s2}; st.server = s; st.status = "status line";
    std::vector<std::uint8_t> f;
    auto shot = [&] { f.clear(); r.render(st, f); out.push_back(fnv(f)); if (keep) keep->push_back(f); };
    st.view = ViewId::ServerList; shot(); st.focus.row = 1; shot();
    st.haveServer = true; st.view = ViewId::Home; st.focus = UiFocus{};
    for (int k = 0; k < 6; ++k) { SidebarLibraryEntry e; e.title = "Library with a long name " + std::to_string(k);
        e.subtitle = "Home"; e.type = k % 2 ? "movie" : "show"; st.sidebarEntries.push_back(e); }
    for (int h = 0; h < 3; ++h) { PlexHubRow row; row.title = "Hub " + std::to_string(h);
        for (int i = 0; i < 12; ++i) row.items.push_back(item(h * 20 + i)); st.hubs.push_back(row); }
    st.dataGeneration = 1; shot();
    st.focus.sidebar = 2; shot(); st.focus.sidebar = 0;
    for (int c = 0; c < 9; ++c) { st.focus.col = c; for (int a = 0; a <= 3; ++a) { st.focusAnim = a; shot(); } }
    st.focus.row = 1; st.focus.col = 2; st.focusAnim = 3; shot();
    st.view = ViewId::LibraryGrid; st.focus = UiFocus{}; st.dataGeneration = 2;
    for (int i = 0; i < 60; ++i) st.grid.push_back(item(i));
    st.gridSectionTitle = "Movies"; st.gridFilterChipTitle = "All"; st.gridItemCount = 60;
    for (const char* t : {"All", "Unwatched", "Recently added", "Genre", "Year", "Decade", "Rating"}) {
        GridCategoryChip ch; ch.title = t; st.gridChips.push_back(ch); }
    shot(); st.focus.gridOnChips = true; st.focus.gridChip = 3; shot(); st.focus.gridOnChips = false;
    for (int k = 0; k < 14; ++k) { st.focus.col = k % 5; st.focus.row = k / 3;
        st.gridScrollRow = st.focus.row > 1 ? st.focus.row - 1 : 0; st.focusAnim = k % 4; shot(); }
    st.view = ViewId::Detail; st.dataGeneration = 3; st.detail = item(7);
    st.detail.summary = "A long summary that wraps over several lines of the detail page so the wrapped text path runs.";
    st.detail.tagline = "Tagline here"; st.detail.genres = "Drama, Sci-Fi"; st.detail.runtimeMs = 5400000;
    st.detail.contentRating = "PG"; st.detail.audienceRating = 7.5f; st.detail.viewOffsetMs = 1000;
    shot(); st.focus.detailButton = 1; shot();
    return out;
}
#ifdef GEOM_GOLDEN_DUMP
int main(int argc, char** argv) {
    Renderer r; std::string err;
    if (!r.initFonts(argc > 1 ? argv[1] : "host/libmisterplex/plexui/third_party/", err)) { std::printf("fonts %s\n", err.c_str()); return 1; }
    const auto h = script(r, nullptr);
    std::printf("static const uint64_t kGolden[%zu] = {\n", h.size());
    for (auto v : h) std::printf("    0x%016llxull,\n", static_cast<unsigned long long>(v));
    std::printf("};\n");
    return 0;
}
#endif
#ifndef GEOM_GOLDEN_DUMP
// R1 frames. Home hashes include the sidebar focus pill. Left into the sidebar
// redraws that strip only; the backdrop is rebuilt when its URL changes.
// Regenerated with tests/unit/test_plexui_geom.cpp -DGEOM_GOLDEN_DUMP.
static const uint64_t kGolden[59] = {
    0xd250fbbf84de509aull,
    0xe17b4eda004871edull,
    0xb56219aada7ef63full,
    0x75fb614f734311efull,
    0x1d23cd6b27c132adull,
    0x2714de8b9f73fa9eull,
    0x40f3fa9f26cee7f4ull,
    0xb56219aada7ef63full,
    0x64019ec05bcfee63ull,
    0x1c29471ea1898b09ull,
    0x36738e801f06039bull,
    0x56d1307a72b0f524ull,
    0x201094d08facfc76ull,
    0x085176f84ca7de85ull,
    0xaf81e51aa6560587ull,
    0xc32b531b8631db75ull,
    0x24987d0ad2ae27ddull,
    0x6ca4b59476aaf939ull,
    0x5bfa8a8e0d02ab63ull,
    0xaa600c5a4cc3cb74ull,
    0x6ed7f5f43447fe4bull,
    0xbe87608f09efe14cull,
    0xf9a6d2f7d695164eull,
    0xae51ded6634d8b99ull,
    0xca1f060ab528aecaull,
    0x1d3c283b5630fe8cull,
    0x72e9019249cd6e1aull,
    0x1857e2dcc99a60d8ull,
    0xac208b048ee54a3full,
    0x99f54f19ea2eb449ull,
    0x1e63e7cd25d7c62full,
    0x7b1d9c1c866cc2f3ull,
    0xa3098aefda607479ull,
    0x9e8d655b290d0341ull,
    0xd82e8892e9007d2bull,
    0x51638a84ff118798ull,
    0xf7ca51c458aff2aaull,
    0x28282a669cf34ac9ull,
    0xb6cbede33623ac9full,
    0x74700c96725221ceull,
    0x1186f4bd33fa920full,
    0x552c79fce4c811aeull,
    0xcec8447b04c44e72ull,
    0x39e7863b03213e6aull,
    0x13b6de5e42c5bf3cull,
    0x8ea5b91c2755f059ull,
    0xb9e65805a1bad5c4ull,
    0x1bc7589504ae6976ull,
    0x8978c43816dcf088ull,
    0x6bad778ba6a765e8ull,
    0xa8aab17400bc4f9bull,
    0x9a1e908637aae0c4ull,
    0x032f3b35fb87a511ull,
    0x7fe5974d13122eeeull,
    0xba9930845b9c5c0cull,
    0x745a982ca7dc55ebull,
    0xcb64e139461814e5ull,
    0x5788466a57e6adeeull,
    0x5788466a57e6adeeull,
};
static void ppm(const char* path, const std::vector<std::uint8_t>& f, int w, int h, int bpp) {
    FILE* o = std::fopen(path, "wb"); if (!o) return;
    std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const std::uint8_t* p = f.data() + static_cast<std::size_t>(i) * bpp;
        const std::uint8_t rgb[3] = {bpp == 4 ? p[2] : p[0], p[1], bpp == 4 ? p[0] : p[2]};
        std::fwrite(rgb, 1, 3, o);
    }
    std::fclose(o);
}
int main(int argc, char** argv) {
    const std::string fonts = argc > 1 ? argv[1] : "host/libmisterplex/plexui/third_party/";
    const char* dump = argc > 2 ? argv[2] : nullptr;   // optional dir for PPM review shots
    {   // (1) R1 byte-identical
        Renderer r; std::string err; CHECK(r.initFonts(fonts, err));
        setScreenGeom(640, 480);
        CHECK(kScreenW() == 640 && kScreenH() == 480 && P(32) == 32 && kGridCols() == 5 && kSafeRight() == 72);
        std::vector<std::vector<std::uint8_t>> frames;
        const auto h = script(r, &frames);
        CHECK(h.size() == sizeof(kGolden) / sizeof(kGolden[0]));
        int bad = 0;
        for (size_t i = 0; i < h.size() && i < sizeof(kGolden) / sizeof(kGolden[0]); ++i)
            if (h[i] != kGolden[i]) { if (bad++ < 5) std::printf("R1 frame %zu differs\n", i); }
        CHECK(bad == 0);
        for (auto& f : frames) CHECK(f.size() == 640u * 480u * 3u);
    }
    for (auto wh : {std::pair<int, int>{1280, 720}, {720, 480}, {1920, 1080}}) {   // (2) OB canvases
        setScreenGeom(wh.first, wh.second);
        CHECK(kScreenW() == wh.first && kScreenH() == wh.second);
        CHECK(computeGridLayout().posterW > 0 && gridVisibleRowCount(computeGridLayout()) >= 2);
        Renderer r; std::string err; CHECK(r.initFonts(fonts, err));
        RenderState st; PlexServer s; s.name = "Home"; st.servers = {s}; st.view = ViewId::ServerList;
        std::vector<std::uint8_t> f; r.render(st, f, nullptr, PixelFormat::BGRA8888);
        CHECK(f.size() == static_cast<std::size_t>(wh.first) * wh.second * 4u);
        bool opaque = true; for (size_t i = 3; i < f.size(); i += 4) opaque &= f[i] == 255;
        CHECK(opaque);
        std::vector<std::vector<std::uint8_t>> frames;
        script(r, &frames);   // every view at this canvas: no OOB (ASan run), right size
        for (auto& fr : frames) CHECK(fr.size() == static_cast<std::size_t>(wh.first) * wh.second * 3u);
        if (dump && wh.first == 1280) {
            const int pick[] = {0, 3, 42, 44, 57};
            for (int k : pick) { char p[256]; std::snprintf(p, sizeof p, "%s/geom_%dx%d_%02d.ppm", dump, wh.first, wh.second, k);
                ppm(p, frames[static_cast<size_t>(k)], wh.first, wh.second, 3); }
        }
    }
    for (auto wh : {std::pair<int, int>{640, 480}, {1280, 720}}) {   // view-layer cache: Back restores, == fresh
        setScreenGeom(wh.first, wh.second);
        const PixelFormat pf = wh.first == 640 ? PixelFormat::RGB24 : PixelFormat::BGRA8888;
        RenderState st; PlexServer s; s.name = "Home"; s.owned = true; s.machineIdentifier = "m1"; s.baseUrl = "http://h";
        st.servers = {s}; st.server = s; st.haveServer = true; st.view = ViewId::Home; st.focusAnim = 3;
        for (int k = 0; k < 4; ++k) { SidebarLibraryEntry e; e.title = "Lib " + std::to_string(k); e.subtitle = "serverA"; st.sidebarEntries.push_back(e); }
        PlexHubRow row; row.title = "Hub"; for (int i = 0; i < 12; ++i) row.items.push_back(item(i)); st.hubs = {row, row};
        st.dataGeneration = 1;
        for (int i = 0; i < 30; ++i) st.grid.push_back(item(i));
        st.gridSectionTitle = "Movies"; st.gridItemCount = 30;
        Renderer a; std::string err; CHECK(a.initFonts(fonts, err));
        auto fresh = [&](const RenderState& x) { Renderer b; std::string e; b.initFonts(fonts, e); std::vector<std::uint8_t> f; b.render(x, f, nullptr, pf); return f; };
        std::vector<std::uint8_t> f; Renderer::RenderPerf rp;
        st.focus.col = 3; a.render(st, f, nullptr, pf);                       // Home, focus moved
        const RenderState home = st;
        st.view = ViewId::LibraryGrid; st.dataGeneration = 2; st.focus = UiFocus{}; st.focus.col = 2; a.render(st, f, nullptr, pf);
        const RenderState grid = st;
        st.view = ViewId::Detail; st.dataGeneration = 3; st.detail = item(2); a.render(st, f, nullptr, pf);
        a.render(grid, f, &rp, pf);                                           // Back -> grid (restored)
        CHECK(f == fresh(grid)); CHECK(rp.rebuild == 0);
        a.render(home, f, &rp, pf);                                           // Back -> home (restored)
        CHECK(f == fresh(home)); CHECK(rp.rebuild == 0);
    }
    {   // 0011: dead lab PMS (every address hangs 1.5 s then curl 28) never stalls the live servers
        auto conn = [](const char* addr, bool https) { PlexConnection c; c.protocol = https ? "https" : "http";
            c.address = addr; c.port = "32400"; c.local = true;
            c.uri = https ? std::string("https://") + addr + ".x.plex.direct:32400" : std::string("http://") + addr + ":32400"; return c; };
        PlexServer g; g.name = "serverA"; g.clientIdentifier = "g1"; g.accessToken = "t"; g.connections = {conn("192-0-2-1", true)};
        PlexServer j = g; j.name = "serverB"; j.clientIdentifier = "j1"; j.connections = {conn("192-0-2-2", true)};
        PlexServer st = g; st.name = "office"; st.clientIdentifier = "s1";
        st.connections = {conn("192-0-2-41", true), conn("203-0-113-21", true), conn("198-51-100-98", true), conn("198-58-116-125", true)};
        std::vector<PlexServer> v = {st, g, j};
        auto get = [](const std::string& url, int connectMs, int, const misterplex::BrowseCancelFn&) {
            misterplex::BrowseHttpResult r;
            if (url.find("192-0-2-2") != std::string::npos) std::this_thread::sleep_for(std::chrono::milliseconds(120));
            if (url.find("192-0-2-") == std::string::npos) {   // office: connect timeout
                std::this_thread::sleep_for(std::chrono::milliseconds(connectMs)); r.curlExit = 28; return r; }
            r.status = 200; r.curlExit = 0;
            r.body = std::string("<MediaContainer machineIdentifier=\"") + (url.find("192-0-2-1") != std::string::npos ? "g1" : "j1") + "\"/>";
            return r; };
        const auto t0 = std::chrono::steady_clock::now();
        std::mutex m; long long liveMs = -1; int updates = 0;
        resolveServerBasesProgressive(get, v, nullptr, [&](const std::vector<PlexServer>& snap) {
            std::lock_guard<std::mutex> lk(m); ++updates;
            if (liveMs < 0 && !snap[1].baseUrl.empty() && !snap[2].baseUrl.empty())
                liveMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); });
        const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("0011 dead lab PMS: live servers listed at %lld ms, all done %lld ms, updates %d\n", liveMs, totalMs, updates);
        CHECK(liveMs >= 0 && liveMs < 600);   // gate: < 2 s; the live list comes long before the dead probes end
        CHECK(v[0].offline && v[0].baseUrl.empty() && !v[1].offline && !v[2].offline);
        CHECK(totalMs < 2500);   // parallel: ~1.5 s (was 4 x 1.5-4 s serial)
        CHECK(updates == 3);
        CHECK(pickHomeServerIndex(v) != 0);   // never the offline one
    }
    {   // 0011 tie: plex.direct https (slower) beats plain http (faster) inside the grace window
        PlexServer sv; sv.name = "home"; sv.clientIdentifier = "h1"; sv.accessToken = "t";
        PlexConnection a1; a1.protocol = "https"; a1.address = "192.0.2.9"; a1.port = "32400"; a1.local = false;
        a1.uri = "https://192-0-2-9.x.plex.direct:32400";
        PlexConnection a2 = a1; a2.protocol = "http"; a2.uri = "";
        sv.connections = {a2, a1};
        std::vector<PlexServer> v = {sv};
        auto get = [](const std::string& url, int, int, const misterplex::BrowseCancelFn&) {
            if (url.find("plex.direct") != std::string::npos) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            misterplex::BrowseHttpResult r; r.status = 200; r.curlExit = 0; r.body = "<MediaContainer machineIdentifier=\"h1\"/>"; return r; };
        resolveServerBasesProgressive(get, v, nullptr, nullptr);
        std::printf("0011 tie: base=%s\n", v[0].baseUrl.c_str());
        CHECK(v[0].baseUrl.find("plex.direct") != std::string::npos);
    }
    {   // a connection answering for ANOTHER server is skipped (office via 198.51.100.17 on a lab MiSTer)
        PlexServer sv; sv.name = "office"; sv.clientIdentifier = "ff58"; sv.accessToken = "t";
        PlexConnection c1; c1.protocol = "https"; c1.address = "198.51.100.17"; c1.port = "32400"; c1.local = true;
        c1.uri = "https://203-0-113-17.x.plex.direct:32400";
        PlexConnection c2 = c1; c2.address = "192.0.2.41"; c2.uri = "https://192-0-2-41.x.plex.direct:32400";
        sv.connections = {c1, c2};
        std::vector<PlexServer> v = {sv};
        auto get = [](const std::string& url, int, int, const misterplex::BrowseCancelFn&) {
            misterplex::BrowseHttpResult r; r.status = 200; r.curlExit = 0;
            r.body = url.find("203-0-113-17") != std::string::npos ? "<MediaContainer machineIdentifier=\"other\"/>"
                                                                 : "<MediaContainer machineIdentifier=\"ff58\"/>";
            return r; };
        resolveServerBases(get, v, nullptr);
        CHECK(v[0].baseUrl.find("192-0-2-41") != std::string::npos);
    }
    for (auto wh : {std::pair<int, int>{640, 480}, {720, 480}, {1280, 720}}) {   // backdrop change keeps the chrome
        setScreenGeom(wh.first, wh.second);
        const PixelFormat pf = wh.first == 640 ? PixelFormat::RGB24 : PixelFormat::BGRA8888;
        RenderState st; PlexServer s; s.name = "Home"; s.owned = true; s.machineIdentifier = "m1"; s.baseUrl = "http://h";
        st.servers = {s}; st.server = s; st.haveServer = true; st.view = ViewId::Home; st.focusAnim = 3;
        for (int k = 0; k < 4; ++k) { SidebarLibraryEntry e; e.title = "Lib " + std::to_string(k); e.subtitle = "serverA"; st.sidebarEntries.push_back(e); }
        PlexHubRow row; row.title = "Hub"; for (int i = 0; i < 8; ++i) row.items.push_back(item(i)); st.hubs.push_back(row);
        st.dataGeneration = 1; st.backdropUrl = "/bd/1";
        Renderer a, b; std::string err; CHECK(a.initFonts(fonts, err) && b.initFonts(fonts, err));
        std::vector<std::uint8_t> f1, f2, f3; a.render(st, f1, nullptr, pf);
        st.backdropUrl = "/bd/2"; a.render(st, f2, nullptr, pf); b.render(st, f3, nullptr, pf);
        CHECK(f2 == f3);   // incremental == fresh (was: sidebar/titles/cards erased)
    }
    {   // geometry 1280x720: 16:9 grid gains columns, layout scales 1.5x
        setScreenGeom(1280, 720);
        CHECK(P(32) == 48 && kGridCols() == 7 && kHubPosterW() == 144 && kHubPosterH() == 216);
    }
    {   // controller: setRenderTarget switches geometry + format; copyFrame follows (MUT: frame stays R1)
        setScreenGeom(640, 480);
        PlexUiController::Hooks h;
        h.get = [](const std::string&, int, int, const misterplex::BrowseCancelFn&) { return misterplex::BrowseHttpResult{}; };
        h.playing = [] { return false; };
        PlexUiController ui(PlexUiController::Config{"tk", fonts}, h);
        ui.start();
        ui.post(misterplex::BrowseOp::Open);
        for (int i = 0; i < 300 && !ui.workerIdle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<std::uint8_t> f;
        CHECK(ui.visible() && ui.copyFrame(f) && f.size() == 640u * 480u * 3u);
        CHECK(ui.renderFormat() == PixelFormat::RGB24);
        CHECK(ui.setRenderTarget(1280, 720, PixelFormat::BGRA8888));
        CHECK(!ui.setRenderTarget(1280, 720, PixelFormat::BGRA8888));   // unchanged
        CHECK(ui.copyFrame(f) && f.size() == 1280u * 720u * 4u && f[3] == 255);
        CHECK(ui.setRenderTarget(640, 480, PixelFormat::RGB24));
        CHECK(ui.copyFrame(f) && f.size() == 640u * 480u * 3u);
        ui.stop();
    }
    setScreenGeom(640, 480);
    std::printf("test_plexui_geom: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
#endif
