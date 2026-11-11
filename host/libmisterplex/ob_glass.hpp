#pragma once
// Glass log and the RAOP idle hand-off. Header-only so a unit test does not
// need the ring. The daemon prefixes the line with "media: ".
#include <cstdint>
#include <cstdio>
#include <string>

namespace misterplex::owenb {

// One line a second on the Owenb play thread (AirPlay and YouTube on PlexOB).
// Glass reads fps from pfps. vfps is the same count: video AUs pushed to the core.
inline std::string playedFramesLine(uint64_t frames, double wallSec) {
    const double pfps = wallSec > 0.05 ? static_cast<double>(frames) / wallSec : 0.0;
    char buf[96];
    std::snprintf(buf, sizeof buf, "frames=%llu vfps=%.1f pfps=%.1f",
                  static_cast<unsigned long long>(frames), pfps, pfps);
    return buf;
}

// true: drop the idle card as the play starts. false: leave it up until the
// first picture (RAOP). A 1 fps spectrum would otherwise reveal the last mp4.
inline bool dropIdleCardAtPlayStart(bool keepIdleUntilPicture) {
    return !keepIdleUntilPicture;
}

}  // namespace misterplex::owenb
