// ARM Plex browse P1 (MPX_BROWSE_V2): host unit tests for browse_v2.hpp, browse_fetch.hpp,
// browse_controller.hpp and the PLXI command widening.
//   parse        helper XML (rel-mpxmenu-contract-1004.md shapes), entities, order,
//                whole-name attributes, offsets, info/offline rows, header
//   urls         helper URL joining, cols, log form
//   tickets      local/PMS/ladder tickets, MpxPlay (build kit) and mpxLadderSrc (contract)
//   window       the paint snapshot renders pixel-identical to the full list
//   commands     PLXI 1-5 unchanged, 6-11 only when widened; browse op map
//   controller   root, HELPER OFFLINE fallback, paging, offsets, A8 reopen, A10 cast,
//                cursors per level, the cancel race, no token in logs
//   http         browseHttpGet against a local server: ok, refused, timeout, cancel
// Usage: test_browse_v2 [SERVER_SCRIPT]  (the http checks need python3 and curl; they skip
// without SERVER_SCRIPT)
#include "libmisterplex/browse_controller.hpp"
#include "libmisterplex/browse_fetch.hpp"
#include "libmisterplex/browse_v2.hpp"
#include "libmisterplex/input_mailbox.hpp"
#include "libmisterplex/library_browser.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace misterplex;

namespace {
int g_checks = 0;
int g_fail = 0;
#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        ++g_checks;                                                                         \
        if (!(cond)) {                                                                      \
            ++g_fail;                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);          \
            std::fprintf(stderr, __VA_ARGS__);                                              \
            std::fprintf(stderr, "\n");                                                     \
        }                                                                                   \
    } while (0)

using Clock = std::chrono::steady_clock;
int64_t msSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// ---- fixtures: the contract's shapes ----
const char* kRootXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<MediaContainer size=\"4\" mpxHeader=\"PLEX\">\n"
    "<Directory title=\"CONTINUE WATCHING\" key=\"/menu/hub/local/continue\" type=\"mpxhub\"/>\n"
    "<Directory title=\"MOVIES\" key=\"/menu/section/local/1/all?page=0&amp;cols=48\" "
    "type=\"mpxsection\"/>\n"
    "<Video title=\"BLADE RUNNER 1982  W\" type=\"movie\" ratingKey=\"12345\" "
    "mpxServer=\"local\" mpxSource=\"local\" mpxOffsetMs=\"0\"/>\n"
    "<Directory title=\"SERVERS &gt;\" key=\"/menu/servers\" type=\"mpxservers\"/>\n"
    "</MediaContainer>\n";

std::string pageXml(int page, int total) {
    std::string x = "<MediaContainer mpxHeader=\"MOVIES " + std::to_string(page * 20 + 1) + "-" +
                    std::to_string(std::min(total, page * 20 + 20)) + "/" +
                    std::to_string(total) + "\">";
    if (page > 0)
        x += "<Directory title=\"&lt; PREV\" key=\"/menu/section/local/1/all?page=" +
             std::to_string(page - 1) + "&amp;cols=48\" type=\"mpxpage\"/>";
    for (int i = page * 20; i < std::min(total, page * 20 + 20); ++i)
        x += "<Directory title=\"MOVIE " + std::to_string(i + 1) + "\" key=\"/menu/item/local/" +
             std::to_string(1000 + i) + "\" type=\"mpxitem\"/>";
    if ((page + 1) * 20 < total)
        x += "<Directory title=\"NEXT &gt;\" key=\"/menu/section/local/1/all?page=" +
             std::to_string(page + 1) + "&amp;cols=48\" type=\"mpxpage\"/>";
    return x + "</MediaContainer>";
}

std::string itemXml(const std::string& rk, int64_t viewOffset, int refresh) {
    std::string x = "<MediaContainer mpxHeader=\"MOVIE " + rk + "\">"
                    "<Directory title=\"1982  117M\" mpxKind=\"info\" type=\"mpxinfo\"/>"
                    "<Directory title=\"REFRESH " + std::to_string(refresh) +
                    "\" mpxKind=\"info\" type=\"mpxinfo\"/>";
    if (viewOffset > 0)
        x += "<Video title=\"RESUME FROM 03:41\" type=\"movie\" ratingKey=\"" + rk +
             "\" mpxServer=\"local\" mpxSource=\"local\" mpxOffsetMs=\"" +
             std::to_string(viewOffset) + "\"/>";
    x += "<Video title=\"PLAY FROM START\" type=\"movie\" ratingKey=\"" + rk +
         "\" mpxServer=\"local\" mpxSource=\"local\" mpxOffsetMs=\"0\"/>";
    return x + "</MediaContainer>";
}

const char* kPmsSectionsXml =
    "<MediaContainer size=\"2\" title1=\"Plex Library\">"
    "<Directory allowSync=\"1\" key=\"1\" type=\"movie\" title=\"Movies &amp; Docs\" "
    "librarySectionKey=\"9\"/>"
    "<Directory key=\"2\" type=\"show\" title=\"TV Shows\"/>"
    "</MediaContainer>";

// ---- parse ----
void testParse() {
    const BrowsePage p = parseBrowseXml(kRootXml, true);
    CHECK(p.isMediaContainer, "root is a MediaContainer");
    CHECK(p.header == "PLEX", "header '%s'", p.header.c_str());
    CHECK(p.rows.size() == 4, "rows %zu", p.rows.size());
    if (p.rows.size() == 4) {
        CHECK(p.rows[0].directory && p.rows[0].key == "/menu/hub/local/continue", "row0");
        CHECK(p.rows[1].key == "/menu/section/local/1/all?page=0&cols=48", "entity in key '%s'",
              p.rows[1].key.c_str());
        // Document order: the Video row stays between the directories (V1 put it last).
        CHECK(!p.rows[2].directory && p.rows[2].ratingKey == "12345", "video row in place");
        CHECK(p.rows[2].mpxSource == "local" && p.rows[2].mpxServer == "local", "mpx attrs");
        CHECK(p.rows[2].offsetPresent && p.rows[2].offsetMs == 0, "mpxOffsetMs=0 is present");
        CHECK(p.rows[3].title == "SERVERS >", "&gt; decoded: '%s'", p.rows[3].title.c_str());
        CHECK(p.rows[2].fromMenu && p.rows[0].fromMenu, "fromMenu");
    }
    // ratingKey never matches key; whole attribute names.
    const BrowsePage q = parseBrowseXml(
        "<MediaContainer><Video ratingKey=\"7\" title='A &quot;Q&quot; &#65;&#x42;' "
        "grandparentKey=\"/x\" mpxOffsetMs=\"221000\" mpxSource=\"ladder\" "
        "key=\"/menu/resolve/s1/7?offsetMs=221000\"/></MediaContainer>",
        true);
    CHECK(q.rows.size() == 1, "one row");
    if (!q.rows.empty()) {
        CHECK(q.rows[0].key == "/menu/resolve/s1/7?offsetMs=221000", "key '%s'",
              q.rows[0].key.c_str());
        CHECK(q.rows[0].title == "A \"Q\" AB", "entities '%s'", q.rows[0].title.c_str());
        CHECK(q.rows[0].offsetPresent && q.rows[0].offsetMs == 221000, "resume offset");
        CHECK(browseRowActivatable(q.rows[0]), "ladder row activatable");
    }
    // A PMS list: mpx attributes are not read; offsets stay absent (doPlay auto-resume).
    const BrowsePage pms = parseBrowseXml(
        "<MediaContainer><Video ratingKey=\"5\" key=\"/library/metadata/5\" title=\"X\" "
        "viewOffset=\"600000\" mpxOffsetMs=\"3\"/></MediaContainer>",
        false);
    CHECK(pms.rows.size() == 1 && !pms.rows[0].fromMenu && !pms.rows[0].offsetPresent,
          "PMS rows carry no offset");
    // Bad offsets are absent, not 0.
    const BrowsePage bad = parseBrowseXml(
        "<MediaContainer><Video ratingKey=\"5\" title=\"X\" mpxOffsetMs=\"-4\"/>"
        "<Video ratingKey=\"6\" title=\"Y\" mpxOffsetMs=\"12x\"/></MediaContainer>",
        true);
    CHECK(bad.rows.size() == 2 && !bad.rows[0].offsetPresent && !bad.rows[1].offsetPresent,
          "invalid offsets absent");
    // Info and offline rows are shown, never activated; a keyless directory neither.
    LibraryRow info;
    info.directory = true;
    info.key = "/menu/x";
    info.mpxKind = "info";
    CHECK(!browseRowActivatable(info), "info row");
    info.mpxKind = "offline";
    CHECK(!browseRowActivatable(info), "offline row");
    LibraryRow nokey;
    nokey.directory = true;
    CHECK(!browseRowActivatable(nokey), "keyless directory");
    // The PMS sections answer, as V1 parses it (same rows; entities decoded in V2).
    const BrowsePage sec = parseBrowseXml(kPmsSectionsXml, false);
    const auto v1 = parseLibraryXml(kPmsSectionsXml);
    CHECK(sec.rows.size() == v1.size() && sec.rows.size() == 2, "sections %zu/%zu",
          sec.rows.size(), v1.size());
    if (sec.rows.size() == 2 && v1.size() == 2) {
        CHECK(sec.rows[0].key == v1[0].key && sec.rows[1].key == v1[1].key, "same keys");
        CHECK(sec.rows[0].title == "Movies & Docs" && v1[0].title == "Movies & Docs",
              "V2 and V1 both decode (V1 since w9 de8d3d17)");
        CHECK(libraryListPath(sec.rows[0]) == "/library/sections/1/all", "list path");
    }
    // Garbage never loops or crashes.
    for (const char* g : {"<", "<<<>>>", "<MediaContainer", "<Video title=\"x", "<Video =\"\"/>",
                          "<Directory key=a title=b/>", "&amp;&#xZZ;&#99999999;"}) {
        const BrowsePage gp = parseBrowseXml(g, true);
        (void)gp;
    }
    CHECK(xmlUnescape("&amp;&lt;&gt;&quot;&apos;&#33;&#x21;&bogus;&") == "&<>\"'!!&bogus;&",
          "unescape '%s'", xmlUnescape("&amp;&lt;&gt;&quot;&apos;&#33;&#x21;&bogus;&").c_str());
    CHECK(xmlUnescape("&#233;") == "\xc3\xa9", "eacute entity is UTF-8");
    CHECK(xmlUnescape("&#8217;") == "\xe2\x80\x99", "apostrophe entity is UTF-8");
    CHECK(xmlUnescape("&#x201C;Hi&#x201D;") == "\xe2\x80\x9cHi\xe2\x80\x9d", "quotes");
    CHECK(xmlUnescape("&#x2014;") == "\xe2\x80\x94", "em dash");
    CHECK(xmlUnescape("&#x2026;") == "\xe2\x80\xa6", "ellipsis");
}

