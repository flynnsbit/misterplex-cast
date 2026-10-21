// FIELD-1018 F4: FairPlay / Apple TV+ protected HLS can never play here (no FairPlay CDM). Without this the
// daemon handed the URL to gcast-xcode, ffprobe failed, and the play sat in "open ... Connection timed out"
// before dropping back with nothing on screen. Detect it up front and fail fast with an on-screen message.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace misterplex {
namespace gcast {

inline const char* kProtectedMsg = "Protected content - can't play";
inline const char* kUnplayableMsg = "Can't play this stream";

inline std::string psLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// URL alone: Apple's FairPlay storefront/keys hosts (Apple TV+, iTunes, Apple Music video) and skd:// keys.
inline bool protectedUrl(const std::string& url) {
    const std::string u = psLower(url);
#ifndef MUT_F4_NO_URL
    for (const char* k : {"skd://", "itunes.apple.com/", ".itunes.apple.com", "mzplaylocal.woa", "mzplay.woa",
                          "streamingkeydelivery", "play.itunes.apple.com", "tv.apple.com/", "hls-svod.itunes",
                          "fairplay"})
        if (u.find(k) != std::string::npos)
            return true;
#endif
    return false;
}

// Playlist text (master or media): an EXT-X-KEY / EXT-X-SESSION-KEY that is FairPlay (SAMPLE-AES with an Apple
// keyformat or an skd:// URI). Plain AES-128 (clear key over https) is playable by ffmpeg and is NOT protected.
inline bool protectedPlaylist(const std::string& text) {
    const std::string t = psLower(text);
    size_t p = 0;
    while ((p = t.find("#ext-x-", p)) != std::string::npos) {
        const size_t e = t.find('\n', p);
        const std::string line = t.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p += 7;
        if (line.rfind("#ext-x-key:", 0) != 0 && line.rfind("#ext-x-session-key:", 0) != 0)
            continue;
        if (line.find("com.apple.streamingkeydelivery") != std::string::npos || line.find("skd://") != std::string::npos)
            return true;
#ifndef MUT_F4_NO_SAMPLE_AES
        if (line.find("method=sample-aes") != std::string::npos)   // also SAMPLE-AES-CTR
            return true;
#endif
    }
    return false;
}

inline bool hlsUrl(const std::string& url) {
    const std::string u = psLower(url);
    return u.find(".m3u8") != std::string::npos || u.rfind("mlhls://", 0) == 0;
}

// Decision for an AirPlay URL: protected up front (no probe), or after a failed helper probe of an HLS source.
enum class CastSourceVerdict { Play, Protected, Unplayable };
inline CastSourceVerdict castSourceVerdict(const std::string& url, bool probed, bool probeOk,
                                           const std::string& playlistHead = std::string()) {
    if (protectedUrl(url) || protectedPlaylist(playlistHead))
        return CastSourceVerdict::Protected;
    if (probed && !probeOk && hlsUrl(url))
        return CastSourceVerdict::Unplayable;
    return CastSourceVerdict::Play;
}

}  // namespace gcast
}  // namespace misterplex
