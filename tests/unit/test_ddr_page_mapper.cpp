// review 07:30 C2 + 08:17 (a) + 08:18: the doorbell (PLX) page has ONE mapper and ONE lock. Rows on
// the real arm/misterplexd/fpga_spi.cpp in a host build (unmanaged gate MPX_CORE_GATE_START_OPEN,
// MPX_DEVMEM_PATH a sparse 4 GiB fake memory file). Test seams (this build only):
// MPX_TEST_DDR_PAGE_MAP_DELAY_US widens a first map (after "not mapped", before the map exists) and
// MPX_TEST_DDR_PAGE_ACCESS_DELAY_US an access inside plxPageRead32 (page mapped, load not yet done).
//   P-a 8 threads' first access at the 640x480 layout: 1 mmap and 1 fd for page 0x300FF000; then
//       every doorbell-page reader and the kick run concurrently with the frame window mapped: still
//       exactly 1 mapping covers the page (the frame window stops below it).
//   P-b close() while a first map is in flight waits for it; nothing is left after close().
//   P-c close() while an access is in flight on the mapped page waits for it; the access returns the
//       page's value; nothing is left after close().
//   P-d plxPageRead32 is read-only and bounded: every 4-aligned word reads back, the page bytes are
//       unchanged, off 4096 / 2 / 0xFFFFFFFC are refused.
//   P-e page-keyed slots: two different pages map once each; a third evicts (never two maps of one
//       page); after a layout change the layout page and a fixed request for the same page share 1 map.
//   P-g/P-h (T-PAGEMAP-CLOSE-WAIT, review 17:06) forced schedule: a first map (P-g) / an access on the
//       mapped page (P-h) is HELD inside its window by the seam MPX_TEST_DDR_PAGE_HOLD, which runs with the
//       page lock held, until the closer thread is seen blocked in futex() on ddrPageMu_
//       (/proc/self/task/<tid>/syscall). close() returning while the window is held is the FAIL. No pass
//       condition depends on timing (P-b/P-c measure close()'s wait against a 20 ms window instead).
// Mutants: MUT_DDR_PAGE_UNLOCKED (no lock), MUT_DDR_WINDOW_COVERS_PAGE (the frame window maps
// the doorbell page too) and MUT_CLOSE_SKIPS_PAGE_WAIT (close()'s release does not take the page lock)
// must FAIL; "test_ddr_page_mapper forced" runs P-g/P-h alone, which must kill MUT_CLOSE_SKIPS_PAGE_WAIT
// and MUT_DDR_PAGE_UNLOCKED by themselves.
#include "fpga_spi.hpp"
#include "libmisterplex/r1_plxh.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace misterplex {
extern std::atomic<int> mpxTestDdrPageMapWindows;
extern std::atomic<int> mpxTestDdrPageAccessWindows;
extern std::atomic<void (*)(int)> mpxTestDdrPageHold;
struct DdrPageMapperTest {   // FpgaSpi's friend: a fixed-page request through the one mapper
    static bool map(FpgaSpi& f, uint32_t page) {
        const auto lock = f.lockDdrPage();
        uint8_t* m = nullptr;
        return f.ensureDdrMailboxMap(page, m) && m;
    }
    static bool kick(FpgaSpi& f, int bank) { return f.kickDdrDoorbell(bank); }
    // glibc: a std::mutex blocks in futex(FUTEX_WAIT) on its own address (the lock word is its first member).
    static uintptr_t pageMutex(FpgaSpi& f) { return reinterpret_cast<uintptr_t>(&f.ddrPageMu_); }
};
}  // namespace misterplex

