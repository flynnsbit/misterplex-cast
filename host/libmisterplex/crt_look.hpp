#pragma once
// FEATURE-GAP E28: HDMI CRT-look docs/presets.
//
// CRT_LOOK (misterplex.conf) / MPX_CRT_LOOK (env wins). Default off.
// Unset, empty, 0, off, false, and no stay off.
//
// Off: applyCrtLook writes nothing. The on-device UI canvas matches a
// build without this flag. The FPGA picture, analog raster, 14:9 crop,
// and overscan insets are not this function.
// On: the finished plexui frame (home, library, detail, and the other
// on-device pages) is the preset named by MPX_CRT_LOOK_PRESET, else
// CRT_LOOK_PRESET, else "scanlines". Unknown names are "scanlines".
//
// Presets (progressive HDMI UI rows, top row is y=0 and even):
//   scanlines       odd rows at 1/2. Even rows unchanged. No mask.
//   scanlines-soft  odd rows at 3/4. Even rows unchanged. No mask.
//   aperture        RGB grille, period 3. Other channels at 1/4, then
//                   odd rows at 3/4.
//   slot            Slot mask, period 6 (two pixels per color). Other
//                   channels at 1/4, then odd rows at 1/2.
// Alpha is left alone. This is not the FPGA shadowmask and not an RBF.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace misterplex {

enum class CrtLookPreset : int {
    Scanlines = 1,
    ScanlinesSoft = 2,
    Aperture = 3,
    Slot = 4,
};

struct CrtLookPresetInfo {
    CrtLookPreset id;
    const char* name;
    int scanNumer;
    int scanDenom;
    int mask; // 0 none, 3 aperture period, 6 slot period
    const char* summary;
};

inline constexpr CrtLookPresetInfo kCrtLookPresets[] = {
    {CrtLookPreset::Scanlines, "scanlines", 1, 2, 0,
     "Odd rows at 1/2. Even rows unchanged. No mask."},
    {CrtLookPreset::ScanlinesSoft, "scanlines-soft", 3, 4, 0,
     "Odd rows at 3/4. Even rows unchanged. No mask."},
    {CrtLookPreset::Aperture, "aperture", 3, 4, 3,
     "RGB aperture grille, period 3. Other channels at 1/4. Odd rows then at 3/4."},
    {CrtLookPreset::Slot, "slot", 1, 2, 6,
     "Slot mask, period 6 (two pixels per color). Other channels at 1/4. Odd rows then at 1/2."},
};

inline bool crtLookEnvOn(const char* e) {
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

// Default off. MPX_CRT_LOOK wins over CRT_LOOK when it is set.
inline bool crtLookEnabled() {
#ifdef MUT_CRTLOOK_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_CRT_LOOK"); e && e[0])
        return crtLookEnvOn(e);
    return crtLookEnvOn(std::getenv("CRT_LOOK"));
#endif
}

inline CrtLookPreset crtLookPresetFromName(const char* e) {
    if (!e || !e[0])
        return CrtLookPreset::Scanlines;
    char buf[32];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    if (e[n])
        return CrtLookPreset::Scanlines;
    for (const CrtLookPresetInfo& p : kCrtLookPresets) {
        if (std::strcmp(buf, p.name) == 0)
            return p.id;
    }
    return CrtLookPreset::Scanlines;
}

// The preset a turned-on flag would paint. Ignored while the flag is off.
inline CrtLookPreset crtLookSelected() {
#ifdef MUT_CRTLOOK_IGNORE_PRESET
    return CrtLookPreset::Scanlines;
#else
    const char* e = std::getenv("MPX_CRT_LOOK_PRESET");
    if (!e || !e[0])
        e = std::getenv("CRT_LOOK_PRESET");
    return crtLookPresetFromName(e);
#endif
}

inline const CrtLookPresetInfo* crtLookPresetInfo(CrtLookPreset id) {
    for (const CrtLookPresetInfo& p : kCrtLookPresets) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

inline const char* crtLookPresetName(CrtLookPreset id) {
    if (const CrtLookPresetInfo* p = crtLookPresetInfo(id))
        return p->name;
    return "scanlines";
}

inline int crtLookByteOf(int logical, bool bgr) {
    if (!bgr)
        return logical;
    if (logical == 0)
        return 2;
    if (logical == 2)
        return 0;
    return 1;
}

// False, and no store, when the flag is off, the pointer is empty, or the
// size is not a 3- or 4-byte canvas inside 8192. True when a byte changes.
inline bool applyCrtLook(std::uint8_t* px, int w, int h, int bpp, bool bgr) {
#ifndef MUT_CRTLOOK_APPLY_WHEN_OFF
    if (!crtLookEnabled()) {
#ifdef MUT_CRTLOOK_PAINT_FIRST
        if (px && w > 0 && h > 0 && (bpp == 3 || bpp == 4))
            px[0] = static_cast<std::uint8_t>(px[0] ^ 0xFFu);
#endif
        return false;
    }
#endif
#ifdef MUT_CRTLOOK_SKIP_WHEN_ON
    (void)px;
    (void)w;
    (void)h;
    (void)bpp;
    (void)bgr;
    return false;
#else
    if (!px || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (bpp != 3 && bpp != 4))
        return false;
    const CrtLookPresetInfo* info = crtLookPresetInfo(crtLookSelected());
    if (!info || info->scanDenom <= 0)
        return false;
    bool changed = false;
    for (int y = 0; y < h; ++y) {
#ifdef MUT_CRTLOOK_EVEN_LINES
        const bool scanRow = (y & 1) == 0;
#else
        const bool scanRow = (y & 1) == 1;
#endif
        std::uint8_t* row = px + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) *
                                      static_cast<std::size_t>(bpp);
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(bpp);
            int primary = -1;
            if (info->mask == 3)
                primary = x % 3;
            else if (info->mask == 6)
                primary = (x / 2) % 3;
            for (int logical = 0; logical < 3; ++logical) {
                const int bi = crtLookByteOf(logical, bgr);
                int v = p[bi];
                if (primary >= 0 && logical != primary)
                    v = v / 4;
                if (scanRow)
                    v = (v * info->scanNumer) / info->scanDenom;
                if (v < 0)
                    v = 0;
                if (v > 255)
                    v = 255;
                const auto out = static_cast<std::uint8_t>(v);
                if (p[bi] != out) {
                    p[bi] = out;
                    changed = true;
                }
            }
        }
    }
    return changed;
#endif
}

} // namespace misterplex
