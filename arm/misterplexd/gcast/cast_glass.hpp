#pragma once
// Gate 2b. A new cast play tears the current stream down before resolve, and a
// stop tears it down even when the YouTube session has already dropped its
// video id. tests/unit/test_yt_skip_stop.cpp decides with this same function.

#include "gcast/session_arbiter.hpp"

namespace misterplex {
namespace gcast {

enum class GlassStep : uint8_t { NewPlay = 0, ResolveFailed = 1, Stop = 2 };

struct GlassAction {
    bool stopMedia;  // player.stop / retirePlayback (logs "media: stop")
    bool idle;       // glass must end on the idle chevron
};

// sessionLoaded is the Lounge video id. Stop does not consult it: stopVideo
// with an empty id still stops media that is actually on glass.
// currentOwner is the arbiter owner. A stop for `owner` leaves a different
// caster's play alone. playGenMatch is activePlayGen==castGen: a None owner
// (a library Plex play never claims Owner::Plex) stops only when the
// generations match. allowNewPlayTeardown is false for a YouTube Seek, which
// must not tear the current stream down to the chevron.
inline GlassAction castGlassAction(GlassStep step, bool mediaActive, bool sessionLoaded,
                                   Owner owner, Owner currentOwner,
                                   bool playGenMatch = true, bool allowNewPlayTeardown = true) {
    switch (step) {
    case GlassStep::NewPlay:
        (void)sessionLoaded;
        (void)owner;
        (void)currentOwner;
#ifndef MUT_SEEK_ALWAYS_TEARDOWN
        if (!allowNewPlayTeardown)
            return GlassAction{false, false};
#endif
        return GlassAction{mediaActive, mediaActive};
    case GlassStep::ResolveFailed:
        (void)sessionLoaded;
        (void)owner;
        (void)currentOwner;
        return GlassAction{mediaActive, true};
    case GlassStep::Stop:
        (void)sessionLoaded;
#ifdef MUT_CAST_STOP_ANY_NONE
        if (currentOwner == owner || (currentOwner == Owner::None && mediaActive))
            return GlassAction{true, true};
#else
        if (currentOwner == owner || (currentOwner == Owner::None && mediaActive && playGenMatch))
            return GlassAction{true, true};
#endif
        return GlassAction{false, false};
    }
    return GlassAction{false, false};
}

}  // namespace gcast
}  // namespace misterplex
