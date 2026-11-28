#pragma once
// SPELUNKR=1 TV: which PlaybackCommands MediaPlayer::dispatchPlaybackInput hands to the
// Spelunkr frontend (spelunkrCmd) instead of swallowing. Pure so a unit test can pin it
// (combined-fleets-1114 glass G5: with the guide open, Left/Right reached the daemon as
// cmd 8/9 PageUp/PageDown but were dropped here because the allow-list lacked them).

#include "libmisterplex/input_mailbox.hpp"

namespace misterplex {
namespace spelunkr {

inline bool spelunkrTvForwardsToFrontend(PlaybackCommand c, bool guideOpen) {
#ifdef MUT_SPELUNKR_TV_FWD_PAGE_OFF
    (void)guideOpen;
#endif
    switch (c) {
    case PlaybackCommand::ChannelUp:
    case PlaybackCommand::ChannelDown:
    case PlaybackCommand::GuideToggle:
    case PlaybackCommand::GuideTune:
    case PlaybackCommand::GuideClose:
    case PlaybackCommand::Stop:
    case PlaybackCommand::Browse:
    case PlaybackCommand::PlayPause:
    case PlaybackCommand::SkipForward:
    case PlaybackCommand::SkipBack:
    case PlaybackCommand::Info:
        return true;
    case PlaybackCommand::PageUp:
    case PlaybackCommand::PageDown:
        // #655: Left/Right page the open Streaming Guide. Closed guide: stay inert.
#ifdef MUT_SPELUNKR_TV_FWD_PAGE_OFF
        return false;
#else
        return guideOpen;
#endif
    default:
        return false;
    }
}

}  // namespace spelunkr
}  // namespace misterplex