namespace {
using misterplex::FpgaSpi;
using Clock = std::chrono::steady_clock;

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

constexpr uint32_t kPage480 = 0x300FF000u;
constexpr uint32_t kPage240 = 0x3007F000u;
constexpr uint32_t kPage480i = 0x301FF000u;

const char* devmemPath() {
    if (const char* e = std::getenv("MPX_DDR_FAKEMEM"); e && *e)
        return e;
    return MPX_DEVMEM_PATH;
}

std::string fakePath() {
    char buf[PATH_MAX];
    return ::realpath(devmemPath(), buf) ? std::string(buf) : std::string(devmemPath());
}

// Mappings of the fake memory whose file range contains `phys` (/proc/self/maps).
int mapsCovering(uint32_t phys) {
    std::ifstream maps("/proc/self/maps");
    const std::string want = fakePath();
    int n = 0;
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::string range, perms, offset, dev, inode, path;
        in >> range >> perms >> offset >> dev >> inode >> path;
        if (path != want)
            continue;
        const auto dash = range.find('-');
        const unsigned long long lo = std::strtoull(range.substr(0, dash).c_str(), nullptr, 16);
        const unsigned long long hi = std::strtoull(range.substr(dash + 1).c_str(), nullptr, 16);
        const unsigned long long off = std::strtoull(offset.c_str(), nullptr, 16);
        if (phys >= off && phys < off + (hi - lo))
            ++n;
    }
    return n;
}

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

// The fake memory through the file, never a mapping (a test map would count as a second map).
bool poke(uint32_t phys, const void* data, size_t n) {
    const int fd = ::open(devmemPath(), O_RDWR | O_CLOEXEC);
    const bool ok = fd >= 0 && ::pwrite(fd, data, n, static_cast<off_t>(phys)) == static_cast<ssize_t>(n);
    if (fd >= 0)
        ::close(fd);
    return ok;
}
bool peek(uint32_t phys, void* data, size_t n) {
    const int fd = ::open(devmemPath(), O_RDONLY | O_CLOEXEC);
    const bool ok = fd >= 0 && ::pread(fd, data, n, static_cast<off_t>(phys)) == static_cast<ssize_t>(n);
    if (fd >= 0)
        ::close(fd);
    return ok;
}

void waitFor(const std::atomic<int>& counter, int above) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (counter.load() <= above && Clock::now() < deadline)
        std::this_thread::yield();
}

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// P-b/P-c: the end of the in-flight window, stamped by the hold seam while the page lock is still held. A
// close() that waits returns after it, whatever the schedule. A close() that was called after it and read
// 0.0 ms is a late main thread (the load-45 0.0 ms), not a close() that skips the wait.
std::atomic<int64_t> g_windowEndNs{0};
void stampWindowEnd(int) { g_windowEndNs.store(nowNs()); }

void classifyShortWait(const char* row, int64_t closeCalledNs, double waitedMs) {
    const int64_t end = g_windowEndNs.load();
    const int64_t closeReturnedNs = closeCalledNs + static_cast<int64_t>(waitedMs * 1e6);
    if (!end)
        std::printf("INFO %s: no in-flight window was stamped\n", row);
    else if (closeCalledNs >= end)
        std::printf("INFO %s: close() was called %.1f ms AFTER the in-flight window ended: a late main thread, "
                    "not a close() that skips the wait (P-g/P-h force the schedule)\n",
                    row, (closeCalledNs - end) / 1e6);
    else
        std::printf("INFO %s: close() was called %.1f ms BEFORE the window ended and returned %.1f ms %s it\n", row,
                    (end - closeCalledNs) / 1e6, std::abs(closeReturnedNs - end) / 1e6,
                    closeReturnedNs < end ? "BEFORE" : "after");
}

