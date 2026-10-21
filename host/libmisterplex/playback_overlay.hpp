#pragma once
// Playback overlay renderer and API contract for input/transport workers.
//
// MediaPlayer exposes:
//   showPlaybackOverlay(PlaybackOverlayState state, positionMs, durationMs)
//     Show the on-screen transport overlay for a few seconds without changing
//     playback. Call this after play/pause/resume/stop or any control touch.
//   flashPlaybackSkip(deltaMs)
//     Show transient skip feedback ("30s >>" or "<< 30s") and refresh the
//     overlay timeout. Transport dispatch owns the actual seek/skip.
//
// The renderer is buffer-format agnostic. It draws into packed RGB formats or
// directly into planar I420 and only touches the overlay dirty region; when
// hidden, render*() returns false without scanning the frame.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace misterplex {

enum class PlaybackOverlayState {
    Playing,
    Paused,
    Stopped,
};

struct OverlayRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
};

inline OverlayRect alignI420DirtyRect(OverlayRect r, int w, int h) {
    if (r.empty() || w <= 0 || h <= 0)
        return {};
    int x0 = std::max(0, std::min(w, r.x));
    int y0 = std::max(0, std::min(h, r.y));
    int x1 = std::max(0, std::min(w, r.x + r.w));
    int y1 = std::max(0, std::min(h, r.y + r.h));
    x0 &= ~1;
    y0 &= ~1;
    x1 = std::min(w, (x1 + 1) & ~1);
    y1 = std::min(h, (y1 + 1) & ~1);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return OverlayRect{x0, y0, x1 - x0, y1 - y0};
}

struct I420DirtyBackup {
    OverlayRect rect;
    int frameWidth = 0;
    int frameHeight = 0;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

    void clear() {
        rect = {};
        frameWidth = 0;
        frameHeight = 0;
        y.clear();
        u.clear();
        v.clear();
    }

    bool empty() const { return rect.empty(); }

    bool capture(const uint8_t* i420, int w, int h, OverlayRect requested) {
        clear();
        if (!i420 || w <= 0 || h <= 0 || (w & 1) || (h & 1))
            return false;
        rect = alignI420DirtyRect(requested, w, h);
        frameWidth = w;
        frameHeight = h;
        if (rect.empty())
            return true;

        const size_t yRowBytes = static_cast<size_t>(rect.w);
        const size_t cRowBytes = static_cast<size_t>(rect.w / 2);
        y.resize(yRowBytes * static_cast<size_t>(rect.h));
        u.resize(cRowBytes * static_cast<size_t>(rect.h / 2));
        v.resize(cRowBytes * static_cast<size_t>(rect.h / 2));

        const size_t yPlaneBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
        const size_t cPlaneBytes = static_cast<size_t>(w / 2) * static_cast<size_t>(h / 2);
        const uint8_t* yPlane = i420;
        const uint8_t* uPlane = yPlane + yPlaneBytes;
        const uint8_t* vPlane = uPlane + cPlaneBytes;
        for (int yy = 0; yy < rect.h; ++yy) {
            const size_t src =
                static_cast<size_t>(rect.y + yy) * static_cast<size_t>(w) + rect.x;
            std::memcpy(y.data() + static_cast<size_t>(yy) * yRowBytes, yPlane + src,
                        yRowBytes);
        }
        const int cStride = w / 2;
        const int cx = rect.x / 2;
        const int cy = rect.y / 2;
        for (int yy = 0; yy < rect.h / 2; ++yy) {
            const size_t src =
                static_cast<size_t>(cy + yy) * static_cast<size_t>(cStride) + cx;
            const size_t dst = static_cast<size_t>(yy) * cRowBytes;
            std::memcpy(u.data() + dst, uPlane + src, cRowBytes);
            std::memcpy(v.data() + dst, vPlane + src, cRowBytes);
        }
        return true;
    }

