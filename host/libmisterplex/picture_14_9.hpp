#pragma once
// E7 14:9 picture mode for a wide title on a 4:3 tube.
//
// The PlexOB core's Analog Aspect menu is two bits: Auto, Fit, Letterbox, Crop
// (h264ring.h H264RING_OPT_ANALOG_ASPECT). disp_fit is inside the RBF and is not
// given a fifth mode here. Letterbox already fits the SPS display window into the
// 4:3 raster, and that window is the SPS frame_crop. A 14:9 compromise is that
// window narrowed to DAR 14:9 (about 6.25% off each side of 16:9) so Letterbox
// keeps smaller bars than a full 16:9 letterbox and more picture than Crop.
//
// PICTURE_14_9 / MPX_PICTURE_14_9 (env wins). Unset, off, 0, or anything else
// leaves the bitstream and the Analog Aspect guard on the tip path. On
// (on, 1, true, yes, 14:9, 14x9) rewrites in-band SPS crops for a fitted
// output (480i or 480p, not Native HDMI). The Program menu, only while that
// flag is on and a title is playing, toggles this play between 14:9 and full.
// A menu of Fit or Crop is asked for Letterbox for that play when a crop was
// actually applied, and the user's value is restored at stop. Auto on 480i
// already letterboxes in the core, so it is not rewritten.

#include "h264_sps.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace misterplex {

inline bool picture149EnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n) {
        unsigned char c = static_cast<unsigned char>(e[n]);
        buf[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
    }
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0 || std::strcmp(buf, "14:9") == 0 || std::strcmp(buf, "14x9") == 0 ||
           std::strcmp(buf, "14-9") == 0 || std::strcmp(buf, "149") == 0;
}

// Default off. MPX_PICTURE_14_9 wins over PICTURE_14_9 when it is set.
inline bool picture149Enabled() {
#ifdef MUT_PICTURE149_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_PICTURE_14_9"); e && e[0])
        return picture149EnvOn(e);
    return picture149EnvOn(std::getenv("PICTURE_14_9"));
#endif
}

// Per-play choice. -1 follows the flag (on means 14:9), 0 is full, 1 is 14:9
// even when the flag is off (a remembered 14:9). 0 stays full when the flag is on.
inline std::atomic<int>& picture149Choice() {
    static std::atomic<int> c{-1};
    return c;
}
inline std::atomic<int>& picture149VoutSlot() {
    static std::atomic<int> v{0};
    return v;
}
inline std::atomic<bool>& picture149AppliedSlot() {
    static std::atomic<bool> a{false};
    return a;
}

inline void picture149BeginPlay() {
    picture149Choice().store(-1);
    picture149VoutSlot().store(0);
    picture149AppliedSlot().store(false);
}

inline bool picture149Active() {
    const int c = picture149Choice().load();
    if (c == 0)
        return false;
    if (c == 1)
        return true;
    return picture149Enabled();
}

inline void picture149TogglePlay() {
    if (!picture149Enabled())
        return;
    picture149Choice().store(picture149Active() ? 0 : 1);
}

// Resolved Video Output: 1 interlaced, 2 progressive, 3 native (h264ring.h).
inline void picture149NoteVout(int videoOut) {
    if (videoOut > 0)
        picture149VoutSlot().store(videoOut);
}

inline bool picture149FittedOutput() {
    const int v = picture149VoutSlot().load();
    return v == 1 || v == 2;
}

inline void picture149NoteApplied() { picture149AppliedSlot().store(true); }

inline bool picture149CropApplied() { return picture149AppliedSlot().load(); }

// Fit (1) or Crop (3) on a 4:3 raster would not show the 14:9 compromise.
// Auto and Letterbox already letterbox there. Native is not a fitted raster.
inline bool picture149ForcesLetterbox(bool applied, unsigned userAa, unsigned videoOut) {
#ifdef MUT_PICTURE149_FORCE_LB_OFF
    (void)applied;
    (void)userAa;
    (void)videoOut;
    return false;
#else
    return applied && (videoOut == 1u || videoOut == 2u) && (userAa == 1u || userAa == 3u);
#endif
}

