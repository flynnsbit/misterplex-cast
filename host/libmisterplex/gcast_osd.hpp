#pragma once
// GCAST P4: the OSD side of the cast receivers.
//  - gcastReadyLine: "Ready", then " · AirPlay" / " · YouTube" when those receivers are up.
//    GH#13: flags off still paints "Ready" (never empty). The card title is MISTERPLEX;
//    Plex is not its own flag. A plex.tv/link code is gcastLinkLine, its own row.
//  - gcastNowPlaying: the playback title, prefixed with the owning receiver ("YouTube: ...").
//  - stampReadyLineRgb24: the ready line, centred near the bottom of a packed RGB24 idle frame.
#include "libmisterplex/gate_error_render.hpp"
#include "libmisterplex/plex_link_label.hpp"
#include "libmisterplex/overscan_safe.hpp"
#include "libmisterplex/ui_safe_layout.hpp"
#include "libmisterplex/plexui/plexui_types.hpp"
#include <algorithm>
#include <cstdint>
#include <string>

namespace misterplex {
namespace airplay_osd {
using gate::kErrGlyphW;
using gate::kErrGlyphH;
using gate::forEachErrorGlyphPixel;

// U+00B7 MIDDLE DOT. The OwenB card font draws it; the 8x16 stamp maps it to '.'.
inline constexpr char kReadyMiddot[] = "\xC2\xB7";

inline std::string gcastReadyLine(bool airplay, bool youtube) {
    std::string s = "Ready";
    auto add = [&](const std::string& part) {
        if (part.empty())
            return;
        s += ' ';
        s += kReadyMiddot;
        s += ' ';
        s += part;
    };
    if (airplay)
        add("AirPlay");
    if (youtube)
        add("YouTube");
    return s;
}

// GH#23 F2150: a linked PLEXTV_TOKEN (len 20 on lab) must not paint plex.tv/link
// on the idle Ready card. QR remaining is the unlink glass path.
inline std::string gcastLinkCodeForIdle(bool linked, const std::string& linkCode) {
#ifdef MUT_GOSD_LINK_WHEN_LINKED
    (void)linked;
    return linkCode;
#else
    return linked ? std::string() : linkCode;
#endif
}

// Own card row. Empty when there is no code, so the row is omitted.
inline std::string gcastLinkLine(const std::string& linkCode) {
    if (linkCode.empty())
        return {};
    if (linkCode == kPlexTvLinkErrorCode)
        return kPlexTvLinkErrorLine;
    if (linkCode == kPlexTvLinkPendingCode)
        return kPlexTvLinkPendingLine;
    return std::string(kPlexTvLinkLabel) + " " + linkCode;
}

// #228: idle-log suffix. The !LINKERR marker is not a code, so it logs as a link error.
inline const char* gcastLinkLogSuffix(const std::string& linkCode) {
    if (linkCode.empty())
        return "";
#ifndef MUT_PLEXLINK_LOG_MARKER_AS_CODE
    if (linkCode == kPlexTvLinkErrorCode)
        return " + plex.tv link error";
    if (linkCode == kPlexTvLinkPendingCode)
        return " + plex.tv link pending";
#endif
    return " + plex.tv code";
}

// Ready row, then the link row when a code is on screen. No browse-key row.
inline std::string gcastIdleHint(bool airplay, bool youtube, const std::string& linkCode = "") {
    std::string s = gcastReadyLine(airplay, youtube);
    const std::string link = gcastLinkLine(linkCode);
    if (!link.empty()) {
        s.push_back('\n');
        s += link;
    }
    return s;
}

// Printable ASCII only (the 8x16 font), at most maxLen bytes (an ellipsis when cut).
inline std::string osdSafe(const std::string& in, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        // U+00B7: the 8x16 font is printable ASCII. A period keeps the separator visible.
        if (c == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0xB7) {
            out.push_back('.');
            ++i;
            continue;
        }
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

inline std::string gcastNowPlaying([[maybe_unused]] const std::string& owner, const std::string& title,
                                   size_t maxLen = 60) {
    // GH#23 F2187: in-play bar is the library title, not "MiSTerPlex Real BBB…".
    const std::string t = osdSafe(plexui::stripMisterPlexPrefix(title), maxLen);
#ifdef MUT_GOSD_PLEX_AS_YOUTUBE
    return osdSafe(std::string("YouTube: ") + t, maxLen);
#else
    if (owner.empty() || owner == "Plex")
        return t;
    return osdSafe(owner + ": " + t, maxLen);
#endif
}

// PLAY-PENDING line under the big chevron. ASCII "..." because the 5x7 OSD font
// has no U+2026; paint uppercases this to "LOADING YOUTUBE...". A letter-only
// source name becomes "Loading <Name>...". Anything else is "Loading...".
// Always on for an armed cue. No conf flag.
inline bool pendingSourceName(const std::string& source) {
    if (source.empty() || source.size() > 24)
        return false;
    for (unsigned char c : source)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return false;
    return true;
}

inline std::string pendingLoadingLine(const std::string& source) {
    if (!pendingSourceName(source))
        return "Loading...";
    return "Loading " + source + "...";
}

// Light grey text on whatever the idle renderer painted; clipped to the frame. Returns false when
// nothing was drawn (empty text, bad target).
inline bool stampReadyLineRgb24(uint8_t* rgb, int w, int h, const std::string& text) {
    if (!rgb || w <= 0 || h <= 0 || text.empty())
        return false;
    const int pct = uiSafePrefs().marginPct;
    const int sym = uiSafeMarginPx(w, pct);
    const OverscanEdges oe = overscanEdges();
    const int mx = overscanEdgeOr(oe.left, w, sym);
    const int my = overscanEdgeOr(oe.bottom, h, uiSafeMarginPx(h, pct));
    const int right = overscanEdgeOr(oe.right, w, sym);
    const int innerW = std::max(kErrGlyphW, w - mx - right);
    const std::string line = osdSafe(text, static_cast<size_t>(innerW / kErrGlyphW));
    const int textW = static_cast<int>(line.size()) * kErrGlyphW;
    int x0 = (w - textW) / 2;
    if (x0 < mx)
        x0 = mx;
    // Default 5% @ 640x480: min(h-48, h-24-16) = 432, same as the historic line.
    const int y0 = std::min(h - 3 * kErrGlyphH, h - my - kErrGlyphH);
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

}  // namespace airplay_osd
namespace gcast_osd = airplay_osd;
}  // namespace misterplex
