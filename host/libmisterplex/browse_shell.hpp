#pragma once
// Library Browse redesign (reviewer 1121): thin UX shell on the plexui data plane.
// MPX_BROWSE_SHELL unset or 1: this shell is the product browse when plexui would open.
// MPX_BROWSE_SHELL=0: opt out to the old Plex TV path.

#include "libmisterplex/browse_v2.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {

constexpr const char* kBrowseShellEnv = "MPX_BROWSE_SHELL";
constexpr const char* kBrowseShellRamMbEnv = "MPX_BROWSE_RAM_CACHE_MB";
constexpr const char* kBrowseShellDiskMbEnv = "MPX_BROWSE_DISK_CACHE_MB";
constexpr const char* kBrowseShellDirEnv = "MPX_BROWSE_DIR";
constexpr const char* kBrowsePrefetchAllEnv = "MPX_BROWSE_PREFETCH_ALL";
constexpr const char* kDefaultBrowseShellDir = "/media/fat/misterplex/browse/";

constexpr std::size_t kBrowseShellRamCacheDefaultMb = 12;
constexpr std::size_t kBrowseShellDiskCacheDefaultMb = 96;
constexpr std::size_t kBrowseShellDiskCacheMinMb = 64;
constexpr std::size_t kBrowseShellDiskCacheMaxMb = 128;
constexpr int kBrowseShellSearchDebounceMs = 150;
constexpr int kBrowseShellThumbBudgetMs = 400;
constexpr int kBrowseShellPrefetchAllMax = 2000;

inline bool browseShellEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    return parseBrowseV2(e) == BrowseV2Setting::On;
}

// Unset means on: the redesigned shell is the product default.
// MPX_BROWSE_SHELL=0 opts out to plexui Home. MPX_BROWSE_SHELL=1 is explicit on.
inline bool browseShellEnabled() {
#ifdef MUT_BROWSE_SHELL_DEFAULT_OFF
    const char* raw = std::getenv(kBrowseShellEnv);
    return browseShellEnvOn(raw);
#else
    const char* raw = std::getenv(kBrowseShellEnv);
    if (!raw || !*raw)
        return true;
    return parseBrowseV2(raw) == BrowseV2Setting::On;
#endif
}

inline std::size_t browseShellRamCacheBytes() {
    std::size_t mb = kBrowseShellRamCacheDefaultMb;
    if (const char* e = std::getenv(kBrowseShellRamMbEnv); e && e[0]) {
        char* end = nullptr;
        const long v = std::strtol(e, &end, 10);
        if (end != e && *end == '\0' && v > 0)
            mb = std::min<std::size_t>(static_cast<std::size_t>(v), kBrowseShellRamCacheDefaultMb);
    }
    return mb * 1024u * 1024u;
}

inline std::size_t browseShellDiskCacheBytes() {
    std::size_t mb = kBrowseShellDiskCacheDefaultMb;
    if (const char* e = std::getenv(kBrowseShellDiskMbEnv); e && e[0]) {
        char* end = nullptr;
        const long v = std::strtol(e, &end, 10);
        if (end != e && v >= 0)
            mb = static_cast<std::size_t>(v);
    }
    if (mb < kBrowseShellDiskCacheMinMb)
        mb = kBrowseShellDiskCacheMinMb;
    if (mb > kBrowseShellDiskCacheMaxMb)
        mb = kBrowseShellDiskCacheMaxMb;
    return mb * 1024u * 1024u;
}

inline std::string browseShellDir() {
    if (const char* e = std::getenv(kBrowseShellDirEnv); e && e[0])
        return e;
    return kDefaultBrowseShellDir;
}

inline bool browsePrefetchAllEnabled() {
    const char* e = std::getenv(kBrowsePrefetchAllEnv);
    if (!e || !e[0])
        return false;
    return browseShellEnvOn(e);
}

inline int64_t browseShellNowMs() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

// Compact speed line Glass greps. Always on at info level. One line per action.
inline std::string formatBrowseMs(const char* action, int64_t ms) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "BROWSE_MS action=%s ms=%lld", action ? action : "?",
                  static_cast<long long>(ms));
    return buf;
}

inline const char* browseShellActionName(const char* key) { return key ? key : "keypress"; }

inline char browseShellTextKey(unsigned code, bool extended) {
    if (code == 0x5A)
        return '\n';
    if (extended)
        return 0;
    static const struct { unsigned code; char ch; } keys[] = {
        {0x1C, 'a'}, {0x32, 'b'}, {0x21, 'c'}, {0x23, 'd'}, {0x24, 'e'}, {0x2B, 'f'},
        {0x34, 'g'}, {0x33, 'h'}, {0x43, 'i'}, {0x3B, 'j'}, {0x42, 'k'}, {0x4B, 'l'},
        {0x3A, 'm'}, {0x31, 'n'}, {0x44, 'o'}, {0x4D, 'p'}, {0x15, 'q'}, {0x2D, 'r'},
        {0x1B, 's'}, {0x2C, 't'}, {0x3C, 'u'}, {0x2A, 'v'}, {0x1D, 'w'}, {0x22, 'x'},
        {0x35, 'y'}, {0x1A, 'z'}, {0x45, '0'}, {0x16, '1'}, {0x1E, '2'}, {0x26, '3'},
        {0x25, '4'}, {0x2E, '5'}, {0x36, '6'}, {0x3D, '7'}, {0x3E, '8'}, {0x46, '9'},
        {0x29, ' '}, {0x66, '\b'}, {0x4E, '-'}, {0x4C, ';'}, {0x49, '.'},
        {0x4A, '/'}, {0x52, '\''},
    };
    for (const auto& key : keys)
        if (key.code == code)
            return key.ch;
    return 0;
}

inline std::string formatBrowseShellNotice(bool on) {
    if (!on)
        return {};
    return "misterplexd: browse shell on (MPX_BROWSE_SHELL default; 0 opts out to Plex TV UI)";
}

} // namespace misterplex
