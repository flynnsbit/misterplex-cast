#pragma once
// OB PlexUI ping-pong (producer side of MediaPlayer::obPlexUiPublish).
//
// The producer (browse shell) writes only the rows that changed since the last
// published frame. The OSD renderer blits whole frames from the surface on a
// replan / first show, and holds a shared_ptr to the surface it is drawing.
// So the buffer handed to the producer must:
//   1. never be one the OSD thread can still see (shared -> fresh buffer), and
//   2. already hold the last published frame, so dirty rows complete it
//      (a zero / two-frames-old back buffer was a black or stale flash).
// Review #1 (Copilot neon 1124).

#include "libmisterplex/owenb_osd_draw.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>

namespace misterplex::owenb_osd {

inline std::shared_ptr<FullSurface> obPingPongBack(std::shared_ptr<FullSurface> (&slot)[2], int front,
                                                   std::size_t bytes) {
    front &= 1;
    const std::shared_ptr<FullSurface>& cur = slot[front];
    std::shared_ptr<FullSurface> b;
#ifdef MUT_OB_PINGPONG_REUSE_SHARED
    b = slot[1 - front];
#else
    // use_count()==1: only the slot holds it. Once the OSD drops its copy it
    // can never get it back (UiModel only ever receives the newest frame).
    if (slot[1 - front] && slot[1 - front].use_count() == 1)
        b = slot[1 - front];
#endif
    if (!b)
        b = std::make_shared<FullSurface>();
#ifndef MUT_OB_PINGPONG_NO_SEED
    if (!cur || cur.get() == b.get() || cur->bgra.size() != bytes) {
        b->bgra.clear();   // no reference frame: the producer must write a full frame
        return b;
    }
    const std::size_t row = std::size_t(cur->w) * 4u;
    const bool rowsOnly = b->bgra.size() == bytes && b->gen + 1 == cur->gen && cur->dirtyN > 0 &&
                          row && row * cur->h == bytes;
    if (rowsOnly) {   // b holds cur's predecessor: copy just what cur changed
        for (unsigned i = 0; i < cur->dirtyN; ++i) {
            const unsigned y0 = std::min(cur->dirty[i].y0, cur->h);
            const unsigned y1 = std::min(cur->dirty[i].y1, cur->h);
            if (y0 < y1)
                std::memcpy(b->bgra.data() + y0 * row, cur->bgra.data() + y0 * row, (y1 - y0) * row);
        }
    } else {
        b->bgra.assign(cur->bgra.begin(), cur->bgra.end());
    }
#else
    (void)cur;
    (void)bytes;
#endif
    return b;
}

// Dirty bands for the renderer. More damage rects than bands: whole page
// (dirtyN 0), never a silently dropped band.
struct ObDamageRect { int y0 = 0, y1 = 0; };
inline void obPingPongSetDirty(FullSurface& s, bool full, const ObDamageRect* rects, int count) {
    s.dirtyN = 0;
    if (full || count <= 0)
        return;
#ifndef MUT_OB_PINGPONG_DROP_BANDS
    if (static_cast<std::size_t>(count) > s.dirty.size())
        return;
#endif
    for (int i = 0; i < count && s.dirtyN < s.dirty.size(); ++i) {
        const unsigned y0 = static_cast<unsigned>(std::max(0, rects[i].y0));
        const unsigned y1 = static_cast<unsigned>(std::min(int(s.h), rects[i].y1));
        if (y0 < y1)
            s.dirty[s.dirtyN++] = {y0, y1};
    }
}

}  // namespace misterplex::owenb_osd
