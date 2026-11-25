#pragma once
// MENU→PlexOB reopen. A real core loss (gate left owenbOpen: MENU / not Plex /
// reprogramming) wipes the framebuffer whether or not an idle session was
// running. A play in progress never closes an idle session, so wiping only on
// that close left hadPicture / presented / spelunkrHasPicture_ describing the
// previous overlay. owenbOpened then skipped and HDMI stayed black (glass
// ec5e28e6 and 08ca64e6 reopen2 / reopen3 / reopen5). GH#648 still skips
// landing on a same-core re-verify, where those flags describe a picture that
// is still on this core. A plexui GH#27 flicker is that re-verify: the idle
// session is not closed, and the picture must not be wiped.
//
// MUT_OWENB_REOPEN_KEEPS_STALE_PICTURE restores today's "sticky picture survives
// the core loss, so owenbOpened skips landing". The reopen cases must fail.

#include "owenb_osd_renderer.hpp"

namespace misterplex::owenb_osd {

enum class OwenbOpenPaint {
    SkipLivePicture,    // GH#648: a live picture or mid-tune chrome is still on this core
    ShowLanding,        // Open the Guide / idle card on the new overlay
    PaintResumeChrome,  // last-channel resume already armed tuning chrome; paint it
};

struct OwenbReopenSticky {
    bool hadPicture = false;
    bool presented = false;
    int64_t bannerGoneMs = 0;
    bool hasPicture = false;
    bool pending = false;
    bool spelunkrTuning = false;
    bool deferResume = false;
    bool playing = false;
};

// True when the gate has left PlexOB for a real core change (MENU / not Plex /
// reprogramming). Idle may or may not have been running; the caller wipes either
// way. A plexui-visible gate flicker is GH#27 (same core, session not closed).
inline bool shouldWipePictureOnCoreLoss(bool gateOwenbOpen, bool plexUiVisible) {
    if (gateOwenbOpen || plexUiVisible)
        return false;
    return true;
}

// showSpelunkrError must not treat the previous framebuffer as still on screen
// once the core is gone or a reopen overlay is owed. Paint the landing on the
// next overlay instead of "picture already shown".
inline bool spelunkrErrorLandingSkipped(bool hadPicture, bool presented, bool coreOpen, bool coreLost,
                                       bool reopenNeeded) {
#ifdef MUT_OWENB_REOPEN_KEEPS_STALE_PICTURE
    (void)coreOpen;
    (void)coreLost;
    (void)reopenNeeded;
    return hadPicture || presented;
#else
    if (!coreOpen || coreLost || reopenNeeded)
        return false;
    return hadPicture || presented;
#endif
}

// (a) Call only when the PlexOB core was actually lost. Do not call on a
// same-core gate re-verify (GH#27 flicker / owenbOpened while the core never left).
// Clears the on-screen picture and the tune chrome that described that framebuffer.
// A fresh resumeChromeToArm after this wipe re-arms pending on the next tick.
inline void wipeOnScreenPictureAfterCoreLost(UiModel& m, bool* hasPicture = nullptr) {
#ifndef MUT_OWENB_REOPEN_KEEPS_STALE_PICTURE
    m.hadPicture = false;
    m.presented = false;
    m.pending = false;
    m.pendingGen = 0;
    m.pendingLabel.clear();
    m.clearSpelunkrBannerFields();
    if (hasPicture)
        *hasPicture = false;
#else
    (void)m;
    (void)hasPicture;
#endif
}

inline OwenbReopenSticky reopenStickyFrom(const UiModel& m, bool hasPicture, bool deferResume,
                                          bool playing) {
    OwenbReopenSticky s;
    s.hadPicture = m.hadPicture;
    s.presented = m.presented;
    s.bannerGoneMs = m.bannerGoneMs;
    s.hasPicture = hasPicture;
    s.pending = m.pending;
    s.spelunkrTuning = m.spelunkrTuning;
    s.deferResume = deferResume;
    s.playing = playing;
    return s;
}

// (b) First owenbOpened after the gate reports PlexOB.
// coreReloaded is true only after wipeOnScreenPictureAfterCoreLost on a real loss.
inline OwenbOpenPaint owenbOpenedPaint(const OwenbReopenSticky& s, bool coreReloaded) {
#ifdef MUT_OWENB_REOPEN_KEEPS_STALE_PICTURE
    coreReloaded = false;
#endif
    if (!coreReloaded) {
        const bool keep = s.pending || s.spelunkrTuning || s.bannerGoneMs > 0 || s.presented ||
                          s.hadPicture || s.hasPicture || s.deferResume;
        return keep ? OwenbOpenPaint::SkipLivePicture : OwenbOpenPaint::ShowLanding;
    }
    // The new overlay is blank. Stale picture flags do not describe it.
    // A resume that armed pending+tuning (resumeChromeToArm / beginSpelunkrTune)
    // has to be painted; anything else is the Guide landing, not a black wait.
    if (s.pending && s.spelunkrTuning)
        return OwenbOpenPaint::PaintResumeChrome;
    return OwenbOpenPaint::ShowLanding;
}

// Idle start on the tick after a real reload must open the overlay even when
// pending or a stale picture flag is set, so the landing or the resume chrome
// hits the new framebuffer. Same-core GH#648 still blocks on a live picture
// or a fresh pending tune.
inline bool owenbReopenIdleBlocked(const OwenbReopenSticky& s, bool coreReloaded, bool pendingBusy) {
#ifdef MUT_OWENB_REOPEN_KEEPS_STALE_PICTURE
    coreReloaded = false;
#endif
    if (coreReloaded && !s.playing)
        return false;
    return s.playing || s.hasPicture || pendingBusy || s.presented || s.hadPicture;
}

}  // namespace misterplex::owenb_osd
