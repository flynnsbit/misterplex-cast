#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {

// OwenbRing: owenb321's H.264 decoder core (the PlexOB fork), fed through the ring v1.x
// (owenb_ring.hpp). OWENB-P1B T3: the core gate picks it per core. A verified PlexOB core
// (CoreGate::owenbOpen()) runs OwenbRing whatever MPX_VIDEO_BACKEND says, because a legacy backend
// cannot drive it. MPX_VIDEO_BACKEND=owenb-ring forces it on any core (lab override). Every
// other core keeps the configured legacy backend. See sessionVideoBackend().
enum class VideoBackend { Unselected, LegacySoftware, FpgaH264, OwenbRing };

inline VideoBackend parseVideoBackend(const char* value) {
    if (value && std::strcmp(value, "fpga-h264") == 0)
        return VideoBackend::FpgaH264;
    if (value && std::strcmp(value, "legacy-software") == 0)
        return VideoBackend::LegacySoftware;
    if (value && std::strcmp(value, "owenb-ring") == 0)
        return VideoBackend::OwenbRing;
    return VideoBackend::Unselected;
}

inline VideoBackend configuredVideoBackend() {
    return parseVideoBackend(std::getenv("MPX_VIDEO_BACKEND"));
}

// The backend for one play session. configured: configuredVideoBackend(). owenbCoreLoaded: the
// core gate saw a PlexOB core (CoreGate::ownsIdleInFabric()). MUT_BACKEND_IGNORE_GATE (test-only)
// drops the gate's say; test_owenb_core_gate row S2 must catch that.
inline VideoBackend sessionVideoBackend(VideoBackend configured, bool owenbCoreLoaded) {
    if (configured == VideoBackend::OwenbRing)
        return VideoBackend::OwenbRing;
#ifndef MUT_BACKEND_IGNORE_GATE
    if (owenbCoreLoaded)
        return VideoBackend::OwenbRing;
#else
    (void)owenbCoreLoaded;
#endif
    return configured;
}

// Legacy Utility reload (menu.rbf, then Plex_240p/480i/480p) is for those cores.
// A PlexOB session stays on the loaded core. Bouncing to Plex_480i leaves the ring.
inline bool legacyMenuReloadApplies(VideoBackend session) {
    return session != VideoBackend::OwenbRing;
}

inline bool matchedLegacyVideoCore(const std::string& livePrefix, const char* expected) {
    if (!expected || livePrefix != expected)
        return false;
    // R15 480i present-only (PLEX_PRESENT_480I) md5 0826ef77 — software paint
    // to 720×480 / 0x301FF000 while decode may still be PMS-scaled.
    return livePrefix == "dfebf2bf" || livePrefix == "41adb98c" ||
           livePrefix == "07f54d9f" || livePrefix == "4d6efef9" ||
           livePrefix == "61db00e7" || livePrefix == "0826ef77" ||
           livePrefix == "061cdb87" || // R15-r2 present (stride2 black)
           livePrefix == "d9eeeebd" || // R15-r3b SEED13 stride1 host-paint
           livePrefix == "5bb30344" || // R17d SEED14 DDR DPB 624x480
           livePrefix == "9703ab7d" || // progressive 480i present (no weave Y alias)
           livePrefix == "56f38fff" || // e36 480i free-run ALSA (SESSION_CONTROL=0) md5
           livePrefix == "9014f49e"; // e37 host-paint 480p free-run ALSA md5
}

} // namespace misterplex
