#pragma once
// AirPlay RAOP Audio "Now Playing" model, DMAP metadata parser, RTP progress calculator,
// and PAR-aware cover-art scaler for 720p, 960x480, 480p/480i, and 240p/288p rasters.
// Header-only and stb-free so host unit tests build it directly; JPEG/PNG decoding lives
// in airplay_cover_load.hpp (which links against plexui_images.cpp's stb_image).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace misterplex {

struct AirplayDmapMeta {
    std::string title;    // minm
    std::string artist;   // asar
    std::string album;    // asal
    std::string genre;    // asgn
    bool any() const { return !title.empty() || !artist.empty() || !album.empty() || !genre.empty(); }
};

namespace airplay_detail {

inline uint32_t readBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline std::string cleanDmapString(const uint8_t* p, size_t n) {
    while (n > 0 && (p[n - 1] == 0 || p[n - 1] == ' ' || p[n - 1] == '\r' || p[n - 1] == '\n' || p[n - 1] == '\t'))
        --n;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\r' || p[i] == '\n' || p[i] == '\t'))
        ++i;
    std::string out;
    out.reserve(n - i);
    for (; i < n; ++i) {
        const uint8_t c = p[i];
        if (c == 0)
            break;
        if (c == '\r' || c == '\n' || c == '\t')
            out.push_back(' ');
        else if (c >= 0x20)
            out.push_back(static_cast<char>(c));
    }
    return out;
}

inline bool isDmapContainerTag(const uint8_t tag[4]) {
    // Standard DMAP/DAAP container tags that wrap track item tags (mlit is what iOS/macOS AirPlay sends).
    static constexpr const char* kContainers[] = {"mlit", "mlcl", "dmap", "mcon", "mdcl"};
    for (const char* c : kContainers)
        if (std::memcmp(tag, c, 4) == 0)
            return true;
    return false;
}

inline void parseDmapChunk(const uint8_t* data, size_t len, int depth, AirplayDmapMeta& out) {
    if (!data || len < 8 || depth > 4)
        return;
    size_t off = 0;
    while (off + 8 <= len) {
        const uint8_t* tag = data + off;
        const uint32_t clen = readBe32(data + off + 4);
        off += 8;
        if (clen > len - off)
            break;
        const uint8_t* val = data + off;
        if (isDmapContainerTag(tag)) {
            parseDmapChunk(val, clen, depth + 1, out);
        } else if (std::memcmp(tag, "minm", 4) == 0) {
            out.title = cleanDmapString(val, clen);
        } else if (std::memcmp(tag, "asar", 4) == 0) {
            out.artist = cleanDmapString(val, clen);
        } else if (std::memcmp(tag, "asal", 4) == 0) {
            out.album = cleanDmapString(val, clen);
        } else if (std::memcmp(tag, "asgn", 4) == 0) {
            out.genre = cleanDmapString(val, clen);
        }
        off += clen;
    }
}

}  // namespace airplay_detail

// Parse Apple RAOP application/x-dmap-tagged payload into title/artist/album/genre.
// Safe against truncated, malformed, or oversized buffers.
inline AirplayDmapMeta parseAirplayDmap(const uint8_t* data, size_t len) {
    AirplayDmapMeta out;
    if (!data || len < 8)
        return out;
    const size_t capped = std::min<size_t>(len, 65536u);
    airplay_detail::parseDmapChunk(data, capped, 0, out);
    return out;
}

struct AirplayProgress {
    bool valid = false;
    int64_t elapsedMs = 0;
    int64_t durationMs = 0;
};

