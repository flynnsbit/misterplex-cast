// F1 PLXL beat (F1-PREREG-1006 v2.2 ba1ae917; review 01:39 k1-k3, H264 53b1779c x1-x3, review 01:30 p2/p3):
// f1_beat.hpp's Beat over the REAL FpgaSpi::plxlBeatStore in a host build. MPX_DEVMEM_PATH is a sparse
// 4 GiB fake memory file. The core gate is MANAGED (no MPX_CORE_GATE_START_OPEN) and is opened the way the
// daemon's tick opens it (observe, settle, probeResult). The FPGA manager registers in the fake memory
// read "user mode, core out of reset", so the fabric is live.
//  B-a  F1 row 24003997, gate open, the PLX page mapped by an R1 PLXH store (as the R1 painter does), idle:
//       a tick writes PLXL 0x300FF148 = {DDR hb + 1, 'PLXL'} (read-back seed); the next tick writes hb + 1.
//  B-b  p2: the gate open on 2400F40B (a 240p row without idleBeatPlxl): 0 writes (MUT_F1_BEAT_ANYROW).
//  B-c  p2: the gate closed: 0 writes. Reopened (a new generation): re-seeded from DDR.
//  B-d  F-6: a session (idleOk false): 0 writes (MUT_F1_BEAT_IN_SESSION).
//  B-e  p3: the page not mapped (a fresh FpgaSpi, no R1 store): Unmapped, and no mapping of 0x300FF000
//       appears (MUT_F1_BEAT_MAPS).
//  B-f  x1/k2: the doorbell-page lock held 2 s by another thread: every tick in that window is Contended and
//       none waits (stall max <= 250 ms) (MUT_F1_BEAT_BLOCKLOCK).
//  B-g  x2/k2: the gate closes between the beat's check and its store while another thread holds spiMutex for
//       2 s: GateMoved, stall <= 250 ms, page hold <= 10 ms (MUT_F1_BEAT_CHOKEPOINT: the R1 choke point
//       plxStore32 -> coreGateAllows -> setErr waits on spiMutex).
//  B-h  x3: a zeroed PLXL word heals at the next beat (MUT_F1_BEAT_MAGIC_ONCE).
//  B-i  restart: a new Beat with hb 1 already in DDR writes hb 2 first (MUT_F1_BEAT_NOSEED).
//  B-j  f4/k2 cadence in real time: the thread at 250 ms for 3 s, with spiMutex held 2 s in the middle by
//       another thread: 10..13 writes, max gap <= 500 ms (2 periods: half the core's 1.0 s timeout), stall
//       max <= 250 ms, page hold max <= 10 ms; the gap histogram is printed.
//  B-l  paintIdle predicate (media_player.cpp): idlePaintsPresentBank is false on every idleBeatPlxl row
//       (MUT_F1_IDLE_LEGACY).
//  B-k  k1, TSan build ("tsan" argument, test_f1_plxl_beat_tsan): the beat thread at a 1 ms period against
//       PLXH stores, PLXO loads, layout changes (releaseDdrMap: unmap and remap), gate close/reopen cycles and
//       fabric reprogram/live flips. Must give no report (TSAN_OPTIONS halt_on_error=1). Out of scope:
//       onCoreReturned/kicks vs setDdrFrameLayout (ddrKickMode_/doorbellSeq_, pre-existing, T-BLIT T6). The
//       beat reads none of those.
#include "fpga_spi.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/f1_beat.hpp"
#include "libmisterplex/r1_idle_fb.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace misterplex {
std::recursive_mutex& mpxTestSpiMutex();
extern std::atomic<int64_t> mpxTestF1BeatPageHoldMaxUs;
struct F1BeatTest {
    static std::unique_lock<std::mutex> lockPage(FpgaSpi& f) { return f.lockDdrPage(); }
    // A second FpgaSpi's close() drops the gate's registered fabric map; give it back to f.
    static void reregisterFabric(FpgaSpi& f) { CoreGate::process().setFabricRegs(f.map_); }
};
}   // namespace misterplex

