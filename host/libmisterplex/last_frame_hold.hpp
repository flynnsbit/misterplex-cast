#pragma once
// IDLE-BLACK (2026-10-18): when the end of a session may skip the idle repaint (last-frame hold v3, c620).
// The hold keeps the last decoded AU on screen by not painting over the frame store. A core that scans out the
// host idle FB once the session ENDs (R1 hostIdleFb, armed by r1IdleFbReady) shows the idle buffer, not the
// last AU, so on that core the hold left HDMI black after every stop or end (prod 4fdf97e0, which predates the
// hold, shows the chevron). There the idle paint runs; paintIdleR1 still keeps the picture for
// IDLE_SCREEN=last-frame.

namespace misterplex {
namespace last_frame_hold {

inline bool holdAtEnd(bool holdEnabled, bool r1IdleFbArmed) {
#ifdef MUT_LFH_HOLD_ON_R1
    (void)r1IdleFbArmed;
    return holdEnabled;
#else
    return holdEnabled && !r1IdleFbArmed;
#endif
}

}  // namespace last_frame_hold
}  // namespace misterplex
