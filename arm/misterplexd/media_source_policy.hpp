// Play-source allowlist for the daemon.
//
// The daemon plays only real media: an http(s) URL or an absolute local path. There is no built-in
// test clip, no synthetic generator source and no "unresolved -> test pattern" fallback; idle on glass
// is the Ready / daemon-not-running card and nothing else. Anything that is not one of the two shapes
// below (plus the YouTube two-URL form, see ytAvSourceAllowed) is refused before a backend is chosen.
#pragma once

#include <string>

#include "gcast/dash_seek.hpp"

namespace misterplex {

// One http(s) URL with a host and no control/space characters (a split DASH part must be exactly one).
inline bool plainHttpUrl(const std::string& u) {
    if (!(u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0))
        return false;
    if (u.size() <= 8)
        return false;
#ifndef MUT_YT_LOOSE_URL
    for (unsigned char c : u)
        if (c <= ' ' || c == 0x7f)
            return false;
#endif
    return true;
}

// The YouTube local-yt-dlp route hands the player "yt-av\n<videoUrl>\n<audioUrl>" (gcast::ytAvPlayable).
// It is accepted only when it parses and BOTH parts are plain http(s) URLs, so the prefix cannot smuggle a
// synthetic source, a relative path, file:// or pipe: past the allowlist.
inline bool ytAvSourceAllowed(const std::string& src) {
#if defined(MUT_YT_ANY_PREFIX)
    return true;
#else
    std::string video, audio;
    if (!gcast::splitYtAv(src, video, audio))
        return false;
#if defined(MUT_YT_VIDEO_ONLY)
    return plainHttpUrl(video);
#elif defined(MUT_YT_ANY_PARTS)
    return !video.empty() && !audio.empty();
#else
    return plainHttpUrl(video) && plainHttpUrl(audio);
#endif
#endif
}

inline bool playableSourceAllowed(const std::string& src) {
#ifndef MUT_YT_NO_YTAV
    if (src.rfind("yt-av\n", 0) == 0)
        return ytAvSourceAllowed(src);
#endif
    if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0)
        return src.size() > 8;
    return src.size() > 1 && src[0] == '/';
}

} // namespace misterplex
