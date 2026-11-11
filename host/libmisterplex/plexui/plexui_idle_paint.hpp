#pragma once

namespace misterplex {
namespace plexui {

// paintIdleV2 must not present the idle chevron/black field while Plex UI is visible but
// poster/backdrop art for the current view is still in flight (matches waitForFrame gating).
inline bool deferIdlePaintOverPlexUi(bool plexVisible, bool gateErrorEmpty, bool paintReady) {
    return plexVisible && gateErrorEmpty && !paintReady;
}

// Never present the Ready chevron while Plex UI is visible (GH#14). copyFrame
// may fail after paintReady, and the 720p idle-scale path would cover browse.
inline bool skipIdleChevronOverPlexUi(bool plexVisible, bool paintPlex) {
#ifdef MUT_PLEXUI_IDLE_OVERLAY
    (void)plexVisible;
    (void)paintPlex;
    return false;
#else
    return plexVisible && !paintPlex;
#endif
}

// OSD Ready card (UiModel.idle) must drop while Plex UI is up, even before
// copyFrame supplies a FullSurface (GH#14 residual: Ready over browse).
inline bool skipOsdIdleOverPlexUi(bool plexVisible) {
#ifdef MUT_PLEXUI_IDLE_OVERLAY
    (void)plexVisible;
    return false;
#else
    return plexVisible;
#endif
}

} // namespace plexui
} // namespace misterplex
