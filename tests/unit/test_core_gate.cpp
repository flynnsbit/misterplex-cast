// Core gate v2 test: core_gate.hpp and the real arm/misterplexd/fpga_spi.cpp on a fake core.
//
// fpga_spi.cpp is built with MPX_DEVMEM_PATH pointing at a sparse 4 GiB file, so its
// /dev/mem maps land there. fake_plex_core.hpp is the FPGA: the SPI handshake (each strobe
// is one SPI word) and the AU-ring control plane (reset ACK, a CAPS reply carrying its build
// ID, session ACKs). The test is Main: the FPGA-manager stat and GPO core-reset bit, Main's
// PID set, and CORENAME/RBFNAME (build-dir files; every write gets a new mtime, as a
// rewrite by a new Main does).
//
//   1. The gate state machine alone: fail closed at start; settle; probe verdicts and
//      retries; names; stale CORENAME across a reload; missed edges; closeNow; unmanaged.
//   2. Closed: 41 public FpgaSpi paths each refused, no byte of fake memory changed, no SPI
//      strobe, Main never stopped. At start, under MENU, mid-reload, after a reject.
//   3. The CAPS probe's own traffic: the ring CTRL word and one Probe record. No SPI.
//   4. Plex->Plex reloads: 480p 0x48005D45 -> 240p 0x2400F40B with CORENAME text unchanged
//      (GPO bit 30 closes the gate at the first FPGA access, before any tick; a stale
//      CORENAME holds the probe); the same RBF reloaded; a reload between two ticks;
//      45D -> 0x48005D51, which opens only where the build table allows it; an unknown
//      build; a Plex core that never answers. Every reopen runs onCoreReturned.
//   5. A running session: begin and AUs on 45D, then a reload: AU push, audio control,
//      drain, presentation read, abort and end are refused with no bytes; the session,
//      audio-write and stale-epoch checks flip; the reopen probe drops the dead session and
//      a new session begins on 240p. media_player.cpp calls those checks on those paths.
// --battery-hash: open the gate with a real probe, then hash the Plex-loaded battery on a
// fresh FpgaSpi. test_core_gate_base_diff.sh compares it with dbcc42fa's fpga_spi.cpp.
#include "fpga_spi.hpp"
#include "libmisterplex/core_gate.hpp"
#include "fake_plex_core.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#ifndef MPX_DEVMEM_PATH
#error "build with -DMPX_DEVMEM_PATH=\"<fake memory file>\""
#endif
#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex;

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

// lab-pc rule (owner; release 09:01): a test that forks a fake MiSTer Main runs in a private PID namespace with its own
// /proc (unshare --pid --fork --mount-proc), inside heavy-sim-slot. The fake-Main rows below fork a "MiSTer" process
// and read Main through /proc, so on the host they would see, and be seen by, other tests' Mains. The full run refuses
// (exit 77, VOID) unless this process is outside the host's initial PID namespace (the kernel's fixed
// PROC_PID_INIT_INO, 0xEFFFFFFC) and /proc is this namespace's own (/proc/1 shares it).
static bool privatePidNsWithOwnProc(std::string* why) {
#ifdef MUT_PIDNS_GUARD_OFF   // test-only mutant: the guard always passes
    (void)why;
    return true;
#else
    char self[64]{}, init[64]{};
    const ssize_t a = ::readlink("/proc/self/ns/pid", self, sizeof self - 1);
    if (a <= 0) {
        *why = "cannot read /proc/self/ns/pid";
        return false;
    }
    const std::string s(self, static_cast<size_t>(a));
    if (s == "pid:[4026531836]") {
        *why = "this is the host's initial PID namespace (" + s + ")";
        return false;
    }
    // Without --mount-proc, /proc/1 is the host's init: unreadable here (EACCES) or in another namespace.
    const ssize_t b = ::readlink("/proc/1/ns/pid", init, sizeof init - 1);
    if (b <= 0) {
        *why = "/proc/1/ns/pid is unreadable, so /proc is the host's, not this namespace's: add --mount-proc";
        return false;
    }
    const std::string i(init, static_cast<size_t>(b));
    if (s != i) {
        *why = "/proc is not this PID namespace's own (/proc/1 is in " + i + ", this process in " + s +
               "): add --mount-proc";
        return false;
    }
    *why = s;
    return true;
#endif
}

namespace {

namespace ring = ddr_bitstream_ring;
namespace abi = mailbox_abi;

constexpr uint64_t kFakeBytes = 0x100000000ull; // 4 GiB, sparse
constexpr uint32_t kRegBase = 0xFF000000u;
constexpr size_t kRegLen = 0x01000000u;
constexpr uint32_t kDdrBase = 0x30000000u;
constexpr size_t kDdrLen = 0x00800000u;
constexpr uint32_t kStatOff = kFpgaMgrStatPhys - kRegBase;
constexpr uint32_t kGpoOff = kFpgaMgrGpoPhys - kRegBase;
constexpr uint32_t kGpiOff = 0xFF706014u - kRegBase;
constexpr uint32_t kGpoSentinel = 0x0000A5A5u; // no strobe, core-reset bit clear
constexpr uint32_t kModeConfig = 2u;           // FPGA manager: configuration phase
constexpr uint32_t kBuild480p = 0x48005D45u;   // prod 45 MHz 480p
constexpr uint32_t kBuild240p = 0x2400F40Bu;
constexpr uint32_t kBuild5D50 = 0x48005D50u;  // prod 50 MHz 480p, on the dbcc42fa table
constexpr uint32_t kBuild5D51 = 0x48005D51u;
constexpr uint32_t kBuildUnknown = 0x12345678u;

// T-CG-FIXTURE-HOOK seam (fpga_spi.hpp, MPX_TEST_MAINSAFE_HOOK_ON): the live FakeHw, guarded by g_hookMu.
// Lock order: spiMutex -> SPI flock -> [Main stopped] -> g_hookMu -> FakeHw::mu. Nothing that holds g_hookMu
// or FakeHw::mu waits for spiMutex or the flock: the fake thread takes only mu, and under it only steps the
// core (fake_plex_core.hpp has no FpgaSpi, spiMutex or flock reference).
struct FakeHw;
std::mutex g_hookMu;
FakeHw* g_hookHw = nullptr;

struct FakeHw {
    int fd = -1;
    uint8_t* regs = nullptr;
    uint8_t* ddr = nullptr;
    std::unique_ptr<fake_core::FakePlexCore> core;
    std::mutex mu; // one core step at a time; fills and fabric changes land between steps
    std::atomic<bool> run{true};
    std::atomic<uint64_t> windows{0};
    // Set GPO bit 30 inside the next SPI window, once Main is stopped and before the daemon samples
    // GPO and reads the fabric again: Main began a load just before the daemon stopped it. The
    // daemon's own thread does it, through the seam (fpga_spi.cpp:304), so nothing depends on when
    // this fake thread runs.
    std::atomic<bool> resetOnWindow{false};
    std::atomic<uint64_t> stops{0};        // seam calls: every Main task was in 'T'
    std::atomic<uint64_t> coveredFires{0}; // seam calls while the fake thread slept (stall knob)
    std::atomic<unsigned> stallUs{0};      // MPX_CG_STALL_MS: the fake thread sleeps once, outside mu
    std::atomic<bool> stalled{false};
    std::thread thread;

    volatile uint32_t* reg(uint32_t off) { return reinterpret_cast<volatile uint32_t*>(regs + off); }

    bool start(uint32_t buildId) {
        fd = ::open(MPX_DEVMEM_PATH, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(kFakeBytes)) != 0)
            return false;
        void* r = ::mmap(nullptr, kRegLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, kRegBase);
        void* d = ::mmap(nullptr, kDdrLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, kDdrBase);
        if (r == MAP_FAILED || d == MAP_FAILED)
            return false;
        regs = static_cast<uint8_t*>(r);
        ddr = static_cast<uint8_t*>(d);
        core = std::make_unique<fake_core::FakePlexCore>(
            regs, ddr + (fake_core::kRingWindowBase - kDdrBase));
        core->reprogram(buildId);
        *reg(kStatOff) = kFpgaMgrModeUser;
        thread = std::thread([this] {
            while (run.load(std::memory_order_relaxed)) {
                if (const unsigned us = stallUs.exchange(0)) {
                    stalled.store(true);
                    std::this_thread::sleep_for(std::chrono::microseconds(us));
                    stalled.store(false);
                }
                {
                    std::lock_guard<std::mutex> lk(mu);
                    core->step();
                }
                if (FpgaSpi::mainPaused())
                    windows.fetch_add(1);
            }
        });
        hookOn();
        return true;
    }
    // Idempotent: ~FakeHw calls it too, so an early return from main can't leave the thread
    // joinable or the seam pointing at a dead FakeHw.
    void stop() {
        unhook(); // first: no seam call reaches this FakeHw once teardown starts
        run.store(false);
        if (thread.joinable())
            thread.join();
        if (regs)
            ::munmap(regs, kRegLen);
        regs = nullptr;
        if (ddr)
            ::munmap(ddr, kDdrLen);
        ddr = nullptr;
        if (fd >= 0)
            ::close(fd);
        fd = -1;
        ::unlink(MPX_DEVMEM_PATH);
    }
    ~FakeHw() { stop(); }
    void hookOn();
    void unhook();
    void onMainStopped();
    // Sentinel everywhere the daemon could write, except GPI (the core's). The fabric reads
    // live (user mode, core out of reset), and the fake core forgets its ring baseline.
    void fillSentinel() {
        std::lock_guard<std::mutex> lk(mu);
        std::memset(ddr, 0xA5, kDdrLen);
        std::memset(regs + 0x700000, 0xA5, 0x10000); // FPGA manager page and around
        *reg(kStatOff) = kFpgaMgrModeUser;
        *reg(kGpoOff) = kGpoSentinel;
        *reg(kGpiOff) = 0;
        core->reprogram(core->buildId());
    }
    void setFabric(uint32_t mode, bool coreReset) {
        std::lock_guard<std::mutex> lk(mu);
        core->setFabric(mode, coreReset);
    }
    void reprogram(uint32_t buildId) {
        std::lock_guard<std::mutex> lk(mu);
        core->reprogram(buildId);
    }
    uint64_t strobes() const { return core->strobes(); }
    uint64_t probes() const { return core->probesAnswered(); }
    uint64_t hash() const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const uint8_t* p, size_t n, size_t skipFrom, size_t skipLen) {
            for (size_t i = 0; i < n; ++i) {
                if (i >= skipFrom && i < skipFrom + skipLen)
                    continue;
                h = (h ^ p[i]) * 1099511628211ull;
            }
        };
        // MAST (the core's audio mailbox republishes it on every poll) is core-owned: no
        // hash that stands for daemon writes covers it.
        constexpr size_t kMast = abi::kAudioStatusAddr - kDdrBase;
        constexpr size_t kMastLen = abi::kAudioStatusCommitAddr + 8 - abi::kAudioStatusAddr;
        mix(ddr, kDdrLen, kMast, kMastLen);
        mix(regs + 0x700000, 0x10000, kGpiOff - 0x700000, 4);
        return h;
    }
    uint64_t regsHash() const {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < 0x10000; ++i) {
            if (i >= kGpiOff - 0x700000 && i < kGpiOff - 0x700000 + 4)
                continue;
            h = (h ^ regs[0x700000 + i]) * 1099511628211ull;
        }
        return h;
    }
};

void FakeHw::hookOn() {
    std::lock_guard<std::mutex> lk(g_hookMu);
    g_hookHw = this;
}

void FakeHw::unhook() {
#ifndef MUT_HOOK_NO_UNHOOK // test-only mutant: proves the at-exit check below bites
    std::lock_guard<std::mutex> lk(g_hookMu);
    if (g_hookHw == this)
        g_hookHw = nullptr;
#endif
}

// The seam calls this with g_hookMu held, on the daemon's thread, with every Main task stopped.
void FakeHw::onMainStopped() {
    stops.fetch_add(1);
    if (stalled.load())
        coveredFires.fetch_add(1);
    if (resetOnWindow.exchange(false)) {
        std::lock_guard<std::mutex> lk(mu);
        core->setFabric(kFpgaMgrModeUser, true);
    }
}

// Every return from main has unhooked (stop() or ~FakeHw run first); a seam pointer left at exit
// would point at a dead FakeHw.
struct HookExitCheck {
    ~HookExitCheck() {
        if (g_hookHw) {
            std::fprintf(stderr, "FAIL hook dangling at exit\n");
            std::fflush(nullptr); std::_Exit(3); // fflush: _Exit skips stdio's flush; same line, so no line moves
        }
    }
} g_hookExitCheck;

} // namespace

namespace misterplex {
// The MPX_TEST_MAINSAFE_HOOK seam target (fpga_spi.cpp:304).
void testMainSafeHook() {
    std::lock_guard<std::mutex> lk(g_hookMu);
    if (g_hookHw)
        g_hookHw->onMainStopped();
}
} // namespace misterplex

