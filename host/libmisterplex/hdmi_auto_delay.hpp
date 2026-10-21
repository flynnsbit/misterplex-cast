#pragma once
// HDMI auto delay: start-hold after first video kick + continuous hold slack.
// AUDIO_DELAY_MS stays the user adelay knob. Conf AV_HDMI_AUDIO_LAG_MS>0
// overrides afterVideoMs only. Numbers are MS2109 glass 2026-08-16
// (USB ≠ CRT; not official ±42).

#include <cstring>

namespace misterplex {

struct HdmiAutoDelay {
    int afterVideoMs = 0;
    int holdSlackMs = 80;
    const char* path = "none";
};

// Source class vs the L4 1280×720 present bank.
inline const char* hdmiDelaySourceClass(int sourceW, int sourceH) {
    if (sourceW == 1280 && sourceH == 720)
        return "native1280";
    if (sourceW > 0 && sourceW <= 400 && sourceH > 0 && sourceH <= 300)
        return "upscale240";
    if (sourceW > 0 && sourceW <= 800 && sourceH > 0 && sourceH <= 540)
        return "upscale480";
    if (sourceW > 1280 || sourceH > 720)
        return "downscale720";
    if (sourceW > 0 && sourceH > 0)
        return "scaled";
    return "unknown";
}

// Native 1280 + Display raster (glass 2026-08-16, identity 720p source):
// Slack +80 let audio lead pictures by up to 80 ms. pfps is 24.0 now, so
// slack is the ascal/PHY trim (negative = hold audio for later glass).
//   720p hold106 slack16 medians=-115/-133/-125 → after 106, slack -110
//   480p hold137 slack-5 median=-0.8 → after 137, slack -5
//   240p hold137 slack-70 median=-100 → after 137, slack -170
// Upscale 320→1280: hold335 slack-200 median=-18.3 → 353 / -218.
// Upscale 640→1280: hold335 slack-200 median=+95 → 240 / -105.
// Downscale (Farpoint 1440×1080→1280) uses the Display native1280 bake.
inline int hdmiNativeAfterVideoMs(const char* displayLabel) {
    const char* d = (displayLabel && displayLabel[0]) ? displayLabel : "720p";
    if (std::strcmp(d, "240p") == 0)
        return 137;
    if (std::strcmp(d, "480p") == 0)
        return 137;
    return 106;
}

inline int hdmiNativeHoldSlackMs(const char* displayLabel) {
    const char* d = (displayLabel && displayLabel[0]) ? displayLabel : "720p";
    if (std::strcmp(d, "240p") == 0)
        return -170;
    if (std::strcmp(d, "480p") == 0)
        return -5;
    return -110;
}

inline HdmiAutoDelay hdmiAutoDelayForPlay(const char* displayLabel, int sourceW,
                                          int sourceH) {
    const char* d = (displayLabel && displayLabel[0]) ? displayLabel : "720p";
    const char* cls = hdmiDelaySourceClass(sourceW, sourceH);

    HdmiAutoDelay out;
    if (std::strcmp(cls, "upscale240") == 0) {
        out.afterVideoMs = 353;
        out.holdSlackMs = -218;
        out.path = "upscale240";
        return out;
    }
    if (std::strcmp(cls, "upscale480") == 0) {
        out.afterVideoMs = 240;
        out.holdSlackMs = -105;
        out.path = "upscale480";
        return out;
    }

    out.afterVideoMs = hdmiNativeAfterVideoMs(d);
    out.holdSlackMs = hdmiNativeHoldSlackMs(d);
    if (std::strcmp(d, "240p") == 0)
        out.path = (std::strcmp(cls, "downscale720") == 0) ? "downscale720_disp240"
                                                          : "native1280_disp240";
    else if (std::strcmp(d, "480p") == 0)
        out.path = (std::strcmp(cls, "downscale720") == 0) ? "downscale720_disp480"
                                                          : "native1280_disp480";
    else if (std::strcmp(cls, "downscale720") == 0)
        out.path = "downscale720_disp720";
    else if (std::strcmp(cls, "native1280") == 0)
        out.path = "native1280_disp720";
    else
        out.path = "native1280_default";
    return out;
}

inline int hdmiAutoAfterVideoMs(const HdmiAutoDelay& baked, int confOverrideMs) {
    return (confOverrideMs > 0) ? confOverrideMs : baked.afterVideoMs;
}

} // namespace misterplex
