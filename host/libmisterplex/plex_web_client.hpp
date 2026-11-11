#pragma once

#include <string>

namespace misterplex {

// Shared product id for player GETs that do not name a box. One version
// string so those GETs, the link PIN, and timeline cannot drift.
inline constexpr char kPlexClientId[] = "misterplex";
inline constexpr char kPlexWebVersion[] = "4.125.0";

// plex.tv /api/v2/resources identifies this box by machineId (the --id),
// not the shared product id. MUT_LINKED_RESOURCES_SHARED_CLIENT sends that
// constant and the resources test fails.
inline std::string plexTvResourcesClientId(const std::string& machineId) {
#ifdef MUT_LINKED_RESOURCES_SHARED_CLIENT
    (void)machineId;
    return kPlexClientId;
#else
    return machineId;
#endif
}

} // namespace misterplex
