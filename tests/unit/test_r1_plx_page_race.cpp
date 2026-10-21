// review 07:30 C2 rows: the R1 PLX page under concurrency, on the real arm/misterplexd/fpga_spi.cpp
// in a host build (unmanaged gate MPX_CORE_GATE_START_OPEN, MPX_DEVMEM_PATH a sparse 4 GiB fake memory
// file). The idle thread, the FPGA thread and the ACK pollers all reach plxStore32/plxStore64/plxLoad64;
// on 818d5780 their first calls raced in a private PLX map (two maps, a leaked fd, a torn pointer) and
// close() could unmap under an access. On canon they share the one doorbell-page mapper and its lock (review 08:17 (a)). Built with MPX_TEST_DDR_PAGE_MAP_DELAY_US, which widens the first
// map's window (after the "not mapped yet" check, before the map is published) to 20 ms.
//   P-a: 8 threads released together, each making its first PLX access: all 8 succeed, and exactly 1
//        mapping of the PLX page (offset 0x300ff000) and 1 fd more on the fake memory exist after.
//   P-b: close() while a first map is inside its window waits for it: after close() no PLX mapping and
//        no fd on the fake memory remain.
// Mutant MUT_DDR_PAGE_UNLOCKED (the mapper without its lock) must FAIL.
#include "fpga_spi.hpp"
#include "libmisterplex/r1_plxh.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
using misterplex::r1::kPlxoPhys;

int g_fails = 0;
int g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

constexpr unsigned long long kPlxPage = misterplex::r1::kPlxhPhys & ~0xFFFull;

std::string fakePath() {
    char buf[PATH_MAX];
    return ::realpath(MPX_DEVMEM_PATH, buf) ? std::string(buf) : std::string(MPX_DEVMEM_PATH);
}

// Mappings of the fake memory at the PLX page's offset (/proc/self/maps).
int plxMaps() {
    std::ifstream maps("/proc/self/maps");
    const std::string want = fakePath();
    int n = 0;
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::string range, perms, offset, dev, inode, path;
        in >> range >> perms >> offset >> dev >> inode >> path;
        if (path == want && std::strtoull(offset.c_str(), nullptr, 16) == kPlxPage)
            ++n;
    }
    return n;
}

// Open fds on the fake memory (/proc/self/fd).
int fakeFds() {
    const std::string want = fakePath();
    int n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (!d)
        return -1;
    while (dirent* e = ::readdir(d)) {
        const std::string link = std::string("/proc/self/fd/") + e->d_name;
        char buf[PATH_MAX];
        const ssize_t len = ::readlink(link.c_str(), buf, sizeof buf - 1);
        if (len > 0) {
            buf[len] = '\0';
            if (want == buf)
                ++n;
        }
    }
    ::closedir(d);
    return n;
}

bool makeSparse(const char* path, off_t size) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    const bool ok = ::ftruncate(fd, size) == 0;
    ::close(fd);
    return ok;
}

void rowA() {
    misterplex::FpgaSpi f;
    CHECK(f.open(), "P-a: FpgaSpi::open on the fake memory failed: %s", f.lastError().c_str());
    const int maps0 = plxMaps(), fds0 = fakeFds();
    constexpr int kThreads = 8;
    std::atomic<int> ready{0}, ok{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
        ts.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load())
                std::this_thread::yield();
            uint64_t v = 0;
            if (f.plxLoad64(kPlxoPhys, &v))   // this thread's first PLX access
                ok.fetch_add(1);
        });
    while (ready.load() < kThreads)
        std::this_thread::yield();
    go.store(true);
    for (auto& t : ts)
        t.join();
    const int maps = plxMaps() - maps0, fds = fakeFds() - fds0;
    CHECK(ok.load() == kThreads, "P-a: %d of %d first PLX accesses failed (%s)", kThreads - ok.load(), kThreads,
          f.lastError().c_str());
    CHECK(maps == 1, "P-a: %d mappings of the PLX page after %d concurrent first accesses (want 1)", maps, kThreads);
    CHECK(fds == 1, "P-a: %d fds opened for the PLX page by %d concurrent first accesses (want 1)", fds, kThreads);
    std::printf("P-a %d threads' first PLX access: %d ok, %d mmap of the PLX page, %d fd\n", kThreads, ok.load(),
                maps, fds);
}

void rowB() {
    const int maps0 = plxMaps(), fds0 = fakeFds();
    {
        misterplex::FpgaSpi f;
        CHECK(f.open(), "P-b: FpgaSpi::open on the fake memory failed: %s", f.lastError().c_str());
        std::atomic<bool> entered{false};
        std::thread first([&] {
            entered.store(true);
            uint64_t v = 0;
            (void)f.plxLoad64(kPlxoPhys, &v);   // the first map: 20 ms inside its window
        });
        while (!entered.load())
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto t0 = std::chrono::steady_clock::now();
        f.close();   // must wait for the first map and its access, then unmap
        const double waitedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        first.join();
        const int maps = plxMaps() - maps0, fds = fakeFds() - fds0;
        CHECK(maps == 0, "P-b: %d PLX page mapping(s) outlived close()", maps);
        CHECK(fds == 0, "P-b: %d fd(s) on the fake memory outlived close()", fds);
        std::printf("P-b close() during the first map: waited %.1f ms, then %d PLX mapping and %d fd left\n",
                    waitedMs, maps, fds);
    }
}
}  // namespace

int main() {
    if (!makeSparse(MPX_DEVMEM_PATH, static_cast<off_t>(1) << 32)) {
        std::printf("test_r1_plx_page_race: cannot create the fake memory file\n");
        return 1;
    }
    rowA();
    rowB();
    ::unlink(MPX_DEVMEM_PATH);
    if (g_fails) {
        std::printf("test_r1_plx_page_race: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_r1_plx_page_race: OK (%d checks; 8 concurrent first PLX accesses map the page once, "
                "close() waits for a first map)\n",
                g_checks);
    return 0;
}