namespace {

using namespace misterplex;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kF1Id = 0x24003997u;
constexpr uint32_t kPlxl = 0x300FF148u;

int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                                                                    \
    do {                                                                                                    \
        ++g_checks;                                                                                         \
        if (!(cond)) {                                                                                      \
            ++g_fails;                                                                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                \
            std::printf(__VA_ARGS__);                                                                       \
            std::printf("\n");                                                                              \
        }                                                                                                   \
    } while (0)
// A row's verdict line: PASS only if no CHECK failed since the row began (f0 = g_fails at entry).
#define ROW_VERDICT(tag) std::printf("%s %s ", tag, g_fails == f0 ? "PASS" : "FAIL")

bool makeSparse(const char* path, off_t size) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    const bool ok = ::ftruncate(fd, size) == 0;
    ::close(fd);
    return ok;
}
// The fake memory through the file (never a mapping of our own; a MAP_SHARED write is in the page cache).
bool poke32(uint32_t phys, uint32_t v) {
    const int fd = ::open(MPX_DEVMEM_PATH, O_RDWR | O_CLOEXEC);
    const bool ok = fd >= 0 && ::pwrite(fd, &v, 4, static_cast<off_t>(phys)) == 4;
    if (fd >= 0)
        ::close(fd);
    return ok;
}
uint32_t peek32(uint32_t phys) {
    uint32_t v = 0xDEADBEEFu;
    const int fd = ::open(MPX_DEVMEM_PATH, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (::pread(fd, &v, 4, static_cast<off_t>(phys)) != 4)
            v = 0xDEADBEEFu;
        ::close(fd);
    }
    return v;
}
void fabricLive(bool live) {
    poke32(kFpgaMgrStatPhys, live ? kFpgaMgrModeUser : 1u);   // 1 = not user mode: reprogramming
    poke32(kFpgaMgrGpoPhys, 0u);
}
// Mappings of the fake memory at the PLX page's offset (/proc/self/maps).
int pageMaps(uint32_t page) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    int n = 0;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::string range, perms, off, dev, inode, path;
        in >> range >> perms >> off >> dev >> inode >> path;
        if (path == MPX_DEVMEM_PATH && std::strtoull(off.c_str(), nullptr, 16) == page)
            ++n;
    }
    return n;
}

// The daemon's tick, condensed (test_core_gate.cpp Sim): a live fabric and a fresh CORENAME, the settle,
// one CAPS probe answered with `id` on the 240p raster.
bool openGate(uint32_t id) {
    CoreGate& g = CoreGate::process();
    g.resetForTest(false);   // managed and closed, as a new daemon has it
    static int64_t now = 5000000000;
    now += 10000000;
    CoreObservation o;
    o.fabric = FabricState{true, kFpgaMgrModeUser, false};
    o.main = MainIdentity{100, 100};
    o.mainStartWallNs = 500;
    o.bootWallNs = 200;
    o.coreName = CoreFileStamp{true, "Plex", 1000};
    o.rbfName = CoreFileStamp{true, "Plex_240p", 1000};
    o.nowUs = now;
    (void)g.observe(o);
    now += CoreGate::kSettleUs;
    o.nowUs = now;
    if (g.observe(o) != CoreGateEvent::ProbeWanted)
        return false;
    return g.probeResult(true, id, "", "240p", "row", now) == CoreGateEvent::Opened && g.open();
}

struct SpiPort final : f1::BeatPort {
    explicit SpiPort(FpgaSpi& f) : fpga(f) {}
    f1::BeatStore store(uint64_t snap, uint32_t plxl, uint32_t* hb, bool* seeded) override {
        return fpga.plxlBeatStore(snap, plxl, hb, seeded);
    }
    FpgaSpi& fpga;
};
// B-j only: records each write's time (test scaffolding around the real store).
struct TimedPort final : f1::BeatPort {
    explicit TimedPort(FpgaSpi& f) : fpga(f) {}
    f1::BeatStore store(uint64_t snap, uint32_t plxl, uint32_t* hb, bool* seeded) override {
        const f1::BeatStore r = fpga.plxlBeatStore(snap, plxl, hb, seeded);
        if (r == f1::BeatStore::Written) {
            std::lock_guard<std::mutex> lk(mu);
            at.push_back(Clock::now());
        }
        return r;
    }
    FpgaSpi& fpga;
    std::mutex mu;
    std::vector<Clock::time_point> at;
};