void rowA() {
    FpgaSpi f;
    CHECK(f.open(), "P-a: open: %s", f.lastError().c_str());
    CHECK(f.setDdrFrameLayout(640, 480), "P-a: 640x480 layout: %s", f.lastError().c_str());
    CHECK((f.ddrFrameLayout().doorbell_phys & ~0xFFFu) == kPage480, "P-a: 640x480 doorbell page 0x%08x",
          f.ddrFrameLayout().doorbell_phys);
    const int maps0 = mapsCovering(kPage480), fds0 = fakeFds();
    constexpr int kThreads = 8;
    std::atomic<int> ready{0}, ok{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
        ts.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load())
                std::this_thread::yield();
            uint32_t v = 0;
            if (f.plxPageRead32(0x118, v))   // this thread's first access to the page
                ok.fetch_add(1);
        });
    while (ready.load() < kThreads)
        std::this_thread::yield();
    go.store(true);
    for (auto& t : ts)
        t.join();
    const int maps = mapsCovering(kPage480) - maps0, fds = fakeFds() - fds0;
    CHECK(ok.load() == kThreads, "P-a: %d of %d first reads failed", kThreads - ok.load(), kThreads);
    CHECK(maps == 1, "P-a: %d mappings of page 0x300FF000 after %d concurrent first reads (want 1)", maps, kThreads);
    CHECK(fds == 1, "P-a: %d fds for page 0x300FF000 after %d concurrent first reads (want 1)", fds, kThreads);
    std::printf("P-a %d threads' first plxPageRead32: %d ok, %d mmap of page 0x300FF000, %d fd\n", kThreads,
                ok.load(), maps, fds);

    // Every doorbell-page user at once, with the frame window mapped by the readers' ensureDdrMap.
    misterplex::FrameStoreStatus fss{};
    misterplex::BankReleaseStatus brs{};
    misterplex::SourceAspectAck saa{};
    FpgaSpi::DdrDoorbellStatus dbs{};
    misterplex::PlaybackCommand cmd = misterplex::PlaybackCommand::None;
    uint16_t osd = 0;
    std::vector<std::function<void()>> users = {
        [&] { (void)f.readFrameStoreStatus(fss); },  [&] { (void)f.readBankRelease(brs); },
        [&] { (void)f.readSourceAspectAck(saa); },   [&] { (void)f.readDdrDoorbellStatus(dbs); },
        [&] { (void)f.readInputMailbox(cmd); },      [&] { (void)f.readOsdMailbox(osd); },
        [&] { (void)misterplex::DdrPageMapperTest::kick(f, 0); }, [&] {
            uint32_t v = 0;
            (void)f.plxPageRead32(0x130, v);
        }};
    ts.clear();
    for (auto& u : users)
        ts.emplace_back(u);
    for (auto& t : ts)
        t.join();
    const uint32_t base = f.ddrFrameLayout().phys_base;
    const int cover = mapsCovering(kPage480) - maps0;
    CHECK(mapsCovering(base) >= 1, "P-a: the frame window at 0x%08x was not mapped by the readers", base);
    CHECK(cover == 1, "P-a: %d mappings cover page 0x300FF000 with the frame window mapped (want 1: the "
                      "window must stop below the doorbell page)", cover);
    std::printf("P-a all 8 doorbell-page users concurrently, frame window 0x%08x..0x%08x mapped: %d mapping "
                "covers page 0x300FF000\n",
                base, kPage480, cover);
    std::printf("INFO pre-existing, not the doorbell page: %d frame-window mappings after the concurrent "
                "first readers (ensureDdrMap itself is not serialized)\n",
                mapsCovering(base));
}

void rowB() {
    const int maps0 = mapsCovering(kPage480), fds0 = fakeFds();
    double waitedMs = -1;
    {
        FpgaSpi f;
        CHECK(f.open(), "P-b: open: %s", f.lastError().c_str());
        CHECK(f.setDdrFrameLayout(640, 480), "P-b: layout: %s", f.lastError().c_str());
        const int windows = misterplex::mpxTestDdrPageMapWindows.load();
        g_windowEndNs.store(0);
        misterplex::mpxTestDdrPageHold.store(stampWindowEnd);
        std::thread first([&] {
            uint32_t v = 0;
            (void)f.plxPageRead32(0x118, v);   // the first map, 20 ms inside its window
        });
        waitFor(misterplex::mpxTestDdrPageMapWindows, windows);
        const auto t0 = Clock::now();
        const int64_t t0Ns = nowNs();
        f.close();   // must wait for the first map and its access, then unmap
        const int64_t closeReturnedNs = nowNs();
        waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        first.join();
        misterplex::mpxTestDdrPageHold.store(nullptr);
        const int maps = mapsCovering(kPage480) - maps0;
        CHECK(waitedMs >= 10.0, "P-b: close() did not wait for the first map in flight (%.1f ms)", waitedMs);
        if (waitedMs < 10.0)
            classifyShortWait("P-b", t0Ns, waitedMs);
        CHECK(closeReturnedNs > g_windowEndNs.load(), "P-b: close() returned before the first map's window ended");
        CHECK(maps == 0, "P-b: %d mapping(s) of page 0x300FF000 outlived close()", maps);
    }
    const int fds = fakeFds() - fds0;
    CHECK(fds == 0, "P-b: %d fd(s) on the fake memory outlived the FpgaSpi", fds);
    std::printf("P-b close() during the first map: waited %.1f ms, then 0 mappings and %d fds left\n", waitedMs, fds);
}