namespace {

// Main as the gate sees it: its identity and the two files SetUARTMode writes.
struct MainSim {
    std::string cn = std::string(MPX_DEVMEM_PATH) + ".CORENAME";
    std::string rbf = std::string(MPX_DEVMEM_PATH) + ".RBFNAME";
    MainIdentity main{1001, 1};
    // Main's start on the wall clock: names written after it are this Main's.
    int64_t mainStartWallNs = (1700000000LL - 100) * 1000000000LL;
    int64_t bootWallNs = (1700000000LL - 100000) * 1000000000LL;
    int nextPid = 1002;
    uint64_t nextStart = 2;
    int64_t stampSec = 1700000000;
    int64_t skewUs = 0;

    void put(const std::string& path, const char* text) {
        if (FILE* fp = std::fopen(path.c_str(), "w")) {
            std::fputs(text, fp);
            std::fclose(fp);
        }
        const struct timespec ts[2] = {{static_cast<time_t>(stampSec), 0},
                                       {static_cast<time_t>(stampSec), 0}};
        ++stampSec;
        ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
    }
    void names(const char* core, const char* rbfName) {
        put(cn, core);
        put(rbf, rbfName);
    }
    // A new Main starts now: names written from here on are its own; older ones are stale.
    void newMain() {
        main = {nextPid++, nextStart++};
        mainStartWallNs = stampSec * 1000000000LL - 1;
    }
    int64_t now() const { return coreGateNowUs() + skewUs; }
    void advance(int64_t us) { skewUs += us; }
    void cleanup() {
        ::unlink(cn.c_str());
        ::unlink(rbf.c_str());
    }
};

std::vector<std::string> g_gateLog;
bool g_quietGateLog = false;   // review 09:03 C1 row: 200 reopen cycles keep their lines, not print them

void flushGateLogs() {
    for (auto& line : CoreGate::process().takeLogs()) {
        if (!g_quietGateLog)
            std::printf("      %s\n", line.c_str());
        g_gateLog.push_back(line);
    }
}

int gateLogCount(const char* needle) {
    int n = 0;
    for (const auto& line : g_gateLog)
        n += line.find(needle) != std::string::npos;
    return n;
}

#ifndef MPX_CORE_GATE_HASH_ONLY
// The daemon's idle tick: coreGateTickStep, the sequence MediaPlayer::coreGateTick runs,
// with the real FpgaSpi probe and onCoreReturned. The repaint is counted, not drawn, so
// probe ticks show the probe's bytes alone; openPhase paints.
bool g_returnPending = false;
int g_repaints = 0;

CoreGateEvent tick(FpgaSpi& f, MainSim& m) {
    CoreGateTickHooks h;
    h.observe = [&] {
        CoreObservation o;
        o.fabric = f.fabricState();
        o.main = m.main;
        o.mainStartWallNs = m.mainStartWallNs;
        o.bootWallNs = m.bootWallNs;
        o.coreName = stampCoreFile(m.cn.c_str());
        o.rbfName = stampCoreFile(m.rbf.c_str());
        o.nowUs = m.now();
        return o;
    };
    h.playing = [] { return false; };
    h.probe = [&] { return f.runCoreGateProbe(200, m.now()); };
    h.coreReturned = [&] { f.onCoreReturned(); };
    h.finishReturn = [] {};
    h.repaint = [] { ++g_repaints; };
    h.log = [](const std::string& line) {
        if (!g_quietGateLog)
            std::printf("      %s\n", line.c_str());
        g_gateLog.push_back(line);
    };
    const CoreGateEvent e = coreGateTickStep(CoreGate::process(), h, g_returnPending);
    flushGateLogs();
    return e;
}

const char* eventName(CoreGateEvent e) {
    switch (e) {
    case CoreGateEvent::None: return "none";
    case CoreGateEvent::Closed: return "closed";
    case CoreGateEvent::ProbeWanted: return "probe";
    case CoreGateEvent::Opened: return "opened";
    case CoreGateEvent::Rejected: return "rejected";
    }
    return "?";
}
#endif // MPX_CORE_GATE_HASH_ONLY

struct Battery {
    int calls = 0, refused = 0, traffic = 0;
    double ms = 0;
    std::vector<std::string> trafficNames;
};

// Every public FpgaSpi path that can reach DDR, the doorbell/mailbox page, the bitstream
// ring or SPI, each run on its own with a before/after hash of the fake memory and the
// strobe count, so traffic is attributed per call.
Battery runAllPaths(FpgaSpi& f, FakeHw& hw) {
    Battery b;
    std::vector<uint8_t> yuv(640 * 480 * 3 / 2, 0x40);
    const DdrFrameGeometry g = ddrFrameGeometryForPresentedSize(640, 480);
    uint8_t raw[16]{}, word[16]{};
    std::string text;
    PlaybackCommand cmd = PlaybackCommand::None;
    uint16_t osd = 0;
    FpgaSpi::DdrDoorbellStatus dbs;
    FrameStoreStatus fss;
    BankReleaseStatus brs;
    SourceAspectAck saa;
    FpgaSpi::BitstreamStatus bs;
    FpgaSpi::VideoCapabilities caps;
    FpgaSpi::VideoPresentation vp;
    FpgaSpi::AudioSessionStatus as;
    FpgaSpi::CoreStatus cs;
    SourceAspect aspect;
    aspect.x = 4;
    aspect.y = 3;
    aspect.valid = true;
    const int pairs[2] = {16, 1};
    int outBank = -1;
    uint8_t buf[64]{};
    FpgaSpi::BitstreamAccessUnit au;
    au.session_id = 1;
    au.seq = 0;
    au.annexb = buf;
    au.len = sizeof buf;
    au.pts = 0;
    au.timebase_num = 1;
    au.timebase_den = 90000;
    FpgaSpi::BitstreamNal nal;
    nal.session_id = 1;
    nal.annexb = buf;
    nal.len = sizeof buf;
    auto frameBytes = [&] {
        return f.ddrFrameLayout().frame_bytes ? f.ddrFrameLayout().frame_bytes : yuv.size();
    };
    using Push = FpgaSpi::BitstreamPushResult;
    const std::vector<std::pair<const char*, std::function<bool()>>> paths = {
        {"sendYuv420pFrameDdr(640x480)",
         [&] { return f.sendYuv420pFrameDdr(yuv.data(), yuv.size(), 640, 480, 0); }},
        {"sendYuv420pFrameDdr(geometry)",
         [&] { return f.sendYuv420pFrameDdr(yuv.data(), yuv.size(), g, 1); }},
        {"blitDisplayedBank", [&] { return f.blitDisplayedBank(yuv.data(), yuv.size()); }},
        {"ddrBankVirt", [&] { return f.ddrBankVirt(0) != nullptr; }},
        {"beginDdrBankIngest",
         [&] { return f.beginDdrBankIngest(frameBytes(), 0, outBank) != nullptr; }},
        {"commitDdrBankIngest", [&] { return f.commitDdrBankIngest(0, frameBytes()); }},
        {"readDdrDoorbellStatus", [&] { return f.readDdrDoorbellStatus(dbs); }},
        {"readFrameStoreStatus", [&] { return f.readFrameStoreStatus(fss); }},
        {"readBankRelease", [&] { return f.readBankRelease(brs); }},
        {"readSourceAspectAck", [&] { return f.readSourceAspectAck(saa); }},
        {"readOsdMailbox", [&] { return f.readOsdMailbox(osd); }},
        {"readInputMailbox", [&] { return f.readInputMailbox(cmd); }},
#ifndef MPX_CORE_GATE_HASH_ONLY // the base daemon (CORE_GATE_BASE_REF) has no plxPageRead32
        {"plxPageRead32", [&] {
             uint32_t v = 0;
             return f.plxPageRead32(0x118, v);
         }},
#endif
        {"sendFileTx", [&] { return f.sendFileTx(buf, sizeof buf, 1); }},
        {"sendRgb565Bytes", [&] { return f.sendRgb565Bytes(buf, sizeof buf, 1); }},
        {"sendPcmChunk", [&] { return f.sendPcmChunk(buf, sizeof buf); }},
        {"sendBitstreamChunk", [&] { return f.sendBitstreamChunk(buf, sizeof buf); }},
        {"sendSourceAspect", [&] { return f.sendSourceAspect(aspect); }},
        {"beginFpgaVideoSession", [&] { return f.beginFpgaVideoSession(1, caps, 50); }},
        {"pushBitstreamAccessUnit",
         [&] { return f.pushBitstreamAccessUnit(au, 50) == Push::Ok; }},
        {"drainBitstreamSession", [&] { return f.drainBitstreamSession(1, 50); }},
        {"readVideoPresentation", [&] { return f.readVideoPresentation(1, vp); }},
        {"controlAudioSession",
         [&] { return f.controlAudioSession(1, FpgaSpi::AudioControl::Begin, 50); }},
        {"readAudioSessionStatus", [&] { return f.readAudioSessionStatus(1, as); }},
        {"abortFpgaVideoSession", [&] { return f.abortFpgaVideoSession(1, 50); }},
        {"beginBitstreamSession", [&] { return f.beginBitstreamSession(1, 50); }},
        {"pushBitstreamNal", [&] { return f.pushBitstreamNal(nal, 50) == Push::Ok; }},
        {"flushBitstreamSession", [&] { return f.flushBitstreamSession(1, 50); }},
        {"pauseBitstreamSession", [&] { return f.pauseBitstreamSession(1, 50); }},
        {"resumeBitstreamSession", [&] { return f.resumeBitstreamSession(1, 50); }},
        {"endBitstreamSession", [&] { return f.endBitstreamSession(1, 50); }},
        {"readBitstreamStatus", [&] { return f.readBitstreamStatus(bs); }},
        {"sendBitstreamChunkDdr", [&] { return f.sendBitstreamChunkDdr(buf, sizeof buf); }},
        {"flushBitstreamDdr", [&] { return f.flushBitstreamDdr(); }},
        {"flushAudioFifo", [&] { return f.flushAudioFifo(); }},
        {"flushBitstreamFifo", [&] { return f.flushBitstreamFifo(); }},
        {"setStatusBit", [&] { return f.setStatusBit(16, 1); }},
        {"setStatusWord", [&] { return f.setStatusWord(word); }},
        {"setStatusBits", [&] { return f.setStatusBits(pairs, 1); }},
        {"getCoreStatus", [&] { return f.getCoreStatus(raw); }},
        {"readCoreStatus", [&] { return f.readCoreStatus(cs); }},
        {"getConfigString", [&] { return f.getConfigString(text); }},
    };
    for (const auto& [name, call] : paths) {
        const uint64_t h0 = hw.hash();
        const uint64_t s0 = hw.strobes();
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = call();
        b.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
        ++b.calls;
        if (!ok)
            ++b.refused;
        if (hw.hash() != h0 || hw.strobes() != s0) {
            ++b.traffic;
            b.trafficNames.push_back(name);
        }
    }
    return b;
}

// What the daemon does with Plex loaded and nothing playing: idle paint (bank +
// doorbell), the input mailbox poll, a status bit (SPI) and a ring flush.
void runOpenPaths(FpgaSpi& f) {
    std::vector<uint8_t> yuv(640 * 480 * 3 / 2);
    for (size_t i = 0; i < yuv.size(); ++i)
        yuv[i] = static_cast<uint8_t>(i * 7);
    PlaybackCommand cmd = PlaybackCommand::None;
    (void)f.sendYuv420pFrameDdr(yuv.data(), yuv.size(), 640, 480, 0);
    (void)f.readInputMailbox(cmd);
    (void)f.setStatusBit(16, 1);
    (void)f.flushBitstreamDdr();
}

#ifndef MPX_CORE_GATE_HASH_ONLY

// review 11:36 R3 (R1 packet D4): the MPX_BROWSE_V2 input and OSD poll threads read through
// coreGateAllowsQuiet. With the gate closed they read nothing AND stay quiet (fpga_spi.hpp: "a closed
// gate reads nothing ... no setErr"): lastError() keeps a loud call's error across 8 polls of each,
// so the mapper's own (loud) gate is never reached. U42 (the quiet gate ignores a closed gate) is RED.
void quietReadersClosed(FpgaSpi& f) {
    std::string text;
    (void)f.getConfigString(text);   // a loud refusal: its error must survive the quiet polls
    const std::string before = f.lastError();
    CHECK(!before.empty() && before.find("ensureDdrMailboxMap") == std::string::npos);
    int got = 0;
    for (int i = 0; i < 8; ++i) {
        PlaybackCommand cmd = PlaybackCommand::PlayPause;
        uint16_t osd = 0xBEEF;
        if (f.readInputMailboxDirect(cmd, kBrowseMaxInputCommand) || cmd != PlaybackCommand::None)
            ++got;
        if (f.readOsdMailboxDirect(osd))
            ++got;
    }
    const bool quiet = f.lastError() == before;
    std::printf("  %-34s 8 polls each: %d read, lastError %s\n", "quiet readers, gate closed", got,
                quiet ? "unchanged" : ("CHANGED to '" + f.lastError() + "'").c_str());
    CHECK(got == 0);
    CHECK(quiet);
}

// Closed: every path refused, no byte, no strobe, no Main window, no probe answered.
// fill=false keeps the caller's sentinel and fabric (a reload already under way).
void closedPhase(const char* label, FpgaSpi& f, FakeHw& hw, bool fill = true) {
    if (fill)
        hw.fillSentinel();
    const uint64_t before = hw.hash();
    const uint64_t strobes = hw.strobes(), windows = hw.windows.load(), probes = hw.probes();
    // Doorbell-first kick mode, as on every play and after onCoreReturned: the fake core
    // never answers a doorbell, which otherwise leaves the SPI kick.
    f.reprobeDdrKick();
    const Battery b = runAllPaths(f, hw);
    runOpenPaths(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t after = hw.hash();
    const uint64_t dStrobes = hw.strobes() - strobes;
    const uint64_t dWindows = hw.windows.load() - windows;
    std::printf("  %-34s calls=%d refused=%d with traffic=%d, %.1f ms, bytes changed=%s, "
                "spi strobes=%llu, main windows=%llu\n",
                label, b.calls, b.refused, b.traffic, b.ms, after == before ? "no" : "YES",
                static_cast<unsigned long long>(dStrobes),
                static_cast<unsigned long long>(dWindows));
    for (const auto& name : b.trafficNames)
        std::fprintf(stderr, "  traffic with the gate closed: %s\n", name.c_str());
    CHECK(b.refused == b.calls);
    CHECK(b.traffic == 0);
    CHECK(after == before);
    CHECK(dStrobes == 0 && dWindows == 0);
    CHECK(hw.probes() == probes);
    CHECK(b.ms < 100.0); // nothing waited on hardware
    CHECK(!CoreGate::process().open());
    quietReadersClosed(f);
}

void openPhase(const char* label, FpgaSpi& f, FakeHw& hw) {
    CHECK(CoreGate::process().open());
    hw.fillSentinel();
    const uint64_t before = hw.hash();
    const uint64_t strobes = hw.strobes();
    f.reprobeDdrKick();
    runOpenPaths(f);
    bool bankWritten = false;
    for (size_t i = 0; i < 640 * 480 && !bankWritten; ++i)
        bankWritten = hw.ddr[i] != 0xA5;
    const uint64_t dStrobes = hw.strobes() - strobes;
    std::printf("  %-34s bank written=%s, bytes changed=%s, spi strobes=%llu\n", label,
                bankWritten ? "yes" : "NO", hw.hash() != before ? "yes" : "NO",
                static_cast<unsigned long long>(dStrobes));
    CHECK(bankWritten);
    CHECK(hw.hash() != before);
    CHECK(dStrobes > 0);
}

// A tick that probes, with the probe's traffic attributed byte by byte: the daemon may
// write only the ring CTRL word and one 32-byte record at the ring head; the core writes
// its READ/ERR/STAT words and CAPS. Nothing else, no SPI, no Main window.
CoreGateEvent probeTick(const char* label, FpgaSpi& f, MainSim& m, FakeHw& hw) {
    hw.fillSentinel();
    const std::vector<uint8_t> before(hw.ddr, hw.ddr + kDdrLen);
    const uint64_t regs0 = hw.regsHash();
    const uint64_t s0 = hw.strobes(), w0 = hw.windows.load(), p0 = hw.probes();
    const CoreGateEvent e = tick(f, m);
    size_t daemonBytes = 0, coreBytes = 0, mastBytes = 0, otherBytes = 0;
    uint32_t firstOther = 0;
    for (size_t i = 0; i < kDdrLen; ++i) {
        if (hw.ddr[i] == before[i])
            continue;
        const uint32_t phys = kDdrBase + static_cast<uint32_t>(i);
        if ((phys >= ring::kCtrlPhys && phys < ring::kCtrlPhys + 8) ||
            (phys >= ring::kDataPhys && phys < ring::kDataPhys + ring::kRecordHeaderBytes))
            ++daemonBytes;
        else if (phys >= ring::kReadPhys && phys < abi::kVideoCapsCommitAddr + 8)
            ++coreBytes;
        else if (phys >= abi::kAudioStatusAddr && phys < abi::kAudioStatusCommitAddr + 8)
            ++mastBytes; // the core's audio mailbox republishes MAST on every poll
        else if (otherBytes++ == 0)
            firstOther = phys;
    }
    const uint8_t* rec = hw.ddr + (ring::kDataPhys - kDdrBase);
    uint32_t magic = 0;
    std::memcpy(&magic, rec, 4);
    const bool recordWritten = magic == ring::kRecordMagic;
    std::printf("  %-34s tick=%s; daemon bytes=%zu (ring CTRL + Probe record%s), core bytes=%zu "
                "(+%zu MAST), other bytes=%zu, spi strobes=%llu, main windows=%llu, probes "
                "answered=%llu\n",
                label, eventName(e), daemonBytes, recordWritten ? "" : ": no record",
                coreBytes, mastBytes, otherBytes, static_cast<unsigned long long>(hw.strobes() - s0),
                static_cast<unsigned long long>(hw.windows.load() - w0),
                static_cast<unsigned long long>(hw.probes() - p0));
    if (otherBytes)
        std::fprintf(stderr, "  probe wrote outside the ring head/CTRL: first at 0x%08X\n",
                     firstOther);
    CHECK(otherBytes == 0);
    CHECK(daemonBytes > 0);
    if (recordWritten) {
        CHECK(rec[4] == static_cast<uint8_t>(ring::Event::Probe));
        uint64_t nonce = 0;
        std::memcpy(&nonce, rec + 8, 8);
        if (hw.probes() > p0) {
            uint64_t lo = 0, hi = 0;
            std::memcpy(&lo, hw.ddr + (abi::kVideoNonceLowAddr - kDdrBase), 8);
            std::memcpy(&hi, hw.ddr + (abi::kVideoNonceHighAddr - kDdrBase), 8);
            CHECK(((lo >> 32) | (hi & 0xffffffff00000000ull)) == nonce);
        }
    }
    CHECK(hw.regsHash() == regs0);
    CHECK(hw.strobes() == s0);
    CHECK(hw.windows.load() == w0);
    return e;
}

// Main loads a core (fpga_io.cpp fpga_load_rbf, then app_restart): GPO bit 30, the FPGA
// manager through configuration, a new Main that clears GPO, and last SetUARTMode's
// CORENAME/RBFNAME. Returns the verdict of the probe that follows.
//   batteryFirst: the daemon's threads keep calling FpgaSpi after bit 30 and before any
//                 tick; the first call must close the gate and none may write.
//   staleUs:      the new Main is up and the fabric is live, but CORENAME is still the
//                 old core's write for this long; no probe may run.
CoreGateEvent reload(const char* label, FpgaSpi& f, MainSim& m, FakeHw& hw, uint32_t newBuild,
                     const char* core, const char* rbfName, bool batteryFirst, int64_t staleUs) {
    CoreGate& g = CoreGate::process();
    std::printf("  -- %s\n", label);
    const bool wasOpen = g.open();
    if (batteryFirst) {
        hw.fillSentinel();
        hw.setFabric(kFpgaMgrModeUser, true);
        closedPhase("reload began, before any tick", f, hw, false);
        flushGateLogs();
        if (wasOpen)
            CHECK(gateLogCount("core gate closed: core held in reset (GPO bit 30)") > 0);
    } else {
        hw.setFabric(kFpgaMgrModeUser, true);
    }
    const CoreGateEvent first = tick(f, m);
    CHECK(first == ((wasOpen && !batteryFirst) ? CoreGateEvent::Closed : CoreGateEvent::None));
    CHECK(!g.open());
    hw.setFabric(kModeConfig, true);
    hw.reprogram(0); // nothing answers while the fabric is configured
    m.advance(200000);
    CHECK(tick(f, m) == CoreGateEvent::None);
    hw.setFabric(kFpgaMgrModeUser, true);
    m.advance(200000);
    CHECK(tick(f, m) == CoreGateEvent::None);
    m.newMain();
    hw.setFabric(kFpgaMgrModeUser, false);
    hw.reprogram(newBuild);
    m.advance(200000);
    CHECK(tick(f, m) == CoreGateEvent::None);
    if (staleUs > 0) {
        hw.fillSentinel();
        const int waits = gateLogCount("waiting for the new Main to rewrite CORENAME");
        const uint64_t h0 = hw.hash(), s0 = hw.strobes(), p0 = hw.probes();
        bool probed = false;
        for (int64_t t = 0; t < staleUs; t += 200000) {
            m.advance(200000);
            if (tick(f, m) != CoreGateEvent::None)
                probed = true;
        }
        std::printf("  %-34s %.1f s with the old CORENAME: probe=%s, bytes changed=%s, spi "
                    "strobes=%llu\n",
                    "stale CORENAME", staleUs / 1e6, probed ? "YES" : "no",
                    hw.hash() == h0 ? "no" : "YES",
                    static_cast<unsigned long long>(hw.strobes() - s0));
        CHECK(!probed && hw.hash() == h0 && hw.strobes() == s0 && hw.probes() == p0);
        CHECK(!g.open());
        CHECK(gateLogCount("waiting for the new Main to rewrite CORENAME") == waits);
    }
    m.names(core, rbfName);
    CHECK(tick(f, m) == CoreGateEvent::None); // settle starts at the rewrite
    m.advance(300000);
    CHECK(tick(f, m) == CoreGateEvent::None);
    m.advance(300000);
    return probeTick("probe after the reload", f, m, hw);
}

// CORENAME rewritten with the fabric live (as after any reload): closed if open, then
// verified again by one probe.
void reopen(FpgaSpi& f, MainSim& m) {
    m.put(m.cn, "Plex");
    (void)tick(f, m);
    m.advance(CoreGate::kSettleUs);
    CHECK(tick(f, m) == CoreGateEvent::Opened);
}

// ---------------------------------------------------------------- 1. state machine

struct Sim {
    FabricState fabric{true, kFpgaMgrModeUser, false};
    MainIdentity main{100, 100};
    int64_t mainStart = 500; // names (mtime 1000) were written after this Main started
    int64_t boot = 200;      // boot on today's clock: every name here was written after it
    CoreFileStamp cn{true, "Plex", 1000};
    CoreFileStamp rbf{true, "Plex_480p", 1000};
    int64_t now = 5000000000;
    CoreObservation obs() const {
        CoreObservation o;
        o.fabric = fabric;
        o.main = main;
        o.mainStartWallNs = mainStart;
        o.bootWallNs = boot;
        o.coreName = cn;
        o.rbfName = rbf;
        o.nowUs = now;
        return o;
    }
    void rewrite(const char* name, const char* rbfName = nullptr) {
        cn = {true, name, cn.mtimeNs + 1000};
        if (rbfName)
            rbf = {true, rbfName, rbf.mtimeNs + 1000};
    }
};

int logLines(CoreGate& g, const char* needle, std::vector<std::string>* all = nullptr) {
    int n = 0;
    for (const auto& line : g.takeLogs()) {
        n += line.find(needle) != std::string::npos;
        if (all)
            all->push_back(line);
    }
    return n;
}

// Ticks every 200 ms for us: true if any tick asked for a probe.
bool anyProbe(CoreGate& g, Sim& s, int64_t us) {
    bool probe = false;
    for (int64_t t = 0; t < us; t += 200000) {
        s.now += 200000;
        probe |= g.observe(s.obs()) == CoreGateEvent::ProbeWanted;
    }
    return probe;
}

void stateMachine() {
    using E = CoreGateEvent;
    {
        CoreGate g; // what the daemon gets: managed, closed, before any look
        CHECK(g.managed() && !g.open() && g.state() == CoreGateState::Closed);
        Sim s;
        CHECK(g.observe(s.obs()) == E::None); // first look: the settle starts here
        s.now += CoreGate::kSettleUs - 1;
        CHECK(g.observe(s.obs()) == E::None && !g.open());
        s.now += 1;
        CHECK(g.observe(s.obs()) == E::ProbeWanted && g.state() == CoreGateState::Verifying);
        CHECK(g.observe(s.obs()) == E::ProbeWanted && !g.open()); // until the verdict
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);
        CHECK(g.open() && g.buildId() == kBuild480p && g.changes() == 1);
        CHECK(logLines(g, "core gate open: 'Plex' build 0x48005D45 (row) verified by CAPS") == 1);
        CHECK(!anyProbe(g, s, 2000000) && g.open());
        CHECK(g.takeLogs().empty()); // nothing per tick

        // Plex 480p 45D -> Plex 480p 5D50, CORENAME text unchanged. Main sets GPO bit 30
        // first.
        s.fabric.coreReset = true;
        CHECK(g.observe(s.obs()) == E::Closed && !g.open() && g.changes() == 2);
        CHECK(logLines(g, "core gate closed: core held in reset (GPO bit 30)") == 1);
        s.fabric.mode = kModeConfig;
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::None);
        s.fabric.mode = kFpgaMgrModeUser;
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::None);
        s.fabric.coreReset = false; // the new Main's fpga_io_init
        s.main = {200, 200};
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::None);
        CHECK(!anyProbe(g, s, 10000000)); // 10 s with the old CORENAME: never asked
        CHECK(logLines(g, "waiting for the new Main to rewrite CORENAME") == 1);
        s.rewrite("Plex", "Plex_480p_5D50");
        CHECK(g.observe(s.obs()) == E::None); // settle restarts at the rewrite
        s.now += CoreGate::kSettleUs;
        CHECK(g.observe(s.obs()) == E::ProbeWanted);
        CHECK(g.probeResult(true, kBuild5D50, "", "480p", "row", s.now) == E::Opened);
        CHECK(g.buildId() == kBuild5D50 && g.changes() == 3);
        CHECK(g.takeRePair());
        CHECK(g.lastOpenId() == kBuild5D50);
        CHECK(g.pairingErrorText().empty());
        CHECK(logLines(g, "core gate re-read build 0x48005D50 raster 480p") == 1);

        // The whole reload between two ticks: only the PID set and the names moved.
        s.main = {300, 300};
        s.rewrite("Plex", "Plex_480p");
        CHECK(g.observe(s.obs()) == E::Closed && !g.open());
        CHECK(logLines(g, "core gate closed: Main restarted (pid 200 -> 300)") == 1);
        s.now += CoreGate::kSettleUs;
        CHECK(g.observe(s.obs()) == E::ProbeWanted);
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);

        // A new Main seen before its CORENAME write: the old name is stale.
        s.main = {400, 400};
        CHECK(g.observe(s.obs()) == E::Closed);
        CHECK(!anyProbe(g, s, 3000000));
        s.rewrite("Plex");
        CHECK(!anyProbe(g, s, CoreGate::kSettleUs));
        CHECK(anyProbe(g, s, 200000));
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);

        // A CORENAME rewrite with no reload (an OSD UART/MIDI change calls SetUARTMode):
        // closed, then verified again.
        s.rewrite("Plex");
        CHECK(g.observe(s.obs()) == E::Closed);
        s.now += CoreGate::kSettleUs;
        CHECK(g.observe(s.obs()) == E::ProbeWanted);
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);
        (void)g.takeLogs();

        // A choke point read the fabric as not live: closed at once; the stale name rule
        // applies, as for any reload.
        g.closeNow("FPGA manager mode 2 (4 = user): reprogramming", s.now);
        CHECK(!g.open() && g.state() == CoreGateState::Closed);
        CHECK(logLines(g, "core gate closed: FPGA manager mode 2") == 1);
        s.fabric.mode = kModeConfig;
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::None);
        s.fabric.mode = kFpgaMgrModeUser;
        s.main = {500, 500};
        CHECK(!anyProbe(g, s, 3000000));
        s.rewrite("Plex");
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));

        // A reload while the probe runs: the verdict is dropped.
        g.closeNow("core held in reset (GPO bit 30): Main is loading a core", s.now);
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::None && !g.open());
    }
    {
        // Names: none of these is ever asked about, however long.
        struct Case {
            const char* label;
            bool exists;
            const char* name;
            const char* hold;
        };
        const Case cases[] = {
            {"no CORENAME", false, "", "CORENAME empty or unreadable"},
            {"empty CORENAME", true, "", "CORENAME empty or unreadable"},
            {"MENU", true, "MENU", "loaded core 'MENU' is not Plex"},
            {"lower case", true, "plex", "loaded core 'plex' is not Plex"},
            {"Plex_480i", true, "Plex_480i", "loaded core 'Plex_480i' is not Plex"},
        };
        for (const Case& c : cases) {
            CoreGate g;
            Sim s;
            s.cn = {c.exists, c.name, 1000};
            CHECK(!anyProbe(g, s, 5000000) && !g.open());
            const int holds = logLines(g, c.hold);
            if (holds != 1)
                std::fprintf(stderr, "  name case '%s': %d hold lines\n", c.label, holds);
            CHECK(holds == 1);
        }
        CoreGate g;
        Sim s;
        s.main = {};
        CHECK(!anyProbe(g, s, 3000000) && logLines(g, "Main is not running") == 1);
        CoreGate r;
        Sim t;
        t.rbf = {false, "", 0};
        CHECK(!anyProbe(r, t, 3000000) && logLines(r, "RBFNAME empty or unreadable") == 1);
        CoreGate u;
        Sim v;
        v.cn = {true, "PLEX", 1000};
        CHECK(anyProbe(u, v, CoreGate::kSettleUs + 200000));
    }
    {
        // The daemon starts while the FPGA is being configured: the CORENAME it finds is
        // the old core's.
        CoreGate g;
        Sim s;
        s.fabric.mode = kModeConfig;
        CHECK(g.observe(s.obs()) == E::None);
        s.fabric.mode = kFpgaMgrModeUser;
        CHECK(!anyProbe(g, s, 5000000));
        CHECK(logLines(g, "waiting for the new Main to rewrite CORENAME") == 1);
        s.rewrite("Plex");
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
    }
    {
        // (a) The daemon starts on a live fabric, under a Main that has not written its
        // names yet: CORENAME 'Plex' (the last core's) is older than this Main. Never asked
        // about, however long, until this Main writes the names.
        struct Case {
            const char* label;
            int64_t cnMtime, rbfMtime, mainStart;
        };
        const Case cases[] = {
            {"both names older than Main", 1000, 1000, 5000},
            {"RBFNAME older than Main", 6000, 1000, 5000},
            {"CORENAME older than Main", 1000, 6000, 5000},
            {"Main start unknown", 6000, 6000, 0},
        };
        for (const Case& c : cases) {
            CoreGate g;
            Sim s;
            s.cn = {true, "Plex", c.cnMtime};
            s.rbf = {true, "Plex", c.rbfMtime};
            s.mainStart = c.mainStart;
            const bool probed = anyProbe(g, s, 10000000);
            const int holds = logLines(g, "CORENAME/RBFNAME predate Main pid 100 (the last "
                                          "core's names); waiting for Main to write them");
            if (probed || holds != 1)
                std::fprintf(stderr, "  stale-name case '%s': probe=%d holds=%d\n", c.label,
                             probed, holds);
            CHECK(!probed && holds == 1 && !g.open());
            s.cn = {true, "Plex", s.cn.mtimeNs + 10000}; // this Main writes its names
            s.rbf = {true, "Plex", s.rbf.mtimeNs + 10000};
            CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        }
        // Names written at or after Main's start (the bound is inclusive): asked at once.
        CoreGate g;
        Sim s;
        s.cn = {true, "Plex", 5000};
        s.rbf = {true, "Plex", 5000};
        s.mainStart = 5000;
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(logLines(g, "predate") == 0);
        // Only the first look asks: later, a reload edge is the evidence.
        CoreGate h;
        Sim t;
        CHECK(anyProbe(h, t, CoreGate::kSettleUs + 200000));
        CHECK(h.probeResult(true, kBuild480p, "", "480p", "row", t.now) == E::Opened);
        t.mainStart = 99999; // e.g. a clock step: no effect once running
        CHECK(!anyProbe(h, t, 2000000) && h.open());
    }
    {
        // (a) and the boot clock step. An RTC-less MiSTer boots at 1970 and ntpd -g steps
        // it to today once the network is up (a lab MiSTer: messages.0.229:269, :430). btime moves
        // with the step, so names the boot core's Main wrote before it predate boot on
        // today's clock, which no /tmp (tmpfs) file can: not comparable. They do not hold;
        // the CAPS probe decides, as before v2.1. A comparable stale name still holds.
        struct Case {
            const char* label;
            int64_t cn, rbf, boot;
            bool held;
        };
        const Case cases[] = {
            {"both names written before the step", 100, 100, 300, false},
            {"CORENAME before the step, RBFNAME fresh", 100, 6000, 300, false},
            {"CORENAME before the step, RBFNAME stale", 100, 1000, 300, true},
            {"RBFNAME before the step, CORENAME stale", 1000, 100, 300, true},
            {"boot unknown: no step can be seen", 100, 100, -1, true},
            // Before ntpd's step (or with no NTP at all) btime is 0: a real boot time.
            {"btime 0 (RTC-less, before the step), names after Main", 6000, 6000, 0, false},
            {"btime 0 (RTC-less, before the step), names before Main", 1000, 1000, 0, true},
        };
        for (const Case& c : cases) {
            CoreGate g;
            Sim s;
            s.mainStart = 5000;
            s.boot = c.boot;
            s.cn = {true, "Plex", c.cn};
            s.rbf = {true, "Plex", c.rbf};
            const bool probed = anyProbe(g, s, c.held ? 10000000 : CoreGate::kSettleUs + 200000);
            const int holds = logLines(g, "predate Main pid");
            if (probed == c.held || holds != (c.held ? 1 : 0))
                std::fprintf(stderr, "  clock-step case '%s': probe=%d holds=%d\n", c.label,
                             probed, holds);
            CHECK(probed != c.held && holds == (c.held ? 1 : 0));
        }
    }
    {
        // (b) RBFNAME must name a Plex core: the CONF_STR name Main writes ("Plex"), or an
        // RBF file name whose basename, less .rbf, starts with Plex.
        CHECK(rbfNameIsPlex("Plex") && rbfNameIsPlex("PLEX") && rbfNameIsPlex("plex"));
        CHECK(rbfNameIsPlex("Plex_240p") && rbfNameIsPlex("Plex_480p.rbf") &&
              rbfNameIsPlex("PLEX.RBF") && rbfNameIsPlex("/media/fat/_Utility/Plex_240p.rbf"));
        CHECK(!rbfNameIsPlex("") && !rbfNameIsPlex("MENU") && !rbfNameIsPlex("NES") &&
              !rbfNameIsPlex(".rbf") && !rbfNameIsPlex("Ple") && !rbfNameIsPlex("menu.rbf") &&
              !rbfNameIsPlex("/media/fat/Plex/NES.rbf") && !rbfNameIsPlex("XPlex"));
        for (const char* bad : {"MENU", "NES", "Arcade-Plex"}) {
            CoreGate g;
            Sim s;
            s.rbf = {true, bad, 1000};
            CHECK(!anyProbe(g, s, 5000000) && !g.open());
            CHECK(logLines(g, (std::string("RBFNAME '") + bad + "' is not a Plex core").c_str()) ==
                  1);
        }
    }
    {
        // Verdicts: a build off the allowlist is never asked again until a reload.
        CoreGate g;
        Sim s;
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(g.probeResult(true, kBuild5D51, "not on the allowlist", "",
                            "not in the FPGA build table", s.now) ==
              E::Rejected);
        CHECK(!g.open() && g.state() == CoreGateState::Rejected);
        CHECK(logLines(g, "answered CAPS with build 0x48005D51 (not in the FPGA build table), "
                          "not on the allowlist") == 1);
        CHECK(!anyProbe(g, s, 30000000));
        s.main = {101, 101};
        s.rewrite("Plex");
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);
    }
    {
        // No reply: backoff 1, 2, 4, 8 s, then rejected after 5 probes; two log lines.
        CoreGate g;
        Sim s;
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        int probes = 1;
        for (int attempt = 1; attempt < CoreGate::kMaxProbeAttempts; ++attempt) {
            CHECK(g.probeResult(false, 0, "", "", "no fresh core capability reply", s.now) ==
                  E::None);
            const int64_t wait = 500000LL << attempt;
            CHECK(!anyProbe(g, s, wait - 200000));
            CHECK(anyProbe(g, s, 200000));
            ++probes;
        }
        CHECK(g.probeResult(false, 0, "", "", "x", s.now) == E::Rejected && probes == 5);
        std::vector<std::string> lines;
        CHECK(logLines(g, "no CAPS reply from 'Plex'", &lines) == 2);
        CHECK(!anyProbe(g, s, 60000000));
    }
    {
        // Lab tools: unmanaged, always open, never closed.
        CoreGate u;
        u.resetForTest(true);
        Sim s;
        s.cn = {true, "MENU", 1000};
        CHECK(u.open() && !u.managed() && u.observe(s.obs()) == E::None);
        u.closeNow("x");
        CHECK(u.open() && u.openAndLive());
    }
    {
        // Pairing: the first build the gate opens on pairs the daemon with its raster.
        CoreGate g;
        Sim s;
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);
        CHECK(g.pairedRaster() == "480p");
        CHECK(logLines(g, "this daemon is paired with 480p cores (first verified build "
                          "0x48005D45)") == 1);
        // A 240p core after a reload: re-pair and open. The previous BUILD_ID does not stick.
        s.main = {200, 200};
        s.rewrite("Plex", "Plex_240p");
        CHECK(g.observe(s.obs()) == E::Closed);
        CHECK(anyProbe(g, s, CoreGate::kSettleUs));
        CHECK(g.probeResult(true, kBuild240p, "", "240p", "row", s.now) == E::Opened);
        CHECK(g.open() && g.pairedRaster() == "240p" && g.buildId() == kBuild240p);
        CHECK(!g.idleErrorPaintAllowed());
        CHECK(g.lastOpenId() == kBuild240p);
        CHECK(g.pairingErrorText().empty());
        CHECK(g.takeRePair());
        CHECK(logLines(g, "core gate re-paired: 480p -> 240p on core load build 0x2400F40B") == 1);
        CHECK(!anyProbe(g, s, 30000000));
        // An allowlist refusal is reported as such, whatever the raster.
        s.main = {300, 300};
        s.rewrite("Plex", "Plex_x");
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(g.probeResult(true, kBuildUnknown, "not on the allowlist", "",
                            "not in the FPGA build table", s.now) == E::Rejected);
        CHECK(logLines(g, "(not in the FPGA build table), not on the allowlist") == 1);
        // Another 480p build: open again, still paired with 480p (no second pairing line).
        s.main = {400, 400};
        s.rewrite("Plex", "Plex_480p_5D50");
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(g.probeResult(true, kBuild5D50, "", "480p", "row", s.now) == E::Opened);
        CHECK(logLines(g, "this daemon is paired") == 0 && g.pairedRaster() == "480p");

        // A daemon whose first core is 240p: a later 480p core re-pairs and opens.
        CoreGate h;
        Sim t;
        t.rbf = {true, "Plex_240p", 1000};
        CHECK(anyProbe(h, t, CoreGate::kSettleUs + 200000));
        CHECK(h.probeResult(true, kBuild240p, "", "240p", "row", t.now) == E::Opened);
        CHECK(h.pairedRaster() == "240p");
        t.main = {200, 200};
        t.rewrite("Plex", "Plex_480p");
        CHECK(h.observe(t.obs()) == E::Closed);
        CHECK(anyProbe(h, t, CoreGate::kSettleUs));
        CHECK(h.probeResult(true, kBuild480p, "", "480p", "row", t.now) == E::Opened);
        CHECK(h.pairedRaster() == "480p" && h.takeRePair());
        CHECK(logLines(h, "core gate re-paired: 240p -> 480p on core load build 0x48005D45") == 1);
        t.main = {300, 300};
        t.rewrite("Plex", "Plex_240p");
        CHECK(anyProbe(h, t, CoreGate::kSettleUs + 200000));
        CHECK(h.probeResult(true, kBuild240p, "", "240p", "row", t.now) == E::Opened);
        // resetForTest is a new daemon process: no pairing.
        h.resetForTest(false);
        CHECK(h.pairedRaster().empty());

        // Goal D: first core 480i product 0x4801F40A pairs 480i; a later 480p re-pairs.
        CoreGate i480;
        Sim vi;
        CHECK(anyProbe(i480, vi, CoreGate::kSettleUs + 200000));
        CHECK(i480.probeResult(true, 0x4801F40Au, "", "480i", "row", vi.now) == E::Opened);
        CHECK(i480.pairedRaster() == "480i");
        CHECK(logLines(i480, "this daemon is paired with 480i cores (first verified build "
                             "0x4801F40A)") == 1);
        vi.main = {200, 200};
        vi.rewrite("Plex", "Plex_480p");
        CHECK(i480.observe(vi.obs()) == E::Closed);
        CHECK(anyProbe(i480, vi, CoreGate::kSettleUs));
        CHECK(i480.probeResult(true, kBuild480p, "", "480p", "row", vi.now) == E::Opened);
        CHECK(i480.pairedRaster() == "480p" && !i480.idleErrorPaintAllowed());
        CHECK(i480.pairingErrorText().empty());
        CHECK(logLines(i480, "core gate re-paired: 480i -> 480p on core load build 0x48005D45") ==
              1);

        // Paired at start with the daemon's FPGA mode (main.cpp): a first core of another
        // raster re-pairs and opens. Returning to the configured raster re-pairs again.
        CoreGate c;
        c.setPairedRaster("480p", "FPGA mode 480p: conf ladder 480p 640x480");
        CHECK(logLines(c, "this daemon is paired with 480p cores (FPGA mode 480p: conf ladder "
                          "480p 640x480)") == 1);
        Sim u;
        u.rbf = {true, "Plex_240p", 1000};
        CHECK(anyProbe(c, u, CoreGate::kSettleUs + 200000));
        CHECK(c.probeResult(true, kBuild240p, "", "240p", "row", u.now) == E::Opened);
        CHECK(c.pairedRaster() == "240p" && c.takeRePair());
        CHECK(!c.idleErrorPaintAllowed() && c.pairingErrorText().empty());
        CHECK(c.lastOpenId() == kBuild240p);
        CHECK(logLines(c, "core gate re-paired: 480p -> 240p on core load build 0x2400F40B") == 1);
        u.main = {200, 200};
        u.rewrite("Plex", "Plex_480p");
        CHECK(anyProbe(c, u, CoreGate::kSettleUs + 200000));
        CHECK(c.probeResult(true, kBuild480p, "", "480p", "row", u.now) == E::Opened);
        CHECK(logLines(c, "core gate re-paired: 240p -> 480p on core load build 0x48005D45") == 1);
        CHECK(c.pairedRaster() == "480p");

        // 480p -> 480i -> 240p -> High (same 480p label, new id) -> 480p.
        // Each allowlisted load re-pairs. The pairing OSD stays empty.
        CoreGate seq;
        seq.setPairedRaster("480p", "FPGA mode 480p: MPX_FPGA_MODE=480p");
        (void)seq.takeLogs();
        Sim qs;
        auto step = [&](uint32_t id, const char* raster, const char* rbfName) {
            qs.main.pid += 10;
            qs.main.startTicks = qs.main.pid;
            qs.rewrite("Plex", rbfName);
            CHECK(seq.observe(qs.obs()) == E::Closed);
            CHECK(anyProbe(seq, qs, CoreGate::kSettleUs + 200000));
            CHECK(seq.probeResult(true, id, "", raster, "row", qs.now) == E::Opened);
            CHECK(seq.takeRePair());
            CHECK(seq.lastOpenId() == id);
            CHECK(seq.pairingErrorText().empty());
            CHECK(seq.pairedRaster() == raster);
            (void)seq.takeLogs();
        };
        CHECK(anyProbe(seq, qs, CoreGate::kSettleUs + 200000));
        CHECK(seq.probeResult(true, 0x48C51FF1u, "", "480p", "row", qs.now) == E::Opened);
        CHECK(!seq.takeRePair());
        CHECK(seq.lastOpenId() == 0x48C51FF1u);
        (void)seq.takeLogs();
        step(0x48F14A0Bu, "480i", "Plex_480i");
        step(0x24F1A2C4u, "240p", "Plex_240p15");
        step(0x48A64AEDu, "480p", "Plex_High");
        step(0x487A1BD1u, "480p", "Plex_480p");
        CHECK(seq.lastOpenId() == 0x487A1BD1u);
        CHECK(seq.pairingErrorText().empty());
    }
    {
        // Main's identity (FpgaSpi::mainIdentity, the oldest process named MiSTer).
        // A tick with no Main readable is no reload; the same Main after it changes nothing.
        CoreGate g;
        Sim s;
        CHECK(anyProbe(g, s, CoreGate::kSettleUs + 200000));
        CHECK(g.probeResult(true, kBuild480p, "", "480p", "row", s.now) == E::Opened);
        (void)g.takeLogs();
        const MainIdentity m0 = s.main;
        s.main = {};
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::None && g.open());
        s.main = m0;
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::None && g.open());
        // The same PID with another start time is another process: a reload.
        s.main = {m0.pid, m0.startTicks + 1};
        s.now += 200000;
        CHECK(g.observe(s.obs()) == E::Closed);
        CHECK(logLines(g, "core gate closed: Main restarted") == 1);

        // Started with no Main at all: the first Main to appear is a reload marker (its own
        // CORENAME write is the fresh one).
        CoreGate h;
        Sim t;
        t.main = {};
        CHECK(h.observe(t.obs()) == E::None);
        CHECK(!anyProbe(h, t, 2000000));
        CHECK(logLines(h, "Main is not running") == 1);
        t.main = {700, 700};
        CHECK(!anyProbe(h, t, 3000000));
        CHECK(logLines(h, "waiting for the new Main to rewrite CORENAME") == 1);
        t.rewrite("Plex");
        CHECK(anyProbe(h, t, CoreGate::kSettleUs + 200000));
    }
    {
        // The daemon's tick sequence (coreGateTickStep, run by MediaPlayer::coreGateTick)
        // with recording hooks: order and conditions.
        struct Rec {
            Sim s;
            bool playing = false;
            bool playAfterProbe = false; // a cast starts right after the probe
            CoreGateEvent verdict = E::Opened;
            std::vector<std::string> calls;
        };
        auto hooks = [](CoreGate& g, Rec& r) {
            CoreGateTickHooks h;
            h.observe = [&r] {
                r.calls.push_back("observe");
                return r.s.obs();
            };
            h.playing = [&r] { return r.playing; };
            h.probe = [&g, &r] {
                r.calls.push_back("probe");
                const bool ok = r.verdict == E::Opened;
                const CoreGateEvent e =
                    g.probeResult(true, ok ? kBuild480p : kBuildUnknown,
                                  ok ? "" : "not on the allowlist", ok ? "480p" : "", "row",
                                  r.s.now);
                if (r.playAfterProbe)
                    r.playing = true;
                return e;
            };
            h.coreReturned = [&r] { r.calls.push_back("coreReturned"); };
            h.finishReturn = [&r] { r.calls.push_back("finishReturn"); };
            h.repaint = [&r] { r.calls.push_back("repaint"); };
            h.log = [&r](const std::string& line) { r.calls.push_back("log:" + line); };
            return h;
        };
        auto actions = [](Rec& r) { // the calls other than observe and log
            std::string out;
            for (const auto& c : r.calls)
                if (c != "observe" && c.rfind("log:", 0) != 0)
                    out += (out.empty() ? "" : ",") + c;
            r.calls.clear();
            return out;
        };
        auto run = [](CoreGate& g, Rec& r, const CoreGateTickHooks& h, bool& pending,
                      int64_t us) {
            for (int64_t t = 0; t < us; t += 200000) {
                r.s.now += 200000;
                (void)coreGateTickStep(g, h, pending);
            }
        };
        {
            // Unmanaged: nothing is called, not even observe.
            CoreGate g;
            g.resetForTest(true);
            Rec r;
            bool pending = false;
            const CoreGateTickHooks h = hooks(g, r);
            run(g, r, h, pending, 2000000);
            CHECK(r.calls.empty());
        }
        {
            // A probe is wanted while a cast plays: none until it stops, then probe, reset,
            // finish, repaint, in that order, once.
            CoreGate g;
            Rec r;
            r.playing = true;
            bool pending = false;
            const CoreGateTickHooks h = hooks(g, r);
            run(g, r, h, pending, 3000000);
            CHECK(actions(r).empty() && g.state() == CoreGateState::Verifying);
            r.playing = false;
            run(g, r, h, pending, 200000);
            CHECK(actions(r) == "probe,coreReturned,finishReturn,repaint");
            CHECK(g.open() && !pending);
            run(g, r, h, pending, 2000000);
            CHECK(actions(r).empty());
        }
        {
            // A cast starts right after the open: the return finishes only once it stops.
            CoreGate g;
            Rec r;
            r.playAfterProbe = true;
            bool pending = false;
            const CoreGateTickHooks h = hooks(g, r);
            run(g, r, h, pending, CoreGate::kSettleUs + 200000);
            CHECK(actions(r) == "probe,coreReturned" && pending);
            run(g, r, h, pending, 2000000);
            CHECK(actions(r).empty() && pending);
            r.playing = false;
            run(g, r, h, pending, 200000);
            CHECK(actions(r) == "finishReturn,repaint" && !pending);
        }
        {
            // Closed again before the return finished: no repaint until the next open,
            // which resets the per-core state again.
            CoreGate g;
            Rec r;
            r.playAfterProbe = true;
            bool pending = false;
            const CoreGateTickHooks h = hooks(g, r);
            run(g, r, h, pending, CoreGate::kSettleUs + 200000);
            CHECK(actions(r) == "probe,coreReturned" && pending);
            r.s.main = {200, 200};
            r.s.rewrite("Plex");
            r.playing = false;
            r.playAfterProbe = false;
            run(g, r, h, pending, 200000);
            CHECK(actions(r).empty() && !g.open() && pending);
            run(g, r, h, pending, CoreGate::kSettleUs);
            CHECK(actions(r) == "probe,coreReturned,finishReturn,repaint" && g.open());
        }
        {
            // Rejected: no reset, no finish, no repaint; the gate's lines reach the log.
            CoreGate g;
            Rec r;
            r.verdict = E::Rejected;
            bool pending = false;
            const CoreGateTickHooks h = hooks(g, r);
            run(g, r, h, pending, CoreGate::kSettleUs + 200000);
            int logged = 0;
            for (const auto& c : r.calls)
                logged += c.find("log:misterplexd: core gate stays closed: 'Plex' answered "
                                 "CAPS with build 0x12345678") == 0;
            CHECK(logged == 1);
            CHECK(actions(r) == "probe" && !g.open() && !pending);
            run(g, r, h, pending, 5000000);
            CHECK(actions(r).empty());
        }
    }
    {
        // The startup glass probe is skipped while the gate is closed (main.cpp:701), so the
        // daemon starts on True480 unless the watcher's pin says L4. The gate opens only on
        // an allowlisted row, and no row is an L4 (720p) raster, so an L4 core is never
        // given the True480 bank. A new L4 row must revisit main.cpp:701.
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
            const std::string r = b.raster;
            CHECK(r == "480p" || r == "240p" || r == "480i");
        }
    }
    std::printf("  state machine: ok\n");
}

