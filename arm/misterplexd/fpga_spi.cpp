#include <execinfo.h>
#include <cstring>
#include <unistd.h>
#include "fpga_spi.hpp"
#include "lock_paths.hpp"
#include "libmisterplex/cached_src_phys.hpp"
#include "libmisterplex/fabric_direct.hpp"
#include "libmisterplex/r1_idle_fb.hpp"
#include "libmisterplex/p720_audio_keepup.hpp"
#include "libmisterplex/pl330_mem2mem.hpp"
#include "libmisterplex/status_telemetry.hpp"
#include "libmisterplex/pixel_format.hpp"
#include "libmisterplex/spi_ack_wait.hpp"
#include "libmisterplex/mraudio_status.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/ddr_frame_wc.hpp"
#include "libmisterplex/spi_window.hpp"
#include "libmisterplex/d52_log.hpp"
#include "libmisterplex/status_heartbeat.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <signal.h>
#include <string>
#include <sys/syscall.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

// usleep
#include <time.h>

// Host stub tests point this at a sparse file; the product build uses /dev/mem.
#ifndef MPX_DEVMEM_PATH
#define MPX_DEVMEM_PATH "/dev/mem"
#endif
// Parallel host recipes each export MPX_DDR_FAKEMEM so they do not share one
// sparse file. Unset (the product, and mutant binaries) keeps the compile-time path.
inline const char* mpxDevmemPath() {
    if (const char* e = std::getenv("MPX_DDR_FAKEMEM"); e && *e)
        return e;
    return MPX_DEVMEM_PATH;
}
// Host tests point this at a fake WC window file; the product build opens the kmod's device.
#ifndef MPX_MPLEX_DDR_PATH
#define MPX_MPLEX_DDR_PATH "/dev/mplex_ddr"
#endif
// Host e2e builds point this at a fake MrAudio; the product build reads the driver.
#ifndef MPX_MRAUDIO_PATH
#define MPX_MRAUDIO_PATH "/dev/MrAudio"
#endif

namespace misterplex {

uint32_t FpgaSpi::resolveCachedSrcPhys(const void* virt, size_t len) {
    return resolveCachedSrcPhysFromPagemap(virt, len);
}

namespace {

// Fresh MAST (readFreshAudioConsumerStatus): publications awaited after the baseline
// read, and the bound. The mailbox republishes MAST on every poll, every few tens of
// microseconds, so a quarter second with none means the lane is stuck.
constexpr unsigned kAudioLaneFreshPublications = 3;
constexpr int kAudioLaneFreshMs = 250;

const char* audioControlName(audio_session::Control command) {
    switch (command) {
    case audio_session::Control::Begin: return "Begin";
    case audio_session::Control::Pause: return "Pause";
    case audio_session::Control::Resume: return "Resume";
    case audio_session::Control::Reset: return "Reset";
    }
    return "?";
}

constexpr uint8_t FIO_FILE_TX = 0x53;
constexpr uint8_t FIO_FILE_TX_DAT = 0x54;
constexpr uint8_t FIO_FILE_INDEX = 0x55;
constexpr uint8_t UIO_SET_STATUS2 = 0x1e;
constexpr uint8_t UIO_GET_STATUS = 0x29;
constexpr uint8_t UIO_GET_STRING = 0x14;
// Same-bank reuse floor when PLXD absent only. 40ms forced half-rate at 24fps;
// 16ms still covers ~one 60Hz glass period without multi-frame stalls.
constexpr int kDdrBankReuseMinUs = 16000;

// Serialize all HPS↔FPGA SPI (F1/F2/F3 + status). Audio + video + stream threads
// share one FpgaSpi; concurrent sendFileTx without this races GPO and Main pause.
// Also guards err_ (std::string is not thread-safe — concurrent writes crashed soak).
// recursive: setErr/lastError may run while SpiExclusive already holds the lock.
std::recursive_mutex& spiMutex() {
    static std::recursive_mutex m;
    return m;
}

// Ring backpressure must not hold the SPI lock needed by PCM delivery.
std::recursive_mutex& bitstreamMutex() {
    static std::recursive_mutex m;
    return m;
}

// MPX_OVERLAY_DECOUPLE=1 (FpgaSpi::setHotPathErrorsNonBlocking): error-text updates
// on the DDR hot paths skip instead of waiting for another thread's SPI window.
std::atomic<bool>& hotErrorsNonBlocking() {
    static std::atomic<bool> on{false};
    return on;
}
std::atomic<uint64_t>& hotErrorsSkipped() {
    static std::atomic<uint64_t> n{0};
    return n;
}

bool cleanDcacheRange(const void* p, size_t len) {
    if (!p || !len)
        return true;
#if defined(__arm__)
#ifndef __ARM_NR_BASE
#define __ARM_NR_BASE 0x0f0000
#endif
#ifndef __ARM_NR_cacheflush
#define __ARM_NR_cacheflush (__ARM_NR_BASE + 2)
#endif
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = start + len;
    return ::syscall(__ARM_NR_cacheflush, start, end, 0) == 0;
#else
    (void)p;
    (void)len;
    return true;
#endif
}

int64_t elapsedUs(std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

// Main_MiSTer owns the same SPI; STOP it for exclusive status/file ops.
// Matches /media/fat/MiSTer and MiSTer_groovy-style host binaries.
// IMPORTANT: no system()/fork here — multi-threaded misterplexd (F1+F2+F3) must
// not call system() under load (glibc fork+malloc deadlock → silent process death).
std::vector<pid_t> findMisterPids() {
    std::vector<pid_t> out;
    DIR* d = opendir("/proc");
    if (!d)
        return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        char path[96];
        std::snprintf(path, sizeof(path), "/proc/%.32s/cmdline", e->d_name);
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char buf[256]{};
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0)
            continue;
        // cmdline is NUL-separated argv; argv0 is first token
        const char* argv0 = buf;
        if (std::strstr(argv0, "misterplex") != nullptr)
            continue;
        // Exact basename match for main host binaries only (avoid "mister*" false positives)
        const char* base = std::strrchr(argv0, '/');
        base = base ? base + 1 : argv0;
        if (std::strcmp(base, "MiSTer") == 0 || std::strcmp(base, "MiSTer_groovy") == 0)
            out.push_back(static_cast<pid_t>(std::atoi(e->d_name)));
    }
    closedir(d);
    return out;
}

// Bits Main sets in GPO while it owns a SPI transaction (spi.cpp EnableFpga/
// EnableOsd/EnableIO) plus the handshake strobe. If every one of these is clear
// then Main is between transactions and cannot be hurt by us driving the bus.
constexpr uint32_t kSspiStrobe = 1u << 17;
constexpr uint32_t kSspiFpgaEn = 1u << 18;
constexpr uint32_t kSspiOsdEn = 1u << 19;
constexpr uint32_t kSspiIoEn = 1u << 20;
constexpr uint32_t kMainBusyMask = kSspiStrobe | kSspiFpgaEn | kSspiOsdEn | kSspiIoEn;

volatile uint32_t* gpoReg(volatile uint32_t* map) {
    constexpr uint32_t kMgrBase = 0xFF706000;
    constexpr uint32_t kMapBase = 0xFF000000;
    return map + ((kMgrBase - kMapBase + 0x10) >> 2);
}

// Non-zero while this process holds Main stopped, so resumeStrandedMain() does
// not fight a window we are legitimately holding right now.
std::atomic<int>& mainPauseDepth() {
    static std::atomic<int> d{0};
    return d;
}

// Read the scheduler state character from /proc/<pid>/stat. 'T' = stopped by a
// signal. stat is "pid (comm) state ..." and comm may contain spaces or ')', so
// scan back from the LAST ')' rather than tokenising forward.
char procStateFile(const char* path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char buf[256]{};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    const char* close_paren = std::strrchr(buf, ')');
    if (!close_paren)
        return 0;
    const char* s = close_paren + 1;
    while (*s == ' ')
        ++s;
    return *s;
}

char misterProcState(pid_t p) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(p));
    return procStateFile(path);
}

// A window in which it is provably safe to drive GPO ourselves.
//
// Acquire: SIGSTOP Main -> wait for /proc to actually report state 'T' (kill()
// returns long before the target stops) -> read the live GPO register. If any
// of Main's transaction-enable bits or the strobe is set, Main was frozen inside
// fpga_spi() and touching GPO now would hang it forever; resume it, back off and
// retry. All-clear means Main is parked between transactions and, being stopped,
// cannot start another one.
//
// Release: put GPO back exactly as Main left it, THEN SIGCONT. Main's shadow
// copy (`gpo_copy`) is never re-read from hardware, so handing the register back
// bit-for-bit is what keeps its view of the world true.
//
// Only the outermost SpiExclusive builds a window; nested frames inherit it.
struct MainSafeWindow {
    volatile uint32_t* map = nullptr;
    std::vector<pid_t> pids;
    uint32_t savedGpo = 0;
    bool enabled = false;
    bool safe = false;

    // A process-directed SIGSTOP initiates a *group* stop: the thread-group
    // leader can already report 'T' while a worker thread is still running.
    // Main is multi-threaded (offload.cpp) and its workers touch SPI, so it is
    // not enough to look at /proc/<pid>/stat — every task in /proc/<pid>/task
    // must have stopped before the GPO sample means anything.
    static bool allTasksStopped(pid_t p) {
        char dir[64];
        std::snprintf(dir, sizeof(dir), "/proc/%d/task", static_cast<int>(p));
        DIR* d = opendir(dir);
        if (!d)
            return true; // process is gone; it can no longer race us
        bool all = true;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9')
                continue;
            char path[128];
            std::snprintf(path, sizeof(path), "%.48s/%.16s/stat", dir, e->d_name);
            const char st = procStateFile(path);
            if (st != 'T' && st != 0) {
                all = false;
                break;
            }
        }
        closedir(d);
        return all;
    }

    static bool waitStopped(const std::vector<pid_t>& pids, int maxUs) {
        for (int waited = 0; waited <= maxUs; waited += 200) {
            bool all = true;
            for (pid_t p : pids) {
                if (!allTasksStopped(p)) {
                    all = false;
                    break;
                }
            }
            if (all)
                return true;
            usleep(200);
        }
        return false;
    }

    MainSafeWindow(volatile uint32_t* m, int attempts, bool enable) : map(m), enabled(enable) {
        if (!enabled)
            return;
        mainPauseDepth().fetch_add(1);
        for (int attempt = 0; attempt < attempts && !safe; ++attempt) {
            pids = findMisterPids();
            if (pids.empty()) {
                // No Main on this system (dev host) — nobody to race.
                if (map)
                    savedGpo = *gpoReg(map);
                safe = true;
                break;
            }
            for (pid_t p : pids)
                kill(p, SIGSTOP);
            if (waitStopped(pids, 50000) && map) { MPX_TEST_MAINSAFE_HOOK()
                savedGpo = *gpoReg(map);
                if ((savedGpo & kMainBusyMask) == 0) {
                    safe = true;
                    break;
                }
            }
            // Main is mid-transaction (or would not stop): let it finish and
            // try again a little later. Backing off is always better than
            // corrupting a handshake we cannot repair.
            for (pid_t p : pids)
                kill(p, SIGCONT);
            usleep(500 + static_cast<unsigned>(attempt) * 500);
        }
        if (!safe) {
            // Never leave Main stopped just because we gave up.
            for (pid_t p : findMisterPids())
                kill(p, SIGCONT);
        }
    }

    ~MainSafeWindow() {
        if (!enabled)
            return;
        if (safe && map)
            *gpoReg(map) = savedGpo;
        auto now = findMisterPids();
        if (now.empty())
            now = pids;
        for (pid_t p : now)
            kill(p, SIGCONT);
        mainPauseDepth().fetch_sub(1);
    }
    MainSafeWindow(const MainSafeWindow&) = delete;
    MainSafeWindow& operator=(const MainSafeWindow&) = delete;
};

// Cross-process lock. Held for the whole window, including the GPO restore —
// another process writing GPO while we are sampling or restoring would make
// savedGpo a lie and could hang Main exactly as before.
struct FlockGuard {
    int fd = -1;
    explicit FlockGuard(bool enable) {
        if (!enable)
            return;
        fd = ::open(misterplex::spiLockPath(), O_CREAT | O_RDWR, 0666);
        if (fd >= 0)
            flock(fd, LOCK_EX);
    }
    ~FlockGuard() {
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            ::close(fd);
        }
    }
    FlockGuard(const FlockGuard&) = delete;
    FlockGuard& operator=(const FlockGuard&) = delete;
};

// Nesting state, only ever touched while spiMutex (recursive) is held.
int& spiDepth() {
    static int d = 0;
    return d;
}
bool& spiWindowSafe() {
    static bool s = false;
    return s;
}

// Every SPI transaction goes through this; there is no "fast, unpaused" variant,
// because an unsynchronised GPO write can hang Main just as dead as a paused one.
//
// Member order IS the lock order: mutex -> flock -> window on the way in, and
// the exact reverse on the way out (members are destroyed in reverse declaration
// order, after the destructor body). The flock must outlive the window so that
// GPO is restored and Main resumed while we still hold the cross-process lock.
// No system()/fork under the lock: multi-threaded misterplexd must not fork
// (glibc fork+malloc deadlock).
//
// Core gate (core_gate.hpp): unless the gate is open and the fabric reads live (FPGA
// manager in user mode, core out of reset) right now, the outermost frame takes no flock
// and builds no window, so Main is never stopped and nothing is driven. safe() is false
// and every caller returns before touching GPI/GPO. A non-live fabric closes the gate.
// The fabric is read again once Main is stopped: a load Main began between the first
// read and the stop has set GPO bit 30 by then (fpga_io.cpp:474), so no word goes out.
bool spiGateAllows(volatile uint32_t* map) {
    CoreGate& gate = CoreGate::process();
    if (!gate.managed())
        return true; // lab tools and non-gate tests: no gate, as before the gate
    const bool open = gate.open();
    const bool hbIdle = statusHeartbeatScope() && gate.idleErrorPaintAllowed();
    if (!open && !hbIdle)
        return false;
    const FabricState fabric = readFabricState(map);
    if (fabric.live())
        return true;
    if (fabric.known && open) // an unmapped register file is no reload signal
        gate.closeNow(fabric.describe());
    return false;
}

struct SpiExclusive {
    std::lock_guard<std::recursive_mutex> mu;
    bool outer;
    bool gate;
    FlockGuard lock;
    MainSafeWindow win;
    explicit SpiExclusive(volatile uint32_t* map, int attempts = 8)
        : mu(spiMutex()), outer(spiDepth()++ == 0), gate(!outer || spiGateAllows(map)),
          lock(outer && gate), win(map, attempts, outer && gate) {
        if (outer)
            spiWindowSafe() = gate && win.safe && spiGateAllows(map);
    }
    ~SpiExclusive() { --spiDepth(); }
    // False when Main could not be parked safely: the caller must not touch SPI.
    // Nested frames report the outermost window's real state, never a blind true.
    bool safe() const { return spiWindowSafe(); }
    SpiExclusive(const SpiExclusive&) = delete;
    SpiExclusive& operator=(const SpiExclusive&) = delete;
};

bool gpiUserMode(volatile uint32_t* map) {
    if (!map)
        return false;
    constexpr uint32_t kMgrBase = 0xFF706000;
    constexpr uint32_t kMapBase = 0xFF000000;
    volatile uint32_t* gpi = map + ((kMgrBase - kMapBase + 0x14) >> 2);
    // MiSTer: high bit of GPI indicates not-in-user-mode (core reconfig / menu).
    return (static_cast<int>(*gpi) >= 0);
}

} // namespace

#ifdef MPX_TEST_F1_BEAT_SEAM
// Test seam (tests/unit/test_f1_plxl_beat.cpp only): the F1 x2 tooth holds spiMutex for 2 s.
std::recursive_mutex& mpxTestSpiMutex() { return spiMutex(); }
#endif

#ifdef MPX_TEST_WEDGE_READ_SEAM
// Test seam (tests/unit/test_wedge_read.cpp only): the W-NOLOCK row drives the spiDepth
// gauge directly so Main-busy depth is held without a real Main SPI transaction.
int& mpxTestSpiDepth() { return spiDepth(); }
// W-XTHREAD row: lets a second test thread hold spiMutex like an SpiExclusive frame.
std::recursive_mutex& mpxTestWedgeSpiMutex() { return spiMutex(); }
// W-NOBMTX row: the test holds the ring mutex around a service call.
std::recursive_mutex& mpxTestBitstreamMutex() { return bitstreamMutex(); }
// W-OFFROW row: count readWedgePage() SPI read attempts that passed the row gate, so a
// service that reads on a non-WEDGE row (mutant m-ungated) is caught without a bus.
int& mpxTestWedgeReadAttempts() { static int n = 0; return n; }
namespace {
std::mutex& wedgeReadTimesMu() { static std::mutex m; return m; }
std::vector<std::chrono::steady_clock::time_point>& wedgeReadTimesRaw() {
    static std::vector<std::chrono::steady_clock::time_point> v;
    return v;
}
}  // namespace
void mpxTestNoteWedgeRead() {
    std::lock_guard<std::mutex> g(wedgeReadTimesMu());
    wedgeReadTimesRaw().push_back(std::chrono::steady_clock::now());
}
std::vector<std::chrono::steady_clock::time_point> mpxTestWedgeReadTimes() {
    std::lock_guard<std::mutex> g(wedgeReadTimesMu());
    return wedgeReadTimesRaw();
}
#endif

FpgaSpi::~FpgaSpi() {
    close();
}

bool FpgaSpi::mainPaused() { return mainPauseDepth().load() > 0; }

bool FpgaSpi::mainAlive() { return !findMisterPids().empty(); }

void FpgaSpi::resumeStrandedMain() {
    // Never fight a pause we are legitimately holding right now.
    if (mainPauseDepth().load() > 0)
        return;
    for (pid_t p : findMisterPids()) {
        if (misterProcState(p) == 'T')
            kill(p, SIGCONT);
    }
}

namespace {

// async-signal-safe enough: kill()/open()/read()/close() are all on the safe
// list, and we re-raise with the default handler so the crash still surfaces.
void crashGuardHandler(int sig) {
    // FIELD-1018 F2b: a lab MiSTer 10:44 death left nothing in the log (no dmesg line on MiSTer either). Say which
    // signal killed us and where, with async-signal-safe write() + backtrace_symbols_fd(), before re-raising.
    {
        char msg[64] = "misterplexd: FATAL signal ";
        size_t n = std::strlen(msg);
        int v = sig, d = 100;
        while (d > v && d > 1) d /= 10;
        for (; d > 0; d /= 10) msg[n++] = static_cast<char>('0' + (v / d) % 10);
        msg[n++] = '\n';
        ssize_t w = ::write(2, msg, n);
        (void)w;
        void* bt[32];
        ::backtrace_symbols_fd(bt, ::backtrace(bt, 32), 2);
    }
    mainPauseDepth().store(0);
    for (pid_t p : findMisterPids())
        kill(p, SIGCONT);
    std::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

void FpgaSpi::installCrashGuard() {
    // SIGKILL cannot be caught — resumeStrandedMain() at startup covers it.
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGQUIT})
        std::signal(sig, crashGuardHandler);
}

void FpgaSpi::setErr(std::string msg) {
    std::lock_guard<std::recursive_mutex> g(spiMutex());
    err_ = std::move(msg);
}

void FpgaSpi::clearErr() {
    if (hotErrorsNonBlocking().load(std::memory_order_relaxed)) {
        if (!updateErrorUnlessBusy(spiMutex(), err_, nullptr))
            hotErrorsSkipped().fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::recursive_mutex> g(spiMutex());
    err_.clear();
}

void FpgaSpi::setErrHot(std::string msg) {
    if (hotErrorsNonBlocking().load(std::memory_order_relaxed)) {
        if (!updateErrorUnlessBusy(spiMutex(), err_, msg.c_str()))
            hotErrorsSkipped().fetch_add(1, std::memory_order_relaxed);
        return;
    }
    setErr(std::move(msg));
}

void FpgaSpi::setHotPathErrorsNonBlocking(bool on) {
    hotErrorsNonBlocking().store(on, std::memory_order_relaxed);
}

bool FpgaSpi::hotPathErrorsNonBlocking() {
    return hotErrorsNonBlocking().load(std::memory_order_relaxed);
}

uint64_t FpgaSpi::hotPathErrorsSkipped() {
    return hotErrorsSkipped().load(std::memory_order_relaxed);
}

void FpgaSpi::withSpiLockForTest(const std::function<void()>& fn) {
    std::lock_guard<std::recursive_mutex> g(spiMutex());
    fn();
}

std::string FpgaSpi::lastError() const {
    std::lock_guard<std::recursive_mutex> g(spiMutex());
    return err_;
}

bool FpgaSpi::open() {
    close();
    fd_ = ::open(mpxDevmemPath(), O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd_ < 0) {
        setErr("open /dev/mem failed");
        return false;
    }
    void* p = mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, kMapBase);
    if (p == MAP_FAILED) {
        setErr("mmap FPGA regs failed");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    map_ = static_cast<volatile uint32_t*>(p);
    CoreGate::process().setFabricRegs(map_);
    // Seed gpo from hardware
    volatile uint32_t* gpo = map_ + ((kMgrBase - kMapBase + 0x10) >> 2);
    gpo_copy_ = *gpo;
    clearErr();
    return true;
}

void FpgaSpi::setNegotiatedBuild(const FpgaBuildInfo* build) {
    negotiatedBuild_.store(build);
    if (build && build->wedgePage && ok())
        (void)setStatusBit(119, 0);
}

void FpgaSpi::close() {
#ifndef MUT_D52_NO_DETACH   // test-only mutant (test_d52_plx_mapper D-c)
    d52::detachReader(this); // D52-LOG: no PLX read through this object starts after this (none in flight)
#endif
    releaseDdrMap();
    releaseBitstreamDdrMap();
    if (map_) {
        CoreGate::process().dropFabricRegs(map_);
        munmap((void*)map_, kMapSize);
        map_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    ddrKickMode_ = 0;
    ddrKickFailMs_ = -1.0;
    doorbellSeq_ = 0;
}

void FpgaSpi::onCoreReturned() {
    std::lock_guard<std::recursive_mutex> g(spiMutex());
    ddrKickMode_ = 0;
    ddrKickFailMs_ = -1.0;
    doorbellSeq_ = 0;
    mboxInit_ = false;
    mboxAlive_ = false;
    mailboxEpoch_.fetch_add(1, std::memory_order_acq_rel);
    negotiatedBuild_.store(nullptr);
}

void FpgaSpi::dropStaleSession() {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    releaseBitstreamDdrMap();
}

bool FpgaSpi::coreGateAllows(const char* what, bool ringProbe) {
    CoreGate& gate = CoreGate::process();
    if (!gate.managed())
        return true; // lab tools and non-gate tests: no gate, as before the gate
    if (!gate.open() && !(ringProbe && gate.ringProbeAllowed())) {
        const bool idleDdr = idleErrorPaintScope() && gate.idleErrorPaintAllowed() && what &&
                             (std::strcmp(what, "ensureDdrMap") == 0 ||
                              std::strcmp(what, "ensureDdrMailboxMap") == 0 ||
                              std::strcmp(what, "commitDdrBankIngest") == 0);
        if (!idleDdr) {
            setErr(coreGateClosedError(what));
            return false;
        }
    }
    // Re-read the fabric right before the access: a reprogram can start between ticks,
    // while CORENAME still says Plex.
    const FabricState fabric = readFabricState(map_);
    if (!fabric.live()) {
        if (fabric.known) // an unmapped register file is no reload signal
            gate.closeNow(fabric.describe());
        setErr(std::string(what) + ": " + fabric.describe());
        return false;
    }
    return true;
}

MainIdentity FpgaSpi::mainIdentity() {
    MainIdentity oldest;
    for (pid_t p : findMisterPids()) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(p));
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0)
            continue;
        buf[n] = 0;
        // "pid (comm) state ppid ...": starttime is field 22, counting from pid = 1.
        const char* q = std::strrchr(buf, ')');
        if (!q)
            continue;
        ++q;
        int field = 2;
        bool got = false;
        uint64_t start = 0;
        while (*q) {
            while (*q == ' ')
                ++q;
            if (!*q)
                break;
            if (++field == 22) {
                start = std::strtoull(q, nullptr, 10);
                got = true;
                break;
            }
            while (*q && *q != ' ')
                ++q;
        }
        if (!got)
            continue;
        if (oldest.pid == 0 || start < oldest.startTicks ||
            (start == oldest.startTicks && static_cast<int>(p) < oldest.pid))
            oldest = {static_cast<int>(p), start};
    }
    return oldest;
}