std::atomic<bool> g_idle{true};
f1::BeatGate gateHooks() { return f1::BeatGate{f1::f1GateOpenNow, [] { return g_idle.load(); }}; }
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
// The R1 painter's first PLXH store maps the PLX page through the one mapper.
bool mapPlxPageLikeR1(FpgaSpi& f) { return f.plxStore32(r1::kPlxhPhys, 0u) && pageMaps(0x300FF000u) == 1; }
const char* outcome(f1::BeatOutcome o) {
    switch (o) {
    case f1::BeatOutcome::Written: return "Written";
    case f1::BeatOutcome::NotF1: return "NotF1";
    case f1::BeatOutcome::NotIdle: return "NotIdle";
    case f1::BeatOutcome::Contended: return "Contended";
    case f1::BeatOutcome::Unmapped: return "Unmapped";
    case f1::BeatOutcome::GateMoved: return "GateMoved";
    }
    return "?";
}

void rowA(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-a: gate did not open on 24003997");
    CHECK(mapPlxPageLikeR1(f), "B-a: the R1 PLXH store did not map the PLX page once: %s", f.lastError().c_str());
    poke32(kPlxl + 4, 41u);
    poke32(kPlxl, 0u);
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    const f1::BeatOutcome o1 = b.tick(nowUs());
    CHECK(o1 == f1::BeatOutcome::Written, "B-a: first tick %s", outcome(o1));
    CHECK(peek32(kPlxl + 4) == 42u && peek32(kPlxl) == f1::kPlxlMagic, "B-a: PLXL %08x:%08x, want 0000002a:504c584c",
          peek32(kPlxl + 4), peek32(kPlxl));
    const f1::BeatOutcome o2 = b.tick(nowUs());
    CHECK(o2 == f1::BeatOutcome::Written && peek32(kPlxl + 4) == 43u, "B-a: second tick %s hb %u", outcome(o2),
          peek32(kPlxl + 4));
    CHECK(peek32(r1::kPlxhPhys + 8) == 0u && peek32(r1::kPlxoPhys) == 0u && peek32(kPlxl + 8) == 0u,
          "B-a: a neighbour word changed");
    ROW_VERDICT("B-a");
    std::printf("F1 row: PLXL 0x300FF148 = {DDR hb + 1, PLXL} then hb + 1 (writes %llu)\n",
                static_cast<unsigned long long>(b.writes()));
}

void rowB(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(0x2400F40Bu), "B-b: gate did not open on 2400F40B");
    CHECK(mapPlxPageLikeR1(f) || pageMaps(0x300FF000u) == 1, "B-b: PLX page not mapped");
    const uint32_t before = peek32(kPlxl + 4);
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    for (int i = 0; i < 8; ++i)
        b.tick(nowUs());
    CHECK(b.writes() == 0 && peek32(kPlxl + 4) == before, "B-b: %llu writes on a row without idleBeatPlxl",
          static_cast<unsigned long long>(b.writes()));
    ROW_VERDICT("B-b");
    std::printf("2400F40B: 0 writes (not_f1 %llu)\n", static_cast<unsigned long long>(b.notF1()));
}

void rowC(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-c: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-c: PLX page not mapped by the R1 store");
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    CHECK(b.tick(nowUs()) == f1::BeatOutcome::Written, "B-c: no write with the gate open");
    const uint32_t hbOpen = peek32(kPlxl + 4);
    CoreGate::process().closeNow("B-c: a reload");
    for (int i = 0; i < 6; ++i)
        b.tick(nowUs());
    CHECK(b.writes() == 1 && peek32(kPlxl + 4) == hbOpen, "B-c: wrote with the gate closed");
    poke32(kPlxl + 4, 1000u);   // what a reload leaves in DDR does not matter: the reopen re-seeds from it
    CHECK(openGate(kF1Id), "B-c: reopen failed");
    CHECK(b.tick(nowUs()) == f1::BeatOutcome::Written && peek32(kPlxl + 4) == 1001u,
          "B-c: after a reopen the first hb is %u, want 1001 (re-seeded)", peek32(kPlxl + 4));
    ROW_VERDICT("B-c");
    std::printf("gate closed: 0 writes; reopened: re-seeded (hb 1001)\n");
}

