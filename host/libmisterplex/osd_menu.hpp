#pragma once
// Decoding of the core's OSD menu word (status[15:0]) into daemon settings.
//
// Why status[15:0]: when the core raises status_set, Main_MiSTer *replaces* the
// whole 128-bit OSD word with status_in. Layout v2 cores (CONF_STR v,9) echo only
// status[15:0] (and AR [122:121]) back, so their user-settable menu bits live down
// there; everything above is Phase-3 telemetry (residual csum, NAL counts) whose
// byte layout is protocol-locked. See Plex.sv "Layout v2 (OSD-safe, 128b)".
// Layout v3 cores (CONF_STR v,10, OSD-MENU-SPEC-1002) also echo bytes 15
// [127:120] and 12 [103:96] as OSD settings: the build row (fpga_build_ids.hpp)
// says which, and status_telemetry.hpp decodes them. This word is [15:0] only.
//
// Bit budget after reclaiming the debug items (Content FPS / Pattern / Audio tone /
// Force bars), which are obsolete now that the daemon detects the exact rational
// rate itself:
//
//   [0]      Reset            (core)
//   [1]      A/V resync       0=On 1=Off
//   [2]      TV Mode          (core)
//   [3]      Audio clock trim 0=On (685 ppm) 1=Off
//   [5:4]    Content res      PMS ladder / DECODE — 0=240p 1=480p 2|3=720p
//                             (v7 only drove O[4] → 240p/480p)
//   [9:6]    A/V offset       4-bit SIGNED, 20 ms/step → -160..+140 ms
//   [10]     unused (v11 Browse is J1/PS2, not a T[] bit)
//   [11]     T Flush bitstream FIFO
//   [12:13]  reserved HPS DDR kick/bank — never reuse
//   [15:14]  Display res (v9) 0=Follow content, 1=240p, 2=480p, 3=720p
//                             Default 0 keeps v8 cores safe (those bits were Idle
//                             logo=0) so PMS 720p is not forced to 240p present.
//                             Idle screen is conf IDLE_SCREEN= only from v9.
//
// Pure decode so it can be unit-tested without an FPGA.
//
// CONF_STR file slots (`F1,raw,...`) and controller labels (`J1,...`) are menu
// metadata only. They do not allocate OSD status bits, so fixing F labels or
// adding J1 names does not change this v7 bit layout.

#include <cstdint>
#include <cstring>

