// OWENB-P1B 0010: the OB input decoder (owenb_input.hpp), the menu panel and the routing
// (owenb_menu.hpp), the library page's rows (owenb_library.hpp).
//   I1-I6  the decoder on a hand-built input region (h264ring.h:395-421): start, keys, pad edges +
//          repeat, Main's OSD (actions after the close), a lost queue, any pad
//   M1-M3  the panel from the daemon's state: items, refresh, commands
//   R1     the routing (library, play, idle)
//   L1     the library page's window
// Mutants: MUT_INPUT_ACT_IN_OSD MUT_INPUT_LEVEL MUT_MENU_STALE MUT_ROUTE_HOME_STOPS_PLAY
//          MUT_LIBRARY_WINDOW_FROM_TOP MUT_LIBRARY_INFO_SELECTABLE -- each must FAIL.
#include "libmisterplex/owenb_input.hpp"
#include "libmisterplex/owenb_library.hpp"
#include "libmisterplex/owenb_menu.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace misterplex;
using namespace misterplex::owenb;

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        ++g_checks;                                     \
        if (!(c)) {                                     \
            ++g_fail;                                   \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                   \
            std::printf("\n");                          \
        }                                               \
    } while (0)

// The region as the core writes it (h264ring_layout.h:766-816, the queue at word 56).
struct Region {
    struct ::h264ring_input in{};
    uint32_t evc = 0;
    void setCount() {
        in.w[0] = (in.w[0] & 0xFFFFFFFFull) | (uint64_t(evc) << 32);
    }
    void push(uint16_t e) {
        const unsigned slot = evc % H264RING_IN_Q_LEN;
        uint64_t& w = in.w[H264RING_IN_Q_WORD - H264RING_IN_WORD + slot / 4];
        const unsigned sh = 16 * (slot % 4);
        w = (w & ~(0xFFFFull << sh)) | (uint64_t(e) << sh);
        ++evc;
        setCount();
    }
    void key(unsigned code, bool ext, bool pressed) {
        push(uint16_t((H264RING_EV_KEY << H264RING_EV_TYPE_LSB) | (pressed ? 1u << 9 : 0) | (ext ? 1u << 8 : 0) | code));
    }
    void tap(unsigned code, bool ext = false) {
        key(code, ext, true);
        key(code, ext, false);
    }
    void osd(bool open) { push(uint16_t((H264RING_EV_OSD << H264RING_EV_TYPE_LSB) | (open ? 1 : 0))); }
    void action(unsigned a) { push(uint16_t((H264RING_EV_ACTION << H264RING_EV_TYPE_LSB) | a)); }
    void pad(unsigned n, uint32_t bits) {
        uint64_t& w = in.w[H264RING_PAD0_JOY_W - H264RING_IN_WORD + n];
        w = (w & ~0xFFFFFFFFull) | bits;
    }
};

static std::string names(const std::vector<ObNav>& v) {
    std::string s;
    for (ObNav n : v)
        s += std::string(s.empty() ? "" : ",") + obNavName(n);
    return s;
}

