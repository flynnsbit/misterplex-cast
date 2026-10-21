// R1 adapter (browse_r1_adapter.hpp): BrowseR1View -> RenderModel. Pure, in-process.
#include "libmisterplex/browse_r1_adapter.hpp"

#include <cstdio>
#include <string>

using namespace misterplex;
using namespace misterplex::browse;

namespace {
int g_fails = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

LibraryRow menuRow(const std::string& title, bool dir, const std::string& kind = "", int pct = -1,
                   const std::string& type = "") {
    LibraryRow r;
    r.title = title;
    r.directory = dir;
    r.fromMenu = true;
    r.mpxKind = kind;
    r.mpxPct = pct;
    r.type = type;
    r.key = "/menu/x";
    return r;
}

BrowseR1View view(const std::string& url, bool fromMenu = true) {
    BrowseR1View v;
    v.visible = true;
    v.levelUrl = url;
    v.fromMenu = fromMenu;
    return v;
}
} // namespace

int main() {
    // States from the level URL.
    struct S {
        const char* url;
        bool menu;
        BrowseState want;
    } states[] = {
        {"http://127.0.0.1:18769/menu/servers/c70/a1", true, BrowseState::S2Servers},
        {"http://127.0.0.1:18769/menu/c70/a1", true, BrowseState::S3Root},
        {"http://127.0.0.1:18769/menu?cols=70", true, BrowseState::S3Root},
        {"http://127.0.0.1:18769/menu", true, BrowseState::S3Root},
        {"http://127.0.0.1:18769/menu/section/local/1/all/s0/c70/a1", true, BrowseState::S4List},
        {"http://127.0.0.1:18769/menu/item/local/103/a1", true, BrowseState::S5Detail},
        {"http://127.0.0.1:18769/menu/s/sremote1/a1", true, BrowseState::S4List},
        {"/library/sections", false, BrowseState::S3Root},
        {"/library/sections/1/all", false, BrowseState::S4List},
    };
    for (const auto& s : states)
        CHECK(r1StateForLevel(view(s.url, s.menu)) == s.want, "state for %s", s.url);

    // Q2: the three action labels; other titles unchanged.
    CHECK(r1ActionLabel("RESUME FROM 43:20") == "Resume from 43:20", "resume");
    CHECK(r1ActionLabel("PLAY FROM START") == "Play from start", "from start");
    CHECK(r1ActionLabel("PLAY") == "Play", "play");
    CHECK(r1ActionLabel("Playtime") == "Playtime" && r1ActionLabel("PLAY ") == "PLAY ", "only exact labels");
    {
        LibraryRow a = menuRow("RESUME FROM 01:01", false, "action");
        CHECK(r1Row(a).label == "Resume from 01:01" && r1Row(a).kind == RowKind::Item, "action row");
        LibraryRow t = menuRow("PLAY", false, "", -1, "movie"); // a title, not an action row
        CHECK(r1Row(t).label == "PLAY", "a title PLAY is not an action");
    }
    // Paging rows: kind + label; the renderer adds the arrows.
    {
        LibraryRow n = menuRow("NEXT", true, "page", -1, "page");
        LibraryRow p = menuRow("PREV", true, "page", -1, "page");
        CHECK(r1Row(n).kind == RowKind::PageNext && r1Row(n).label == "Next", "next");
        CHECK(r1Row(p).kind == RowKind::PagePrev && r1Row(p).label == "Prev", "prev");
    }
    // Info / offline / header rows are Info; directories; items.
    CHECK(r1Row(menuRow("1982  117m", true, "info")).kind == RowKind::Info, "info");
    CHECK(r1Row(menuRow("SHARED B  OFFLINE", true, "offline")).kind == RowKind::Info, "offline");
    CHECK(r1Row(menuRow("Movies", true)).kind == RowKind::Directory, "directory");
    CHECK(r1Row(menuRow("Blade Runner (1982)", false)).kind == RowKind::Item, "item");
    // Q5: trailing double-space W is the watched mark; a W inside a title is not.
    {
        Row w = r1Row(menuRow("Blade Runner (1982)  W", false));
        CHECK(w.watched && w.label == "Blade Runner (1982)", "watched: '%s'", w.label.c_str());
        Row x = r1Row(menuRow("Agent W", false));
        CHECK(!x.watched && x.label == "Agent W", "single-space W is part of the title");
        Row y = r1Row(menuRow("S01E03  The Naked Now  46m  W", false));
        CHECK(y.watched && y.label == "S01E03  The Naked Now  46m", "episode W: '%s'", y.label.c_str());
    }
    // pct: 1..99 drawn; 0, 100 and none not.
    CHECK(r1Row(menuRow("A", false, "", 37)).pct == 37, "pct 37");
    CHECK(r1Row(menuRow("A", false, "", 0)).pct == -1 && r1Row(menuRow("A", false, "", 100)).pct == -1 &&
              r1Row(menuRow("A", false)).pct == -1,
          "pct only 1..99");

    // Viewport: cursor kept on rows 2..12 of the 15.
    CHECK(r1ViewportTop(0, 40, 0) == 0 && r1ViewportTop(12, 40, 0) == 0, "no scroll inside the band");
    CHECK(r1ViewportTop(13, 40, 0) == 1, "scroll down past row 12");
    CHECK(r1ViewportTop(3, 40, 5) == 1, "scroll up above row 2");
    CHECK(r1ViewportTop(39, 40, 0) == 25 && r1ViewportTop(5, 10, 3) == 0, "clamped to the last page / short lists");

    // The model: header, page indicator, footer and error rules.
    {
        BrowseR1View v = view("http://127.0.0.1:18769/menu/section/local/1/all/s20/c70/a1");
        v.header = "Movies";
        v.mpxPage = "21-40 / 812";
        v.rows = {menuRow("PREV", true, "page", -1, "page"), menuRow("Alien (1979)  W", false),
                  menuRow("Blade Runner (1982)", false, "", 37)};
        v.cursor = 2;
        RenderModel m = r1RenderModel(v, 0);
        CHECK(m.state == BrowseState::S4List && m.headerTitle == "Movies" && m.pageIndicator == "21-40 / 812",
              "list header");
        CHECK(m.rows.size() == 3 && m.rows[0].kind == RowKind::PagePrev && m.rows[1].watched && m.rows[2].pct == 37,
              "rows mapped");
        CHECK(m.cursorIndex == 2 && !m.error && !m.footerError && m.footerText.empty(), "clean list");
        v.status = "NOT PLAYABLE HERE";
        m = r1RenderModel(v, 0);
        CHECK(m.state == BrowseState::S4List && m.footerError && !m.error && m.footerText == "NOT PLAYABLE HERE",
              "status-line error: footer only");
        v.status = "LOADING...";
        m = r1RenderModel(v, 0);
        CHECK(!m.footerError && !m.error && m.footerText == "LOADING...", "LOADING is dim");
        v.rows.clear();
        v.status = "HELPER OFFLINE";
        m = r1RenderModel(v, 0);
        CHECK(m.state == BrowseState::S8Error && m.error, "no rows + a reason = S8");
        v.status = "EMPTY";
        m = r1RenderModel(v, 0);
        CHECK(m.state == BrowseState::S4List && !m.error && !m.footerError, "EMPTY is not an error");
    }
    {
        BrowseR1View v = view("http://127.0.0.1:18769/menu/servers/a1");
        RenderModel m = r1RenderModel(v, 0);
        CHECK(m.state == BrowseState::S2Servers && m.headerTitle == "Servers", "servers default header");
    }
    // One rule (review 09:48): the rows the renderer may highlight are the rows the cursor may rest on,
    // and an inert row never activates, over every fromMenu/directory/type/mpxKind/title shape.
    {
        const char* types[] = {"", "page", "mpxpage", "mpxinfo", "mpxitem", "movie"};
        const char* kinds[] = {"", "info", "offline", "header", "page", "action", "item", "server", "az"};
        const char* titles[] = {"NEXT", "PREV", "X"};
        for (int fm = 0; fm < 2; ++fm)
            for (int dir = 0; dir < 2; ++dir)
                for (const char* ty : types)
                    for (const char* k : kinds)
                        for (const char* ti : titles) {
                            LibraryRow r;
                            r.fromMenu = fm != 0;
                            r.directory = dir != 0;
                            r.type = ty;
                            r.mpxKind = k;
                            r.title = ti;
                            r.key = "/k";
                            r.ratingKey = "7";
                            const bool drawn = selectable(r1Row(r).kind);
                            CHECK(drawn == LibraryBrowser::selectable(r),
                                  "renderer vs cursor: fromMenu=%d dir=%d type='%s' mpxKind='%s' title='%s'", fm,
                                  dir, ty, k, ti);
                            if (!drawn)
                                CHECK(!browseRowActivatable(r), "an inert row never activates: type='%s' mpxKind='%s'",
                                      ty, k);
                        }
        LibraryRow odd;
        odd.fromMenu = true;
        odd.directory = true;
        odd.type = "page";
        odd.mpxKind = "info";
        odd.title = "NEXT";
        CHECK(r1Row(odd).kind == RowKind::Info, "a page row marked info is drawn as info, not as Next");
    }

    if (g_fails) {
        std::printf("test_browse_r1_adapter: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_browse_r1_adapter: OK (%d checks)\n", g_checks);
    return 0;
}
