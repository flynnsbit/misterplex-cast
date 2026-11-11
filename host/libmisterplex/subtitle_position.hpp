#pragma once
// Soft-sub placement preference (DESIGN-soft-subs §6, §8). Overlay only.
// PMS burn-in URLs are not built here.
//
// safe (default): when the raster is letterboxed, bottom lines sit in the
// bottom black bar, still inside the bar's title-safe area.
// picture: today's placement, over the picture and above the transport bar.
// With no letterbox bar, safe uses the picture placement.

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace misterplex {

enum class SubtitlePosition : std::uint8_t { Safe, Picture };

inline const char* subtitlePositionName(SubtitlePosition p) {
    return p == SubtitlePosition::Picture ? "picture" : "safe";
}

// Empty, junk, or an unknown token is safe (the design default).
inline SubtitlePosition parseSubtitlePosition(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isspace(c))
            continue;
        s.push_back(static_cast<char>(std::tolower(c)));
    }
#ifdef MUT_SUBPOS_PARSE_SWAP
    if (s == "safe")
        return SubtitlePosition::Picture;
    if (s == "picture")
        return SubtitlePosition::Safe;
#else
    if (s == "picture")
        return SubtitlePosition::Picture;
    if (s == "safe")
        return SubtitlePosition::Safe;
#endif
#ifdef MUT_SUBPOS_DEFAULT_PICTURE
    return SubtitlePosition::Picture;
#else
    return SubtitlePosition::Safe;
#endif
}

inline std::atomic<int>& subtitlePositionStore() {
#ifdef MUT_SUBPOS_DEFAULT_PICTURE
    static std::atomic<int> v{static_cast<int>(SubtitlePosition::Picture)};
#else
    static std::atomic<int> v{static_cast<int>(SubtitlePosition::Safe)};
#endif
    return v;
}

inline SubtitlePosition subtitlePosition() {
    return subtitlePositionStore().load(std::memory_order_relaxed) == static_cast<int>(SubtitlePosition::Picture)
               ? SubtitlePosition::Picture
               : SubtitlePosition::Safe;
}

inline void setSubtitlePosition(SubtitlePosition p) {
    subtitlePositionStore().store(static_cast<int>(p), std::memory_order_relaxed);
}

}  // namespace misterplex