bool FpgaSpi::probeVideoCapabilities(VideoCapabilities& capabilities, int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    capabilities = {};
    if (bitstreamSessionActive_) {
        setErr("probeVideoCapabilities: a session is active");
        return false;
    }
    uint64_t nonce = 0;
    const int randomFd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (randomFd < 0) {
        setErr("probeVideoCapabilities: cannot obtain a fresh core challenge");
        return false;
    }
    size_t filled = 0;
    while (filled < sizeof(nonce)) {
        const ssize_t count = ::read(randomFd,
            reinterpret_cast<uint8_t*>(&nonce) + filled, sizeof(nonce) - filled);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        filled += static_cast<size_t>(count);
    }
    ::close(randomFd);
    if (filled != sizeof(nonce) || nonce == 0) {
        setErr("probeVideoCapabilities: incomplete core challenge");
        return false;
    }
    if (!resetBitstreamRing(timeout_ms))
        return false;
    if (writeBitstreamRecord(ring::Event::Probe, nonce, mailbox_abi::kFpgaVideoLayoutId,
                            0, nullptr, 0, timeout_ms) != BitstreamPushResult::Ok)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    do {
        if (readVideoCapabilities(nonce, capabilities)) {
            clearErr();
            return true;
        }
        BitstreamStatus status;
        if (readBitstreamStatus(status) && (status.fatal || status.desync)) {
            setErr("probeVideoCapabilities: core rejected FPGA-video ABI negotiation");
            return false;
        }
        usleep(500);
    } while (std::chrono::steady_clock::now() < deadline);
    setErr("probeVideoCapabilities: no fresh core capability reply");
    return false;
}

CoreGateEvent FpgaSpi::runCoreGateProbe(int timeout_ms, int64_t nowUs) {
    CoreGate& gate = CoreGate::process();
    if (!gate.managed() || gate.state() != CoreGateState::Verifying)
        return CoreGateEvent::None;
    VideoCapabilities caps;
    bool replied = false;
    std::string why;
    {
        std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
        if (bitstreamSessionActive_ || bitstreamResetPending_ || bitstreamAbortPending_)
            releaseBitstreamDdrMap();
        gate.beginProbe();
        replied = probeVideoCapabilities(caps, timeout_ms);
        gate.endProbe();
        if (replied)
            gateCapabilities_ = caps;
        if (!replied)
            why = lastError();
    }
    std::string refusal;
    std::string raster;
    std::string row = why;
    if (replied) {
        if (const FpgaBuildInfo* b = recognisedFpgaBuild(caps.build_id)) {
            row = fpgaBuildLabel(*b);
            raster = b->raster;
        } else {
            row = retiredFpgaBuild(caps.build_id) ? "a retired FPGA build"
                                                   : "not in the FPGA build table";
        }
        if (!fpgaBuildRefusal(caps.build_id).empty())
            refusal = "not on the allowlist";
    }
    return gate.probeResult(replied, caps.build_id, refusal, raster, row, nowUs);
}

bool FpgaSpi::ensureBitstreamDdrMap() {
    namespace ring = ddr_bitstream_ring;
    if (!coreGateAllows("ensureBitstreamDdrMap", /*ringProbe=*/true))
        return false;
    if (bitstreamMap_ && bitstreamMemFd_ >= 0)
        return true;
    releaseBitstreamDdrMap();
    bitstreamMapLen_ = ring::kRingBytes + 0x1000u;
    bitstreamMemFd_ = ::open(mpxDevmemPath(), O_RDWR | O_CLOEXEC | O_SYNC);
    if (bitstreamMemFd_ < 0) {
        setErr("ensureBitstreamDdrMap: open /dev/mem failed");
        return false;
    }
    void* p = mmap(nullptr, bitstreamMapLen_, PROT_READ | PROT_WRITE, MAP_SHARED,
                   bitstreamMemFd_, static_cast<off_t>(ring::kDataPhys));
    if (p == MAP_FAILED) {
        setErr("ensureBitstreamDdrMap: mmap bitstream ring failed");
        ::close(bitstreamMemFd_);
        bitstreamMemFd_ = -1;
        bitstreamMapLen_ = 0;
        return false;
    }
    bitstreamMap_ = static_cast<uint8_t*>(p);
    return true;
}

void FpgaSpi::releaseBitstreamDdrMap() {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (bitstreamMap_) {
        munmap(bitstreamMap_, bitstreamMapLen_);
        bitstreamMap_ = nullptr;
        bitstreamMapLen_ = 0;
    }
    if (bitstreamMemFd_ >= 0) {
        ::close(bitstreamMemFd_);
        bitstreamMemFd_ = -1;
    }
    bitstreamWriteCount_ = 0;
    bitstreamLegacySeq_ = 0;
    bitstreamLegacyActive_ = false;
    bitstreamResetEpoch_ = false;
    bitstreamProtocolVersion_ = 0;
    bitstreamSessionActive_ = false;
    bitstreamSessionId_ = 0;
    bitstreamNextSeq_ = 0;
    audioSessionOwned_ = false;
    bitstreamResetPending_ = false;
    bitstreamAbortPending_ = false;
    videoCapabilities_ = {};
}

bool FpgaSpi::readBitstreamFpgaCount(uint32_t& readCount) {
    namespace ring = ddr_bitstream_ring;
    if (!ensureBitstreamDdrMap())
        return false;
    const size_t off = ring::kReadPhys - ring::kDataPhys;
    volatile uint64_t* p = reinterpret_cast<volatile uint64_t*>(bitstreamMap_ + off);
    const uint64_t raw = *p;
    if (static_cast<uint32_t>(raw) != ring::kReadMagic)
        return false;
    readCount = static_cast<uint32_t>(raw >> 32) & ring::kCountMask;
    return true;
}

bool FpgaSpi::readBitstreamStatus(BitstreamStatus& status) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    status = BitstreamStatus{};
    if (!ensureBitstreamDdrMap())
        return false;
    auto read64 = [&](uint32_t phys) -> uint64_t {
        const size_t off = phys - ring::kDataPhys;
        volatile uint64_t* p = reinterpret_cast<volatile uint64_t*>(bitstreamMap_ + off);
        return *p;
    };
    const uint64_t readRaw = read64(ring::kReadPhys);
    const uint64_t errRaw = read64(ring::kErrPhys);
    const uint64_t st0 = read64(ring::kStat0Phys);
    const uint64_t st1 = read64(ring::kStat1Phys);
    const uint64_t st2 = read64(ring::kStat2Phys);
    const uint64_t st3 = read64(ring::kStat3Phys);
    const uint64_t st4 = read64(ring::kStat4Phys);
    const uint64_t st5 = read64(ring::kStat5Phys);
    const uint64_t st6 = read64(ring::kStat6Phys);
    if (static_cast<uint32_t>(readRaw) != ring::kReadMagic ||
        static_cast<uint32_t>(errRaw) != ring::kErrMagic)
        return false;
    status.producer_count = bitstreamWriteCount_;
    status.consumer_count = static_cast<uint32_t>(readRaw >> 32) & ring::kCountMask;
    (void)ring::decodeErrStatusWord(errRaw, status);
    if (static_cast<uint32_t>(st0) == ring::kStat0Magic)
        status.ring_level = static_cast<uint32_t>(st0 >> 32);
    else
        status.ring_level = ring::countDistance(bitstreamWriteCount_, status.consumer_count);
    if (static_cast<uint32_t>(st1) == ring::kStat1Magic)
        status.consumer_seq = static_cast<uint32_t>(st1 >> 32);
    if (static_cast<uint32_t>(st2) == ring::kStat2Magic)
        status.last_bad_seq = static_cast<uint32_t>(st2 >> 32);
    if (static_cast<uint32_t>(st3) == ring::kStat3Magic)
        status.session_id = static_cast<uint32_t>(st3 >> 32);
    if (static_cast<uint32_t>(st4) == ring::kStat4Magic)
        status.session_id |= static_cast<uint64_t>(static_cast<uint32_t>(st4 >> 32)) << 32;
    (void)ring::decodeStat5StatusWord(st5, status);
    (void)ring::decodeStat6StatusWord(st6, status);
    return true;
}

bool FpgaSpi::waitBitstreamReadCount(uint32_t target, int timeout_ms) {
    const int stepUs = 500;
    const int maxUs = std::max(0, timeout_ms) * 1000;
    for (int waited = 0; waited <= maxUs; waited += stepUs) {
        uint32_t readCount = 0;
        if (readBitstreamFpgaCount(readCount) &&
            readCount == (target & ddr_bitstream_ring::kCountMask))
            return true;
        usleep(stepUs);
    }
    setErr("waitBitstreamReadCount: FPGA did not acknowledge the complete record");
    return false;
}

bool FpgaSpi::waitBitstreamControl(ddr_bitstream_ring::Event event, uint64_t session_id,
                                  uint32_t target, int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    do {
        BitstreamStatus status;
        if (readBitstreamStatus(status)) {
            if (status.fatal || status.desync) {
                setErr("waitBitstreamControl: FPGA rejected the session record");
                return false;
            }
            bool stateMatches = status.session_id == session_id;
            if (event == ring::Event::End)
                stateMatches = !status.active;
            else if (event == ring::Event::Pause)
                stateMatches = stateMatches && status.active && status.paused;
            else if (event == ring::Event::Resume || event == ring::Event::Begin)
                stateMatches = stateMatches && status.active && !status.paused;
            if (status.consumer_count == (target & ring::kCountMask) && stateMatches) {
                clearErr();
                return true;
            }
        }
        usleep(500);
    } while (std::chrono::steady_clock::now() < deadline);
    setErr("waitBitstreamControl: FPGA session/fence acknowledgement timed out");
    return false;
}

bool FpgaSpi::requireBitstreamSession(uint64_t session_id, const char* operation, bool hot) {
    if (bitstreamSessionActive_ && session_id == bitstreamSessionId_ &&
        !bitstreamAbortPending_ && !bitstreamResetPending_)
        return true;
    std::string msg = std::string(operation) + ": stale or inactive session";
    if (hot)
        setErrHot(std::move(msg));
    else
        setErr(std::move(msg));
    return false;
}

FpgaSpi::BitstreamPushResult FpgaSpi::writeBitstreamRecord(ddr_bitstream_ring::Event event,
                                                           uint64_t session_id,
                                                           uint32_t seq,
                                                           uint8_t nal_type,
                                                           const uint8_t* payload,
                                                           size_t len,
                                                           int timeout_ms,
                                                           const uint8_t* prefix,
                                                           size_t prefix_len) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    const bool dataRecord = event == ring::Event::Nal || event == ring::Event::AccessUnit;
    const size_t capacity = ring::kRingBytes - (dataRecord ? ring::kControlReserveBytes : 0);
    if ((len && !payload) || (prefix_len && !prefix)) {
        setErr("writeBitstreamRecord: empty payload pointer");
        return BitstreamPushResult::Fatal;
    }
    if (prefix_len > capacity - ring::kRecordHeaderBytes ||
        len > capacity - ring::kRecordHeaderBytes - prefix_len) {
        setErr("writeBitstreamRecord: record larger than ring");
        return BitstreamPushResult::Fatal;
    }
    const size_t recordLen = ring::kRecordHeaderBytes + prefix_len + len;
    if (!ensureBitstreamDdrMap())
        return BitstreamPushResult::Fatal;

    auto statusIsFatal = [&]() {
        BitstreamStatus st;
        return readBitstreamStatus(st) && (st.fatal || st.desync);
    };
    if (statusIsFatal()) {
        setErr("writeBitstreamRecord: FPGA reports transport desync");
        return BitstreamPushResult::Desync;
    }

    uint32_t readCount = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    while (true) {
        const bool haveCount = readBitstreamFpgaCount(readCount);
        const uint32_t used = ring::countDistance(bitstreamWriteCount_, readCount);
        if (haveCount && used > ring::kRingBytes) {
            setErr("writeBitstreamRecord: invalid FPGA consumer count");
            return BitstreamPushResult::Desync;
        }
        if (haveCount && used + recordLen <= capacity)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            // Ring full is the AU push's retryable answer (timeout 0, every attempt):
            // a hot-path error. Missing telemetry is fatal and keeps the plain setErr.
            if (haveCount)
                setErrHot("writeBitstreamRecord: FPGA ring full");
            else
                setErr("writeBitstreamRecord: FPGA consumer telemetry unavailable");
            return haveCount ? BitstreamPushResult::Full : BitstreamPushResult::Fatal;
        }
        usleep(500);
        if (statusIsFatal()) {
            setErr("writeBitstreamRecord: FPGA reports transport desync");
            return BitstreamPushResult::Desync;
        }
    }

    std::array<uint8_t, ring::kRecordHeaderBytes> header{};
    ring::putLe32(header.data(), ring::kRecordMagic);
    header[4] = static_cast<uint8_t>(event);
    header[5] = nal_type;
    const uint16_t version = event == ring::Event::Probe
                                 ? mailbox_abi::kFpgaVideoAbiVersion
                                 : bitstreamProtocolVersion_;
    header[6] = static_cast<uint8_t>(version);
    header[7] = static_cast<uint8_t>(version >> 8);
    ring::putLe64(header.data() + 8, session_id);
    ring::putLe32(header.data() + 16, seq);
    ring::putLe32(header.data() + 20, static_cast<uint32_t>(prefix_len + len));

    auto writeBytes = [&](const uint8_t* src, size_t n) {
        uint32_t wr = bitstreamWriteCount_ & static_cast<uint32_t>(ring::kRingBytes - 1u);
        const size_t first = std::min(n, static_cast<size_t>(ring::kRingBytes - wr));
        ring::copyToDeviceBytes(bitstreamMap_ + wr, src, first);
        if (first < n)
            ring::copyToDeviceBytes(bitstreamMap_, src + first, n - first);
        bitstreamWriteCount_ = (bitstreamWriteCount_ + static_cast<uint32_t>(n)) &
                              ring::kCountMask;
    };
    writeBytes(header.data(), header.size());
    if (prefix_len)
        writeBytes(prefix, prefix_len);
    if (len)
        writeBytes(payload, len);
    __sync_synchronize();
    publishBitstreamCtrl();
    if (!dataRecord)
        if (d52::Logger* d = d52::active())
            d->ringRecord(d52::eventName(static_cast<int>(event)), session_id, bitstreamWriteCount_);
    clearErr();
    return BitstreamPushResult::Ok;
}

void FpgaSpi::publishBitstreamCtrl() {
    namespace ring = ddr_bitstream_ring;
    const size_t off = ring::kCtrlPhys - ring::kDataPhys;
    volatile uint64_t* p = reinterpret_cast<volatile uint64_t*>(bitstreamMap_ + off);
    *p = (static_cast<uint64_t>(bitstreamResetEpoch_ ? 1u : 0u) << 63) |
         (static_cast<uint64_t>(bitstreamWriteCount_ & 0x7fffffffu) << 32) |
         ring::kCtrlMagic;
}