static void testDecoder() {
    {   // I1: events before the start are not ours; a button held at the start is not a press
        Region r;
        r.tap(0x5A);
        r.pad(0, H264RING_PAD_A);
        ObInputDecoder d;
        CHECK(d.feed(r.in, 0).empty(), "I1 the first copy only syncs");
        CHECK(d.feed(r.in, 15).empty(), "I1 a held A at the start is no press, the old Enter no event");
        r.pad(0, 0);
        CHECK(d.feed(r.in, 30).empty(), "I1 the release: nothing");
        r.pad(0, H264RING_PAD_A);
        CHECK(names(d.feed(r.in, 45)) == "select", "I1 the next press is a select");
    }
    {   // I2: keys (PS/2 set 2): the make codes only, unmapped codes ignored
        Region r;
        ObInputDecoder d;
        d.feed(r.in, 0);
        r.tap(0x75, true);   // Up
        r.tap(0x72, true);   // Down
        r.tap(0x5A, true);   // keypad Enter
        r.tap(0x1C);         // A: nothing
        r.tap(0x76);         // Esc
        r.tap(0x3A);         // M
        r.tap(0x75);         // keypad 8 (no E0): not an arrow
        const std::string got = names(d.feed(r.in, 15));
        CHECK(got == "up,down,select,back,menu", "I2 keys -> %s", got.c_str());
        CHECK(d.stats().keys == 5 && d.stats().events == 14, "I2 keys %u events %u", d.stats().keys, d.stats().events);
    }
    {   // I3: pad edges; the d-pad and LB/RB repeat (400 ms, then every 120 ms), A does not
        Region r;
        ObInputDecoder d;
        d.feed(r.in, 0);
        r.pad(0, H264RING_PAD_A);
        int64_t t = 15;
        std::string a = names(d.feed(r.in, t));
        for (int i = 0; i < 60; ++i)
            a += names(d.feed(r.in, t += 15));
        CHECK(a == "select", "I3 A held 0.9 s: one select (%s)", a.c_str());
        r.pad(0, 0);
        d.feed(r.in, t += 15);
        r.pad(0, H264RING_PAD_DOWN);
        const int64_t t0 = t += 15;
        unsigned downs = unsigned(d.feed(r.in, t0).size());
        while (t < t0 + 1000)
            downs += unsigned(d.feed(r.in, t += 10).size());
        // at 0, 400, 520, 640, 760, 880, 1000
        CHECK(downs == 7, "I3 Down held 1 s: 7 downs (%u)", downs);
        r.pad(0, H264RING_PAD_DOWN | H264RING_PAD_B);
        const std::string both = names(d.feed(r.in, t += 10));
        CHECK(both == "back", "I3 B while Down is held: back (%s)", both.c_str());
    }
    {   // I4: Main's OSD has the input while open; its actions arrive before the close, act after it
        Region r;
        ObInputDecoder d;
        d.feed(r.in, 0);
        r.osd(true);
        r.tap(0x75, true);
        r.pad(0, H264RING_PAD_A);   // A navigates Main's menu
        r.action(1);
        auto v = d.feed(r.in, 15);
        CHECK(v.empty() && d.osdOpen(), "I4 the OSD open: nothing (%s)", names(v).c_str());
        r.osd(false);   // A still held as the OSD closes: not a press for us
        v = d.feed(r.in, 30);
        CHECK(names(v) == "menu" && !d.osdOpen(), "I4 the close delivers the Program menu action (%s)", names(v).c_str());
        CHECK(d.feed(r.in, 45).empty(), "I4 the A held through the close is no select");
        r.osd(true);
        r.action(0);
        r.osd(false);
        v = d.feed(r.in, 60);
        CHECK(names(v) == "home", "I4 Send Home -> home after the close (%s)", names(v).c_str());
        r.action(7);
        CHECK(d.feed(r.in, 75).empty(), "I4 an unknown action is ignored");
        CHECK(d.stats().unknownActions == 1 && d.stats().lastUnknownAction == 7,
              "I4 ... but counted and kept for the log (bug 3: what the RBF really sends)");
    }
    {   // I5: a queue read too late: counted lost, the newest 32 taken
        Region r;
        ObInputDecoder d;
        d.feed(r.in, 0);
        for (int i = 0; i < 20; ++i)
            r.tap(0x72, true);   // 40 events
        auto v = d.feed(r.in, 15);
        CHECK(d.stats().lost == 8 && v.size() == 16, "I5 lost %u, downs %zu", d.stats().lost, v.size());
    }
    {   // I6: any of the six pads drives the menus
        Region r;
        ObInputDecoder d;
        d.feed(r.in, 0);
        r.pad(5, H264RING_PAD_START);
        CHECK(names(d.feed(r.in, 15)) == "menu", "I6 pad 5's Start -> menu");
        r.pad(3, H264RING_PAD_BACK);
        CHECK(names(d.feed(r.in, 30)) == "home", "I6 pad 3's Back -> home");
    }
}

static std::string labels(const ObProgramMenu& m) {
    std::string s;
    for (const auto& it : m.items())
        s += (s.empty() ? "" : "|") + it.label;
    return s;
}

static ObMenuState playingState() {
    ObMenuState s;
    s.playing = true;
    s.title = "Blade Runner";
    s.positionMs = 3723000;
    s.durationMs = 7020000;
    s.libraryAvailable = true;
    s.castName = "MiSTer Living Room";
    s.pmsLabel = "Home";
    s.pmsState = 1;
    return s;
}

