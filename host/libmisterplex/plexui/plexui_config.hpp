#pragma once

#include "libmisterplex/browse_v2.hpp"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {
namespace plexui {

constexpr const char* kPlexUiEnv = "MPX_PLEXUI";
constexpr const char* kPlexUiDirEnv = "MPX_PLEXUI_DIR";
constexpr const char* kPlexUiTokenFileEnv = "MPX_PLEXUI_TOKEN_FILE";
constexpr const char* kDefaultPlexUiDir = "/media/fat/misterplex/plexui/";

// Unset means on: the on-device Plex TV browser is the product default.
// MPX_PLEXUI=0 opts out. MPX_PLEXUI=1 is an explicit on.
inline bool plexUiFlagOn() {
    const char* raw = std::getenv(kPlexUiEnv);
    if (!raw || !*raw)
        return true;
    return parseBrowseV2(raw) == BrowseV2Setting::On;
}

inline bool plexUiEnabled(bool browseV2On, bool browseMvpbOn) {
#ifndef MUT_PLEXUI_IGNORE_BROWSE_FLAGS
    if (!browseV2On || !browseMvpbOn)
        return false;
#else
    (void)browseV2On; (void)browseMvpbOn;
#endif
    return plexUiFlagOn();
}

// The daemon play hook must be installed whenever plexui is on, even if
// MPX_BROWSE_V2 is unset. Glass of ba8efac6 logged only "plexui input select"
// because startPlay called an empty hook and returned 0.
inline bool browsePlayWanted(bool plexUiOn, BrowseV2Setting v2) {
    return plexUiOn || v2 == BrowseV2Setting::On;
}

inline std::string plexUiDirFromEnvironment() {
    const char* raw = std::getenv(kPlexUiDirEnv);
    if (raw && *raw)
        return raw;
    return kDefaultPlexUiDir;
}

inline bool writeTokenFile(const std::string& path, const std::string& token) {
    if (path.empty() || token.empty())
        return false;
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return false;
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    const std::string payload = token + "\n";
    size_t off = 0;
    while (off < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + off, payload.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    std::string dir = ".";
    const size_t slash = path.find_last_of('/');
    if (slash == 0)
        dir = "/";
    else if (slash != std::string::npos)
        dir = path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
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
    return "misterplexd: Plex TV UI on (MPX_PLEXUI default; 0 opts out), fonts from " +
           plexUiDirFromEnvironment();
}

} // namespace plexui
} // namespace misterplex