struct Picture149Window {
    bool apply = false;
    int lumaLeft = 0;
    int lumaRight = 0;
    int targetW = 0;
};

// Side crop so (dispW - left - right) : dispH, times SAR, is 14:9.
// Narrower than 14:9, or already within a couple of luma samples, is left alone.
inline Picture149Window picture149Window(int dispW, int dispH, int sarW, int sarH) {
#ifdef MUT_PICTURE149_NO_CROP
    (void)dispW;
    (void)dispH;
    (void)sarW;
    (void)sarH;
    return {};
#else
    Picture149Window out;
    if (dispW <= 0 || dispH <= 0 || (dispW & 1) || (dispH & 1) || sarW <= 0 || sarH <= 0)
        return out;
#ifdef MUT_PICTURE149_IGNORE_SAR
    sarW = 1;
    sarH = 1;
#endif
    const int64_t num = static_cast<int64_t>(dispH) * sarH * 14;
    const int64_t den = static_cast<int64_t>(sarW) * 9;
    if (den <= 0)
        return out;
    int64_t tw = num / den;
    if (tw & 1)
        --tw;
    const bool wide = tw >= 16 && tw < dispW;
    const int extra = wide ? dispW - static_cast<int>(tw) : 0;
    if (wide && extra >= 4) {
        const int left = (extra / 2) & ~1;
        const int right = extra - left;
        if (left >= 0 && right >= 0 && ((left | right) & 1) == 0 && left + right == extra) {
            out.apply = true;
            out.lumaLeft = left;
            out.lumaRight = right;
            out.targetW = static_cast<int>(tw);
        }
    }
#ifdef MUT_PICTURE149_CROP_ALL
    if (!out.apply && dispW >= 32 && dispH >= 16) {
        out.apply = true;
        out.lumaLeft = 8;
        out.lumaRight = 8;
        out.targetW = dispW - 16;
    }
#endif
    return out;
#endif
}

// H.264 Table E-1. Unspecified and reserved idc are square, which is how the core reads them.
inline bool picture149Sar(int idc, int extW, int extH, int& sarW, int& sarH) {
    static const int kTab[17][2] = {
        {1, 1},   {1, 1},   {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11},
        {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3},  {3, 2},   {2, 1},
    };
    if (idc == 255) {
        if (extW <= 0 || extH <= 0)
            return false;
        sarW = extW;
        sarH = extH;
        return true;
    }
    if (idc <= 0 || idc > 16) {
        sarW = 1;
        sarH = 1;
        return true;
    }
    sarW = kTab[idc][0];
    sarH = kTab[idc][1];
    return true;
}

namespace picture149_detail {

struct Br {
    const uint8_t* d = nullptr;
    size_t n = 0;
    size_t bit = 0;
    bool ok = true;
    int u1() {
        if (!ok || (bit / 8) >= n) {
            ok = false;
            return 0;
        }
        const int bi = 7 - static_cast<int>(bit % 8);
        const int v = (d[bit / 8] >> bi) & 1;
        ++bit;
        return v;
    }
    uint32_t u(int nbits) {
        uint32_t v = 0;
        for (int i = 0; i < nbits; ++i)
            v = (v << 1) | static_cast<uint32_t>(u1());
        return v;
    }
    uint32_t ue() {
        int z = 0;
        while (ok && u1() == 0)
            ++z;
        if (!ok)
            return 0;
        if (z == 0)
            return 0;
        if (z > 24) {
            ok = false;
            return 0;
        }
        return (1u << z) - 1u + u(z);
    }
};

struct Bw {
    std::vector<uint8_t> b;
    int n = 0;
    void u(unsigned v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if (n % 8 == 0)
                b.push_back(0);
            if ((v >> i) & 1u)
                b.back() = static_cast<uint8_t>(b.back() | (0x80u >> (n % 8)));
            ++n;
        }
    }
    void ue(unsigned v) {
        const unsigned x = v + 1;
        int len = 0;
        while ((x >> len) > 1)
            ++len;
        if (len)
            u(0, len);
        u(x, len + 1);
    }
    void copyBits(const uint8_t* d, size_t nbytes, size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            const size_t by = i / 8;
            if (by >= nbytes)
                break;
            const int bi = 7 - static_cast<int>(i % 8);
            u(static_cast<unsigned>((d[by] >> bi) & 1), 1);
        }
    }
    std::vector<uint8_t> take() {
        while (n % 8)
            u(0, 1);
        return b;
    }
};