void segvInRowC(int) {
    static const char msg[] = "FAIL P-c: SIGSEGV: an access was in flight when the page was unmapped\n";
    (void)!::write(1, msg, sizeof msg - 1);
    ::_exit(3);
}

void rowC() {
    const int maps0 = mapsCovering(kPage480);
    const uint32_t want = 0x5A5A1234u;
    CHECK(poke(kPage480 + 0x130, &want, 4), "P-c: cannot seed the fake page");
    FpgaSpi f;
    CHECK(f.open(), "P-c: open: %s", f.lastError().c_str());
    CHECK(f.setDdrFrameLayout(640, 480), "P-c: layout: %s", f.lastError().c_str());
    uint32_t v0 = 0;
    CHECK(f.plxPageRead32(0x130, v0) && v0 == want, "P-c: first read 0x%08x", v0);
    const int windows = misterplex::mpxTestDdrPageAccessWindows.load();
    uint32_t v1 = 0;
    bool ok1 = false;
    auto* prev = std::signal(SIGSEGV, segvInRowC);
    g_windowEndNs.store(0);
    misterplex::mpxTestDdrPageHold.store(stampWindowEnd);
    std::thread reader([&] { ok1 = f.plxPageRead32(0x130, v1); });   // in flight: 20 ms before its load
    waitFor(misterplex::mpxTestDdrPageAccessWindows, windows);
    const auto t0 = Clock::now();
    const int64_t t0Ns = nowNs();
    f.close();
    const int64_t closeReturnedNs = nowNs();
    const double waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    reader.join();
    misterplex::mpxTestDdrPageHold.store(nullptr);
    std::signal(SIGSEGV, prev);
    const int maps = mapsCovering(kPage480) - maps0;
    CHECK(waitedMs >= 10.0, "P-c: close() did not wait for the access in flight (%.1f ms)", waitedMs);
    if (waitedMs < 10.0)
        classifyShortWait("P-c", t0Ns, waitedMs);
    CHECK(closeReturnedNs > g_windowEndNs.load(), "P-c: close() returned before the access's window ended");
    CHECK(ok1 && v1 == want, "P-c: the in-flight read returned ok=%d 0x%08x (want 0x%08x)", ok1 ? 1 : 0, v1, want);
    CHECK(maps == 0, "P-c: %d mapping(s) of page 0x300FF000 outlived close()", maps);
    std::printf("P-c close() during an access on the mapped page: waited %.1f ms, the read returned 0x%08x, "
                "0 mappings left\n",
                waitedMs, v1);
}

void rowD() {
    std::vector<uint32_t> pattern(1024);
    for (uint32_t i = 0; i < pattern.size(); ++i)
        pattern[i] = 0x9E3779B9u * (i + 1);
    CHECK(poke(kPage480, pattern.data(), 4096), "P-d: cannot seed the fake page");
    FpgaSpi f;
    CHECK(f.open(), "P-d: open: %s", f.lastError().c_str());
    CHECK(f.setDdrFrameLayout(640, 480), "P-d: layout: %s", f.lastError().c_str());
    int bad = 0;
    for (uint32_t off = 0; off < 4096; off += 4) {
        uint32_t v = 0;
        if (!f.plxPageRead32(off, v) || v != pattern[off / 4])
            ++bad;
    }
    std::vector<uint32_t> after(1024);
    CHECK(peek(kPage480, after.data(), 4096), "P-d: cannot read the fake page back");
    uint32_t v = 0;
    CHECK(bad == 0, "P-d: %d of 1024 words read back wrong", bad);
    CHECK(after == pattern, "P-d: plxPageRead32 changed the page");
    CHECK(!f.plxPageRead32(4096, v) && !f.plxPageRead32(2, v) && !f.plxPageRead32(0xFFFFFFFCu, v),
          "P-d: an out-of-range or unaligned offset was read");
    std::printf("P-d 1024 words read back, page unchanged, off 4096/2/0xFFFFFFFC refused\n");
}

