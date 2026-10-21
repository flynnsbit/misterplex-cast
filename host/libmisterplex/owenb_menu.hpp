#pragma once
// OWENB-P1B 0010 (T6 gap G2): the menu panel's items from the daemon's state, and where an OB input
// goes. The panel opens from the core's OSD action 1 "Program menu" (h264ring.h:390-394) or Menu
// (Start / Y / M / Tab, owenb_input.hpp). Its commands are the daemon's own transport and library
// commands (PlaybackCommand, input_mailbox.hpp:36-49), dispatched as the legacy input path does
// (MediaPlayer::dispatchPlaybackInput): nothing here touches playback by itself.
//
// Routing (obRoute), first match:
//   1. the panel is open: every navigation is the panel's (Back / Menu close it);
//   2. Menu: open the panel;
//   3. the library page is up: the browser's keys (Up/Down/PageUp/PageDown/Info, Select/Right/Space
//      = select, Back/Left/Home = back), as browseOpForVisibleCommand maps them (the legacy list:
//      SkipBack/SkipForward move, PlayPause opens, Stop pops -- media_player libraryHandleInput);
//   4. a play: Select/Space = play/pause, Left/Right = skip back/forward; Home never stops a play;
//   5. idle: Select/Down/Home open the library (when there is one).

#include "input_mailbox.hpp"
#include "owenb_input.hpp"
#include "owenb_osd_draw.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace misterplex::owenb {

struct ObMenuState {
    bool playing = false, paused = false;
    bool libraryAvailable = false;   // a browser is configured (MPX_BROWSE_V2 or the PMS list)
    bool libraryVisible = false;
    bool libraryLegacy = false;      // the pre-V2 PMS list: commands 1-5 only (library_browser.hpp; media_player
                                     // libraryHandleInput: PlayPause opens, Stop pops, SkipBack/Forward move)
    std::string title;               // the play's title
    int64_t positionMs = -1, durationMs = 0;
    std::string castName;            // the companion's name (what Plex apps show)
    std::string pmsLabel;
    int pmsState = -1;               // 1 reachable, 0 unreachable, -1 not probed
    int skipBackS = 10, skipForwardS = 30;
};

enum class ObMenuCmd : uint8_t { None, PlayPause, Stop, SkipBack, SkipForward, Library, Close };

struct ObMenuItem {
    std::string label;
    ObMenuCmd cmd = ObMenuCmd::None;
    bool selectable = true;
};

inline std::string obClock(int64_t ms) {
    if (ms < 0)
        ms = 0;
    const int64_t s = ms / 1000, h = s / 3600, m = s / 60 % 60, ss = s % 60;
    char b[32];
    if (h)
        std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)ss);
    else
        std::snprintf(b, sizeof b, "%lld:%02lld", (long long)m, (long long)ss);
    return b;
}

inline std::vector<ObMenuItem> obMenuItems(const ObMenuState& s) {
    std::vector<ObMenuItem> v;
    if (s.playing) {
        std::string now = s.title.empty() ? "Playing" : s.title;
        if (s.positionMs >= 0)   // the position only: the bar under the panel has the duration
            now += "  " + obClock(s.positionMs);
        v.push_back({now, ObMenuCmd::None, false});
        v.push_back({s.paused ? "Resume" : "Pause", ObMenuCmd::PlayPause, true});
        v.push_back({"Back " + std::to_string(s.skipBackS) + " s", ObMenuCmd::SkipBack, true});
        v.push_back({"Forward " + std::to_string(s.skipForwardS) + " s", ObMenuCmd::SkipForward, true});
        v.push_back({"Stop", ObMenuCmd::Stop, true});
        if (s.libraryAvailable)
            v.push_back({"Library (stops the play)", ObMenuCmd::Library, true});
    } else {
        if (s.libraryAvailable)
            v.push_back({"Browse the library", ObMenuCmd::Library, true});
        if (!s.castName.empty())   // the name Plex apps cast to
            v.push_back({s.castName, ObMenuCmd::None, false});
    }
    if (s.pmsState == 1 || s.pmsState == 0)   // the main loop's PMS probe (the server's name is on its error screen)
        v.push_back({s.pmsState == 1 ? "Server online" : "Server offline", ObMenuCmd::None, false});
    v.push_back({"Close", ObMenuCmd::Close, true});
    return v;
}

