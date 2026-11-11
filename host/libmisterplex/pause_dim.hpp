#pragma once
// PAUSE_DIM (misterplex.conf) / MPX_PAUSE_DIM (env wins). Default off.
//
// On: the OSD/UI pixels this daemon paints are scaled to kPauseDimNumer/255
// (~40%) of their straight RGB while playback is paused, and again after a
// still PlexUI / library / menu has sat for PAUSE_DIM_IDLE_MIN (default 5).
// Alpha is left alone so the core's blend weight does not change. Resume,
// play, or a new UI frame restores full brightness.
//
// Off: pauseDimWant() is 0 and pauseDimApply() does not walk pixels. The
// FPGA-held picture is not scaled (no RBF). The idle card and the
// play-pending chevron are IDLE_SCREEN / the pending cue, not this dim.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace misterplex {

// 102/255 of straight R, G, B. Readable pause chrome, lower static level.
constexpr int kPauseDimNumer = 102;
constexpr int64_t kPauseDimIdleDefaultMs = 5 * 60 * 1000;

struct PauseDimInput {
    bool paused = false;     // pinned pause bar (UiModel::paused -> Bar::Paused)
    bool staticUi = false;   // PlexUI surface, library page, or menu
    uint64_t stillKey = 0;   // changes when that surface changes
    int64_t nowMs = 0;
};

struct PauseDimClock {
    bool have = false;
    uint64_t key = 0;
    int64_t sinceMs = 0;
};

inline bool pauseDimEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

inline bool pauseDimEnabled() {
#ifdef MUT_PAUSE_DIM_IGNORED
    return false;
#endif
#ifdef MUT_PAUSE_DIM_FORCED
    return true;
#endif
    if (const char* e = std::getenv("MPX_PAUSE_DIM"); e && e[0])
        return pauseDimEnvOn(e);
    return pauseDimEnvOn(std::getenv("PAUSE_DIM"));
}

// PlexUI, the library page, and the menu. The idle card and the play-pending
// chevron stay at full brightness (IDLE_SCREEN / the pending cue).
inline bool pauseDimIsStatic(bool surface, bool libraryOpen, bool menuOpen, bool idle, bool pending) {
#ifdef MUT_PAUSE_DIM_NO_STATIC
    (void)surface;
    (void)libraryOpen;
    (void)menuOpen;
    (void)idle;
    (void)pending;
    return false;
#endif
#ifdef MUT_PAUSE_DIM_IDLE_CARD
    if (idle || pending)
        return true;
#endif
    (void)idle;
    (void)pending;
    return surface || libraryOpen || menuOpen;
}

inline int pauseDimLevel() {
#ifdef MUT_PAUSE_DIM_NO_SCALE
    return 0;
#endif
    return kPauseDimNumer;
}

inline int64_t pauseDimIdleMs() {
#ifdef MUT_PAUSE_DIM_IDLE_OFF
    return 0;
#endif
    const char* e = std::getenv("MPX_PAUSE_DIM_IDLE_MIN");
    if (!e || !e[0])
        e = std::getenv("PAUSE_DIM_IDLE_MIN");
    if (!e || !e[0])
        return kPauseDimIdleDefaultMs;
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || v < 0)
        return kPauseDimIdleDefaultMs;
    if (v > 24 * 60)
        return 24 * 60 * 60 * 1000;
    return static_cast<int64_t>(v) * 60 * 1000;
}

inline int pauseDimFinish(int scale) {
#ifdef MUT_PAUSE_DIM_NO_RESTORE
    static int latched = 0;
    if (scale > 0)
        latched = scale;
    return latched;
#else
    return scale;
#endif
}

// 0 = full brightness. Non-zero = straight-RGB numerator (of 255) to apply.
// Disabled leaves `clock` alone. Enabled and not in the idle wait clears it.
inline int pauseDimCompute(int64_t idleMs, const PauseDimInput& in, PauseDimClock& clock) {
#ifdef MUT_PAUSE_DIM_PLAYING_TOO
    (void)idleMs;
    (void)in;
    clock = {};
    return pauseDimFinish(pauseDimLevel());
#endif
    if (in.paused) {
#ifndef MUT_PAUSE_DIM_NO_PAUSE
        clock = {};
        return pauseDimFinish(pauseDimLevel());
#endif
    }
    if (!in.staticUi || idleMs <= 0) {
        clock = {};
        return pauseDimFinish(0);
    }
#ifdef MUT_PAUSE_DIM_IDLE_NOW
    return pauseDimFinish(pauseDimLevel());
#endif
    if (!clock.have || clock.key != in.stillKey) {
#ifdef MUT_PAUSE_DIM_NO_RESET
        if (!clock.have) {
            clock.have = true;
            clock.key = in.stillKey;
            clock.sinceMs = in.nowMs;
            return pauseDimFinish(0);
        }
#else
        clock.have = true;
        clock.key = in.stillKey;
        clock.sinceMs = in.nowMs;
        return pauseDimFinish(0);
#endif
    }
    if (in.nowMs < clock.sinceMs)
        clock.sinceMs = in.nowMs;
    if (in.nowMs - clock.sinceMs >= idleMs)
        return pauseDimFinish(pauseDimLevel());
    return pauseDimFinish(0);
}

inline int pauseDimWant(const PauseDimInput& in, PauseDimClock& clock) {
    if (!pauseDimEnabled())
        return 0;
    return pauseDimCompute(pauseDimIdleMs(), in, clock);
}

inline uint8_t pauseDimByte(uint8_t c, int scale) {
    if (scale <= 0 || scale >= 255)
        return c;
    return static_cast<uint8_t>((static_cast<unsigned>(c) * static_cast<unsigned>(scale) + 127u) / 255u);
}

// Scale straight B,G,R. Alpha unchanged. scale <= 0 does not touch `bgra`.
inline void pauseDimApply(uint8_t* bgra, std::size_t bytes, int scale) {
    if (!bgra || scale <= 0 || bytes < 4)
        return;
    for (std::size_t i = 0; i + 3 < bytes; i += 4) {
        bgra[i] = pauseDimByte(bgra[i], scale);
        bgra[i + 1] = pauseDimByte(bgra[i + 1], scale);
        bgra[i + 2] = pauseDimByte(bgra[i + 2], scale);
#ifdef MUT_PAUSE_DIM_SCALES_ALPHA
        bgra[i + 3] = pauseDimByte(bgra[i + 3], scale);
#endif
    }
}

} // namespace misterplex
