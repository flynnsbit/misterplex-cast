#pragma once
// SOFT_SUBS (conf) / MPX_SOFT_SUBS (env, wins when set). Default off.
//
// Sidecar SRT/ASS (and other external/key text streams) always use the soft
// overlay band: fetch /library/streams/N and paint. That path does not wait
// on SOFT_SUBS.
//
// Off: packet text is not decoded onto the band except mov_text / "text"
// (#260 overlay even with SOFT_SUBS off). Embedded WebVTT / embedded SRT stay
// on the burn / skip path. Image/PGS stays burn-in.
//
// On: text packets (subrip, ass, ssa, mov_text, webvtt) are kept and decoded
// with libav, then drawn by the existing title-safe caption band (even lines
// at 480i, SUBTITLE_POSITION). External WebVTT is fetched from
// /library/streams/N like SRT. Image/PGS stays burn-in.
//
// Not this slice (STATUS): Phase 0 clock changes, Roboto/atlas, deadline
// pre-render, 720p band scratch, fetch posix_spawn, LB/RB offset, ASS karaoke,
// RTL, CJK coverage burn, image/PGS soft decode, 240p. The cue clock is the
// tip's onCaptionClock(mediaMs()) — no second clock.
//
// Text flatten is ours. Do not copy FFmpeg clients/common/subs.c (GPL).

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace misterplex {

inline bool parseSoftSubsFlag(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isspace(c))
            continue;
        s.push_back(static_cast<char>(std::tolower(c)));
    }
#ifdef MUT_SOFTSUBS_PARSE_SWAP
    if (s == "1" || s == "on" || s == "true" || s == "yes")
        return false;
    if (s == "0" || s == "off" || s == "false" || s == "no")
        return true;
#else
    if (s == "1" || s == "on" || s == "true" || s == "yes")
        return true;
    if (s == "0" || s == "off" || s == "false" || s == "no")
        return false;
#endif
#ifdef MUT_SOFTSUBS_DEFAULT_ON
    return true;
#else
    return false;
#endif
}

inline std::atomic<int>& softSubsStore() {
#ifdef MUT_SOFTSUBS_DEFAULT_ON
    static std::atomic<int> v{1};
#else
    static std::atomic<int> v{0};
#endif
    return v;
}

inline bool softSubsEnabled() {
    return softSubsStore().load(std::memory_order_relaxed) != 0;
}

// Sidecar soft overlay is always on. MUT_SOFTSUBS_OFF_OVERLAY restores the old
// tip that wrongly gated sidecars behind SOFT_SUBS (mutant must fail).
inline bool softOverlayAllowed() {
#ifdef MUT_SOFTSUBS_OFF_OVERLAY
    return softSubsEnabled();
#else
    return true;
#endif
}

inline void setSoftSubsEnabled(bool on) {
    softSubsStore().store(on ? 1 : 0, std::memory_order_relaxed);
}

// #258 embedded SRT demux fallback (see embedded_srt_fallback.hpp).
inline std::atomic<int>& embeddedTextFallbackStore() {
    static std::atomic<int> v{0};
    return v;
}

// True while the live play has armed the #258 fallback. Cleared on every new
// subtitle offer (play start, track change, off).
inline bool embeddedTextFallbackActive() {
    return embeddedTextFallbackStore().load(std::memory_order_relaxed) != 0;
}

inline void setEmbeddedTextFallbackActive(bool on) {
    embeddedTextFallbackStore().store(on ? 1 : 0, std::memory_order_relaxed);
}

// The container packet decoder may run: SOFT_SUBS on, or the #258 fallback is
// armed. Only the one selected text index is decoded either way.
inline bool packetTextDecodeAllowed() {
#ifdef MUT_258_DECODE_GATE
    return softSubsEnabled();
#else
    return softSubsEnabled() || embeddedTextFallbackActive();
#endif
}

// #260: mov_text / "text" always overlay (GET /library/streams is 501). SOFT_SUBS
// still gates other packet-text codecs (embedded WebVTT/SRT).
inline bool packetTextAlwaysOverlay(std::string_view codec) {
#ifdef MUT_260_MOV_STILL_BURN
    (void)codec;
    return false;
#else
    std::string s;
    s.reserve(codec.size());
    for (unsigned char c : codec)
        s.push_back(static_cast<char>(std::tolower(c)));
    return s == "mov_text" || s == "text";
#endif
}

inline bool packetTextDecodeAllowedForCodec(std::string_view codec) {
    return packetTextDecodeAllowed() || packetTextAlwaysOverlay(codec);
}

}  // namespace misterplex
