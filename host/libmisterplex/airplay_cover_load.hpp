#pragma once
// Decode AirPlay RAOP cover art (JPEG/PNG bytes from RTSP SET_PARAMETER) via the daemon's
// already-linked stb_image (plexui_images.cpp), downscale on ingest to <= 512 px for ARM RAM
// safety, and publish into AirplayCoverStore.
#include "airplay_now_playing.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file,
                                     int desired_channels);
void stbi_image_free(void* retval_from_stbi_load);
}

namespace misterplex {

inline constexpr size_t kAirplayCoverMaxBytes = 6u << 20;   // 6 MiB raw JPEG/PNG ceiling
inline constexpr int kAirplayCoverMaxSourceDim = 4096;
inline constexpr int kAirplayCoverStoreMaxDim = 720;

inline bool airplayCoverDecode(const uint8_t* bytes, size_t len, AirplayCoverImage& out) {
    if (!bytes || len < 16 || len > kAirplayCoverMaxBytes)
        return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes, static_cast<int>(len), &w, &h, &n, 4);
    if (!px)
        return false;
    if (w < 8 || h < 8 || w > kAirplayCoverMaxSourceDim || h > kAirplayCoverMaxSourceDim) {
        stbi_image_free(px);
        return false;
    }
    out = airplayDownscaleRgbaToMax(px, w, h, kAirplayCoverStoreMaxDim);
    stbi_image_free(px);
    return out.ok();
}

// Decode and store cover art; returns the new AirplayCoverStore generation (> 0 on success, 0 on clear/error).
inline uint64_t airplayCoverDecodeAndStore(const uint8_t* bytes, size_t len) {
    if (!bytes || len == 0) {
        AirplayCoverStore::get().clear();
        return 0;
    }
    AirplayCoverImage im;
    if (!airplayCoverDecode(bytes, len, im))
        return 0;
    return AirplayCoverStore::get().set(std::move(im));
}

}  // namespace misterplex