// Convert RAOP RTSP SET_PARAMETER "progress: start/curr/end" RTP sample timestamps into ms.
// Uses unsigned 32-bit modular arithmetic so RTP wraparound works seamlessly.
inline AirplayProgress parseAirplayProgress(uint32_t start, uint32_t curr, uint32_t end,
                                            uint32_t sampleRate = 44100u) {
    AirplayProgress out;
    if (sampleRate == 0)
        return out;
    const uint32_t durTicks = end - start;
    // Reject zero or absurd (> 24 h) durations (e.g. live radio / garbage timestamps).
    const uint64_t maxTicks = uint64_t(sampleRate) * 86400ull;
    if (durTicks == 0 || uint64_t(durTicks) > maxTicks)
        return out;
    const uint32_t elTicks = curr - start;
    uint32_t clampedTicks = elTicks;
    if (elTicks > durTicks) {
        // If curr is slightly before start (pre-roll), clamp to 0; otherwise clamp to end.
        const uint32_t beforeStart = start - curr;
        clampedTicks = beforeStart < 0x40000000u ? 0u : durTicks;
    }
    out.valid = true;
    out.durationMs = static_cast<int64_t>((uint64_t(durTicks) * 1000ull) / sampleRate);
    out.elapsedMs = static_cast<int64_t>((uint64_t(clampedTicks) * 1000ull) / sampleRate);
    return out;
}

// Compact clock for music/audio: "M:SS" under an hour, "H:MM:SS" at or above an hour.
inline std::string airplayClockText(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms) / 1000;
    char b[32];
    if (s >= 3600) {
        std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", static_cast<long long>(s / 3600),
                      static_cast<long long>((s / 60) % 60), static_cast<long long>(s % 60));
    } else {
        std::snprintf(b, sizeof b, "%lld:%02lld", static_cast<long long>(s / 60),
                      static_cast<long long>(s % 60));
    }
    return b;
}

// State carried through UiModel -> UiState for the AirPlay Audio Now Playing screen.
struct AirplayNowPlayingState {
    bool active = false;
    bool paused = false;
    bool fullArt = false;   // pad A / Select toggle: full-screen cover art with no chrome
    std::string codec;      // e.g. "ALAC", "AAC-LC", "AAC-ELD"
    std::string title;
    std::string artist;
    std::string album;
    uint64_t coverGen = 0;  // 0 = no cover art (placeholder); > 0 = AirplayCoverStore generation
    bool progressValid = false;
    int64_t elapsedMs = 0;
    int64_t durationMs = 0;
    int64_t progressAnchorMs = 0;  // steady clock ms when elapsedMs was sampled

    int64_t liveElapsedMs(int64_t nowMs) const {
        if (!progressValid || durationMs <= 0)
            return std::max<int64_t>(0, elapsedMs);
        if (paused || progressAnchorMs <= 0 || nowMs <= progressAnchorMs)
            return std::clamp<int64_t>(elapsedMs, 0, durationMs);
        return std::clamp<int64_t>(elapsedMs + (nowMs - progressAnchorMs), 0, durationMs);
    }

    void clear() {
        active = false;
        paused = false;
        fullArt = false;
        codec.clear();
        title.clear();
        artist.clear();
        album.clear();
        coverGen = 0;
        progressValid = false;
        elapsedMs = 0;
        durationMs = 0;
        progressAnchorMs = 0;
    }
};

struct AirplayCoverImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // straight alpha, w*h*4
    uint8_t avgR = 16, avgG = 18, avgB = 24;
    bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * size_t(h) * 4u; }
};

// Scaled cover art in overlay BGRA8888 order, fitted inside a boxW x boxH frame.
struct AirplayCoverBitmap {
    uint64_t gen = 0;
    int boxW = 0, boxH = 0;
    int offX = 0, offY = 0;   // offset inside the frame (non-zero if source art is non-square)
    int w = 0, h = 0;
    uint8_t avgR = 16, avgG = 18, avgB = 24;
    std::vector<uint8_t> bgra;
    bool ok() const { return w > 0 && h > 0 && bgra.size() == size_t(w) * size_t(h) * 4u; }
};

