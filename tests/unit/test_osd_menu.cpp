// Unit tests for the OSD menu word decode and the idle-screen renderer.
//
// These two headers are the contract between Plex.sv's CONF_STR and the daemon.
// If they drift apart the symptom is silent and awful — a menu item labelled
// "-40ms" applies +120ms — so the layout is pinned here rather than trusted.
#include "libmisterplex/idle_screen.hpp"
#include "libmisterplex/osd_menu.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    using namespace misterplex;

    // --- video delay: 4-bit signed wrap, 20 ms per step, biased to the default ---
    // Power-on status is all zeroes and Main_MiSTer cannot express a non-zero
    // default, so index 0 MUST mean 0 ms.
    CHECK(osdAvOffsetMsFromIndex(0) == kOsdAvOffsetDefaultMs);
    CHECK(osdAvOffsetMsFromIndex(1) == kOsdAvOffsetDefaultMs + 20);
    CHECK(osdAvOffsetMsFromIndex(7) == kOsdAvOffsetDefaultMs + 140);
    CHECK(osdAvOffsetMsFromIndex(8) == kOsdAvOffsetDefaultMs - 160);
    CHECK(osdAvOffsetMsFromIndex(15) == kOsdAvOffsetDefaultMs - 20);
    // The knob must stay monotonic across the wrap seam: index 15 is exactly one
    // step below index 0, so left/right on the OSD never jumps.
    CHECK(osdAvOffsetMsFromIndex(15) + kOsdAvOffsetStepMs == osdAvOffsetMsFromIndex(0));
    // Every index maps to a distinct, in-range value.
    for (unsigned i = 0; i < 16; ++i) {
        const int ms = osdAvOffsetMsFromIndex(i);
        CHECK(ms >= kOsdAvOffsetDefaultMs - 160 && ms <= kOsdAvOffsetDefaultMs + 140);
        CHECK(ms % kOsdAvOffsetStepMs == 0);
    }

    // --- word decode: bit positions must match CONF_STR exactly ---
    {
        const OsdSettings d = decodeOsdWord(0x0000);
        CHECK(d.avOffsetMs == kOsdAvOffsetDefaultMs);
        CHECK(d.audioClockTrimEnabled);
        CHECK(d.resyncEnabled);
        CHECK(d.idleMode == 0);
        CHECK(d.contentResolution.width == 320);
        CHECK(d.contentResolution.height == 240);
        CHECK(std::string(d.contentResolution.label) == "240p");
        CHECK(d.contentResolution.weakBitrateKbps == 1000);
    }
    CHECK(decodeOsdWord(1u << 1).resyncEnabled == false);   // O[1] A/V auto resync
    CHECK(!decodeOsdWord(1u << 3).audioClockTrimEnabled); // O[3] Audio clock trim
    // O[5:4] content resolution: 0=240p, 1=480p, 2|3=720p (v7 O[4]-only still 240/480)
    CHECK(decodeOsdWord(1u << 4).contentResolution.width == 640);
    CHECK(decodeOsdWord(1u << 4).contentResolution.height == 480);
    CHECK(std::string(decodeOsdWord(1u << 4).contentResolution.label) == "480p");
    CHECK(decodeOsdWord(2u << 4).contentResolution.width == 1280);
    CHECK(decodeOsdWord(2u << 4).contentResolution.height == 720);
    CHECK(std::string(decodeOsdWord(2u << 4).contentResolution.label) == "720p");
    CHECK(decodeOsdWord(3u << 4).contentResolution.width == 1280);
    CHECK(decodeOsdWord(0xFu << 6).avOffsetMs == kOsdAvOffsetDefaultMs - 20); // O[9:6] idx 15
    CHECK(decodeOsdWord(8u << 6).avOffsetMs == kOsdAvOffsetDefaultMs - 160); // O[9:6] idx 8
    // v9: O[15:14] Display — 0=follow content, 1=240p, 2=480p, 3=720p.
    CHECK(decodeOsdWord(0u).displayResolution.width == 320); // follow 240p content
    CHECK(decodeOsdWord(2u << 4).displayResolution.width == 1280); // follow 720p content
    CHECK(decodeOsdWord((2u << 4) | (1u << 14)).displayResolution.width == 320); // force 240p
    CHECK(std::string(decodeOsdWord((2u << 4) | (1u << 14)).displayResolution.label) == "240p");
    CHECK(decodeOsdWord((0u << 4) | (2u << 14)).displayResolution.width == 640);
    CHECK(decodeOsdWord((0u << 4) | (3u << 14)).displayResolution.width == 1280);
    CHECK(decodeOsdWord(3u << 14).idleMode == 0); // idle conf-only
    // Core-owned bits must not leak into user settings (O[5] is content-res high).
    for (int bit : {0, 2, 10, 11, 12, 13}) {
        const OsdSettings d = decodeOsdWord(static_cast<uint16_t>(1u << bit));
        CHECK(d.avOffsetMs == kOsdAvOffsetDefaultMs);
        CHECK(d.resyncEnabled);
        CHECK(d.audioClockTrimEnabled);
        CHECK(d.idleMode == 0);
        CHECK(d.contentResolution.width == 320);
    }
    // Alone, O[5]=1 with O[4]=0 is code 2 → 720p (not a leak).
    CHECK(decodeOsdWord(1u << 5).contentResolution.width == 1280);
    CHECK(contentResolutionFromSize(320, 240).width == 320);
    CHECK(std::string(contentResolutionFromSize(320, 240).label) == "240p");
    CHECK(contentResolutionFromSize(640, 480).width == 640);
    CHECK(std::string(contentResolutionFromSize(640, 480).label) == "480p");
    CHECK(contentResolutionFromSize(1280, 720).width == 1280);
    CHECK(std::string(contentResolutionFromSize(1280, 720).label) == "720p");

    // --- change detection ignores core traffic ---
    // [10]/[11] flush pulses and [12]/[13] DDR kick/bank toggle constantly during
    // playback; reacting to them would re-log and re-apply settings every frame.
    for (int bit : {0, 2, 10, 11, 12, 13})
        CHECK(!osdChanged(0, static_cast<uint16_t>(1u << bit)));
    for (int bit : {1, 3, 4, 5, 6, 7, 8, 9, 14, 15})
        CHECK(osdChanged(0, static_cast<uint16_t>(1u << bit)));

    // --- idle mode bits match CONF_STR order ---
    CHECK(idleModeFromBits(0) == IdleMode::Logo);
    CHECK(idleModeFromBits(1) == IdleMode::Black);
    CHECK(idleModeFromBits(2) == IdleMode::Screensaver);
    CHECK(idleModeFromBits(3) == IdleMode::LastFrame);

    // --- screensaver drift stays on screen and reverses ---
    const int span = 40;
    for (int p = -5000; p < 5000; ++p) {
        const int d = idleDrift(p, span);
        CHECK(d >= 0 && d <= span);
    }
    CHECK(idleDrift(0, span) == 0);
    CHECK(idleDrift(kIdlePhasePeriod / 2, span) == span);
    CHECK(idleDrift(kIdlePhasePeriod, span) == 0);
    CHECK(idleDrift(0, 0) == 0);

    // --- renderer ---
    const int w = 320, h = 240;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3);

    // Black is exactly black: this is the burn-in-safe mode, no stray pixels.
    std::fill(buf.begin(), buf.end(), 0xAB);
    renderIdleRgb24(buf.data(), w, h, IdleMode::Black, 0);
    for (uint8_t v : buf)
        CHECK(v == 0);

    // LastFrame must not touch the buffer — it is the "leave it alone" mode.
    std::fill(buf.begin(), buf.end(), 0xAB);
    renderIdleRgb24(buf.data(), w, h, IdleMode::LastFrame, 0);
    for (uint8_t v : buf)
        CHECK(v == 0xAB);

    // Logo paints the background everywhere and the mark somewhere.
    std::fill(buf.begin(), buf.end(), 0xAB);
    renderIdleRgb24(buf.data(), w, h, IdleMode::Logo, 0);
    size_t fg = 0, bg = 0, other = 0;
    for (size_t i = 0; i + 2 < buf.size(); i += 3) {
        if (buf[i] == kIdleFgR && buf[i + 1] == kIdleFgG && buf[i + 2] == kIdleFgB)
            ++fg;
        else if (buf[i] == kIdleBgR && buf[i + 1] == kIdleBgG && buf[i + 2] == kIdleBgB)
            ++bg;
        else
            ++other;
    }
    // AA skirt may blend FG/BG (other > 0); solid FG and BG must still dominate.
    CHECK(fg > 0);
    CHECK(bg > fg);
    CHECK(other < fg); // feather is a thin edge, not a third palette

    // Solid FG must stay inside overscan margin. AA skirt may extend 1–3 px
    // outside the hit box but never invents solid FG outside the box.
    for (int p = 0; p < kIdlePhasePeriod; p += 7) {
        std::fill(buf.begin(), buf.end(), 0);
        renderIdleRgb24(buf.data(), w, h, IdleMode::Screensaver, p);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                const bool isFg =
                    buf[i] == kIdleFgR && buf[i + 1] == kIdleFgG && buf[i + 2] == kIdleFgB;
                if (!isFg)
                    continue;
                CHECK(x >= kIdleMargin && x < w - kIdleMargin);
                CHECK(y >= kIdleMargin && y < h - kIdleMargin);
            }
        }
    }

    // DDR idle is I420/YUV420p now. Black must be exact video black, while
    // logo/screensaver must preserve the renderer rather than degenerating to
    // a black-only frame.
    std::vector<uint8_t> yuv(static_cast<size_t>(w) * h * 3 / 2);
    CHECK(renderIdleYuv420p(yuv.data(), w, h, IdleMode::Black, 0));
    const size_t yBytes = static_cast<size_t>(w) * h;
    const size_t cBytes = yBytes / 4;
    for (size_t i = 0; i < yBytes; ++i)
        CHECK(yuv[i] == 16);
    for (size_t i = 0; i < cBytes; ++i) {
        CHECK(yuv[yBytes + i] == 128);
        CHECK(yuv[yBytes + cBytes + i] == 128);
    }

    std::vector<uint8_t> yuv2(yuv.size());
    CHECK(renderIdleYuv420p(yuv.data(), w, h, IdleMode::Screensaver, 0));
    CHECK(renderIdleYuv420p(yuv2.data(), w, h, IdleMode::Screensaver, kIdlePhasePeriod / 4));
    size_t nonBlackLuma = 0;
    size_t changed = 0;
    for (size_t i = 0; i < yBytes; ++i) {
        if (yuv[i] != 16)
            ++nonBlackLuma;
        if (yuv[i] != yuv2[i])
            ++changed;
    }
    CHECK(nonBlackLuma > yBytes / 2);
    CHECK(changed > 0);

    // A valid YUV doorbell carrying valid all-black pixels is still a regression:
    // the frame store cannot distinguish that from a deliberate black screen.
    // Grade the actual I420 pixels so logo/screensaver content cannot disappear
    // behind a formally valid payload again.
    constexpr uint8_t kBgY = 45, kBgU = 130, kBgV = 126;
    constexpr uint8_t kFgY = 157, kFgU = 53, kFgV = 169;
    auto sampleIsFg = [](int x, int y, const IdleRenderState& state) {
        uint8_t r = 0, g = 0, b = 0;
        idlePixelRgb(x, y, state, r, g, b);
        return r == kIdleFgR && g == kIdleFgG && b == kIdleFgB;
    };
    auto findSolid2x2 = [&](IdleMode mode, int phase, bool wantFg, int& outX, int& outY) {
        const IdleRenderState state = idleRenderState(w, h, mode, phase);
        for (int y = 0; y + 1 < h; y += 2) {
            for (int x = 0; x + 1 < w; x += 2) {
                bool solid = true;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        solid = solid && (sampleIsFg(x + dx, y + dy, state) == wantFg);
                if (solid) {
                    outX = x;
                    outY = y;
                    return true;
                }
            }
        }
        return false;
    };
    auto expectI420Sample = [&](const std::vector<uint8_t>& frame, int x, int y, uint8_t yy,
                                uint8_t uu, uint8_t vv) {
        const size_t yi = static_cast<size_t>(y) * w + x;
        const size_t ci = static_cast<size_t>(y / 2) * (w / 2) + (x / 2);
        CHECK(frame[yi] == yy);
        CHECK(frame[yBytes + ci] == uu);
        CHECK(frame[yBytes + cBytes + ci] == vv);
    };

    int fgX0 = 0, fgY0 = 0, bgX0 = 0, bgY0 = 0, fgX1 = 0, fgY1 = 0;
    CHECK(findSolid2x2(IdleMode::Screensaver, 0, true, fgX0, fgY0));
    CHECK(findSolid2x2(IdleMode::Screensaver, 0, false, bgX0, bgY0));
    CHECK(findSolid2x2(IdleMode::Screensaver, kIdlePhasePeriod / 4, true, fgX1, fgY1));
    expectI420Sample(yuv, fgX0, fgY0, kFgY, kFgU, kFgV);
    expectI420Sample(yuv, bgX0, bgY0, kBgY, kBgU, kBgV);
    expectI420Sample(yuv2, fgX1, fgY1, kFgY, kFgU, kFgV);
    CHECK(fgX0 != fgX1 || fgY0 != fgY1);

    if (fails) {
        std::fprintf(stderr, "test_osd_menu: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_osd_menu: OK\n");
    return 0;
}