bool FpgaSpi::ensureDdrMap() {
    if (!coreGateAllows("ensureDdrMap"))
        return false;
    if (ddrMap_ && ddrMemFd_ >= 0)
        return true;
    releaseDdrMap();
    // Map both frame banks. The doorbell/mailbox page after them stays out of this window: its
    // one mapper is ensureDdrMailboxMap (review 08:17 (a)), so 320×240 maps 0x7F000 and larger
    // cores grow this at runtime.
#ifdef MUT_DDR_WINDOW_COVERS_PAGE   // test-only mutant: the window maps the doorbell page too
    const size_t kLen = ddrLayout_.map_bytes;
#else
    const size_t kLen = static_cast<size_t>(ddrLayout_.doorbell_phys - ddrLayout_.phys_base);
#endif
    void* p = MAP_FAILED;
    ddrMapViaMplex_ = false;
    ddrMapBase_ = nullptr;
    ddrMapBaseLen_ = 0;

    // K1-B1 (ddr_frame_wc.hpp): frames and the doorbell use the WC window only with
    // MPX_DDR_FRAME_WC=1. By default they stay on /dev/mem whether or not the kmod is loaded.
    const DdrFrameWcSetting wc = parseDdrFrameWc(std::getenv(kDdrFrameWcEnv));

    // Product 720p: /dev/mplex_ddr is pgprot_writecombine (~2 ms/frame vs ~15 ms
    // uncached /dev/mem). mmap offset 0 always (kmod rejects vm_pgoff != 0);
    // ddrMap_ is the phys_base slice inside that window.
    if (wc == DdrFrameWcSetting::On &&
        mplexDdrCovers(ddrLayout_.phys_base, static_cast<uint32_t>(kLen))) {
        const size_t winOff =
            static_cast<size_t>(ddrLayout_.phys_base - kMplexDdrPhys);
        const size_t need = winOff + kLen;
        int mfd = ::open(MPX_MPLEX_DDR_PATH, O_RDWR | O_CLOEXEC);
        if (mfd >= 0) {
            void* base = mmap(nullptr, need, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
            if (base != MAP_FAILED) {
                ddrMemFd_ = mfd;
                ddrMapBase_ = static_cast<uint8_t*>(base);
                ddrMapBaseLen_ = need;
                ddrMap_ = ddrMapBase_ + winOff;
                ddrMapLen_ = kLen;
                ddrMapViaMplex_ = true;
                std::fprintf(stderr,
                             "misterplexd: DDR frame map via /dev/mplex_ddr WC "
                             "phys=0x%08x off=0x%zx len=0x%zx\n",
                             ddrLayout_.phys_base, winOff, kLen);
                p = ddrMap_;
            } else {
                ::close(mfd);
            }
        }
    }

    if (p == MAP_FAILED) {
        int flags = O_RDWR | O_CLOEXEC;
        if (ddrMemSync_)
            flags |= O_SYNC;
        ddrMemFd_ = ::open(mpxDevmemPath(), flags);
        if (ddrMemFd_ < 0) {
            setErr("ensureDdrMap: open /dev/mem failed");
            return false;
        }
        p = mmap(nullptr, kLen, PROT_READ | PROT_WRITE, MAP_SHARED, ddrMemFd_,
                 static_cast<off_t>(ddrLayout_.phys_base));
        if (p == MAP_FAILED) {
            setErr("ensureDdrMap: mmap frame window failed");
            ::close(ddrMemFd_);
            ddrMemFd_ = -1;
            return false;
        }
        ddrMap_ = static_cast<uint8_t*>(p);
        ddrMapLen_ = kLen;
    }
    std::fprintf(stderr,
                 "misterplexd: ddr_frame_map path=%s wc_guard=%d phys=0x%08x len=0x%zx%s\n",
                 ddrMapViaMplex_ ? "mplex_ddr" : "devmem", wc == DdrFrameWcSetting::On ? 1 : 0,
                 ddrLayout_.phys_base, kLen,
                 wc == DdrFrameWcSetting::Invalid
                     ? " (MPX_DDR_FRAME_WC is neither 0 nor 1: treated as 0)"
                 : (wc == DdrFrameWcSetting::Off && ::access(MPX_MPLEX_DDR_PATH, F_OK) == 0)
                     ? " (" MPX_MPLEX_DDR_PATH " present, not used: MPX_DDR_FRAME_WC=1 allows WC)"
                     : "");
    {
        const auto pageLock = lockDdrPage();
        volatile uint32_t* dw = ddrMailboxWord(ddrLayout_.doorbell_phys);
        for (int attempt = 0; dw && attempt < 4; ++attempt) {
            const uint32_t lo0 = dw[0];
            const uint32_t hi = dw[1];
            const uint32_t lo1 = dw[0];
            if (lo0 != lo1)
                continue;
            uint32_t seq = 0;
            int bank = 0;
            if (decodeDdrDoorbell(lo0, hi, ddrLayout_.format, seq, bank)) {
                (void)bank;
                doorbellSeq_ = seq;
            }
            break;
        }
    }
    return true;
}

bool FpgaSpi::setDdrFrameLayout(const DdrFrameGeometry& geometry, DdrFrameFormat format) {
    DdrFrameLayout next = makeDdrFrameLayout(
        geometry, ddrFramePhysBaseForGeometry(geometry), kDdrFrameStrideAlign, format);
    if (!ddrFrameLayoutValid(next)) {
        setErr("setDdrFrameLayout: invalid DDR frame layout");
        return false;
    }
    if (next.phys_base == ddrLayout_.phys_base &&
        next.coded_width == ddrLayout_.coded_width &&
        next.coded_height == ddrLayout_.coded_height &&
        next.display_width == ddrLayout_.display_width &&
        next.display_height == ddrLayout_.display_height &&
        next.presented_width == ddrLayout_.presented_width &&
        next.presented_height == ddrLayout_.presented_height &&
        next.crop_left == ddrLayout_.crop_left && next.crop_right == ddrLayout_.crop_right &&
        next.crop_top == ddrLayout_.crop_top && next.crop_bottom == ddrLayout_.crop_bottom &&
        next.present_x == ddrLayout_.present_x && next.present_y == ddrLayout_.present_y &&
        next.format == ddrLayout_.format)
        return true;
    ddrLayout_ = next;
    ddrDoorbellPage_.store(next.doorbell_phys & ~0xFFFu);
    mailboxDoorbellPhys_.store(ddrLayout_.doorbell_phys, std::memory_order_release);
    releaseDdrMap();
    ddrKickMode_ = 0;
    ddrKickFailMs_ = -1.0;
    doorbellSeq_ = 0;
    clearErr();
    return true;
}

bool FpgaSpi::setDdrFrameLayout(int width, int height, DdrFrameFormat format) {
    return setDdrFrameLayout(makeDdrFrameGeometry(width, height), format);
}

void FpgaSpi::setDdrMemSync(bool on) {
    if (ddrMemSync_ == on)
        return;
    ddrMemSync_ = on;
    releaseDdrMap();
    ddrKickMode_ = 0;
    ddrKickFailMs_ = -1.0;
}

void FpgaSpi::releaseDdrMap() {
#ifdef MUT_CLOSE_SKIPS_PAGE_WAIT   // test-only mutant (test_ddr_page_mapper P-g/P-h): close() does not wait
    std::unique_lock<std::mutex> pageLock(ddrPageMu_, std::defer_lock);
#else
    const auto pageLock = lockDdrPage();   // the doorbell pages go with no access in flight
#endif
    strictDdrRelease_.reset();
    mboxInit_ = false;
    mboxAlive_ = false;
    inputMboxEdge_.reset();
    mailboxEpoch_.fetch_add(1, std::memory_order_acq_rel);
    if (ddrMapBase_) {
        munmap(ddrMapBase_, ddrMapBaseLen_);
        ddrMapBase_ = nullptr;
        ddrMapBaseLen_ = 0;
        ddrMap_ = nullptr;
        ddrMapLen_ = 0;
    } else if (ddrMap_) {
        munmap(ddrMap_, ddrMapLen_);
        ddrMap_ = nullptr;
        ddrMapLen_ = 0;
    }
    ddrMapViaMplex_ = false;
    if (ddrMemFd_ >= 0) {
        ::close(ddrMemFd_);
        ddrMemFd_ = -1;
    }
    releaseDdrPagesLocked();
}

// review 07:30 C2, 08:17 (a), 08:18: one lock for every doorbell-page map, access and unmap.
// Lock order: ddrPageMu_ before spiMutex (setErr) and the core gate's mutex; nothing that holds
// either of those takes ddrPageMu_.
std::unique_lock<std::mutex> FpgaSpi::lockDdrPage() {
#ifdef MUT_DDR_PAGE_UNLOCKED   // test-only mutant: the pre-C2 unsynchronized page map and accesses
    return std::unique_lock<std::mutex>(ddrPageMu_, std::defer_lock);
#else
    return std::unique_lock<std::mutex>(ddrPageMu_);
#endif
}

// Caller holds lockDdrPage().
void FpgaSpi::releaseDdrPagesLocked() {
    for (auto& s : ddrPages_) {
        if (s.map)
            munmap(s.map, 4096);
        if (s.fd >= 0)
            ::close(s.fd);
        s = DdrPageMap{};
    }
}

#if defined(MPX_TEST_DDR_PAGE_MAP_DELAY_US) || defined(MPX_TEST_DDR_PAGE_ACCESS_DELAY_US)
std::atomic<int> mpxTestDdrPageMapWindows{0};      // test seams: a first map / an access in flight
std::atomic<int> mpxTestDdrPageAccessWindows{0};
#endif
#ifdef MPX_TEST_DDR_PAGE_HOLD
// Test seam (T-PAGEMAP-CLOSE-WAIT): called with the page lock held at the end of a window, 1 = a first map
// (before its mmap), 2 = an access (before its load). A test may hold the window there for a forced schedule.
std::atomic<void (*)(int)> mpxTestDdrPageHold{nullptr};
#endif

// Caller holds lockDdrPage(). The one mmap call site of a doorbell page: each page is mapped once
// (uncached /dev/mem) and every user of that page gets the same mapping.
bool FpgaSpi::ensureDdrMailboxMap(uint32_t pagePhys, uint8_t*& map) {
    if (!coreGateAllows("ensureDdrMailboxMap"))
        return false;
    map = nullptr;
    for (const auto& s : ddrPages_) {
        if (s.map && s.phys == pagePhys) {
            map = s.map;
            return true;
        }
    }
    DdrPageMap* slot = nullptr;
    for (auto& s : ddrPages_) {
        if (!s.map) {
            slot = &s;
            break;
        }
    }
    if (!slot) {   // every slot holds another page: retire one (the lock: no access is in flight)
        slot = &ddrPages_[ddrPageEvict_++ % ddrPages_.size()];
        munmap(slot->map, 4096);
        ::close(slot->fd);
        *slot = DdrPageMap{};
    }
#ifdef MPX_TEST_DDR_PAGE_MAP_DELAY_US
    mpxTestDdrPageMapWindows.fetch_add(1);
    usleep(MPX_TEST_DDR_PAGE_MAP_DELAY_US);
#endif
#ifdef MPX_TEST_DDR_PAGE_HOLD
    if (auto* hold = mpxTestDdrPageHold.load())
        hold(1);
#endif
    const int fd = ::open(mpxDevmemPath(), O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        setErr("ensureDdrMailboxMap: open /dev/mem failed");
        return false;
    }
    void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(pagePhys));
    if (p == MAP_FAILED) {
        setErr("ensureDdrMailboxMap: mmap mailbox page failed");
        ::close(fd);
        return false;
    }
    slot->phys = pagePhys;
    slot->fd = fd;
    slot->map = static_cast<uint8_t*>(p);
    map = slot->map;
    return true;
}

// Caller holds lockDdrPage() for as long as it uses the word (the layout's doorbell page).
volatile uint32_t* FpgaSpi::ddrMailboxWord(uint32_t phys) {
    const uint32_t page = ddrLayout_.doorbell_phys & ~0xFFFu;
    uint8_t* map = nullptr;
    if (!ensureDdrMailboxMap(page, map) || !map)
        return nullptr;
    if (phys < page || phys + 8 > page + 4096u)
        return nullptr;
    return reinterpret_cast<volatile uint32_t*>(map + (phys - page));
}

bool FpgaSpi::plxPageRead32(uint32_t off, uint32_t& out) {
    if (off > 4096u - 4u || (off & 3u) != 0)
        return false;
    const auto pageLock = lockDdrPage();
    uint8_t* map = nullptr;
    if (!ensureDdrMailboxMap(ddrDoorbellPage_.load(), map) || !map)
        return false;
#ifdef MPX_TEST_DDR_PAGE_ACCESS_DELAY_US   // test seam: this access is in flight
    mpxTestDdrPageAccessWindows.fetch_add(1);
    usleep(MPX_TEST_DDR_PAGE_ACCESS_DELAY_US);
#endif
#ifdef MPX_TEST_DDR_PAGE_HOLD
    if (auto* hold = mpxTestDdrPageHold.load())
        hold(2);
#endif
    out = *reinterpret_cast<volatile uint32_t*>(map + off);
    return true;
}

bool FpgaSpi::plxPageRead32(uint32_t page, uint32_t off, uint32_t& out, bool* otherPage) {
    if (otherPage)
        *otherPage = false;
    if (off > 4096u - 4u || (off & 3u) != 0)
        return false;
    const auto pageLock = lockDdrPage();
    if (page != ddrDoorbellPage_.load()) {
        if (otherPage)
            *otherPage = true;
        return false;
    }
    uint8_t* map = nullptr;
    if (!ensureDdrMailboxMap(page, map) || !map)
        return false;
#ifdef MPX_TEST_DDR_PAGE_ACCESS_DELAY_US   // test seam: this access is in flight
    mpxTestDdrPageAccessWindows.fetch_add(1);
    usleep(MPX_TEST_DDR_PAGE_ACCESS_DELAY_US);
#endif
#ifdef MPX_TEST_DDR_PAGE_HOLD
    if (auto* hold = mpxTestDdrPageHold.load())
        hold(2);
#endif
    out = *reinterpret_cast<volatile uint32_t*>(map + off);
    return true;
}

// R1 (review 06:57 D1): the PLX slots' own page, independent of the frame layout. The start-up
// 320x240 layout's doorbell page is 0x3007F000, so PLXH at 0x300FF138 was unreachable until the
// first 480p present. The page goes through the one doorbell-page mapper (review 08:17 (a)): its
// own slot at start-up, the same mapping as the layout's doorbell page on 480p.
namespace {
constexpr uint32_t kPlxPagePhys = r1::kPlxhPhys & ~0xFFFu;
static_assert(r1::kPlxhPhys >= kPlxPagePhys && r1::kPlxoPhys + 8u <= kPlxPagePhys + 4096u,
              "PLXH 0x300FF138..13F and PLXO 0x300FF140..147 share one 4 KiB page");
// F1 (v2.2 p1): PLXL 0x300FF148..14F follows PLXO on the same page, the F1 row's PLX page.
static_assert(r1::kPlxoPhys + 8u <= kPlxPagePhys + f1::kPlxlOff &&
                  kPlxPagePhys + f1::kPlxlOff + 8u <= kPlxPagePhys + 4096u,
              "PLXL 0x300FF148..14F follows PLXO inside the PLX page");
}  // namespace

// Caller holds lockDdrPage() for as long as it uses the word.
volatile uint32_t* FpgaSpi::plxWord(uint32_t phys) {
#ifdef MUT_R1_PLX_LAYOUT_PAGE   // test-only mutant: the pre-D1 path through the layout's mailbox page
    return ddrMailboxWord(phys);
#elif defined(MUT_R1_PLX_OWN_MAP)   // test-only mutant: R1's pre-mapper private PLX page map
    if (phys < kPlxPagePhys || phys + 8u > kPlxPagePhys + 4096u)
        return nullptr;
    static uint8_t* own = nullptr;
    if (!own) {
        const int fd = ::open(mpxDevmemPath(), O_RDWR | O_SYNC | O_CLOEXEC);
        if (fd < 0)
            return nullptr;
        void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(kPlxPagePhys));
        ::close(fd);
        if (p == MAP_FAILED)
            return nullptr;
        own = static_cast<uint8_t*>(p);
    }
    return reinterpret_cast<volatile uint32_t*>(own + (phys - kPlxPagePhys));
#else
    if (phys < kPlxPagePhys || phys + 8u > kPlxPagePhys + 4096u)
        return nullptr;
    uint8_t* map = nullptr;
    if (!ensureDdrMailboxMap(kPlxPagePhys, map) || !map)
        return nullptr;
    return reinterpret_cast<volatile uint32_t*>(map + (phys - kPlxPagePhys));
#endif
}

bool FpgaSpi::waitCoreFlag(bool clearBusy, bool clearPending, int maxUs) {
    // Poll status until flags clear (or timeout).
    const int step = 500;
    int waited = 0;
    while (waited < maxUs) {
        uint8_t raw[16]{};
        {
            SpiExclusive guard(map_);
            if (!guard.safe() || !map_ || !gpiUserMode(map_))
                return false;
            if (!readStatusRaw(raw))
                return false;
        }
        CoreStatus st = parseCoreStatus(raw, negotiatedBuild_.load());
        const bool busyOk = !clearBusy || !st.ddr_busy;
        const bool pendOk = !clearPending || !st.swap_pending;
        if (busyOk && pendOk)
            return true;
        usleep(step);
        waited += step;
    }
    return false;
}

bool FpgaSpi::waitPlxdHeardKick(const BankReleaseStatus& pre, int timeoutUs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs);
    for (;;) {
        BankReleaseStatus cur{};
        if (readBankRelease(cur)) {
            if (cur.swap_pending)
                return true;
            if (frameCounterDelta(cur.frames_done, pre.frames_done) > 0)
                return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        usleep(500);
    }
}

// R15 host-paint: only treat PLXF as kick evidence when the frame-store seq
// actually advances. A live-but-static PLXF word is NOT acceptance — that
// false-green locked doorbell mode while has_frame stayed low (black glass).
bool FpgaSpi::waitPlxfHeardKick(const FrameStoreStatus& pre, int timeoutUs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs);
    for (;;) {
        FrameStoreStatus cur{};
        if (readFrameStoreStatus(cur) && cur.seq != pre.seq)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        usleep(500);
    }
}

bool FpgaSpi::kickDdrDoorbell(int bank) {
    if (bank < 0 || bank > 1)
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* dw = ddrMailboxWord(ddrLayout_.doorbell_phys);
    if (!dw)
        return false;
    if (doorbellSeq_ == 0)
        doorbellSeq_ = 1;
    // Pack: [31:0]=magic, high=[31]=bank, [30:29]=format, [28:0]=sequence.
    // Do not bump seq until PLXD hears this token. Incrementing every kick
    // defeats IGNORE_STALE_DOORBELL_AFTER_RESET fallback (needs same token
    // for STALE_DOORBELL_FALLBACK_POLLS).
    const uint32_t seq = doorbellSeq_ & 0x1FFFFFFFu;
    const uint32_t hi = ddrDoorbellHi(seq, bank, ddrLayout_.format);
    // Publish the complete token before magic. On a cold mailbox this prevents
    // the reader from seeing PLXK paired with a zero/old format; on a warm
    // mailbox the already-valid magic may expose the new token immediately,
    // which is safe because sendDdrFrame fences payload writes before this call.
    dw[1] = hi;
    // L4 DYN_BASE_EN=1: leftover sticky valid+phys0 made the store read a
    // bogus base (grey chevron while ARM wrote Trek at 0x30180000). Always
    // publish the destination bank PA when srcPhys is unset so burst-2
    // valid=1 points at the WC copy, not 0.
    uint32_t dynPhys = doorbellDynPhys_;
    if (dynPhys == 0 && ddrLayout_.phys_base != 0 && ddrLayout_.bank_stride != 0)
        dynPhys = ddrLayout_.phys_base +
                  static_cast<uint32_t>(bank) * ddrLayout_.bank_stride;
    const uint32_t dyn = ddrDoorbellDynWord(dynPhys);
    dw[2] = dyn;
    dw[3] = 0;
    __sync_synchronize();
    dw[0] = kDdrDoorbellMagic;
    __sync_synchronize();
    return true;
}

bool FpgaSpi::readOsdMailbox(uint16_t& osd) {
    if (!ensureDdrMap())
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mb = ddrMailboxWord(ddrStatusMailboxPhys());
    if (!mb)
        return false;
    // The core writes all 8 bytes in one DDR burst, but the ARM sees it as two
    // 32-bit loads, so re-read until the sequence number is stable to be sure we
    // did not catch a publish in flight.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo = mb[0];
        const uint32_t hi = mb[1];
        __sync_synchronize();
        if (lo != kDdrMailboxMagic)
            return false; // core has not published (pre-mailbox RBF, or reset)
        if (mb[1] != hi || mb[0] != lo)
            continue; // caught a publish in flight; re-read
        const uint16_t seq = static_cast<uint16_t>(hi >> 16);
        const double now = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (!mboxInit_) {
            // First sight proves nothing: the magic may be a leftover from a
            // previous core. Wait for seq to actually move.
            mboxInit_ = true;
            mboxSeq_ = seq;
            mboxSeqMs_ = now;
            return false;
        }
        if (seq != mboxSeq_) {
            mboxSeq_ = seq;
            mboxSeqMs_ = now;
            mboxAlive_ = true;
        } else if (now - mboxSeqMs_ > 2000.0) {
            // Heartbeat is milliseconds; two seconds of a frozen counter means
            // nothing is publishing. Hand the caller back to the SPI path.
            mboxAlive_ = false;
        }
        if (!mboxAlive_)
            return false;
        osd = static_cast<uint16_t>(hi & 0xFFFFu);
        return true;
    }
    return false;
}

bool FpgaSpi::readDdrDoorbellStatus(DdrDoorbellStatus& status) {
    status = DdrDoorbellStatus{};
    if (!ensureDdrMap())
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* dw = ddrMailboxWord(ddrLayout_.doorbell_phys);
    if (!dw)
        return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo = dw[0];
        const uint32_t hi = dw[1];
        __sync_synchronize();
        if (dw[0] != lo || dw[1] != hi)
            continue;
        uint32_t seq = 0;
        int bank = 0;
        if (!decodeDdrDoorbell(lo, hi, ddrLayout_.format, seq, bank))
            return false;
        status.seq = seq;
        status.bank = bank;
        status.format = ddrLayout_.format;
        return true;
    }
    return false;
}

bool FpgaSpi::readInputMailbox(PlaybackCommand& command) {
    command = PlaybackCommand::None;
    if (!ensureDdrMap())
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mb = ddrMailboxWord(ddrInputMailboxPhys());
    if (!mb)
        return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo = mb[0];
        const uint32_t hi = mb[1];
        __sync_synchronize();
        const uint32_t verifyLo = mb[0];
        const uint32_t verifyHi = mb[1];
        if (lo != verifyLo || hi != verifyHi)
            continue;
        if (lo != kInputMailboxMagic) {
            inputMboxEdge_.noteNoValidWord();
            return false;
        }
        InputMailboxSample sample;
        if (!decodeInputMailboxWord(static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32),
                                    sample))
            return false;
        if (inputMboxEdge_.accept(sample, command))
            return true;
        return false;
    }
    return false;
}

bool FpgaSpi::coreGateAllowsQuiet() {
    CoreGate& gate = CoreGate::process();
    if (!gate.managed())
        return true;
    if (!gate.open())
        return false;
    const FabricState fabric = readFabricState(map_);
    if (!fabric.live()) {
        if (fabric.known)
            gate.closeNow(fabric.describe());
        return false;
    }
    return true;
}

// MPX_BROWSE_V2 direct readers: the doorbell page through the one mapper (review 08:17 (a));
// caller holds lockDdrPage() for as long as it uses the word.
volatile uint32_t* FpgaSpi::mailboxWordDirect(uint32_t phys) {
    const uint32_t pagePhys = phys & ~0xFFFu;
    if (phys + 8 > pagePhys + 4096u)
        return nullptr;
#ifdef MUT_BROWSE_DIRECT_OWN_MAP   // test-only mutant: each poll thread maps the page itself (pre-mapper)
    static thread_local uint8_t* own = nullptr;
    static thread_local uint32_t ownPage = 0;
    if (!own || ownPage != pagePhys) {
        const int fd = ::open(mpxDevmemPath(), O_RDONLY | O_SYNC | O_CLOEXEC);
        if (fd < 0)
            return nullptr;
        void* p = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(pagePhys));
        ::close(fd);
        if (p == MAP_FAILED)
            return nullptr;
        own = static_cast<uint8_t*>(p);
        ownPage = pagePhys;
    }
    return reinterpret_cast<volatile uint32_t*>(own + (phys - pagePhys));
#else
    uint8_t* map = nullptr;
    if (!ensureDdrMailboxMap(pagePhys, map) || !map)
        return nullptr;
    return reinterpret_cast<volatile uint32_t*>(map + (phys - pagePhys));
#endif
}

bool FpgaSpi::readInputMailboxDirect(PlaybackCommand& command, uint8_t maxCommand) {
    command = PlaybackCommand::None;
    if (!coreGateAllowsQuiet())
        return false;
    const uint32_t epoch = mailboxEpoch_.load(std::memory_order_acquire);
    if (epoch != inputPageEpoch_) { // where readInputMailbox's detector resets
        inputPageEpoch_ = epoch;
        inputEdgeDirect_.reset();
    }
    const uint32_t phys = mailboxDoorbellPhys_.load(std::memory_order_acquire) +
                          kInputMailboxOff;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mb = mailboxWordDirect(phys);
    if (!mb)
        return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo = mb[0];
        const uint32_t hi = mb[1];
        __sync_synchronize();
        const uint32_t verifyLo = mb[0];
        const uint32_t verifyHi = mb[1];
        if (lo != verifyLo || hi != verifyHi)
            continue;
        if (lo != kInputMailboxMagic) {
            inputEdgeDirect_.noteNoValidWord();
            return false;
        }
        InputMailboxSample sample;
        if (!decodeInputMailboxWord(static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32),
                                    sample, maxCommand))
            return false;
        return inputEdgeDirect_.accept(sample, command);
    }
    return false;
}

bool FpgaSpi::readOwenbRingCtrlWord(unsigned wordIndex, uint32_t& loOut) {
    if (!coreGateAllowsQuiet())
        return false;
    static constexpr uint32_t kCtrlBase = 0x31400000u;
    const uint32_t phys = kCtrlBase + wordIndex * 8u;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* w = mailboxWordDirect(phys);
    if (!w)
        return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo = w[0];
        const uint32_t hi = w[1];
        __sync_synchronize();
        if (w[0] != lo || w[1] != hi)
            continue;
        loOut = lo;
        return true;
    }
    return false;
}