// Compute average RGB (sampled cleanly across all pixels) for subtle backdrop tinting.
inline void computeAirplayCoverAvgRgb(AirplayCoverImage& im) {
    if (!im.ok())
        return;
    uint64_t sr = 0, sg = 0, sb = 0;
    const size_t n = size_t(im.w) * size_t(im.h);
    const size_t step = n > 16384 ? n / 16384 : 1;
    uint64_t cnt = 0;
    for (size_t i = 0; i < n; i += step) {
        const uint8_t* p = &im.rgba[i * 4u];
        sr += p[0];
        sg += p[1];
        sb += p[2];
        ++cnt;
    }
    if (cnt > 0) {
        im.avgR = uint8_t(sr / cnt);
        im.avgG = uint8_t(sg / cnt);
        im.avgB = uint8_t(sb / cnt);
    }
}

// Downscale an RGBA image so max(w, h) <= maxDim using area averaging. Used on ingest
// to cap ARM RAM usage (e.g. a 3000x3000 iPhone cover -> <= 512x512, <= 1 MiB).
inline AirplayCoverImage airplayDownscaleRgbaToMax(const uint8_t* rgba, int iw, int ih, int maxDim = 512) {
    AirplayCoverImage out;
    if (!rgba || iw <= 0 || ih <= 0 || maxDim <= 0)
        return out;
    if (iw <= maxDim && ih <= maxDim) {
        out.w = iw;
        out.h = ih;
        out.rgba.assign(rgba, rgba + size_t(iw) * size_t(ih) * 4u);
        computeAirplayCoverAvgRgb(out);
        return out;
    }
    const double s = double(maxDim) / double(std::max(iw, ih));
    const int ow = std::max(1, int(std::lround(double(iw) * s)));
    const int oh = std::max(1, int(std::lround(double(ih) * s)));
    out.w = ow;
    out.h = oh;
    out.rgba.assign(size_t(ow) * size_t(oh) * 4u, 0);
    for (int y = 0; y < oh; ++y) {
        const double sy0 = double(y) * ih / oh, sy1 = double(y + 1) * ih / oh;
        const int iy0 = int(sy0), iy1 = std::min(ih, int(std::ceil(sy1)));
        for (int x = 0; x < ow; ++x) {
            const double sx0 = double(x) * iw / ow, sx1 = double(x + 1) * iw / ow;
            const int ix0 = int(sx0), ix1 = std::min(iw, int(std::ceil(sx1)));
            double r = 0, g = 0, b = 0, wsum = 0;
            for (int iy = iy0; iy < iy1; ++iy) {
                const double wy = std::min(sy1, double(iy + 1)) - std::max(sy0, double(iy));
                for (int ix = ix0; ix < ix1; ++ix) {
                    const double wx = std::min(sx1, double(ix + 1)) - std::max(sx0, double(ix));
                    const double wgt = wx * wy;
                    const uint8_t* p = &rgba[(size_t(iy) * size_t(iw) + size_t(ix)) * 4u];
                    r += p[0] * wgt;
                    g += p[1] * wgt;
                    b += p[2] * wgt;
                    wsum += wgt;
                }
            }
            if (wsum <= 0.0)
                wsum = 1.0;
            uint8_t* d = &out.rgba[(size_t(y) * size_t(ow) + size_t(x)) * 4u];
            d[0] = uint8_t(std::lround(r / wsum));
            d[1] = uint8_t(std::lround(g / wsum));
            d[2] = uint8_t(std::lround(b / wsum));
            d[3] = 0xFF;
        }
    }
    computeAirplayCoverAvgRgb(out);
    return out;
}

