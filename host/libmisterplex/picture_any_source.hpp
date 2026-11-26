#pragma once
// Program-menu Picture modes (letterbox / 14:9 / zoom 4:3 / stretch / fit-crop)
// are Analog Aspect + Aspect Ratio on the shared OwenbRing scaler (AspectGuard),
// not a per-source filter. AirPlay video, screen mirroring, Plex cast, YouTube
// cast, and Spelunkr all feed that ring. The menu row is on whenever the ring
// is showing a play (OwenbPlaySession or an external claim).

namespace misterplex {

// ringActive: OwenbPlayer::active_ (Plex/YouTube session or claimExternalPlay).
// anyPlaying: MediaPlayer::playing_ (AirPlay mirror sets this without a session).
// mediaPlaySession: OwenbPlaySession running (false for AirPlay mirror).
inline bool pictureMenuOnGlass(bool ringActive, bool anyPlaying, bool mediaPlaySession) {
#ifdef MUT_PIC_ANY_MENU_PLEX_ONLY
    (void)anyPlaying;
    return ringActive && mediaPlaySession;
#else
    (void)mediaPlaySession;
    return ringActive || anyPlaying;
#endif
}

// Input-loop tick applies Analog Aspect for every source, including external
// claims that have no OwenbPlaySession thread.
inline bool picturePumpShared() {
#ifdef MUT_PIC_ANY_NO_PUMP
    return false;
#else
    return true;
#endif
}

} // namespace misterplex
