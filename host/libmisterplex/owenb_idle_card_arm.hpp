// The "daemon not running" painter's arming step, shared by tools/owenb_idle_card and its host test.
// ROOT CAUSE of the PR #30 flat-black card (glass 10-23 21:16): the painter opened the ring and began the idle session
// but never gave its OwenbPlayer an OsdConfig (mapFb), so startOsdLocked() returned silently: no fb0 mapping, no
// renderer thread, no overlay page ever shown. The core's idle output (black) was all the HDMI had. The daemon's
// MediaPlayer::ensureOwenbPlayerLocked does setOsd(owenbOsdConfigFromEnv()); the painter did not. armCard() therefore
// refuses to report "armed" until the overlay renderer is running.
#pragma once
#include <string>

#include "idle_pairing_error.hpp"
#include "owenb_play.hpp"

namespace misterplex::owenb_idle_card {

// The status line under the headline (the headline is ErrorScreen::DaemonDown: "MISTERPLEX DAEMON NOT RUNNING").
inline constexpr char kStatusText[] = "Casting is off until the daemon starts. This card clears by itself.";

// One attempt, with p.setOsd(...) already done: the ring + idle session, the overlay renderer, then the card.
// False (err says why) when the ring/session is not up, or when the overlay renderer is not running.
inline bool armCard(owenb::OwenbPlayer& p, const owenb::IdleConfig& cfg, int64_t nowMs, std::string& err,
                    const std::string& status = kStatusText) {
    if (!p.startIdle(cfg, err))
        return false;
#ifndef MUT_CARD_NO_OSD_CHECK
    if (!p.osdRunning()) {
        err = "overlay renderer not running (no OsdConfig / fb0 not mapped): the card would be black";
        return false;
    }
#endif
    if (!p.ui().snapshot().spelunkr)
        p.ui().setErrorFor(nowMs, owenb_osd::ErrorScreen::DaemonDown, status, 0);   // until cleared
    return true;
}

} // namespace misterplex::owenb_idle_card