bool FpgaSpi::readOsdMailboxDirect(uint16_t& osd) {
    if (!coreGateAllowsQuiet())
        return false;
    const uint32_t epoch = mailboxEpoch_.load(std::memory_order_acquire);
    if (epoch != osdPageEpoch_) { // where readOsdMailbox's heartbeat state resets
        osdPageEpoch_ = epoch;
        osdDirectInit_ = false;
        osdDirectAlive_ = false;
    }
    const uint32_t phys = mailboxDoorbellPhys_.load(std::memory_order_acquire) +
                          kDdrStatusMailboxOff;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mb = mailboxWordDirect(phys);
    if (!mb)
        return false;
    // As readOsdMailbox: a stable 8-byte read, live only while seq keeps moving.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo = mb[0];
        const uint32_t hi = mb[1];
        __sync_synchronize();
        if (lo != kDdrMailboxMagic)
            return false;
        if (mb[1] != hi || mb[0] != lo)
            continue;
        const uint16_t seq = static_cast<uint16_t>(hi >> 16);
        const double now = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (!osdDirectInit_) {
            osdDirectInit_ = true;
            osdDirectSeq_ = seq;
            osdDirectSeqMs_ = now;
            return false;
        }
        if (seq != osdDirectSeq_) {
            osdDirectSeq_ = seq;
            osdDirectSeqMs_ = now;
            osdDirectAlive_ = true;
        } else if (now - osdDirectSeqMs_ > 2000.0) {
            osdDirectAlive_ = false;
        }
        if (!osdDirectAlive_)
            return false;
        osd = static_cast<uint16_t>(hi & 0xFFFFu);
        return true;
    }
    return false;
}

bool FpgaSpi::kickDdrSpi(int bank, bool first_verify, bool& saw_busy, bool& saw_kick,
                         bool& saw_frame) {
    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    if (!gpiUserMode(map_)) {
        err_ = "FPGA not in user mode";
        return false;
    }
    err_.clear();
    // One UIO_GET_STATUS per kick, inside this SPI window. A failed read leaves
    // the shadow, which holds the previous kick's live copy (writeStatusWordRaw
    // clears err_, so it does not fail the kick).
    uint8_t live[16]{};
    const bool have_live = readStatusRaw(live);
    uint8_t word[16]{};
    composeKickBase(word, status_, have_live ? live : nullptr, first_verify);
    word[0] = static_cast<uint8_t>(word[0] & ~0x01);
    if (bank)
        word[1] = static_cast<uint8_t>(word[1] | 0x20);
    else
        word[1] = static_cast<uint8_t>(word[1] & ~0x20);
    word[1] = static_cast<uint8_t>(word[1] & ~0x10);
    word[1] = static_cast<uint8_t>(word[1] & ~0x02);
    if (!writeStatusWordRaw(word))
        return false;
    word[1] = static_cast<uint8_t>(word[1] | 0x10);
    if (!writeStatusWordRaw(word))
        return false;
    if (first_verify) {
        for (int i = 0; i < 40; ++i) {
            uint8_t raw[16]{};
            if (!readStatusRaw(raw))
                break;
            if (raw[1] & 0x10)
                saw_kick = true;
            CoreStatus st = parseCoreStatus(raw, negotiatedBuild_.load());
            if (st.ddr_busy)
                saw_busy = true;
            if (st.has_frame)
                saw_frame = true;
            if (saw_busy || (saw_kick && saw_frame && i >= 2))
                break;
            usleep(200);
        }
    }
    word[1] = static_cast<uint8_t>(word[1] & ~0x10);
    if (!writeStatusWordRaw(word))
        return false;
    if (first_verify) {
        uint8_t raw[16]{};
        if (readStatusRaw(raw)) {
            if (raw[1] & 0x10)
                saw_kick = true;
            CoreStatus st = parseCoreStatus(raw, negotiatedBuild_.load());
            if (st.has_frame)
                saw_frame = true;
            if (st.ddr_busy)
                saw_busy = true;
        }
    }
    // status_ already committed by successful writeStatusWordRaw (rd-duck).
    return true;
}

void FpgaSpi::gpoWrite(uint32_t v) {
    gpo_copy_ = v;
    volatile uint32_t* gpo = map_ + ((kMgrBase - kMapBase + 0x10) >> 2);
    *gpo = v;
}

uint32_t FpgaSpi::gpoRead() const {
    // Read the live register, not a shadow. Main keeps a static `gpo_copy` and
    // never re-reads hardware, so a shadow of our own would drift from Main's
    // every time it drove the bus, and we would hand back bits it never set.
    // Inside a SpiExclusive window we are the only writer, so this is stable.
    if (map_)
        return *gpoReg(map_);
    return gpo_copy_;
}

int FpgaSpi::gpiRead() const {
    volatile uint32_t* gpi = map_ + ((kMgrBase - kMapBase + 0x14) >> 2);
    return static_cast<int>(*gpi);
}

void FpgaSpi::spiEn(uint32_t mask, int en) {
    uint32_t gpo = gpoRead() | 0x80000000u;
    gpoWrite(en ? (gpo | mask) : (gpo & ~mask));
}

uint16_t FpgaSpi::spiWord(uint16_t word) {
    // Caller must hold SpiExclusive (spiMutex). Touch err_ without re-locking.
    // Production path = spiWordTxn (spi_ack_wait.hpp, unit-tested):
    // yield + poll-budget abort; deassert SSPI_STROBE on set-phase fail.
    // NO short wall-clock abort (L30: 2 ms cap broke HDMI).
    const uint32_t gpo_cur = gpoRead();
    const SpiWordTxnResult r = spiWordTxn(
        word, gpo_cur, SSPI_STROBE,
        [this](uint32_t v) { gpoWrite(v); },
        [this]() { return gpiRead(); },
        spiAckDefaultSleepUs);
    lastAckSet_ = r.set_stats;
    lastAckClr_ = r.clr_stats;
    if (!r.ok) {
        if (r.err)
            err_ = r.err;
        else if (err_.empty())
            err_ = "SPI ACK failed";
        return 0;
    }
    return r.data;
}

void FpgaSpi::spiByte(uint8_t b) { spiWord(b); }

void FpgaSpi::spiWriteBytes(const uint8_t* p, size_t n) {
    // Fast path (no ACK wait) — matches Main_MiSTer fpga_spi_fast for bulk data.
    uint32_t gpoH = gpoRead() & ~(0xFFFFu | SSPI_STROBE);
    while (n--) {
        uint32_t gpo = gpoH | *p++;
        gpoWrite(gpo);
        gpoWrite(gpo | SSPI_STROBE);
        gpoWrite(gpo);
    }
}

void FpgaSpi::enableFpga(int on) { spiEn(SSPI_FPGA_EN, on); }
void FpgaSpi::enableIo(int on) { spiEn(SSPI_IO_EN, on); }

void FpgaSpi::setIndex(uint8_t index) {
    enableFpga(1);
    spiByte(FIO_FILE_INDEX);
    spiByte(index);
    enableFpga(0);
}

void FpgaSpi::setDownload(int enable) {
    enableFpga(1);
    spiByte(FIO_FILE_TX);
    spiByte(enable ? 0xff : 0x00);
    enableFpga(0);
}

// SPI body for UIO_SET_STATUS2. Caller must hold SpiExclusive and user mode.
bool FpgaSpi::writeStatusWordRaw(const uint8_t word[16]) {
    if (!word) {
        err_ = "writeStatusWordRaw: null";
        return false;
    }
    err_.clear();
    int failed_at = -1;
    const char* terr = nullptr;
    const bool ok = statusWriteWordTxnEnabled(
        word, status_,
        [this](uint16_t w) -> SpiTxnPhaseResult {
            (void)spiWord(w);
            if (!err_.empty())
                return SpiTxnPhaseResult{false, err_.c_str()};
            return SpiTxnPhaseResult{true, nullptr};
        },
        [this](int on) { enableIo(on); }, failed_at, terr);
    if (!ok) {
        if (terr && err_.empty())
            err_ = terr;
        else if (err_.empty())
            err_ = "writeStatusWordRaw: incomplete";
        lastStatusWriteFailedAt_ = failed_at;
        return false;
    }
    lastStatusWriteFailedAt_ = -1;
    return true;
}

// SPI body for UIO_GET_STATUS. Caller must hold SpiExclusive and user mode.
bool FpgaSpi::readStatusRaw(uint8_t out[16]) {
    return readStatusRawEx(out, nullptr);
}

bool FpgaSpi::readStatusRawEx(uint8_t out[16], uint32_t* opword) {
    if (!out)
        return false;
    err_.clear();
    enableIo(1);
    const uint16_t op = spiWord(UIO_GET_STATUS);
    if (opword)
        *opword = op;
    if (!err_.empty()) {
        enableIo(0);
        return false;
    }
    for (int i = 0; i < 16; i += 2) {
        uint16_t w = spiWord(0);
        if (!err_.empty()) {
            enableIo(0);
            return false;
        }
        out[i] = static_cast<uint8_t>(w & 0xFF);
        out[i + 1] = static_cast<uint8_t>((w >> 8) & 0xFF);
    }
    enableIo(0);
    return true;
}

const char* FpgaSpi::classifyWedge(const WedgePage& p) {
    if (!p.valid)
        return "VOID(tag|stale)";
    if (p.arb && p.owed > 0)
        return "below-arbiter";
    if (!p.arb && p.owed == 0 && p.mux3)
        return "arb-mux3";
    if (!p.mux3 && p.amux)
        return "mux3-amux";
    if (p.dbr && !p.amux && !p.mux3 && !p.arb)
        return "amux-dbr";
    if (p.portbusy && p.owed == 0)
        return "port-busy";
    if (!p.dbr && !p.amux && !p.mux3 && !p.arb && !p.portbusy && !p.decAlive && !p.pll)
        return "clock-lost";
    if (!p.dbr && !p.amux && !p.mux3 && !p.arb && !p.portbusy && p.decAlive && p.ddrAlive)
        return "not-bus-wedge";
    return "mixed";
}

void FpgaSpi::parseWedgePage(WedgePage& p) {
    const uint8_t b12 = p.raw[12];
    const uint8_t b13 = p.raw[13];
    const uint8_t b14 = p.raw[14];
    const uint8_t b15 = p.raw[15];
    p.dbr = (b15 & 0x80) != 0;
    p.amux = (b15 & 0x40) != 0;
    p.mux3 = (b15 & 0x20) != 0;
    p.arb = (b15 & 0x10) != 0;
    p.portbusy = (b15 & 0x08) != 0;
    p.guard = (b15 & 0x01) != 0;
    p.valid = ((b14 >> 4) & 0x07) == 0x05;
    p.pll = (b14 & 0x08) != 0;
    p.rdec = (b14 & 0x04) != 0;
    p.decAlive = (b14 & 0x02) != 0;
    p.ddrAlive = (b14 & 0x01) != 0;
    p.owed = (b13 >> 4) & 0x0f;
    p.busState = (b13 >> 2) & 0x03;
    p.killed = (b13 & 0x02) != 0;
    p.orphan = static_cast<uint8_t>(b13 & 0x01);
    p.gwin = (b12 >> 3) & 0x07;
    p.resets = b12 & 0x07;
    p.classification = classifyWedge(p);
}

void FpgaSpi::logWedgePage(const char* site, int rd, const WedgePage& p) {
    std::fprintf(stderr,
                 "misterplexd: WEDGE site=%s build=0x%08X rd=%d/2 stflg=%u "
                 "dbr=%d amux=%d mux3=%d arb=%d portbusy=%d guard=%d pll=%d rdec=%d "
                 "dec_alive=%d ddr_alive=%d owed=%u bus_state=%u killed=%d orphan=%u "
                 "gwin=%u resets=%u class=%s\n",
                 site ? site : "?", CoreGate::process().buildId(), rd,
                 static_cast<unsigned>(p.stflg), p.dbr ? 1 : 0, p.amux ? 1 : 0,
                 p.mux3 ? 1 : 0, p.arb ? 1 : 0, p.portbusy ? 1 : 0, p.guard ? 1 : 0,
                 p.pll ? 1 : 0, p.rdec ? 1 : 0, p.decAlive ? 1 : 0, p.ddrAlive ? 1 : 0,
                 static_cast<unsigned>(p.owed), static_cast<unsigned>(p.busState),
                 p.killed ? 1 : 0, static_cast<unsigned>(p.orphan),
                 static_cast<unsigned>(p.gwin), static_cast<unsigned>(p.resets),
                 p.classification ? p.classification : "VOID(tag|stale)");
}

bool FpgaSpi::wedgePageArmed() const {
    const FpgaBuildInfo* row = negotiatedBuild_.load(std::memory_order_acquire);
    if (!row) {
#ifndef MUT_WEDGE_NO_REARM
        // Cold start (no play has negotiated yet) and the tick after
        // onCoreReturned (that call clears the pointer): the open gate's CAPS
        // build is the row. onCoreReturned itself still stores nullptr.
        CoreGate& gate = CoreGate::process();
        if (gate.managed() && gate.open())
            row = recognisedFpgaBuild(gate.buildId());
#endif
    }
    return row && row->wedgePage;
}

bool FpgaSpi::readWedgePage(WedgePage& out) {
    out = {};
    if (!wedgePageArmed()) {
        std::fprintf(stderr, "misterplexd: WEDGE skipped row build=0x%08X\n",
                     CoreGate::process().buildId());
        return false;
    }
#ifdef MPX_TEST_WEDGE_READ_SEAM
    ++mpxTestWedgeReadAttempts();  // test seam: one count per SPI read attempt that passed the row gate
    mpxTestNoteWedgeRead();
#endif
    if (!ok()) {
        setErr("readWedgePage: not open");
        return false;
    }

    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    if (!gpiUserMode(map_)) {
        err_ = "FPGA not in user mode";
        return false;
    }

    uint8_t base[16]{};
    uint32_t op0 = 0;
    if (!readStatusRawEx(base, &op0))
        return false;
    const uint16_t st0 = static_cast<uint16_t>(op0 & 0x0fff);
    if (!setStatusBit(119, 1)) {
        // A failed ACK may still have shifted the word out: clear the page bit
        // so a WEDGE page never stays selected (review 1010 NIT, fpga_spi.cpp:2024).
        const std::string setErr = err_;
        (void)setStatusBit(119, 0);
        err_ = setErr;
        return false;
    }

    bool ok = false;
    int fresh = 0;
    uint16_t last = st0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        WedgePage sample;
        if (!readStatusRawEx(sample.raw, &sample.opword))
            break;
        sample.stflg = static_cast<uint16_t>(sample.opword & 0x0fff);
        if (sample.stflg != last) {
            last = sample.stflg;
            ++fresh;
        }
        if (fresh >= 2) {
            parseWedgePage(sample);
            out = sample;
            ok = out.valid;
            break;
        }
    } while (std::chrono::steady_clock::now() < deadline);

    if (!setStatusBit(119, 0))
        return false;
    if (!ok)
        out.classification = "VOID(tag|stale)";
    return ok;
}

void FpgaSpi::noteWedgeTrigger(const char* site) {
    if (!wedgePageArmed()) {
#ifndef MUT_WEDGE_NO_REARM
        // H-c runs while the gate is closed, before CAPS has a row. Latch and
        // let the next open tick service it. A known non-wedge row still skips.
        CoreGate& gate = CoreGate::process();
        if (gate.managed() && !gate.open() && negotiatedBuild_.load() == nullptr) {
            std::fprintf(stderr, "misterplexd: WEDGE trigger site=%s reason=epoch-ack-timeout\n",
                         site ? site : "?");
            wedgePending_.store(true, std::memory_order_release);
            return;
        }
#endif
        std::fprintf(stderr, "misterplexd: WEDGE skipped row site=%s build=0x%08X\n",
                     site ? site : "?", CoreGate::process().buildId());
        return;
    }
    std::fprintf(stderr, "misterplexd: WEDGE trigger site=%s reason=epoch-ack-timeout\n",
                 site ? site : "?");
    wedgePending_.store(true, std::memory_order_release);
}

void FpgaSpi::serviceWedgeRead(const char* site, bool requirePending) {
    if (requirePending && !wedgePending_.load(std::memory_order_acquire))
        return;
    if (!wedgePageArmed()) {
        std::fprintf(stderr, "misterplexd: WEDGE skipped row site=%s build=0x%08X\n",
                     site ? site : "?", CoreGate::process().buildId());
#ifndef MUT_WEDGE_STALE_LATCH
        // Closed gate: no row yet, keep the latch for the open tick.
        // Open gate on a non-wedge row: drop it, or the next 0x48FF65F2 open
        // would run a pair from this stale latch.
        if (CoreGate::process().open())
            wedgePending_.store(false, std::memory_order_release);
#endif
        return;
    }
    bool expected = false;
    if (!wedgeBusy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::fprintf(stderr, "misterplexd: WEDGE deferred busy site=%s\n", site ? site : "?");
        return;
    }
    struct BusyClear {
        std::atomic<bool>& b;
        ~BusyClear() { b.store(false, std::memory_order_release); }
    } busyClear{wedgeBusy_};

    {
        std::unique_lock<std::recursive_mutex> g(spiMutex());
        const int depth = spiDepth();
        if (depth > 0) {
            std::fprintf(stderr, "misterplexd: WEDGE deferred site=%s depth=%d\n",
                         site ? site : "?", depth);
            return;
        }
    }
    // W-NOBMTX: never read (or sleep 1 s) under bitstreamMutex. It is recursive, so this thread's
    // own try_lock() succeeds even when it already holds it; probe from a helper thread instead:
    // that try_lock fails while any thread, this one included, holds the mutex.
    bool bitstreamFree = false;
    try {
        std::thread probe([&bitstreamFree] {
            if (bitstreamMutex().try_lock()) {
                bitstreamFree = true;
                bitstreamMutex().unlock();
            }
        });
        probe.join();
    } catch (const std::system_error&) {
        bitstreamFree = false;  // could not probe: defer rather than risk the sleep under it
    }
    if (!bitstreamFree) {
        std::fprintf(stderr, "misterplexd: WEDGE deferred bitstream-busy site=%s\n",
                     site ? site : "?");
        return;
    }

    WedgePage first;
    (void)readWedgePage(first);
    logWedgePage(site, 1, first);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    WedgePage second;
    (void)readWedgePage(second);
    logWedgePage(site, 2, second);
    wedgePending_.store(false, std::memory_order_release);
}

// UIO_GET_STRING: the same read Main_MiSTer does to build the OSD. Reading it back
// from the live core is the only way to prove the deployed RBF carries the CONF_STR
// we think it does — the string lives in the bitstream, so nothing on disk shows it.
bool FpgaSpi::getConfigString(std::string& out) {
    if (!ok()) {
        setErr("getConfigString: not open");
        return false;
    }
    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    if (!gpiUserMode(map_)) {
        err_ = "FPGA not in user mode";
        return false;
    }
    err_.clear();
    out.clear();
    enableIo(1);
    (void)spiWord(UIO_GET_STRING);
    // The core streams one character per word, NUL-terminated. Cap the read so a
    // wedged core cannot spin here forever.
    for (int i = 0; i < 4096; ++i) {
        const uint8_t c = static_cast<uint8_t>(spiWord(0) & 0xFF);
        if (c == 0)
            break;
        out.push_back(static_cast<char>(c));
    }
    enableIo(0);
    return !out.empty();
}

bool FpgaSpi::setStatusWord(const uint8_t word[16]) {
    if (!ok() || !word) {
        setErr("setStatusWord: bad args");
        return false;
    }
    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    if (!gpiUserMode(map_)) {
        err_ = "FPGA not in user mode";
        return false;
    }
    err_.clear();
    const bool wrote = writeStatusWordRaw(word);
    if (wrote && (word[0] & 0x01u))
        strictDdrRelease_.reset();
    return err_.empty();
}

void FpgaSpi::composeStatusBit(uint8_t word[16], const uint8_t shadow[16], const uint8_t* live,
                               int bit, int value) {
    const int byte = bit / 8;
    const int b = bit % 8;
    std::memcpy(word, shadow, 16);
    if (live) {
        // Live RMW of OSD / DDR kick low word (status_in v2 [15:0]) so rising-edge
        // kicks on bit 12 see a true 0→1 on the FPGA (private shadow alone is stale
        // across processes and after status_set echoes).
        if (bit < 16) {
            word[0] = live[0];
            word[1] = live[1];
            if (bit != 0)
                word[0] = static_cast<uint8_t>(word[0] & ~0x01);
        }
        // Layout v3 OSD bytes 12/15 (H/V position, field order, 16:9, ...), so a
        // pulse such as the play-start audio flush (bit 16) never resets them.
        status_telemetry::overlayLiveOsdBytes(word, live);
    }
    if (value)
        word[byte] = static_cast<uint8_t>(word[byte] | (1u << b));
    else
        word[byte] = static_cast<uint8_t>(word[byte] & ~(1u << b));
}

bool FpgaSpi::setStatusBit(int bit, int value) {
    if (!ok() || bit < 0 || bit > 127) {
        setErr("setStatusBit: bad args");
        return false;
    }
    // One SPI window (SpiExclusive nests) for the live read and the write: a
    // read skipped because Main was busy must not be followed by a write of
    // the bare shadow, which would zero the OSD bytes 12/15.
    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    uint8_t live[16]{};
    const bool have_live = getCoreStatus(live);
    uint8_t word[16];
    composeStatusBit(word, status_, have_live ? live : nullptr, bit, value);
    return setStatusWord(word);
}

void FpgaSpi::composeKickBase(uint8_t word[16], const uint8_t shadow[16], const uint8_t* live,
                              bool first_verify) {
    std::memcpy(word, shadow, 16);
    if (!live)
        return;
    if (first_verify) {
        word[0] = live[0];
        word[1] = live[1];
    }
    // Every kick, not only the first: the shadow holds the previous kick's
    // copy and would undo an OSD change the user made since.
    status_telemetry::overlayLiveOsdBytes(word, live);
}