// ---------------------------------------------------------------- 5. source paths

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The checks the per-path helpers stand for are called on those paths in the daemon.
void sourceChecks() {
    const std::string src = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/media_player.cpp");
    CHECK(!src.empty());
    auto near = [&](const std::string& a, const std::string& b, size_t within) {
        for (size_t at = src.find(a); at != std::string::npos; at = src.find(a, at + 1)) {
            const size_t bt = src.find(b, at);
            if (bt != std::string::npos && bt - at <= within)
                return true;
        }
        return false;
    };
    auto count = [&](const std::string& a) {
        int n = 0;
        for (size_t at = src.find(a); at != std::string::npos; at = src.find(a, at + 1))
            ++n;
        return n;
    };
    // Audio pump: checked every iteration, right before the MrAudio write.
    CHECK(near("if (!coreGateAllowsAudioWrite()) {\n                audioFailure(coreGateClosedError(\"MrAudio write\"));",
               "::write(output, src + offset", 6000));
    // ARM legacy pump.
    CHECK(near("bool liveCoreStillPlex() {", "return coreGateAllowsAudioWrite();", 80));
    // Session AU loop: every iteration.
    CHECK(near("while (current()) {\n                if (!coreGateAllowsSession()) {",
               "failFpgaSession(coreGateClosedError(\"FPGA session\"));", 120));
    // Stale epochs: the session-end path, the begin path and the deferred tick step.
    CHECK(count("coreGateEpochIsStale(") == 3);
    // The tick runs coreGateTickStep (behaviour tested above) with these hooks.
    CHECK(count("(void)coreGateTickStep(CoreGate::process(), h, coreReturnPending_);") == 1);
    CHECK(near("h.playing = [this] {", "return playing_.load();", 60));
    CHECK(near("h.probe = [this] {", "return fpga_.runCoreGateProbe(500);", 400));
    // The core-return hook resets the per-core FPGA state. Bounded to the h.coreReturned lambda's own body (to its
    // closing "\n    };"), not a character window: SPS-gate L3 r2 put a comment and clearGateError(...) ahead of the
    // call, and a window wide enough for those would also reach h.finishReturn, the next lambda.
    {
        const size_t head = src.find("h.coreReturned = [this] {");
        const size_t end = head == std::string::npos ? std::string::npos : src.find("\n    };", head);
        const std::string body =
            end == std::string::npos ? std::string() : src.substr(head, end - head);
        CHECK(body.find("fpga_.onCoreReturned();") != std::string::npos);
    }
    CHECK(near("h.finishReturn = [this] {",
               "coreGateEpochIsStale(retained, fpgaSessionCoreChanges_)", 500));
    CHECK(near("h.repaint = [this] {", "paintIdle();", 40));
    CHECK(near("o.main = FpgaSpi::mainIdentity();", "o.mainStartWallNs = mainStartWallNs(o.main);",
               80));
    CHECK(near("o.mainStartWallNs = mainStartWallNs(o.main);", "o.bootWallNs = bootWallNs();",
               80));
    CHECK(near("o.bootWallNs = bootWallNs();", "o.coreName = stampCoreFile(kCoreNamePath);", 80));
    // video_mode write and fb0 writes.
    CHECK(near("if (!CoreGate::process().open()) {", "writeMisterCmdLine(kMisterCmdPath, next)", 300));
    CHECK(count("fb_.clear()") == 2 && count("if (CoreGate::process().open())\n                fb_.clear();") == 1 &&
          count("if (fb_.ok() && CoreGate::process().open())\n            fb_.clear();") == 1);
    CHECK(count("if (reconToFb && fb_.ok() && CoreGate::process().open()) {") == 1);
    CHECK(count("if (!fb_.ok() || !CoreGate::process().open())\n                return;") == 1);
    CHECK(count("if (!CoreGate::process().open() && !pairingPaint)\n        return;") == 1);
    CHECK(src.find("incorrect pairing daemon=") == std::string::npos);
    CHECK(count("idleErrorPaintAllowed()") >= 1);
    std::printf("  media_player.cpp: audio write, session loop, stale epochs, tick, video_mode "
                "and fb0 checks in place\n");
}

