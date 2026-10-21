// D52-LOG on the daemon's one PLX page mapper (review 08:17 (b), 08:18). The real fpga_spi.cpp in a host
// build: MPX_CORE_GATE_START_OPEN, MPX_DEVMEM_PATH a sparse 4 GiB fake memory file, the fake Plex core
// (fake_plex_core.hpp) on the 480p PLX page 0x300FF000, the seam MPX_TEST_DDR_PAGE_ACCESS_DELAY_US widening
// every page access. Each row runs in a forked child (D52-LOG's instance is process-wide).
//   D-a the same 0x48005D52 session (CAPS -> BEGIN -> a doorbell kick -> arm -> END -> close), MPX_D52_LOG unset/=1: while
//       armed the daemon holds 1 mmap of page 0x300FF000 and the same fds with D52-LOG on as off (D52-LOG adds 0
//       maps, 0 fds); the poller's baseline proves it read the page; after close() 0 maps and the fds are gone.
//   D-b poller + a concurrent doorbell writer (kickDdrDoorbell) + close() while a poller read is in flight:
//       close() waits for it; after the FpgaSpi is destroyed the poller keeps running without it (one 'plx read
//       failed' line, then the absent window), no crash, 0 maps, 0 fds.
//   D-c after close() (the FpgaSpi still alive) the poller never reads through it again: 0 maps of the page.
// The fake core's own map of the page (the test's, not the daemon's) is subtracted from every count.
// Mutants that must FAIL: MUT_D52_OWN_MMAP (d52 maps the page itself again: D-a 2 maps, +1 fd) and
// MUT_D52_NO_DETACH (close() does not detach the reader: D-c the poller re-maps the page after close()).
#include "fake_plex_core.hpp"
#include "fpga_spi.hpp"
#include "libmisterplex/d52_log.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MPX_DEVMEM_PATH
#error "build with -DMPX_DEVMEM_PATH=\"<fake /dev/mem file>\""
#endif

namespace misterplex {
extern std::atomic<int> mpxTestDdrPageAccessWindows;
struct DdrPageMapperTest {   // FpgaSpi's friend: the doorbell writer
    static bool kick(FpgaSpi& f, int bank) { return f.kickDdrDoorbell(bank); }
};
}  // namespace misterplex

using namespace misterplex;

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kPage = 0x300FF000u;
constexpr uint32_t kBuild = 0x48005D52u;

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

std::string fakePath() {
    char buf[PATH_MAX];
    return ::realpath(MPX_DEVMEM_PATH, buf) ? std::string(buf) : std::string(MPX_DEVMEM_PATH);
}