// The panel: items rebuilt from the state on every refresh (a pause, the position, the server),
// the cursor kept on the same command; it rests only on selectable rows.
class ObProgramMenu {
public:
    bool isOpen() const { return open_; }
    void open(const ObMenuState& s) {
        open_ = true;
        items_ = obMenuItems(s);
        sel_ = -1;
        moveTo(0, 1);
    }
    void close() { open_ = false; }
    void refresh(const ObMenuState& s) {
        if (!open_)
            return;
#ifdef MUT_MENU_STALE
        (void)s;   // mutant: the panel keeps the items it opened with (Pause after a pause)
#else
        const ObMenuCmd keep = sel_ >= 0 && sel_ < int(items_.size()) ? items_[size_t(sel_)].cmd : ObMenuCmd::None;
        items_ = obMenuItems(s);
        sel_ = -1;
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].selectable && items_[i].cmd == keep)
                sel_ = int(i);
        if (sel_ < 0)
            moveTo(0, 1);
#endif
    }
    // a navigation while open: the command chosen (the panel closes), or None
    ObMenuCmd nav(ObNav n) {
        if (!open_)
            return ObMenuCmd::None;
        switch (n) {
        case ObNav::Up:
        case ObNav::Left:
            moveTo(sel_ - 1, -1);
            return ObMenuCmd::None;
        case ObNav::Down:
        case ObNav::Right:
            moveTo(sel_ + 1, 1);
            return ObMenuCmd::None;
        case ObNav::Select:
        case ObNav::PlayPause: {
            const ObMenuCmd c = sel_ >= 0 && sel_ < int(items_.size()) ? items_[size_t(sel_)].cmd : ObMenuCmd::None;
            if (c != ObMenuCmd::None)
                open_ = false;
            return c;
        }
        case ObNav::Back:
        case ObNav::Menu:
        case ObNav::Aux:
        case ObNav::Home:
            open_ = false;
            return ObMenuCmd::Close;
        default:
            return ObMenuCmd::None;
        }
    }
    owenb_osd::Menu view() const {
        owenb_osd::Menu m;
        m.open = open_;
        m.title = "MiSTerPlex";
        for (const ObMenuItem& it : items_) {
            m.items.push_back(it.label);
            m.dim.push_back(it.selectable ? 0 : 1);
        }
        m.selected = sel_;
        return m;
    }
    const std::vector<ObMenuItem>& items() const { return items_; }
    int selected() const { return sel_; }

private:
    void moveTo(int from, int dir) {   // the next selectable row from `from` in `dir`, wrapping
        const int n = int(items_.size());
        if (n == 0)
            return;
        int i = ((from % n) + n) % n;
        for (int k = 0; k < n; ++k, i = (i + dir + n) % n)
            if (items_[size_t(i)].selectable) {
                sel_ = i;
                return;
            }
    }
    bool open_ = false;
    std::vector<ObMenuItem> items_;
    int sel_ = -1;
};

inline PlaybackCommand obMenuCommand(ObMenuCmd c) {
    switch (c) {
    case ObMenuCmd::PlayPause: return PlaybackCommand::PlayPause;
    case ObMenuCmd::Stop: return PlaybackCommand::Stop;
    case ObMenuCmd::SkipBack: return PlaybackCommand::SkipBack;
    case ObMenuCmd::SkipForward: return PlaybackCommand::SkipForward;
    case ObMenuCmd::Library: return PlaybackCommand::Browse;
    default: return PlaybackCommand::None;
    }
}

// Where a navigation goes when the panel is not open (rule 1 is the caller's: ObProgramMenu::nav).
enum class ObRouteKind : uint8_t { None, OpenMenu, Command };
struct ObRoute {
    ObRouteKind kind = ObRouteKind::None;
    PlaybackCommand cmd = PlaybackCommand::None;
};

