#pragma once
// N14: decode the stand-by card (JPEG/PNG) with the stb_image the PlexUI already vendors and register
// it with AdCardStore. Links against the daemon's existing stb_image (plexui_images.cpp).
#include "ad_break_card.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// stb_image is compiled into plexui_images.cpp (same extern decl as main.cpp's ambient art).
extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file,
                                     int desired_channels);
void stbi_image_free(void* retval_from_stbi_load);
}

namespace misterplex {

inline std::string adCardDefaultPath() {
    if (const char* e = std::getenv("MPX_AD_BREAK_CARD"); e && e[0])
        return e;
    return "/media/fat/misterplex/plexui/misterplex-ad-break.jpg";
}

inline bool adCardDecode(const std::string& bytes, AdCardImage& out) {
    int w = 0, h = 0, n = 0;
    if (bytes.size() < 16 || bytes.size() > (8u << 20))
        return false;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (!px)
        return false;
    if (w < 16 || h < 16 || w > 4096 || h > 4096) {
        stbi_image_free(px);
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + size_t(w) * size_t(h) * 4u);
    stbi_image_free(px);
    return true;
}

inline bool adCardLoadFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string bytes;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() <= (8u << 20))
        bytes.append(buf, n);
    std::fclose(f);
    AdCardImage im;
    if (!adCardDecode(bytes, im))
        return false;
    AdCardStore::get().set(std::move(im));
    return true;
}

}  // namespace misterplex
