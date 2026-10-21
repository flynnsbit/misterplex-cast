#pragma once
// PlexOB aspect request. The daemon passes this mask and these values to
// h264ring_request_settings. Crop from the demux is not rewritten here.

#include "h264_sps.hpp"
#include "h264ring.h"

#include <cstdint>

namespace misterplex {

enum class OwenbAspectMode { Auto, Letterbox, Fit, Crop };

struct OwenbCrop {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

struct OwenbAspectRequest {
    unsigned mask = 0;
    unsigned values[H264RING_OPT_COUNT]{};
};

inline unsigned owenbAnalogAspectValue(OwenbAspectMode mode) {
    switch (mode) {
    case OwenbAspectMode::Fit:
        return 1;
    case OwenbAspectMode::Letterbox:
        return 2;
    case OwenbAspectMode::Crop:
        return 3;
    case OwenbAspectMode::Auto:
        return 0;
    }
    return 2;
}

// HDMI aspect stays Auto unless the container DAR is 4:3 and the stream has no VUI.
// The core treats a missing VUI as 16:9.
// 0008: "4:3" within 3% (a 720x480 DVD at SAR 10:11 is 1.364, 704 active columns make it 4:3 exactly).
inline bool owenbIs43(int64_t darNum, int64_t darDen) {
    if (darNum <= 0 || darDen <= 0)
        return false;
    const int64_t a = darNum * 3, b = darDen * 4;   // 4:3 <=> a == b
    return (a > b ? a - b : b - a) * 100 <= b * 3;
}
inline bool owenbForceAspect43(bool vuiPresent, int64_t darNum, int64_t darDen) {
    return !vuiPresent && owenbIs43(darNum, darDen);
}

// 0008 (design d.3.5): one play's decision, from its first video keyframe (Annex-B, SPS in band after
// h264_mp4toannexb) and the container's display aspect (IMediaSource::containerAspect).
//   - The SPS states an aspect (VUI aspect_ratio_info, idc != Unspecified): the core uses it. Nothing.
//   - No SPS found, or its VUI unreadable: unknown. Nothing (the user's menu decides).
//   - No aspect in the SPS: the core reads it as 16:9 (OB display.md:178-179). The display aspect is
//     the container's when it states one, otherwise the picture's own shape (square samples); 4:3 ->
//     ask Aspect Ratio 4:3 for this session (owenb_aspect_guard.hpp restores the user's at the end).
struct OwenbStreamAspect {
    bool spsFound = false;
    bool spsAspect = false;   // the SPS states a sample aspect
    int64_t darNum = 0, darDen = 0;
    bool force43 = false;
    // BUG6 (glass a lab MiSTer 1017): the coded picture itself is 4:3 (square samples, e.g. a PMS 960x720
    // transcode of a 1440x1080 4:3 source). Video Output Native (HDMI) shows it at its own pixels,
    // pillarboxed in the core's 16:9 raster, so Aspect 4:3 there would squeeze the whole raster.
    bool square43 = false;
};
inline OwenbStreamAspect owenbStreamAspect(const uint8_t* au, size_t n, bool haveContainer, int64_t cNum, int64_t cDen) {
    OwenbStreamAspect r;
    const SpsInfo sps = parseFirstSpsAnnexB(au, n);
    if (!sps.valid || !sps.vuiParsed)
        return r;
    r.spsFound = true;
    r.spsAspect = sps.hasAspect();
    if (haveContainer && cNum > 0 && cDen > 0) {
        r.darNum = cNum;
        r.darDen = cDen;
    } else {
        r.darNum = sps.width;
        r.darDen = sps.height;
    }
    r.square43 = !r.spsAspect && owenbIs43(sps.width, sps.height) && owenbIs43(r.darNum, r.darDen);
#ifdef MUT_ASPECT43_ANY_VUI
    r.force43 = owenbForceAspect43(false, r.darNum, r.darDen);   // mutant: over the SPS's own aspect
#else
    r.force43 = owenbForceAspect43(r.spsAspect, r.darNum, r.darDen);
#endif
    return r;
}

// 0006 (review 10:08): no VIDEO_OUTPUT in the mask -- changing it switches the display's signal
// (the TV resyncs, h264ring.h:229-231) and overrides the user's menu. HDMI ASPECT is asked only to
// force 4:3 for a VUI-less 4:3 stream; otherwise the user's ASPECT is left alone. The analog value
// is the daemon's Letterbox default (owenb_aspect_guard.hpp restores the user's at the end).
inline OwenbAspectRequest owenbAspectRequest(OwenbAspectMode mode, bool vuiPresent, int darNum, int darDen) {
    OwenbAspectRequest req;
    req.mask = 1u << H264RING_OPT_ANALOG_ASPECT;
    req.values[H264RING_OPT_ANALOG_ASPECT] = owenbAnalogAspectValue(mode);
    if (owenbForceAspect43(vuiPresent, darNum, darDen)) {
        req.mask |= 1u << H264RING_OPT_ASPECT;
        req.values[H264RING_OPT_ASPECT] = 1u;
    }
    return req;
}

inline OwenbCrop owenbKeepCrop(const OwenbCrop& crop) {
    return crop;
}

} // namespace misterplex
