#pragma once
// Present decoded RGB frames via MiSTer /dev/fb0 (HPS framebuffer → FPGA ascal).
// Phase 2 path while SDRAM/native decode matures. Matches console-core principle:
// FPGA owns final scanout; ARM only fills the frame store.

#include <cstdint>
#include <string>

namespace misterplex {

class FbPresent {
public:
    ~FbPresent();

    // Open /dev/fb0 (or path). Returns false if unavailable.
    bool open(const char* path = "/dev/fb0");
    void close();
    bool ok() const { return fd_ >= 0 && mem_ != nullptr; }

    int width() const { return width_; }
    int height() const { return height_; }
    int bpp() const { return bpp_; }

    // Copy packed RGB24 (w*h*3) centered into ARGB8888 or RGB565 fb.
    // Returns false on size mismatch / not open.
    bool blitRgb24(const uint8_t* rgb, int w, int h);
    // Copy packed BGRA8888 (w*h*4), matching little-endian 32bpp fb layout.
    bool blitBgra32(const uint8_t* bgra, int w, int h);
    // Copy packed RGB565 little-endian (w*h*2).
    bool blitRgb565Le(const uint8_t* rgb565le, int w, int h);
    // Convert planar YUV420p/I420 (Y, U, V) into the mapped framebuffer.
    bool blitYuv420p(const uint8_t* yuv420p, int w, int h);

    // Clear to black
    void clear();

    std::string info() const;

private:
    int fd_ = -1;
    uint8_t* mem_ = nullptr;
    size_t mapLen_ = 0;
    int width_ = 0;
    int height_ = 0;
    int bpp_ = 32;
    int stride_ = 0; // bytes per line
};

} // namespace misterplex
