#pragma once
// FEATURE-GAP E8: a 4:3-only filter on the library grid.
//
// FILTER_4_3 (misterplex.conf) / MPX_FILTER_4_3 (env wins). Default off.
// Unset, empty, 0, off, false, and no stay off.
//
// Off: the library chip row and the grid match a build without this flag.
// aspectRatio is still stored on each item. Nothing is hidden.
// On: the chip row gains a "4:3" chip after All. Select toggles the filter.
// On keeps titles whose stored aspect is about 4:3, in the same order.
// Select again restores that grid. A genre or collection load keeps the
// toggle. Leaving the library clears it. There is no extra PMS query.
//
// aspectRatio is the PMS attribute on the item, or on the first Media child
// when the item itself has none. A title counts as 4:3 when that text parses
// to a display aspect in [1.30, 1.40] (4:3 is 1.333..., academy is 1.37,
// Plex "1.33"). "4:3", "4x3", and "4/3" are the same ratio. 16:9 (1.78),
// 1.85, 2.35, 2.39, and a missing or unparsed aspect are not 4:3 and are
// hidden while the filter is on. An empty grid is allowed.

#include "plexui_types.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex {

inline bool filter43EnvOn(const char* e) {
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

// Default off. MPX_FILTER_4_3 wins over FILTER_4_3 when it is set.
inline bool filter43Enabled() {
#ifdef MUT_FILTER43_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_FILTER_4_3"); e && e[0])
        return filter43EnvOn(e);
    return filter43EnvOn(std::getenv("FILTER_4_3"));
#endif
}

// PMS aspectRatio text. One number is the display aspect (width/height).
// Two numbers separated by ':' or 'x' or '/' are width and height.
// Empty or unparsed is 0.
inline double parseAspectRatio(const std::string& text) {
    std::size_t b = 0;
    while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b])))
        ++b;
    std::size_t e = text.size();
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])))
        --e;
    if (b >= e)
        return 0;
    std::string s = text.substr(b, e - b);
    for (char& c : s) {
        if (c == 'x' || c == 'X' || c == ':' || c == '/')
            c = ' ';
    }
    char* end = nullptr;
    const double a = std::strtod(s.c_str(), &end);
    if (end == s.c_str())
        return 0;
    while (*end && std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (*end) {
        char* end2 = nullptr;
        const double h = std::strtod(end, &end2);
        if (end2 != end && h > 0.0)
            return a / h;
    }
    if (a > 0.0)
        return a;
    return 0;
}

// ~4:3. 4:3 and academy sit inside. 16:9 and wider do not.
inline bool aspectIs43(double ratio) {
#ifdef MUT_FILTER43_KEEP_WIDE
    return ratio > 0.0;
#else
    return ratio >= 1.30 && ratio <= 1.40;
#endif
}

inline void storeItemAspect(plexui::PlexItem& it, const std::string& raw) {
#ifdef MUT_FILTER43_DROP_ASPECT
    (void)raw;
    it.aspectRatio.clear();
#else
    if (it.aspectRatio.empty() && !raw.empty())
        it.aspectRatio = raw;
#endif
}

inline bool itemIs43(const plexui::PlexItem& it) {
    return aspectIs43(parseAspectRatio(it.aspectRatio));
}

// Flag off, or the toggle off: the same items in the same order.
// Flag on and the toggle on: only ~4:3 items, same order.
inline std::vector<plexui::PlexItem> libraryGridFiltered(const std::vector<plexui::PlexItem>& items,
                                                        bool toggleOn) {
#ifdef MUT_FILTER43_IGNORE_TOGGLE
    (void)toggleOn;
    return items;
#else
#ifdef MUT_FILTER43_FILTER_WHEN_OFF
    const bool on = true;
    (void)toggleOn;
#else
    const bool on = filter43Enabled() && toggleOn;
#endif
    if (!on)
        return items;
#endif
    std::vector<plexui::PlexItem> out;
    out.reserve(items.size());
    for (const plexui::PlexItem& it : items) {
        if (itemIs43(it))
            out.push_back(it);
    }
    return out;
}

// The "4:3" chip sits after All. Flag off leaves the chip list alone.
inline void insertFilter43Chip(std::vector<plexui::GridCategoryChip>& chips) {
#ifndef MUT_FILTER43_CHIP_WHEN_OFF
    if (!filter43Enabled())
        return;
#endif
    plexui::GridCategoryChip c;
    c.kind = plexui::GridCategoryChip::Kind::Aspect43;
    c.title = "4:3";
    std::size_t at = 0;
    if (!chips.empty() && chips.front().kind == plexui::GridCategoryChip::Kind::All)
        at = 1;
    chips.insert(chips.begin() + static_cast<std::ptrdiff_t>(at), std::move(c));
}

} // namespace misterplex