// Status block heartbeat (ported from 1cb004e5; OSD-MENU-SPEC-1002 section 9).
void FpgaSpi::composeStatusHeartbeat(uint8_t word[16], const uint8_t shadow[16],
                                     const uint8_t* live, uint8_t h, uint32_t daemon_id) {
    namespace hb = status_heartbeat;
    bool want_pulse0 = false;
    composeStatusBits(word, shadow, live, nullptr, 0, want_pulse0);
    const uint8_t running = (live ? live[hb::kByte] : shadow[hb::kByte]) & hb::kRunning;
    word[hb::kByte] = static_cast<uint8_t>(running | (h & hb::kDaemonBits));
    // Pairing idle: H[2:1]=3 and LE daemon BUILD_ID in bytes 8-11 (status[95:64]).
    // Play / daemon-down: copy live telem so a stale pairing id cannot stick in the
    // shadow (480i 0x4801F40A would parse those bytes as ddr_busy/swap_pending).
    const bool pairing = ((h & hb::kServerMask) >> hb::kServerShift) ==
                         static_cast<uint8_t>(hb::Server::Pairing);
    if (pairing) {
        word[8]  = static_cast<uint8_t>(daemon_id);
        word[9]  = static_cast<uint8_t>(daemon_id >> 8);
        word[10] = static_cast<uint8_t>(daemon_id >> 16);
        word[11] = static_cast<uint8_t>(daemon_id >> 24);
    } else if (live) {
        word[8]  = live[8];
        word[9]  = live[9];
        word[10] = live[10];
        word[11] = live[11];
    }
}

bool FpgaSpi::statusBlockActive() const {
    const CoreGate& gate = CoreGate::process();
    uint32_t id = 0;
    if (gate.open())
        id = gate.buildId();
    else if (gate.idleErrorPaintAllowed())
        id = gate.lastCapsId();
    else
        return false;
    return statusBlockRow(recognisedFpgaBuild(id), statusBlockForce_.load());
}

bool FpgaSpi::writeStatusHeartbeat(uint8_t h) {
    // Gate closed, pairing-error idle without a statusBlock CAPS row, or no status
    // block on the open row: no SPI at all. StatusHeartbeatGuard lets SpiExclusive
    // RMW H on a Rejected Plex core; doorbell/overlay stay refused.
    StatusHeartbeatGuard hbScope;
    if (!statusBlockActive()) {
        const CoreGate& g = CoreGate::process();
        setErr((g.open() || g.idleErrorPaintAllowed()) ? "status block: not on this build row"
                                                       : "status block: core gate closed");
        return false;
    }
    if (!ok()) {
        setErr("status block: SPI not open");
        return false;
    }
    // One SPI window (SpiExclusive nests) for the live read and the write, as setStatusBit.
    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    uint8_t live[16]{};
    if (!getCoreStatus(live)) {
        // Never write a stale word: the next 2 s tick tries again.
        setErr("status block: live status unreadable — skipped");
        return false;
    }
    uint8_t word[16];
    namespace hb = status_heartbeat;
    uint32_t daemon_id = 0;
    if (((h & hb::kServerMask) >> hb::kServerShift) == static_cast<uint8_t>(hb::Server::Pairing)) {
        const CoreGate& g = CoreGate::process();
        const uint32_t caps = g.lastCapsId();
        // Allowlisted CAPS is the live core. Do not publish the previous open id.
        daemon_id = recognisedFpgaBuild(caps)
                        ? caps
                        : (g.lastOpenId() ? g.lastOpenId()
                                          : daemonIdForPairedRaster(g.pairedRaster()));
    }
    composeStatusHeartbeat(word, status_, live, h, daemon_id);
    return setStatusWord(word);
}

bool FpgaSpi::composeStatusBits(uint8_t word[16], const uint8_t shadow[16], const uint8_t* live,
                                const int* bit_val_pairs, int n_pairs, bool& want_pulse0) {
    // RMW base: private shadow (same process), then overlay live OSD echo from
    // status_in v2 [15:0] when non-zero so successive set_status CLI processes
    // accumulate (shadow is not shared across processes).
    std::memcpy(word, shadow, 16);
    if (live) {
        const uint16_t live_lo =
            static_cast<uint16_t>(live[0] | (static_cast<uint16_t>(live[1]) << 8));
        if (live_lo != 0) {
            word[0] = live[0];
            word[1] = live[1];
        }
        // Aspect ratio O[122:121] lives in byte 15 bits [2:1]
        if (live[15] & 0x06)
            word[15] = static_cast<uint8_t>((word[15] & ~0x06) | (live[15] & 0x06));
        // Layout v3 OSD bytes 12/15; the pairs below still set their own bits.
        status_telemetry::overlayLiveOsdBytes(word, live);
    }

    // CRITICAL: never leave Reset (bit 0) or flush pulses stuck high.
    // Holding status[0] keeps the core in reset → status_set stops → Main never
    // adopts a clear, and UIO_GET_STATUS freezes. Pulse bits must be edge-only.
    want_pulse0 = false;
    for (int i = 0; i < n_pairs; ++i) {
        const int bit = bit_val_pairs[i * 2];
        const int value = bit_val_pairs[i * 2 + 1] ? 1 : 0;
        if (bit < 0 || bit > 127)
            return false;
        if (bit == 0 && value)
            want_pulse0 = true;
        const int byte = bit / 8;
        const int b = bit % 8;
        if (value)
            word[byte] = static_cast<uint8_t>(word[byte] | (1u << b));
        else
            word[byte] = static_cast<uint8_t>(word[byte] & ~(1u << b));
    }
    // Clear sticky pulses unless this call intentionally raises bit 0.
    if (!want_pulse0)
        word[0] = static_cast<uint8_t>(word[0] & ~0x01);
    // Always clear flush sticky bits unless the pairs explicitly set them.
    bool want10 = false, want11 = false;
    for (int i = 0; i < n_pairs; ++i) {
        if (bit_val_pairs[i * 2] == 10 && bit_val_pairs[i * 2 + 1])
            want10 = true;
        if (bit_val_pairs[i * 2] == 11 && bit_val_pairs[i * 2 + 1])
            want11 = true;
    }
    if (!want10)
        word[1] = static_cast<uint8_t>(word[1] & ~0x04);
    if (!want11)
        word[1] = static_cast<uint8_t>(word[1] & ~0x08);
    return true;
}

bool FpgaSpi::setStatusBits(const int* bit_val_pairs, int n_pairs) {
    if (!ok() || !bit_val_pairs || n_pairs <= 0) {
        setErr("setStatusBits: bad args");
        return false;
    }
    uint8_t word[16];
    bool want_pulse0 = false;
    {
        // One SPI window for the live read and the first write (see
        // setStatusBit). The sleeps stay outside it so Main is not held off.
        SpiExclusive guard(map_);
        if (!guard.safe()) {
            err_ = "Main busy on SPI — skipped";
            return false;
        }
        uint8_t live[16]{};
        const bool have_live = getCoreStatus(live);
        if (!composeStatusBits(word, status_, have_live ? live : nullptr, bit_val_pairs, n_pairs,
                               want_pulse0)) {
            setErr("setStatusBits: bit out of range");
            return false;
        }
        if (!setStatusWord(word))
            return false;
    }
    // Give FPGA status_set a chance to echo; Main check_status_change adopts it.
    usleep(30000);
    // If we raised bit 0 for reset, immediately clear so the core can run.
    if (want_pulse0) {
        usleep(5000);
        word[0] = static_cast<uint8_t>(word[0] & ~0x01);
        if (!setStatusWord(word))
            return false;
        usleep(20000);
    }
    return true;
}

bool FpgaSpi::sendFileTx(const uint8_t* data, size_t len, uint8_t index) {
    if (index == 1) {
        setErr("non-YUV frame send refused: SPI F1 RGB frame path is disabled; use DDR "
               "YUV420p (sendYuv420pFrameDdr / push_frame --ddr --yuv420p)");
        return false;
    }
    if (!ok() || !data || !len) {
        setErr("sendFileTx: not open or empty");
        return false;
    }
    // D52-LOG (lab, MPX_D52_LOG=1): every index-4 (PLXA) download, whatever the caller, before the transfer.
    if (index == kSourceAspectIoctlIndex)
        if (d52::Logger* d = d52::active())
            d->plxaDownload(data, len);
    auto t0 = std::chrono::steady_clock::now();
    // Mutex + flock + Main pause (no system()/fork — thread-safe under STREAM load).
    SpiExclusive guard(map_);
    err_.clear(); // drop stale DDR probe text so SPI callers see real SPI errors
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    if (!gpiUserMode(map_)) {
        err_ = "FPGA not in user mode";
        return false;
    }

    setIndex(index);
    setDownload(1);

    // Chunk data with FIO_FILE_TX_DAT sessions.
    // Lab measure @320×240 (153600 B): 8 KiB→~220 ms, 32 KiB→~194 ms, 128 KiB→~196 ms.
    // SPI is the ceiling (~0.8 MB/s); DDR3 bulk (3.1b) needed for real-time F1.
    const size_t chunk = 32768;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > chunk)
            n = chunk;
        enableFpga(1);
        spiByte(FIO_FILE_TX_DAT);
        spiWriteBytes(data + off, n);
        enableFpga(0);
        off += n;
        if (!err_.empty()) {
            setDownload(0);
            return false;
        }
    }

    setDownload(0);
    auto t1 = std::chrono::steady_clock::now();
    lastPushMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    err_.clear();
    return true;
}

bool FpgaSpi::sendRgb24Frame(const uint8_t* rgb, int w, int h, uint8_t index) {
    if (!rgb || w <= 0 || h <= 0) {
        setErr("bad rgb frame");
        return false;
    }
    std::vector<uint8_t> packed(static_cast<size_t>(w) * static_cast<size_t>(h) * 2);
    pixel::rgb24ToRgb565Le(rgb, packed.data(), static_cast<size_t>(w) * static_cast<size_t>(h));
    return sendFileTx(packed.data(), packed.size(), index);
}

bool FpgaSpi::sendRgb565Bytes(const uint8_t* rgb565le, size_t len, uint8_t index) {
    if (!rgb565le || !len || (len & 1)) {
        setErr("bad rgb565 bytes");
        return false;
    }
    return sendFileTx(rgb565le, len, index);
}

bool FpgaSpi::sendRgb565Frame(const uint16_t* rgb, int w, int h, uint8_t index) {
    if (!rgb || w <= 0 || h <= 0) {
        setErr("bad rgb565 frame");
        return false;
    }
    const size_t npx = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> packed(npx * 2);
    for (size_t i = 0; i < npx; ++i) {
        const uint16_t p = rgb[i];
        packed[i * 2 + 0] = static_cast<uint8_t>(p & 0xFF);
        packed[i * 2 + 1] = static_cast<uint8_t>(p >> 8);
    }
    return sendFileTx(packed.data(), packed.size(), index);
}

bool FpgaSpi::sendDdrFrame(const uint8_t* payload, size_t len, int bank,
                           DdrBankWritePolicy policy) {
    if (!payload || len != ddrLayout_.frame_bytes) {
        setErr("sendDdrFrame: frame size does not match DDR geometry");
        return false;
    }
    if (bank < 0 || bank > 1) {
        setErr("sendDdrFrame: bank must be 0 or 1");
        return false;
    }
    if (ddrKickMode_ < 0) {
        // Allow re-probe after a cooldown so transient FPGA stalls (e.g. core
        // reload, timing recovery) don't permanently kill the frame path.
        constexpr double kReprobeIntervalMs = 5000.0;
        const double nowMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        if (ddrKickFailMs_ >= 0.0 && (nowMs - ddrKickFailMs_) < kReprobeIntervalMs) {
            setErr("sendDdrFrame: DDR path previously unavailable (re-probe in " +
                   std::to_string(static_cast<int>(kReprobeIntervalMs - (nowMs - ddrKickFailMs_))) +
                   " ms)");
            return false;
        }
        ddrKickMode_ = 0;
        strictDdrRelease_.reset();
    }
    if (!ok() && !open())
        return false;
    if (!ensureDdrMap())
        return false;

    DdrTiming timing{};
    auto t0 = std::chrono::steady_clock::now();

    // --- Bank selection via PLXD bank-release ACK ---
    // If the FPGA publishes a valid PLXD mailbox, use it to determine which
    // bank is free instead of relying on fixed delays. Best-effort callers keep
    // the legacy absent/pending fallbacks; strict true480 callers fail closed.
    //
    // PLXD semantics (w-a3 b187df5):
    //   free_bank_mask != 0 → at least one bank is safe to write
    //   free_bank_mask == 0 → swap pending, both banks in use; poll or timeout
    auto tPrep0 = std::chrono::steady_clock::now();
    bool plxdUsed = false;
    bool strictWriteAuthorized = false;
    BankReleaseStatus strictReleaseSample{};
    {
        BankReleaseStatus brs;
        int plxdIters = 0;
        bool sawPlxd = false;
        if (policy == DdrBankWritePolicy::RequireReleased) {
            // True-480 product pipeline: never overwrite a bank while a prior
            // frame is pending. After the first write, both anyFree and a
            // changed/wrapped frames_done are required; absence, stale data, or
            // timeout fails closed before the payload copy.
            constexpr int kPlxdWaitMaxUs = 50000;
            const auto deadline =
                tPrep0 + std::chrono::microseconds(kPlxdWaitMaxUs);
            for (;;) {
                if (readBankRelease(brs)) {
                    sawPlxd = true;
                    const DdrBankWriteDecision decision =
                        decideDdrBankWrite(brs, policy, strictDdrRelease_);
                    if (decision.ready) {
                        bank = decision.bank;
                        strictWriteAuthorized = true;
                        strictReleaseSample = brs;
                        plxdUsed = true;
                        break;
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline)
                    break;
                usleep(1000);
                ++plxdIters;
            }
            auto tPlxd1 = std::chrono::steady_clock::now();
            timing.plxa_poll_us = elapsedUs(tPrep0, tPlxd1);
            timing.plxa_poll_iters = plxdIters;
            timing.plxa_used = plxdUsed;
            if (!plxdUsed) {
                timing.prep_wait_us = timing.plxa_poll_us;
                timing.total_us = timing.plxa_poll_us;
                lastDdrTiming_ = timing;
                setErr(sawPlxd
                           ? "sendDdrFrame: PLXD timeout waiting for frames_done advance "
                             "and a released bank"
                           : "sendDdrFrame: PLXD acknowledgement required but unavailable");
                return false;
            }
        } else if (readBankRelease(brs)) {
            // Legacy/diagnostic/720 behaviour is unchanged: use a released bank
            // when available, otherwise reuse the non-display bank immediately.
            const DdrBankWriteDecision decision =
                decideDdrBankWrite(brs, policy, strictDdrRelease_);
            bank = decision.bank;
            plxdUsed = brs.anyFree();
            auto tPlxd1 = std::chrono::steady_clock::now();
            timing.plxa_poll_us = elapsedUs(tPrep0, tPlxd1);
            timing.plxa_poll_iters = 0;
            timing.plxa_used = plxdUsed;
        } else {
            // PLXD absent — pre-PLXD RBF or mailbox not yet written.
            // Fall back to the old timing-based mitigation: brief yield for
            // previous DMA (~1–3 ms) plus a two-vsync same-bank reuse floor.
            usleep(1500);
            const double nowMs = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
            const double lastBankMs = lastDdrBankDoorbellMs_[bank];
            if (lastBankMs >= 0.0) {
                const int64_t sinceUs =
                    static_cast<int64_t>((nowMs - lastBankMs) * 1000.0);
                if (sinceUs < kDdrBankReuseMinUs) {
                    const int64_t waitUs = kDdrBankReuseMinUs - sinceUs;
                    usleep(static_cast<useconds_t>(waitUs));
                    timing.bank_reuse_wait_us = waitUs;
                }
            }
        }
    }
    auto tPrep1 = std::chrono::steady_clock::now();
    timing.prep_wait_us = elapsedUs(tPrep0, tPrep1);

    // Copy frame into bank (persistent map).
    const size_t bankOff = static_cast<size_t>(bank) * ddrLayout_.bank_stride;
    auto tCopy0 = std::chrono::steady_clock::now();
    bool usedPl330 = false;
    const char* how = "memcpy";
    // WC /dev/mplex_ddr is the product 720p publication path (~2 ms). Userspace
    // PL330 DMAGO faults while kernel dma-pl330 owns the controller.
    if (!ddrMapViaMplex_ && ddrLayout_.presented_width == 1280 &&
        ddrLayout_.presented_height == 720 && len == kPlex720pYuv420pBytes) {
        static Pl330Mem2Mem dma;
        static bool dmaTried = false;
        static bool dmaOk = false;
        if (!dmaTried) {
            dmaTried = true;
            std::string derr;
            dmaOk = dma.open(&derr);
            std::fprintf(stderr, "misterplexd: 720p pl330 open %s %s\n",
                         dmaOk ? "ok" : "fail", derr.c_str());
        }
        if (dmaOk) {
            const uint32_t dstBase = ddrLayout_.phys_base +
                                     static_cast<uint32_t>(bank) * ddrLayout_.bank_stride;
            const uint32_t contig = resolveContiguousCachedPhys(payload, len);
            Pl330Mem2MemRequest req;
            req.dst_phys = dstBase;
            req.len = len;
            if (contig != 0) {
                req.src_phys = contig;
                const auto dr = dma.transferBlocking(req, 20);
                usedPl330 = dr.ok;
                if (usedPl330)
                    how = "pl330_contig";
                static bool loggedDma = false;
                if (!loggedDma) {
                    loggedDma = true;
                    std::fprintf(stderr, "misterplexd: 720p publish pl330_contig=%s %s\n",
                                 dr.ok ? "ok" : "fail", dr.detail.c_str());
                }
            }
        }
    }
    // Helper is (dynPhys, dynBaseRbf). Env default off so 03f1b95a still copies.
    const bool skipBankCopy = p720_av::plex720pSkipBankMemcpy(doorbellDynPhys_, [] {
        const char* e = std::getenv("MPX_DYN_SKIP_COPY");
        return e && e[0] == '1';
    }());
    if (skipBankCopy) {
        how = "dyn_skip";
        static bool loggedSkip = false;
        if (!loggedSkip) {
            loggedSkip = true;
            std::fprintf(stderr, "misterplexd: 720p publish dyn_skip phys=0x%x\n",
                         doorbellDynPhys_);
        }
    } else if (!usedPl330) {
        std::memcpy(ddrMap_ + bankOff, payload, len);
        // Do not mirror into the other 720p bank. Dual memcpy was 5.28 ms
        // post-swap (unique 21.5 vs 24.1 Hz). L4 doorbell names the dest
        // bank; leftover bank1 chevron was bank-0-only kicks, not a reason
        // to copy 1.38 MiB twice every frame.
        how = ddrMapViaMplex_ ? "wc" : "memcpy";
    }
    lastPublishHow_ = how;
    timing.publish_how = how;
    __sync_synchronize();
    auto tCopy1 = std::chrono::steady_clock::now();
    timing.copy_us = elapsedUs(tCopy0, tCopy1);
    if (!ddrMemSync_ && ddrMemFlush_) {
        auto tFlush0 = std::chrono::steady_clock::now();
        const uint8_t* flushPtr = skipBankCopy ? payload : (ddrMap_ + bankOff);
        if (!cleanDcacheRange(flushPtr, len)) {
            setErr("sendDdrFrame: cache clean failed");
            return false;
        }
        __sync_synchronize();
        auto tFlush1 = std::chrono::steady_clock::now();
        timing.flush_us = elapsedUs(tFlush0, tFlush1);
    }

    if (policy == DdrBankWritePolicy::RequireReleased) {
        if (!strictWriteAuthorized) {
            setErr("sendDdrFrame: strict PLXD write missing release sample");
            return false;
        }
        // From this point until a stable post-kick sample is captured, no retry
        // may trust PLXD. A kick may have escaped even if later verification
        // fails, so only reset/reprobe can recover from an unknown baseline.
        strictDdrRelease_.beginWrite();
    }

    bool saw_busy = false;
    bool saw_kick = false;
    bool saw_frame = false;
    const bool first = (ddrKickMode_ == 0);
    auto frameStoreStatusSuffix = [this]() -> std::string {
        FrameStoreStatus st{};
        if (readFrameStoreStatus(st)) {
            if (st.nonYuvDoorbellRejected())
                return std::string(": ") + frameStoreDebugDescription(st.debug_state);
            char buf[128]{};
            std::snprintf(buf, sizeof(buf),
                          ": frame-store status frame_debug=0x%02x frame_seq=%u "
                          "frame_underrun=%u",
                          static_cast<unsigned>(st.debug_state), static_cast<unsigned>(st.seq),
                          static_cast<unsigned>(st.underrun_count));
            return std::string(buf);
        }
        return std::string(": ") + frameStoreStatusUnavailableDescription() + ": " + lastError();
    };

    // Prefer mmap doorbell (no SPI on hot path). Fall back to SPI kick.
    bool kicked = false;
    auto tKick0 = std::chrono::steady_clock::now();
    BankReleaseStatus preKick{};
    const bool havePreKick = first && readBankRelease(preKick);
    if (ddrKickMode_ == 1 || ddrKickMode_ == 0) {
        if (kickDdrDoorbell(bank)) {
            kicked = true;
            if (first) {
                // Leftover has_frame from the idle chevron is not a new present.
                // Lock doorbell-only mode if PLXD heard the kick. R15 host-paint
                // may publish PLXF while PLXD never advances — accept PLXF
                // evidence even when PLXD words are readable but silent.
                FrameStoreStatus prePlxf{};
                const bool havePrePlxf = readFrameStoreStatus(prePlxf);
                if (havePreKick && waitPlxdHeardKick(preKick, 80000)) {
                    ++doorbellSeq_;
                    ddrKickMode_ = 1;
                } else if (havePrePlxf && waitPlxfHeardKick(prePlxf, 80000)) {
                    ++doorbellSeq_;
                    ddrKickMode_ = 1;
                } else {
                    kicked = false;
                }
            } else {
                ++doorbellSeq_;
            }
        }
    }
    if (!kicked && (ddrKickMode_ == 2 || ddrKickMode_ == 0)) {
        if (!kickDdrSpi(bank, first, saw_busy, saw_kick, saw_frame)) {
            if (policy == DdrBankWritePolicy::RequireReleased) {
                ddrKickMode_ = -1;
                ddrKickFailMs_ = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
            }
            return false;
        }
        if (first) {
            const bool heard = havePreKick && waitPlxdHeardKick(preKick, 80000);
            const bool ok = heard ||
                            (!havePreKick && (saw_busy || (saw_kick && saw_frame)));
            if (!ok) {
                if (policy == DdrBankWritePolicy::RequireReleased) {
                    ddrKickMode_ = -1;
                    ddrKickFailMs_ = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
                    setErr("sendDdrFrame: no kick/frame via SPI or doorbell" +
                           frameStoreStatusSuffix());
                    return false;
                }
                // BestEffort: keep probing. A 5s fail-closed cooldown freezes
                // glass on the chevron while Plex Web still reports playing.
                ddrKickMode_ = 0;
                setErr("sendDdrFrame: no kick/frame via SPI or doorbell" +
                       frameStoreStatusSuffix());
                return false;
            }
            if (heard)
                ++doorbellSeq_;
            ddrKickMode_ = 2;
        }
    }
    auto tKick1 = std::chrono::steady_clock::now();
    timing.doorbell_us = elapsedUs(tKick0, tKick1);
    if (first && ddrKickMode_ == 0) {
        ddrKickMode_ = -1;
        ddrKickFailMs_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        setErr("sendDdrFrame: could not kick DDR path" + frameStoreStatusSuffix());
        return false;
    }
    if (policy == DdrBankWritePolicy::RequireReleased) {
        // frames_done counts swaps only. Retain the release sample from before
        // this payload and publish it only after the kick succeeds. If the swap
        // completes before this point, the next call still observes a different
        // counter and may proceed instead of waiting for an impossible extra swap.
        strictDdrRelease_.noteWrite(strictReleaseSample);
    }
    lastDdrBankDoorbellMs_[bank] = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();

    // No post usleep: under dual-A9 decode load usleep(500) was inflated to
    // tens of ms; PLXD (when live) already serializes bank ownership.
    auto tPost0 = std::chrono::steady_clock::now();
    auto tPost1 = tPost0;
    timing.post_wait_us = elapsedUs(tPost0, tPost1);

    auto t1 = std::chrono::steady_clock::now();
    lastPushMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    timing.total_us = elapsedUs(t0, t1);
    lastDdrTiming_ = timing;
    clearErr();
    return true;
}

uint8_t* FpgaSpi::beginDdrBankIngest(size_t expectLen, int preferredBank, int& outBank) {
    if (expectLen != ddrLayout_.frame_bytes && ddrLayout_.frame_bytes != 0) {
        // Layout may not be programmed yet; open/map will set it via geometry path.
    }
    if (preferredBank < 0 || preferredBank > 1) {
        setErr("beginDdrBankIngest: bank must be 0 or 1");
        return nullptr;
    }
    if (ddrKickMode_ < 0) {
        constexpr double kReprobeIntervalMs = 5000.0;
        const double nowMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        if (ddrKickFailMs_ >= 0.0 && (nowMs - ddrKickFailMs_) < kReprobeIntervalMs) {
            setErr("beginDdrBankIngest: DDR path previously unavailable");
            return nullptr;
        }
        ddrKickMode_ = 0;
        strictDdrRelease_.reset();
    }
    if (!ok() && !open())
        return nullptr;
    if (!ensureDdrMap())
        return nullptr;
    if (expectLen != ddrLayout_.frame_bytes) {
        setErr("beginDdrBankIngest: frame size does not match DDR geometry");
        return nullptr;
    }

    DdrTiming timing{};
    ingestT0_ = std::chrono::steady_clock::now();
    auto tPrep0 = ingestT0_;
    int bank = preferredBank;
    bool plxdUsed = false;
    {
        BankReleaseStatus brs;
        constexpr int kPlxdPollMaxIters = 0; // match sendDdrFrame: no wait for free bank
        int plxdIters = 0;
        if (readBankRelease(brs)) {
            if (brs.anyFree()) {
                bank = brs.freeBank();
                plxdUsed = true;
            } else {
                for (int i = 0; i < kPlxdPollMaxIters; ++i) {
                    usleep(500);
                    ++plxdIters;
                    if (readBankRelease(brs) && brs.anyFree()) {
                        bank = brs.freeBank();
                        plxdUsed = true;
                        break;
                    }
                }
                if (!plxdUsed)
                    bank = brs.disp_bank ^ 1;
            }
            auto tPlxd1 = std::chrono::steady_clock::now();
            timing.plxa_poll_us = elapsedUs(tPrep0, tPlxd1);
            timing.plxa_poll_iters = plxdIters;
            timing.plxa_used = plxdUsed || (!plxdUsed && plxdIters > 0);
        } else {
            // No PLXD: skip multi-ms reuse floor on rate path (begin ingest).
            usleep(200);
        }
    }
    auto tPrep1 = std::chrono::steady_clock::now();
    timing.prep_wait_us = elapsedUs(tPrep0, tPrep1);
    timing.copy_us = 0; // caller fills bank via pipe read
    lastDdrTiming_ = timing;
    ingestBank_ = bank;
    outBank = bank;
    clearErr();
    return ddrMap_ + static_cast<size_t>(bank) * ddrLayout_.bank_stride;
}

bool FpgaSpi::commitDdrBankIngest(int bank, size_t len) {
    if (!coreGateAllows("commitDdrBankIngest"))
        return false;
    if (bank < 0 || bank > 1 || !ddrMap_) {
        setErr("commitDdrBankIngest: bad bank/map");
        return false;
    }
    if (len != ddrLayout_.frame_bytes) {
        setErr("commitDdrBankIngest: frame size mismatch");
        return false;
    }
    DdrTiming timing = lastDdrTiming_;
    auto t0 = ingestT0_;
    const size_t bankOff = static_cast<size_t>(bank) * ddrLayout_.bank_stride;
    // Dest-bank only. Dual-bank mirror here was 2.6 ms after the blank
    // (kick_lag 13–80 ms, unique 21.5). FPGA doorbell already selects `bank`.
    __sync_synchronize();
    if (!ddrMemSync_ && ddrMemFlush_) {
        auto tFlush0 = std::chrono::steady_clock::now();
        if (!cleanDcacheRange(ddrMap_ + bankOff, len)) {
            setErr("commitDdrBankIngest: cache clean failed");
            return false;
        }
        __sync_synchronize();
        auto tFlush1 = std::chrono::steady_clock::now();
        timing.flush_us = elapsedUs(tFlush0, tFlush1);
    }

    bool saw_busy = false;
    bool saw_kick = false;
    bool saw_frame = false;
    const bool first = (ddrKickMode_ == 0);
    auto frameStoreStatusSuffix = [this]() -> std::string {
        FrameStoreStatus st{};
        if (readFrameStoreStatus(st)) {
            if (st.nonYuvDoorbellRejected())
                return std::string(": ") + frameStoreDebugDescription(st.debug_state);
            char buf[128]{};
            std::snprintf(buf, sizeof(buf),
                          ": frame-store status frame_debug=0x%02x frame_seq=%u "
                          "frame_underrun=%u",
                          static_cast<unsigned>(st.debug_state), static_cast<unsigned>(st.seq),
                          static_cast<unsigned>(st.underrun_count));
            return std::string(buf);
        }
        return std::string(": ") + frameStoreStatusUnavailableDescription() + ": " + lastError();
    };

    bool kicked = false;
    auto tKick0 = std::chrono::steady_clock::now();
    BankReleaseStatus preKick{};
    const bool havePreKick = first && readBankRelease(preKick);
    if (ddrKickMode_ == 1 || ddrKickMode_ == 0) {
        if (kickDdrDoorbell(bank)) {
            kicked = true;
            if (first) {
                if (havePreKick && waitPlxdHeardKick(preKick, 80000)) {
                    ++doorbellSeq_;
                    ddrKickMode_ = 1;
                } else {
                    kicked = false;
                }
            } else {
                ++doorbellSeq_;
            }
        }
    }
    if (!kicked && (ddrKickMode_ == 2 || ddrKickMode_ == 0)) {
        if (!kickDdrSpi(bank, first, saw_busy, saw_kick, saw_frame))
            return false;
        if (first) {
            const bool heard = havePreKick && waitPlxdHeardKick(preKick, 80000);
            const bool okKick = heard ||
                                (!havePreKick && (saw_busy || (saw_kick && saw_frame)));
            if (!okKick) {
                ddrKickMode_ = 0;
                setErr("commitDdrBankIngest: no kick/frame via SPI or doorbell" +
                       frameStoreStatusSuffix());
                return false;
            }
            if (heard)
                ++doorbellSeq_;
            ddrKickMode_ = 2;
        }
    }
    auto tKick1 = std::chrono::steady_clock::now();
    timing.doorbell_us = elapsedUs(tKick0, tKick1);
    if (first && ddrKickMode_ == 0) {
        ddrKickMode_ = -1;
        ddrKickFailMs_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        setErr("commitDdrBankIngest: could not kick DDR path" + frameStoreStatusSuffix());
        return false;
    }
    lastDdrBankDoorbellMs_[bank] = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
    timing.post_wait_us = 0;
    auto t1 = std::chrono::steady_clock::now();
    lastPushMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    timing.total_us = elapsedUs(t0, t1);
    // copy_us remains 0 — pipe read into bank is timed by media_player as read_us.
    lastDdrTiming_ = timing;
    clearErr();
    return true;
}

bool FpgaSpi::readFrameStoreStatus(FrameStoreStatus& out) {
    if (!ok() && !open())
        return false;
    if (!ensureDdrMap())
        return false;
    // RTL: FRAME_MAILBOX_PHYS = page + 0x118 (mailbox_abi kPlxfOff) on the
    // build's PLX page; the layout doorbell is that page on every recognised
    // build (plx_page.hpp).
    if (ddrLayout_.doorbell_phys < ddrLayout_.phys_base) {
        setErr("readFrameStoreStatus: invalid doorbell_phys");
        return false;
    }
    const uint32_t plxfPhys = ddrLayout_.doorbell_phys + mailbox_abi::kPlxfOff;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mw = ddrMailboxWord(plxfPhys);
    if (!mw) {
        setErr("readFrameStoreStatus: PLXF mailbox is outside uncached doorbell page");
        return false;
    }
    uint32_t lastLo = 0;
    uint32_t lastHi = 0;
    bool stable = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo0 = mw[0];
        const uint32_t hi0 = mw[1];
        __sync_synchronize();
        const uint32_t lo1 = mw[0];
        const uint32_t hi1 = mw[1];
        lastLo = lo1;
        lastHi = hi1;
        stable = (lo0 == lo1 && hi0 == hi1);
        if (decodeStableFrameStoreStatus(lo0, hi0, lo1, hi1, out)) {
            clearErr();
            return true;
        }
        usleep(200);
    }
    if (stable && lastLo != kUnderrunMailboxMagic) {
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "readFrameStoreStatus: PLXF mailbox absent/unwritten "
                      "(lo=0x%08x hi=0x%08x)",
                      static_cast<unsigned>(lastLo), static_cast<unsigned>(lastHi));
        setErr(buf);
        return false;
    }
    setErr("readFrameStoreStatus: PLXF mailbox not valid/stable");
    return false;
}