    bool restore(uint8_t* i420, int w, int h) const {
        if (!i420 || w != frameWidth || h != frameHeight || w <= 0 || h <= 0 ||
            (w & 1) || (h & 1))
            return false;
        if (rect.empty())
            return y.empty() && u.empty() && v.empty();

        const size_t yRowBytes = static_cast<size_t>(rect.w);
        const size_t cRowBytes = static_cast<size_t>(rect.w / 2);
        if (y.size() != yRowBytes * static_cast<size_t>(rect.h) ||
            u.size() != cRowBytes * static_cast<size_t>(rect.h / 2) ||
            v.size() != cRowBytes * static_cast<size_t>(rect.h / 2))
            return false;

        const size_t yPlaneBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
        const size_t cPlaneBytes = static_cast<size_t>(w / 2) * static_cast<size_t>(h / 2);
        uint8_t* yPlane = i420;
        uint8_t* uPlane = yPlane + yPlaneBytes;
        uint8_t* vPlane = uPlane + cPlaneBytes;
        for (int yy = 0; yy < rect.h; ++yy) {
            const size_t dst =
                static_cast<size_t>(rect.y + yy) * static_cast<size_t>(w) + rect.x;
            std::memcpy(yPlane + dst, y.data() + static_cast<size_t>(yy) * yRowBytes,
                        yRowBytes);
        }
        const int cStride = w / 2;
        const int cx = rect.x / 2;
        const int cy = rect.y / 2;
        for (int yy = 0; yy < rect.h / 2; ++yy) {
            const size_t dst =
                static_cast<size_t>(cy + yy) * static_cast<size_t>(cStride) + cx;
            const size_t src = static_cast<size_t>(yy) * cRowBytes;
            std::memcpy(uPlane + dst, u.data() + src, cRowBytes);
            std::memcpy(vPlane + dst, v.data() + src, cRowBytes);
        }
        return true;
    }
};

class PlaybackOverlay {
public:
    // Shared by the command-plane renderer; the legacy rasterizer is unchanged.
    static const uint8_t* fontGlyph(char ch) { return glyph(ch); }
    static void formatTimestamp(int64_t ms, char (&out)[32]) {
        formatTime(std::max<int64_t>(0, ms), out);
    }

    static constexpr int64_t kVisibleMs = 3000;
    static constexpr int64_t kFadeMs = 500;
    static constexpr int64_t kSkipVisibleMs = 1200;

    void show(PlaybackOverlayState state, int64_t positionMs, int64_t durationMs) {
        showAt(state, positionMs, durationMs, monotonicMs());
    }

    void showAt(PlaybackOverlayState state, int64_t positionMs, int64_t durationMs,
                int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = state;
        positionMs_ = clampNonNegative(positionMs);
        durationMs_ = clampNonNegative(durationMs);
        shownAtMs_ = nowMs;
    }

    void setProgress(int64_t positionMs, int64_t durationMs) {
        std::lock_guard<std::mutex> lock(mu_);
        positionMs_ = clampNonNegative(positionMs);
        durationMs_ = clampNonNegative(durationMs);
    }

