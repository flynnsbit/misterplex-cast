#pragma once
// Spelunkr picture on the owenb MisterPlex core OSD (h264ring_request_settings).
// Auto Aspect Ratio defaults to 4:3 + Auto Analog Aspect to Letterbox.
// SPELUNKR_ASPECT=16:9 changes only the Auto default, never a hard-set OSD choice.
// Never CEA video_mode poke.

#include "h264ring.h"
#include "libmisterplex/source_aspect.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace misterplex::spelunkr {

// Product 240p/480p banks default 4:3. 720p HDMI class is 16:9 (source_aspect.hpp).
inline constexpr int kTvPresentW = 640;
inline constexpr int kTvPresentH = 480;
inline constexpr SourceAspect kCanvas43{4, 3, true};

// Core OSD Aspect Ratio (h264ring.h): 0 Auto, 1 4:3, 2 16:9.
enum class OsdAspect : unsigned { Auto = 0, Ratio43 = 1, Ratio169 = 2 };
inline constexpr unsigned kAnalogLetterbox = 2;  // Analog Aspect: 4:3 CRT-safe (not Fit=16:9 stretch)

inline OsdAspect osdAspectFromEnv(const char* e) {
    if (!e || !*e)
        return OsdAspect::Ratio43;
    char b[8] = {};
    size_t n = 0;
    for (; e[n] && n + 1 < sizeof b; ++n)
        b[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    b[n] = 0;
    if (std::strcmp(b, "16:9") == 0 || std::strcmp(b, "16x9") == 0 || std::strcmp(b, "169") == 0 ||
        std::strcmp(b, "16") == 0 || std::strcmp(b, "wide") == 0)
        return OsdAspect::Ratio169;
    return OsdAspect::Ratio43;
}

// S01E01 sample (SETTLED): 636x480 SAR 1:1 = 53:40, not exact 4:3.
// 53:40 on a 4:3 640x480 canvas is 636x480 with 2 px pillars per side.
inline constexpr SourceAspect kS01E01Dar{53, 40, true};

inline SourceAspect darFromSquarePixels(int w, int h) {
    return reduceSourceAspect(w, h);
}

// Default is the stream DAR. Bank 4:3 is only the unknown-DAR fallback.
// crop43.json / official b2() is optional labeled crop-to-fill, never default.
inline SourceAspect p1SourceAspect(const SourceAspect& streamDar, int presentW, int presentH) {
    if (streamDar.valid && streamDar.x > 0 && streamDar.y > 0)
        return streamDar;
    return defaultSourceAspectForBank(presentW, presentH);
}

} // namespace misterplex::spelunkr