// ---- urls ----
void testUrls() {
    const std::string m = "http://192.0.2.21:18769/menu";
    CHECK(menuUrlForKey(m, "/menu/servers", 48) == "http://192.0.2.21:18769/menu/servers?cols=48",
          "abs path");
    CHECK(menuUrlForKey(m, "/menu/section/a/1/all?page=2&cols=30", 48) ==
              "http://192.0.2.21:18769/menu/section/a/1/all?page=2&cols=30",
          "cols kept");
    CHECK(menuUrlForKey(m, "http://h:1/x", 0) == "http://h:1/x", "absolute URL");
    CHECK(menuUrlForKey(m, "sub", 10) == "http://192.0.2.21:18769/menu/sub?cols=10", "relative");
    CHECK(urlOrigin("http://h:1/a/b?c") == "http://h:1", "origin");
    CHECK(urlForLog("http://h/x?X-Plex-Token=SECRET&a=1") == "http://h/x?...", "log form");
    CHECK(!hasQueryParam("http://h/x?acols=1", "cols") && hasQueryParam("http://h/x?a=1&cols=2", "cols"),
          "param match");
    CHECK(menuUrlFromEnvironment(nullptr, nullptr).empty(), "default: no helper");
    CHECK(menuUrlFromEnvironment(nullptr, "http://h:9/") == "http://h:9/menu", "base form");
    CHECK(menuUrlFromEnvironment("http://u/m", "http://h:9") == "http://u/m", "url wins");
    CHECK(parseBrowseV2(nullptr) == BrowseV2Setting::Off && parseBrowseV2("") == BrowseV2Setting::Off &&
              parseBrowseV2("0") == BrowseV2Setting::Off && parseBrowseV2("1") == BrowseV2Setting::On &&
              parseBrowseV2("yes") == BrowseV2Setting::Invalid,
          "env parse");
    CHECK(formatBrowseV2Notice(BrowseV2Setting::Off, nullptr, "x").empty(), "off: no line");
    CHECK(!formatBrowseV2Notice(BrowseV2Setting::Invalid, "2", "x").empty(), "invalid: a line");
}

// ---- tickets ----
void testTickets() {
    LibraryRow local;
    local.fromMenu = true;
    local.ratingKey = "42";
    local.mpxSource = "local";
    local.offsetPresent = true;
    local.offsetMs = 221000;
    BrowsePlayTicket t = localBrowseTicket(local);
    CHECK(t.offsetPresent && t.offsetMs == 221000 && t.key == "/library/metadata/42" &&
              t.address.empty() && t.source == "local",
          "local resume ticket");
    local.offsetMs = 0;
    t = localBrowseTicket(local);
    CHECK(t.offsetPresent && t.offsetMs == 0, "PLAY FROM START is offset 0, present");
    local.offsetPresent = false; // a local helper row without mpxOffsetMs: from start
    t = localBrowseTicket(local);
    CHECK(t.offsetPresent && t.offsetMs == 0, "helper row without offset starts at 0");
    LibraryRow pms;
    pms.ratingKey = "5";
    t = localBrowseTicket(pms);
    CHECK(!t.offsetPresent && t.source == "pms", "PMS row keeps doPlay's auto-resume");
    LibraryRow pathOnly;
    pathOnly.key = "/library/metadata/9";
    t = localBrowseTicket(pathOnly);
    CHECK(t.key == "/library/metadata/9" && t.ratingKey == "9" && t.source == "pms",
          "a metadata path plays that path");
    pathOnly.ratingKey = "/library/metadata/9";
    t = localBrowseTicket(pathOnly);
    CHECK(t.key == "/library/metadata/9" && t.ratingKey == "9",
          "a rating key that is already a metadata path is not prefixed again");

    LibraryRow ladder;
    ladder.fromMenu = true;
    ladder.ratingKey = "115463";
    ladder.mpxSource = "ladder";
    ladder.mpxServer = "sf4cc754";
    ladder.offsetPresent = true;
    ladder.offsetMs = 221000;
    ladder.key = "/menu/resolve/sf4cc754/115463?offsetMs=221000";
    // wT's resolve answer: the Part URL only (encoded, with the server's token).
    t = parseBrowseTicket(
        "<MediaContainer size=\"1\"><Video title=\"S01E03\" type=\"episode\" "
        "mpxLadderSrc=\"https%3A%2F%2F192-0-2-3.abc.plex.direct%3A32443%2Flibrary%2Fparts%2F9%2F1%2F"
        "file.mkv%3FX-Plex-Token%3DSrvTok%252B1\" mpxW=\"640\" mpxH=\"480\" mpxBr=\"2000\" "
        "mpxFps=\"src\" mpxOffsetMs=\"221000\" mpxDurationMs=\"2763136\"/></MediaContainer>",
        ladder);
    CHECK(t.error.empty(), "ticket error '%s'", t.error.c_str());
    CHECK(t.protocol == "https" && t.address == "192-0-2-3.abc.plex.direct" && t.port == "32443",
          "server from Part URL %s://%s:%s", t.protocol.c_str(), t.address.c_str(), t.port.c_str());
    CHECK(t.token == "SrvTok+1", "token decoded once from the Part URL");
    CHECK(t.offsetPresent && t.offsetMs == 221000 && t.key == "/library/metadata/115463" &&
              t.ratingKey == "115463",
          "resume kept");
    // The build kit's ticket: explicit fields win.
    t = parseBrowseTicket("<MpxPlay address=\"192.0.2.30\" port=\"32400\" protocol=\"http\" "
                          "token=\"T2\" machineIdentifier=\"mid\" key=\"/library/metadata/77\" "
                          "ratingKey=\"77\" mpxLadderSrc=\"http%3A%2F%2Fother%3A1%2Fp\"/>",
                          ladder);
    CHECK(t.error.empty() && t.address == "192.0.2.30" && t.port == "32400" && t.token == "T2" &&
              t.machineIdentifier == "mid" && t.ratingKey == "77" && t.key == "/library/metadata/77",
          "MpxPlay ticket");
    CHECK(t.offsetPresent && t.offsetMs == 221000, "row offset carried");
    // [v6] host, default port, no token.
    BrowsePlayTicket v6;
    CHECK(serverFromPartUrl("http://[::1]/library/parts/1", v6) && v6.address == "[::1]" &&
              v6.port == "80" && v6.token.empty(),
          "v6 %s %s", v6.address.c_str(), v6.port.c_str());
    // Refusals.
    t = parseBrowseTicket("<MediaContainer><Video mpxError=\"CANNOT PLAY FROM B\"/></MediaContainer>",
                          ladder);
    CHECK(t.error == "CANNOT PLAY FROM B", "mpxError");
    t = parseBrowseTicket("<MediaContainer/>", ladder);
    CHECK(!t.error.empty(), "no ticket");
    t = parseBrowseTicket("<MediaContainer><Video mpxLadderSrc=\"ftp%3A%2F%2Fx\"/></MediaContainer>",
                          ladder);
    CHECK(!t.error.empty(), "no server");
}

