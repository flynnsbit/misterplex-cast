#pragma once
#include "airplay/ns.hpp"
// AirPlay start keeps the last Plex frame: policy + RAII guard (no I/O, host-testable).
// Before: a new AirPlay cast stopped the Plex play at once (retirePlayback -> idle card / black) and then
// waited seconds for resolve + probe; a mirror began a new ring session at MirrorStart (the core's display
// resets there) and sat black until the first IDR. Now the Plex play is only PAUSED (picture held), and the
// stop happens at the real handoff: right before the AirPlay play opens (castPlay) or when the first mirror
// AU arrives (beginAirplayMirrorSession, already lazy). Every failure path still stops it (idle card).
// Limits: the core resets its display at the new session's begin(); the gap from begin to the first decoded
// frame is the decoder's and is not held by this code.
#include "gcast/session_arbiter.hpp"

#include <functional>
#include <utility>

namespace misterplex {
namespace airplay {

// URL cast: hold the Plex picture (pause) instead of stopping it? AirPlay only; a seek never tears down.
inline bool holdPlexForHandoff(Owner owner, bool mediaActive, bool isSeek) {
#ifdef MUT_HH_NEVER
    (void)owner; (void)mediaActive; (void)isSeek;
    return false;
#else
#ifdef MUT_HH_ANY_OWNER
    const bool ownerOk = true;
#else
    const bool ownerOk = owner == Owner::AirPlay;
#endif
#ifdef MUT_HH_ON_SEEK
    return ownerOk && mediaActive;
#else
    return ownerOk && mediaActive && !isSeek;
#endif
#endif
}

enum class MirrorStartAction { BeginNow, HoldPlex };
// Mirror: with a Plex play on glass hold it; the ring session begins with the first AU. Idle: BeginNow keeps
// the old eager begin only when there is nothing to hold, and the first AU path begins otherwise.
inline MirrorStartAction mirrorStartAction(bool plexActive) {
#ifdef MUT_HH_MIRROR_EAGER
    (void)plexActive;
    return MirrorStartAction::BeginNow;
#else
    return plexActive ? MirrorStartAction::HoldPlex : MirrorStartAction::BeginNow;
#endif
}

// MirrorStop: a held Plex play whose mirror never produced a frame must be stopped (idle card), or it stays
// paused forever. After the first AU began the session the Plex play is already gone.
inline bool mirrorStopNeedsPlexStop(bool held, bool mirrorBegan) {
#ifdef MUT_HH_MIRROR_NO_STOP
    (void)held; (void)mirrorBegan;
    return false;
#else
    return held && !mirrorBegan;
#endif
}

// While a held Plex play waits for the AirPlay stream, a sender Rate=1 must not resume it.
inline bool rateEventAllowed(bool handoffHeld) {
#ifdef MUT_HH_RATE_RESUMES
    (void)handoffHeld;
    return true;
#else
    return !handoffHeld;
#endif
}

// RAII: arm() after pausing; commit() at the real handoff; any other exit runs abort (stop + idle).
class HandoffHold {
public:
    explicit HandoffHold(std::function<void()> abort) : abort_(std::move(abort)) {}
    HandoffHold(const HandoffHold&) = delete;
    HandoffHold& operator=(const HandoffHold&) = delete;
    ~HandoffHold() {
#ifdef MUT_HH_NO_ABORT
        return;
#else
        if (held_) {
            held_ = false;
            if (abort_)
                abort_();
        }
#endif
    }
    void arm() { held_ = true; }
    void commit() {
#ifndef MUT_HH_NO_COMMIT
        held_ = false;
#endif
    }
    bool held() const { return held_; }

private:
    std::function<void()> abort_;
    bool held_ = false;
};

} // namespace airplay
} // namespace misterplex
