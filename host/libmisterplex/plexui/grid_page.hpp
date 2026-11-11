#pragma once
// FEATURE-GAP #10 paging remainder: library grid past the first 200 titles.
//
// GRID_PAGE (misterplex.conf) / MPX_GRID_PAGE (env wins over GRID_PAGE).
// Default off. Off: fetchSectionGrid and fetchSectionGridGenre stay on the
// first page (X-Plex-Container-Start=0, X-Plex-Container-Size=200) and the
// grid does not ask for a later offset. On: moving past the loaded rows
// requests the next page at that start offset. Search, collections, seasons,
// and playback are not part of this flag.

#include "libmisterplex/browse_v2.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {

constexpr int kGridPageSize = 200;

inline bool gridPageEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// Default off. MPX_GRID_PAGE wins over GRID_PAGE when it is set.
inline bool gridPageEnabled() {
#ifdef MUT_GRID_PAGE_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_GRID_PAGE"); e && e[0])
        return gridPageEnvOn(e);
    return gridPageEnvOn(std::getenv("GRID_PAGE"));
#endif
}

// Start offset actually placed on the wire. Flag off is always the first page.
inline int gridPageStart(int start) {
    if (start < 0)
        start = 0;
#if defined(MUT_GRID_PAGE_IGNORE_START)
    return 0;
#elif defined(MUT_GRID_PAGE_OFF_STILL_PAGES)
    return start;
#else
    if (!gridPageEnabled())
        return 0;
    return start;
#endif
}

// False when the flag is off and the caller asked for a later page, so the
// helper does not hit PMS. The first page (start 0) stays allowed.
inline bool gridPageRequestAllowed(int start) {
#if defined(MUT_GRID_PAGE_OFF_STILL_PAGES)
    (void)start;
    return true;
#else
    if (start < 0)
        start = 0;
    if (gridPageEnabled())
        return true;
    return start == 0;
#endif
}

inline std::string gridPageAllQuery(int start) {
    const int s = gridPageStart(start);
    return "sort=titleSort&X-Plex-Container-Start=" + std::to_string(s) +
           "&X-Plex-Container-Size=" + std::to_string(kGridPageSize);
}

// genreId is already url-encoded. The genre grid has no sort= parameter.
inline std::string gridPageGenreQuery(const std::string& genreIdEncoded, int start) {
    const int s = gridPageStart(start);
    return "genre=" + genreIdEncoded + "&X-Plex-Container-Start=" + std::to_string(s) +
           "&X-Plex-Container-Size=" + std::to_string(kGridPageSize);
}

// MediaContainer totalSize, or -1 when the attribute is absent.
inline int gridPageTotalSize(const std::string& xml) {
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name != "MediaContainer")
            continue;
        const std::string t = el.get("totalSize");
        if (t.empty())
            return -1;
        char* end = nullptr;
        const long v = std::strtol(t.c_str(), &end, 10);
        if (end == t.c_str() || v < 0 || v > 10000000L)
            return -1;
        return static_cast<int>(v);
    }
    return -1;
}

// More rows exist past the ones already stored. Does not read the flag.
inline bool gridMoreLoaded(int loaded, int totalSize, bool lastFull) {
    if (loaded < 0)
        loaded = 0;
    if (totalSize >= 0)
        return loaded < totalSize;
    return lastFull && loaded > 0;
}

inline bool gridPageHasMore(int loaded, int totalSize, bool lastFull) {
    if (!gridPageEnabled())
        return false;
    return gridMoreLoaded(loaded, totalSize, lastFull);
}

} // namespace misterplex
