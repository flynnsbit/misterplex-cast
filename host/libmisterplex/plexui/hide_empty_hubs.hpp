#pragma once
// #220: an empty Home hub (Music / Photos / Videos / Recent Playlists on a server with
// nothing in them) drew its header with no tiles under it.
//
// HIDE_EMPTY_HUBS (misterplex.conf) / MPX_HIDE_EMPTY_HUBS (env wins). Default on.
// Unset and empty hide empty Home hubs. 0, off, false, and no keep them.
//
// On: a Home hub with no items is dropped before it is stored, so neither the
// header nor its row slot is drawn, and focus/nav count only hubs that have tiles.
// Off: Home hubs are kept exactly as the server sent them.
// Only Home hubs pass through this; library grids and section rows are untouched.

#include "plexui_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace misterplex {
namespace plexui {

inline bool hideEmptyHubsEnvOn(const char* e) {
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

// Default on (#220). An explicit 0/off wins. MPX_HIDE_EMPTY_HUBS wins over HIDE_EMPTY_HUBS.
inline bool hideEmptyHubsEnabled() {
#ifdef MUT_HEH_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_HIDE_EMPTY_HUBS"); e && e[0])
        return hideEmptyHubsEnvOn(e);
    if (const char* e = std::getenv("HIDE_EMPTY_HUBS"); e && e[0])
        return hideEmptyHubsEnvOn(e);
    return true;
#endif
}

// Drops Home hubs with no items when `on`. Order of the kept hubs is unchanged.
inline void filterEmptyHomeHubs(std::vector<PlexHubRow>& hubs, bool on) {
#if defined(MUT_HEH_ON_IGNORED)
    (void)hubs;
    (void)on;
    return;
#else
#ifdef MUT_HEH_OFF_STILL_BINDS
    on = true;
#endif
    if (!on)
        return;
    hubs.erase(std::remove_if(hubs.begin(), hubs.end(),
                              [](const PlexHubRow& h) {
#ifdef MUT_HEH_WRONG_CONTEXT
                                  return h.items.size() <= 1;   // wrong test: drops a real one-tile hub
#else
                                  return h.items.empty();
#endif
                              }),
               hubs.end());
#endif
}

}  // namespace plexui
}  // namespace misterplex