void rowE() {
    const int a0 = mapsCovering(kPage240), b0 = mapsCovering(kPage480), c0 = mapsCovering(kPage480i);
    FpgaSpi f;
    CHECK(f.open(), "P-e: open: %s", f.lastError().c_str());   // start-up layout 320x240: page 0x3007F000
    uint32_t v = 0;
    CHECK(f.plxPageRead32(0x118, v), "P-e: layout page read");
    CHECK(misterplex::DdrPageMapperTest::map(f, kPage480), "P-e: fixed page 0x300FF000");
    CHECK(mapsCovering(kPage240) - a0 == 1 && mapsCovering(kPage480) - b0 == 1,
          "P-e: two pages: 0x3007F000 %d, 0x300FF000 %d mappings (want 1 each)", mapsCovering(kPage240) - a0,
          mapsCovering(kPage480) - b0);
    CHECK(misterplex::DdrPageMapperTest::map(f, kPage480), "P-e: fixed page again");
    CHECK(mapsCovering(kPage480) - b0 == 1, "P-e: a second request for 0x300FF000 mapped it again");
    CHECK(misterplex::DdrPageMapperTest::map(f, kPage480i), "P-e: a third page");
    const int a = mapsCovering(kPage240) - a0, b = mapsCovering(kPage480) - b0, c = mapsCovering(kPage480i) - c0;
    CHECK(c == 1 && a + b == 1, "P-e: third page: 0x3007F000 %d, 0x300FF000 %d, 0x301FF000 %d (want one evicted)", a,
          b, c);
    CHECK(f.setDdrFrameLayout(640, 480), "P-e: layout 640x480: %s", f.lastError().c_str());
    CHECK(mapsCovering(kPage240) - a0 == 0 && mapsCovering(kPage480) - b0 == 0 && mapsCovering(kPage480i) - c0 == 0,
          "P-e: a layout change left a page mapped");
    CHECK(misterplex::DdrPageMapperTest::map(f, kPage480) && f.plxPageRead32(0x118, v), "P-e: shared page");
    CHECK(mapsCovering(kPage480) - b0 == 1, "P-e: the layout page and the fixed request for it map %d times (want 1)",
          mapsCovering(kPage480) - b0);
    std::printf("P-e slots: 2 pages 1 map each, a 3rd evicts one, the layout page and a fixed request for it "
                "share 1 map\n");
}

// P-f (R1 rebase onto the canonical restack, review 08:17 (a)): every other user of the 480p
// doorbell page shares the one mapping: the browse P1 direct readers (readInputMailboxDirect,
// readOsdMailboxDirect, which mapped a page each before) and the R1 PLXH/PLXO accessors (a private
// PLX map before), concurrently with plxPageRead32. Mutants MUT_BROWSE_DIRECT_OWN_MAP and
// MUT_R1_PLX_OWN_MAP (the pre-mapper private maps) must FAIL.
void rowF() {
    const int maps0 = mapsCovering(kPage480);
    FpgaSpi f;
    CHECK(f.open(), "P-f: open: %s", f.lastError().c_str());
    CHECK(f.setDdrFrameLayout(640, 480), "P-f: layout: %s", f.lastError().c_str());
    std::atomic<int> plxo{0};
    std::vector<std::thread> ts;
    ts.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            misterplex::PlaybackCommand c = misterplex::PlaybackCommand::None;
            (void)f.readInputMailboxDirect(c, 11);
        }
    });
    ts.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            uint16_t o = 0;
            (void)f.readOsdMailboxDirect(o);
        }
    });
    ts.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            uint32_t v = 0;
            (void)f.plxPageRead32(0x130, v);
        }
    });
    ts.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            uint64_t w = 0;
            if (f.plxLoad64(misterplex::r1::kPlxoPhys, &w))
                plxo.fetch_add(1);
        }
    });
    for (auto& t : ts)
        t.join();
    const int maps = mapsCovering(kPage480) - maps0;
    CHECK(plxo.load() == 100, "P-f: %d of 100 R1 PLXO loads failed", 100 - plxo.load());
    CHECK(maps == 1, "P-f: %d mappings cover page 0x300FF000 with the browse direct readers, the R1 PLX accessors "
                     "and plxPageRead32 in use (want 1)", maps);
    std::printf("P-f browse direct input + OSD readers, R1 PLXO loads and plxPageRead32 concurrently: %d mapping "
                "covers page 0x300FF000\n", maps);
}