    // Movie/show title drawn in place of the PLAYING/PAUSED word. Empty keeps
    // the transport word so existing golden frames stay byte-identical.
    void setTitle(const char* title) {
        std::lock_guard<std::mutex> lock(mu_);
        std::memset(title_, 0, sizeof title_);
        if (!title)
            return;
        size_t n = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(title); *p; ++p) {
            if (n + 1 >= sizeof title_)
                break;
            unsigned char c = *p;
            if (c >= 'a' && c <= 'z')
                c = static_cast<unsigned char>(c - 'a' + 'A');
            if (c < 32 || c > 126)
                c = ' ';
            title_[n++] = static_cast<char>(c);
        }
    }

    void flashSkip(int64_t deltaMs, int64_t positionMs, int64_t durationMs) {
        flashSkipAt(deltaMs, positionMs, durationMs, monotonicMs());
    }

    void flashSkipAt(int64_t deltaMs, int64_t positionMs, int64_t durationMs, int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mu_);
        positionMs_ = clampNonNegative(positionMs);
        durationMs_ = clampNonNegative(durationMs);
        skipDeltaMs_ = deltaMs;
        skipAtMs_ = nowMs;
        shownAtMs_ = nowMs;
    }

    bool visible() const { return visibleAt(monotonicMs()); }

    bool visibleAt(int64_t nowMs) const {
        Snapshot s = snapshot();
        return alphaFor(s, nowMs) > 0 || skipAlphaFor(s, nowMs) > 0;
    }

    OverlayRect dirtyBounds(int w, int h) const { return dirtyBoundsAt(w, h, monotonicMs()); }

    OverlayRect dirtyBoundsAt(int w, int h, int64_t nowMs) const {
        Snapshot s = snapshot();
        return dirtyBoundsFor(s, w, h, nowMs);
    }

    OverlayRect dirtyBoundsI420(int w, int h) const {
        return dirtyBoundsI420At(w, h, monotonicMs());
    }

    OverlayRect dirtyBoundsI420At(int w, int h, int64_t nowMs) const {
        if ((w & 1) || (h & 1))
            return {};
        return alignI420DirtyRect(dirtyBoundsAt(w, h, nowMs), w, h);
    }

    bool renderRgb24(uint8_t* rgb, int w, int h) const {
        return renderRgb24At(rgb, w, h, monotonicMs());
    }

    bool renderRgb24At(uint8_t* rgb, int w, int h, int64_t nowMs) const {
        if (!rgb || w <= 0 || h <= 0)
            return false;
        Snapshot s = snapshot();
        const OverlayRect dirty = dirtyBoundsFor(s, w, h, nowMs);
        if (dirty.empty())
            return false;
        Rgb24Target target{rgb, w, h};
        render(target, s, w, h, nowMs);
        return true;
    }

    bool renderRgb565Le(uint8_t* rgb565le, int w, int h) const {
        return renderRgb565LeAt(rgb565le, w, h, monotonicMs());
    }

    bool renderRgb565LeAt(uint8_t* rgb565le, int w, int h, int64_t nowMs) const {
        if (!rgb565le || w <= 0 || h <= 0)
            return false;
        Snapshot s = snapshot();
        const OverlayRect dirty = dirtyBoundsFor(s, w, h, nowMs);
        if (dirty.empty())
            return false;
        Rgb565LeTarget target{rgb565le, w, h};
        render(target, s, w, h, nowMs);
        return true;
    }

    bool renderBgra32(uint8_t* bgra, int w, int h) const {
        return renderBgra32At(bgra, w, h, monotonicMs());
    }

    bool renderBgra32At(uint8_t* bgra, int w, int h, int64_t nowMs) const {
        if (!bgra || w <= 0 || h <= 0)
            return false;
        Snapshot s = snapshot();
        const OverlayRect dirty = dirtyBoundsFor(s, w, h, nowMs);
        if (dirty.empty())
            return false;
        Bgra32Target target{bgra, w, h};
        render(target, s, w, h, nowMs);
        return true;
    }

    bool renderI420(uint8_t* i420, int w, int h) const {
        return renderI420At(i420, w, h, monotonicMs());
    }

    bool renderI420At(uint8_t* i420, int w, int h, int64_t nowMs) const {
        if (!i420 || w <= 0 || h <= 0 || (w & 1) || (h & 1))
            return false;
        Snapshot s = snapshot();
        const OverlayRect dirty =
            alignI420DirtyRect(dirtyBoundsFor(s, w, h, nowMs), w, h);
        if (dirty.empty())
            return false;
        compositeI420Snapshot(i420, w, h, nowMs, s, dirty);
        return true;
    }

    bool renderI420WithBackup(uint8_t* i420, int w, int h,
                              I420DirtyBackup& backup) const {
        return renderI420WithBackupAt(i420, w, h, monotonicMs(), backup);
    }

    // Freeze state/time once so backup.rect is exactly the region this call can paint.
    bool renderI420WithBackupAt(uint8_t* i420, int w, int h, int64_t nowMs,
                                I420DirtyBackup& backup) const {
        backup.clear();
        if (!i420 || w <= 0 || h <= 0 || (w & 1) || (h & 1))
            return false;
        const Snapshot s = snapshot();
        const OverlayRect dirty =
            alignI420DirtyRect(dirtyBoundsFor(s, w, h, nowMs), w, h);
        if (dirty.empty() || !backup.capture(i420, w, h, dirty))
            return false;
        compositeI420Snapshot(i420, w, h, nowMs, s, dirty);
        return true;
    }

