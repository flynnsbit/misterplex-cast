#pragma once
// N14: the stand-by card shown full-screen while a Pluto ad pod is dropped (the live relay is
// silent, or ffmpeg is being re-spawned). Header-only and stb-free: the decoded RGBA image is
// registered once at daemon start (ad_break_card_load.hpp), and paint() asks for it scaled to the
// current raster. Letterboxed (the art is 16:9), area-averaged downscale, sharpened on <= 288-line
// rasters so the lettering holds up on S-Video 240p.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace misterplex {

struct AdCardImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // straight alpha, w*h*4
    bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * size_t(h) * 4u; }
};

// The scaled card, BGRA (the overlay's byte order), placed inside a W x H canvas.
struct AdCardBitmap {
    int canvasW = 0, canvasH = 0;
    int x = 0, y = 0, w = 0, h = 0;   // the picture rectangle on the canvas (bars are black)
    std::vector<uint8_t> bgra;        // w*h*4
    bool ok() const { return w > 0 && h > 0 && bgra.size() == size_t(w) * size_t(h) * 4u; }
};

// Fit iw x ih into a canvas of W x H whose pixels are `par` times wider than tall (par 1 = square).
// even: interlaced, y and h even.
inline AdCardBitmap adCardScale(const AdCardImage& im, int W, int H, double par, bool even) {
    AdCardBitmap out;
    out.canvasW = W;
    out.canvasH = H;
    if (!im.ok() || W <= 0 || H <= 0)
        return out;
    if (par <= 0.01)
        par = 1.0;
    const double s = std::min(double(W) * par / double(im.w), double(H) / double(im.h));
    int ow = std::max(1, std::min(W, int(std::lround(double(im.w) * s / par))));
    int oh = std::max(1, std::min(H, int(std::lround(double(im.h) * s))));
    if (even)
        oh = std::max(2, oh & ~1);
    out.w = ow;
    out.h = oh;
    out.x = (W - ow) / 2;
    out.y = (H - oh) / 2;
    if (even)
        out.y &= ~1;
    out.bgra.assign(size_t(ow) * size_t(oh) * 4u, 0);
    // Area average: each output pixel covers [x*iw/ow, (x+1)*iw/ow) x [y*ih/oh, (y+1)*ih/oh).
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
            uint8_t* d = &out.bgra[(size_t(y) * size_t(ow) + size_t(x)) * 4u];
            if (wsum <= 0)
                wsum = 1;
            d[0] = uint8_t(std::lround(b / wsum));
            d[1] = uint8_t(std::lround(g / wsum));
            d[2] = uint8_t(std::lround(r / wsum));
            d[3] = 0xFF;
        }
    }
    // 240p / S-Video: a downscale by 2.4x+ softens thin strokes. Unsharp mask (3x3, amount 0.8).
    if (H <= 288 && ow >= 3 && oh >= 3) {
        std::vector<uint8_t> src = out.bgra;
        for (int y = 1; y < oh - 1; ++y)
            for (int x = 1; x < ow - 1; ++x)
                for (int c = 0; c < 3; ++c) {
                    int sum = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            sum += src[(size_t(y + dy) * size_t(ow) + size_t(x + dx)) * 4u + size_t(c)];
                    const int blur = sum / 9;
                    const int v = src[(size_t(y) * size_t(ow) + size_t(x)) * 4u + size_t(c)];
                    out.bgra[(size_t(y) * size_t(ow) + size_t(x)) * 4u + size_t(c)] =
                        uint8_t(std::max(0, std::min(255, v + (v - blur) * 4 / 5)));
                }
    }
    return out;
}

