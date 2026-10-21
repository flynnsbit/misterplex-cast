// OB port: ring_in nav -> PlexUI ops (owenb_plexui_nav.hpp), the PLXI 5-11 mapping on R1.
#include "libmisterplex/owenb_plexui_nav.hpp"
#include <cstdio>

using namespace misterplex;
using namespace misterplex::owenb;

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void op(ObNav n, BrowseOp want) {
    const PlexUiObOp r = plexUiObNav(n, true, false);
    CHECK(r.take && !r.horizontal && r.op == want);
}

int main() {
    op(ObNav::Up, BrowseOp::Up);
    op(ObNav::Down, BrowseOp::Down);
    op(ObNav::Select, BrowseOp::Select);
    op(ObNav::PlayPause, BrowseOp::Select);
    op(ObNav::Back, BrowseOp::Back);
    op(ObNav::Home, BrowseOp::Open);   // PLXI 5 Browse: Home, never Back
    op(ObNav::PageUp, BrowseOp::PageUp);
    op(ObNav::PageDown, BrowseOp::PageDown);
    op(ObNav::Info, BrowseOp::Info);
    PlexUiObOp l = plexUiObNav(ObNav::Left, true, false), r = plexUiObNav(ObNav::Right, true, false);
    CHECK(l.take && l.horizontal && l.delta == -1);
    CHECK(r.take && r.horizontal && r.delta == 1);
    // Menu / Aux keep the Program panel; an open panel and a hidden UI take nothing
    CHECK(!plexUiObNav(ObNav::Menu, true, false).take);
    CHECK(!plexUiObNav(ObNav::Aux, true, false).take);
    for (ObNav n : {ObNav::Up, ObNav::Select, ObNav::Home, ObNav::Back, ObNav::Left}) {
        CHECK(!plexUiObNav(n, false, false).take);
        CHECK(!plexUiObNav(n, true, true).take);
    }
    std::printf("test_owenb_plexui_nav: %d/%d\n", checks - fails, checks);
    return fails ? 1 : 0;
}
