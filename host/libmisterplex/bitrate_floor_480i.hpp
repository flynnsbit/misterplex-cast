#pragma once

#include <cstring>

namespace misterplex {

constexpr int bitrateFloor480i = 2000;

inline bool bitrateFloor480iApplies(const char* fpgaMode, const char* present480i,
                                    const char* contentLabel) {
    if (fpgaMode && std::strcmp(fpgaMode, "480i") == 0)
        return true;
    if (present480i && present480i[0] && std::strcmp(present480i, "0") != 0)
        return true;
    if (contentLabel && std::strcmp(contentLabel, "480i") == 0)
        return true;
    return false;
}

// Raise a positive kbps below the 480i floor. 0 stays 0 (unset).
inline int bitrate_floor_480i(const char* fpgaMode, const char* present480i,
                              const char* contentLabel, int kbps) {
    if (!bitrateFloor480iApplies(fpgaMode, present480i, contentLabel))
        return kbps;
    if (kbps <= 0)
        return kbps;
    return kbps < bitrateFloor480i ? bitrateFloor480i : kbps;
}

}  // namespace misterplex
