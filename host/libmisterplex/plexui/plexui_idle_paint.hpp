#pragma once

namespace misterplex {
namespace plexui {

// paintIdleV2 must not present the idle chevron/black field while Plex UI is visible but
// poster/backdrop art for the current view is still in flight (matches waitForFrame gating).
inline bool deferIdlePaintOverPlexUi(bool plexVisible, bool gateErrorEmpty, bool paintReady) {
    return plexVisible && gateErrorEmpty && !paintReady;
}

} // namespace plexui
} // namespace misterplex