bool FpgaSpi::plxStore32(uint32_t phys, uint32_t value) {
    if (!ok() && !open())
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* w = plxWord(phys);   // core gate
    if (!w) {
        setErr("plxStore32: PLX slot not mapped (core gate closed, or outside the PLX page)");
        return false;
    }
    *w = value;
    return true;
}

bool FpgaSpi::plxStore64(uint32_t phys, uint64_t value) {
    // K2 PLXO zero only (review 20:00 N2): an STRD may split its beats magic-first, so a
    // PLXH request never goes through a 64-bit store.
    if (phys != r1::kPlxoPhys) {
        setErr("plxStore64: only the K2 PLXO zero may use a 64-bit store");
        return false;
    }
    if ((phys & 7u) != 0 || (!ok() && !open()))
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* w = plxWord(phys);   // core gate
    if (!w) {
        setErr("plxStore64: PLX slot not mapped (core gate closed, or outside the PLX page)");
        return false;
    }
    *reinterpret_cast<volatile uint64_t*>(w) = value;
    return true;
}

bool FpgaSpi::plxLoad64(uint32_t phys, uint64_t* value) {
    if (!value || (phys & 7u) != 0 || (!ok() && !open()))
        return false;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* w = plxWord(phys);   // core gate
    if (!w) {
        setErr("plxLoad64: PLX slot not mapped (core gate closed, or outside the PLX page)");
        return false;
    }
    // PLXO is written by the RTL as one beat; two equal reads rule out a torn pair.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo0 = w[0], hi0 = w[1];
        __sync_synchronize();
        const uint32_t lo1 = w[0], hi1 = w[1];
        if (lo0 == lo1 && hi0 == hi1) {
            *value = (static_cast<uint64_t>(hi0) << 32) | lo0;
            return true;
        }
        usleep(50);
    }
    setErr("plxLoad64: PLX slot unstable");
    return false;
}

#ifdef MPX_TEST_F1_BEAT_SEAM
std::atomic<int64_t> mpxTestF1BeatPageHoldMaxUs{0};   // test seam: the beat's longest ddrPageMu_ hold
#endif

// F1 PLXL beat (fpga_spi.hpp; f1_beat.hpp). The only lock is the try_lock below (review 01:39 k1).
f1::BeatStore FpgaSpi::plxlBeatStore(uint64_t gateSnap, uint32_t plxlPhys, uint32_t* hb, bool* seeded) {
#ifdef MUT_F1_BEAT_CHOKEPOINT   // test-only mutant: the beat through R1's choke point (blocking lock,
                                // the mapper, coreGateAllows -> setErr on a closed gate)
    (void)gateSnap;
    *seeded = true;
    ++*hb;
    if (!plxStore32(plxlPhys + 4u, *hb) || !plxStore32(plxlPhys, f1::kPlxlMagic))
        return f1::BeatStore::GateMoved;
    return f1::BeatStore::Written;
#else
#ifdef MUT_F1_BEAT_BLOCKLOCK   // test-only mutant: a blocking page lock
    std::unique_lock<std::mutex> pageLock(ddrPageMu_);
#else
    std::unique_lock<std::mutex> pageLock(ddrPageMu_, std::try_to_lock);
    if (!pageLock.owns_lock())
        return f1::BeatStore::Contended;
#endif
#ifdef MPX_TEST_F1_BEAT_SEAM
    struct HoldMeter {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~HoldMeter() {
            const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - t0).count();
            int64_t cur = mpxTestF1BeatPageHoldMaxUs.load();
            while (us > cur && !mpxTestF1BeatPageHoldMaxUs.compare_exchange_weak(cur, us)) {
            }
        }
    } holdMeter;
#endif
    // In-lock re-check, lock-free: the opening the beat decided on is still the open one
    // (or the Rejected Plex idle-error snap, so 240p F1 can paint both BUILD_IDs).
    const CoreGate& gate = CoreGate::process();
    const bool openOk = gate.openAndLiveNoLock() && gate.openSnapshot() == gateSnap;
    const bool idleOk = gate.idleErrorPaintAllowed() && gate.fabricLiveNoLock() &&
                        gate.idleErrorSnap() == gateSnap;
    if (!openOk && !idleOk)
        return f1::BeatStore::GateMoved;
    const uint32_t page = plxlPhys & ~0xFFFu;
    if ((plxlPhys & 7u) != 0 || plxlPhys + 8u > page + 4096u)
        return f1::BeatStore::GateMoved;
    uint8_t* map = nullptr;
    for (const auto& s : ddrPages_) {   // mapped-only: the beat never maps (p3)
        if (s.map && s.phys == page) {
            map = s.map;
            break;
        }
    }
#ifdef MUT_F1_BEAT_MAPS   // test-only mutant: the beat maps an unmapped page
    if (!map && !ensureDdrMailboxMap(page, map))
        map = nullptr;
#endif
    if (!map)
        return f1::BeatStore::Unmapped;
    volatile uint32_t* w = reinterpret_cast<volatile uint32_t*>(map + (plxlPhys - page));
#ifndef MUT_F1_BEAT_NOSEED   // test-only mutant: hb restarts at 1 instead of the DDR word's hb + 1
    if (!*seeded)
        *hb = w[1];
#else
    if (!*seeded)
        *hb = 0;
#endif
    *seeded = true;
    ++*hb;
    w[1] = *hb;            // hb first: a reader never pairs a new magic with a stale hb
    __sync_synchronize();
#ifdef MUT_F1_BEAT_MAGIC_ONCE   // test-only mutant: the magic only on the first write
    static bool magicDone = false;
    if (!magicDone)
        w[0] = f1::kPlxlMagic;
    magicDone = true;
#else
    w[0] = f1::kPlxlMagic;  // every beat: a zeroed word heals at the next beat (H264 x3)
#endif
    return f1::BeatStore::Written;
#endif
}

bool FpgaSpi::readBankRelease(BankReleaseStatus& out) {
    if (!ok() && !open())
        return false;
    if (!ensureDdrMap())
        return false;
    // RTL: BANK_MAILBOX_PHYS = page + 0x128 (mailbox_abi kPlxdOff) on the
    // build's PLX page; the layout doorbell is that page on every recognised build.
    if (ddrLayout_.doorbell_phys < ddrLayout_.phys_base) {
        setErr("readBankRelease: invalid doorbell_phys");
        return false;
    }
    const uint32_t plxdPhys = ddrLayout_.doorbell_phys + mailbox_abi::kPlxdOff;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mw = ddrMailboxWord(plxdPhys);
    if (!mw) {
        setErr("readBankRelease: PLXD mailbox is outside uncached doorbell page");
        return false;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo0 = mw[0];
        const uint32_t hi0 = mw[1];
        __sync_synchronize();
        const uint32_t lo1 = mw[0];
        const uint32_t hi1 = mw[1];
        if (decodeStableBankRelease(lo0, hi0, lo1, hi1, out)) {
            clearErr();
            return true;
        }
        usleep(200);
    }
    setErr("readBankRelease: PLXD mailbox absent or unstable");
    return false;
}

bool FpgaSpi::blitDisplayedBank(const uint8_t* payload, size_t len) {
    if (!payload || len == 0) {
        setErr("blitDisplayedBank: empty");
        return false;
    }
    if (!ok() && !open())
        return false;
    if (!ensureDdrMap())
        return false;
    if (ddrLayout_.frame_bytes == 0 || len < ddrLayout_.frame_bytes) {
        setErr("blitDisplayedBank: frame size mismatch");
        return false;
    }
    BankReleaseStatus brs;
    int prefer = 0;
    bool havePlxd = false;
    if (readBankRelease(brs) && brs.disp_bank <= 1) {
        prefer = static_cast<int>(brs.disp_bank);
        havePlxd = true;
    }
    for (int bank = 0; bank < 2; ++bank) {
        const size_t bankOff = static_cast<size_t>(bank) * ddrLayout_.bank_stride;
        if (bankOff + ddrLayout_.frame_bytes > ddrMapLen_)
            continue;
        std::memcpy(ddrMap_ + bankOff, payload, ddrLayout_.frame_bytes);
        if (!ddrMemSync_ && ddrMemFlush_)
            (void)cleanDcacheRange(ddrMap_ + bankOff, ddrLayout_.frame_bytes);
    }
    __sync_synchronize();
    // A second doorbell while swap_pending is already 1 restacks the request
    // and can leave pending_ready false across a video_mode vsync gap.
    if (!havePlxd || !brs.swap_pending)
        (void)kickDdrDoorbell(prefer);
    clearErr();
    return true;
}

bool FpgaSpi::readSourceAspectAck(SourceAspectAck& out) {
    if (!ok() && !open())
        return false;
    if (!ensureDdrMap())
        return false;
    if (ddrLayout_.doorbell_phys < ddrLayout_.phys_base) {
        setErr("readSourceAspectAck: invalid doorbell_phys");
        return false;
    }
    const uint32_t plxjPhys = ddrLayout_.doorbell_phys + mailbox_abi::kPlxjOff;
    const auto pageLock = lockDdrPage();
    volatile uint32_t* mw = ddrMailboxWord(plxjPhys);
    if (!mw) {
        setErr("readSourceAspectAck: PLXJ mailbox is outside uncached doorbell page");
        return false;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t lo0 = mw[0];
        const uint32_t hi0 = mw[1];
        __sync_synchronize();
        const uint32_t lo1 = mw[0];
        const uint32_t hi1 = mw[1];
        if (decodeStableSourceAspectAck(lo0, hi0, lo1, hi1, out)) {
            clearErr();
            return true;
        }
        usleep(200);
    }
    setErr("readSourceAspectAck: PLXJ mailbox absent or unstable");
    return false;
}

uint8_t* FpgaSpi::ddrBankVirt(int bank) {
    if (bank < 0 || bank > 1)
        return nullptr;
    if (!ensureDdrMap() || !ddrMap_)
        return nullptr;
    const size_t off = static_cast<size_t>(bank) * ddrLayout_.bank_stride;
    if (off + ddrLayout_.frame_bytes > ddrMapLen_)
        return nullptr;
    return ddrMap_ + off;
}

bool FpgaSpi::sendYuv420pFrameDdr(const uint8_t* yuv420p, size_t len,
                                  const DdrFrameGeometry& geometry, int bank,
                                  DdrBankWritePolicy policy, uint32_t srcPhys) {
    if (!yuv420p || geometry.coded_width <= 0 || geometry.coded_height <= 0 ||
        (geometry.coded_width & 1) || (geometry.coded_height & 1)) {
        setErr("sendYuv420pFrameDdr: bad YUV420p frame");
        return false;
    }
    if (geometry.coded_width != ddrLayout_.coded_width ||
        geometry.coded_height != ddrLayout_.coded_height ||
        geometry.display_width != ddrLayout_.display_width ||
        geometry.display_height != ddrLayout_.display_height ||
        geometry.presented_width != ddrLayout_.presented_width ||
        geometry.presented_height != ddrLayout_.presented_height ||
        geometry.crop_left != ddrLayout_.crop_left || geometry.crop_right != ddrLayout_.crop_right ||
        geometry.crop_top != ddrLayout_.crop_top || geometry.crop_bottom != ddrLayout_.crop_bottom ||
        geometry.present_x != ddrLayout_.present_x || geometry.present_y != ddrLayout_.present_y ||
        ddrLayout_.format != DdrFrameFormat::Yuv420p) {
        if (!setDdrFrameLayout(geometry, DdrFrameFormat::Yuv420p))
            return false;
    }
    doorbellDynPhys_ = srcPhys;
    return sendDdrFrame(yuv420p, len, bank, policy);
}