static void testMenu() {
    {   // M1: the items from the daemon's state; the cursor rests on selectable rows only
        ObProgramMenu m;
        ObMenuState s = playingState();
        m.open(s);
        CHECK(labels(m) == "Blade Runner  1:02:03|Pause|Back 10 s|Forward 30 s|Stop|Library (stops the play)|"
                           "Server online|Close", "M1 playing: %s", labels(m).c_str());
        CHECK(m.selected() == 1, "M1 the cursor skips the now-playing row (%d)", m.selected());
        m.nav(ObNav::Up);
        CHECK(m.selected() == 7, "M1 Up from the first command wraps to Close, past the info row (%d)", m.selected());
        m.nav(ObNav::Down);
        CHECK(m.selected() == 1, "M1 Down from Close wraps past the now-playing row (%d)", m.selected());
        const owenb_osd::Menu v = m.view();
        CHECK(v.open && v.items.size() == 8 && v.dim.size() == 8 && v.dim[0] == 1 && v.dim[6] == 1 && v.dim[1] == 0,
              "M1 the view dims the info rows");
        ObMenuState idle;
        idle.castName = "MiSTer Living Room";
        idle.pmsLabel = "Home";
        idle.pmsState = 0;
        m.open(idle);
        CHECK(labels(m) == "MiSTer Living Room|Server offline|Close", "M1 idle, no library: %s",
              labels(m).c_str());
        idle.libraryAvailable = true;
        m.open(idle);
        CHECK(m.items().front().label == "Browse the library" && m.selected() == 0, "M1 idle with a library");
    }
    {   // M2: refresh follows the state; the cursor stays on its command
        ObInputRouter rt;
        ObMenuState s = playingState();
        bool ch = false;
        rt.handle(ObNav::Menu, s, ch);
        CHECK(ch && rt.menu().isOpen(), "M2 Menu opens the panel");
        rt.handle(ObNav::Down, s, ch);
        rt.handle(ObNav::Down, s, ch);   // Forward 30 s
        s.paused = true;
        s.positionMs = 3724000;
        CHECK(rt.refresh(s), "M2 a pause and a new position change the view");
        CHECK(rt.menu().items()[1].label == "Resume", "M2 Pause became Resume (%s)", rt.menu().items()[1].label.c_str());
        CHECK(rt.menu().items()[0].label.find("1:02:04") != std::string::npos, "M2 the position follows");
        CHECK(rt.menu().selected() == 3, "M2 the cursor stays on Forward (%d)", rt.menu().selected());
        CHECK(!rt.refresh(s), "M2 the same state: no change");
        s.playing = false;   // the play ended under the open panel
        rt.refresh(s);
        CHECK(rt.menu().items().front().label == "Browse the library", "M2 the play's end rebuilds the idle panel");
    }
    {   // M3: commands; the panel closes on one; Back / Menu close it
        ObInputRouter rt;
        ObMenuState s = playingState();
        bool ch = false;
        rt.handle(ObNav::Menu, s, ch);
        CHECK(rt.handle(ObNav::Select, s, ch) == PlaybackCommand::PlayPause && !rt.menu().isOpen(),
              "M3 Pause -> PlayPause, closed");
        rt.handle(ObNav::Menu, s, ch);
        for (int i = 0; i < 3; ++i)
            rt.handle(ObNav::Down, s, ch);
        CHECK(rt.handle(ObNav::Select, s, ch) == PlaybackCommand::Stop, "M3 Stop");
        rt.handle(ObNav::Menu, s, ch);
        rt.handle(ObNav::Up, s, ch);   // Close
        CHECK(rt.handle(ObNav::Select, s, ch) == PlaybackCommand::None && !rt.menu().isOpen(), "M3 Close");
        rt.handle(ObNav::Menu, s, ch);
        CHECK(rt.handle(ObNav::Back, s, ch) == PlaybackCommand::None && ch && !rt.menu().isOpen(), "M3 Back closes");
        rt.handle(ObNav::Menu, s, ch);
        rt.handle(ObNav::Up, s, ch);
        rt.handle(ObNav::Up, s, ch);   // Library
        CHECK(rt.handle(ObNav::Select, s, ch) == PlaybackCommand::Browse, "M3 Library -> Browse");
    }
}

