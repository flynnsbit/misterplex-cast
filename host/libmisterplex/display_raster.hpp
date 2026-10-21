#pragma once
// F12 Display → /dev/MiSTer_cmd video_mode. Unknown label = no write.
// L4 product stays on CEA 720p60. kVideoModeCmd720p24 is legal, unused.

#include "libmisterplex/osd_menu.hpp"
#include "libmisterplex/core_id_select.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace misterplex {

inline constexpr const char* kMisterCmdPath = "/dev/MiSTer_cmd";
inline constexpr std::size_t kMisterCmdBuf = 80;

// 15 kHz NTSC 240p (bob). Htot=416 Vtot=262 Fpix=6.5 MHz → ~15.625 kHz / 59.6 Hz.
// VGA/RGB CRT TV. Not HDMI-safe (below TMDS floor). Present store stays 640×480.
inline constexpr const char* kVideoModeCmd240p = "video_mode 320,16,32,48,240,4,3,15,6500";
// Legacy 31 kHz LCD/VGA monitor "240p" (VESA 640×480@59.94, ascal preset 6).
inline constexpr const char* kVideoModeCmd240pLcd = "video_mode 640,16,96,48,480,10,2,33,25175";
// 15 kHz NTSC 480i weave. BT.601 858×262 field, 13.5 MHz → 15.734 kHz / 60.05 field.
// vact=240 is one field; ascal weaves even/odd lines of the 640×480 store.
inline constexpr const char* kVideoModeCmd480i = "video_mode 720,19,62,57,240,4,3,15,13500";
// VESA SVGA 800×600@60 (ascal preset 5). LCD / 31 kHz VGA.
inline constexpr const char* kVideoModeCmd480p = "video_mode 800,40,128,88,600,1,4,23,40000";
// CEA-861 720p60 (ascal preset 0). Not 24 Hz.
inline constexpr const char* kVideoModeCmd720p = "video_mode 1280,110,40,220,720,5,5,20,74250";
// CEA 720p24. Legal cmd; L4 stays on 720p60 (ascal 60→24 hurt decode).
inline constexpr const char* kVideoModeCmd720p24 =
    "video_mode 1280,110,40,220,720,5,5,28,30000";

static_assert(sizeof("video_mode 1280,110,40,220,720,5,5,20,74250\n") < kMisterCmdBuf,
              "CEA 720p60 cmd + NL must fit kMisterCmdBuf");
static_assert(sizeof("video_mode 1280,110,40,220,720,5,5,28,30000\n") < kMisterCmdBuf,
              "CEA 720p24 cmd + NL must fit kMisterCmdBuf");

inline const char* videoModeCmdForDisplayLabel(const char* label) {
    if (!label || !label[0])
        return nullptr;
    if (std::strcmp(label, "240p") == 0)
        return kVideoModeCmd240p;
    if (std::strcmp(label, "240p_lcd") == 0)
        return kVideoModeCmd240pLcd;
    if (std::strcmp(label, "480i") == 0)
        return kVideoModeCmd480i;
    if (std::strcmp(label, "480p") == 0)
        return kVideoModeCmd480p;
    if (std::strcmp(label, "720p") == 0)
        return kVideoModeCmd720p;
    return nullptr;
}

inline const char* videoModeCmdForDisplaySel(unsigned displaySel, const char* contentLabel) {
    switch (displaySel & 3u) {
    case 1:
        return videoModeCmdForDisplayLabel("240p");
    case 2:
        return videoModeCmdForDisplayLabel("480p");
    case 3:
        return videoModeCmdForDisplayLabel("720p");
    default:
        return videoModeCmdForDisplayLabel(contentLabel);
    }
}

inline const char* videoModeCmdForOsdWord(uint16_t word) {
    const OsdSettings s = decodeOsdWord(word);
    return videoModeCmdForDisplayLabel(s.displayResolution.label);
}

