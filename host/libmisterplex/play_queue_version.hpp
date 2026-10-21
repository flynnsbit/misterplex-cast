#pragma once
// playQueueVersion the companion reports after a cast replaces the queue.
// A new playQueueId that reuses the previous version is ignored by Plex Web.

#include <cstdlib>
#include <string>

namespace misterplex {

inline std::string playQueueVersionAfterReplace(const std::string& prevId,
                                                const std::string& prevVersion,
                                                const std::string& nextId,
                                                const std::string& requested) {
    const bool replaced = !prevId.empty() && !nextId.empty() && prevId != nextId;
    if (!replaced)
        return requested.empty() ? (prevVersion.empty() ? "1" : prevVersion) : requested;
    if (!requested.empty() && requested != prevVersion)
        return requested;
    long v = 0;
    if (!prevVersion.empty()) {
        char* end = nullptr;
        v = std::strtol(prevVersion.c_str(), &end, 10);
        if (end == prevVersion.c_str())
            v = 0;
    }
    if (v < 0)
        v = 0;
    return std::to_string(v + 1);
}

}  // namespace misterplex
