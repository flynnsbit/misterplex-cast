#pragma once
#include "airplay/ns.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace misterplex::airplay::yt_dlp {

inline std::string ytCacheDir(const std::string& conf = {}) {
    if (const char* v = std::getenv("MPX_YT_CACHE_DIR"); v && v[0])
        return v;
    if (const char* v = std::getenv("YT_CACHE_DIR"); v && v[0])
        return v;
    return conf.empty() ? "/media/fat/misterplex/yt-cache" : conf;
}

inline void ensureYtCacheDir() {
    const std::string dir = ytCacheDir();
    for (size_t end = 1; end <= dir.size(); ++end) {
        if (end != dir.size() && dir[end] != '/')
            continue;
        const std::string part = dir.substr(0, end);
        if (::mkdir(part.c_str(), 0755) == 0)
            continue;
        const int error = errno;
        struct stat st {};
        if (error == EEXIST && ::stat(part.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            continue;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            std::fprintf(stderr, "misterplexd: cannot create YouTube cache directory: %s\n",
                         error == EEXIST ? "path is not a directory" : std::strerror(error));
        return;
    }
}

inline void appendYtCacheArgs(std::vector<std::string>& args) {
    args.emplace_back("--cache-dir");
    args.emplace_back(ytCacheDir());
}

} // namespace misterplex::airplay::yt_dlp
