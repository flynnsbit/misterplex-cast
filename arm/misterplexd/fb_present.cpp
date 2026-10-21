#include "fb_present.hpp"

#include "libmisterplex/pixel_format.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <linux/fb.h>
#include <sstream>

namespace misterplex {

FbPresent::~FbPresent() { close(); }

bool FbPresent::open(const char* path) {
    close();
    fd_ = ::open(path, O_RDWR);
    if (fd_ < 0) {
        std::perror("fb open");
        return false;
    }

    fb_var_screeninfo vinfo{};
    fb_fix_screeninfo finfo{};
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
        ioctl(fd_, FBIOGET_FSCREENINFO, &finfo) != 0) {
        std::perror("fb ioctl");
        close();
        return false;
    }

    width_ = static_cast<int>(vinfo.xres);
    height_ = static_cast<int>(vinfo.yres);
    bpp_ = static_cast<int>(vinfo.bits_per_pixel);
    stride_ = static_cast<int>(finfo.line_length);
    mapLen_ = static_cast<size_t>(finfo.smem_len);
    if (mapLen_ == 0)
        mapLen_ = static_cast<size_t>(stride_) * static_cast<size_t>(height_);

    mem_ = static_cast<uint8_t*>(
        mmap(nullptr, mapLen_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (mem_ == MAP_FAILED) {
        std::perror("fb mmap");
        mem_ = nullptr;
        close();
        return false;
    }
    return true;
}

void FbPresent::close() {
    if (mem_ && mem_ != MAP_FAILED) {
        munmap(mem_, mapLen_);
        mem_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    mapLen_ = 0;
}

void FbPresent::clear() {
    if (!ok())
        return;
    std::memset(mem_, 0, mapLen_);
}

std::string FbPresent::info() const {
    std::ostringstream o;
    o << width_ << "x" << height_ << " bpp=" << bpp_ << " stride=" << stride_;
    return o.str();
}

bool FbPresent::blitRgb24(const uint8_t* rgb, int w, int h) {
    if (!ok() || !rgb || w <= 0 || h <= 0)
        return false;

    // Letterbox into fb
    const int dstW = width_;
    const int dstH = height_;
    const int x0 = (dstW > w) ? (dstW - w) / 2 : 0;
    const int y0 = (dstH > h) ? (dstH - h) / 2 : 0;
    const int copyW = (w < dstW) ? w : dstW;
    const int copyH = (h < dstH) ? h : dstH;

    if (bpp_ == 32 || bpp_ == 24) {
        const int dstStep = bpp_ / 8;
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src = rgb + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
            uint8_t* dst = mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                           static_cast<size_t>(x0) * static_cast<size_t>(dstStep);
            for (int x = 0; x < copyW; ++x) {
                // ARGB8888 little-endian: B G R A (MiSTer often BGR order with RxB flag)
                dst[0] = src[2]; // B
                dst[1] = src[1]; // G
                dst[2] = src[0]; // R
                if (dstStep == 4)
                    dst[3] = 0xFF;
                dst += dstStep;
                src += 3;
            }
        }
        return true;
    }

    if (bpp_ == 16) {
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src = rgb + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
            uint16_t* dst = reinterpret_cast<uint16_t*>(
                mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                static_cast<size_t>(x0) * 2);
            for (int x = 0; x < copyW; ++x) {
                const unsigned r = src[0] >> 3;
                const unsigned g = src[1] >> 2;
                const unsigned b = src[2] >> 3;
                *dst++ = static_cast<uint16_t>((r << 11) | (g << 5) | b);
                src += 3;
            }
        }
        return true;
    }

    return false;
}

bool FbPresent::blitBgra32(const uint8_t* bgra, int w, int h) {
    if (!ok() || !bgra || w <= 0 || h <= 0)
        return false;

    const int dstW = width_;
    const int dstH = height_;
    const int x0 = (dstW > w) ? (dstW - w) / 2 : 0;
    const int y0 = (dstH > h) ? (dstH - h) / 2 : 0;
    const int copyW = (w < dstW) ? w : dstW;
    const int copyH = (h < dstH) ? h : dstH;

    if (bpp_ == 32) {
        const size_t rowBytes = static_cast<size_t>(copyW) * 4;
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src = bgra + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
            uint8_t* dst = mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                           static_cast<size_t>(x0) * 4;
            std::memcpy(dst, src, rowBytes);
        }
        return true;
    }

