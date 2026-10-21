#pragma once
// OB port: ring_in navigation -> PlexUI. On a PlexOB core the pads / keys arrive through the ring's
// input region (owenb_input.hpp ObInputDecoder -> ObNav), never through PLXI. While PlexUI is
// visible each ObNav maps to the op the R1 PLXI path gives PlexUI (media_player
// libraryHandleInputV2: cmd 5 Browse -> Open, 6/7 Up/Down, 8/9 PageUp/PageDown, 10 Back,
// 11 Info, 1 PlayPause -> Select, 3/4 SkipForward/Back -> Right/Left): a 2D UI, so Left / Right
// move focus instead of the list browser's Back / Select (owenb_menu.hpp obRoute). Unchanged:
// Menu and Aux still open the Program panel, an open panel takes every key, a hidden UI keeps
// obRoute's rules (idle Select / Down / Home open it; Home never stops a play).
#include "browse_controller.hpp"
#include "owenb_input.hpp"

namespace misterplex::owenb {

struct PlexUiObOp {
    bool take = false;           // false: obRoute / the panel handle it as before
    bool horizontal = false;     // true: postHorizontal(delta)
    int delta = 0;
    BrowseOp op = BrowseOp::Open;
};

inline PlexUiObOp plexUiObNav(ObNav n, bool uiVisible, bool menuOpen) {
    PlexUiObOp r;
    if (!uiVisible || menuOpen || n == ObNav::Menu || n == ObNav::Aux)
        return r;
    r.take = true;
    switch (n) {
    case ObNav::Up: r.op = BrowseOp::Up; break;
    case ObNav::Down: r.op = BrowseOp::Down; break;
    case ObNav::Left: r.horizontal = true; r.delta = -1; break;
    case ObNav::Right: r.horizontal = true; r.delta = 1; break;
    case ObNav::Select:
    case ObNav::PlayPause: r.op = BrowseOp::Select; break;
    case ObNav::Back: r.op = BrowseOp::Back; break;
#ifdef MUT_PLEXUI_OB_HOME_BACK
    case ObNav::Home: r.op = BrowseOp::Back; break;   // mutant: Home pops a level instead of Home
#else
    case ObNav::Home: r.op = BrowseOp::Open; break;   // PLXI 5 Browse: Plex Home
#endif
    case ObNav::PageUp: r.op = BrowseOp::PageUp; break;
    case ObNav::PageDown: r.op = BrowseOp::PageDown; break;
    case ObNav::Info: r.op = BrowseOp::Info; break;
    default: r.take = false; break;
    }
    return r;
}

} // namespace misterplex::owenb