bool FpgaSpi::sendYuv420pFrameDdr(const uint8_t* yuv420p, size_t len, int width, int height,
                                  int bank, DdrBankWritePolicy policy) {
    return sendYuv420pFrameDdr(yuv420p, len, makeDdrFrameGeometry(width, height), bank,
                               policy);
}

bool FpgaSpi::sendPcmChunk(const uint8_t* pcm, size_t len, uint8_t index) {
    if (!pcm || !len) {
        setErr("sendPcmChunk: empty");
        return false;
    }
    // Align to 4-byte stereo frames
    len &= ~size_t(3);
    if (!len)
        return true;
    return sendFileTx(pcm, len, index);
}

bool FpgaSpi::sendBitstreamChunk(const uint8_t* data, size_t len, uint8_t index) {
    if (!data || !len) {
        setErr("sendBitstreamChunk: empty");
        return false;
    }
    return sendFileTx(data, len, index);
}

bool FpgaSpi::sendSourceAspect(const SourceAspect& requested, SourceAspect* sent,
                               std::string* note) {
    // Never put a term above kSourceAspectMax on the wire: the RTL keeps 12 bits
    // (source_aspect_ack.sv) and would latch the value mod 4096.
    const SourceAspect aspect = sourceAspectForSend(requested, note);
    if (sent)
        *sent = aspect;
    if (!aspect.valid) {
        setErr("sendSourceAspect: invalid source aspect");
        return false;
    }

    SourceAspectAck baseline;
    uint8_t token = static_cast<uint8_t>(sourceAspectToken_ + 1u);
    const bool baselineOk = readSourceAspectAck(baseline);
    if (baselineOk)
        token = static_cast<uint8_t>(baseline.token + 1u);
    sourceAspectToken_ = token;

    const auto packet = encodeSourceAspectPacket(aspect, token);
    if (!sendFileTx(packet.data(), packet.size(), kSourceAspectIoctlIndex)) {
        if (d52::Logger* d = d52::active())
            d->plxaResult(aspect.x, aspect.y, token, baselineOk, baseline, "download_failed", SourceAspectAck{});
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
    SourceAspectAck ack;
    do {
        if (readSourceAspectAck(ack) && ack.token == token &&
            ack.aspect.x == aspect.x && ack.aspect.y == aspect.y) {
            if (d52::Logger* d = d52::active())
                d->plxaResult(aspect.x, aspect.y, token, baselineOk, baseline, "matched", ack);
            clearErr();
            return true;
        }
        usleep(1000);
    } while (std::chrono::steady_clock::now() < deadline);
    if (d52::Logger* d = d52::active())
        d->plxaResult(aspect.x, aspect.y, token, baselineOk, baseline, "ack_timeout", ack);

    char buf[192]{};
    std::snprintf(buf, sizeof(buf),
                  "sendSourceAspect: PLXJ ACK timeout want=%u:%u token=%u "
                  "last=%u:%u token=%u",
                  aspect.x, aspect.y, token,
                  ack.aspect.x, ack.aspect.y, ack.token);
    setErr(buf);
    return false;
}

bool FpgaSpi::flushBitstreamDdr() {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (bitstreamSessionActive_ && bitstreamProtocolVersion_ != 0) {
        setErr("flushBitstreamDdr: end the FPGA video session before resetting its ring");
        return false;
    }
    if (!ensureBitstreamDdrMap())
        return false;
    bitstreamWriteCount_ = 0;
    bitstreamLegacySeq_ = 0;
    bitstreamLegacyActive_ = false;
    bitstreamSessionActive_ = false;
    bitstreamSessionId_ = 0;
    bitstreamNextSeq_ = 0;
    bitstreamProtocolVersion_ = 0;
    videoCapabilities_ = {};
    bitstreamResetEpoch_ = !bitstreamResetEpoch_;
    std::memset(bitstreamMap_, 0, ring::kRingBytes);
    publishBitstreamCtrl();
    if (d52::Logger* d = d52::active()) {
        d->ringReset("EPOCH_RESET (flushBitstreamDdr)", bitstreamResetEpoch_ ? 1 : 0, "published");
        d->disarm("ring epoch reset");
    }
    clearErr();
    return true;
}

bool FpgaSpi::resetBitstreamRing(int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    if (!ensureBitstreamDdrMap())
        return false;
    auto* ctrl = reinterpret_cast<volatile uint64_t*>(
        bitstreamMap_ + ring::kCtrlPhys - ring::kDataPhys);
    if (!bitstreamResetPending_) {
        const uint64_t previous = *ctrl;
        bitstreamResetEpoch_ = ((previous >> 63) & 1u) == 0;
        bitstreamWriteCount_ = 0;
        bitstreamResetPending_ = true;
        __sync_synchronize();
        publishBitstreamCtrl();
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    do {
        BitstreamStatus status;
        if (readBitstreamStatus(status) && status.consumer_count == 0 &&
            !status.active && !status.fatal && !status.desync &&
            status.reset_epoch == bitstreamResetEpoch_) {
            bitstreamResetPending_ = false;
            bitstreamProtocolVersion_ = 0;
            bitstreamLegacyActive_ = false;
            videoCapabilities_ = {};
            if (d52::Logger* d = d52::active()) {
                d->ringReset("RESET (resetBitstreamRing)", bitstreamResetEpoch_ ? 1 : 0, "acknowledged");
                d->disarm("ring reset");
            }
            clearErr();
            return true;
        }
        usleep(500);
    } while (std::chrono::steady_clock::now() < deadline);
    // Drop pending so the next begin can issue a fresh epoch toggle. Leaving
    // pending stuck made every subsequent play fail until process restart.
    bitstreamResetPending_ = false;
    if (d52::Logger* d = d52::active()) {
        d->ringReset("RESET (resetBitstreamRing)", bitstreamResetEpoch_ ? 1 : 0, "timeout");
        d->disarm("ring reset");
    }
    noteWedgeTrigger("H-a");
    setErr("resetBitstreamRing: no fresh FPGA reset-epoch acknowledgement");
    return false;
}

bool FpgaSpi::readBitstreamSnapshot(uint32_t phys, uint64_t* snapshot, size_t count) {
    namespace ring = ddr_bitstream_ring;
    if (!ensureBitstreamDdrMap())
        return false;
    if (phys < ring::kDataPhys || count == 0 || snapshot == nullptr ||
        count > bitstreamMapLen_ / sizeof(uint64_t) ||
        static_cast<uint64_t>(phys - ring::kDataPhys) + count * sizeof(uint64_t) >
            bitstreamMapLen_) {
        setErr("readBitstreamSnapshot: mailbox outside the negotiated mapping");
        return false;
    }
    auto* words = reinterpret_cast<volatile uint64_t*>(
        bitstreamMap_ + phys - ring::kDataPhys);
    const uint64_t before = words[count - 1];
    if ((before >> 32) == 0)
        return false;
    __sync_synchronize();
    for (size_t i = 0; i < count; ++i)
        snapshot[i] = words[i];
    __sync_synchronize();
    return before == snapshot[count - 1] && before == words[count - 1];
}

bool FpgaSpi::readVideoCapabilities(uint64_t nonce, VideoCapabilities& capabilities) {
    namespace ring = ddr_bitstream_ring;
    std::array<uint64_t, 8> snapshot{};
    if (!readBitstreamSnapshot(mailbox_abi::kVideoCapsAddr, snapshot.data(), snapshot.size()))
        return false;
    return ring::decodeVideoCapabilities(snapshot, nonce, capabilities);
}

bool FpgaSpi::readVideoPresentation(uint64_t session_id, VideoPresentation& status) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    status = {};
    // The video thread polls this every ~2 ms under presentMu_: its error text is
    // a hot-path update (setErrHot / clearErr, non-blocking under the lab flag).
    if (!requireBitstreamSession(session_id, "readVideoPresentation", true))
        return false;
    if (bitstreamProtocolVersion_ != mailbox_abi::kFpgaVideoAbiVersion) {
        setErr("readVideoPresentation: session has no negotiated video feedback ABI");
        return false;
    }
    std::array<uint64_t, 9> snapshot{};
    if (!readBitstreamSnapshot(mailbox_abi::kVideoPresentationAddr, snapshot.data(),
                              snapshot.size()) ||
        !ring::decodeVideoPresentation(snapshot, session_id, videoCapabilities_.nonce, status)) {
        setErrHot("readVideoPresentation: awaiting current-epoch presentation feedback");
        return false;
    }
    clearErr();
    return true;
}

bool FpgaSpi::readAudioSessionStatus(uint64_t session_id, AudioSessionStatus& status) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    status = {};
    if (!bitstreamSessionActive_ || session_id != bitstreamSessionId_ ||
        videoCapabilities_.nonce == 0)
        return false;
    std::array<uint64_t, 8> words{};
    return readBitstreamSnapshot(mailbox_abi::kAudioStatusAddr, words.data(), words.size()) &&
           audio_session::decodeStatus(words, status) && status.supported &&
           status.session_id == session_id && status.nonce == videoCapabilities_.nonce;
}

bool FpgaSpi::controlAudioSession(uint64_t session_id, AudioControl command, int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    constexpr uint32_t required = ring::ConsumedAudioClock | ring::AudioSessionControl;
    if (!audio_session::validControl(command) || !bitstreamSessionActive_ ||
        session_id != bitstreamSessionId_ || bitstreamResetPending_ ||
        (bitstreamAbortPending_ && command != AudioControl::Reset) ||
        bitstreamProtocolVersion_ != mailbox_abi::kFpgaVideoAbiVersion ||
        (videoCapabilities_.features & required) != required ||
        videoCapabilities_.nonce == 0) {
        setErr("controlAudioSession: no matching real DMA-session capability");
        return false;
    }
    uint32_t producerPosition = 0;
    // MrAudio open() reads status without moving the ring origin. Its write() is the only
    // metadata publisher, so carry the quiesced producer position rather than waiting for
    // an impossible later write.
    if (command != AudioControl::Pause && !readMrAudioProducerPosition(producerPosition))
        return false;
    const uint64_t nonce = videoCapabilities_.nonce;
    const uint64_t token = publishAudioControl(session_id, nonce, command, producerPosition);
    if (token == 0)
        return false;
    if (command == AudioControl::Begin)
        audioSessionOwned_ = true; // A timed-out command may still be in flight.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    do {
        AudioSessionStatus status;
        if (readAudioSessionStatus(session_id, status) &&
            audio_session::matchesAck(status, session_id, nonce, token, command)) {
            if (status.error != 0) {
                setErr("controlAudioSession: DMA consumer rejected the command");
                return false;
            }
            if (audio_session::completed(status, command)) {
                if (command == AudioControl::Reset)
                    audioSessionOwned_ = false;
                clearErr();
                return true;
            }
        }
        usleep(500);
    } while (std::chrono::steady_clock::now() < deadline);
    // No ACK bound to this session. The consumer only answers its own identity, and the
    // bound read above drops every other one, so ask the lane itself (unfiltered MAST).
    std::string why;
    AudioSessionStatus lane;
    const AudioLane verdict = classifyAudioLane(session_id, nonce, token, command, why, lane);
    if (verdict == AudioLane::NotOwned &&
        (command == AudioControl::Begin || command == AudioControl::Reset)) {
        // The consumer does not run this session: a Begin it did not adopt leaves no
        // audio of ours, and a Reset has nothing of ours to reset.
        audioSessionOwned_ = false;
        if (command == AudioControl::Reset) {
            clearErr();
            return true;
        }
    }
    if (verdict == AudioLane::Owned && command == AudioControl::Reset && !lane.active &&
        !lane.read_pending && !lane.prefetched) {
        // Our identity, inactive and retired: the Reset took effect (its ACK was missed).
        audioSessionOwned_ = false;
        clearErr();
        return true;
    }
    setErr(std::string("controlAudioSession: no fresh nonce/token-bound DMA acknowledgement (") +
           audioControlName(command) + "): " + why);
    return false;
}

bool FpgaSpi::audioSessionOwned() const {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    return audioSessionOwned_;
}

bool FpgaSpi::readMrAudioProducerPosition(uint32_t& position) {
    position = 0;
    for (unsigned observation = 0; observation < 2; ++observation) {
        const int fd = ::open(MPX_MRAUDIO_PATH, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            setErr("controlAudioSession: cannot read MrAudio producer position");
            return false;
        }
        char text[256]{};
        ssize_t count;
        do {
            count = ::read(fd, text, sizeof(text) - 1);
        } while (count < 0 && errno == EINTR);
        ::close(fd);
        const auto producer = parseMrAudioStatus(text, count);
        if (!producer.valid() || (producer.writePointer & 3) != 0) {
            setErr("controlAudioSession: invalid MrAudio producer position");
            return false;
        }
        const auto observed = static_cast<uint32_t>(producer.writePointer % kMrAudioRingBytes);
        if (observation != 0 && observed != position) {
            setErr("controlAudioSession: PCM producer is not quiescent");
            return false;
        }
        position = observed;
    }
    return true;
}

uint64_t FpgaSpi::publishAudioControl(uint64_t session_id, uint64_t nonce, AudioControl command,
                                      uint32_t producerPosition) {
    namespace ring = ddr_bitstream_ring;
    if (audioCommandToken_ == UINT64_MAX || !ensureBitstreamDdrMap()) {
        setErr("controlAudioSession: command token exhausted or mailbox unavailable");
        return 0;
    }
    const uint64_t token = ++audioCommandToken_;
    if (++audioPublication_ == 0)
        ++audioPublication_;
    const auto commandWords =
        audio_session::encodeControl(session_id, nonce, token, command, audioPublication_,
                                     producerPosition);
    auto* words = reinterpret_cast<volatile uint64_t*>(
        bitstreamMap_ + mailbox_abi::kAudioControlAddr - ring::kDataPhys);
    words[5] = 0;
    __sync_synchronize();
    for (size_t i = 0; i < 5; ++i)
        words[i] = commandWords[i];
    __sync_synchronize();
    words[5] = commandWords[5];
    __sync_synchronize();
    return token;
}

bool FpgaSpi::readAudioConsumerStatus(AudioSessionStatus& status) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    status = {};
    std::array<uint64_t, 8> words{};
    return readBitstreamSnapshot(mailbox_abi::kAudioStatusAddr, words.data(), words.size()) &&
           audio_session::decodeStatus(words, status) && status.supported;
}

bool FpgaSpi::readFreshAudioConsumerStatus(unsigned fresh, int timeout_ms,
                                           AudioSessionStatus& status) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    status = {};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    bool haveBase = false;
    uint32_t base = 0;
    do {
        AudioSessionStatus s;
        if (readAudioConsumerStatus(s)) {
            if (!haveBase) {
                haveBase = true;
                base = s.publication;
            } else if (static_cast<uint32_t>(s.publication - base) >= fresh) {
                status = s;
                return true;
            }
        }
        usleep(200);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

FpgaSpi::AudioLane FpgaSpi::classifyAudioLane(uint64_t session_id, uint64_t nonce,
                                              uint64_t token, AudioControl command,
                                              std::string& why, AudioSessionStatus& status) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    why.clear();
    status = {};
    if (!ensureBitstreamDdrMap()) {
        why = "MAST unreadable (" + lastError() + ")";
        return AudioLane::Wedged;
    }
    // Withdraw the command first: a poll that reads the slot from here on finds no valid
    // request, and one that already dispatched it publishes the outcome within the fresh
    // publications awaited below. Nothing of this session can be adopted after this.
    auto* words = reinterpret_cast<volatile uint64_t*>(
        bitstreamMap_ + mailbox_abi::kAudioControlAddr - ring::kDataPhys);
    words[5] = 0;
    __sync_synchronize();
    if (!readFreshAudioConsumerStatus(kAudioLaneFreshPublications, kAudioLaneFreshMs, status)) {
        why = "MrAudio lane wedged: no fresh MAST publication in " +
              std::to_string(kAudioLaneFreshMs) +
              " ms (the mailbox waits on a consumer ACK, or its DDR lane stalls); "
              "reload the core";
        return AudioLane::Wedged;
    }
    const bool ours = status.session_id == session_id && status.nonce == nonce;
    const bool answered = status.ack_session_id == session_id && status.ack_nonce == nonce &&
                          status.token == token && status.command == command;
    char text[320];
    std::snprintf(text, sizeof(text),
                  "%s; MAST pub=%u consumer=%016llx/%016llx%s%s%s%s ack=%016llx/%016llx "
                  "token=%llu op=%u err=%u",
                  ours ? "the consumer runs this session"
                       : answered ? (status.error != 0 ? "the consumer rejected it"
                                                       : "the consumer applied it")
                                  : "the mailbox never forwarded it to the consumer",
                  status.publication, static_cast<unsigned long long>(status.session_id),
                  static_cast<unsigned long long>(status.nonce),
                  status.active ? " active" : " inactive", status.paused ? " paused" : "",
                  status.read_pending ? " read_pending" : "",
                  status.prefetched ? " prefetched" : "",
                  static_cast<unsigned long long>(status.ack_session_id),
                  static_cast<unsigned long long>(status.ack_nonce),
                  static_cast<unsigned long long>(status.token),
                  static_cast<unsigned>(status.command), static_cast<unsigned>(status.error));
    why = text;
    return ours ? AudioLane::Owned : AudioLane::NotOwned;
}

bool FpgaSpi::recoverOrphanedAudioConsumer(const VideoCapabilities& capabilities,
                                           int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    constexpr uint32_t required = ring::ConsumedAudioClock | ring::AudioSessionControl;
    if ((capabilities.features & required) != required)
        return true; // no MrAudio session lane on this core
    // A fresh MAST: one left in DDR by an earlier core instance must not be read as the
    // consumer's state, and a lane that publishes nothing cannot take this session's
    // Begin either. Say so now, before video begins and an epoch is owned.
    AudioSessionStatus orphan;
    if (!readFreshAudioConsumerStatus(kAudioLaneFreshPublications,
                                      std::min(timeout_ms, kAudioLaneFreshMs), orphan)) {
        beginWedged_ = true;
        setErr("beginFpgaVideoSession: MrAudio lane wedged: no fresh MAST publication in " +
               std::to_string(std::min(timeout_ms, kAudioLaneFreshMs)) + " ms; reload the core");
        return false;
    }
    if (!orphan.active || orphan.session_id == 0 || orphan.nonce == 0)
        return true; // inactive consumer: the session's Begin is accepted
    uint32_t producerPosition = 0;
    if (!readMrAudioProducerPosition(producerPosition))
        return false;
    const uint64_t token =
        publishAudioControl(orphan.session_id, orphan.nonce, AudioControl::Reset, producerPosition);
    if (token == 0)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    do {
        AudioSessionStatus status;
        if (readAudioConsumerStatus(status) &&
            audio_session::matchesAck(status, orphan.session_id, orphan.nonce, token,
                                      AudioControl::Reset)) {
            if (status.error != 0) {
                setErr("beginFpgaVideoSession: MrAudio consumer refused the recovery Reset of "
                       "a previous owner's session; reload the core");
                return false;
            }
            if (audio_session::completed(status, AudioControl::Reset)) {
                char note[200];
                std::snprintf(note, sizeof(note),
                              "FPGA reclaimed the MrAudio consumer a previous owner left "
                              "active (session=%016llx nonce=%016llx samples=%llu): recovery "
                              "Reset ACK, no core reload",
                              static_cast<unsigned long long>(orphan.session_id),
                              static_cast<unsigned long long>(orphan.nonce),
                              static_cast<unsigned long long>(orphan.samples_consumed));
                audioRecoveryNote_ = note;
                clearErr();
                return true;
            }
        }
        usleep(500);
    } while (std::chrono::steady_clock::now() < deadline);
    setErr("beginFpgaVideoSession: no recovery Reset ACK from the MrAudio consumer a previous "
           "owner left active; reload the core");
    return false;
}

std::string FpgaSpi::takeAudioRecoveryNote() {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    std::string note;
    note.swap(audioRecoveryNote_);
    return note;
}

bool FpgaSpi::lastBeginWedged() const {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    return beginWedged_;
}

bool FpgaSpi::withdrawAudioCommand(std::string& why) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    why.clear();
    if (!ensureBitstreamDdrMap()) { // the core gate decides: another core's DDR is not ours
        why = lastError();
        return false;
    }
    auto* words = reinterpret_cast<volatile uint64_t*>(
        bitstreamMap_ + mailbox_abi::kAudioControlAddr - ring::kDataPhys);
#ifndef MUT_MAST_NO_EXIT_WITHDRAW   // test-only mutant: an exit leaves the command valid
    words[5] = 0;
    __sync_synchronize();
#else
    (void)words;
#endif
    return true;
}

bool FpgaSpi::probeAudioLane(mast::Probe& out) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    out = {};
    out.needed = kAudioLaneFreshPublications;
    out.windowMs = kAudioLaneFreshMs;
    constexpr uint32_t required = ring::ConsumedAudioClock | ring::AudioSessionControl;
    out.lane = (gateCapabilities_.features & required) == required;
    if (!out.lane) {
        out.live = true;
        return true;
    }
    out.mapped = ensureBitstreamDdrMap();
    if (!out.mapped)
        return false;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(kAudioLaneFreshMs);
    do {
        ++out.reads;
        AudioSessionStatus s;
        if (readAudioConsumerStatus(s)) {
            if (out.decoded++ == 0)
                out.firstPub = s.publication;
            out.lastPub = s.publication;
            out.fresh = static_cast<uint32_t>(s.publication - out.firstPub);
#ifdef MUT_MAST_STUCK_IS_LIVE   // test-only mutant: a decodable MAST counts as live
            out.live = true;
#else
            out.live = out.fresh >= out.needed;
#endif
            if (out.live)
                break;
        }
        usleep(200);
    } while (std::chrono::steady_clock::now() < deadline);
    out.elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
    return out.live;
}