struct CropMap {
    bool ok = false;
    bool frameOnly = false;
    bool chroma420 = true;
    size_t cropFlagBit = 0;
    size_t afterCropBit = 0;
    uint32_t cropL = 0, cropR = 0, cropT = 0, cropB = 0;
    int dispW = 0, dispH = 0;
};

inline std::vector<uint8_t> addEpb(const uint8_t* p, size_t n) {
    std::vector<uint8_t> o;
    o.reserve(n + 8);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && p[i] <= 3) {
            o.push_back(3);
            zeros = 0;
        }
        o.push_back(p[i]);
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

// Bit position of frame_crop, plus the display size the same way parseSpsRbsp measures it.
// A scaling matrix is refused: a wrong skip would move the VUI and the core would reject the SPS.
inline CropMap walkSps(const std::vector<uint8_t>& rbsp) {
    CropMap m;
    Br br{rbsp.data(), rbsp.size()};
    const uint32_t profile = br.u(8);
    br.u(8);
    br.u(8);
    br.ue();
    const bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
                      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
                      profile == 139 || profile == 134 || profile == 135;
    if (high) {
        const uint32_t chroma = br.ue();
        if (chroma == 3)
            br.u(1);
        br.ue();
        br.ue();
        br.u(1);
        if (br.u(1))
            return m;
        m.chroma420 = chroma == 1;
    }
    if (!m.chroma420 || !br.ok)
        return m;
    br.ue();
    const uint32_t poc = br.ue();
    if (poc == 0) {
        br.ue();
    } else if (poc == 1) {
        br.u(1);
        br.ue();
        br.ue();
        const uint32_t n = br.ue();
        for (uint32_t i = 0; i < n && br.ok; ++i)
            br.ue();
    } else if (poc > 2) {
        return m;
    }
    br.ue();
    br.u(1);
    const uint32_t wMbs = br.ue() + 1;
    const uint32_t hMap = br.ue() + 1;
    const uint32_t frameOnly = br.u(1);
    m.frameOnly = frameOnly == 1;
    if (!frameOnly)
        br.u(1);
    br.u(1);
    if (!br.ok || !m.frameOnly)
        return m;
    m.cropFlagBit = br.bit;
    const uint32_t crop = br.u(1);
    if (crop) {
        m.cropL = br.ue();
        m.cropR = br.ue();
        m.cropT = br.ue();
        m.cropB = br.ue();
    }
    m.afterCropBit = br.bit;
    if (!br.ok)
        return m;
    int w = static_cast<int>(wMbs * 16u);
    int h = static_cast<int>(hMap * 16u);
    w -= static_cast<int>(m.cropL + m.cropR) * 2;
    h -= static_cast<int>(m.cropT + m.cropB) * 2;
    if (w <= 0 || h <= 0)
        return m;
    m.dispW = w;
    m.dispH = h;
    m.ok = true;
    return m;
}