void rowD(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-d: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-d: PLX page not mapped by the R1 store");
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    const uint32_t before = peek32(kPlxl + 4);
    g_idle.store(false);
    for (int i = 0; i < 6; ++i)
        b.tick(nowUs());
    g_idle.store(true);
    CHECK(b.writes() == 0 && b.notIdle() == 6 && peek32(kPlxl + 4) == before, "B-d: %llu writes in a session",
          static_cast<unsigned long long>(b.writes()));
    ROW_VERDICT("B-d");
    std::printf("in a session: 0 writes\n");
}

void rowE(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-e: gate did not open");
    {
        FpgaSpi g2;   // a daemon that has made no R1 store: the page is not mapped
        CHECK(g2.open(), "B-e: open: %s", g2.lastError().c_str());
        const int maps0 = pageMaps(0x300FF000u);
        SpiPort port(g2);
        f1::Beat b(port, gateHooks());
        const f1::BeatOutcome o = b.tick(nowUs());
        CHECK(o == f1::BeatOutcome::Unmapped, "B-e: %s, want Unmapped", outcome(o));
        CHECK(pageMaps(0x300FF000u) == maps0, "B-e: the beat mapped the PLX page (%d -> %d maps)", maps0,
              pageMaps(0x300FF000u));
    }
    F1BeatTest::reregisterFabric(f);
    ROW_VERDICT("B-e");
    std::printf("page not mapped: Unmapped, no map made\n");
}

