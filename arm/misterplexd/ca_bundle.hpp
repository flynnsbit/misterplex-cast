#pragma once
// Shared CA bundle lookup. HTTPS Date, the YouTube lounge client, and
// plex.tv sign-in all need the same file. Header-only so a second
// translation unit does not define mpx::caBundlePath() again.
//
// First non-empty regular file, or "" when none qualify:
//   $MPX_CA_BUNDLE
//   /media/fat/linux/misterplex-cacert.pem
//   /etc/ssl/certs/ca-certificates.crt
//   /etc/ssl/certs/cacert.pem
//   /etc/ssl/cert.pem
// An empty or truncated file does not win; the next candidate is tried.
// The last two are all that stock MiSTer (a lab MiSTer) has today: a 2021 bundle
// and a symlink to it. stat() follows that symlink. A stock image has no
// ca-certificates.crt, so the release package also installs misterplex-cacert.pem.
//
// kCaBundlePaths and caBundlePathPick match the plexui-link-signin shim so
// its tests compile against this header.

#include <cstdlib>
#include <string>

#include <sys/stat.h>

namespace mpx {

constexpr const char* kCaBundleEnv = "MPX_CA_BUNDLE";
constexpr const char* kCaBundlePaths[] = {
    "/media/fat/linux/misterplex-cacert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/certs/cacert.pem",
    "/etc/ssl/cert.pem",
};
constexpr int kCaBundlePathCount = 4;

inline bool caBundleFileUsable(const char* path) {
    if (!path || !*path)
        return false;
    struct stat st {};
    return ::stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

template <typename Usable>
inline std::string caBundlePathPick(const char* env, const char* const* paths, int count,
                                    Usable usable) {
    if (env && *env && usable(env))
        return env;
    if (!paths || count <= 0)
        return {};
    for (int i = 0; i < count; ++i) {
        if (paths[i] && usable(paths[i]))
            return paths[i];
    }
    return {};
}

inline std::string caBundlePath() {
    return caBundlePathPick(std::getenv(kCaBundleEnv), kCaBundlePaths, kCaBundlePathCount,
                            caBundleFileUsable);
}

} // namespace mpx