// Fit im into a boxW x boxH overlay frame whose pixels have pixel aspect ratio `par`
// (par = display_aspect * H / W; 1.0 = square pixels, ~0.889 on 4:3 720x480, ~0.444 on 4:3 720x240).
// Area-averages when downscaling, bilinearly interpolates when upscaling, snaps even lines on
// interlaced modes, and applies a 3x3 unsharp mask when sharpen240p is true (H <= 288).
inline AirplayCoverBitmap scaleAirplayCoverArt(const AirplayCoverImage& im, int boxW, int boxH, double par,
                                               bool even, bool sharpen240p, uint64_t gen = 0) {
    AirplayCoverBitmap out;
    out.gen = gen;
    out.boxW = boxW;
    out.boxH = boxH;
    out.avgR = im.avgR;
    out.avgG = im.avgG;
    out.avgB = im.avgB;
    if (!im.ok() || boxW <= 0 || boxH <= 0)
        return out;
    if (par <= 0.01)
        par = 1.0;

    // Fit im.w x im.h (square source pixels) inside visual box (boxW * par) x boxH.
    const double s = std::min((double(boxW) * par) / double(im.w), double(boxH) / double(im.h));
    int ow = std::max(1, std::min(boxW, int(std::lround(double(im.w) * s / par))));
    int oh = std::max(1, std::min(boxH, int(std::lround(double(im.h) * s))));
    if (even)
        oh = std::max(2, oh & ~1);
    out.w = ow;
    out.h = oh;
    out.offX = (boxW - ow) / 2;
    out.offY = (boxH - oh) / 2;
    if (even)
        out.offY &= ~1;
    out.bgra.assign(size_t(ow) * size_t(oh) * 4u, 0);

    const bool upscale = (ow > im.w || oh > im.h);
    if (!upscale) {
        // Area-average downscale: alias-free across 720p, 960x480, 480p/i, and 240p.
        for (int y = 0; y < oh; ++y) {
            const double sy0 = double(y) * im.h / oh, sy1 = double(y + 1) * im.h / oh;
            const int iy0 = int(sy0), iy1 = std::min(im.h, int(std::ceil(sy1)));
            for (int x = 0; x < ow; ++x) {
                const double sx0 = double(x) * im.w / ow, sx1 = double(x + 1) * im.w / ow;
                const int ix0 = int(sx0), ix1 = std::min(im.w, int(std::ceil(sx1)));
                double r = 0, g = 0, b = 0, wsum = 0;
                for (int iy = iy0; iy < iy1; ++iy) {
                    const double wy = std::min(sy1, double(iy + 1)) - std::max(sy0, double(iy));
                    for (int ix = ix0; ix < ix1; ++ix) {
                        const double wx = std::min(sx1, double(ix + 1)) - std::max(sx0, double(ix));
                        const double wgt = wx * wy;
                        const uint8_t* p = &im.rgba[(size_t(iy) * size_t(im.w) + size_t(ix)) * 4u];
                        r += p[0] * wgt;
                        g += p[1] * wgt;
                        b += p[2] * wgt;
                        wsum += wgt;
                    }
                }
                if (wsum <= 0.0)
                    wsum = 1.0;
                uint8_t* d = &out.bgra[(size_t(y) * size_t(ow) + size_t(x)) * 4u];
                d[0] = uint8_t(std::lround(b / wsum));
                d[1] = uint8_t(std::lround(g / wsum));
                d[2] = uint8_t(std::lround(r / wsum));
                d[3] = 0xFF;
            }
        }
    } else {
        // Bilinear sample when upscaling small album thumbnails so edges stay smooth.
        for (int y = 0; y < oh; ++y) {
            const double fy = ((double(y) + 0.5) * im.h / oh) - 0.5;
            const int y0 = std::clamp(int(std::floor(fy)), 0, im.h - 1);
            const int y1 = std::min(im.h - 1, y0 + 1);
            const double ty = std::clamp(fy - y0, 0.0, 1.0);
            for (int x = 0; x < ow; ++x) {
                const double fx = ((double(x) + 0.5) * im.w / ow) - 0.5;
                const int x0 = std::clamp(int(std::floor(fx)), 0, im.w - 1);
                const int x1 = std::min(im.w - 1, x0 + 1);
                const double tx = std::clamp(fx - x0, 0.0, 1.0);
                const uint8_t* p00 = &im.rgba[(size_t(y0) * size_t(im.w) + size_t(x0)) * 4u];
                const uint8_t* p10 = &im.rgba[(size_t(y0) * size_t(im.w) + size_t(x1)) * 4u];
                const uint8_t* p01 = &im.rgba[(size_t(y1) * size_t(im.w) + size_t(x0)) * 4u];
                const uint8_t* p11 = &im.rgba[(size_t(y1) * size_t(im.w) + size_t(x1)) * 4u];
                auto lerp = [&](int c) -> uint8_t {
                    const double top = p00[c] * (1.0 - tx) + p10[c] * tx;
                    const double bot = p01[c] * (1.0 - tx) + p11[c] * tx;
                    return uint8_t(std::clamp(int(std::lround(top * (1.0 - ty) + bot * ty)), 0, 255));
                };
                uint8_t* d = &out.bgra[(size_t(y) * size_t(ow) + size_t(x)) * 4u];
                d[0] = lerp(2);   // B
                d[1] = lerp(1);   // G
                d[2] = lerp(0);   // R
                d[3] = 0xFF;
            }
        }
    }

    // 240p / 288p CRT & S-Video: crisp unsharp mask so small album cover details hold up.
    if (sharpen240p && ow >= 3 && oh >= 3) {
        std::vector<uint8_t> src = out.bgra;
        for (int y = 1; y < oh - 1; ++y) {
            for (int x = 1; x < ow - 1; ++x) {
                for (int c = 0; c < 3; ++c) {
                    int sum = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            sum += src[(size_t(y + dy) * size_t(ow) + size_t(x + dx)) * 4u + size_t(c)];
                    const int blur = sum / 9;
                    const int v = src[(size_t(y) * size_t(ow) + size_t(x)) * 4u + size_t(c)];
                    out.bgra[(size_t(y) * size_t(ow) + size_t(x)) * 4u + size_t(c)] =
                        uint8_t(std::clamp(v + ((v - blur) * 3) / 5, 0, 255));
                }
            }
        }
    }
    return out;
}

