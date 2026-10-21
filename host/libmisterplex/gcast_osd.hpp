#pragma once
// GCAST P4: the OSD side of the cast receivers. With GAIRPLAY/GYT off nothing here is called
// (the ready line is empty and the idle screens are unchanged).
//  - gcastReadyLine: "Ready: Plex | AirPlay | YouTube" for the enabled receivers.
//  - gcastNowPlaying: the playback title, prefixed with the owning receiver ("YouTube: ...").
//  - stampReadyLineRgb24: the ready line, centred near the bottom of a packed RGB24 idle frame.
#include "libmisterplex/gate_error_render.hpp"
#include <cstdint>
#include <string>

namespace misterplex {
namespace gcast_osd {
using gate::kErrGlyphW;
using gate::kErrGlyphH;
using gate::forEachErrorGlyphPixel;

inline std::string gcastReadyLine(bool airplay, bool youtube) {
#ifndef MUT_GOSD_READY_FLAGS_OFF
    if (!airplay && !youtube)
        return std::string();
#endif
    std::string s = "Ready: Plex";
    if (airplay)
        s += " | AirPlay";
    if (youtube)
        s += " | YouTube";
    return s;
}

// Printable ASCII only (the 8x16 font), at most maxLen bytes (an ellipsis when cut).
inline std::string osdSafe(const std::string& in, size_t maxLen) {
    std::string out;
    for (unsigned char c : in) {
        if (c >= 0x20 && c < 0x7f)
            out.push_back(static_cast<char>(c));
        else if (c >= 0x80 && (out.empty() || out.back() != '?'))
            out.push_back('?');   // a UTF-8 sequence becomes one '?'
    }
#ifndef MUT_GOSD_NO_CAP
    if (out.size() > maxLen)
        out = out.substr(0, maxLen > 3 ? maxLen - 3 : 0) + "...";
#endif
    return out;
}

inline std::string gcastNowPlaying(const std::string& owner, const std::string& title, size_t maxLen = 60) {
    const std::string t = osdSafe(title, maxLen);
    if (owner.empty() || owner == "Plex")
        return t;
    return osdSafe(owner + ": " + t, maxLen);
}

// Light grey text on whatever the idle renderer painted; clipped to the frame. Returns false when
// nothing was drawn (empty text, bad target).
inline bool stampReadyLineRgb24(uint8_t* rgb, int w, int h, const std::string& text) {
    if (!rgb || w <= 0 || h <= 0 || text.empty())
        return false;
    const std::string line = osdSafe(text, static_cast<size_t>(w / kErrGlyphW));
    const int textW = static_cast<int>(line.size()) * kErrGlyphW;
    const int x0 = (w - textW) / 2;
    const int y0 = h - 3 * kErrGlyphH;
    if (y0 < 0)
        return false;
    forEachErrorGlyphPixel({line}, x0, y0, [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h)
            return;
        uint8_t* p = rgb + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
        p[0] = p[1] = p[2] = 200;
    });
    return true;
}

}  // namespace gcast_osd
}  // namespace misterplex
