#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
// rc2 PlexUI host tests: flag gating (default off), token file, PMS/plex.tv XML parsers, grid layout,
// offline render of every view with the bundled Roboto fonts (no network, no token).
#include "plexui_api_cache.hpp"
#include "plexui_config.hpp"
#include "plexui_data.hpp"
#include "plexui_http.hpp"
#include "plexui_render.hpp"
#include "plexui_controller.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace misterplex::plexui;
using misterplex::BrowseOp; using misterplex::BrowseCancelFn; using misterplex::BrowseHttpResult;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static int distinctBytes(const std::vector<std::uint8_t>& f) {
    bool seen[256] = {}; int n = 0;
    for (auto b : f) if (!seen[b]) { seen[b] = true; ++n; }
    return n;
}
int main(int argc, char** argv) {
    const std::string fonts = argc > 1 ? argv[1] : "host/libmisterplex/plexui/third_party/";
    ::unsetenv("MPX_PLEXUI");
    CHECK(!plexUiEnabled(true, true));                  // default off
    ::setenv("MPX_PLEXUI", "1", 1);
    CHECK(plexUiEnabled(true, true));
    CHECK(!plexUiEnabled(false, true));                 // MUT_PLEXUI_IGNORE_BROWSE_FLAGS
    CHECK(!plexUiEnabled(true, false));
    ::setenv("MPX_PLEXUI", "0", 1);
    CHECK(!plexUiEnabled(true, true));
    CHECK(formatPlexUiNotice(false).empty());
    ::unsetenv("MPX_PLEXUI_DIR");
    CHECK(plexUiDirFromEnvironment() == kDefaultPlexUiDir);
    const char* tf = "/tmp/test_plexui_token.txt";
    { std::ofstream o(tf); o << "  abc123 \r\n second line\n"; }
    CHECK(readTokenFile(tf) == "abc123");
    ::setenv("MPX_PLEXUI_TOKEN_FILE", tf, 1);
    CHECK(plexUiAccountToken("conf") == "abc123");
    ::setenv("MPX_PLEXUI_TOKEN_FILE", "/nonexistent/x", 1);
    CHECK(plexUiAccountToken("conf") == "conf");
    std::remove(tf);

    const std::string res =
        "<MediaContainer><Device name=\"Home\" provides=\"server\" clientIdentifier=\"c1\" accessToken=\"t1\" owned=\"0\" presence=\"1\" product=\"Plex Media Server\">"
        "<Connection protocol=\"https\" address=\"192.0.2.5\" port=\"32400\" local=\"1\" uri=\"https://192-0-2-5.x.plex.direct:32400\"/>"
        "<Connection protocol=\"https\" address=\"1.2.3.4\" port=\"32400\" local=\"0\" relay=\"1\" uri=\"https://r\"/></Device>"
        "<Device name=\"Phone\" provides=\"client,player\" accessToken=\"t2\"/>"
        "<Device name=\"NoTok\" provides=\"server\" presence=\"1\"/>"
        "<Device name=\"Offline\" provides=\"server\" accessToken=\"t3\" presence=\"0\"/></MediaContainer>";   // rc3: plexui keeps only presence=1 product=Plex Media Server
    auto servers = parsePlexResources(res);
    CHECK(servers.size() == 1 && servers[0].name == "Home" && servers[0].connections.size() == 2);
    if (servers.empty()) { std::printf("plexui: no servers parsed\n"); return 1; }
    pickServerConnections(servers);
    CHECK(!servers[0].connections.empty() && servers[0].connections[0].local);
    auto secs = parseLibrarySections("<MediaContainer><Directory key=\"1\" title=\"Movies\" type=\"movie\"/>"
        "<Directory key=\"9\" title=\"F\" type=\"folder\"/><Directory key=\"2\" title=\"TV\" type=\"show\"/></MediaContainer>");
    CHECK(secs.size() == 2 && secs[1].title == "TV");
    auto items = parseLibraryAll("<MediaContainer><Video ratingKey=\"29729\" key=\"/library/metadata/29729\" title=\"The Man Trap\" year=\"1966\"/>"
        "<Directory ratingKey=\"x\"/><Movie ratingKey=\"5\" title=\"M\"/></MediaContainer>");
    CHECK(items.size() == 2 && items[0].ratingKey == "29729" && items[0].title == "The Man Trap");
    PlexItem d = parseMetadataDetail("<MediaContainer><Video ratingKey=\"7\" title=\"T\" summary=\"S\">"
        "<Genre tag=\"Drama\"/><Genre tag=\"Sci-Fi\"/></Video></MediaContainer>");
    CHECK(d.ratingKey == "7" && d.genres.find("Drama") != std::string::npos && d.genres.find("Sci-Fi") != std::string::npos);
    CHECK(urlEncode("a b/c") != "a b/c");
    CHECK(appendToken("http://h/x", "tk").find("X-Plex-Token=tk") != std::string::npos);
    const GridLayout g = computeGridLayout();
    CHECK(g.cols == 5);
    CHECK(gridVisibleRowCount(g) >= 1);

    Renderer r; std::string err;
    CHECK(r.initFonts(fonts, err));
    RenderState st; st.servers = servers; std::vector<std::uint8_t> f;
    for (ViewId v : {ViewId::ServerList, ViewId::Home, ViewId::LibraryGrid, ViewId::Detail}) {
        st.view = v; st.haveServer = v != ViewId::ServerList; st.server = servers[0];
        st.sections = secs; st.grid = items; st.detail = d; st.gridSectionTitle = "Movies"; st.gridItemCount = 2;
        f.clear(); r.render(st, f);
        CHECK(f.size() == 640u * 480u * 3u);
        CHECK(distinctBytes(f) > 8);                    // not blank: text/accent drawn
    }
    {   // rc2.1: home/Browse during a play stops it before the UI opens (MUT_PLEXUI_OPEN_KEEPS_PLAY)
        std::atomic<bool> playing{true}; std::atomic<int> stops{0};
        PlexUiController::Hooks h;
        h.get = [](const std::string&, int, int, const BrowseCancelFn&) { return BrowseHttpResult{}; };
        h.playing = [&] { return playing.load(); };
        h.stopPlayback = [&] { ++stops; playing = false; };
        PlexUiController ui(PlexUiController::Config{"tk", fonts}, h);
        ui.start();
        ui.post(BrowseOp::Open);
        for (int i = 0; i < 200 && !ui.workerIdle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(stops.load() == 1 && !playing.load());
        CHECK(ui.visible());
        ui.stop();
    }
    {   // Back from home is cast-ready, not the server list.
        PlexUiController::Hooks h;
        h.get = [](const std::string&, int, int, const BrowseCancelFn&) { return BrowseHttpResult{}; };
        PlexUiController ui(PlexUiController::Config{"tk", fonts}, h);
        ui.start();
        PlexServer s;
        s.name = "Box";
        s.baseUrl = "http://127.0.0.1:9";
        ui.testSeedHome({s});
        ui.post(BrowseOp::Back);
        for (int i = 0; i < 200 && !ui.workerIdle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!ui.visible());
        CHECK(ui.testView() != ViewId::ServerList);
        ui.stop();
    }
    {   // Back from playback (StopAndReopen) returns to the detail the play started from.
        std::atomic<int> stops{0};
        PlexUiController::Hooks h;
        h.get = [](const std::string&, int, int, const BrowseCancelFn&) { return BrowseHttpResult{}; };
        h.stopPlayback = [&] { ++stops; };
        PlexUiController ui(PlexUiController::Config{"tk", fonts}, h);
        ui.start();
        ui.testArmPlaybackView(ViewId::Detail);
        ui.post(BrowseOp::StopAndReopen);
        for (int i = 0; i < 200 && !ui.workerIdle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(stops.load() == 1);
        CHECK(ui.visible());
        CHECK(ui.testView() == ViewId::Detail);
        ui.stop();
    }
    {   // Chosen base, not "any connection is local".
        PlexServer s;
        s.owned = false;
        PlexConnection local;
        local.local = true;
        local.protocol = "http";
        local.address = "127.0.0.1";
        local.port = "32400";
        PlexConnection pub;
        pub.protocol = "https";
        pub.uri = "https://1-2-3-4.abc.plex.direct:32400";
        PlexConnection relay;
        relay.relay = true;
        relay.protocol = "https";
        relay.uri = "https://relay.example:8443";
        s.connections = {local, pub, relay};
        s.baseUrl = pub.uri;
        CHECK(serverLinkLabel(s) == "remote");
        s.baseUrl = "http://127.0.0.1:32400";
        CHECK(serverLinkLabel(s) == "local");
        s.baseUrl = relay.uri;
        CHECK(serverLinkLabel(s) == "relay");
        s.baseUrl.clear();
        CHECK(serverLinkLabel(s) == "Connecting...");
        s.offline = true;
        CHECK(serverLinkLabel(s) == "Offline");
        Renderer lr;
        CHECK(lr.initFonts(fonts, err));
        RenderState lst;
        lst.view = ViewId::ServerList;
        lst.focusAnim = 3;
        s.offline = false;
        s.name = "Shared box";
        s.baseUrl = pub.uri;
        lst.servers = {s};
        std::vector<std::uint8_t> remoteFrame, localFrame;
        lr.render(lst, remoteFrame);
        lst.servers[0].baseUrl = "http://127.0.0.1:32400";
        lr.markStaticDirty();
        lr.render(lst, localFrame);
        CHECK(remoteFrame != localFrame);
        lst.servers[0].offline = true;
        lst.status.clear();
        std::vector<std::uint8_t> offlineFrame;
        lr.markStaticDirty();
        lr.render(lst, offlineFrame);
        CHECK(offlineFrame != localFrame);
        bool hint = false;
        for (int y = P(440); y < P(440) + P(18) && !hint; ++y) {
            for (int x = kSafeMargin(); x < kSafeMargin() + 120; ++x) {
                const std::uint8_t* p =
                    offlineFrame.data() + (static_cast<std::size_t>(y) * 640u + x) * 3u;
                if (p[0] > 160 && p[1] > 100 && p[2] < 140)
                    hint = true;
            }
        }
        CHECK(hint);
    }
    {   // Sidebar focus repaints, with the accent pill on the focused row only.
        Renderer sr;
        CHECK(sr.initFonts(fonts, err));
        RenderState st;
        st.view = ViewId::Home;
        st.haveServer = true;
        st.focusAnim = 3;
        st.dataGeneration = 1;
        PlexServer s;
        s.name = "Home";
        s.machineIdentifier = "m";
        s.baseUrl = "http://h";
        st.server = s;
        st.servers = {s};
        for (int k = 0; k < 4; ++k) {
            SidebarLibraryEntry e;
            e.title = "Library " + std::to_string(k);
            e.subtitle = "Shared";
            e.type = k % 2 ? "movie" : "show";
            st.sidebarEntries.push_back(e);
        }
        st.sidebarEntries[0].title = "Librar\xe2\x80\x99y with a long name that must truncate";
        PlexHubRow hub;
        hub.title = "Continue";
        PlexItem it;
        it.ratingKey = "1";
        it.title = "Item";
        it.thumb = "/t";
        hub.items.push_back(it);
        st.hubs.push_back(hub);
        auto accentInRow = [](const std::vector<std::uint8_t>& f, int rowIndex) {
            const int y0 = P(64) + rowIndex * kSidebarRowH();
            const int y1 = y0 + kSidebarRowH();
            for (int y = y0; y < y1; ++y) {
                for (int x = P(8); x < P(8) + 6; ++x) {
                    const std::uint8_t* p = f.data() + (static_cast<std::size_t>(y) * 640u + x) * 3u;
                    if (p[0] > 180 && p[1] > 100 && p[2] < 90)
                        return true;
                }
            }
            return false;
        };
        st.focus.sidebar = 1;
        std::vector<std::uint8_t> a, b;
        sr.render(st, a);
        st.focus.sidebar = 2;
        Renderer::RenderPerf moved;
        sr.render(st, b, &moved);
        CHECK(a != b);
        CHECK(moved.reason == "sidebar");
        CHECK(accentInRow(a, 1) && !accentInRow(a, 2));
        CHECK(accentInRow(b, 2) && !accentInRow(b, 1));
        // Left from the first hub poster into the sidebar keeps that poster's backdrop URL.
        // The highlight still has to be rebuilt.
        st.focus.sidebar = 0;
        st.focus.col = 0;
        st.backdropUrl = "http://h/photo/same";
        sr.markStaticDirty();
        Renderer::RenderPerf entered;
        std::vector<std::uint8_t> homeFocus, sideFocus;
        sr.render(st, homeFocus);
        st.focus.sidebar = 1;
        sr.render(st, sideFocus, &entered);
        CHECK(homeFocus != sideFocus);
        CHECK(entered.reason == "sidebar");
        CHECK(accentInRow(sideFocus, 1) && !accentInRow(homeFocus, 1));
    }
    {   // Typographic UTF-8 draws; a missing glyph uses an ASCII stand-in.
        Renderer fr;
        CHECK(fr.initFonts(fonts, err));
        const std::string apos = "\xe2\x80\x99";
        const float curly = fr.fonts().measureText(apos, 28.f, true);
        const float qmarkBold = fr.fonts().measureText("?", 28.f, true);
        const float qmark = fr.fonts().measureText("?", 28.f, false);
        const float ascii = fr.fonts().measureText("'", 28.f, true);
        CHECK(curly > 0.f && curly != qmarkBold);
        const std::string emoji = "\xf0\x9f\x98\x80";
        const float missing = fr.fonts().measureText(emoji, 28.f, false);
        CHECK(missing == qmark);
        const float missingThen = fr.fonts().measureText(emoji + "X", 28.f, false);
        const float qThen = fr.fonts().measureText("?X", 28.f, false);
        CHECK(missingThen == qThen && missingThen > fr.fonts().measureText("X", 28.f, false));
        const float eacute = fr.fonts().measureText("\xc3\xa9", 28.f, false);
        const float letterE = fr.fonts().measureText("e", 28.f, false);
        CHECK(eacute > 0.f);
        (void)ascii;
        (void)letterE;
        std::string accents;
        for (int i = 0; i < 40; ++i)
            accents += "\xc3\xa9";
        const std::string cut = fr.fonts().ellipsize(accents, 16.f, false, 48.f);
        const std::string ell = "\xe2\x80\xa6";
        CHECK(cut.size() > ell.size() && cut.compare(cut.size() - ell.size(), ell.size(), ell) == 0);
        const std::string body = cut.substr(0, cut.size() - ell.size());
        bool whole = !body.empty() && body.size() % 2 == 0;
        for (size_t i = 0; whole && i < body.size(); i += 2)
            whole = body[i] == '\xc3' && body[i + 1] == '\xa9';
        CHECK(whole);
        CHECK(fr.fonts().ellipsize("\xc3\xa9", 16.f, false, 200.f) == "\xc3\xa9");
        RenderState st;
        st.view = ViewId::Detail;
        st.haveServer = true;
        st.focusAnim = 3;
        st.detail.ratingKey = "9";
        st.detail.title = "The " + apos + "80s: Top Ten";
        st.server.baseUrl = "http://h";
        std::vector<std::uint8_t> good, bad;
        fr.render(st, good);
        st.detail.title = "The ?80s: Top Ten";
        fr.markStaticDirty();
        fr.render(st, bad);
        CHECK(good != bad);
    }
    {   // On-screen poster size is part of the key. A cached grid thumb still fills the detail poster,
        // then the detail-sized decode replaces it.
        const std::string rev = "rev";
        const GridLayout gl = computeGridLayout(true);
        const ImageCacheKey gridKey =
            thumbCacheKey("m", "143", gl.posterW, gl.posterH, rev);
        const ImageCacheKey detailKey = thumbCacheKey("m", "143", P(150), P(225), rev);
        CHECK(gridKey != detailKey);
        CHECK(gridKey.w == gl.posterW && gridKey.h == gl.posterH);
        ImageCache cache(4u << 20);
        CHECK(cache.get(detailKey) == nullptr);
        Renderer dr;
        CHECK(dr.initFonts(fonts, err));
        dr.setImageCache(&cache);
        RenderState st;
        st.view = ViewId::Detail;
        st.haveServer = true;
        st.focusAnim = 3;
        st.dataGeneration = 4;
        st.server.machineIdentifier = "m";
        st.server.baseUrl = "http://h";
        st.detail.ratingKey = "143";
        st.detail.title = "Poster";
        st.detail.thumb = "/library/metadata/143/thumb";
        st.detail.updatedAt = rev;
        std::vector<std::uint8_t> blank, shown, again;
        dr.render(st, blank);
        cache.testInsertDecoded(gridKey, gl.posterW, gl.posterH, 40);
        CHECK(cache.get(gridKey) != nullptr);
        CHECK(cache.get(detailKey) == nullptr);
        dr.markStaticDirty();
        dr.render(st, shown);
        CHECK(shown != blank);
        cache.testInsertDecoded(detailKey, P(150), P(225), 220);
        dr.markImageReady(detailKey);
        dr.render(st, again);
        CHECK(again != shown);
    }
    {   // Presses posted while a paint is in progress stay in order. Each move logs sidebar focus.
        std::mutex gate;
        std::condition_variable cv;
        bool hold = true;
        int paints = 0;
        std::mutex logMu;
        std::vector<std::string> logs;
        PlexUiController::Hooks h;
        h.get = [](const std::string&, int, int, const BrowseCancelFn&) { return BrowseHttpResult{}; };
        h.log = [&](const std::string& s) {
            std::lock_guard<std::mutex> lk(logMu);
            logs.push_back(s);
        };
        h.repaint = [&] {
            std::unique_lock<std::mutex> lk(gate);
            ++paints;
            cv.notify_all();
            cv.wait(lk, [&] { return !hold; });
        };
        PlexUiController ui(PlexUiController::Config{"tk", fonts}, h);
        std::vector<SidebarLibraryEntry> entries(6);
        for (int i = 0; i < 6; ++i) {
            entries[static_cast<std::size_t>(i)].title = "L" + std::to_string(i);
            entries[static_cast<std::size_t>(i)].sectionKey = std::to_string(i + 1);
            entries[static_cast<std::size_t>(i)].type = "show";
        }
        ui.testSetHome(std::move(entries));
        ui.start();
        ui.post(BrowseOp::Left);
        {
            std::unique_lock<std::mutex> lk(gate);
            CHECK(cv.wait_for(lk, std::chrono::seconds(2), [&] { return paints >= 1; }));
        }
        ui.post(BrowseOp::Down);
        ui.post(BrowseOp::Down);
        ui.post(BrowseOp::Up);
        {
            std::lock_guard<std::mutex> lk(gate);
            hold = false;
        }
        cv.notify_all();
        for (int i = 0; i < 200 && !ui.workerIdle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(ui.workerIdle());
        CHECK(ui.testSidebarFocus() == 2);
        auto has = [&](const char* needle) {
            std::lock_guard<std::mutex> lk(logMu);
            for (const auto& s : logs)
                if (s.find(needle) != std::string::npos)
                    return true;
            return false;
        };
        CHECK(has("plexui: sidebar event=left focus=1 section=1"));
        CHECK(has("plexui: sidebar event=down focus=2 section=2"));
        CHECK(has("plexui: sidebar event=up focus=2 section=2"));
        ui.stop();
    }
    {   // Uncached Select keeps nav_visible open across the LOADING frame. A second open still
        // waits for collections and genres; the items page may come from the metadata cache.
        plexUiApiCacheClear();
        std::mutex gate;
        std::condition_variable cv;
        bool release = false;
        std::atomic<int> calls{0};
        std::mutex logMu;
        std::vector<std::string> logs;
        std::atomic<bool> watchInstant{false};
        std::atomic<int> callsWhenGridShown{-1};
        PlexUiController* ui = nullptr;
        auto note = [&](const std::string& s) {
            std::lock_guard<std::mutex> lk(logMu);
            logs.push_back(s);
        };
        setPlexuiProbeLog(note);
        PlexUiController::Hooks h;
        h.get = [&](const std::string& url, int, int, const BrowseCancelFn&) {
            ++calls;
            {
                std::unique_lock<std::mutex> lk(gate);
                cv.wait(lk, [&] { return release; });
            }
            BrowseHttpResult r;
            r.status = 200;
            if (url.find("/collections") != std::string::npos)
                r.body = "<MediaContainer><Directory ratingKey=\"9\" title=\"Favs\"/></MediaContainer>";
            else if (url.find("/genre") != std::string::npos)
                r.body = "<MediaContainer><Directory key=\"g1\" title=\"Drama\"/></MediaContainer>";
            else if (url.find("/all") != std::string::npos)
                r.body = "<MediaContainer><Video ratingKey=\"143\" title=\"Show\"/></MediaContainer>";
            else
                r.body = "<MediaContainer/>";
            return r;
        };
        h.log = note;
        h.repaint = [&] {
            if (!watchInstant.load() || !ui)
                return;
            if (ui->frameSummary().view == "grid" && callsWhenGridShown.load() < 0)
                callsWhenGridShown.store(calls.load());
        };
        PlexUiController controller(PlexUiController::Config{"tk", fonts}, h);
        ui = &controller;
        controller.testArmLibrarySelect();
        controller.start();
        controller.post(BrowseOp::Select);
        bool sawLoading = false;
        for (int i = 0; i < 200 && !sawLoading; ++i) {
            if (calls.load() >= 1 && controller.frameSummary().loading)
                sawLoading = true;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(sawLoading);
        auto countOf = [&](const char* needle) {
            std::lock_guard<std::mutex> lk(logMu);
            int n = 0;
            for (const auto& s : logs)
                if (s.find(needle) != std::string::npos)
                    ++n;
            return n;
        };
        const int navBefore = countOf("nav_visible");
        controller.onR1FlipAck(1, 2);
        CHECK(countOf("nav_visible") == navBefore);
        {
            std::lock_guard<std::mutex> lk(gate);
            release = true;
        }
        cv.notify_all();
        for (int i = 0; i < 200 && !controller.workerIdle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(controller.workerIdle());
        controller.onR1FlipAck(1, 2);
        CHECK(countOf("nav_visible") == navBefore + 1);
        CHECK(controller.frameSummary().view == "grid");
        CHECK(controller.frameSummary().grid > 0);
        CHECK(calls.load() == 3);
        CHECK(countOf("plexui: library collections server=mid-lib section=2") == 1);
        CHECK(countOf("plexui: library genres server=mid-lib section=2") == 1);
        CHECK(countOf("plexui: library items server=mid-lib section=2") == 1);
        CHECK(countOf("event=select focus=1 section=2") == 1);
        const int baseline = calls.load();
        callsWhenGridShown.store(-1);
        watchInstant.store(true);
        controller.testArmLibrarySelect();
        controller.post(BrowseOp::Select);
        for (int i = 0; i < 200 && !controller.workerIdle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(controller.workerIdle());
        // Collections and genres are fetched again. The grid is not painted from a listing cache
        // before those requests. The items page may be served from the short metadata cache.
        CHECK(callsWhenGridShown.load() > baseline);
        CHECK(calls.load() >= baseline + 2);
        for (int i = 0; i < 50 && calls.load() < baseline + 3; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(calls.load() >= baseline + 3);
        CHECK(controller.frameSummary().view == "grid");
        CHECK(countOf("event=select focus=1 section=2") == 2);
        CHECK(plexUiApiCacheDrain());
        controller.stop();
        setPlexuiProbeLog({});
    }
    {   // A down collections or genre response does not abort the library. Only items does.
        plexUiApiCacheClear();
        PlexUiController::Hooks h;
        h.get = [](const std::string& url, int, int, const BrowseCancelFn&) {
            BrowseHttpResult r;
            if (url.find("/collections") != std::string::npos) {
                r.status = 500;
                return r;
            }
            if (url.find("/genre") != std::string::npos) {
                r.status = 0;
                return r;
            }
            r.status = 200;
            r.body = "<MediaContainer><Video ratingKey=\"143\" title=\"Show\"/></MediaContainer>";
            return r;
        };
        PlexUiController controller(PlexUiController::Config{"tk", fonts}, h);
        controller.testArmLibrarySelect();
        controller.start();
        controller.post(BrowseOp::Select);
        for (int i = 0; i < 200 && !controller.workerIdle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(controller.workerIdle());
        CHECK(controller.frameSummary().view == "grid");
        CHECK(controller.frameSummary().grid > 0);
        CHECK(plexUiApiCacheDrain());
        controller.stop();
    }
    std::printf("plexui: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
