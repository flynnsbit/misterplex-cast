#pragma once
// F1 (PLEX_IDLE_FB_240P) daemon beat: F1-PREREG-1006 v2.2 (ba1ae917; review 01:39 CLEAR-WITH-BARS
// for code, H264 53b1779c x1-x4, Bug k1-k3; review 01:30 p2/p3).
//
// The 240p F1 core turns its MISTER_FB (the R1 idle buffers, flipped by PLXH) on only while it sees
// the daemon alive: the PLXL word <PLX page> + 0x148 = {hb32 [63:32], 'PLXL' 0x504C584C [31:0]} must
// CHANGE between the core's polls (its first post-reset read only primes; 1.0 s without a change
// turns the FB off at the next VBL rise). This thread is PLXL's only writer:
//  - every 250 ms, and ONLY while the core gate is open on a verified idleBeatPlxl row with a live
//    fabric (p2), the R1 idle target is armed, and no session is up or starting;
//  - a write is hb at +4, then the magic at +0: two 32-bit stores (never a 64-bit store, which an
//    STRD may split magic-first), every beat, so a zeroed word heals at the next beat (x3);
//  - the first hb after a start, or after the gate reopens, is the DDR word's hb + 1 (read back), so
//    a daemon restart is a change for the core;
//  - its ONLY lock is a try_lock of the doorbell-page lock inside BeatPort::store (k1): no spiMutex,
//    no doorbellMu_, no gate mutex, no logging. A held page skips the beat (counted) instead of
//    waiting (x1); a closed gate or an unmapped page skips it without mapping or erroring (x2, p3).
// The counters are atomics; another thread logs them (MediaPlayer's idle paint).
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/fpga_build_ids.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace misterplex {
namespace f1 {

constexpr uint32_t kPlxlOff = 0x148u;            // <PLX page> + 0x148 .. + 0x14F
constexpr uint32_t kPlxlMagic = 0x504C584Cu;     // 'PLXL'
constexpr int64_t kBeatPeriodUs = 250000;        // the core times out after 1.0 s
static_assert(kPlxlOff >= 0x140u + 8u && kPlxlOff + 8u <= 0x1000u,
              "PLXL sits after R1's PLXO (+0x140..0x147) inside the 4 KiB PLX page");

enum class BeatStore { Written, Contended, Unmapped, GateMoved };
enum class BeatOutcome { Written, NotF1, NotIdle, Contended, Unmapped, GateMoved };

// The one daemon call the beat thread makes (FpgaSpi::plxlBeatStore). It takes no lock but a try_lock
// of the doorbell-page lock: under it, it re-checks that the gate is still open with snapshot gateSnap
// (else GateMoved), finds the PLX page already mapped (else Unmapped: it never maps), seeds *hb from the
// word in DDR when !*seeded, increments *hb, and writes hb, then the magic.
struct BeatPort {
    virtual ~BeatPort() = default;
    virtual BeatStore store(uint64_t gateSnap, uint32_t plxlPhys, uint32_t* hb, bool* seeded) = 0;
};

// Lock-free (k1): open() is an atomic, the snapshot is an atomic, the fabric read is a volatile load
// through the gate's registered map, and the row lookup is a table scan. True when the gate is open on
// a verified idleBeatPlxl row with a live fabric; *snap = the open snapshot, *plxlPhys = the row's PLX
// page + kPlxlOff.
inline bool f1GateOpenNow(uint64_t* snap, uint32_t* plxlPhys) {
    const CoreGate& g = CoreGate::process();
    const FpgaBuildInfo* b = nullptr;
    uint64_t s = 0;
    if (g.openAndLiveNoLock()) {
        s = g.openSnapshot();
        b = recognisedFpgaBuild(CoreGate::snapshotBuildId(s));
    } else if (g.idleErrorPaintAllowed() && g.fabricLiveNoLock()) {
        s = g.idleErrorSnap();
        b = recognisedFpgaBuild(g.lastCapsId());
    } else {
        return false;
    }
#ifndef MUT_F1_BEAT_ANYROW   // test-only mutant: the beat writes on any recognised row
    if (!b || !b->idleBeatPlxl)
        return false;
#else
    if (!b)
        return false;
#endif
    const uint32_t page = fpgaPlxPage(b);
    if (!page)
        return false;
    *snap = s;
    *plxlPhys = page + kPlxlOff;
    return true;
}

// paintIdle (media_player.cpp): a row whose core is publish-only with the F1 idle FB never presents
// a host-painted bank, so the present-bank paint (blitDisplayedBank, sendYuv420pFrameDdr, the PLXK
// kick) is dead work there and never runs (review 01:05: the blit excluded on F1 builds).
inline bool idlePaintsPresentBank(const FpgaBuildInfo* gateRow) {
#ifdef MUT_F1_IDLE_LEGACY   // test-only mutant: F1 rows keep the present-bank idle paint
    (void)gateRow;
    return true;
#else
    return !(gateRow && gateRow->idleBeatPlxl);
#endif
}

struct BeatGate {
    // Lock-free; default f1GateOpenNow.
    // Lambda, not the function pointer: on ARM Thumb a std::function bound straight
    // to f1GateOpenNow is a reference to an odd address (UBSan misaligned binding).
    std::function<bool(uint64_t* snap, uint32_t* plxlPhys)> f1Open =
        [](uint64_t* snap, uint32_t* plxlPhys) { return f1GateOpenNow(snap, plxlPhys); };
    // Lock-free: the R1 idle target is armed and no session is up or starting.
    std::function<bool()> idleOk;
};

class Beat {
public:
    Beat(BeatPort& port, BeatGate gate, int64_t periodUs = kBeatPeriodUs)
        : port_(port), gate_(std::move(gate)), periodUs_(periodUs) {}
    ~Beat() { stop(); }
    Beat(const Beat&) = delete;
    Beat& operator=(const Beat&) = delete;

    void start() {
        if (thread_.joinable())
            return;
        stop_.store(false);
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        stop_.store(true);
        if (thread_.joinable())
            thread_.join();
    }
    bool running() const { return thread_.joinable(); }

    // One beat decision (the thread calls it every period; tests call it directly).
    BeatOutcome tick(int64_t nowUs) {
        const auto t0 = std::chrono::steady_clock::now();
        const BeatOutcome o = tickInner(nowUs);
        const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t0).count();
        atomicMax(maxTickUs_, us);
        return o;
    }

    uint64_t writes() const { return writes_.load(); }
    uint64_t notF1() const { return notF1_.load(); }
    uint64_t notIdle() const { return notIdle_.load(); }
    uint64_t contended() const { return contended_.load(); }
    uint64_t unmapped() const { return unmapped_.load(); }
    uint64_t gateMoved() const { return gateMoved_.load(); }
    int64_t maxGapUs() const { return maxGapUs_.load(); }      // between consecutive writes
    int64_t maxTickUs() const { return maxTickUs_.load(); }    // the beat's longest stall
    uint32_t lastHb() const { return lastHb_.load(); }
    void resetStats() {
        maxGapUs_.store(0);
        maxTickUs_.store(0);
        lastWriteUs_ = 0;
    }

#ifdef MPX_TEST_F1_BEAT_SEAM
    // Test seam (this build only): runs between the gate check and the store, on the beat's thread.
    std::function<void()> beforeStore;
#endif

private:
    static void atomicMax(std::atomic<int64_t>& a, int64_t v) {
        int64_t cur = a.load();
        while (v > cur && !a.compare_exchange_weak(cur, v)) {
        }
    }

    BeatOutcome tickInner(int64_t nowUs) {
        uint64_t snap = 0;
        uint32_t plxl = 0;
        if (!gate_.f1Open || !gate_.f1Open(&snap, &plxl)) {
            ++notF1_;
            return BeatOutcome::NotF1;
        }
#ifndef MUT_F1_BEAT_IN_SESSION   // test-only mutant: beats run in a session
        if (gate_.idleOk && !gate_.idleOk()) {
            ++notIdle_;
            return BeatOutcome::NotIdle;
        }
#endif
        if (snap != seededSnap_) {   // a new gate opening (a new core instance): re-seed from DDR
            seededSnap_ = snap;
            seeded_ = false;
        }
#ifdef MPX_TEST_F1_BEAT_SEAM
        if (beforeStore)
            beforeStore();
#endif
        switch (port_.store(snap, plxl, &hb_, &seeded_)) {
        case BeatStore::Written:
            ++writes_;
            lastHb_.store(hb_);
            if (lastWriteUs_ != 0)
                atomicMax(maxGapUs_, nowUs - lastWriteUs_);
            lastWriteUs_ = nowUs;
            return BeatOutcome::Written;
        case BeatStore::Contended:
            ++contended_;
            return BeatOutcome::Contended;
        case BeatStore::Unmapped:
            ++unmapped_;
            return BeatOutcome::Unmapped;
        case BeatStore::GateMoved:
            ++gateMoved_;
            return BeatOutcome::GateMoved;
        }
        return BeatOutcome::GateMoved;
    }

    void run() {
        using Clock = std::chrono::steady_clock;
        const auto period = std::chrono::microseconds(periodUs_);
        auto next = Clock::now() + period;
        while (!stop_.load()) {
            const auto now = Clock::now();
            if (now < next) {
                std::this_thread::sleep_for(std::min<Clock::duration>(next - now, std::chrono::milliseconds(20)));
                continue;
            }
            next += period;
            if (now > next)   // late by more than a period: resync, never a burst
                next = now + period;
            tick(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
        }
    }

    BeatPort& port_;
    BeatGate gate_;
    const int64_t periodUs_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    // The beat thread's own state (tick is single-threaded).
    uint32_t hb_ = 0;
    bool seeded_ = false;
    uint64_t seededSnap_ = 0;
    int64_t lastWriteUs_ = 0;
    std::atomic<uint32_t> lastHb_{0};
    std::atomic<uint64_t> writes_{0}, notF1_{0}, notIdle_{0}, contended_{0}, unmapped_{0}, gateMoved_{0};
    std::atomic<int64_t> maxGapUs_{0}, maxTickUs_{0};
};

}   // namespace f1
}   // namespace misterplex