// ---- the live mpx-menu shapes (contract §8-9, mpx-menu cd431cb3) ----
void testLiveShapes() {
    const BrowsePage p = parseBrowseXml(
        "<MediaContainer size=\"5\">"
        "<Directory title=\"21-40/6930\" mpxKind=\"header\" type=\"mpxheader\"/>"
        "<Directory title=\"PREV\" key=\"/menu/section/local/1/movie/s0/c48\" type=\"page\" "
        "mpxKind=\"page\"/>"
        "<Directory title=\"A-Z\" key=\"/menu/az/local/1/c48\" mpxKind=\"az\"/>"
        "<Directory title=\"ALIEN 1979\" key=\"/menu/item/local/77/c48\" mpxKind=\"item\"/>"
        "<Directory title=\"NEXT\" key=\"/menu/section/local/1/movie/s40/c48\" type=\"page\" "
        "mpxKind=\"page\"/>"
        "</MediaContainer>",
        true);
    CHECK(p.rows.size() == 5, "live section rows %zu", p.rows.size());
    if (p.rows.size() == 5) {
        CHECK(!browseRowActivatable(p.rows[0]), "header row inert");
        CHECK(browsePageRowIsPrev(p.rows[1]) && !browsePageRowIsNext(p.rows[1]), "PREV row pages");
        CHECK(browseRowActivatable(p.rows[2]) && !browseRowIsPage(p.rows[2]), "A-Z opens a level");
        CHECK(browseRowActivatable(p.rows[3]) && !browseRowIsPage(p.rows[3]), "item row");
        CHECK(browsePageRowIsNext(p.rows[4]) && !browsePageRowIsPrev(p.rows[4]), "NEXT row pages");
    }
    LibraryRow videoPage;
    videoPage.fromMenu = true;
    videoPage.type = "page";
    videoPage.ratingKey = "1";
    CHECK(!browseRowIsPage(videoPage), "only Directory rows page");
    CHECK(menuUrlForKey("http://h:18769/menu", "/menu/item/local/77/c48", 0) ==
              "http://h:18769/menu/item/local/77/c48",
          "path-only key: nothing added");
    const BrowsePage item = parseBrowseXml(
        "<MediaContainer><Directory title=\"1979  117M\" mpxKind=\"info\" type=\"mpxinfo\"/>"
        "<Video title=\"RESUME FROM 03:41\" mpxKind=\"action\" ratingKey=\"77\" mpxServer=\"local\" "
        "mpxOffsetMs=\"221000\" key=\"/menu/ticket/local/77/o221000\"/></MediaContainer>",
        true);
    CHECK(item.rows.size() == 2 && !browseRowActivatable(item.rows[0]) &&
              browseRowActivatable(item.rows[1]) && item.rows[1].mpxSource.empty(),
          "action row activatable, no mpxSource");
    if (item.rows.size() != 2)
        return;
    // The local ticket: the helper's 127.0.0.1 view is dropped (conf PMS), always the Part.
    BrowsePlayTicket t = parseBrowseTicket(
        "<MediaContainer><MpxPlay ratingKey=\"77\" key=\"/library/metadata/77\" "
        "machineIdentifier=\"abc\" address=\"127.0.0.1\" port=\"32400\" protocol=\"http\" "
        "token=\"HELPERTOK\" local=\"1\" mpxOffsetMs=\"221000\" mpxPlayable=\"yes\" "
        "mpxLadderHint=\"480p\" mpxPartKbps=\"2619\" mpxLadderInput=\"part\"/></MediaContainer>",
        item.rows[1]);
    CHECK(t.error.empty() && t.source == "local" && t.address.empty() && t.port.empty() &&
              t.token.empty() && t.machineIdentifier.empty() && t.ladderInput.empty() &&
              t.ladderSrc.empty() && t.key == "/library/metadata/77",
          "local ticket: source=%s address=%s input=%s", t.source.c_str(), t.address.c_str(),
          t.ladderInput.c_str());
    CHECK(t.offsetPresent && t.offsetMs == 221000, "local ticket offset");
    t = parseBrowseTicket("<MpxPlay ratingKey=\"77\" address=\"127.0.0.1\" port=\"32400\" "
                          "mpxLadderInput=\"server-transcode\" mpxPartKbps=\"40000\"/>",
                          item.rows[1]);
    CHECK(t.error.empty() && t.ladderInput.empty(), "local is never server-transcode (§9.3)");
    // A friend's server above ~8 Mbps: the owner's transcode (lead Q4).
    LibraryRow remote;
    remote.fromMenu = true;
    remote.ratingKey = "101";
    remote.mpxServer = "s1a2b3c";
    remote.mpxKind = "action";
    remote.offsetPresent = true;
    remote.offsetMs = 95000;
    remote.key = "/menu/ticket/s1a2b3c/101/o95000";
    t = parseBrowseTicket(
        "<MediaContainer><MpxPlay ratingKey=\"101\" key=\"/library/metadata/101\" "
        "machineIdentifier=\"midB\" address=\"plex.example.art\" port=\"32401\" protocol=\"http\" "
        "token=\"RESTOK\" local=\"0\" relay=\"0\" mpxOffsetMs=\"95000\" mpxPlayable=\"yes\" "
        "mpxPartKbps=\"25148\" mpxLadderInput=\"server-transcode\"/></MediaContainer>",
        remote);
    CHECK(t.error.empty() && t.source == "ladder" && t.address == "plex.example.art" &&
              t.port == "32401" && t.protocol == "http" && t.token == "RESTOK" &&
              t.machineIdentifier == "midB" && t.ladderInput == "server-transcode" &&
              t.ladderSrc.empty() && t.offsetPresent && t.offsetMs == 95000,
          "remote server-transcode ticket: %s %s", t.address.c_str(), t.ladderInput.c_str());
    t = parseBrowseTicket("<MpxPlay ratingKey=\"101\" address=\"192.0.2.9\" port=\"32400\" "
                          "mpxLadderInput=\"part\" mpxPartKbps=\"6000\"/>",
                          remote);
    CHECK(t.error.empty() && t.ladderInput == "part", "remote part ticket");
    // Refusals: mpxPlayable=no (relay-only), with or without a reason; a container mpxError.
    t = parseBrowseTicket("<MpxPlay ratingKey=\"101\" address=\"office.local\" mpxPlayable=\"no\"/>",
                          remote);
    CHECK(t.error == "UNPLAYABLE", "mpxPlayable=no: '%s'", t.error.c_str());
    t = parseBrowseTicket("<MpxPlay ratingKey=\"101\" mpxPlayable=\"no\" mpxReason=\"RELAY ONLY\"/>",
                          remote);
    CHECK(t.error == "RELAY ONLY", "mpxReason: '%s'", t.error.c_str());
    t = parseBrowseTicket("<MediaContainer mpxError=\"NOT FOUND\"><Directory title=\"NOT FOUND\" "
                          "mpxKind=\"info\" type=\"mpxinfo\"/></MediaContainer>",
                          remote);
    CHECK(t.error == "NOT FOUND", "container mpxError: '%s'", t.error.c_str());
    CHECK(browseContainerError("<MediaContainer size=\"0\"/>").empty(), "no error");
}