inline bool rewriteSpsPayload(const uint8_t* payload, size_t len, std::vector<uint8_t>& out) {
    const SpsInfo info = parseSpsRbsp(payload, len);
    if (!info.valid)
        return false;
    const std::vector<uint8_t> rbsp = detail::removeEpb(payload, len);
    const CropMap map = walkSps(rbsp);
    if (!map.ok || map.dispW != info.width || map.dispH != info.height)
        return false;
    int sarW = 1, sarH = 1;
    if (info.hasAspect() && !picture149Sar(info.aspectIdc, info.sarW, info.sarH, sarW, sarH))
        return false;
    const Picture149Window win = picture149Window(map.dispW, map.dispH, sarW, sarH);
    if (!win.apply)
        return false;
    const int addL = win.lumaLeft / 2;
    const int addR = win.lumaRight / 2;
    if (addL < 0 || addR < 0 || (win.lumaLeft & 1) || (win.lumaRight & 1))
        return false;
    const uint32_t nL = map.cropL + static_cast<uint32_t>(addL);
    const uint32_t nR = map.cropR + static_cast<uint32_t>(addR);
    if (nL < map.cropL || nR < map.cropR)
        return false;
    Bw w;
    w.copyBits(rbsp.data(), rbsp.size(), 0, map.cropFlagBit);
    w.u(1, 1);
    w.ue(nL);
    w.ue(nR);
    w.ue(map.cropT);
    w.ue(map.cropB);
    w.copyBits(rbsp.data(), rbsp.size(), map.afterCropBit, rbsp.size() * 8);
    const std::vector<uint8_t> raw = w.take();
    if (raw.empty() || raw.size() > 4096)
        return false;
    std::vector<uint8_t> nal = addEpb(raw.data(), raw.size());
    const SpsInfo check = parseSpsRbsp(nal.data(), nal.size());
    if (!check.valid || check.width != win.targetW || check.height != info.height)
        return false;
    if (check.profile_idc != info.profile_idc || check.level_idc != info.level_idc)
        return false;
    if (check.vuiPresent != info.vuiPresent || check.aspectInfo != info.aspectInfo || check.aspectIdc != info.aspectIdc ||
        check.sarW != info.sarW || check.sarH != info.sarH)
        return false;
    out.swap(nal);
    return true;
}

} // namespace picture149_detail

// Rewrite every SPS NAL in an Annex-B access unit. False leaves `au` unchanged.
inline bool applyPicture149AnnexB(std::vector<uint8_t>& au) {
#ifdef MUT_PICTURE149_SPS_KEEP
    (void)au;
    return false;
#else
    if (au.size() < 5)
        return false;
    struct Nal {
        size_t sc = 0, hdr = 0, end = 0;
    };
    std::vector<Nal> nals;
    size_t i = 0;
    while (i + 3 < au.size()) {
        size_t sc = 0;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && i + 3 < au.size() && au[i + 3] == 1)
            sc = 4;
        else if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
            sc = 3;
        else {
            ++i;
            continue;
        }
        const size_t hdr = i + sc;
        size_t j = hdr + 1;
        while (j + 2 < au.size()) {
            if (au[j] == 0 && au[j + 1] == 0 &&
                (au[j + 2] == 1 || (j + 3 < au.size() && au[j + 2] == 0 && au[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 2 >= au.size())
            j = au.size();
        nals.push_back(Nal{i, hdr, j});
        i = j;
    }
    if (nals.empty())
        return false;
    std::vector<uint8_t> out;
    out.reserve(au.size() + 32);
    size_t prev = 0;
    bool changed = false;
    for (const Nal& nal : nals) {
        if (nal.hdr >= au.size())
            continue;
        out.insert(out.end(), au.begin() + static_cast<std::ptrdiff_t>(prev),
                   au.begin() + static_cast<std::ptrdiff_t>(nal.hdr + 1));
        const bool sps = (au[nal.hdr] & 0x1f) == 7 && nal.hdr + 1 < nal.end;
        std::vector<uint8_t> rewritten;
        if (sps && picture149_detail::rewriteSpsPayload(au.data() + nal.hdr + 1, nal.end - (nal.hdr + 1), rewritten)) {
            out.insert(out.end(), rewritten.begin(), rewritten.end());
            changed = true;
        } else if (nal.hdr + 1 < nal.end) {
            out.insert(out.end(), au.begin() + static_cast<std::ptrdiff_t>(nal.hdr + 1),
                       au.begin() + static_cast<std::ptrdiff_t>(nal.end));
        }
        prev = nal.end;
    }
    if (prev < au.size())
        out.insert(out.end(), au.begin() + static_cast<std::ptrdiff_t>(prev), au.end());
    if (!changed)
        return false;
    au.swap(out);
    return true;
#endif
}

// Active flag and a fitted raster. Records that a crop went out so the aspect guard can
// ask for Letterbox when the menu is Fit or Crop.
inline bool maybeApplyPicture149(std::vector<uint8_t>& au) {
    if (!picture149Active() || !picture149FittedOutput())
        return false;
    if (!applyPicture149AnnexB(au))
        return false;
    picture149NoteApplied();
    return true;
}

} // namespace misterplex
