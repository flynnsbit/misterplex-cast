#pragma once

#include "libmisterplex/browse_v2.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace misterplex {
namespace plexui {

constexpr const char* kPlexUiEnv = "MPX_PLEXUI";
constexpr const char* kPlexUiDirEnv = "MPX_PLEXUI_DIR";
constexpr const char* kPlexUiTokenFileEnv = "MPX_PLEXUI_TOKEN_FILE";
constexpr const char* kDefaultPlexUiDir = "/media/fat/misterplex/plexui/";

inline bool plexUiEnabled(bool browseV2On, bool browseMvpbOn) {
#ifndef MUT_PLEXUI_IGNORE_BROWSE_FLAGS
    if (!browseV2On || !browseMvpbOn)
        return false;
#else
    (void)browseV2On; (void)browseMvpbOn;
#endif
    return parseBrowseV2(std::getenv(kPlexUiEnv)) == BrowseV2Setting::On;
}

inline std::string plexUiDirFromEnvironment() {
    const char* raw = std::getenv(kPlexUiDirEnv);
    if (raw && *raw)
        return raw;
    return kDefaultPlexUiDir;
}

inline std::string readTokenFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.pop_back();
    size_t start = 0;
    while (start < line.size() && line[start] == ' ')
        ++start;
    return line.substr(start);
}

inline std::string plexUiAccountToken(const std::string& confToken) {
    const char* file = std::getenv(kPlexUiTokenFileEnv);
    if (file && *file) {
        const std::string t = readTokenFile(file);
        if (!t.empty())
            return t;
    }
    return confToken;
}

inline std::string formatPlexUiNotice(bool on) {
    if (!on)
        return {};
    return "misterplexd: LAB MPX_PLEXUI=1 -> Plex TV UI (direct PMS), fonts from " +
           plexUiDirFromEnvironment();
}

} // namespace plexui
} // namespace misterplex
