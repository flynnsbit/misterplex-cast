#pragma once
// OSD Browse controls (issue #121).
//
// The decoder in owenb_input.hpp is unchanged. It is what a default MiSTer mapping already
// delivers on an OwenB core (ring PADn_JOY / PS/2 set-2), before this pin and after it:
//
//   Joystick bit   MiSTer default                         ObNav
//   0              D-pad Right                            Right
//   1              D-pad Left                             Left
//   2              D-pad Down                             Down
//   3              D-pad Up                               Up
//   4              J1 button 1 "Play/Pause" (A)           Select
//   5              J1 button 2 "Stop" (B)                 Back
//   6              J1 button 3 "Skip Fwd" (X)             Info
//   7              J1 button 4 "Skip Back" (Y)            Aux
//   8 / 9          LB / RB                                PageUp / PageDown (repeat)
//   10             Back                                   Home
//   11             Start                                  Menu
//
//   Key            ObNav
//   arrows         Up Down Left Right
//   PgUp / PgDn    PageUp / PageDown
//   Enter          Select
//   Home           Home
//   Esc, Backspace Back
//   Space          PlayPause
//   I              Info
//   M, Tab, F12    Menu
//
// Before this pin the list browser (obRoute, local PMS and remote helper rows alike) used
// Left as back and Right as select/play. The Plex UI already moved on Left/Right, except the
// server list and the episode list, which ignored them. Remote hosts show up on that server
// list. Browse now uses one scheme on every host: d-pad moves, A / Enter / Space selects or
// plays, B / Esc goes back. On the Plex TV browser, X (Info) adds the focused
// movie or episode to the PMS play queue. Program → Play next plays that title
// when nothing is playing, or inserts it next when a play is already up.
// While a queued play is up, RB and Program → Skip to next advance one item.

namespace misterplex {

inline const char* browseHintLine() {
#ifdef MUT_RC1037_NO_HINT
    return "";
#else
    return "D-pad move  A select/play  B back";
#endif
}

} // namespace misterplex