// P-g/P-h forced schedule (T-PAGEMAP-CLOSE-WAIT): see the header. holdWindow() runs in the in-flight thread with
// the page lock held; the closer thread starts only after it is held, so the row cannot pass early or late.
struct Forced {
    std::atomic<int> window{0};          // the window to hold, once: 1 a first map, 2 an access
    std::atomic<bool> held{false};
    std::atomic<long> closerTid{0};
    std::atomic<bool> closeReturned{false};
    std::atomic<int64_t> closeReturnedNs{0};
    std::atomic<int64_t> releasedNs{0};
    std::atomic<int> verdict{0};         // 1 close() blocked on ddrPageMu_; 2 close() returned while held; 3 neither
    uintptr_t pageMutex = 0;
    const char* row = "";
    char lastSyscall[96] = {};
};
Forced g_forced;

bool blockedOnPageMutex(long tid, uintptr_t mutex, char (&last)[96]) {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/syscall");
    std::string line;
    if (!std::getline(in, line))
        return false;
    std::snprintf(last, sizeof last, "%s", line.c_str());
    std::istringstream ss(line);
    long nr = -2;
    std::string uaddr;
    ss >> nr >> uaddr;
    return nr == SYS_futex && !uaddr.empty() && std::strtoull(uaddr.c_str(), nullptr, 16) == mutex;
}

