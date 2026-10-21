#pragma once

namespace misterplex {

// 177 L1 exited on SIGTERM during the Utility reload, and :3005 stayed closed.
// 178 L1 ignored that TERM and reached playing, then the port was gone before
// L2 because an idle TERM still exited. Once the companion is listening, every
// SIGTERM is ignored, including the gap between legs. stopd still has SIGKILL.
// A TERM before listen (companionUp false) still exits.
inline bool daemonTermShouldExit(bool companionUp, bool /*alreadyHeld*/) {
    return !companionUp;
}

// 179 L1: an HTTP stop about 2s after playMedia retired the generation while the
// WAN resolve was still running, so the play returned "superseded during resolve".
// Hold that stop until the player is actually playing. The harness end-stop
// arrives after playing() and still retires the generation.
inline bool stopShouldWaitForResolve(bool resolveInFlight, bool playing) {
    return resolveInFlight && !playing;
}

} // namespace misterplex