// Process-wide thread-safe cover art store + one-entry scaled bitmap cache (mirrors AdCardStore).
class AirplayCoverStore {
public:
    static AirplayCoverStore& get() {
        static AirplayCoverStore s;
        return s;
    }

    uint64_t set(AirplayCoverImage im) {
        std::lock_guard<std::mutex> g(mu_);
        if (!im.ok()) {
            img_.reset();
            cache_.reset();
            gen_ = 0;
            return 0;
        }
        img_ = std::make_shared<const AirplayCoverImage>(std::move(im));
        cache_.reset();
        gen_ = ++nextGen_;
        return gen_;
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        img_.reset();
        cache_.reset();
        gen_ = 0;
    }

    uint64_t generation() const {
        std::lock_guard<std::mutex> g(mu_);
        return gen_;
    }

    std::shared_ptr<const AirplayCoverBitmap> bitmapFor(int boxW, int boxH, double par, bool even,
                                                        bool sharpen240p) {
        std::lock_guard<std::mutex> g(mu_);
        if (!img_ || !img_->ok() || gen_ == 0)
            return nullptr;
        if (cache_ && cache_->gen == gen_ && cache_->boxW == boxW && cache_->boxH == boxH &&
            cachePar_ == par && cacheEven_ == even && cacheSharpen_ == sharpen240p) {
            return cache_;
        }
        cache_ = std::make_shared<const AirplayCoverBitmap>(
            scaleAirplayCoverArt(*img_, boxW, boxH, par, even, sharpen240p, gen_));
        cachePar_ = par;
        cacheEven_ = even;
        cacheSharpen_ = sharpen240p;
        return cache_;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const AirplayCoverImage> img_;
    std::shared_ptr<const AirplayCoverBitmap> cache_;
    uint64_t nextGen_ = 0;
    uint64_t gen_ = 0;
    double cachePar_ = 1.0;
    bool cacheEven_ = false;
    bool cacheSharpen_ = false;
};

}  // namespace misterplex