// ---- window: the snapshot paints exactly like the full list ----
void testWindow() {
    for (const auto& wh : std::vector<std::pair<int, int>>{{640, 480}, {320, 240}, {624, 480}}) {
        const int w = wh.first, h = wh.second;
        for (int n : {0, 1, 5, 22, 23, 24, 60}) {
            LibraryBrowser br;
            std::vector<LibraryRow> rows;
            for (int i = 0; i < n; ++i) {
                LibraryRow r;
                r.title = "ROW " + std::to_string(i) + " <A> (B) 50%";
                r.directory = (i % 3) == 0;
                rows.push_back(r);
            }
            br.show(rows, n % 2 ? "LOADING..." : "");
            for (int c = 0; c < std::max(1, n); c += std::max(1, n / 7)) {
                br.cursors.back() = c;
                std::vector<uint8_t> a(static_cast<size_t>(w) * h * 3 / 2, 7),
                    b(a.size(), 9);
                renderLibraryI420Titled(a.data(), w, h, br, "HDR", true);
                const BrowseView v = browseWindow(br, "HDR", w, h);
                renderLibraryI420Titled(b.data(), w, h, v.window, v.header.c_str(), true);
                CHECK(a == b, "window paint differs w=%d h=%d n=%d c=%d", w, h, n, c);
                CHECK(v.window.stack.empty() ||
                          static_cast<int>(v.window.stack.back().size()) <=
                              libraryRowsOnScreen(w, h),
                      "window bounded");
            }
        }
    }
    CHECK(libraryRowsOnScreen(640, 480) == 23 && libraryRowsOnScreen(320, 240) == 22,
          "rows %d %d", libraryRowsOnScreen(640, 480), libraryRowsOnScreen(320, 240));
    CHECK(browseTitleCols(640) == 50 && browseTitleCols(320) == 50, "cols %d %d",
          browseTitleCols(640), browseTitleCols(320));
    // V2 glyphs: '<' and '>' differ, '%' is drawn; V1's table is untouched.
    CHECK(std::memcmp(libraryGlyphExt('<'), libraryGlyphExt('>'), 7) != 0, "< vs >");
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(libraryGlyphExt('%'), blank, 7) != 0 &&
              std::memcmp(libraryGlyph('%'), blank, 7) == 0,
          "%% only in V2");
}

// ---- commands ----
void testCommands() {
    auto word = [](uint8_t cmd, uint8_t cmdSeq, uint16_t seq) {
        return static_cast<uint64_t>(kInputMailboxMagic) | (static_cast<uint64_t>(cmd) << 32) |
               (static_cast<uint64_t>(cmdSeq) << 40) | (static_cast<uint64_t>(seq) << 48);
    };
    for (int c = 0; c < 256; ++c) {
        InputMailboxSample s;
        const bool legacy = decodeInputMailboxWord(word(static_cast<uint8_t>(c), 1, 2), s);
        CHECK(legacy == (c <= 5), "legacy cmd %d", c);
        InputMailboxSample s2;
        const bool wide =
            decodeInputMailboxWord(word(static_cast<uint8_t>(c), 1, 2), s2, kBrowseMaxInputCommand);
        CHECK(wide == (c <= 11), "wide cmd %d", c);
        if (wide)
            CHECK(static_cast<int>(s2.command) == c && s2.cmdSeq == 1 && s2.seq == 2, "fields %d", c);
    }
    CHECK(static_cast<int>(PlaybackCommand::Up) == 6 && static_cast<int>(PlaybackCommand::Down) == 7 &&
              static_cast<int>(PlaybackCommand::PageUp) == 8 &&
              static_cast<int>(PlaybackCommand::PageDown) == 9 &&
              static_cast<int>(PlaybackCommand::Back) == 10 && static_cast<int>(PlaybackCommand::Info) == 11,
          "P2 numbers");
    // While playing, the new commands do nothing (never seek or stop).
    for (PlaybackCommand c : {PlaybackCommand::Up, PlaybackCommand::Down, PlaybackCommand::PageUp,
                              PlaybackCommand::PageDown, PlaybackCommand::Back, PlaybackCommand::Info}) {
        const PlaybackAction a = mapPlaybackCommand(c, true, false, 5000, 60000, 30000, 10000);
        CHECK(a.kind == PlaybackActionKind::None, "cmd %d during play", static_cast<int>(c));
    }
    CHECK(mapPlaybackCommand(PlaybackCommand::SkipForward, true, false, 5000, 60000, 30000, 10000)
                  .kind == PlaybackActionKind::Seek,
          "skip still seeks");
    BrowseOp op;
    CHECK(browseOpForVisibleCommand(PlaybackCommand::PlayPause, op) && op == BrowseOp::Select, "select");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::Stop, op) && op == BrowseOp::Back, "stop=back");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::Back, op) && op == BrowseOp::Back, "back");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::SkipForward, op) && op == BrowseOp::Down, "fwd");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::Down, op) && op == BrowseOp::Down, "down");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::SkipBack, op) && op == BrowseOp::Up, "back=up");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::Up, op) && op == BrowseOp::Up, "up");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::PageDown, op) && op == BrowseOp::PageDown, "pgdn");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::PageUp, op) && op == BrowseOp::PageUp, "pgup");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::Info, op) && op == BrowseOp::Info, "info");
    CHECK(browseOpForVisibleCommand(PlaybackCommand::Browse, op) && op == BrowseOp::Open, "browse");
    CHECK(!browseOpForVisibleCommand(PlaybackCommand::None, op), "none");
}

// ---- controller ----

// A fake helper + PMS. Routes by URL; optional per-route delay that ends early on cancel.
struct FakeNet {
    std::mutex mu;
    std::vector<std::string> urls;
    bool menuDown = false;
    int menuStatus = 200;
    std::map<std::string, int> delayMs; // URL substring -> delay
    std::map<std::string, int> itemOffset{{"1000", 221000}};
    int refreshes = 0;
    std::atomic<int> cancels{0};
    std::atomic<int> inFlight{0};

    BrowseHttpResult get(const std::string& url, int connectMs, int timeoutMs,
                         const BrowseCancelFn& cancelled) {
        {
            std::lock_guard<std::mutex> lk(mu);
            urls.push_back(url);
        }
        ++inFlight;
        BrowseHttpResult r;
        const auto t0 = Clock::now();
        int delay = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const auto& d : delayMs)
                if (url.find(d.first) != std::string::npos)
                    delay = d.second;
        }
        const bool menu = url.find(":18769") != std::string::npos;
        if (menu && menuDown)
            delay = std::max(delay, connectMs); // nothing answers: curl's connect timeout
        while (msSince(t0) < std::min(delay, timeoutMs + 250)) {
            if (cancelled && cancelled()) {
                r.cancelled = true;
                r.elapsedMs = msSince(t0);
                ++cancels;
                --inFlight;
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        r.elapsedMs = msSince(t0);
        if (menu && menuDown) {
            r.timedOut = true;
            --inFlight;
            return r;
        }
        if (delay > timeoutMs) {
            r.timedOut = true;
            --inFlight;
            return r;
        }
        r.status = 200;
        r.curlExit = 0;
        if (menu) {
            if (menuStatus != 200) {
                r.status = menuStatus;
            } else if (url.find("/menu?") != std::string::npos || url.find("/menu/servers") != std::string::npos ||
                       url == "http://192.0.2.21:18769/menu") {
                r.body = kRootXml;
            } else if (url.find("/menu/section/local/1/all") != std::string::npos) {
                const size_t p = url.find("page=");
                r.body = pageXml(p == std::string::npos ? 0 : std::atoi(url.c_str() + p + 5), 45);
            } else if (url.find("/menu/item/local/") != std::string::npos) {
                const size_t p = url.find("/menu/item/local/") + 17;
                const std::string rk = url.substr(p, url.find_first_of("?", p) - p);
                int refresh = 0;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    refresh = ++refreshes;
                }
                const auto it = itemOffset.find(rk);
                r.body = itemXml(rk, it == itemOffset.end() ? 0 : it->second, refresh);
            } else if (url.find("/menu/resolve/") != std::string::npos) {
                r.body = "<MediaContainer size=\"1\"><Video title=\"E\" mpxLadderSrc=\""
                         "http%3A%2F%2F%5B%3A%3A1%5D%3A32499%2Flibrary%2Fparts%2F1%2Ff.mkv%3F"
                         "X-Plex-Token%3DREMOTESECRET\" mpxOffsetMs=\"95000\"/></MediaContainer>";
            } else if (url.find("/menu/hub/") != std::string::npos) {
                r.body = "<MediaContainer mpxHeader=\"CONTINUE WATCHING\"></MediaContainer>";
            } else {
                r.status = 404;
            }
        } else if (url.find("/library/sections?") != std::string::npos ||
                   url.find("/library/sections") == url.size() - 17) {
            r.body = kPmsSectionsXml;
        } else if (url.find("/library/sections/1/all") != std::string::npos) {
            r.body = "<MediaContainer title1=\"Movies\"><Video ratingKey=\"5\" "
                     "key=\"/library/metadata/5\" title=\"PMS MOVIE\" viewOffset=\"600000\"/>"
                     "</MediaContainer>";
        } else {
            r.status = 404;
        }
        --inFlight;
        return r;
    }
    size_t count(const std::string& part) {
        std::lock_guard<std::mutex> lk(mu);
        size_t n = 0;
        for (const auto& u : urls)
            n += u.find(part) != std::string::npos;
        return n;
    }
    std::string last() {
        std::lock_guard<std::mutex> lk(mu);
        return urls.empty() ? std::string() : urls.back();
    }
};

struct Rig {
    FakeNet net;
    std::mutex mu;
    std::vector<std::string> logs;
    std::vector<BrowsePlayTicket> plays;
    std::atomic<int> paints{0};
    std::atomic<int> stops{0};
    std::atomic<bool> playing{false};
    uint64_t nextGen = 100;
    std::unique_ptr<BrowseController> bc;