int mapsCovering(uint32_t phys) {   // mappings of the fake memory whose file range holds phys
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

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

std::string readFile(const std::string& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

long meas(const std::string& err, const char* key) {   // "MEAS key=<n>" from a child's stderr
    const std::string k = std::string("MEAS ") + key + "=";
    const size_t at = err.find(k);
    return at == std::string::npos ? LONG_MIN : std::strtol(err.c_str() + at + k.size(), nullptr, 10);
}

template <class Fn> std::string inChild(const char* tag, Fn fn, int& rc) {
    const std::string cap = std::string(MPX_DEVMEM_PATH) + "." + tag + ".stderr";
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int fd = ::open(cap.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0)
            ::dup2(fd, 2);
        const int r = fn();
        std::fflush(stderr);
        ::_exit(r);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    std::string out = readFile(cap);
    ::unlink(cap.c_str());
    return out;
}

// The fake Plex core on the fake memory (as test_d52_log G): its ring window and its 480p PLX page are the
// file's; its SPI registers a private map. Its page map is the test's, counted in base_maps.
struct Bench {
    void* w = MAP_FAILED;
    void* p = MAP_FAILED;
    void* r = MAP_FAILED;
    fake_core::FakePlexCore* core = nullptr;
    std::atomic<bool> run{true};
    std::thread t;
    int base_maps = 0;
    int base_fds = 0;
    bool ok = false;

    Bench() {
        const int fd = ::open(MPX_DEVMEM_PATH, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return;
        w = ::mmap(nullptr, fake_core::kRingWindowLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   fake_core::kRingWindowBase);
        p = ::mmap(nullptr, 0x1000u, PROT_READ | PROT_WRITE, MAP_SHARED, fd, fake_core::kPlex480pPlxPage);
        ::close(fd);
        r = ::mmap(nullptr, 0x01000000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (w == MAP_FAILED || p == MAP_FAILED || r == MAP_FAILED)
            return;
        core = new fake_core::FakePlexCore(static_cast<volatile uint8_t*>(r), static_cast<volatile uint8_t*>(w),
                                           static_cast<volatile uint8_t*>(p));
        core->reprogram(kBuild);
        t = std::thread([this] {
            while (run.load(std::memory_order_relaxed))
                core->step();
        });
        base_maps = mapsCovering(kPage);
        base_fds = fakeFds();
        ok = true;
    }
    ~Bench() {
        run.store(false);
        if (t.joinable())
            t.join();
    }
    int maps() const { return mapsCovering(kPage) - base_maps; }
    int fds() const { return fakeFds() - base_fds; }
};

void envD52(bool on) {
    if (on) {
        ::setenv(d52::kEnv, "1", 1);
        ::setenv(d52::kPollEnv, "2", 1);
    } else {
        ::unsetenv(d52::kEnv);
    }
}

bool begin(FpgaSpi& f, uint64_t sid) {
    if (!f.setDdrFrameLayout(ddrFrameGeometryForPresentedSize(640, 480), DdrFrameFormat::Yuv420p))
        return false;
    FpgaSpi::VideoCapabilities caps;
    return f.beginFpgaVideoSession(sid, caps, 2000) && caps.build_id == kBuild;
}

// D-a: one session; MEAS armed/closed maps and fds (the fake core's map and fds subtracted).
int rowA(bool on) {
    envD52(on);
    Bench b;
    if (!b.ok)
        return 3;
    int out = 0;
    {
        FpgaSpi f;
        if (!begin(f, 0xD52A0001ull))
            out = 4;
        else if (!DdrPageMapperTest::kick(f, 0))   // the daemon's own user of the page: mapped, on or off
            out = 6;
        ::usleep(400000);   // armed: the poller's reads (each load 2 ms inside the seam)
        std::fprintf(stderr, "MEAS armed_maps=%d\nMEAS armed_fds=%d\n", b.maps(), b.fds());
        if (!out && !f.endBitstreamSession(0xD52A0001ull, 2000))
            out = 5;
        f.close();
        ::usleep(50000);
        std::fprintf(stderr, "MEAS closed_maps=%d\nMEAS closed_fds=%d\n", b.maps(), b.fds());
    }
    if (out)
        std::fprintf(stderr, "D-a error %d\n", out);
    return out;
}

// D-b: poller + kicker + close() with a poller read in flight; then the FpgaSpi is destroyed under the poller.
int rowB() {
    envD52(true);
    Bench b;
    if (!b.ok)
        return 3;
    long waitedUs = -1;
    long kicks = 0;
    {
        FpgaSpi f;
        if (!begin(f, 0xD52B0001ull))
            return 4;
        std::atomic<bool> kicking{true};
        std::thread kicker([&] {
            for (int i = 0; kicking.load(); ++i) {
                (void)DdrPageMapperTest::kick(f, i & 1);
                ++kicks;
            }
        });
        ::usleep(200000);   // poller and kicker interleave on the page lock
        const int w0 = mpxTestDdrPageAccessWindows.load();
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (mpxTestDdrPageAccessWindows.load() <= w0 && Clock::now() < deadline)
            std::this_thread::yield();   // a page access (the poller's or the kicker's) is in flight
        const auto t0 = Clock::now();
        f.close();
        waitedUs = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        kicking.store(false);
        kicker.join();
    }   // ~FpgaSpi: close() again, then gone; the poller is still running
    ::usleep(300000);
    std::fprintf(stderr, "MEAS close_wait_us=%ld\nMEAS kicks=%ld\nMEAS gone_maps=%d\nMEAS gone_fds=%d\n", waitedUs,
                 kicks, b.maps(), b.fds());
    return 0;
}

// D-c: close() while the session is still armed; the FpgaSpi stays alive; the poller must not map the page again.
int rowC() {
    envD52(true);
    Bench b;
    if (!b.ok)
        return 3;
    FpgaSpi f;
    if (!begin(f, 0xD52C0001ull))
        return 4;
    ::usleep(100000);
    f.close();
    ::usleep(200000);   // many poll periods
    std::fprintf(stderr, "MEAS after_close_maps=%d\n", b.maps());
    return 0;
}
}  // namespace

int main() {
    {
        const int fd = ::open(MPX_DEVMEM_PATH, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        CHECK(fd >= 0 && ::ftruncate(fd, 0x100000000ll) == 0, "sparse fake memory %s", MPX_DEVMEM_PATH);
        if (fd >= 0)
            ::close(fd);
    }

    // ---- D-a ----
    int rcOff = -1, rcOn = -1;
    const std::string off = inChild("da-off", [] { return rowA(false); }, rcOff);
    const std::string on = inChild("da-on", [] { return rowA(true); }, rcOn);
    CHECK(rcOff == 0 && rcOn == 0, "D-a: child rc off %d on %d: %s | %s", rcOff, rcOn, off.c_str(), on.c_str());
    CHECK(!contains(off, "d52 ") && !contains(off, "plx baseline"), "D-a: D52-LOG unset prints nothing");
    CHECK(contains(on, "arm session=0xd52a0001 plx_page=0x300ff000 source=doorbell build=0x48005D52") &&
              contains(on, "plx baseline plxj=") && contains(on, "disarm (END)") && !contains(on, "plx read failed"),
          "D-a: D52-LOG=1 armed on 0x300ff000 and read the page (baseline), then END: %s", on.c_str());
    const long mOff = meas(off, "armed_maps"), mOn = meas(on, "armed_maps");
    const long fOff = meas(off, "armed_fds"), fOn = meas(on, "armed_fds");
    CHECK(mOn == 1, "D-a: %ld mmap(s) of page 0x300FF000 in a D52-LOG=1 session (want 1: the daemon's one mapper)",
          mOn);
    CHECK(mOff == 1 && mOn == mOff, "D-a: maps of the page off %ld / on %ld (D52-LOG must add none)", mOff, mOn);
    CHECK(fOn == fOff && fOn >= 1, "D-a: fake-memory fds off %ld / on %ld (D52-LOG must add none)", fOff, fOn);
    CHECK(meas(on, "closed_maps") == 0 && meas(on, "closed_fds") == 0 && meas(off, "closed_maps") == 0 &&
              meas(off, "closed_fds") == 0,
          "D-a: after close(): maps %ld/%ld fds %ld/%ld (want 0)", meas(off, "closed_maps"), meas(on, "closed_maps"),
          meas(off, "closed_fds"), meas(on, "closed_fds"));
    std::printf("D-a 0x48005D52 session armed: page 0x300FF000 maps off %ld / on %ld, daemon fds on the fake "
                "memory off %ld / on %ld; after close() maps %ld, fds %ld\n",
                mOff, mOn, fOff, fOn, meas(on, "closed_maps"), meas(on, "closed_fds"));

    // ---- D-b ----
    int rcB = -1;
    const std::string eb = inChild("db", [] { return rowB(); }, rcB);
    CHECK(rcB == 0, "D-b: child rc %d (a crash is a FAIL): %s", rcB, eb.c_str());
    const long waited = meas(eb, "close_wait_us");
    CHECK(waited >= 500, "D-b: close() returned in %ld us with an access in flight (want it to wait)", waited);
    CHECK(meas(eb, "kicks") > 0, "D-b: the doorbell writer ran (%ld kicks)", meas(eb, "kicks"));
    CHECK(meas(eb, "gone_maps") == 0 && meas(eb, "gone_fds") == 0,
          "D-b: after the FpgaSpi is gone: %ld maps, %ld fds (want 0)", meas(eb, "gone_maps"), meas(eb, "gone_fds"));
    size_t fails = 0;
    for (size_t at = eb.find("plx read failed"); at != std::string::npos; at = eb.find("plx read failed", at + 1))
        ++fails;
    CHECK(fails == 1 && contains(eb, "plx read failed page=0x300ff000 (the daemon's mapper gave no read"),
          "D-b: one 'plx read failed' line after close() (%zu): %s", fails, eb.c_str());
    std::printf("D-b poller + %ld doorbell kicks + close() with an access in flight: close() waited %ld us; FpgaSpi "
                "gone: %ld maps, %ld fds, %zu 'plx read failed' line, rc %d\n",
                meas(eb, "kicks"), waited, meas(eb, "gone_maps"), meas(eb, "gone_fds"), fails, rcB);

    // ---- D-c ----
    int rcC = -1;
    const std::string ec = inChild("dc", [] { return rowC(); }, rcC);
    CHECK(rcC == 0, "D-c: child rc %d: %s", rcC, ec.c_str());
    CHECK(meas(ec, "after_close_maps") == 0,
          "D-c: %ld map(s) of the page 200 ms after close() (want 0: the poller must not read through a closed "
          "FpgaSpi)",
          meas(ec, "after_close_maps"));
    std::printf("D-c 200 ms after close() with the session armed: %ld maps of the page\n", meas(ec, "after_close_maps"));

    ::unlink(MPX_DEVMEM_PATH);
    if (g_fails) {
        std::printf("test_d52_plx_mapper: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_d52_plx_mapper: OK (%d checks)\n", g_checks);
    return 0;
}