void holdWindow(int which) {
    if (which != g_forced.window.load())
        return;
    g_forced.window.store(0);
    g_forced.held.store(true);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    int verdict = 3;
    while (Clock::now() < deadline) {
        if (g_forced.closeReturned.load()) {
            verdict = 2;
            break;
        }
        const long tid = g_forced.closerTid.load();
        if (tid && blockedOnPageMutex(tid, g_forced.pageMutex, g_forced.lastSyscall)) {
            verdict = 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (verdict == 2)
        std::printf("FAIL %s: close() returned while the %s was held in flight (it did not wait for the page lock)\n",
                    g_forced.row, which == 1 ? "first map" : "access");
    g_forced.verdict.store(verdict);
    g_forced.releasedNs.store(nowNs());
}

void segvInForcedRow(int) {
    static const char msg[] = "FAIL P-g/P-h: SIGSEGV: the held window's page was unmapped under it\n";
    (void)!::write(1, msg, sizeof msg - 1);
    ::_exit(3);
}

// One forced row: `inFlight` runs in its own thread and must reach seam `window` (held there); then close().
void forcedRow(const char* row, FpgaSpi& f, int window, const std::function<void()>& inFlight, double& blockedMs) {
    g_forced.window.store(0);
    g_forced.held.store(false);
    g_forced.closerTid.store(0);
    g_forced.closeReturned.store(false);
    g_forced.closeReturnedNs.store(0);
    g_forced.releasedNs.store(0);
    g_forced.verdict.store(0);
    g_forced.pageMutex = misterplex::DdrPageMapperTest::pageMutex(f);
    g_forced.row = row;
    g_forced.lastSyscall[0] = '\0';
    auto* prev = std::signal(SIGSEGV, segvInForcedRow);
    g_forced.window.store(window);
    misterplex::mpxTestDdrPageHold.store(holdWindow);
    std::thread holder(inFlight);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!g_forced.held.load() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    const bool held = g_forced.held.load();
    int64_t closeCalledNs = 0;
    std::thread closer([&] {
        if (!held)
            return;
        g_forced.closerTid.store(static_cast<long>(::syscall(SYS_gettid)));
        closeCalledNs = nowNs();
        f.close();
        g_forced.closeReturnedNs.store(nowNs());
        g_forced.closeReturned.store(true);
    });
    holder.join();
    closer.join();
    misterplex::mpxTestDdrPageHold.store(nullptr);
    g_forced.window.store(0);
    std::signal(SIGSEGV, prev);
    CHECK(held, "%s: the in-flight thread never reached its window (seam %d)", row, window);
    CHECK(g_forced.verdict.load() == 1,
          "%s: close() was not seen blocked on the page lock while the window was held (verdict %d: 2 = it "
          "returned, 3 = no futex wait on ddrPageMu_ in 30 s; last /proc syscall '%s')",
          row, g_forced.verdict.load(), g_forced.lastSyscall);
    CHECK(g_forced.closeReturnedNs.load() > g_forced.releasedNs.load(),
          "%s: close() returned before the held window was released", row);
    blockedMs = (g_forced.closeReturnedNs.load() - closeCalledNs) / 1e6;
}

void rowG() {
    const int fails0 = g_fails;
    const int maps0 = mapsCovering(kPage480), fds0 = fakeFds();
    double blockedMs = -1;
    int maps = -1;
    {
        FpgaSpi f;
        CHECK(f.open(), "P-g: open: %s", f.lastError().c_str());
        CHECK(f.setDdrFrameLayout(640, 480), "P-g: layout: %s", f.lastError().c_str());
        forcedRow("P-g", f, 1, [&] {
            uint32_t v = 0;
            (void)f.plxPageRead32(0x118, v);   // the first map, held before its mmap
        }, blockedMs);
        maps = mapsCovering(kPage480) - maps0;
        CHECK(maps == 0, "P-g: %d mapping(s) of page 0x300FF000 outlived close()", maps);
    }
    const int fds = fakeFds() - fds0;
    CHECK(fds == 0, "P-g: %d fd(s) on the fake memory outlived the FpgaSpi", fds);
    if (g_fails == fails0)
        std::printf("P-g close() during a HELD first map: blocked in futex() on ddrPageMu_ until the window was "
                    "released, returned %.1f ms after the call; %d mappings and %d fds left\n", blockedMs, maps, fds);
    else
        std::printf("P-g FAILED (%d check(s)): close() during a HELD first map; %d mapping(s) and %d fd(s) left\n",
                    g_fails - fails0, maps, fds);
}

void rowH() {
    const int fails0 = g_fails;
    const int maps0 = mapsCovering(kPage480);
    const uint32_t want = 0xC105EA17u;
    CHECK(poke(kPage480 + 0x134, &want, 4), "P-h: cannot seed the fake page");
    double blockedMs = -1;
    uint32_t v1 = 0;
    bool ok1 = false;
    int maps = -1;
    {
        FpgaSpi f;
        CHECK(f.open(), "P-h: open: %s", f.lastError().c_str());
        CHECK(f.setDdrFrameLayout(640, 480), "P-h: layout: %s", f.lastError().c_str());
        uint32_t v0 = 0;
        CHECK(f.plxPageRead32(0x134, v0) && v0 == want, "P-h: first read 0x%08x", v0);   // maps the page
        forcedRow("P-h", f, 2, [&] { ok1 = f.plxPageRead32(0x134, v1); }, blockedMs);   // held before its load
        CHECK(ok1 && v1 == want, "P-h: the held read returned ok=%d 0x%08x (want 0x%08x)", ok1 ? 1 : 0, v1, want);
        maps = mapsCovering(kPage480) - maps0;
        CHECK(maps == 0, "P-h: %d mapping(s) of page 0x300FF000 outlived close()", maps);
    }
    if (g_fails == fails0)
        std::printf("P-h close() during a HELD access on the mapped page: blocked in futex() on ddrPageMu_ until the "
                    "window was released, returned %.1f ms after the call; the read returned 0x%08x, %d mappings "
                    "left\n", blockedMs, v1, maps);
    else
        std::printf("P-h FAILED (%d check(s)): close() during a HELD access; the read returned ok=%d 0x%08x, "
                    "%d mapping(s) left\n", g_fails - fails0, ok1 ? 1 : 0, v1, maps);
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // every row line survives a mutant's SIGSEGV exit
    const bool forcedOnly = argc > 1 && std::strcmp(argv[1], "forced") == 0;
    if (!makeSparse(devmemPath(), static_cast<off_t>(1) << 32)) {
        std::printf("test_ddr_page_mapper: cannot create the fake memory file\n");
        return 1;
    }
    if (!forcedOnly) {
        rowA();
        rowB();
        rowC();
        rowD();
        rowE();
        rowF();
    }
    rowG();
    rowH();
    ::unlink(devmemPath());
    if (g_fails) {
        std::printf("test_ddr_page_mapper: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    if (forcedOnly) {
        std::printf("test_ddr_page_mapper forced: OK (%d checks; close() blocks on the page lock while a first map "
                    "and an access are held)\n", g_checks);
        return 0;
    }
    std::printf("test_ddr_page_mapper: OK (%d checks; one mapper and one lock for the doorbell page: 8 first "
                "readers 1 mmap, close() waits for a first map and for an access, read-only, slots; forced: close() "
                "blocks on the page lock while a first map and an access are held)\n",
                g_checks);
    return 0;
}