    explicit Rig(bool helper = true) {
        BrowseController::Config c;
        c.menuUrl = helper ? "http://192.0.2.21:18769/menu" : "";
        c.pmsBase = "http://YOUR-PLEX-SERVER:32400";
        c.pmsToken = "CONFSECRET";
        c.cols = 48;
        c.rows = 23;
        BrowseController::Hooks h;
        h.get = [this](const std::string& u, int cm, int tm, const BrowseCancelFn& cf) {
            return net.get(u, cm, tm, cf);
        };
        h.repaint = [this] { ++paints; };
        h.playing = [this] { return playing.load(); };
        h.stopPlayback = [this] {
            ++stops;
            playing.store(false);
        };
        h.play = [this](const BrowsePlayTicket& t) -> uint64_t {
            std::lock_guard<std::mutex> lk(mu);
            plays.push_back(t);
            return ++nextGen;
        };
        h.log = [this](const std::string& l) {
            std::lock_guard<std::mutex> lk(mu);
            logs.push_back(l);
        };
        bc = std::make_unique<BrowseController>(c, h);
        bc->start();
    }
    bool settle(int timeoutMs = 3000) {
        const auto t0 = Clock::now();
        while (msSince(t0) < timeoutMs) {
            if (bc->idle())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    void post(BrowseOp op) {
        bc->post(op);
        settle();
    }
    BrowseController::Snapshot s() { return bc->snapshot(); }
    bool logged(const std::string& part) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& l : logs)
            if (l.find(part) != std::string::npos)
                return true;
        return false;
    }
    bool anyLogHas(const std::string& secret) { return logged(secret); }
};

void testControllerRoot() {
    Rig r;
    r.post(BrowseOp::Open);
    auto s = r.s();
    CHECK(s.visible && s.depth == 1 && s.titles.size() == 4 && s.status.empty() && s.header == "PLEX",
          "root: vis=%d depth=%zu rows=%zu status='%s'", s.visible, s.depth, s.titles.size(),
          s.status.c_str());
    CHECK(r.net.count("/menu?cols=48") == 1, "one helper root GET with cols (%s)", r.net.last().c_str());
    CHECK(r.net.count(":32400") == 0, "no PMS fetch when the helper answers");
    CHECK(r.paints.load() >= 2, "LOADING... painted, then the list (%d)", r.paints.load());

    // Helper down: HELPER OFFLINE + today's PMS sections (A9).
    Rig d;
    d.net.menuDown = true;
    const auto t0 = Clock::now();
    d.post(BrowseOp::Open);
    s = d.s();
    CHECK(s.visible && s.status == "HELPER OFFLINE" && s.titles.size() == 2,
          "offline fallback: status='%s' rows=%zu", s.status.c_str(), s.titles.size());
    CHECK(d.net.count("/library/sections?X-Plex-Token=CONFSECRET") == 1, "PMS sections with conf token");
    CHECK(msSince(t0) < 2500, "fallback after the 1.5 s connect timeout (%lld ms)",
          static_cast<long long>(msSince(t0)));
    CHECK(d.logged("HELPER OFFLINE"), "logged");
    CHECK(!d.anyLogHas("CONFSECRET"), "no token in logs");
    // Fallback rows behave as V1's: enter a section on the PMS, play keeps auto-resume.
    d.post(BrowseOp::Select); // "Movies & Docs"
    s = d.s();
    CHECK(s.depth == 2 && s.titles.size() == 1 && s.titles[0] == "PMS MOVIE", "PMS section list");
    CHECK(d.net.count("/library/sections/1/all?X-Plex-Token=CONFSECRET") == 1, "PMS list path");
    d.post(BrowseOp::Select);
    CHECK(d.plays.size() == 1 && !d.plays[0].offsetPresent && d.plays[0].source == "pms",
          "PMS row: offset absent (doPlay applies viewOffset as today)");

    // Helper answers 500: also falls back.
    Rig e;
    e.net.menuStatus = 500;
    e.post(BrowseOp::Open);
    CHECK(e.s().status == "HELPER OFFLINE" && e.s().titles.size() == 2, "500 -> fallback");

    // No helper configured: PMS only, no offline status.
    Rig n(false);
    n.post(BrowseOp::Open);
    CHECK(n.s().status.empty() && n.s().titles.size() == 2 && n.net.count(":18769") == 0,
          "no helper: PMS list, status '%s'", n.s().status.c_str());
}

void testControllerPaging() {
    Rig r;
    r.post(BrowseOp::Open);
    r.post(BrowseOp::Down); // MOVIES
    r.post(BrowseOp::Select);
    auto s = r.s();
    CHECK(s.depth == 2 && s.titles.size() == 21 && s.header == "MOVIES 1-20/45" &&
              s.titles.back() == "NEXT >",
          "page 1: depth=%zu rows=%zu header='%s'", s.depth, s.titles.size(), s.header.c_str());
    r.post(BrowseOp::PageDown); // NEXT > replaces the page
    s = r.s();
    CHECK(s.depth == 2 && s.header == "MOVIES 21-40/45" && s.titles.front() == "< PREV" &&
              s.titles.back() == "NEXT >" && s.cursor == 0,
          "page 2: depth=%zu header='%s'", s.depth, s.header.c_str());
    r.post(BrowseOp::PageDown);
    s = r.s();
    CHECK(s.header == "MOVIES 41-45/45" && s.titles.back() == "MOVIE 45", "last page '%s'",
          s.header.c_str());
    r.post(BrowseOp::PageDown); // no NEXT: a screen down, clamped (no wrap)
    s = r.s();
    CHECK(s.header == "MOVIES 41-45/45" && s.cursor == static_cast<int>(s.titles.size()) - 1,
          "pgdn on the last page clamps (cursor %d)", s.cursor);
    r.post(BrowseOp::PageUp); // < PREV
    r.post(BrowseOp::PageUp);
    s = r.s();
    CHECK(s.header == "MOVIES 1-20/45", "back to page 1 '%s'", s.header.c_str());
    // Select NEXT > with the cursor: same as PageDown.
    for (int i = 0; i < 20; ++i)
        r.bc->post(BrowseOp::Down);
    r.settle();
    s = r.s();
    CHECK(s.cursor == 20, "cursor on NEXT > (%d)", s.cursor);
    r.post(BrowseOp::Select);
    CHECK(r.s().header == "MOVIES 21-40/45" && r.s().depth == 2, "NEXT by select");
    r.post(BrowseOp::Back);
    s = r.s();
    CHECK(s.depth == 1 && s.cursor == 1, "back to the root, its cursor kept (%d)", s.cursor);
    r.post(BrowseOp::Back);
    CHECK(!r.s().visible && r.s().depth == 0, "back at the root hides");
    // Paging requests carry cols once.
    CHECK(r.net.count("cols=48&cols") == 0 && r.net.count("cols=30") == 0, "cols kept as given");
}