// Process-wide registry: the image (set once) and a one-entry scaled cache keyed by raster.
class AdCardStore {
public:
    static AdCardStore& get() {
        static AdCardStore s;
        return s;
    }
    void set(AdCardImage im) {
        std::lock_guard<std::mutex> g(mu_);
        img_ = std::make_shared<const AdCardImage>(std::move(im));
        cache_.reset();
    }
    bool loaded() const {
        std::lock_guard<std::mutex> g(mu_);
        return img_ && img_->ok();
    }
    // nullptr when no image is registered (paint then shows a plain black field: still a card, not a frozen frame).
    std::shared_ptr<const AdCardBitmap> bitmapFor(int W, int H, double par, bool even) {
        std::lock_guard<std::mutex> g(mu_);
        if (!img_ || !img_->ok())
            return nullptr;
        if (cache_ && cache_->canvasW == W && cache_->canvasH == H && cachePar_ == par && cacheEven_ == even)
            return cache_;
        cache_ = std::make_shared<const AdCardBitmap>(adCardScale(*img_, W, H, par, even));
        cachePar_ = par;
        cacheEven_ = even;
        return cache_;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const AdCardImage> img_;
    std::shared_ptr<const AdCardBitmap> cache_;
    double cachePar_ = 1.0;
    bool cacheEven_ = false;
};

// Trigger / clear policy for the card (pure, testable). The relay reports On when it has been
// silent for the threshold (or is re-spawning ffmpeg) and Resumed when real bytes flow again; the card
// clears on the first shown frame after that (the player's position advancing), or on the hold timeout.
// MUT_ADBREAK_NEVER_CLEAR: the card is never cleared.
class AdBreakGate {
public:
    static constexpr int64_t kResumeHoldMs = 4000;   // clear anyway this long after bytes resumed
    bool shown() const { return shown_.load(std::memory_order_acquire); }
    void on() {
        std::lock_guard<std::mutex> g(mu_);
        awaiting_ = false;
        shown_.store(true, std::memory_order_release);
    }
    // Real bytes flow again. posMs = the player's current position (a later, larger one = a shown frame).
    void resumed(int64_t posMs, int64_t nowMs) {
        std::lock_guard<std::mutex> g(mu_);
        if (!shown_.load())
            return;
        awaiting_ = true;
        posAtResume_ = posMs;
        resumedAtMs_ = nowMs;
    }
    // Returns true when this call cleared the card.
    bool position(int64_t posMs, int64_t nowMs) {
        std::lock_guard<std::mutex> g(mu_);
        if (!awaiting_ || !shown_.load())
            return false;
        if (posMs > posAtResume_ || nowMs - resumedAtMs_ >= kResumeHoldMs)
            return clearLocked();
        return false;
    }
    bool clearNow() {
        std::lock_guard<std::mutex> g(mu_);
        return clearLocked();
    }

private:
    bool clearLocked() {
#ifdef MUT_ADBREAK_NEVER_CLEAR
        return false;
#else
        awaiting_ = false;
        return shown_.exchange(false, std::memory_order_acq_rel);
#endif
    }
    std::mutex mu_;
    std::atomic<bool> shown_{false};
    bool awaiting_ = false;
    int64_t posAtResume_ = 0, resumedAtMs_ = 0;
};

// N16: glass test hook. Default OFF and inert: it does nothing unless MPX_AD_CARD_TEST_HOOK=1 (env, or the conf key
// AD_CARD_TEST_HOOK=1) was set when the daemon started. When armed, and only then, it watches ONE fixed file,
// /tmp/mpx-ad-card-hold (a regular file, no symlinks; at most 31 bytes are read). A number of seconds in it forces the
// stand-by card on for that long (empty file = 30 s, clamped to 1..600; garbage, zero or negative = ignored), so a glass
// run can open the program menu / library / Spelunkr guide over the card without waiting for a real ad pod:
//     echo 60 > /tmp/mpx-ad-card-hold
// The daemon consumes (unlinks) the file, logs "ad card test hold <n> s" and, at the end, "ad card test hold ended".
class AdCardTestHold {
public:
    enum class Act { None, On, Off };
    static constexpr const char* kPath = "/tmp/mpx-ad-card-hold";
    AdCardTestHold() {
        const char* e = std::getenv("MPX_AD_CARD_TEST_HOOK");
        armed_ = e && e[0] == '1' && e[1] == 0;
    }
    bool armed() const { return armed_; }
    // `path` is injectable for tests. Returns On once when the file is consumed, Off once when the hold elapses.
    Act poll(int64_t nowMs, const char* path = kPath) {
#ifndef MUT_N16_HOLD_UNGATED   // mutant: the hook is live in production
        if (!armed_)
            return Act::None;
#endif
        if (untilMs_ != 0 && nowMs >= untilMs_) {
#ifndef MUT_N16_HOLD_NO_EXPIRE   // mutant: the forced card never goes away
            untilMs_ = 0;
            return Act::Off;
#endif
        }
        if (untilMs_ == 0 || nowMs - lastPollMs_ >= 500) {
            lastPollMs_ = nowMs;
            const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st {};
                char buf[32] = {0};
                ssize_t n = -1;
                if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
                    n = ::read(fd, buf, sizeof buf - 1);
                ::close(fd);
                std::remove(path);
                if (n < 0)
                    return Act::None;
                char* end = nullptr;
                long sec = 30;
                bool ok = true;
                if (n > 0) {
                    sec = std::strtol(buf, &end, 10);
                    if (end == buf)
                        ok = false;   // garbage
                    else
                        while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')
                            ++end;
                    if (ok && *end != 0)
                        ok = false;   // trailing garbage
                    if (ok && sec <= 0)
                        ok = false;
                }
                if (!ok)
                    return Act::None;
                if (sec > 600)
                    sec = 600;
                seconds_ = static_cast<int>(sec);
                untilMs_ = nowMs + sec * 1000;
                return Act::On;
            }
        }
        return Act::None;
    }
    int seconds() const { return seconds_; }

private:
    bool armed_ = false;
    int64_t untilMs_ = 0, lastPollMs_ = -1000000;
    int seconds_ = 0;
};

}  // namespace misterplex