static void testRoute() {
    ObMenuState s;
    auto cmd = [&](ObNav n) {
        const ObRoute r = obRoute(n, s);
        return r.kind == ObRouteKind::Command ? int(r.cmd) : r.kind == ObRouteKind::OpenMenu ? 100 : -1;
    };
    // idle, no library: only the menu
    CHECK(cmd(ObNav::Menu) == 100 && cmd(ObNav::Select) == -1 && cmd(ObNav::Home) == -1, "R1 idle, no library");
    s.libraryAvailable = true;
    CHECK(cmd(ObNav::Select) == int(PlaybackCommand::Browse) && cmd(ObNav::Home) == int(PlaybackCommand::Browse) &&
              cmd(ObNav::Down) == int(PlaybackCommand::Browse) && cmd(ObNav::Back) == -1,
          "R1 idle: Select / Home / Down open the library");
    s.libraryVisible = true;   // the browser's keys
    CHECK(cmd(ObNav::Up) == int(PlaybackCommand::Up) && cmd(ObNav::Down) == int(PlaybackCommand::Down) &&
              cmd(ObNav::PageDown) == int(PlaybackCommand::PageDown) && cmd(ObNav::Info) == int(PlaybackCommand::Info) &&
              cmd(ObNav::Select) == int(PlaybackCommand::PlayPause) && cmd(ObNav::Right) == int(PlaybackCommand::PlayPause) &&
              cmd(ObNav::Back) == int(PlaybackCommand::Back) && cmd(ObNav::Left) == int(PlaybackCommand::Back) &&
              cmd(ObNav::Home) == int(PlaybackCommand::Back) && cmd(ObNav::Menu) == 100,
          "R1 the library page: the browser's mapping");
    s.libraryLegacy = true;
    CHECK(cmd(ObNav::Up) == int(PlaybackCommand::SkipBack) && cmd(ObNav::Down) == int(PlaybackCommand::SkipForward) &&
              cmd(ObNav::Select) == int(PlaybackCommand::PlayPause) && cmd(ObNav::Back) == int(PlaybackCommand::Stop) &&
              cmd(ObNav::Info) == -1,
          "R1 the legacy list: its five commands");
    s.libraryLegacy = false;
    s.libraryVisible = false;
    s.playing = true;
    CHECK(cmd(ObNav::Select) == int(PlaybackCommand::PlayPause) && cmd(ObNav::Left) == int(PlaybackCommand::SkipBack) &&
              cmd(ObNav::Right) == int(PlaybackCommand::SkipForward) && cmd(ObNav::PlayPause) == int(PlaybackCommand::PlayPause),
          "R1 a play: transport");
    // a lab MiSTer bug 3: the RBF's J line puts its four transport buttons on pad bits 4-7 (the Plex cores'
    // "J1,Play/Pause,Stop,Skip Fwd,Skip Back", owenb's A,B,X,Y): while playing they are Play/Pause, Stop,
    // Skip Fwd, Skip Back, and Esc is Stop as on the Plex cores (docs/playback-input-mailbox.md:7-8).
    CHECK(cmd(ObNav::Back) == int(PlaybackCommand::Stop), "R2 a play: B / Esc (pad bit 5) -> Stop (%d)", cmd(ObNav::Back));
    CHECK(cmd(ObNav::Info) == int(PlaybackCommand::SkipForward), "R2 a play: X (pad bit 6) -> Skip Fwd (%d)",
          cmd(ObNav::Info));
    CHECK(cmd(ObNav::Aux) == int(PlaybackCommand::SkipBack), "R2 a play: Y (pad bit 7) -> Skip Back (%d)",
          cmd(ObNav::Aux));
    CHECK(cmd(ObNav::Home) == -1 && cmd(ObNav::Up) == -1, "R2 a play: Home / Up never stop it");
    s.playing = false;
    CHECK(cmd(ObNav::Back) == -1 && cmd(ObNav::Info) == -1, "R2 idle: Back / Info do nothing (no play to stop)");
    CHECK(cmd(ObNav::Aux) == 100, "R2 idle: Y opens the menu as before");
}

static void testLibrary() {
    std::vector<LibraryRow> rows;
    for (int i = 0; i < 300; ++i) {
        LibraryRow r;
        r.title = "Movie " + std::to_string(i);
        rows.push_back(r);
    }
    rows[0].mpxKind = "header";
    rows[1].directory = true;
    owenb_osd::LibraryPage p = obLibraryPage(true, rows, 100, "", "Plex / Movies");
    CHECK(p.open && p.total == 300 && p.rows.size() == size_t(kObLibraryWindow), "L1 a window of %d rows (%zu)",
          kObLibraryWindow, p.rows.size());
    CHECK(p.offset == 68 && p.cursor == 32 && p.rows[size_t(p.cursor)].text == "Movie 100",
          "L1 the window centres the cursor (offset %d cursor %d)", p.offset, p.cursor);
    p = obLibraryPage(true, rows, 299, "", "Plex");
    CHECK(p.offset == 236 && p.cursor == 63, "L1 the last row: the window ends there (offset %d cursor %d)", p.offset,
          p.cursor);
    p = obLibraryPage(true, rows, 1, "LOADING...", "Plex");
    CHECK(p.offset == 0 && p.cursor == 1 && !p.rows[0].selectable && p.rows[1].selectable && p.rows[1].dir &&
              p.status == "LOADING...", "L1 the top: a header row is dim, a directory marked");
    CHECK(!obLibraryPage(false, rows, 5, "", "Plex").open, "L1 a hidden browser: no page");
    p = obLibraryPage(true, {}, 0, "EMPTY", "Plex");
    CHECK(p.open && p.rows.empty() && p.total == 0 && p.status == "EMPTY", "L1 an empty level shows its status");
    LibraryBrowser br;
    br.show({rows.begin(), rows.begin() + 5});
    br.move(2);
    p = obLibraryPage(br, "Plex");
    CHECK(p.open && p.total == 5 && p.cursor == br.cursor(), "L1 the legacy browser (cursor %d)", p.cursor);
}

int main() {
    testDecoder();
    testMenu();
    testRoute();
    testLibrary();
    if (g_fail) {
        std::printf("test_owenb_input: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    std::printf("test_owenb_input: OK (%d checks)\n", g_checks);
    return 0;
}
