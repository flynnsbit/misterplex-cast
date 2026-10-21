// mplex_ddr_probe: K1 KM-3 ABI check of /dev/mplex_ddr on the MiSTer (k1-kmod-wc-window-note-1004 §6).
// READ-ONLY on DDR (review 19:01 F: KM-3 on a lab MiSTer is read-only; no scratch write): it only opens, ioctls and maps, and
// never touches mapped memory. Prints one line per check and "MPLEX_PROBE_OK" or "MPLEX_PROBE_FAIL n". --hold S
// keeps the idle-FB mapping open for S seconds (KM-6: rmmod must answer EBUSY meanwhile). Build:
// -D_FILE_OFFSET_BITS=64 (mmap2 passes the page offset, so a huge pgoff reaches the driver instead of failing in
// libc).
#include "libmisterplex/mplex_ddr_abi.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MPLEX_PROBE_DEV
#define MPLEX_PROBE_DEV "/dev/mplex_ddr"
#endif

using namespace misterplex;

static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64");

namespace {
int fails = 0;
void line(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++fails;
}
std::string e(int err) { return std::string(" (") + std::strerror(err) + ")"; }

// mmap expectation: want 0 = must succeed, else must fail with that errno.
void tryMap(int fd, const char* what, size_t len, int prot, int flags, unsigned long pgoff, int wantErr) {
    errno = 0;
    void* p = ::mmap(nullptr, len, prot, flags, fd, static_cast<off_t>(pgoff) * 4096);
    const int err = errno;
    if (wantErr == 0) {
        line(p != MAP_FAILED, std::string(what) + (p == MAP_FAILED ? e(err) : ""));
    } else {
        line(p == MAP_FAILED && err == wantErr,
             std::string(what) + " refused" + (p == MAP_FAILED ? e(err) : std::string(" -- it MAPPED")));
    }
    if (p != MAP_FAILED)
        ::munmap(p, len);
}
} // namespace

int main(int argc, char** argv) {
    int holdS = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--hold") && i + 1 < argc)
            holdS = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: %s [--hold SECONDS]   (read-only)\n", argv[0]);
            return 2;
        }
    }
    // O_RDWR: Linux clears VM_SHARED on a MAP_SHARED mapping of a read-only fd, and the idle-FB key requires it.
    const int fd = ::open(MPLEX_PROBE_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("FAIL open %s%s\nMPLEX_PROBE_FAIL 1\n", MPLEX_PROBE_DEV, e(errno).c_str());
        return 1;
    }
    uint32_t caps = 0;
    const int capsRc = ::ioctl(fd, MPLEX_IOC_CAPS, &caps);
    line(capsRc == 0 && caps == kMplexCapIdleFbWc,
         "CAPS = " + std::to_string(caps) + (capsRc ? e(errno) : std::string()) + " (want 1)");

    // Unchanged ABI: offset 0 (8 MiB WC) and the cached slots (pgoff 0x800, 4 MiB) + CACHED_PHYS.
    tryMap(fd, "offset 0, 8 MiB, MAP_SHARED", 0x800000, PROT_READ | PROT_WRITE, MAP_SHARED, 0, 0);
    MplexCachedPhys cp{};
    const int cpRc = ::ioctl(fd, MPLEX_IOC_CACHED_PHYS, &cp);
    line(cpRc == 0 && cp.slot_bytes == kMplexCachedSlotBytes && cp.phys[0] && cp.phys[1],
         "CACHED_PHYS slot_bytes=" + std::to_string(cp.slot_bytes) + (cpRc ? e(errno) : std::string()));
    if (cpRc == 0)
        tryMap(fd, "pgoff 0x800, 4 MiB cached slots", 2 * kMplexCachedSlotBytes, PROT_READ | PROT_WRITE, MAP_SHARED,
               kMplexCachedPgoff, 0);

    // New: the idle-FB window, and the K1-N2 negatives.
    tryMap(fd, "pgoff 0x1600, 2 MiB, MAP_SHARED (idle FB)", kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_SHARED,
           kMplexIdleFbPgoff, 0);
    tryMap(fd, "pgoff 0x1600, 1 MiB", 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, kMplexIdleFbPgoff, EINVAL);
    tryMap(fd, "pgoff 0x1600, 4 MiB", 0x400000, PROT_READ | PROT_WRITE, MAP_SHARED, kMplexIdleFbPgoff, EINVAL);
    tryMap(fd, "pgoff 0x1601, 2 MiB", kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_SHARED, kMplexIdleFbPgoff + 1,
           EINVAL);
    tryMap(fd, "pgoff 0x1600, MAP_PRIVATE", kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, kMplexIdleFbPgoff,
           EINVAL);
    tryMap(fd, "pgoff 0x1600, PROT_EXEC", kMplexIdleFbBytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED,
           kMplexIdleFbPgoff, EINVAL);
    tryMap(fd, "huge pgoff 0xFFFFF", kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_SHARED, 0xFFFFFUL, EINVAL);

    if (holdS > 0) {
        void* p = ::mmap(nullptr, kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         static_cast<off_t>(kMplexIdleFbPgoff) * 4096);
        std::printf("holding the idle-FB mapping %d s (pid %d): rmmod mplex_ddr must answer EBUSY now\n", holdS,
                    static_cast<int>(::getpid()));
        std::fflush(stdout);
        ::sleep(static_cast<unsigned>(holdS));
        if (p != MAP_FAILED)
            ::munmap(p, kMplexIdleFbBytes);
    }
    ::close(fd);
    if (fails) {
        std::printf("MPLEX_PROBE_FAIL %d\n", fails);
        return 1;
    }
    std::printf("MPLEX_PROBE_OK\n");
    return 0;
}
