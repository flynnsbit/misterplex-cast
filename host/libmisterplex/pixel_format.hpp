#pragma once

#include <cstddef>
#include <cstdint>

namespace misterplex::pixel {

inline uint16_t packRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline void storeLe16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>(v >> 8);
}

inline uint16_t loadLe16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0] | (static_cast<uint16_t>(src[1]) << 8));
}

inline void expandRgb565(uint16_t p, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t r5 = static_cast<uint8_t>((p >> 11) & 0x1F);
    const uint8_t g6 = static_cast<uint8_t>((p >> 5) & 0x3F);
    const uint8_t b5 = static_cast<uint8_t>(p & 0x1F);
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

inline void rgb24ToRgb565Le(const uint8_t* rgb, uint8_t* rgb565le, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        const uint16_t p = packRgb565(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);
        storeLe16(rgb565le + i * 2, p);
    }
}

inline void rgb565LeToBgra8888(const uint8_t* rgb565le, uint8_t* bgra, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t r = 0, g = 0, b = 0;
        expandRgb565(loadLe16(rgb565le + i * 2), r, g, b);
        bgra[i * 4 + 0] = b;
        bgra[i * 4 + 1] = g;
        bgra[i * 4 + 2] = r;
        bgra[i * 4 + 3] = 0xFF;
    }
}

inline uint8_t clamp8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return static_cast<uint8_t>(v);
}

inline void yuvToRgb(uint8_t y, uint8_t u, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const int uu = static_cast<int>(u) - 128;
    const int vv = static_cast<int>(v) - 128;
    r = clamp8((static_cast<int>(y) * 256 + 359 * vv) >> 8);
    g = clamp8((static_cast<int>(y) * 256 - 88 * uu - 183 * vv) >> 8);
    b = clamp8((static_cast<int>(y) * 256 + 454 * uu) >> 8);
}

} // namespace misterplex::pixel
