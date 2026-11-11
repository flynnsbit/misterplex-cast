#pragma once

namespace misterplex::owenb {

// Overlay status can report tiny rasters (256x144) while Plex UI is up; re-arming
// the idle card then covers browse with Ready (GH#14). Ignore those.
inline bool idleRasterCredible(unsigned w, unsigned h) {
#ifdef MUT_IDLE_REARM_TINY
    (void)w;
    (void)h;
    return true;
#else
    return w >= 320 && h >= 240;
#endif
}

} // namespace misterplex::owenb