    if (bpp_ == 24) {
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src = bgra + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
            uint8_t* dst = mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                           static_cast<size_t>(x0) * 3;
            for (int x = 0; x < copyW; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst += 3;
                src += 4;
            }
        }
        return true;
    }

    if (bpp_ == 16) {
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src = bgra + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
            uint16_t* dst = reinterpret_cast<uint16_t*>(
                mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                static_cast<size_t>(x0) * 2);
            for (int x = 0; x < copyW; ++x) {
                *dst++ = pixel::packRgb565(src[2], src[1], src[0]);
                src += 4;
            }
        }
        return true;
    }

    return false;
}

bool FbPresent::blitRgb565Le(const uint8_t* rgb565le, int w, int h) {
    if (!ok() || !rgb565le || w <= 0 || h <= 0)
        return false;

    const int dstW = width_;
    const int dstH = height_;
    const int x0 = (dstW > w) ? (dstW - w) / 2 : 0;
    const int y0 = (dstH > h) ? (dstH - h) / 2 : 0;
    const int copyW = (w < dstW) ? w : dstW;
    const int copyH = (h < dstH) ? h : dstH;

    if (bpp_ == 16) {
        const size_t rowBytes = static_cast<size_t>(copyW) * 2;
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src = rgb565le + static_cast<size_t>(y) * static_cast<size_t>(w) * 2;
            uint8_t* dst = mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                           static_cast<size_t>(x0) * 2;
            std::memcpy(dst, src, rowBytes);
        }
        return true;
    }

    if (bpp_ == 32 || bpp_ == 24) {
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* src =
                rgb565le + static_cast<size_t>(y) * static_cast<size_t>(w) * 2;
            uint8_t* dst = mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                           static_cast<size_t>(x0) * static_cast<size_t>(bpp_ / 8);
            for (int x = 0; x < copyW; ++x) {
                uint8_t r = 0, g = 0, b = 0;
                pixel::expandRgb565(pixel::loadLe16(src), r, g, b);
                dst[0] = b;
                dst[1] = g;
                dst[2] = r;
                if (bpp_ == 32)
                    dst[3] = 0xFF;
                dst += bpp_ / 8;
                src += 2;
            }
        }
        return true;
    }

    return false;
}

bool FbPresent::blitYuv420p(const uint8_t* yuv420p, int w, int h) {
    if (!ok() || !yuv420p || w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return false;

    const int dstW = width_;
    const int dstH = height_;
    const int x0 = (dstW > w) ? (dstW - w) / 2 : 0;
    const int y0 = (dstH > h) ? (dstH - h) / 2 : 0;
    const int copyW = (w < dstW) ? w : dstW;
    const int copyH = (h < dstH) ? h : dstH;
    const uint8_t* yPlane = yuv420p;
    const uint8_t* uPlane = yPlane + static_cast<size_t>(w) * static_cast<size_t>(h);
    const uint8_t* vPlane = uPlane + static_cast<size_t>(w / 2) * static_cast<size_t>(h / 2);

    if (bpp_ == 32 || bpp_ == 24) {
        const int dstStep = bpp_ / 8;
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* yRow = yPlane + static_cast<size_t>(y) * w;
            const uint8_t* uRow = uPlane + static_cast<size_t>(y / 2) * (w / 2);
            const uint8_t* vRow = vPlane + static_cast<size_t>(y / 2) * (w / 2);
            uint8_t* dst = mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                           static_cast<size_t>(x0) * static_cast<size_t>(dstStep);
            for (int x = 0; x < copyW; ++x) {
                uint8_t r = 0, g = 0, b = 0;
                pixel::yuvToRgb(yRow[x], uRow[x / 2], vRow[x / 2], r, g, b);
                dst[0] = b;
                dst[1] = g;
                dst[2] = r;
                if (dstStep == 4)
                    dst[3] = 0xFF;
                dst += dstStep;
            }
        }
        return true;
    }

    if (bpp_ == 16) {
        for (int y = 0; y < copyH; ++y) {
            const uint8_t* yRow = yPlane + static_cast<size_t>(y) * w;
            const uint8_t* uRow = uPlane + static_cast<size_t>(y / 2) * (w / 2);
            const uint8_t* vRow = vPlane + static_cast<size_t>(y / 2) * (w / 2);
            uint16_t* dst = reinterpret_cast<uint16_t*>(
                mem_ + static_cast<size_t>(y0 + y) * static_cast<size_t>(stride_) +
                static_cast<size_t>(x0) * 2);
            for (int x = 0; x < copyW; ++x) {
                uint8_t r = 0, g = 0, b = 0;
                pixel::yuvToRgb(yRow[x], uRow[x / 2], vRow[x / 2], r, g, b);
                *dst++ = pixel::packRgb565(r, g, b);
            }
        }
        return true;
    }

    return false;
}

} // namespace misterplex