#endif // MPX_CORE_GATE_HASH_ONLY

} // namespace

int main(int argc, char** argv) {
#ifdef MPX_CORE_GATE_HASH_ONLY
    const bool hashOnly = true;
    (void)argc;
    (void)argv;
#else
    const bool hashOnly = argc > 1 && std::strcmp(argv[1], "--battery-hash") == 0;
#endif
    // The battery-hash run forks no Main; every full run does (the fake-Main rows), so it needs the private namespace.
    if (!hashOnly) {
        std::string why;
        if (!privatePidNsWithOwnProc(&why)) {
            std::fprintf(stderr,
                         "test_core_gate: VOID (exit 77): the full run forks a fake MiSTer Main and must run in a "
                         "private PID namespace with its own /proc, inside heavy-sim-slot "
                         "(heavy-sim-slot -- unshare --user --map-root-user --pid --fork --mount-proc ...): %s\n",
                         why.c_str());
            return 77;
        }
        // The guard's own check: stop here, before any fork (the mutant check runs this on the host).
        if (std::getenv("CORE_GATE_GUARD_ONLY")) {
            std::printf("test_core_gate: guard passed (%s); CORE_GATE_GUARD_ONLY set, no test run\n", why.c_str());
            return 0;
        }
    }
    FakeHw hw;
    if (!hw.start(kBuild480p)) {
        std::fprintf(stderr, "test_core_gate: cannot set up %s\n", MPX_DEVMEM_PATH);
        return 1;
    }
    if (hashOnly) {
#ifndef MPX_CORE_GATE_HASH_ONLY
        // This tree: the daemon's managed gate, opened the way the daemon opens it.
        {
            MainSim m;
            FpgaSpi p;
            if (!p.open())
                return 1;
            m.names("Plex", "Plex_480p");
            hw.fillSentinel();
            (void)tick(p, m);
            m.advance(CoreGate::kSettleUs + 100000);
            const CoreGateEvent e = tick(p, m);
            p.close();
            m.cleanup();
            if (e != CoreGateEvent::Opened) {
                std::fprintf(stderr, "battery-hash: the gate did not open (%s)\n", eventName(e));
                hw.stop();
                return 1;
            }
        }
#endif
        hw.fillSentinel();
        FpgaSpi f;
        if (!f.open()) {
            hw.stop();
            return 1;
        }
        const uint64_t strobes0 = hw.strobes();
        runOpenPaths(f);
        f.close();
        std::printf("battery_hash=%016llx strobes=%llu\n", static_cast<unsigned long long>(hw.hash()),
                    static_cast<unsigned long long>(hw.strobes() - strobes0));
        hw.stop();
        return 0;
    }
#ifndef MPX_CORE_GATE_HASH_ONLY
    std::printf("test_core_gate: real fpga_spi.cpp on %s, fake Plex core\n", MPX_DEVMEM_PATH);
    CoreGate& gate = CoreGate::process();
    // Fail closed: the daemon's gate is closed before it has looked at anything.
    CHECK(gate.managed() && !gate.open() && gate.state() == CoreGateState::Closed);
    stateMachine();

    MainSim m;
    m.names("MENU", "menu");
    hw.reprogram(0); // the menu core speaks no AU ring
    FpgaSpi f;
    CHECK(f.open());
    closedPhase("closed at start, no tick yet", f, hw);
    {
        bool probe = false;
        for (int i = 0; i < 15; ++i) {
            m.advance(200000);
            probe |= tick(f, m) != CoreGateEvent::None;
        }
        CHECK(!probe && hw.probes() == 0);
        CHECK(gateLogCount("core gate stays closed: loaded core 'MENU' is not Plex") == 1);
    }
    closedPhase("MENU loaded, 3 s of ticks", f, hw);

    CoreGateEvent e = reload("MENU -> Plex 480p 0x48005D45", f, m, hw, kBuild480p, "Plex",
                             "Plex_480p", false, 0);
    CHECK(e == CoreGateEvent::Opened && gate.open() && gate.buildId() == kBuild480p);
    openPhase("Plex 480p open", f, hw);
    {
        // Control: with the gate open these calls do write DDR or drive SPI, so their
        // silence in the closed phases is the gate.
        hw.fillSentinel();
        f.reprobeDdrKick();
        const Battery b = runAllPaths(f, hw);
        std::string names;
        for (const auto& name : b.trafficNames)
            names += (names.empty() ? "" : " ") + name;
        std::printf("  %-34s calls=%d with traffic=%d: %s\n", "control, gate open", b.calls,
                    b.traffic, names.c_str());
        CHECK(b.traffic >= 10);
        CHECK(gate.open());
    }

    // The incident: Plex -> Plex, CORENAME 'Plex' both times. The daemon keeps calling
    // FpgaSpi after Main sets GPO bit 30 and before any tick.
    f.setNegotiatedBuild(recognisedFpgaBuild(kBuild480p));
    e = reload("Plex 480p 45D -> Plex 480p 5D50, CORENAME 'Plex' both", f, m, hw, kBuild5D50,
               "Plex", "Plex_480p_5D50", true, 3000000);
    CHECK(e == CoreGateEvent::Opened && gate.buildId() == kBuild5D50);
    CHECK(f.negotiatedBuild() == nullptr); // onCoreReturned: the old row is gone
    openPhase("Plex 5D50 open", f, hw);

    // The same, to a 240p core: the daemon re-pairs on that load and opens.
    e = reload("Plex 480p -> Plex 240p 0x2400F40B, CORENAME 'Plex' both", f, m, hw, kBuild240p,
               "Plex", "Plex_240p", true, 2000000);
    CHECK(e == CoreGateEvent::Opened && gate.open() && gate.buildId() == kBuild240p);
    CHECK(gate.pairedRaster() == "240p");
    CHECK(gateLogCount("core gate re-paired: 480p -> 240p on core load build 0x2400F40B") == 1);
    openPhase("240p re-paired onto a 480p daemon", f, hw);

    e = reload("Plex 240p -> Plex 480p 5D50", f, m, hw, kBuild5D50, "Plex", "Plex_480p_5D50",
               false, 0);
    CHECK(e == CoreGateEvent::Opened && gate.buildId() == kBuild5D50);

    e = reload("Plex 5D50 reloaded, same RBF and names", f, m, hw, kBuild5D50, "Plex",
               "Plex_480p_5D50", false, 0);
    CHECK(e == CoreGateEvent::Opened && gate.buildId() == kBuild5D50);

    {
        std::printf("  -- 5D50 -> 45D entirely between two ticks\n");
        hw.setFabric(kFpgaMgrModeUser, true);
        hw.setFabric(kModeConfig, true);
        hw.reprogram(kBuild480p);
        hw.setFabric(kFpgaMgrModeUser, false);
        m.newMain();
        m.names("Plex", "Plex_480p");
        CHECK(tick(f, m) == CoreGateEvent::Closed);
        CHECK(gateLogCount("core gate closed: Main restarted") > 0);
        m.advance(CoreGate::kSettleUs);
        CHECK(probeTick("probe", f, m, hw) == CoreGateEvent::Opened);
        CHECK(gate.buildId() == kBuild480p);
    }

    {
        const bool allow5D51 = fpgaBuildRefusal(kBuild5D51).empty();
        e = reload(allow5D51 ? "Plex 45D -> 5D51 (in this build table)"
                             : "Plex 45D -> 5D51 (not in this build table)",
                   f, m, hw, kBuild5D51, "Plex", "Plex_480p_5D51", false, 0);
        if (allow5D51) {
            CHECK(e == CoreGateEvent::Opened && gate.buildId() == kBuild5D51);
            openPhase("Plex 5D51 open", f, hw);
        } else {
            CHECK(e == CoreGateEvent::Rejected && !gate.open());
            CHECK(gateLogCount("answered CAPS with build 0x48005D51 (not in the FPGA build "
                               "table), not on the allowlist") == 1);
            closedPhase("5D51 rejected", f, hw);
            const uint64_t p0 = hw.probes();
            for (int i = 0; i < 50; ++i) {
                m.advance(200000);
                CHECK(tick(f, m) == CoreGateEvent::None);
            }
            CHECK(hw.probes() == p0); // asked once
            closedPhase("5D51 rejected, 10 s later", f, hw);
        }
    }

    e = reload("-> unknown build 0x12345678", f, m, hw, kBuildUnknown, "Plex", "Plex_x", false, 0);
    CHECK(e == CoreGateEvent::Rejected);
    closedPhase("unknown build rejected", f, hw);

    {
        // A core named Plex that never answers CAPS: 5 probes with backoff, then rejected.
        e = reload("-> 'Plex' that never answers", f, m, hw, 0, "Plex", "Plex_old", false, 0);
        CHECK(e == CoreGateEvent::None && !gate.open());
        for (int i = 0; i < 100 && gate.state() != CoreGateState::Rejected; ++i) {
            m.advance(200000);
            const CoreGateEvent t = tick(f, m);
            CHECK(t == CoreGateEvent::None || t == CoreGateEvent::Rejected);
        }
        CHECK(gate.state() == CoreGateState::Rejected);
        CHECK(gateLogCount("no CAPS reply from 'Plex'") == 2);
        CHECK(gateLogCount("after 5 probes") == 1);
        closedPhase("no CAPS reply, rejected", f, hw);
    }

    e = reload("-> Plex 480p 0x48005D45", f, m, hw, kBuild480p, "Plex", "Plex_480p", false, 0);
    CHECK(e == CoreGateEvent::Opened);

    {
        // The first FPGA access after Main sets GPO bit 30 closes the gate itself, at each
        // choke point: every one re-reads the fabric right before it touches anything.
        std::printf("  -- first access after GPO bit 30, per choke point\n");
        std::vector<uint8_t> yuv(640 * 480 * 3 / 2, 0x40);
        PlaybackCommand cmd = PlaybackCommand::None;
        FpgaSpi::BitstreamStatus bs;
        const std::vector<std::pair<const char*, std::function<bool()>>> firsts = {
            {"SPI (setStatusBit)", [&] { return f.setStatusBit(16, 1); }},
            {"DDR bank (sendYuv420pFrameDdr)",
             [&] { return f.sendYuv420pFrameDdr(yuv.data(), yuv.size(), 640, 480, 0); }},
            {"mailbox (readInputMailbox)", [&] { return f.readInputMailbox(cmd); }},
            {"AU ring (readBitstreamStatus)", [&] { return f.readBitstreamStatus(bs); }},
            {"audio pump (AllowsAudioWrite)", [&] { return coreGateAllowsAudioWrite(); }},
            {"session loop (AllowsSession)", [&] { return coreGateAllowsSession(); }},
            {"commit of a mapped bank",
             [&] { return f.commitDdrBankIngest(0, f.ddrFrameLayout().frame_bytes); }},
        };
        for (const auto& [name, call] : firsts) {
            reopen(f, m);
            f.reprobeDdrKick();
            CHECK(f.ddrBankVirt(0) != nullptr); // mapped under the open gate (decoder side)
            hw.fillSentinel();
            hw.setFabric(kFpgaMgrModeUser, true);
            const uint64_t h0 = hw.hash(), s0 = hw.strobes(), w0 = hw.windows.load();
            const bool ok = call();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const bool quiet =
                hw.hash() == h0 && hw.strobes() == s0 && hw.windows.load() == w0;
            std::printf("  %-34s %s, bytes changed=%s, spi strobes=%llu, gate %s\n", name,
                        ok ? "ALLOWED" : "refused", hw.hash() == h0 ? "no" : "YES",
                        static_cast<unsigned long long>(hw.strobes() - s0),
                        gate.open() ? "OPEN" : "closed");
            CHECK(!ok && quiet && !gate.open());
            hw.setFabric(kFpgaMgrModeUser, false);
            flushGateLogs();
        }
        {
            // Main began a load just before the SPI window stopped it: GPO bit 30 is set by
            // the time Main is stopped. The window reads the fabric again then, so no word
            // goes out. Main here is a "MiSTer" symlink to sleep, so the window really stops
            // a process.
            reopen(f, m);
            const std::string dir = std::string(MPX_DEVMEM_PATH) + ".main";
            ::mkdir(dir.c_str(), 0755);
            const std::string fakeMain = dir + "/MiSTer";
            ::unlink(fakeMain.c_str());
            CHECK(::symlink("/bin/sleep", fakeMain.c_str()) == 0);
            const pid_t child = ::fork();
            if (child == 0) {
                ::execl(fakeMain.c_str(), fakeMain.c_str(), "1000", static_cast<char*>(nullptr));
                ::_exit(127);
            }
            for (int i = 0; i < 200 && !FpgaSpi::mainAlive(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            CHECK(FpgaSpi::mainAlive());
            hw.fillSentinel();
            // MPX_CG_STALL_MS (teeth only): the fake thread sleeps across the window, so nothing but
            // the seam can set the bit before the daemon reads the fabric again (fpga_spi.cpp:413).
            const char* stallEnv = std::getenv("MPX_CG_STALL_MS");
            const unsigned stallMs = stallEnv ? static_cast<unsigned>(std::strtoul(stallEnv, nullptr, 10)) : 0u;
            bool stallEntered = false;
            if (stallMs > 0) {
                hw.stallUs.store(stallMs * 1000u);
                for (int i = 0; i < 1000 && !hw.stalled.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                stallEntered = hw.stalled.load();
            }
            const uint64_t s0 = hw.strobes(), st0 = hw.stops.load(), c0 = hw.coveredFires.load();
            hw.resetOnWindow.store(true);
            const bool ok = f.setStatusBit(16, 1);
            hw.resetOnWindow.store(false);
            const uint64_t ds = hw.strobes() - s0;
            std::printf("  %-34s %s, spi strobes=%llu, main stopped=%s, gate %s\n",
                        "load begun as Main is stopped", ok ? "SENT" : "refused",
                        static_cast<unsigned long long>(ds),
                        hw.stops.load() > st0 ? "yes" : "no", gate.open() ? "OPEN" : "closed");
            if (stallMs > 0)
                std::printf("  %-34s %u ms: entered=%s covered=%s\n", "stall", stallMs,
                            stallEntered ? "yes" : "no", hw.coveredFires.load() > c0 ? "yes" : "no");
            CHECK(hw.stops.load() > st0); // the window did stop Main: every task in 'T' (the seam)
            CHECK(!ok && ds == 0 && !gate.open());
            ::kill(child, SIGCONT);
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
            ::unlink(fakeMain.c_str());
            ::rmdir(dir.c_str());
            hw.setFabric(kFpgaMgrModeUser, false);
            flushGateLogs();
        }
        // An FpgaSpi with no register map refuses without closing the gate: no map is not a
        // reload signal.
        reopen(f, m);
        FpgaSpi unmapped;
        CHECK(!unmapped.readBitstreamStatus(bs));
        CHECK(gate.open());
    }

    // ------------------------------------------------------------ 5. running session
    {
        std::printf("  -- running session on 480p 45D, then a reload to 480p 5D50\n");
        using Push = FpgaSpi::BitstreamPushResult;
        FpgaSpi::VideoCapabilities caps;
        const uint64_t sid = 0x5E550001ull;
        CHECK(f.beginFpgaVideoSession(sid, caps, 500));
        CHECK(caps.build_id == kBuild480p);
        const uint64_t changesAtBegin = gate.changes();
        uint8_t payload[64];
        std::memset(payload, 0x11, sizeof payload);
        FpgaSpi::BitstreamAccessUnit au;
        au.session_id = sid;
        au.annexb = payload;
        au.len = sizeof payload;
        au.timebase_num = 1;
        au.timebase_den = 90000;
        au.duration = 3003;
        int pushed = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            au.seq = i;
            au.pts = static_cast<int64_t>(i) * 3003;
            au.flags = i == 0 ? ring::kAccessUnitKeyframe : 0u;
            pushed += f.pushBitstreamAccessUnit(au, 200) == Push::Ok;
        }
        CHECK(pushed == 4);
        CHECK(coreGateAllowsSession() && coreGateAllowsAudioWrite());
        CHECK(!coreGateEpochIsStale(sid, changesAtBegin));

        // Main starts loading 5D50; the session's threads run on. The audio pump and the AU
        // loop check first, before any FpgaSpi call has seen the fabric.
        hw.fillSentinel();
        hw.setFabric(kFpgaMgrModeUser, true);
        const uint64_t h0 = hw.hash(), s0 = hw.strobes(), w0 = hw.windows.load();
        const bool audioRefused = !coreGateAllowsAudioWrite();
        const bool loopRefused = !coreGateAllowsSession();
        au.seq = 4;
        au.flags = 0;
        const bool pushRefused = f.pushBitstreamAccessUnit(au, 50) != Push::Ok;
        const bool pauseRefused =
            !f.controlAudioSession(sid, FpgaSpi::AudioControl::Pause, 50);
        const bool drainRefused = !f.drainBitstreamSession(sid, 50);
        FpgaSpi::VideoPresentation vp;
        const bool presentRefused = !f.readVideoPresentation(sid, vp);
        const bool abortRefused = !f.abortFpgaVideoSession(sid, 50);
        const bool endRefused = !f.endBitstreamSession(sid, 50);
        std::printf("  %-34s push=%s audio-pause=%s drain=%s presentation=%s abort=%s end=%s "
                    "audio-write=%s session-loop=%s; bytes changed=%s, spi strobes=%llu\n",
                    "session paths after GPO bit 30", pushRefused ? "refused" : "SENT",
                    pauseRefused ? "refused" : "SENT", drainRefused ? "refused" : "SENT",
                    presentRefused ? "refused" : "READ", abortRefused ? "refused" : "SENT",
                    endRefused ? "refused" : "SENT", audioRefused ? "refused" : "ALLOWED",
                    loopRefused ? "refused" : "ALLOWED", hw.hash() == h0 ? "no" : "YES",
                    static_cast<unsigned long long>(hw.strobes() - s0));
        CHECK(pushRefused && pauseRefused && drainRefused && presentRefused && abortRefused &&
              endRefused && audioRefused && loopRefused);
        CHECK(hw.hash() == h0 && hw.strobes() == s0 && hw.windows.load() == w0);
        CHECK(!gate.open());
        CHECK(coreGateEpochIsStale(sid, changesAtBegin));

        // The reload completes. The reopen probe must get past the dead session.
        tick(f, m);
        hw.setFabric(kModeConfig, true);
        hw.reprogram(0);
        m.advance(200000);
        tick(f, m);
        m.newMain();
        hw.setFabric(kFpgaMgrModeUser, false);
        hw.reprogram(kBuild5D50);
        m.names("Plex", "Plex_480p_5D50");
        m.advance(200000);
        tick(f, m);
        m.advance(CoreGate::kSettleUs);
        CHECK(probeTick("reopen probe after the session", f, m, hw) == CoreGateEvent::Opened);
        CHECK(gate.buildId() == kBuild5D50);
        f.dropStaleSession(); // the daemon's deferred step; idempotent after the probe
        FpgaSpi::VideoCapabilities caps2;
        const uint64_t sid2 = 0x5E550002ull;
        CHECK(f.beginFpgaVideoSession(sid2, caps2, 500));
        CHECK(caps2.build_id == kBuild5D50);
        au.session_id = sid2;
        au.seq = 0;
        au.flags = ring::kAccessUnitKeyframe;
        CHECK(f.pushBitstreamAccessUnit(au, 200) == Push::Ok);
        CHECK(!coreGateEpochIsStale(sid2, gate.changes()));
        CHECK(coreGateAllowsSession() && coreGateAllowsAudioWrite());
        CHECK(f.endBitstreamSession(sid2, 500));
        std::printf("  %-34s new session on 0x%08X: begin, AU, end ok\n", "after the reopen",
                    caps2.build_id);
    }

    {
        // The probe's ring exception belongs to the probing thread alone.
        hw.fillSentinel();
        m.names("Plex", "Plex_480p_5D50"); // a rewrite: closed, then verifying after the settle
        CHECK(tick(f, m) == CoreGateEvent::Closed);
        m.advance(CoreGate::kSettleUs);
        CoreObservation o;
        o.fabric = f.fabricState();
        o.main = m.main;
        o.mainStartWallNs = m.mainStartWallNs;
        o.bootWallNs = m.bootWallNs;
        o.coreName = stampCoreFile(m.cn.c_str());
        o.rbfName = stampCoreFile(m.rbf.c_str());
        o.nowUs = m.now();
        CHECK(gate.observe(o) == CoreGateEvent::ProbeWanted);
        hw.fillSentinel();
        const uint64_t h0 = hw.hash();
        gate.beginProbe();
        bool other = true;
        std::thread t([&] { other = f.flushBitstreamDdr(); });
        t.join();
        gate.endProbe();
        std::printf("  %-34s ring reset from another thread: %s, bytes changed=%s\n",
                    "during a probe", other ? "SENT" : "refused", hw.hash() == h0 ? "no" : "YES");
        CHECK(!other && hw.hash() == h0);
        CHECK(f.runCoreGateProbe(200, m.now()) == CoreGateEvent::Opened);
        f.onCoreReturned();
        flushGateLogs();
    }

    {
        // (a) A new daemon started while Main loads a non-Plex core: the fabric is live and
        // the new Main runs, but CORENAME still says 'Plex' (the last core's). No probe and
        // no byte until this Main writes its names.
        std::printf("  -- a new daemon under a stale 'Plex' CORENAME\n");
        gate.resetForTest(false);
        g_returnPending = false;
        hw.reprogram(0); // the new core never answers CAPS
        m.newMain();     // started after the names were written: they are the last core's
        hw.fillSentinel();
        const uint64_t h0 = hw.hash(), s0 = hw.strobes(), p0 = hw.probes();
        bool moved = false;
        for (int i = 0; i < 15; ++i) {
            m.advance(200000);
            moved |= tick(f, m) != CoreGateEvent::None;
        }
        std::printf("  %-34s 3 s: probe=%s, bytes changed=%s, spi strobes=%llu\n",
                    "stale CORENAME at daemon start", hw.probes() != p0 ? "YES" : "no",
                    hw.hash() == h0 ? "no" : "YES",
                    static_cast<unsigned long long>(hw.strobes() - s0));
        CHECK(!moved && hw.probes() == p0 && hw.hash() == h0 && hw.strobes() == s0);
        CHECK(!gate.open());
        CHECK(gateLogCount("CORENAME/RBFNAME predate Main pid") == 1);
        closedPhase("stale CORENAME, no tick moved it", f, hw);
        m.names("NES", "NES"); // this Main writes its names
        for (int i = 0; i < 10; ++i) {
            m.advance(200000);
            CHECK(tick(f, m) == CoreGateEvent::None);
        }
        CHECK(gateLogCount("loaded core 'NES' is not Plex") >= 1 && hw.probes() == p0);
        // (b) A Plex CORENAME over a non-Plex RBFNAME is not asked about either.
        m.names("Plex", "MENU");
        for (int i = 0; i < 10; ++i) {
            m.advance(200000);
            CHECK(tick(f, m) == CoreGateEvent::None);
        }
        CHECK(gateLogCount("RBFNAME 'MENU' is not a Plex core") == 1 && hw.probes() == p0);
        closedPhase("CORENAME 'Plex', RBFNAME 'MENU'", f, hw);
    }

    {
        // A new daemon configured for 240p (main.cpp pairs it with its FPGA mode at start):
        // its first core, 480p, re-pairs and opens. A later 240p load re-pairs back.
        std::printf("  -- a new daemon, configured for 240p (MPX_FPGA_MODE=240p)\n");
        gate.resetForTest(false);
        g_returnPending = false;
        gate.setPairedRaster("240p", "FPGA mode 240p: MPX_FPGA_MODE=240p");
        flushGateLogs();
        CHECK(gateLogCount("this daemon is paired with 240p cores (FPGA mode 240p: "
                           "MPX_FPGA_MODE=240p)") == 1);
        e = reload("-> Plex 480p 0x48005D45 (this daemon's first core)", f, m, hw, kBuild480p,
                   "Plex", "Plex_480p", false, 0);
        CHECK(e == CoreGateEvent::Opened && gate.pairedRaster() == "480p");
        CHECK(gate.buildId() == kBuild480p);
        CHECK(gateLogCount("core gate re-paired: 240p -> 480p on core load build 0x48005D45") == 1);
        openPhase("480p first, re-paired off a 240p daemon", f, hw);
        // Not on the allowlist: rejected as such.
        e = reload("-> unknown build 0x12345678", f, m, hw, kBuildUnknown, "Plex", "Plex_x", false,
                   0);
        CHECK(e == CoreGateEvent::Rejected);
        CHECK(gateLogCount("answered CAPS with build 0x12345678 (not in the FPGA build table), "
                           "not on the allowlist") == 2);
        e = reload("-> Plex 480p 0x48005D45", f, m, hw, kBuild480p, "Plex", "Plex_480p", false, 0);
        CHECK(e == CoreGateEvent::Opened && gate.pairedRaster() == "480p");
        f.setNegotiatedBuild(recognisedFpgaBuild(kBuild240p));
        e = reload("Plex 480p -> Plex 240p, CORENAME 'Plex' both", f, m, hw, kBuild240p, "Plex",
                   "Plex_240p", true, 2000000);
        CHECK(e == CoreGateEvent::Opened && gate.buildId() == kBuild240p);
        CHECK(f.negotiatedBuild() == nullptr);
        openPhase("Plex 240p open", f, hw);
    }

    {
        // Main's identity on real processes (FpgaSpi::mainIdentity). Main's own child, named
        // MiSTer until its exec, is newer than Main and changes nothing, while it runs or
        // after it exits. A new Main with the old one gone is a reload.
        std::printf("  -- Main identity on real processes\n");
        const std::string dir = std::string(MPX_DEVMEM_PATH) + ".mainid";
        ::mkdir(dir.c_str(), 0755);
        const std::string exe = dir + "/MiSTer";
        ::unlink(exe.c_str());
        CHECK(::symlink("/bin/sleep", exe.c_str()) == 0);
        auto spawn = [&] {
            const pid_t c = ::fork();
            if (c == 0) {
                ::execl(exe.c_str(), exe.c_str(), "1000", static_cast<char*>(nullptr));
                ::_exit(127);
            }
            return c;
        };
        auto reap = [](pid_t p) {
            ::kill(p, SIGCONT);
            ::kill(p, SIGKILL);
            ::waitpid(p, nullptr, 0);
        };
        auto until = [](const std::function<bool()>& pred) {
            for (int i = 0; i < 400 && !pred(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return pred();
        };
        auto namedMister = [](pid_t p) {
            const std::string cmd = readFile("/proc/" + std::to_string(p) + "/cmdline");
            const std::string argv0 = cmd.c_str();
            return argv0.substr(argv0.rfind('/') + 1) == "MiSTer";
        };
        const pid_t main1 = spawn();
        CHECK(until([&] { return FpgaSpi::mainIdentity().pid == main1; }));
        const MainIdentity id1 = FpgaSpi::mainIdentity();
        {
            // mainStartWallNs: btime + start ticks, a lower bound of the true start (btime is
            // whole seconds, rounded down). The child started a moment ago.
            struct timespec ts {};
            ::clock_gettime(CLOCK_REALTIME, &ts);
            const int64_t nowNs = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
            const int64_t startNs = mainStartWallNs(id1);
            const double ageS = (nowNs - startNs) / 1e9;
            std::printf("  %-34s pid %d started %.2f s ago by btime + ticks\n",
                        "Main start on the wall clock", id1.pid, ageS);
            CHECK(startNs > 0 && ageS >= 0.0 && ageS < 3.0);
            CHECK(mainStartWallNs(MainIdentity{}) == 0);
            // bootWallNs: btime on today's clock, at or before every process start.
            const int64_t bootNs = bootWallNs();
            CHECK(bootNs > 0 && bootNs % 1000000000LL == 0 && bootNs <= startNs);
            CHECK(bootWallNs("/nonexistent/proc-stat") == -1);
            CHECK(mainStartWallNs(id1, "/nonexistent/proc-stat") == 0);
            // An RTC-less MiSTer before ntpd's step (or never synced): btime 0 is a real
            // boot time, and Main's start is its start ticks. Unparsable or missing: -1.
            const std::string fakeStat = std::string(MPX_DEVMEM_PATH) + ".stat";
            auto statWith = [&](const char* text) {
                if (FILE* fp = std::fopen(fakeStat.c_str(), "w")) {
                    std::fputs(text, fp);
                    std::fclose(fp);
                }
            };
            const long hz = ::sysconf(_SC_CLK_TCK);
            statWith("cpu  1 2 3 4\nintr 5\nbtime 0\nprocesses 7\n");
            CHECK(bootWallNs(fakeStat.c_str()) == 0);
            CHECK(mainStartWallNs(id1, fakeStat.c_str()) ==
                  static_cast<int64_t>(id1.startTicks) * 1000000000LL / hz);
            CHECK(mainStartWallNs(id1, fakeStat.c_str()) > 0);
            statWith("cpu  1 2 3 4\nbtime 1700000000\n");
            CHECK(bootWallNs(fakeStat.c_str()) == 1700000000LL * 1000000000LL);
            statWith("cpu  1 2 3 4\nbtime x\n");
            CHECK(bootWallNs(fakeStat.c_str()) == -1 && mainStartWallNs(id1, fakeStat.c_str()) == 0);
            statWith("cpu  1 2 3 4\nprocesses 7\n");
            CHECK(bootWallNs(fakeStat.c_str()) == -1);
            ::unlink(fakeStat.c_str());
        }
        m.main = id1;
        reopen(f, m);
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // a later start tick
        const pid_t kid = spawn();
        CHECK(until([&] { return namedMister(kid); }));
        const bool kidIgnored = FpgaSpi::mainIdentity() == id1;
        bool moved = false;
        for (int i = 0; i < 5; ++i) { // ticks with Main's child present
            m.main = FpgaSpi::mainIdentity();
            m.advance(200000);
            moved |= tick(f, m) != CoreGateEvent::None;
        }
        reap(kid);
        for (int i = 0; i < 5; ++i) { // and after it is gone
            m.main = FpgaSpi::mainIdentity();
            m.advance(200000);
            moved |= tick(f, m) != CoreGateEvent::None;
        }
        std::printf("  %-34s Main pid %d, its child %d: Main unchanged=%s, gate %s over 2 s\n",
                    "Main's child named MiSTer", main1, kid, kidIgnored ? "yes" : "NO",
                    moved || !gate.open() ? "MOVED" : "open");
        CHECK(kidIgnored && !moved && gate.open());
        reap(main1);
        const pid_t main2 = spawn();
        CHECK(until([&] { return FpgaSpi::mainIdentity().pid == main2; }));
        m.main = FpgaSpi::mainIdentity();
        m.advance(200000);
        const CoreGateEvent replaced = tick(f, m);
        const std::string line =
            "Main restarted (pid " + std::to_string(main1) + " -> " + std::to_string(main2) + ")";
        std::printf("  %-34s pid %d -> %d: %s\n", "Main replaced", main1, main2,
                    eventName(replaced));
        CHECK(replaced == CoreGateEvent::Closed && gateLogCount(line.c_str()) == 1);
        reap(main2);
        ::unlink(exe.c_str());
        ::rmdir(dir.c_str());
        m.newMain(); // back to the simulated Main
        m.names("Plex", "Plex_240p");
        (void)tick(f, m);
        m.advance(CoreGate::kSettleUs);
        CHECK(tick(f, m) == CoreGateEvent::Opened);
    }

    // review 09:03 C1 (lock order of the one doorbell-page lock). Under ddrPageMu_, the page's
    // gate check takes the gate mutex (closeNow) and spiMutex (setErr); nothing that holds either
    // takes ddrPageMu_ (fpga_spi.cpp audit). A D52-style poller loops plxPageRead32 while this
    // thread runs 200 cycles of: Main loads a core (GPO bit 30: the poller's access or the tick
    // closes the gate), a layout change (releaseDdrMap under ddrPageMu_), the reopen (CORENAME
    // rewrite, settle, the probe under bitstreamMutex, onCoreReturned under spiMutex). A watchdog
    // turns a deadlock into a FAIL. Test-only mutant MUT_C1_LOCK_ORDER_ABBA: the layout change runs
    // holding spiMutex (the inverted order a gate path would create) and must trip the watchdog.
    {
        std::printf("  -- C1 lock order: plxPageRead32 poller vs 200 close / core return / reopen cycles\n");
        constexpr int kCycles = 200;
        constexpr int kWatchdogS = 90;
        std::atomic<bool> stopPoll{false}, finished{false};
        std::atomic<uint64_t> polls{0}, pollOk{0};
        std::thread watchdog([&] {
            for (int i = 0; i < kWatchdogS * 10 && !finished.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!finished.load()) {
                std::printf("FAIL C1: no progress in %d s: a lock-order deadlock (polls=%llu)\n", kWatchdogS,
                            static_cast<unsigned long long>(polls.load()));
                std::fflush(stdout);
                ::_exit(2);
            }
        });
        std::thread poller([&] {
            uint32_t v = 0;
            while (!stopPoll.load()) {
                polls.fetch_add(1);
                if (f.plxPageRead32(0x118, v))
                    pollOk.fetch_add(1);
            }
        });
        g_quietGateLog = true;
        int closedSeen = 0, opened = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCycles; ++i) {
            hw.setFabric(kFpgaMgrModeUser, true);   // Main loads a core: GPO bit 30
            for (int k = 0; k < 200 && gate.open(); ++k) {
                if (k % 20 == 19)
                    (void)tick(f, m);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            closedSeen += !gate.open();
#ifdef MUT_C1_LOCK_ORDER_ABBA
            for (int k = 0; k < 20; ++k)
                f.withSpiLockForTest([&] { (void)f.setDdrFrameLayout(k % 2 ? 640 : 320, k % 2 ? 480 : 240); });
#else
            (void)f.setDdrFrameLayout(i % 2 ? 640 : 320, i % 2 ? 480 : 240);
#endif
            hw.setFabric(kFpgaMgrModeUser, false);
            m.names("Plex", "Plex_240p");
            (void)tick(f, m);
            m.advance(CoreGate::kSettleUs);
            opened += tick(f, m) == CoreGateEvent::Opened;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stopPoll.store(true);
        poller.join();
        finished.store(true);
        watchdog.join();
        g_quietGateLog = false;
        std::printf("  %-34s %d cycles in %.1f s: gate closed %d, reopened %d; poller %llu reads, %llu ok\n",
                    "C1 lock order", kCycles, secs, closedSeen, opened,
                    static_cast<unsigned long long>(polls.load()), static_cast<unsigned long long>(pollOk.load()));
        CHECK(closedSeen == kCycles && opened == kCycles);
        CHECK(pollOk.load() > 0 && pollOk.load() < polls.load());   // it read both while open and while closed
        CHECK(gate.open());
    }

    sourceChecks();

    f.close();
    hw.stop();
    m.cleanup();
    if (fails) {
        std::fprintf(stderr, "test_core_gate: %d FAIL\n", fails);
        return 1;
    }
    std::printf("test_core_gate: OK\n");
#endif
    return 0;
}