inline bool videoModeCmdIsSafe(const char* cmd) {
    return cmd && (std::strcmp(cmd, kVideoModeCmd240p) == 0 ||
                   std::strcmp(cmd, kVideoModeCmd240pLcd) == 0 ||
                   std::strcmp(cmd, kVideoModeCmd480i) == 0 ||
                   std::strcmp(cmd, kVideoModeCmd480p) == 0 ||
                   std::strcmp(cmd, kVideoModeCmd720p) == 0 ||
                   std::strcmp(cmd, kVideoModeCmd720p24) == 0);
}

inline const char* videoModeCmdIfChanged(const char* nextCmd, const char* lastCmd) {
    if (!videoModeCmdIsSafe(nextCmd))
        return nullptr;
    if (lastCmd && lastCmd[0] && std::strcmp(nextCmd, lastCmd) == 0)
        return nullptr;
    return nextCmd;
}

// Content O[5:4] applies live. Display O[15:14] latches at daemon start / core
// reset only — flipping F12 Display must not poke video_mode until reset.
inline bool displayRasterApplyOnOsdChange() { return false; }
inline bool contentResApplyOnOsdChange() { return true; }
inline bool displayRasterApplyOnStartup() { return true; }
// Cast/play must not rewrite /dev/MiSTer_cmd when the latched CEA mode is
// already live. force=true renegotiates HDMI, drops vsync, and leaves glass
// on the last chevron while Plex Web still reports playing (hw_presents=0).
inline constexpr bool kForceDisplayRasterOnPlay = false;

// Last comma field of a frozen custom modeline is Fpix in kHz.
inline int videoModePixelKhz(const char* cmd) {
    if (!cmd || !cmd[0])
        return 0;
    const char* last = std::strrchr(cmd, ',');
    if (!last || !last[1])
        return 0;
    int khz = 0;
    for (const char* p = last + 1; *p >= '0' && *p <= '9'; ++p)
        khz = khz * 10 + (*p - '0');
    return khz;
}

// 31 kHz LCD/VGA floor. 15 kHz CRT modes (240p 6.5 MHz, 480i 13.5 MHz) are
// legal and listed in videoModeCmdIsSafe; they are below this floor on purpose.
inline constexpr int kVgaMinPixelKhz = 25175;
inline bool videoModeIsCrt15k(const char* cmd) {
    return cmd && (std::strcmp(cmd, kVideoModeCmd240p) == 0 ||
                   std::strcmp(cmd, kVideoModeCmd480i) == 0);
}

inline bool misterCmdLineIsVolume(const char* line) {
    // Main input.cpp: "volume mute|unmute|0..7". 0=quietest, 7=loudest.
    if (!line)
        return false;
    if (std::strncmp(line, "volume ", 7) != 0)
        return false;
    const char* arg = line + 7;
    if (std::strcmp(arg, "mute") == 0 || std::strcmp(arg, "unmute") == 0)
        return true;
    return arg[0] >= '0' && arg[0] <= '7' && arg[1] == '\0';
}

inline int writeMisterCmdLine(const char* path, const char* line) {
    if (!path || !path[0])
        return 0;
    if (!videoModeCmdIsSafe(line) && !misterCmdLineIsVolume(line) && !loadCoreLineAllowed(line))
        return 0;
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char buf[kMisterCmdBuf];
    const int n = std::snprintf(buf, sizeof(buf), "%s\n", line);
    ssize_t w = -1;
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(buf))
        w = ::write(fd, buf, static_cast<std::size_t>(n));
    ::close(fd);
    if (w != static_cast<ssize_t>(n))
        return -1;
    return 1;
}

// FPGA sys_top vol_att resets to 5'b11111 (mute) until Main UIO_AUDVOL.
// Free-run MrAudio can advance rptr while HDMI/analog stay silent. Assert
// full volume after Plex load / before host-paint ALSA.
inline void assertMisterFullVolume(const char* path = kMisterCmdPath) {
    (void)writeMisterCmdLine(path, "volume unmute");
    (void)writeMisterCmdLine(path, "volume 7");
}

} // namespace misterplex