void testControllerOffsetsAndReopen() {
    Rig r;
    r.post(BrowseOp::Open);
    r.post(BrowseOp::Down);
    r.post(BrowseOp::Select); // MOVIES
    r.post(BrowseOp::Select); // MOVIE 1 -> item 1000 (resume 221000)
    auto s = r.s();
    CHECK(s.depth == 3 && s.titles.size() == 4 && s.titles[2] == "RESUME FROM 03:41" && s.cursor == 2,
          "detail opens on RESUME, the row the renderer highlights: rows=%zu cursor=%d", s.titles.size(),
          s.cursor);
    r.post(BrowseOp::Down); // PLAY FROM START
    r.post(BrowseOp::Down); // wraps past the two info rows
    CHECK(r.s().cursor == 2, "down x2 is back on RESUME (cursor %d)", r.s().cursor);
    r.post(BrowseOp::Select); // RESUME at once: no Down needed (review 09:38)
    CHECK(!r.logged("not activatable"), "the cursor never rests on an info row");
    CHECK(r.plays.size() == 1 && r.plays[0].offsetPresent && r.plays[0].offsetMs == 221000 &&
              r.plays[0].ratingKey == "1000" && r.plays[0].address.empty(),
          "resume play offset=%lld", r.plays.empty() ? -1LL : static_cast<long long>(r.plays[0].offsetMs));
    CHECK(!r.s().visible, "the play hides the browser");
    // The play starts (MediaPlayer::playInternal): the browser owns the session.
    r.bc->onPlayStarted(101);
    r.playing.store(true);
    CHECK(r.bc->ownsSession(), "browse play owns the session");
    // Stop on the remote (A8): stop, then the same detail, refreshed, cursor kept.
    const int before = r.net.refreshes;
    r.post(BrowseOp::StopAndReopen);
    s = r.s();
    CHECK(r.stops.load() == 1 && s.visible && s.depth == 3 && s.cursor == 2 &&
              r.net.refreshes == before + 1 && s.titles[1] == "REFRESH " + std::to_string(before + 1),
          "A8 reopen: vis=%d depth=%zu cursor=%d refreshes=%d", s.visible, s.depth, s.cursor,
          r.net.refreshes);
    r.post(BrowseOp::Down); // PLAY FROM START
    r.post(BrowseOp::Select);
    CHECK(r.plays.size() == 2 && r.plays[1].offsetPresent && r.plays[1].offsetMs == 0,
          "from start: offset 0, present");
    // A phone cast while visible (A10): hidden, not owned, kept for the next open.
    r.bc->onPlayStarted(555);
    CHECK(!r.bc->ownsSession() && !r.s().visible, "a cast: not owned, hidden");
    r.post(BrowseOp::Back); // hidden: no effect
    CHECK(r.s().depth == 3, "back while hidden does nothing");
    r.playing.store(false);
    r.post(BrowseOp::Open);
    s = r.s();
    CHECK(s.visible && s.depth == 3, "open after the cast shows the kept level (depth %zu)", s.depth);
    // Back keeps each level's cursor.
    r.post(BrowseOp::Back);
    s = r.s();
    CHECK(s.depth == 2 && s.cursor == 0 && s.header == "MOVIES 1-20/45", "list cursor kept");
    r.post(BrowseOp::Back);
    CHECK(r.s().cursor == 1, "root cursor kept");

    // Remote (ladder) row: GET the row key, play on the ticket's server.
    Rig l;
    l.post(BrowseOp::Open);
    {
        // Put a ladder row in place: a fake page whose only row is a remote episode.
        l.net.delayMs.clear();
    }
    LibraryRow lr;
    lr.fromMenu = true;
    lr.title = "S01E03";
    lr.ratingKey = "115463";
    lr.mpxSource = "ladder";
    lr.mpxServer = "sabc1234";
    lr.offsetPresent = true;
    lr.offsetMs = 95000;
    lr.key = "/menu/resolve/sabc1234/115463?offsetMs=95000";
    const BrowsePlayTicket t = parseBrowseTicket(
        "<MediaContainer><Video mpxLadderSrc=\"http%3A%2F%2F%5B%3A%3A1%5D%3A32499%2Flibrary%2F"
        "parts%2F1%2Ff.mkv%3FX-Plex-Token%3DREMOTESECRET\" mpxOffsetMs=\"95000\"/></MediaContainer>",
        lr);
    CHECK(t.address == "[::1]" && t.port == "32499" && t.token == "REMOTESECRET" &&
              t.offsetMs == 95000,
          "remote ticket");
}

