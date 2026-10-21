#include "libmisterplex/pixel_format.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    using namespace misterplex::pixel;

    assert(packRgb565(255, 0, 0) == 0xF800);
    assert(packRgb565(0, 255, 0) == 0x07E0);
    assert(packRgb565(0, 0, 255) == 0x001F);
    assert(packRgb565(255, 255, 255) == 0xFFFF);

    const std::vector<uint8_t> rgb = {
        255, 0, 0,     // red
        0,   255, 0,   // green
        0,   0,   255, // blue
        18,  52,  86,  // quantized mixed colour
    };
    std::vector<uint8_t> rgb565(rgb.size() / 3 * 2);
    rgb24ToRgb565Le(rgb.data(), rgb565.data(), rgb.size() / 3);

    assert(loadLe16(rgb565.data() + 0) == 0xF800);
    assert(loadLe16(rgb565.data() + 2) == 0x07E0);
    assert(loadLe16(rgb565.data() + 4) == 0x001F);
    assert(loadLe16(rgb565.data() + 6) == packRgb565(18, 52, 86));

    uint8_t r = 0, g = 0, b = 0;
    expandRgb565(0xF800, r, g, b);
    assert(r == 255 && g == 0 && b == 0);
    expandRgb565(0x07E0, r, g, b);
    assert(r == 0 && g == 255 && b == 0);
    expandRgb565(0x001F, r, g, b);
    assert(r == 0 && g == 0 && b == 255);

    std::vector<uint8_t> bgra(3 * 4);
    rgb565LeToBgra8888(rgb565.data(), bgra.data(), 3);
    assert((bgra[0] == 0 && bgra[1] == 0 && bgra[2] == 255 && bgra[3] == 255));
    assert((bgra[4] == 0 && bgra[5] == 255 && bgra[6] == 0 && bgra[7] == 255));
    assert((bgra[8] == 255 && bgra[9] == 0 && bgra[10] == 0 && bgra[11] == 255));

    yuvToRgb(128, 128, 128, r, g, b);
    assert(r == 128 && g == 128 && b == 128);
    yuvToRgb(16, 128, 128, r, g, b);
    assert(r == 16 && g == 16 && b == 16);
    yuvToRgb(76, 85, 255, r, g, b);
    assert(r == 254 && g == 0 && b == 0);

    return 0;
}