uint64_t FpgaSpi::initialAudioCommandToken() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 && clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

bool FpgaSpi::abortFpgaVideoSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!bitstreamSessionActive_ || session_id != bitstreamSessionId_ ||
        (videoCapabilities_.features & ddr_bitstream_ring::FencedReset) == 0) {
        setErr("abortFpgaVideoSession: no matching session with a real decoder-reset fence");
        return false;
    }
    bitstreamAbortPending_ = true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    auto remaining = [&]() {
        return static_cast<int>(std::max<int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count()));
    };
    // The audio stage may not eat the whole budget: when the consumer turns out not to
    // own this session (classifyAudioLane), the ring reset still needs its own wait.
    if (audioSessionOwned_ &&
        !controlAudioSession(session_id, AudioControl::Reset, std::min(remaining(), 1000)))
        return false;
    if (!resetBitstreamRing(std::max(remaining(), 500)))
        return false;
    bitstreamSessionActive_ = false;
    bitstreamSessionId_ = 0;
    bitstreamNextSeq_ = 0;
    bitstreamAbortPending_ = false;
    clearErr();
    return true;
}

FpgaSpi::VideoCapabilities FpgaSpi::videoCapabilities() const {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    return videoCapabilities_;
}

bool FpgaSpi::beginFpgaVideoSession(uint64_t session_id,
                                   VideoCapabilities& capabilities, int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    capabilities = {};
    beginWedged_ = false;
    if (session_id == 0 || bitstreamSessionActive_) {
        setErr("beginFpgaVideoSession: require a nonzero epoch and no active session");
        return false;
    }
    uint64_t nonce = 0;
    const int randomFd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (randomFd < 0) {
        setErr("beginFpgaVideoSession: cannot obtain a fresh core challenge");
        return false;
    }
    size_t filled = 0;
    while (filled < sizeof(nonce)) {
        const ssize_t count = ::read(randomFd,
            reinterpret_cast<uint8_t*>(&nonce) + filled, sizeof(nonce) - filled);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        filled += static_cast<size_t>(count);
    }
    ::close(randomFd);
    if (filled != sizeof(nonce) || nonce == 0) {
        setErr("beginFpgaVideoSession: incomplete core challenge");
        return false;
    }
    if (!resetBitstreamRing(timeout_ms))
        return false;
    if (writeBitstreamRecord(ring::Event::Probe, nonce, mailbox_abi::kFpgaVideoLayoutId,
                            0, nullptr, 0, timeout_ms) != BitstreamPushResult::Ok)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    bool haveCapabilities = false;
    do {
        if (readVideoCapabilities(nonce, capabilities)) {
            haveCapabilities = true;
            break;
        }
        BitstreamStatus status;
        if (readBitstreamStatus(status) && (status.fatal || status.desync)) {
            setErr("beginFpgaVideoSession: core rejected FPGA-video ABI negotiation");
            return false;
        }
        usleep(500);
    } while (std::chrono::steady_clock::now() < deadline);
    if (!haveCapabilities) {
        setErr("beginFpgaVideoSession: no fresh core capability reply");
        return false;
    }
    if (!capabilities.supportsVideo()) {
        setErr("beginFpgaVideoSession: incompatible core layout, build or H.264/color/commit capabilities");
        return false;
    }
    // The AU-ring reset above made video inactive: reclaim a MrAudio consumer that a
    // killed owner left active, so this session's audio Begin is not refused.
    if (!recoverOrphanedAudioConsumer(capabilities, timeout_ms))
        return false;
    bitstreamProtocolVersion_ = mailbox_abi::kFpgaVideoAbiVersion;
    videoCapabilities_ = capabilities;
    if (!beginBitstreamSession(session_id, timeout_ms)) {
        if (!bitstreamSessionActive_) {
            bitstreamProtocolVersion_ = 0;
            videoCapabilities_ = {};
        }
        return false;
    }
    return true;
}

bool FpgaSpi::beginBitstreamSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (session_id == 0 || bitstreamSessionActive_) {
        setErr("beginBitstreamSession: invalid epoch or active session");
        return false;
    }
    BitstreamStatus st;
    if (readBitstreamStatus(st) && st.active) {
        setErr("beginBitstreamSession: session already active; end first");
        return false;
    }
    const auto r = writeBitstreamRecord(ddr_bitstream_ring::Event::Begin, session_id, 0, 0,
                                        nullptr, 0, timeout_ms);
    if (r != BitstreamPushResult::Ok)
        return false;
    if (bitstreamProtocolVersion_ == mailbox_abi::kFpgaVideoAbiVersion) {
        // Once published, a timeout is not proof that Begin did not happen.
        // Retain the epoch/capabilities so bounded fenced abort remains possible.
        bitstreamSessionActive_ = true;
        bitstreamSessionId_ = session_id;
    }
    if (!waitBitstreamControl(ddr_bitstream_ring::Event::Begin, session_id,
                             bitstreamWriteCount_, timeout_ms)) {
        if (d52::Logger* d = d52::active())
            d->ringFence("BEGIN", session_id, false);
        return false;
    }
    bitstreamSessionActive_ = true;
    bitstreamSessionId_ = session_id;
    bitstreamNextSeq_ = 0;
    d52::armSession(session_id, videoCapabilities_.build_id, ddrLayout_.doorbell_phys,
                    ddrLayout_.phys_base, this,
                    [this](uint32_t page, uint32_t off, uint32_t& v) {
                        bool other = false;
                        if (plxPageRead32(page, off, v, &other))
                            return d52::PlxRead::Ok;
                        return other ? d52::PlxRead::OtherPage : d52::PlxRead::Unavailable;
                    }); // D52-LOG (lab, MPX_D52_LOG=1): no-op when off
    return true;
}

FpgaSpi::BitstreamPushResult FpgaSpi::pushBitstreamAccessUnit(
    const BitstreamAccessUnit& au, int timeout_ms) {
    namespace ring = ddr_bitstream_ring;
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!bitstreamSessionActive_ || bitstreamAbortPending_ || bitstreamResetPending_ ||
        bitstreamProtocolVersion_ != mailbox_abi::kFpgaVideoAbiVersion ||
        au.session_id != bitstreamSessionId_ || !videoCapabilities_.supportsVideo()) {
        setErr("pushBitstreamAccessUnit: no matching negotiated FPGA video session");
        return BitstreamPushResult::Desync;
    }
    if (!ring::validAccessUnit(au, videoCapabilities_.max_au_bytes)) {
        setErr("pushBitstreamAccessUnit: invalid or oversized access unit metadata");
        return BitstreamPushResult::Fatal;
    }
    if (au.seq != bitstreamNextSeq_) {
        setErr("pushBitstreamAccessUnit: discontinuous access unit sequence");
        return BitstreamPushResult::Desync;
    }
    BitstreamStatus status;
    if (!readBitstreamStatus(status) || !status.active ||
        status.session_id != au.session_id || status.fatal || status.desync) {
        setErr("pushBitstreamAccessUnit: FPGA session lost or rejected");
        return BitstreamPushResult::Desync;
    }
    if (status.paused) {
        // Retryable (Full): after a missed resume ACK the push retries every 2 ms
        // under presentMu_, so this is a hot-path error like ring full.
        setErrHot("pushBitstreamAccessUnit: FPGA session paused");
        return BitstreamPushResult::Full;
    }
    const auto metadata = ring::encodeAccessUnitMetadata(au);
    const auto result = writeBitstreamRecord(ring::Event::AccessUnit, au.session_id,
        au.seq, 0, au.annexb, au.len, timeout_ms, metadata.data(), metadata.size());
    if (result == BitstreamPushResult::Ok)
        ++bitstreamNextSeq_;
    return result;
}

FpgaSpi::BitstreamPushResult FpgaSpi::pushBitstreamNal(const BitstreamNal& nal,
                                                       int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (bitstreamProtocolVersion_ != 0) {
        setErr("pushBitstreamNal: negotiated video requires complete access units");
        return BitstreamPushResult::Fatal;
    }
    if (!nal.annexb || !nal.len) {
        setErr("pushBitstreamNal: empty NAL");
        return BitstreamPushResult::Fatal;
    }
    BitstreamStatus st;
    if (readBitstreamStatus(st)) {
        if (st.fatal || st.desync)
            return BitstreamPushResult::Desync;
        if (!st.active || st.session_id != nal.session_id) {
            setErr("pushBitstreamNal: stale or inactive session");
            return BitstreamPushResult::Desync;
        }
    }
    return writeBitstreamRecord(ddr_bitstream_ring::Event::Nal, nal.session_id, nal.seq,
                                nal.nal_type, nal.annexb, nal.len, timeout_ms);
}

bool FpgaSpi::flushBitstreamSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!requireBitstreamSession(session_id, "flushBitstreamSession"))
        return false;
    const auto r = writeBitstreamRecord(ddr_bitstream_ring::Event::Flush, session_id, 0, 0,
                                        nullptr, 0, timeout_ms);
    if (r != BitstreamPushResult::Ok)
        return false;
    const bool ok = waitBitstreamControl(ddr_bitstream_ring::Event::Flush, session_id,
                                         bitstreamWriteCount_, timeout_ms);
    if (d52::Logger* d = d52::active())
        d->ringFence("FLUSH", session_id, ok);
    if (ok)
        bitstreamNextSeq_ = 0;
    return ok;
}

bool FpgaSpi::endBitstreamSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!requireBitstreamSession(session_id, "endBitstreamSession"))
        return false;
    if (audioSessionOwned_) {
        setErr("endBitstreamSession: quiesce producer and acknowledge audio Reset first");
        return false;
    }
    const auto r = writeBitstreamRecord(ddr_bitstream_ring::Event::End, session_id, 0, 0,
                                        nullptr, 0, timeout_ms);
    if (r != BitstreamPushResult::Ok)
        return false;
    const bool ok = waitBitstreamControl(ddr_bitstream_ring::Event::End, session_id,
                                         bitstreamWriteCount_, timeout_ms);
    if (d52::Logger* d = d52::active()) {
        d->ringFence("END", session_id, ok);
        if (ok)
            d->disarm("END");
    }
    if (ok && session_id == bitstreamLegacySessionId_)
        bitstreamLegacyActive_ = false;
    if (ok) {
        bitstreamSessionActive_ = false;
        bitstreamSessionId_ = 0;
        bitstreamNextSeq_ = 0;
        bitstreamProtocolVersion_ = 0;
        videoCapabilities_ = {};
    }
    return ok;
}

bool FpgaSpi::drainBitstreamSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!bitstreamSessionActive_ || bitstreamAbortPending_ || bitstreamResetPending_ ||
        session_id != bitstreamSessionId_ ||
        bitstreamProtocolVersion_ != mailbox_abi::kFpgaVideoAbiVersion) {
        setErr("drainBitstreamSession: no matching FPGA video session");
        return false;
    }
    const auto r = writeBitstreamRecord(ddr_bitstream_ring::Event::Drain, session_id,
                                        0, 0, nullptr, 0, timeout_ms);
    return r == BitstreamPushResult::Ok &&
           waitBitstreamControl(ddr_bitstream_ring::Event::Drain, session_id,
                                bitstreamWriteCount_, timeout_ms);
}

bool FpgaSpi::pauseBitstreamSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!requireBitstreamSession(session_id, "pauseBitstreamSession"))
        return false;
    const auto r = writeBitstreamRecord(ddr_bitstream_ring::Event::Pause, session_id, 0, 0,
                                        nullptr, 0, timeout_ms);
    if (r != BitstreamPushResult::Ok)
        return false;
    return waitBitstreamControl(ddr_bitstream_ring::Event::Pause, session_id,
                                bitstreamWriteCount_, timeout_ms);
}

bool FpgaSpi::resumeBitstreamSession(uint64_t session_id, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!requireBitstreamSession(session_id, "resumeBitstreamSession"))
        return false;
    const auto r = writeBitstreamRecord(ddr_bitstream_ring::Event::Resume, session_id, 0, 0,
                                        nullptr, 0, timeout_ms);
    if (r != BitstreamPushResult::Ok)
        return false;
    return waitBitstreamControl(ddr_bitstream_ring::Event::Resume, session_id,
                                bitstreamWriteCount_, timeout_ms);
}

bool FpgaSpi::sendBitstreamChunkDdr(const uint8_t* data, size_t len) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (!data || !len) {
        setErr("sendBitstreamChunkDdr: empty");
        return false;
    }
    if (!bitstreamLegacyActive_) {
        if (!beginBitstreamSession(bitstreamLegacySessionId_, 250))
            return false;
        bitstreamLegacyActive_ = true;
        bitstreamLegacySeq_ = 0;
    }
    BitstreamNal nal;
    nal.session_id = bitstreamLegacySessionId_;
    nal.seq = bitstreamLegacySeq_;
    nal.nal_type = len >= 5 && data[0] == 0 && data[1] == 0
                       ? static_cast<uint8_t>(data[(data[2] == 1) ? 3 : 4] & 0x1f)
                       : 0;
    nal.annexb = data;
    nal.len = len;
    const auto r = pushBitstreamNal(nal, 250);
    if (r == BitstreamPushResult::Ok) {
        ++bitstreamLegacySeq_;
        return true;
    }
    if (r == BitstreamPushResult::Full)
        setErr("sendBitstreamChunkDdr: FPGA ring full");
    else if (r == BitstreamPushResult::Desync)
        setErr("sendBitstreamChunkDdr: FPGA transport desync");
    else
        setErr("sendBitstreamChunkDdr: FPGA transport fatal");
    return false;
}

bool FpgaSpi::flushAudioFifo() {
    // Pulse status[16] (CONF_STR-invisible). Browse is PLXI cmd=5, not T[10].
    if (!setStatusBit(16, 1))
        return false;
    // brief hold so FPGA samples the bit
    usleep(2000);
    return setStatusBit(16, 0);
}

bool FpgaSpi::idleEntryStreamClear(int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(bitstreamMutex());
    if (bitstreamSessionActive_ || bitstreamLegacyActive_ || audioSessionOwned_ ||
        bitstreamResetPending_ || bitstreamAbortPending_) {
        setErr("idleEntryStreamClear: a session, ring reset or abort is active or pending");
        return false;
    }
    return resetBitstreamRing(timeout_ms);
}

bool FpgaSpi::flushBitstreamFifo() {
    bool ddr_ok = flushBitstreamDdr();
    if (!setStatusBit(11, 1)) {
        if (d52::Logger* d = d52::active())
            d->statusFlush(false);
        return ddr_ok;
    }
    usleep(2000);
    const bool ok = setStatusBit(11, 0);
    if (d52::Logger* d = d52::active())
        d->statusFlush(ok);
    return ok || ddr_ok;
}

bool FpgaSpi::getCoreStatus(uint8_t out[16]) {
    if (!ok() || !out) {
        setErr("getCoreStatus: not open");
        return false;
    }
    // Mutex + flock + Main pause so UIO_GET_STATUS is not interleaved.
    SpiExclusive guard(map_);
    if (!guard.safe()) {
        err_ = "Main busy on SPI — skipped";
        return false;
    }
    if (!gpiUserMode(map_)) {
        err_ = "FPGA not in user mode";
        return false;
    }
    err_.clear();
    if (!readStatusRaw(out))
        return false;
    err_.clear();
    return true;
}

FpgaSpi::CoreStatus FpgaSpi::parseCoreStatus(const uint8_t raw[16], const FpgaBuildInfo* build) {
    // status_in v2 (OSD-safe) — see Plex.sv. Telemetry starts at bit 16 so
    // Main status_set does not wipe OSD / DDR kick bits 12–13. Bytes 12 and 15
    // follow the negotiated build row (fpga_build_ids.hpp): telemetry as below
    // on v2 (and with no row yet), the user's echoed OSD bytes on v3 (v,10).
    //   [15:0]   OSD (ignored for telem)
    //   [23:16]  flags   [31:24] last_nal
    //   [47:32]  nalu_count
    //   [55:48]  mb0     [63:56] slice_type
    //   [71:64]  residual pack   [79:72] {ddr_busy,swap_pending,qp}
    //   [87:80]  sps_mb_w        [95:88] sps_mb_h
    //   [103:96] residual_dc  [111:104] residual_csum8
    //   [119:112] recon_sig   [127:120] recon RCA debug flags (AR may touch [122:121])
    //   Pre-3.3l-1 RBF: [127:104] was 24b bytes; pre-3.3l-2: [127:112] was 16b bytes.
    CoreStatus s{};
    const uint16_t w1 = static_cast<uint16_t>(raw[2] | (raw[3] << 8));
    const uint16_t w2 = static_cast<uint16_t>(raw[4] | (raw[5] << 8));
    const uint16_t w3 = static_cast<uint16_t>(raw[6] | (raw[7] << 8));
    const uint16_t w4 = static_cast<uint16_t>(raw[8] | (raw[9] << 8));
    const uint16_t w5 = static_cast<uint16_t>(raw[10] | (raw[11] << 8));

    const uint8_t flags = static_cast<uint8_t>(w1 & 0xFF);
    s.has_frame = (flags & 1) != 0;
    s.has_audio = (flags & 2) != 0;
    s.has_stream = (flags & 4) != 0;
    s.audio_underrun = (flags & 8) != 0;
    s.has_idr = (flags & 16) != 0;
    s.stub_busy = (flags & 32) != 0;
    s.sps_valid = (flags & 64) != 0;
    s.pps_valid = (flags & 128) != 0;
    s.last_nal_type = static_cast<uint8_t>((w1 >> 8) & 0xFF);
    s.nalu_count = w2;
    s.first_mb_type = static_cast<uint8_t>(w3 & 0xFF);
    s.slice_type = static_cast<uint8_t>((w3 >> 8) & 0xFF);
    s.stub_frames = s.slice_type;
    s.wr_count_lo = w3;

    const uint8_t res_pack = static_cast<uint8_t>(w4 & 0xFF);
    s.residual_ok = (res_pack & 0x80) != 0;
    s.residual_tc = static_cast<uint8_t>((res_pack >> 2) & 0x1F);
    s.residual_t1 = static_cast<uint8_t>(res_pack & 0x3);
    const uint8_t ddr_qp = static_cast<uint8_t>((w4 >> 8) & 0xFF);
    s.ddr_busy = (ddr_qp & 0x80) != 0;
    s.swap_pending = (ddr_qp & 0x40) != 0;
    s.slice_qp = static_cast<uint8_t>(ddr_qp & 0x3F);
    s.stream_fifo_level = 0;

    const uint8_t mb_w = static_cast<uint8_t>(w5 & 0xFF);
    const uint8_t mb_h = static_cast<uint8_t>((w5 >> 8) & 0xFF);
    s.sps_width = static_cast<uint16_t>(mb_w) * 16u;
    s.sps_height = static_cast<uint16_t>(mb_h) * 16u;

    s.stream_bytes_seen = 0;
    // [103:96]=residual_dc (raw[12]); [111:104]=residual_csum (raw[13]);
    // [119:112]=recon_sig (raw[14]); [127:120]=recon RCA flags (raw[15]).
    // residual_dc/csum/recon_sig stay below AR splice. On a row that echoes
    // raw[12] / raw[15] those bytes are OSD settings, never telemetry.
    using namespace status_telemetry;
    s.osd_byte12 = build && build->byte12Echoed;
    s.osd_byte15 = build && build->byte15Echoed;
    if (!s.osd_byte12)
        s.residual_dc = static_cast<int8_t>(raw[kResidualDcByte]);
    s.residual_csum = raw[kResidualCsumByte];
    s.recon_sig = raw[kReconSigByte];
    if (!s.osd_byte15)
        s.recon_dbg = raw[kReconDbgByte];
    if (s.osd_byte12 || s.osd_byte15) {
        const OsdStatusV3 o = decodeOsdStatusV3(raw);
        if (s.osd_byte12) {
            s.osd.h_position = o.h_position;
            s.osd.v_position = o.v_position;
        }
        if (s.osd_byte15) {
            s.osd.bob = o.bob;
            s.osd.aspect = o.aspect;
            s.osd.field_inverted = o.field_inverted;
            s.osd.crop_169 = o.crop_169;
            s.osd.top_swap = o.top_swap;
        }
    }
    // Stream byte telemetry was reclaimed for P3-3l2 silicon RCA. Publish the
    // NAL-count liveness mirror under an explicit name; keep stream_bytes_in
    // only as a deprecated in-process compatibility alias.
    s.stream_nalus = static_cast<uint32_t>(s.nalu_count);
    s.stream_bytes_in = s.stream_nalus;
    s.idr_count = s.has_idr ? 1 : 0;
    return s;
}

bool FpgaSpi::readCoreStatus(CoreStatus& out) {
    // Prefer consistent samples: same NAL liveness twice, or any stable stream with NALs.
    CoreStatus best{};
    bool have = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint8_t raw[16]{};
        if (!getCoreStatus(raw))
            return false;
        CoreStatus s = parseCoreStatus(raw, negotiatedBuild_.load());
        // Sanity: status no longer carries byte counts; nalu not huge garbage.
        bool sane = s.nalu_count < 10000 && s.stream_nalus < 10000 &&
                    s.sps_width <= 4096 && s.sps_height <= 2160;
        if (!sane) {
            usleep(10000);
            continue;
        }
        if (have && s.nalu_count == best.nalu_count && s.stream_nalus == best.stream_nalus &&
            s.has_stream == best.has_stream && s.sps_valid == best.sps_valid) {
            out = s;
            return true;
        }
        best = s;
        have = true;
        // Strong signal: stream with NALs (and optional SPS)
        if (s.has_stream && s.stream_nalus > 0 && s.nalu_count > 0) {
            out = s;
            return true;
        }
        usleep(10000);
    }
    out = best;
    return have;
}

} // namespace misterplex
