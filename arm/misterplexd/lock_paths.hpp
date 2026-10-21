#pragma once
// The two host-global lock paths, in one place (T-DAEMON-LOCK-SHARED).
// Product builds return the fixed literals and never read the environment. Only host TEST builds
// (MPX_TEST_BUILD: build/misterplexd and the e2e scripts' own compiles, never arm-plexd*) may move
// BOTH paths into a private directory, so concurrent test jobs on one host stop colliding on /tmp.
// Never unlink either path: flock is on the inode (ProcessSingleton, FlockGuard).
#if defined(MPX_TEST_LOCK_DIR) && !defined(MPX_TEST_BUILD)
#error "MPX_TEST_LOCK_DIR is honoured only in an MPX_TEST_BUILD (host test) build"
#endif
#ifdef MPX_TEST_BUILD
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace misterplex {

#ifndef MPX_TEST_BUILD

constexpr const char* daemonLockPath() { return "/tmp/misterplexd.lock"; }
constexpr const char* spiLockPath() { return "/tmp/misterplex_spi.lock"; }

#else

// Precedence: the env MPX_LOCK_DIR, then the dir baked in at compile time (MPX_TEST_LOCK_DIR, set by
// the e2e scripts; it survives a launcher that clears the env, misterplexd-boot.sh's env -i), then
// the prod /tmp paths. A dir from either source must be absolute, existing and writable, else exit 2:
// never a silent fall back to /tmp. Logged once on stderr, so an e2e can prove that every participant
// (daemon, tools, the test's own flock) resolved the same two paths.
inline const std::string& testLockDir() {
    static const std::string dir = [] {
        const char* src = "env";
        const char* d = std::getenv("MPX_LOCK_DIR");
#ifdef MPX_TEST_LOCK_DIR
        if (d == nullptr) {
            d = MPX_TEST_LOCK_DIR;
            src = "baked";
        }
#endif
        if (d == nullptr) {
            std::fprintf(stderr, "mpx-lock-paths: source=default daemon=/tmp/misterplexd.lock "
                                 "spi=/tmp/misterplex_spi.lock\n");
            return std::string();
        }
        struct stat st {};
        if (d[0] != '/' || ::stat(d, &st) != 0 || !S_ISDIR(st.st_mode) || ::access(d, W_OK) != 0) {
            std::fprintf(stderr, "mpx-lock-paths: %s lock dir '%s' is not an absolute writable directory\n",
                         src, d);
            std::exit(2);
        }
        // /tmp itself (or /tmp/., a symlink or a bind mount of it) would land on the prod inodes while
        // looking compliant: compare by inode, not by spelling.
        struct stat tmp {};
        if (::stat("/tmp", &tmp) == 0 && st.st_dev == tmp.st_dev && st.st_ino == tmp.st_ino) {
            std::fprintf(stderr, "mpx-lock-paths: %s lock dir '%s' is /tmp itself (the prod lock dir)\n", src, d);
            std::exit(2);
        }
        char real[PATH_MAX];
        if (::realpath(d, real) == nullptr) {
            std::fprintf(stderr, "mpx-lock-paths: %s lock dir '%s': realpath failed\n", src, d);
            std::exit(2);
        }
        std::fprintf(stderr, "mpx-lock-paths: source=%s daemon=%s/misterplexd.lock spi=%s/misterplex_spi.lock\n",
                     src, real, real);
        return std::string(real);
    }();
    return dir;
}
inline const char* daemonLockPath() {
    static const std::string p = testLockDir().empty() ? std::string("/tmp/misterplexd.lock")
                                                       : testLockDir() + "/misterplexd.lock";
    return p.c_str();
}
inline const char* spiLockPath() {
    static const std::string p = testLockDir().empty() ? std::string("/tmp/misterplex_spi.lock")
                                                       : testLockDir() + "/misterplex_spi.lock";
    return p.c_str();
}
// Eager: every test-build TU that includes this header resolves both paths during static
// initialization, before main() and before any thread, so a bad dir exits 2 there and never inside a
// lazy init on a worker thread (atexit handlers under live threads).
inline const bool kLockPathsResolved = (daemonLockPath(), spiLockPath(), true);

#endif

} // namespace misterplex