void testControllerLadderPlay() {
    // A helper page with a ladder row, served by the fake net.
    struct LadderNet : FakeNet {};
    Rig r;
    r.net.delayMs.clear();
    // Use the real controller path: a root with one ladder row.
    BrowseController::Config c;
    c.menuUrl = "http://192.0.2.21:18769/menu";
    c.pmsBase = "http://YOUR-PLEX-SERVER:32400";
    c.pmsToken = "CONFSECRET";
    std::vector<BrowsePlayTicket> plays;
    std::vector<std::string> logs;
    std::mutex mu;
    std::vector<std::string> urls;
    BrowseController::Hooks h;
    h.get = [&](const std::string& u, int, int, const BrowseCancelFn&) {
        {
            std::lock_guard<std::mutex> lk(mu);
            urls.push_back(u);
        }
        BrowseHttpResult res;
        res.status = 200;
        if (u.find("/menu/resolve/") != std::string::npos)
            res.body = "<MediaContainer><Video mpxLadderSrc=\"http%3A%2F%2F%5B%3A%3A1%5D%3A32499"
                       "%2Flibrary%2Fparts%2F1%2Ff.mkv%3FX-Plex-Token%3DREMOTESECRET\" "
                       "mpxOffsetMs=\"95000\"/></MediaContainer>";
        else
            res.body = "<MediaContainer><Video title=\"S01E03 THE NAKED NOW\" type=\"episode\" "
                       "ratingKey=\"115463\" mpxServer=\"sabc1234\" mpxSource=\"ladder\" "
                       "mpxOffsetMs=\"95000\" key=\"/menu/resolve/sabc1234/115463?offsetMs=95000\"/>"
                       "</MediaContainer>";
        return res;
    };
    h.repaint = [] {};
    h.playing = [] { return false; };
    h.play = [&](const BrowsePlayTicket& t) -> uint64_t {
        std::lock_guard<std::mutex> lk(mu);
        plays.push_back(t);
        return 7;
    };
    h.log = [&](const std::string& l) {
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back(l);
    };
    BrowseController bc(c, h);
    bc.start();
    auto settle = [&] {
        for (int i = 0; i < 1500 && !bc.idle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    bc.post(BrowseOp::Open);
    settle();
    bc.post(BrowseOp::Select);
    settle();
    std::lock_guard<std::mutex> lk(mu);
    CHECK(plays.size() == 1, "ladder play %zu", plays.size());
    if (!plays.empty()) {
        CHECK(plays[0].address == "[::1]" && plays[0].port == "32499" && plays[0].protocol == "http" &&
                  plays[0].token == "REMOTESECRET" && plays[0].offsetPresent &&
                  plays[0].offsetMs == 95000 && plays[0].source == "ladder" &&
                  plays[0].key == "/library/metadata/115463",
              "ladder ticket to doPlay");
    }
    bool resolveFetched = false;
    for (const auto& u : urls)
        resolveFetched |= u == "http://192.0.2.21:18769/menu/resolve/sabc1234/115463?offsetMs=95000";
    CHECK(resolveFetched, "the row key was fetched (no cols added)");
    for (const auto& l : logs)
        CHECK(l.find("REMOTESECRET") == std::string::npos && l.find("CONFSECRET") == std::string::npos,
              "token in log line: %s", l.c_str());
}

// The live flow (contract §8): path-only keys fetched as given, action rows played from their
// /menu/ticket key, a 404 ticket answer shows the helper's own mpxError text.
void testControllerLiveTicket() {
    BrowseController::Config c;
    c.menuUrl = "http://192.0.2.21:18769/menu";
    c.pmsBase = "http://YOUR-PLEX-SERVER:32400";
    c.pmsToken = "CONFSECRET";
    c.cols = 48;
    std::mutex mu;
    std::vector<std::string> urls, logs;
    std::vector<BrowsePlayTicket> plays;
    BrowseController::Hooks h;
    h.get = [&](const std::string& u, int, int, const BrowseCancelFn&) {
        {
            std::lock_guard<std::mutex> lk(mu);
            urls.push_back(u);
        }
        BrowseHttpResult res;
        res.status = 200;
        if (u.find("/menu/ticket/local/77/o221000") != std::string::npos) {
            res.body = "<MediaContainer><MpxPlay ratingKey=\"77\" key=\"/library/metadata/77\" "
                       "address=\"127.0.0.1\" port=\"32400\" protocol=\"http\" token=\"HELPERTOK\" "
                       "mpxOffsetMs=\"221000\" mpxPlayable=\"yes\" mpxLadderInput=\"part\"/>"
                       "</MediaContainer>";
        } else if (u.find("/menu/ticket/sb/101/o95000") != std::string::npos) {
            res.body = "<MediaContainer><MpxPlay ratingKey=\"101\" key=\"/library/metadata/101\" "
                       "machineIdentifier=\"midB\" address=\"203.0.113.9\" port=\"32401\" "
                       "protocol=\"http\" token=\"REMOTESECRET\" mpxOffsetMs=\"95000\" "
                       "mpxPlayable=\"yes\" mpxLadderInput=\"server-transcode\"/></MediaContainer>";
        } else if (u.find("/menu/ticket/sb/102/o0") != std::string::npos) {
            res.status = 404;
            res.body = "<MediaContainer mpxError=\"NOT FOUND\"><Directory title=\"NOT FOUND\" "
                       "mpxKind=\"info\" type=\"mpxinfo\"/></MediaContainer>";
        } else if (u.find("/menu/item/local/77/c48") != std::string::npos) {
            res.body = "<MediaContainer><Directory title=\"1979  117M\" mpxKind=\"info\"/>"
                       "<Video title=\"RESUME FROM 03:41\" mpxKind=\"action\" ratingKey=\"77\" "
                       "mpxServer=\"local\" mpxOffsetMs=\"221000\" "
                       "key=\"/menu/ticket/local/77/o221000\"/></MediaContainer>";
        } else {
            res.body = "<MediaContainer><Directory title=\"ALIEN\" key=\"/menu/item/local/77/c48\" "
                       "mpxKind=\"item\"/><Video title=\"S01E03\" mpxKind=\"action\" "
                       "ratingKey=\"101\" mpxServer=\"sb\" mpxOffsetMs=\"95000\" "
                       "key=\"/menu/ticket/sb/101/o95000\"/><Video title=\"GONE\" "
                       "mpxKind=\"action\" ratingKey=\"102\" mpxServer=\"sb\" mpxOffsetMs=\"0\" "
                       "key=\"/menu/ticket/sb/102/o0\"/></MediaContainer>";
        }
        return res;
    };
    h.repaint = [] {};
    h.playing = [] { return false; };
    h.play = [&](const BrowsePlayTicket& t) -> uint64_t {
        std::lock_guard<std::mutex> lk(mu);
        plays.push_back(t);
        return 9;
    };
    h.log = [&](const std::string& l) {
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back(l);
    };
    BrowseController bc(c, h);
    bc.start();
    auto op = [&](BrowseOp o) {
        bc.post(o);
        for (int i = 0; i < 1500 && !bc.idle(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto fetched = [&](const std::string& want) {
        std::lock_guard<std::mutex> lk(mu);
        return std::count(urls.begin(), urls.end(), want);
    };
    op(BrowseOp::Open);
    CHECK(fetched("http://192.0.2.21:18769/menu?cols=48") == 1, "root with cols");
    op(BrowseOp::Select); // ALIEN
    CHECK(fetched("http://192.0.2.21:18769/menu/item/local/77/c48") == 1,
          "path-only key fetched as given");
    op(BrowseOp::Down);
    op(BrowseOp::Select); // RESUME
    CHECK(fetched("http://192.0.2.21:18769/menu/ticket/local/77/o221000") == 1, "local ticket GET");
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(plays.size() == 1 && plays[0].source == "local" && plays[0].address.empty() &&
                  plays[0].token.empty() && plays[0].offsetMs == 221000 &&
                  plays[0].ladderInput.empty(),
              "local live play (%zu)", plays.size());
    }
    op(BrowseOp::Open); // the kept detail
    op(BrowseOp::Back); // root
    op(BrowseOp::Down);
    op(BrowseOp::Select); // S01E03 on the friend's server
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(plays.size() == 2 && plays.back().address == "203.0.113.9" &&
                  plays.back().ladderInput == "server-transcode" &&
                  plays.back().token == "REMOTESECRET" && plays.back().offsetMs == 95000,
              "remote live play (%zu)", plays.size());
    }
    op(BrowseOp::Open);
    op(BrowseOp::Down);
    op(BrowseOp::Select); // GONE: 404 + mpxError
    const auto s = bc.snapshot();
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(plays.size() == 2 && s.visible && s.status == "NOT FOUND",
              "404 ticket: status '%s', plays %zu", s.status.c_str(), plays.size());
        for (const auto& l : logs)
            CHECK(l.find("REMOTESECRET") == std::string::npos &&
                      l.find("HELPERTOK") == std::string::npos,
                  "token in log line: %s", l.c_str());
    }
}

// The helper's server-transcode relay (contract §9.3 rev, mpx-menu 59044aa): its start is in
// its path, so a play at another start fetches the ticket again there; else the Part.
void testLadderInputForStart() {
    const std::string tk = "http://192.0.2.21:18769/menu/ticket/sb/101/o95000";
    CHECK(browseTicketUrlAtOffset(tk, 120000) == "http://192.0.2.21:18769/menu/ticket/sb/101/o120000",
          "o<ms> rewritten: %s", browseTicketUrlAtOffset(tk, 120000).c_str());
    CHECK(browseTicketUrlAtOffset(tk + "/a1", 7) == "http://192.0.2.21:18769/menu/ticket/sb/101/o7/a1",
          "/a1 kept after o<ms>: %s", browseTicketUrlAtOffset(tk + "/a1", 7).c_str());
    CHECK(browseTicketUrlAtOffset(tk + "?x=1", 0) == "http://192.0.2.21:18769/menu/ticket/sb/101/o0?x=1",
          "query kept");
    CHECK(browseTicketUrlAtOffset(tk, -5) == "http://192.0.2.21:18769/menu/ticket/sb/101/o0", "negative -> o0");
    CHECK(browseTicketUrlAtOffset("http://h/menu/resolve/sb/101?offsetMs=1", 2).empty(), "no o<ms>: empty");
    CHECK(browseTicketUrlAtOffset("http://h/menu/ticket/sb/101/o", 2).empty(), "bare o: empty");
    CHECK(browseTicketUrlAtOffset("http://h/menu/ticket/sb/101/oX9", 2).empty(), "o + non-digits: empty");
    CHECK(browseTicketUrlAtOffset("", 2).empty(), "empty URL");

    const std::string relay0 = "http://127.0.0.1:18769/menu/tsrc/sb/101/o95000";
    std::vector<std::string> asked;
    std::string answer;
    int status = 200;
    auto get = [&](const std::string& url) {
        asked.push_back(url);
        BrowseHttpResult r;
        r.status = status;
        r.curlExit = 0;
        r.body = answer;
        return r;
    };
    auto ticket = [](const char* rk, int64_t off, const char* input) {
        return std::string("<MediaContainer><MpxPlay ratingKey=\"") + rk +
               "\" key=\"/library/metadata/" + rk + "\" address=\"203.0.113.9\" port=\"32401\" "
               "protocol=\"http\" token=\"REMOTESECRET\" mpxOffsetMs=\"" + std::to_string(off) +
               "\" mpxPlayable=\"yes\" mpxLadderInput=\"" + input + "\" mpxLadderSrc=\"http%3A%2F%2F"
               "127.0.0.1%3A18769%2Fmenu%2Ftsrc%2Fsb%2F" + rk + "%2Fo" + std::to_string(off) +
               "\"/></MediaContainer>";
    };
    // Not a relay, or the same start: unchanged, nothing fetched.
    BrowseLadderInput li = browseLadderInputForStart("http://p/part", "part", 95000, tk, "101", 1000, get);
    CHECK(li.src == "http://p/part" && !li.serverTranscode && !li.refetched && asked.empty(), "part passes");
    li = browseLadderInputForStart(relay0, "server-transcode", 95000, tk, "101", 95000, get);
    CHECK(li.src == relay0 && li.serverTranscode && !li.refetched && asked.empty(), "same start keeps the relay");
    li = browseLadderInputForStart("", "server-transcode", 95000, tk, "101", 1000, get);
    CHECK(li.src.empty() && li.serverTranscode && !li.refetched && asked.empty(),
          "no helper src: the daemon-side owner transcode, nothing fetched");
    // Another start: the ticket at that start, its relay used.
    answer = ticket("101", 120000, "server-transcode");
    li = browseLadderInputForStart(relay0, "server-transcode", 95000, tk, "101", 120000, get);
    CHECK(asked.size() == 1 && asked[0] == "http://192.0.2.21:18769/menu/ticket/sb/101/o120000",
          "re-fetch at the new start (%zu)", asked.size());
    CHECK(li.refetched && li.serverTranscode && li.src == "http://127.0.0.1:18769/menu/tsrc/sb/101/o120000",
          "new relay: '%s'", li.src.c_str());
    CHECK(li.note.find("re-fetched at the new start") != std::string::npos &&
              li.note.find("REMOTESECRET") == std::string::npos,
          "note: %s", li.note.c_str());
    // The re-fetch fails, or answers for another item, or no longer a relay: the Part.
    for (int c = 0; c < 4; ++c) {
        asked.clear();
        status = c == 0 ? 500 : 200;
        answer = c == 1 ? ticket("999", 120000, "server-transcode")
                        : c == 2 ? ticket("101", 120000, "part")
                                 : c == 3 ? "<MediaContainer mpxError=\"OFFLINE\"/>" : "";
        li = browseLadderInputForStart(relay0, "server-transcode", 95000, tk, "101", 120000, get);
        CHECK(li.refetched && li.src.empty() && !li.serverTranscode && asked.size() == 1 &&
                  li.note.find("re-fetch failed; the original Part") != std::string::npos,
              "fallback case %d: src='%s' st=%d", c, li.src.c_str(), li.serverTranscode);
    }
    status = 200;
    // No o<ms> in the ticket URL (P0 resolve keys): nothing fetched, the Part.
    asked.clear();
    li = browseLadderInputForStart(relay0, "server-transcode", 95000,
                                   "http://h/menu/resolve/sb/101?offsetMs=95000", "101", 120000, get);
    CHECK(li.refetched && li.src.empty() && !li.serverTranscode && asked.empty(), "no o<ms>: no GET, Part");
    // The /a1 helper's ticket keys.
    asked.clear();
    answer = ticket("101", 30000, "server-transcode");
    li = browseLadderInputForStart(relay0, "server-transcode", 95000, tk + "/a1", "101", 30000, get);
    CHECK(asked.size() == 1 && asked[0] == "http://192.0.2.21:18769/menu/ticket/sb/101/o30000/a1" &&
              li.src == "http://127.0.0.1:18769/menu/tsrc/sb/101/o30000",
          "/a1 ticket re-fetch: %s", asked.empty() ? "-" : asked[0].c_str());
}

// The cancel race: a fetch in flight, newer input. The fetch ends within a few ms of the
// post, its result is dropped, the newer op applies, and post() itself never waits.
void testCancelRace() {
    {
        Rig r;
        r.post(BrowseOp::Open);
        r.post(BrowseOp::Down); // MOVIES
        r.net.delayMs["/menu/section/local/1/all"] = 2000;
        r.bc->post(BrowseOp::Select);
        // Wait until the fetch is in flight.
        for (int i = 0; i < 500 && r.net.inFlight.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(r.net.inFlight.load() == 1, "fetch in flight");
        CHECK(r.s().status == "LOADING...", "LOADING... while fetching ('%s')", r.s().status.c_str());
        const auto t0 = Clock::now();
        r.bc->post(BrowseOp::Down); // newer input
        const int64_t postMs = msSince(t0);
        CHECK(postMs < 5, "post() never waits (%lld ms)", static_cast<long long>(postMs));
        CHECK(r.settle(1000), "worker idle after the cancel");
        const int64_t endMs = msSince(t0);
        const auto s = r.s();
        CHECK(endMs < 200, "cancel took %lld ms", static_cast<long long>(endMs));
        CHECK(r.net.cancels.load() == 1 && s.depth == 1 && s.cursor == 2 && s.status.empty(),
              "dropped: cancels=%d depth=%zu cursor=%d status='%s'", r.net.cancels.load(), s.depth,
              s.cursor, s.status.c_str());
        CHECK(r.logged("cancelled after"), "cancel logged");
    }
    // Randomized: Select (slow fetch) then 0-3 other ops after a random delay. The final state
    // is the one the newest ops imply; the worker never wedges.
    std::mt19937 rng(1004);
    int raced = 0;
    for (int iter = 0; iter < 60; ++iter) {
        Rig r;
        r.post(BrowseOp::Open);
        r.post(BrowseOp::Down);
        const int delay = 5 + static_cast<int>(rng() % 40);
        r.net.delayMs["/menu/section/local/1/all"] = delay;
        r.bc->post(BrowseOp::Select);
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 50));
        const int extra = static_cast<int>(rng() % 3);
        for (int k = 0; k < extra; ++k)
            r.bc->post(BrowseOp::Up);
        CHECK(r.settle(2000), "iteration %d wedged", iter);
        const auto s = r.s();
        // Either the fetch finished before the Up(s) (depth 2) or was cancelled (depth 1).
        CHECK(s.depth == 1 || s.depth == 2, "depth %zu", s.depth);
        CHECK(s.status != "LOADING...", "LOADING... left on screen (iter %d)", iter);
        if (s.depth == 1) {
            ++raced;
            CHECK(r.net.cancels.load() == 1, "a dropped fetch was cancelled (iter %d)", iter);
        }
        CHECK(r.net.inFlight.load() == 0, "no fetch left in flight");
    }
    CHECK(raced > 0, "the race was exercised (%d of 60 cancelled)", raced);
    // A cast starting mid-fetch cancels it too (A10).
    {
        Rig r;
        r.post(BrowseOp::Open);
        r.post(BrowseOp::Down);
        r.net.delayMs["/menu/section/local/1/all"] = 2000;
        r.bc->post(BrowseOp::Select);
        for (int i = 0; i < 500 && r.net.inFlight.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto t0 = Clock::now();
        r.playing.store(true);
        r.bc->onPlayStarted(999);
        CHECK(r.settle(1000) && msSince(t0) < 200, "cast cancels the fetch (%lld ms)",
              static_cast<long long>(msSince(t0)));
        CHECK(!r.s().visible && r.net.cancels.load() == 1, "hidden, cancelled");
    }
    // Shutdown with a fetch in flight returns at once.
    {
        auto r = std::make_unique<Rig>();
        r->post(BrowseOp::Open);
        r->post(BrowseOp::Down);
        r->net.delayMs["/menu/section/local/1/all"] = 5000;
        r->bc->post(BrowseOp::Select);
        for (int i = 0; i < 500 && r->net.inFlight.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto t0 = Clock::now();
        r->bc->stop();
        CHECK(msSince(t0) < 200, "stop() with a fetch in flight took %lld ms",
              static_cast<long long>(msSince(t0)));
    }
}

void testOpenWhilePlaying() {
    Rig r;
    r.playing.store(true);
    r.post(BrowseOp::Open);
    CHECK(r.stops.load() == 1 && r.s().visible && r.s().titles.size() == 4,
          "Browse during playback stops first (stops=%d)", r.stops.load());
}

// ---- http (real curl against a local server) ----
struct Server {
    pid_t pid = -1;
    int port = 0;
    ~Server() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

bool startServer(const char* script, Server& s) {
    char tmpl[] = "/tmp/browse-port-XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0)
        return false;
    ::close(fd);
    ::unlink(tmpl);
    s.pid = ::fork();
    if (s.pid == 0) {
        ::execlp("python3", "python3", script, "--port-file", tmpl, "--menu-only", nullptr);
        ::_exit(127);
    }
    for (int i = 0; i < 300; ++i) {
        std::ifstream f(tmpl);
        int p = 0;
        if (f >> p && p > 0) {
            s.port = p;
            ::unlink(tmpl);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void testHttp(const char* script) {
    Server srv;
    if (!startServer(script, srv)) {
        CHECK(false, "test server did not start");
        return;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port);
    auto none = [] { return false; };
    BrowseHttpResult r = browseHttpGet(base + "/menu?cols=48", 1500, 3000, none);
    CHECK(r.ok() && r.body.find("<MediaContainer") != std::string::npos && r.curlExit == 0,
          "ok: status=%d exit=%d bytes=%zu", r.status, r.curlExit, r.body.size());
    r = browseHttpGet(base + "/status/500", 1500, 3000, none);
    CHECK(!r.ok() && r.status == 500 && !r.unreachable(), "HTTP 500 is an answer (%d)", r.status);
    // Refused: a port nothing listens on answers at once.
    r = browseHttpGet("http://127.0.0.1:1/menu", 1500, 3000, none);
    CHECK(r.unreachable() && r.elapsedMs < 1000, "refused: status=%d exit=%d %lld ms", r.status,
          r.curlExit, static_cast<long long>(r.elapsedMs));
    // Slow: the 1.5 s budget ends it near 1.5 s, whatever the server does.
    r = browseHttpGet(base + "/slow/5000", 1500, 1500, none);
    CHECK(r.unreachable() && r.timedOut && r.elapsedMs >= 1400 && r.elapsedMs < 2200,
          "timeout: %lld ms exit=%d", static_cast<long long>(r.elapsedMs), r.curlExit);
    // Cancel mid-fetch: back within a poll slice; curl is reaped (no zombie left).
    std::atomic<bool> stop{false};
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true);
    });
    const auto t0 = Clock::now();
    r = browseHttpGet(base + "/slow/5000", 1500, 8000, [&] { return stop.load(); });
    const int64_t took = msSince(t0);
    t.join();
    CHECK(r.cancelled && took < 400, "cancel: %lld ms", static_cast<long long>(took));
    CHECK(::waitpid(-1, nullptr, WNOHANG) <= 0, "no unreaped child after a cancel");
    // A token in the URL never reaches stderr/stdout of this process (curl's stderr is null).
    r = browseHttpGet(base + "/menu?X-Plex-Token=SECRET", 1500, 3000, none);
    CHECK(r.ok(), "token URL fetch");
}
} // namespace

int main(int argc, char** argv) {
    testParse();
    testUrls();
    testTickets();
    testLiveShapes();
    testWindow();
    testCommands();
    testControllerRoot();
    testControllerPaging();
    testControllerOffsetsAndReopen();
    testControllerLadderPlay();
    testControllerLiveTicket();
    testLadderInputForStart();
    testCancelRace();
    testOpenWhilePlaying();
    if (argc > 1)
        testHttp(argv[1]);
    else
        std::printf("test_browse_v2: http checks SKIP (no server script)\n");
    if (g_fail) {
        std::printf("test_browse_v2: %d FAIL of %d checks\n", g_fail, g_checks);
        return 1;
    }
    std::printf("test_browse_v2: OK (%d checks)\n", g_checks);
    return 0;
}
