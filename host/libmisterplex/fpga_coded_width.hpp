#pragma once

#include "libmisterplex/ddr_frame_layout.hpp"

#include <algorithm>
#include <cstdint>

namespace misterplex {

// Env/config ceiling for 480-tier FPGA decode (624 default, 640 TRUE640 opt-in).
// Implemented in plex_resolve.cpp (reads MPX_FPGA_480P_CODED_W).
int fpga480pCodedWidthCeiling();

// One product rule (F918 / F906 232003): coded width may be any value up to the
// core's CAPS max_width; the present bank pads horizontally (480i 720-wide host
// bank accepts 624-wide ladder / bitstream).
inline int fpga480TierCodedWidthCeiling(uint16_t capsMaxWidth) {
    constexpr int kTrue640 = 640;
    const int envCeiling = fpga480pCodedWidthCeiling();
    if (envCeiling == kTrue640 && capsMaxWidth >= static_cast<uint16_t>(kTrue640))
        return kTrue640;
    if (capsMaxWidth > 0)
        return std::min(envCeiling, static_cast<int>(capsMaxWidth));
    return envCeiling;
}

inline bool fpgaCodedWidthWithinCaps(int codedWidth, uint16_t capsMaxWidth) {
    if (codedWidth <= 0)
        return true;
    return codedWidth <= fpga480TierCodedWidthCeiling(capsMaxWidth);
}

// Session begin + demux open: clamp DECODE/ladder geometry to CAPS (ceiling, not equality).
inline void fpgaDemuxExpectCodedGeometry(int decodeW, int decodeH, uint16_t capsMaxW,
                                         uint16_t capsMaxH, int& expectW, int& expectH) {
    expectW = decodeW > 0 ? decodeW : 320;
    expectH = decodeH > 0 ? decodeH : 240;
    if (expectW >= 640 && expectH >= 480) {
        expectW = fpga480TierCodedWidthCeiling(capsMaxW);
        expectH = kPlex480pCodedHeight;
    }
    if (capsMaxW >= 480 && expectW > static_cast<int>(capsMaxW))
        expectW = static_cast<int>(capsMaxW);
    if (capsMaxH >= 480 && expectH > static_cast<int>(capsMaxH))
        expectH = static_cast<int>(capsMaxH);
}

inline bool fpgaCoreAcceptsDecodeGeometry(int wantW, int wantH, uint16_t capsMaxW,
                                        uint16_t capsMaxH) {
    if (capsMaxW < 320 || capsMaxH < 240)
        return false;
    int ceilingW = wantW;
    int ceilingH = wantH;
    if (wantW >= 640 && wantH >= 480) {
        ceilingW = fpga480TierCodedWidthCeiling(capsMaxW);
        ceilingH = kPlex480pCodedHeight;
    }
    if ((ceilingW > 320 || ceilingH > 240) && capsMaxW <= 320 && capsMaxH <= 240)
        return true; // stale CAPS; DECODE geometry wins (pre-CAPS-fix cores)
    return ceilingW <= static_cast<int>(capsMaxW) && ceilingH <= static_cast<int>(capsMaxH);
}

}  // namespace misterplex