namespace misterplex {

// Step/bias of the video delay list. Kept here so the CONF_STR generator, the
// daemon and the tests cannot disagree about what a menu index means.
constexpr int kOsdAvOffsetSteps = 16;
constexpr int kOsdAvOffsetStepMs = 20;

// Menu index 0 is the power-on default. The present loop waits for the audio
// clock to reach `frameContentMs + avOffsetMs`, so POSITIVE holds the frame back
// and makes video LATER. Raise it when audio sounds late (video running ahead).
//
// The default is 0 and that is now a real claim, not a punt. The latency that
// software CAN see is measured and removed: the MrAudio DMA ring runs ~185 ms
// deep during playback (and varies per session, see mraudio_status.hpp), and the
// pump subtracts it live. What remains is only the difference between the audio
// and video paths downstream of the driver — FPGA output, HDMI, and the
// display's own processing — which is per-display and cannot be measured from
// the ARM. Displays commonly add tens of ms of VIDEO processing, so a negative
// trim is normal here.
//
// Do not bake a non-zero constant in without eyes-on or capture evidence for
// THIS clock; a value tuned against the old submitted-byte clock is not
// comparable, because it silently absorbed that session's ring depth.
constexpr int kOsdAvOffsetDefaultMs = 0;

struct ContentResolution {
    int width = 320;
    int height = 240;
    const char* label = "240p";
    int weakBitrateKbps = 1000;
};

struct OsdSettings {
    int avOffsetMs = 0;
    // O[3] is a debug kill-switch for the feed-rate trim, not a value. It used
    // to decode to a hardcoded ppm, which silently overrode AUDIO_CLOCK_PPM the
    // moment the OSD was polled — i.e. always — so the conf key did nothing.
    // The ppm itself belongs to the daemon; the menu only says on or off.
    bool audioClockTrimEnabled = true;
    bool resyncEnabled = true;
    int idleMode = 0; // conf IDLE_SCREEN from v9; legacy v8 OSD bits ignored
    ContentResolution contentResolution; // O[5:4] → PMS / DECODE
    ContentResolution displayResolution; // O[15:14] → FPGA present bank (v9)
};

// Signed wrap around the default: index 0 is the default, 1..7 step up and
// 8..15 step down. That keeps the list monotonic across the wrap (index 15 is
// one step BELOW index 0), so right/left on the OSD is a plain up/down knob.
inline int osdAvOffsetMsFromIndex(unsigned idx) {
    int i = static_cast<int>(idx % kOsdAvOffsetSteps);
    if (i >= kOsdAvOffsetSteps / 2)
        i -= kOsdAvOffsetSteps;
    return i * kOsdAvOffsetStepMs + kOsdAvOffsetDefaultMs;
}

inline ContentResolution contentResolutionFromOsdWord(uint16_t word) {
    // O[5:4] two-bit selector (v8+). v7 only toggled O[4] with O[5]=0, so
    // codes 0/1 still mean 240p/480p on older cores without a daemon break.
    // Labels are product names (240p/480p/720p); width/height remain bank geom.
    // weakBitrateKbps is NOT kept equal to plexTranscodeProfiles(): 240p and 720p differ
    // (known ladder split, parked as option C; tests/unit/test_resolve.cpp pins it).
    // See the 240p ladder split ticket (1005).
    switch ((word >> 4) & 3u) {
    case 1:
        return {640, 480, "480p", 2500};
    case 2:
    case 3:
        return {1280, 720, "720p", 1500};
    default:
        return {320, 240, "240p", 1000};
    }
}

inline ContentResolution resolutionFromLabel(const char* label) {
    if (label && std::strcmp(label, "720p") == 0)
        return {1280, 720, "720p", 1500};
    if (label && std::strcmp(label, "480i") == 0)
        return {720, 480, "480i", 8000};
    if (label && std::strcmp(label, "480p") == 0)
        return {640, 480, "480p", 2500};
    if (label && std::strcmp(label, "240p_lcd") == 0)
        return {320, 240, "240p_lcd", 1000};
    return {320, 240, "240p", 1000};
}

inline ContentResolution contentResolutionFromSize(int w, int h) {
    // Match product DDR frame-store tiers: 320x240, 640x480 (→624 coded),
    // 720x480 (R15 PLEX_PRESENT_480I host-paint), 1280x720.
    // Prior bug: any w>=640 collapsed to 640x480, so DECODE=1280x720 still played
    // 624x480 into a 1280x720 core → full-field yellow/static on glass.
    // Second bug: DECODE=720x480 collapsed to 640x480 "480p", so the 480i host
    // ladder never stuck (WEAK_RES=720x480 rewritten at play time).
    if (w >= 1280 || h >= 720)
        return {1280, 720, "720p", 1500};
    if (w >= 720 && h >= 480)
        return {720, 480, "480i", 8000};
    if (w >= 640 || h >= 480)
        return {640, 480, "480p", 2500};
    return {320, 240, "240p", 1000};
}

inline ContentResolution displayResolutionFromOsdWord(uint16_t word,
                                                      const ContentResolution& content) {
    // O[15:14] v9: 0=Follow content (also v8 idle default), 1=240p, 2=480p, 3=720p.
    switch ((word >> 14) & 3u) {
    case 1:
        return {320, 240, "240p", 1000};
    case 2:
        return {640, 480, "480p", 2500};
    case 3:
        return {1280, 720, "720p", 1500};
    default:
        return content;
    }
}

inline OsdSettings decodeOsdWord(uint16_t word) {
    OsdSettings s;
    s.contentResolution = contentResolutionFromOsdWord(word);
    s.displayResolution = displayResolutionFromOsdWord(word, s.contentResolution);
    s.resyncEnabled = ((word >> 1) & 1u) == 0u;
    s.audioClockTrimEnabled = ((word >> 3) & 1u) == 0u;
    s.avOffsetMs = osdAvOffsetMsFromIndex((word >> 6) & 0x0Fu);
    // Idle screen is conf-only from v9 (O[15:14] = display res). Leave 0=logo.
    s.idleMode = 0;
    return s;
}

// Bits the daemon reacts to. [0] reset, [2] TV mode,
// [11] bitstream flush and [13:12] DDR kick/bank are not user settings and
// toggle constantly during playback. O[5] is part of content-res (v8).
//
// The daemon NEVER writes these bits. Main_MiSTer owns the OSD word (and saves it
// to the core's config file, whose saved word the CONF_STR "v,N" bump resets:
// v,9 on v2 cores, v,10 on v3); a daemon-side write only fights Main's shadow and
// makes the value flap between the two.
// Includes O[5] content-res and O[15:14] display-res so 240↔720 are not ignored.
// Still ignored: reset[0] TV[2] unused[10] T[11] DDR[13:12].
constexpr uint16_t kOsdOwnedMask = 0xC3FA;

inline bool osdChanged(uint16_t a, uint16_t b) {
    return ((a ^ b) & kOsdOwnedMask) != 0;
}

} // namespace misterplex
