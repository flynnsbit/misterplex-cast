#pragma once
// Standalone yt-dlp on the card plays through libav. A split DASH pair is two
// URLs (video, then audio). Both are opened whole. The start position is a
// time seek (avformat_seek_file) on each opened input, never a raw byte
// offset glued onto the audio itag. A byte offset into itag 140 is not a
// container, which is the failure lab hit on the lab-pc helper.

#include <cstdint>
#include <string>

#if defined(MPX_HAVE_LIBAV)
extern "C" {
#include <libavformat/avformat.h>
}
#endif

namespace misterplex {
namespace gcast {

struct DashOpen {
    std::string videoUrl;
    std::string audioUrl;
    int64_t startMs = 0;
};

struct DashSeekPlan {
    std::string videoUrl;
    std::string audioUrl;
    int64_t seekTs = 0;   // AV_TIME_BASE ticks (microseconds)
    bool byteSeek = false;
};

inline DashSeekPlan planDashSeek(const DashOpen& in) {
    DashSeekPlan p;
    p.videoUrl = in.videoUrl;
    p.audioUrl = in.audioUrl;
    p.seekTs = in.startMs > 0 ? in.startMs * 1000 : 0;
    p.byteSeek = false;
    return p;
}

// playable for a resolved video URL plus a separate audio URL.
inline std::string ytAvPlayable(const std::string& video, const std::string& audio) {
    return std::string("yt-av\n") + video + "\n" + audio;
}

inline bool splitYtAv(const std::string& playable, std::string& video, std::string& audio) {
    static const char kPfx[] = "yt-av\n";
    if (playable.compare(0, sizeof kPfx - 1, kPfx) != 0)
        return false;
    const auto nl = playable.find('\n', sizeof kPfx - 1);
    if (nl == std::string::npos)
        return false;
    video = playable.substr(sizeof kPfx - 1, nl - (sizeof kPfx - 1));
    audio = playable.substr(nl + 1);
    if (!audio.empty() && audio.back() == '\n')
        audio.pop_back();
    return !video.empty() && !audio.empty();
}

#if defined(MPX_HAVE_LIBAV)
// Time seek on an input avformat_open_input already opened. AVSEEK_FLAG_BYTE
// is intentionally not passed: the timestamp is in AV_TIME_BASE, and libav
// turns that into a container seek.
inline int seekInputToMs(AVFormatContext* fmt, int64_t startMs) {
    if (!fmt || startMs <= 0)
        return 0;
    const int64_t ts = startMs * (AV_TIME_BASE / 1000);
    return avformat_seek_file(fmt, -1, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
}
#endif

} // namespace gcast
} // namespace misterplex