inline ObRoute obRoute(ObNav n, const ObMenuState& s) {
    auto cmd = [](PlaybackCommand c) {
        ObRoute r;
        r.kind = ObRouteKind::Command;
        r.cmd = c;
        return r;
    };
    if (n == ObNav::Menu || (n == ObNav::Aux && !s.playing)) {   // Aux: pad Y, the menu off a play
        ObRoute r;
        r.kind = ObRouteKind::OpenMenu;
        return r;
    }
    if (s.libraryVisible && s.libraryLegacy) {
        switch (n) {
        case ObNav::Up:
        case ObNav::PageUp: return cmd(PlaybackCommand::SkipBack);
        case ObNav::Down:
        case ObNav::PageDown: return cmd(PlaybackCommand::SkipForward);
        case ObNav::Select:
        case ObNav::Right:
        case ObNav::PlayPause: return cmd(PlaybackCommand::PlayPause);
        case ObNav::Back:
        case ObNav::Left:
        case ObNav::Home: return cmd(PlaybackCommand::Stop);
        default: return {};
        }
    }
    if (s.libraryVisible) {   // the browser's own mapping (browse_controller.hpp browseOpForVisibleCommand)
        switch (n) {
        case ObNav::Up: return cmd(PlaybackCommand::Up);
        case ObNav::Down: return cmd(PlaybackCommand::Down);
        case ObNav::PageUp: return cmd(PlaybackCommand::PageUp);
        case ObNav::PageDown: return cmd(PlaybackCommand::PageDown);
        case ObNav::Info: return cmd(PlaybackCommand::Info);
        case ObNav::Select:
        case ObNav::Right:
        case ObNav::PlayPause: return cmd(PlaybackCommand::PlayPause);
        case ObNav::Back:
        case ObNav::Left:
        case ObNav::Home: return cmd(PlaybackCommand::Back);
        default: return {};
        }
    }
    if (s.playing) {
        switch (n) {
        case ObNav::Select:
        case ObNav::PlayPause: return cmd(PlaybackCommand::PlayPause);
        case ObNav::Left: return cmd(PlaybackCommand::SkipBack);
        case ObNav::Right: return cmd(PlaybackCommand::SkipForward);
#ifndef MUT_ROUTE_PLAY_NO_TRANSPORT
        // v2-bugfix (a lab MiSTer bug 3): the RBF's J line puts its transport on pad bits 4-7 (the Plex cores'
        // "J1,Play/Pause,Stop,Skip Fwd,Skip Back"; owenb's A,B,X,Y): B / Esc Stop, X Skip Fwd, Y Skip Back.
        // Home and Up still never stop a play.
        case ObNav::Back: return cmd(PlaybackCommand::Stop);
        case ObNav::Info: return cmd(PlaybackCommand::SkipForward);
        case ObNav::Aux: return cmd(PlaybackCommand::SkipBack);
#endif
#ifdef MUT_ROUTE_HOME_STOPS_PLAY
        case ObNav::Home: return cmd(PlaybackCommand::Browse);   // mutant: Home throws the play away
#endif
        default: return {};   // Home, Back, Up/Down: never stop a play by accident (the menu has Stop)
        }
    }
    if (s.libraryAvailable && (n == ObNav::Select || n == ObNav::Down || n == ObNav::Home))
        return cmd(PlaybackCommand::Browse);
    return {};
}

// The panel and the routing together: one navigation -> the command to dispatch (None: nothing),
// menuChanged when the panel's view must be published again (UiBus::setMenu).
class ObInputRouter {
public:
    PlaybackCommand handle(ObNav n, const ObMenuState& s, bool& menuChanged) {
        menuChanged = false;
        if (menu_.isOpen()) {
            const ObMenuCmd c = menu_.nav(n);
            menuChanged = true;
            return obMenuCommand(c);
        }
        const ObRoute r = obRoute(n, s);
        if (r.kind == ObRouteKind::OpenMenu) {
            menu_.open(s);
            menuChanged = true;
            return PlaybackCommand::None;
        }
        return r.kind == ObRouteKind::Command ? r.cmd : PlaybackCommand::None;
    }
    // each tick while the panel is open: true when its view changed (the position, a pause, the server)
    bool refresh(const ObMenuState& s) {
        if (!menu_.isOpen())
            return false;
        const std::vector<ObMenuItem> before = menu_.items();
        const int sel = menu_.selected();
        menu_.refresh(s);
        if (sel != menu_.selected() || before.size() != menu_.items().size())
            return true;
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i].label != menu_.items()[i].label)
                return true;
        return false;
    }
    void close() { menu_.close(); }
    const ObProgramMenu& menu() const { return menu_; }

private:
    ObProgramMenu menu_;
};

} // namespace misterplex::owenb
