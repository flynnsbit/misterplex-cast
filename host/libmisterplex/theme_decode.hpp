#pragma once
// GH#235: decode a Plex show-theme payload (MP3/AAC in memory) to s16le stereo
// 48 kHz for pushOneShotPcm. Does not open a play session. Caps at 2 s so the
// MrAudio ring cannot be filled with a full theme file. Token never appears
// here (the GET already used a header).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace misterplex {

inline constexpr std::size_t kThemePcmByteCap = 48000u * 4u * 2u; // 2 s

// Empty on failure, empty input, or no audio stream. Never returns more than
// kThemePcmByteCap bytes. Size is always a multiple of 4 (stereo s16le).
std::vector<uint8_t> themeDecodePcm(const uint8_t* data, std::size_t n);

} // namespace misterplex
