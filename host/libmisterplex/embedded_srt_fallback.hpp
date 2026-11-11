#pragma once
// #258: PMS answers GET /library/streams/<id> with 501 Not Implemented for an
// EMBEDDED SRT/ASS track (glass rk 28, stream 69). The overlay fetch then fails
// and, flags off, no caption shows. Fallback: demux that text track from the
// container the daemon already reads (owenb_av_source packet decoder, the same
// decoder SOFT_SUBS uses) and paint its cues on the same overlay band.
//
// Spec #252: SRT/ASS go to the overlay. This fallback has no burn path; burn is
// only the SUBTITLE_MODE=burn opt-in last resort (SubtitleControl::unavailable).
// A soft-text pick it cannot serve (external sidecar, or a PMS remux without the
// track) logs; no caption unless the user opted into burn.

#include "soft_subs.hpp"
#include "track_select.hpp"

#include <string>
#include <utility>
#include <vector>

namespace misterplex {

// embeddedTextFallbackActive / packetTextDecodeAllowed live in soft_subs.hpp
// (the demuxer includes only that header).

// SRT/ASS/SSA the overlay owns (spec #252).
inline bool subtitleIsOverlayText(const std::string& codec) {
    return subtitleCodecIsSoftText(codec);
}

// The pick may take the demux fallback: an embedded (not external / not a
// sidecar key) SRT/ASS subtitle with a known container index. The demuxer then
// checks that this index in the input it actually opened is a text subtitle
// (armTextSubtitle); a PMS remux without the track fails there, logged, no burn.
inline bool embeddedTextFetchFallbackEligible(const MediaStream& s) {
    if (s.kind != StreamKind::Subtitle || !subtitleIsOverlayText(s.codec))
        return false;
#ifndef MUT_258_EXTERNAL_ELIGIBLE
    // A PMS key (/library/streams/<id>) is not proof of a sidecar: embedded rk 28
    // stream 69 carries one and answers 501. Only location=external is a sidecar.
    if (trackLower(s.location) == "external")
        return false;
#endif
    return s.index >= 0;
}

// One band, one source. The sidecar wins when it is armed. Packet cues paint
// only when it is not, so a caption never shows twice.
enum class CaptionSource { None, Sidecar, Packet };

inline CaptionSource captionSourceFor(bool sidecarArmed, bool packetNonEmpty) {
#ifdef MUT_258_DOUBLE_PAINT
    if (packetNonEmpty)
        return CaptionSource::Packet;
#endif
    if (sidecarArmed)
        return CaptionSource::Sidecar;
    return packetNonEmpty ? CaptionSource::Packet : CaptionSource::None;
}

// FU8 nit C: the burn /decision and the old-session stop must not carry the start
// probe's Range header (PMS answered the decision with 206).
inline std::vector<std::pair<std::string, std::string>> withoutRangeHeader(
    std::vector<std::pair<std::string, std::string>> h) {
#ifndef MUT_FU8_KEEP_RANGE
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& kv : h) {
        std::string low;
        for (unsigned char c : kv.first) low.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        if (low != "range") out.push_back(kv);
    }
    return out;
#else
    return h;
#endif
}

} // namespace misterplex