void rowF(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-f: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-f: PLX page not mapped by the R1 store");
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    std::atomic<bool> held{false};
    std::thread holder([&] {
        auto lk = F1BeatTest::lockPage(f);
        held.store(true);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    while (!held.load())
        std::this_thread::yield();
    b.resetStats();
    int contended = 0;
    for (int i = 0; i < 8; ++i) {
        contended += b.tick(nowUs()) == f1::BeatOutcome::Contended;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    holder.join();
    CHECK(contended == 8, "B-f: %d of 8 ticks Contended while the page was held", contended);
    CHECK(b.maxTickUs() <= 250000, "B-f: a tick stalled %lld us on the held page", static_cast<long long>(b.maxTickUs()));
    CHECK(b.tick(nowUs()) == f1::BeatOutcome::Written, "B-f: no write after the page was released");
    ROW_VERDICT("B-f");
    std::printf("page held 2 s: 8/8 Contended, max stall %lld us\n", static_cast<long long>(b.maxTickUs()));
}

void rowG(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-g: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-g: PLX page not mapped by the R1 store");
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    std::atomic<bool> held{false};
    std::thread holder([&] {
        std::lock_guard<std::recursive_mutex> lk(mpxTestSpiMutex());
        held.store(true);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    while (!held.load())
        std::this_thread::yield();
    b.beforeStore = [] { CoreGate::process().closeNow("B-g: a reload lands between the check and the store"); };
    b.resetStats();
    mpxTestF1BeatPageHoldMaxUs.store(0);
    const f1::BeatOutcome o = b.tick(nowUs());
    b.beforeStore = nullptr;
    holder.join();
    CHECK(o == f1::BeatOutcome::GateMoved, "B-g: %s, want GateMoved", outcome(o));
    CHECK(b.maxTickUs() <= 250000, "B-g: the beat stalled %lld us behind spiMutex", static_cast<long long>(b.maxTickUs()));
    CHECK(mpxTestF1BeatPageHoldMaxUs.load() <= 10000, "B-g: page held %lld us",
          static_cast<long long>(mpxTestF1BeatPageHoldMaxUs.load()));
    ROW_VERDICT("B-g");
    std::printf("gate closed under a held spiMutex: GateMoved, stall %lld us, page hold %lld us\n",
                static_cast<long long>(b.maxTickUs()), static_cast<long long>(mpxTestF1BeatPageHoldMaxUs.load()));
}

void rowH(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-h: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-h: PLX page not mapped by the R1 store");
    SpiPort port(f);
    f1::Beat b(port, gateHooks());
    CHECK(b.tick(nowUs()) == f1::BeatOutcome::Written, "B-h: no first write");
    poke32(kPlxl, 0u);
    poke32(kPlxl + 4, 0u);
    CHECK(b.tick(nowUs()) == f1::BeatOutcome::Written, "B-h: no write after the zeroing");
    CHECK(peek32(kPlxl) == f1::kPlxlMagic && peek32(kPlxl + 4) != 0u, "B-h: PLXL %08x:%08x after one beat",
          peek32(kPlxl + 4), peek32(kPlxl));
    ROW_VERDICT("B-h");
    std::printf("a zeroed PLXL heals at the next beat\n");
}

void rowI(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-i: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-i: PLX page not mapped by the R1 store");
    poke32(kPlxl + 4, 1u);
    poke32(kPlxl, f1::kPlxlMagic);
    SpiPort port(f);
    f1::Beat b(port, gateHooks());   // a restarted daemon
    CHECK(b.tick(nowUs()) == f1::BeatOutcome::Written && peek32(kPlxl + 4) == 2u,
          "B-i: a restart's first hb is %u, want 2 (DDR held 1)", peek32(kPlxl + 4));
    ROW_VERDICT("B-i");
    std::printf("restart: first hb = DDR hb + 1\n");
}

void rowJ(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-j: gate did not open");
    CHECK(mapPlxPageLikeR1(f), "B-j: PLX page not mapped by the R1 store");
    TimedPort port(f);
    f1::Beat b(port, gateHooks());
    mpxTestF1BeatPageHoldMaxUs.store(0);
    const auto t0 = Clock::now();
    b.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::thread holder([] {
        std::lock_guard<std::recursive_mutex> lk(mpxTestSpiMutex());   // a 2 s SPI window
        std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    holder.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    b.stop();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::vector<int64_t> gaps;
    for (size_t i = 1; i < port.at.size(); ++i)
        gaps.push_back(std::chrono::duration_cast<std::chrono::microseconds>(port.at[i] - port.at[i - 1]).count());
    std::sort(gaps.begin(), gaps.end());
    const int64_t maxGap = gaps.empty() ? 0 : gaps.back();
    const int64_t p50 = gaps.empty() ? 0 : gaps[gaps.size() / 2];
    CHECK(b.writes() >= 10 && b.writes() <= 13, "B-j: %llu writes in %.2f s (want 10..13 at 250 ms)",
          static_cast<unsigned long long>(b.writes()), secs);
    CHECK(maxGap <= 500000, "B-j: max gap %lld us (bar 500 ms)", static_cast<long long>(maxGap));
    CHECK(b.maxTickUs() <= 250000 && mpxTestF1BeatPageHoldMaxUs.load() <= 10000,
          "B-j: stall %lld us, page hold %lld us", static_cast<long long>(b.maxTickUs()),
          static_cast<long long>(mpxTestF1BeatPageHoldMaxUs.load()));
    ROW_VERDICT("B-j");
    std::printf("cadence: %llu writes in %.2f s with a 2 s spiMutex hold; gaps us p50 %lld max %lld;"
                " stall max %lld us; page hold max %lld us; gaps:",
                static_cast<unsigned long long>(b.writes()), secs, static_cast<long long>(p50),
                static_cast<long long>(maxGap), static_cast<long long>(b.maxTickUs()),
                static_cast<long long>(mpxTestF1BeatPageHoldMaxUs.load()));
    for (int64_t g : gaps)
        std::printf(" %lld", static_cast<long long>(g / 1000));
    std::printf(" ms\n");
}

void rowL() {
    const int f0 = g_fails;
    int f1Rows = 0;
    int beatRows = 0;
    for (const FpgaBuildInfo& r : kRecognisedFpgaBuilds) {
        if (r.id == kF1Id)
            f1Rows += 1;
        if (r.idleBeatPlxl)
            beatRows += 1;
        CHECK(f1::idlePaintsPresentBank(&r) == !r.idleBeatPlxl, "B-l: row %08x present-bank paint %d", r.id,
              int(f1::idlePaintsPresentBank(&r)));
    }
    CHECK(f1Rows == 1 && beatRows >= 1 && f1::idlePaintsPresentBank(nullptr),
          "B-l: the F1 row is not in the table once");
    ROW_VERDICT("B-l");
    std::printf("paintIdle: the present-bank paint is skipped on every idleBeatPlxl row (%d)\n", beatRows);
}

void rowK(FpgaSpi& f) {
    const int f0 = g_fails;
    CHECK(openGate(kF1Id), "B-k: gate did not open");
    CHECK(mapPlxPageLikeR1(f) || pageMaps(0x300FF000u) >= 1, "B-k: PLX page not mapped");
    SpiPort port(f);
    f1::Beat b(port, gateHooks(), 1000);   // 1 ms: stress
    std::atomic<bool> stop{false};
    b.start();
    std::thread plxh([&] {
        uint32_t t = 1;
        while (!stop.load()) {
            (void)f.plxStore32(r1::kPlxhPhys + 4, t++ << 16);
            (void)f.plxStore32(r1::kPlxhPhys, 0x504C5848u);
        }
    });
    std::thread plxo([&] {
        uint64_t w = 0;
        while (!stop.load())
            (void)f.plxLoad64(r1::kPlxoPhys, &w);
    });
    std::thread layout([&] {
        bool big = false;
        while (!stop.load()) {
            big = !big;
            (void)(big ? f.setDdrFrameLayout(640, 480) : f.setDdrFrameLayout(320, 240));   // releaseDdrMap
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::thread gate([&] {
        int i = 0;
        while (!stop.load()) {
            if ((++i & 3) == 0) {   // a reprogram window: the fabric reads not live, then live again
                fabricLive(false);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                fabricLive(true);
            }
            CoreGate::process().closeNow("B-k: reload");
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            (void)openGate(kF1Id);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::this_thread::sleep_for(std::chrono::seconds(4));
    stop.store(true);
    plxh.join();
    plxo.join();
    layout.join();
    gate.join();
    b.stop();
    CHECK(b.writes() > 0 && (b.unmapped() + b.gateMoved() + b.notF1() + b.contended()) > 0,
          "B-k: the stress exercised no skip path (writes %llu)", static_cast<unsigned long long>(b.writes()));
    ROW_VERDICT("B-k");
    std::printf("TSan stress 4 s: writes %llu not_f1 %llu contended %llu unmapped %llu gate_moved %llu"
                " (no TSan report)\n",
                static_cast<unsigned long long>(b.writes()), static_cast<unsigned long long>(b.notF1()),
                static_cast<unsigned long long>(b.contended()), static_cast<unsigned long long>(b.unmapped()),
                static_cast<unsigned long long>(b.gateMoved()));
}

}   // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // argv[1]: "tsan" (B-k only), or one row tag ("B-a".."B-l") to run that row alone (the mutant-check
    // runs each mutant on its pre-registered row: KILLED = rc 1 + a first FAIL line naming that row).
    const std::string only = argc > 1 ? argv[1] : "";
    const bool tsan = only == "tsan";
    auto want = [&](const char* tag) { return only.empty() || only == tag; };
    if (!makeSparse(MPX_DEVMEM_PATH, static_cast<off_t>(1) << 32)) {
        std::printf("test_f1_plxl_beat: cannot create the fake memory file\n");
        return 1;
    }
    fabricLive(true);
    int rows = 0;
    {
        FpgaSpi f;
        CHECK(f.open(), "open: %s", f.lastError().c_str());
        if (tsan) {
            rowK(f);
            ++rows;
        } else {
            if (want("B-a")) { rowA(f); ++rows; }
            if (want("B-b")) { rowB(f); ++rows; }
            if (want("B-c")) { rowC(f); ++rows; }
            if (want("B-d")) { rowD(f); ++rows; }
            if (want("B-e")) { rowE(f); ++rows; }
            if (want("B-f")) { rowF(f); ++rows; }
            if (want("B-g")) { rowG(f); ++rows; }
            if (want("B-h")) { rowH(f); ++rows; }
            if (want("B-i")) { rowI(f); ++rows; }
            if (want("B-j")) { rowJ(f); ++rows; }
            if (want("B-l")) { rowL(); ++rows; }
        }
    }
    ::unlink(MPX_DEVMEM_PATH);
    if (!rows) {
        std::printf("test_f1_plxl_beat: no row named '%s'\n", only.c_str());
        return 2;
    }
    if (g_fails) {
        std::printf("test_f1_plxl_beat%s%s: %d FAIL of %d checks\n", only.empty() ? "" : " ", only.c_str(), g_fails, g_checks);
        return 1;
    }
    std::printf("test_f1_plxl_beat%s%s: OK (%d rows, %d checks)\n", only.empty() ? "" : " ", only.c_str(), rows, g_checks);
    return 0;
}
