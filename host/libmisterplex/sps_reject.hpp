#pragma once

#include "libmisterplex/ddr_frame_layout.hpp"

#include <cstring>

namespace misterplex {

// Coarse SPS reject code for glass/logs. "" if the inspect field is not
// profile / level / vui (PPS, slice, refs, geometry stay unset).
inline const char* spsRejectCodeFromField(const char* field) {
    if (!field || !field[0])
        return "";
    if (std::strcmp(field, "profile_idc") == 0 || std::strcmp(field, "constraint_set_flags") == 0)
        return "profile";
    if (std::strcmp(field, "level_idc") == 0)
        return "level";
    if (std::strcmp(field, "vui_parameters") == 0 ||
        std::strcmp(field, "video_full_range_flag") == 0 ||
        std::strcmp(field, "matrix_coefficients") == 0 ||
        std::strcmp(field, "colour_description_present_flag") == 0)
        return "vui";
    return "";
}

inline const char* spsRejectCode480pFromField(const char* field) {
    return spsRejectCodeFromField(field);
}

inline bool isPlex480pPresentBank(int outW, int outH) {
    return outW == kPlex480pPresentedWidth && outH == kPlex480pPresentedHeight;
}

// 480i-only: drop when a valid SPS is not 720x480. 240p/480p always false.
inline bool spsReject480i(bool mode480i, bool coded480i, bool spsValid, int spsW, int spsH) {
    if (!mode480i && !coded480i)
        return false;
    if (!spsValid)
        return false;
    if (spsW == kPlex480iCodedWidth && spsH == kPlex480iCodedHeight)
        return false;
    // FPGA 480 tier is 624-wide (624x480, and the 624x454 field that selects
    // Plex_480i). The present bank for that core is 624 wide.
    if (spsW == kPlex480pCodedWidth && spsH >= 450 && spsH <= 480)
        return false;
    return true;
}

}  // namespace misterplex