private:
    struct Color {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    struct Snapshot {
        PlaybackOverlayState state = PlaybackOverlayState::Stopped;
        int64_t positionMs = 0;
        int64_t durationMs = 0;
        int64_t shownAtMs = -kVisibleMs;
        int64_t skipAtMs = -kSkipVisibleMs;
        int64_t skipDeltaMs = 0;
        char title[48] = {};
    };

    struct Rgb24Target {
        uint8_t* p;
        int w;
        int h;

        Color get(int x, int y) const {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            return Color{p[i + 0], p[i + 1], p[i + 2]};
        }

        void set(int x, int y, Color c) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            p[i + 0] = c.r;
            p[i + 1] = c.g;
            p[i + 2] = c.b;
        }

        void blend(int x, int y, Color c, int alpha) {
            if (alpha >= 255) {
                set(x, y, c);
                return;
            }
            const Color d = get(x, y);
            const int inv = 255 - alpha;
            set(x, y, Color{static_cast<uint8_t>((c.r * alpha + d.r * inv) / 255),
                            static_cast<uint8_t>((c.g * alpha + d.g * inv) / 255),
                            static_cast<uint8_t>((c.b * alpha + d.b * inv) / 255)});
        }
    };

    struct Rgb565LeTarget {
        uint8_t* p;
        int w;
        int h;

        Color get(int x, int y) const {
            const size_t i = (static_cast<size_t>(y) * w + x) * 2;
            const uint16_t v = static_cast<uint16_t>(p[i] | (p[i + 1] << 8));
            const uint8_t r5 = static_cast<uint8_t>((v >> 11) & 0x1f);
            const uint8_t g6 = static_cast<uint8_t>((v >> 5) & 0x3f);
            const uint8_t b5 = static_cast<uint8_t>(v & 0x1f);
            return Color{static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                         static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
                         static_cast<uint8_t>((b5 << 3) | (b5 >> 2))};
        }

        void set(int x, int y, Color c) {
            const uint16_t v =
                static_cast<uint16_t>(((c.r & 0xf8) << 8) | ((c.g & 0xfc) << 3) | (c.b >> 3));
            const size_t i = (static_cast<size_t>(y) * w + x) * 2;
            p[i] = static_cast<uint8_t>(v & 0xff);
            p[i + 1] = static_cast<uint8_t>(v >> 8);
        }

        void blend(int x, int y, Color c, int alpha) {
            if (alpha >= 255) {
                set(x, y, c);
                return;
            }
            const Color d = get(x, y);
            const int inv = 255 - alpha;
            set(x, y, Color{static_cast<uint8_t>((c.r * alpha + d.r * inv) / 255),
                            static_cast<uint8_t>((c.g * alpha + d.g * inv) / 255),
                            static_cast<uint8_t>((c.b * alpha + d.b * inv) / 255)});
        }
    };

    struct Bgra32Target {
        uint8_t* p;
        int w;
        int h;

        Color get(int x, int y) const {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            return Color{p[i + 2], p[i + 1], p[i + 0]};
        }

        void set(int x, int y, Color c) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            p[i + 0] = c.b;
            p[i + 1] = c.g;
            p[i + 2] = c.r;
            p[i + 3] = 0xff;
        }

        void blend(int x, int y, Color c, int alpha) {
            if (alpha >= 255) {
                set(x, y, c);
                return;
            }
            const Color d = get(x, y);
            const int inv = 255 - alpha;
            set(x, y, Color{static_cast<uint8_t>((c.r * alpha + d.r * inv) / 255),
                            static_cast<uint8_t>((c.g * alpha + d.g * inv) / 255),
                            static_cast<uint8_t>((c.b * alpha + d.b * inv) / 255)});
        }
    };

    struct I420LayerTarget {
        struct Pixel {
            uint16_t r = 0;
            uint16_t g = 0;
            uint16_t b = 0;
            uint16_t a = 0;
        };

        int w;
        int h;
        OverlayRect dirty;
        std::vector<Pixel>& pixels;

        I420LayerTarget(int width, int height, OverlayRect bounds, std::vector<Pixel>& scratch)
            : w(width), h(height), dirty(bounds), pixels(scratch) {
            pixels.assign(static_cast<size_t>(bounds.w) * static_cast<size_t>(bounds.h),
                          Pixel{});
        }

        void blend(int x, int y, Color c, int alpha) {
            if (x < dirty.x || y < dirty.y || x >= dirty.x + dirty.w ||
                y >= dirty.y + dirty.h)
                return;
            Pixel& d = at(x, y);
            if (alpha >= 255) {
                d.r = static_cast<uint16_t>(c.r * 255);
                d.g = static_cast<uint16_t>(c.g * 255);
                d.b = static_cast<uint16_t>(c.b * 255);
                d.a = 255;
                return;
            }
            const int inv = 255 - alpha;
            d.r = static_cast<uint16_t>(c.r * alpha + (d.r * inv + 127) / 255);
            d.g = static_cast<uint16_t>(c.g * alpha + (d.g * inv + 127) / 255);
            d.b = static_cast<uint16_t>(c.b * alpha + (d.b * inv + 127) / 255);
            d.a = static_cast<uint16_t>(alpha + (d.a * inv + 127) / 255);
        }

        void composite(uint8_t* i420) const {
            const size_t yPlaneBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
            const size_t cPlaneBytes = static_cast<size_t>(w / 2) * static_cast<size_t>(h / 2);
            uint8_t* yPlane = i420;
            uint8_t* uPlane = yPlane + yPlaneBytes;
            uint8_t* vPlane = uPlane + cPlaneBytes;

            for (int yy = dirty.y; yy < dirty.y + dirty.h; ++yy) {
                for (int xx = dirty.x; xx < dirty.x + dirty.w; ++xx) {
                    const Pixel& p = at(xx, yy);
                    if (p.a == 0)
                        continue;
                    const Color c = straight(p);
                    const size_t i =
                        static_cast<size_t>(yy) * static_cast<size_t>(w) + xx;
                    yPlane[i] = blendSample(yPlane[i], rgbToY(c), p.a);
                }
            }

            const int cStride = w / 2;
            // I420 shares one chroma sample per 2x2 luma cell. Fold the four
            // independently composited pixels into one coverage-weighted write.
            for (int yy = dirty.y; yy < dirty.y + dirty.h; yy += 2) {
                for (int xx = dirty.x; xx < dirty.x + dirty.w; xx += 2) {
                    int sumAlpha = 0;
                    int sumU = 0;
                    int sumV = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const Pixel& p = at(xx + dx, yy + dy);
                            if (p.a == 0)
                                continue;
                            const Color c = straight(p);
                            sumAlpha += p.a;
                            sumU += rgbToU(c) * p.a;
                            sumV += rgbToV(c) * p.a;
                        }
                    }
                    if (sumAlpha == 0)
                        continue;
                    constexpr int kCellAlpha = 4 * 255;
                    const size_t ci = static_cast<size_t>(yy / 2) *
                                          static_cast<size_t>(cStride) +
                                      xx / 2;
                    uPlane[ci] = static_cast<uint8_t>(
                        (sumU + uPlane[ci] * (kCellAlpha - sumAlpha) +
                         kCellAlpha / 2) /
                        kCellAlpha);
                    vPlane[ci] = static_cast<uint8_t>(
                        (sumV + vPlane[ci] * (kCellAlpha - sumAlpha) +
                         kCellAlpha / 2) /
                        kCellAlpha);
                }
            }
        }

    private:
        Pixel& at(int x, int y) {
            return pixels[static_cast<size_t>(y - dirty.y) *
                              static_cast<size_t>(dirty.w) +
                          static_cast<size_t>(x - dirty.x)];
        }

        const Pixel& at(int x, int y) const {
            return pixels[static_cast<size_t>(y - dirty.y) *
                              static_cast<size_t>(dirty.w) +
                          static_cast<size_t>(x - dirty.x)];
        }

        static uint8_t clamp8(int v) {
            return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }

        static uint8_t rgbToY(Color c) {
            return clamp8(((66 * c.r + 129 * c.g + 25 * c.b + 128) >> 8) + 16);
        }

        static uint8_t rgbToU(Color c) {
            return clamp8(((-38 * c.r - 74 * c.g + 112 * c.b + 128) >> 8) + 128);
        }

        static uint8_t rgbToV(Color c) {
            return clamp8(((112 * c.r - 94 * c.g - 18 * c.b + 128) >> 8) + 128);
        }

        static Color straight(const Pixel& p) {
            return Color{static_cast<uint8_t>((p.r + p.a / 2) / p.a),
                         static_cast<uint8_t>((p.g + p.a / 2) / p.a),
                         static_cast<uint8_t>((p.b + p.a / 2) / p.a)};
        }

        static uint8_t blendSample(uint8_t dst, uint8_t src, int alpha) {
            return static_cast<uint8_t>(
                (src * alpha + dst * (255 - alpha) + 127) / 255);
        }
    };

    void compositeI420Snapshot(uint8_t* i420, int w, int h, int64_t nowMs,
                               const Snapshot& s, OverlayRect dirty) const {
        std::lock_guard<std::mutex> renderLock(i420RenderMu_);
        I420LayerTarget target{w, h, dirty, i420Scratch_};
        render(target, s, w, h, nowMs);
        target.composite(i420);
    }

    static int64_t clampNonNegative(int64_t v) { return v < 0 ? 0 : v; }

    static int64_t monotonicMs() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        Snapshot s{state_, positionMs_, durationMs_, shownAtMs_, skipAtMs_, skipDeltaMs_};
        std::memcpy(s.title, title_, sizeof s.title);
        return s;
    }

    static int alphaFor(const Snapshot& s, int64_t nowMs) {
        const int64_t age = nowMs - s.shownAtMs;
        if (age < 0 || age >= kVisibleMs)
            return 0;
        if (age <= kVisibleMs - kFadeMs)
            return 255;
        return std::max<int>(1, static_cast<int>(((kVisibleMs - age) * 255) / kFadeMs));
    }

    static int skipAlphaFor(const Snapshot& s, int64_t nowMs) {
        const int64_t age = nowMs - s.skipAtMs;
        if (age < 0 || age >= kSkipVisibleMs || s.skipDeltaMs == 0)
            return 0;
        if (age <= kSkipVisibleMs - 300)
            return 255;
        return std::max<int>(1, static_cast<int>(((kSkipVisibleMs - age) * 255) / 300));
    }

    static int geometryScale(int h) {
        return std::max(1, h / 240);
    }

    static OverlayRect panelBounds(int w, int h) {
        const int scale = geometryScale(h);
        const int margin = 10 * scale;
        const int ph = 60 * scale;
        return OverlayRect{margin, h - ph - margin, w - margin * 2, ph};
    }

    static void formatSkipText(const Snapshot& s, char (&text)[24]) {
        const int64_t sec =
            std::min<int64_t>(9999, std::max<int64_t>(1, std::llabs(s.skipDeltaMs) / 1000));
        if (s.skipDeltaMs >= 0)
            std::snprintf(text, sizeof(text), "%lldS >>", static_cast<long long>(sec));
        else
            std::snprintf(text, sizeof(text), "<< %lldS", static_cast<long long>(sec));
    }

    static OverlayRect skipBounds(const Snapshot& s, int w, int h) {
        const int scale = geometryScale(h);
        char text[24];
        formatSkipText(s, text);
        const int textScale = 2 * scale;
        const int tw = static_cast<int>(std::strlen(text)) * 6 * textScale - textScale;
        const int boxW = std::min(w - 16 * scale, std::max(76 * scale, tw + 24 * scale));
        return OverlayRect{(w - boxW) / 2, std::max(8 * scale, h / 2 - 30 * scale),
                           boxW, 28 * scale};
    }

    static OverlayRect dirtyBoundsFor(const Snapshot& s, int w, int h, int64_t nowMs) {
        if (w <= 0 || h <= 0)
            return {};
        OverlayRect out{};
        if (alphaFor(s, nowMs) > 0)
            out = panelBounds(w, h);
        if (skipAlphaFor(s, nowMs) > 0)
            out = unionRect(out, skipBounds(s, w, h));
        return out;
    }

    static OverlayRect unionRect(OverlayRect a, OverlayRect b) {
        if (a.empty())
            return b;
        if (b.empty())
            return a;
        const int x0 = std::min(a.x, b.x);
        const int y0 = std::min(a.y, b.y);
        const int x1 = std::max(a.x + a.w, b.x + b.w);
        const int y1 = std::max(a.y + a.h, b.y + b.h);
        return OverlayRect{x0, y0, x1 - x0, y1 - y0};
    }

    template <typename Target>
    static void blendPixel(Target& t, int x, int y, Color c, int alpha) {
        if (x < 0 || y < 0 || x >= t.w || y >= t.h || alpha <= 0)
            return;
        t.blend(x, y, c, alpha);
    }

    template <typename Target>
    static void fillRect(Target& t, int x, int y, int w, int h, Color c, int alpha) {
        const int x0 = std::max(0, x);
        const int y0 = std::max(0, y);
        const int x1 = std::min(t.w, x + w);
        const int y1 = std::min(t.h, y + h);
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx)
                blendPixel(t, xx, yy, c, alpha);
    }

    template <typename Target>
    static void strokeRect(Target& t, int x, int y, int w, int h, Color c, int alpha,
                           int thickness) {
        fillRect(t, x, y, w, thickness, c, alpha);
        fillRect(t, x, y + h - thickness, w, thickness, c, alpha);
        fillRect(t, x, y, thickness, h, c, alpha);
        fillRect(t, x + w - thickness, y, thickness, h, c, alpha);
    }

    static const uint8_t* glyph(char ch) {
        static constexpr uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
        static constexpr uint8_t d0[7] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
        static constexpr uint8_t d1[7] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
        static constexpr uint8_t d2[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
        static constexpr uint8_t d3[7] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
        static constexpr uint8_t d4[7] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
        static constexpr uint8_t d5[7] = {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e};
        static constexpr uint8_t d6[7] = {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e};
        static constexpr uint8_t d7[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        static constexpr uint8_t d8[7] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
        static constexpr uint8_t d9[7] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c};
        static constexpr uint8_t colon[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        static constexpr uint8_t lt[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
        static constexpr uint8_t gt[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
        static constexpr uint8_t minus[7] = {0, 0, 0, 0x1f, 0, 0, 0};
        static constexpr uint8_t a[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
        static constexpr uint8_t d[7] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
        static constexpr uint8_t e[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
        static constexpr uint8_t g[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f};
        static constexpr uint8_t i[7] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
        static constexpr uint8_t l[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
        static constexpr uint8_t n[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        static constexpr uint8_t o[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
        static constexpr uint8_t p[7] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
        static constexpr uint8_t s[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
        static constexpr uint8_t t[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        static constexpr uint8_t u[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
        static constexpr uint8_t y[7] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
        static constexpr uint8_t b[7] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
        static constexpr uint8_t c[7] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
        static constexpr uint8_t f[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
        static constexpr uint8_t h[7] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
        static constexpr uint8_t j[7] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0e};
        static constexpr uint8_t k[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        static constexpr uint8_t m[7] = {0x11, 0x1b, 0x15, 0x11, 0x11, 0x11, 0x11};
        static constexpr uint8_t q[7] = {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d};
        static constexpr uint8_t r[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
        static constexpr uint8_t v[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
        static constexpr uint8_t w[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11};
        static constexpr uint8_t x[7] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11};
        static constexpr uint8_t z[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f};
        switch (ch) {
        case '0': return d0;
        case '1': return d1;
        case '2': return d2;
        case '3': return d3;
        case '4': return d4;
        case '5': return d5;
        case '6': return d6;
        case '7': return d7;
        case '8': return d8;
        case '9': return d9;
        case ':': return colon;
        case '<': return lt;
        case '>': return gt;
        case '-': return minus;
        case 'A': return a;
        case 'B': return b;
        case 'C': return c;
        case 'D': return d;
        case 'E': return e;
        case 'F': return f;
        case 'G': return g;
        case 'H': return h;
        case 'I': return i;
        case 'J': return j;
        case 'K': return k;
        case 'L': return l;
        case 'M': return m;
        case 'N': return n;
        case 'O': return o;
        case 'P': return p;
        case 'Q': return q;
        case 'R': return r;
        case 'S': return s;
        case 'T': return t;
        case 'U': return u;
        case 'V': return v;
        case 'W': return w;
        case 'X': return x;
        case 'Y': return y;
        case 'Z': return z;
        default: return space;
        }
    }

    static int textWidth(const char* text, int scale) {
        return static_cast<int>(std::strlen(text)) * 6 * scale - scale;
    }

    template <typename Target>
    static void drawText(Target& t, int x, int y, const char* text, int scale, Color c,
                         int alpha) {
        for (const char* p = text; *p; ++p) {
            const uint8_t* g = glyph(*p);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if ((g[row] & (1u << (4 - col))) == 0)
                        continue;
                    fillRect(t, x + col * scale, y + row * scale, scale, scale, c, alpha);
                }
            }
            x += 6 * scale;
        }
    }

    static void formatTime(int64_t ms, char (&out)[32]) {
        int64_t sec = ms / 1000;
        const int64_t h = sec / 3600;
        sec %= 3600;
        const int64_t m = sec / 60;
        const int64_t s = sec % 60;
        if (h > 0)
            std::snprintf(out, 32, "%lld:%02lld:%02lld", static_cast<long long>(h),
                          static_cast<long long>(m), static_cast<long long>(s));
        else
            std::snprintf(out, 32, "%lld:%02lld", static_cast<long long>(m),
                          static_cast<long long>(s));
    }

    template <typename Target>
    static void drawIcon(Target& t, PlaybackOverlayState state, int cx, int cy, int scale,
                         int alpha) {
        constexpr Color amber{255, 178, 32};
        if (state == PlaybackOverlayState::Playing) {
            // Point right: wide base on the left, tip on the right (play).
            for (int x = 0; x < 16; ++x) {
                const int half = (15 - x) / 2;
                fillRect(t, cx + (-5 + x) * scale, cy - half * scale, scale,
                         (half * 2 + 1) * scale, amber, alpha);
            }
        } else if (state == PlaybackOverlayState::Paused) {
            fillRect(t, cx - 9 * scale, cy - 10 * scale, 6 * scale, 20 * scale, amber,
                     alpha);
            fillRect(t, cx + 3 * scale, cy - 10 * scale, 6 * scale, 20 * scale, amber,
                     alpha);
        } else {
            fillRect(t, cx - 9 * scale, cy - 9 * scale, 18 * scale, 18 * scale, amber,
                     alpha);
        }
    }

    static const char* stateLabel(PlaybackOverlayState state) {
        switch (state) {
        case PlaybackOverlayState::Playing: return "PLAYING";
        case PlaybackOverlayState::Paused: return "PAUSED";
        case PlaybackOverlayState::Stopped: return "STOPPED";
        }
        return "STOPPED";
    }

    template <typename Target>
    static void render(Target& t, const Snapshot& s, int w, int h, int64_t nowMs) {
        const int alpha = alphaFor(s, nowMs);
        const int scale = geometryScale(h);
        constexpr Color black{0, 0, 0};
        constexpr Color panelEdge{70, 74, 82};
        constexpr Color white{235, 238, 244};
        constexpr Color muted{130, 138, 150};
        constexpr Color amber{255, 178, 32};

        if (alpha > 0) {
            const OverlayRect p = panelBounds(w, h);
            fillRect(t, p.x, p.y, p.w, p.h, black, (170 * alpha) / 255);
            strokeRect(t, p.x, p.y, p.w, p.h, panelEdge, (150 * alpha) / 255, scale);

            const int iconX = p.x + 22 * scale;
            const int labelY = p.y + 10 * scale;
            drawIcon(t, s.state, iconX, p.y + 20 * scale, scale, alpha);
            const char* label = stateLabel(s.state);
            char titleFit[48];
            if (s.title[0]) {
                std::snprintf(titleFit, sizeof titleFit, "%s", s.title);
                const int maxPx = std::max(6 * scale, p.x + p.w - (iconX + 24 * scale) - 8 * scale);
                while (titleFit[0] && textWidth(titleFit, scale) > maxPx)
                    titleFit[std::strlen(titleFit) - 1] = '\0';
                label = titleFit;
            }
            drawText(t, iconX + 24 * scale, labelY, label, scale, white, alpha);

            char elapsed[32];
            char total[32];
            formatTime(s.positionMs, elapsed);
            formatTime(s.durationMs, total);
            drawText(t, p.x + 16 * scale, p.y + 34 * scale, elapsed, scale, white, alpha);
            const int totalW = textWidth(total, scale);
            drawText(t, p.x + p.w - 16 * scale - totalW, p.y + 34 * scale, total, scale,
                     muted, alpha);

            const int barX = p.x + 16 * scale;
            const int barY = p.y + p.h - 18 * scale;
            const int barW = p.w - 32 * scale;
            fillRect(t, barX, barY, barW, 6 * scale, Color{58, 63, 72},
                     (220 * alpha) / 255);
            int fillW = 0;
            if (s.durationMs > 0) {
                const int logicalBarW = barW / scale;
                fillW =
                    static_cast<int>((static_cast<long long>(logicalBarW) *
                                      std::min(s.positionMs, s.durationMs)) /
                                     s.durationMs) *
                    scale;
            }
            fillW = std::max(0, std::min(barW, fillW));
            if (fillW > 0)
                fillRect(t, barX, barY, fillW, 6 * scale, amber, alpha);
            const int knobX = barX + fillW;
            fillRect(t, knobX - 2 * scale, barY - 2 * scale, 5 * scale, 10 * scale,
                     white, alpha);
        }

        const int skipAlpha = skipAlphaFor(s, nowMs);
        if (skipAlpha > 0) {
            char text[24];
            formatSkipText(s, text);
            const int textScale = 2 * scale;
            const int tw = textWidth(text, textScale);
            const OverlayRect box = skipBounds(s, w, h);
            fillRect(t, box.x, box.y, box.w, box.h, black, (190 * skipAlpha) / 255);
            strokeRect(t, box.x, box.y, box.w, box.h, amber, skipAlpha, scale);
            drawText(t, box.x + (box.w - tw) / 2, box.y + 7 * scale, text, textScale,
                     white, skipAlpha);
        }
    }

    mutable std::mutex mu_;
    PlaybackOverlayState state_ = PlaybackOverlayState::Stopped;
    int64_t positionMs_ = 0;
    int64_t durationMs_ = 0;
    int64_t shownAtMs_ = -kVisibleMs;
    int64_t skipAtMs_ = -kSkipVisibleMs;
    int64_t skipDeltaMs_ = 0;
    char title_[48] = {};
    mutable std::mutex i420RenderMu_;
    mutable std::vector<I420LayerTarget::Pixel> i420Scratch_;
};

} // namespace misterplex
