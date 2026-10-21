#pragma once

// HPS bank ingest (w-path). Parent 2026-08-03 architecture lock (+ incident pivot):
//
// /dev/mem STRUCTURAL (narrow claim): bank at 0x30xx_xxxx is OUTSIDE kernel
// System RAM (mem=511M memmap=513M$511M; iomem RAM ends 0x1fefffff). pfn_valid
// is false → kernel forces pgprot_noncached; O_SYNC is inert. Measured CPU
// store ceiling ~88 MiB/s (~14.97 ms/f for 720p I420) applies to **/dev/mem
// mappings of the reserved region only** — not to a dedicated driver that maps
// pgprot_writecombine (rd-duck; parent accepted).
//
// Modes (product default remains CpuSerial via existing sendDdrFrame):
//   CpuSerial         — memcpy into /dev/mem bank (DEAD for true 720p24 source)
//   DecodeIntoBank    — SUSPECT on noncached map; do not productize yet
//   Pl330Userspace    — LAB ONLY. Raw DMAGO fights live dma-pl330; unbind hung
//                       the SoC. Not a product path (parent incident 2026-08-03).
//   KernelDma         — PRODUCT DMA route: dmaengine client / small module
//                       Retires uncached *publication* memcpy ONLY after pinned
//                       contiguous/SG + cache-coherency contract (rd-duck).
//                       Does NOT mean "ARM never touches pixels" — software
//                       decode/rawvideo still writes a source buffer first.
//   CpuWriteCombine   — PRODUCT serial upgrade: driver WC map + CPU memcpy
//   FabricDirectReader— STRATEGIC PREFER (w-mem/fabric): dynamic-base reader of
//                       decode output; avoids source→bank mover (extra R+W).
//
// Ship product glass path is still 960×540 source → fabric upscale → 720p out.
// True 720p *source* needs KernelDma and/or CpuWriteCombine ≥ ~230 MiB/s, or
// fabric-direct publish. Source→bank DMA mover is dispreferred (adds traffic).
//
// Sweep116 49% idle is IDLE-AT-REST (sampled before decode). Do not budget a
// free core concurrent with decode until same-window /proc/stat+wait4.
//
// Doorbell ordering (absolute): payload FPGA-visible → real barrier → doorbell.
// Coordinate w-mem: bank/doorbell ABI; fabric_ddr_writer = fabric producer.

#include "libmisterplex/ddr_frame_layout.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace misterplex {

enum class DdrIngestMode : uint8_t {
    CpuSerial = 0,
    DecodeIntoBank = 1,
    Pl330Overlap = 2,       // alias: Pl330Userspace (lab/diagnostic only)
    Pl330Userspace = 2,     // same value — raw register poke; NOT product
    KernelDma = 3,          // dmaengine owner (product DMA)
    CpuWriteCombine = 4,    // WC-mapped bank + CPU memcpy (product serial upgrade)
};

enum class DdrIngestPhase : uint8_t {
    Idle = 0,
    AwaitBankCredit = 1, // soft wait — must NOT collapse to Failed
    PayloadMoving = 2,
    PayloadVisible = 3, // safe to ring doorbell after visibility barrier
    DoorbellRung = 4,
    Failed = 5,
};

struct DdrIngestJob {
    const uint8_t* src_virt = nullptr; // CpuSerial / Pl330 source (RAM)
    uint32_t src_phys = 0;             // Pl330: physical source (0 = unresolved)
    uint8_t* bank_virt = nullptr;      // mapped bank window
    uint32_t bank_phys = 0;
    size_t len = 0;
    int bank = 0; // 0/1
    bool plxd_credit_held = false;
};

struct DdrIngestTiming {
    int64_t wait_credit_us = 0;
    int64_t move_us = 0; // CPU copy or DMA wall
    int64_t barrier_us = 0;
    int64_t doorbell_us = 0;
    bool used_dma = false;
    bool decode_into_bank = false;
};

struct DdrIngestContract {
    DdrIngestMode mode = DdrIngestMode::CpuSerial;
    // When true, ring is refused unless phase==PayloadVisible.
    bool enforce_payload_before_doorbell = true;
    // PLXD path: require credit before fill (product when mailbox live).
    bool require_bank_credit = false;
};

// --- Budget helpers (parent-measured constants; do not invent device rates) ---

constexpr double kIngestBudget24fpsMs = 41.667;
// Parent CLEAN budget 2026-08-03 (post-awk; nosync 0/5; anomaly CLOSED):
//   decode alone 35.94 (margin 5.73); CPU bank copy 14.978 STRUCTURAL uncached
//   serial 50.9+ FAIL closed; DMA verified sustained = THE open ARM question
//   DecodeIntoBank suspect (same uncached stores) — do not build further yet
//
// rd-duck archive attack (Memory/.../misterplex-parent-720p-decode-verdict.txt):
//   :303-319  ddr_write_bench 1382400 → 88.022 MiB/s, 14.978 ms/f (14.763 CPU)
//             serial decode 35.94 + copy 14.978 = 50.92 > 41.667 FAIL by 9.25
//   :354-385  O_SYNC inert — bank outside System RAM → uncached /dev/mem always
//   Sweep-116 1500k decode 32.705 ms/f → headroom 8.962 < 14.978 → STILL FAIL
//   Product path: media_player → sendYuv420pFrameDdr → sendDdrFrame memcpy
//   Caveat: FFmpeg child pipe MAY overlap decode|copy — need product A/B; do NOT
//           claim strict sum solely from call graph. Fabric beat-budget sim ≠ ARM
//           uncached store cost (w-mem G4 proves fabric side only).
constexpr double kParentBankCopy720pMs = 14.978;
constexpr double kParentBankCopy720pMiBps = 88.045; // --sync control, structural
constexpr double kParentDecode720pCleanAloneMs = 35.94;
// Sweep-116 (parent/rd-duck): faster decode still leaves insufficient headroom.
constexpr double kParentDecode720pSweep116Ms = 32.705;
constexpr double kParentSweep116HeadroomMs =
    kIngestBudget24fpsMs - kParentDecode720pSweep116Ms; // 8.962
// true ONLY for /dev/mem noncached maps of reserved banks (parent narrowed claim).
// A driver WC map is a separate falsifiable route (target ≥ kParentTrue720pCopyMiBpsNeed).
constexpr bool kParentCpuBankCopyIsStructuralCeiling = true;
constexpr bool kParentCpuBankCopyCeilingIsDevMemOnly = true;
// True 720p source serial: need copy ≤ (41.667 - 35.94) = 5.727 ms → ≥230.2 MiB/s.
constexpr double kParentTrue720pCopyBudgetMs = 5.727;
constexpr double kParentTrue720pCopyMiBpsNeed = 230.2; // parent; ~2.615× /dev/mem rate
constexpr double kParentDecode720p2500kMs = 35.87; // earlier same-session arm
constexpr double kParentDecode720pProduct2600kMs = kParentDecode720pCleanAloneMs;
constexpr double kParentDecode720pApprox2600kMs = kParentDecode720pProduct2600kMs;
// Universal geometry floor content ~2585k after maxBR=3100 (w-path fine matrix).

// Hardware + compiler barrier before doorbell. atomic_signal_fence is NOT enough
// (compiler-only; rd-duck audit). Match fpga_spi.cpp __sync_synchronize() → ARM DMB.
inline void ddrIngestPayloadVisibilityBarrier() {
#if defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

inline bool ddrIngestSerialDecodePlusCopyFits(double decodeMs,
                                              double copyMs = kParentBankCopy720pMs,
                                              double budgetMs = kIngestBudget24fpsMs) {
    return (decodeMs + copyMs) <= budgetMs;
}

inline bool ddrIngestTrue720pSerialCopyRateFits(double copyMiBps) {
    return copyMiBps >= kParentTrue720pCopyMiBpsNeed;
}

// Headroom after decode must cover measured /dev/mem bank copy — Sweep-116 fails.
inline bool ddrIngestHeadroomCoversDevMemCopy(double decodeMs,
                                              double copyMs = kParentBankCopy720pMs,
                                              double budgetMs = kIngestBudget24fpsMs) {
    return (budgetMs - decodeMs) >= copyMs;
}

// Full-frame true-720p I420 (1382400 B): CpuSerial into /dev/mem is product-forbidden
// by parent arithmetic (not opinion). Smaller native/540p sources may still use it.
inline bool ddrIngestCpuSerialAllowedForPayload(size_t payload_bytes) {
    return payload_bytes < static_cast<size_t>(kPlex720pYuv420pBytes);
}

// Fabric beat budget (w-mem Verilator G4) must NEVER be cited as the ARM copy cost.
// ARM uncached store is structural ~88 MiB/s; fabric peak theoretical 720 MB/s.
inline bool ddrIngestFabricBwIsNotArmCopyCost() { return true; }

// Route A sketch: throughput ~ max(decode, copy). NOT proven under DDR contention.
// Product pipeline A/B required before shipping as PASS (rd-duck pipe-overlap caveat).
inline bool ddrIngestOverlapMaxFits(double decodeMs, double copyMs = kParentBankCopy720pMs,
                                    double budgetMs = kIngestBudget24fpsMs) {
    const double m = (decodeMs > copyMs) ? decodeMs : copyMs;
    return m <= budgetMs;
}

// Overlap claim is UNSCORED as a product ship gate until parent measures pipeline A/B.
inline bool ddrIngestOverlapIsProductShipGate() { return false; }

inline const char* ddrIngestModeName(DdrIngestMode m) {
    switch (m) {
    case DdrIngestMode::CpuSerial:
        return "cpu_serial";
    case DdrIngestMode::DecodeIntoBank:
        return "decode_into_bank";
    case DdrIngestMode::Pl330Overlap: // Pl330Userspace
        return "pl330_userspace_lab";
    case DdrIngestMode::KernelDma:
        return "kernel_dma";
    case DdrIngestMode::CpuWriteCombine:
        return "cpu_write_combine";
    }
    return "unknown";
}

inline bool ddrIngestModeIsProductCandidate(DdrIngestMode m) {
    // CpuSerial remains product for sub-720p source only (see
    // ddrIngestCpuSerialAllowedForPayload). True-720p source needs DMA/WC.
    return m == DdrIngestMode::KernelDma || m == DdrIngestMode::CpuWriteCombine ||
           m == DdrIngestMode::CpuSerial;
}

// True-720p source product modes: KernelDma or measured WC ≥ 230.2 MiB/s.
inline bool ddrIngestModeOkForTrue720pSource(DdrIngestMode m) {
    return m == DdrIngestMode::KernelDma || m == DdrIngestMode::CpuWriteCombine;
}

inline bool ddrIngestModeIsLabOnly(DdrIngestMode m) {
    return m == DdrIngestMode::Pl330Userspace || m == DdrIngestMode::DecodeIntoBank;
}

inline DdrIngestMode ddrIngestModeFromConf(const char* s) {
    if (!s || !*s)
        return DdrIngestMode::CpuSerial;
    if (std::strcmp(s, "kernel_dma") == 0 || std::strcmp(s, "dmaengine") == 0)
        return DdrIngestMode::KernelDma;
    if (std::strcmp(s, "write_combine") == 0 || std::strcmp(s, "wc") == 0 ||
        std::strcmp(s, "cpu_wc") == 0)
        return DdrIngestMode::CpuWriteCombine;
    // Raw PL330 still parseable for lab benches — never the product default.
    if (std::strcmp(s, "pl330") == 0 || std::strcmp(s, "pl330_overlap") == 0 ||
        std::strcmp(s, "pl330_userspace") == 0)
        return DdrIngestMode::Pl330Userspace;
    if (std::strcmp(s, "dma") == 0)
        return DdrIngestMode::KernelDma; // "dma" now means kernel owner, not raw poke
    if (std::strcmp(s, "decode_into_bank") == 0 || std::strcmp(s, "into_bank") == 0)
        return DdrIngestMode::DecodeIntoBank;
    return DdrIngestMode::CpuSerial;
}

// State machine: fill → visible → doorbell. Unit-tested; backends plug in.
class DdrIngestSession {
public:
    explicit DdrIngestSession(DdrIngestContract c = {}) : c_(c) {}

    DdrIngestPhase phase() const { return phase_; }
    const DdrIngestTiming& timing() const { return timing_; }
    const std::string& lastError() const { return err_; }

    bool begin(const DdrIngestJob& job) {
        err_.clear();
        timing_ = {};
        job_ = job;
        if (!job_.bank_virt || job_.len == 0 || (job_.bank != 0 && job_.bank != 1)) {
            fail("begin: bad bank/len");
            return false;
        }
        if (c_.mode != DdrIngestMode::DecodeIntoBank && !job_.src_virt) {
            fail("begin: null src for non-decode-into-bank mode");
            return false;
        }
        if (c_.require_bank_credit && !job_.plxd_credit_held) {
            // Soft wait — do NOT call fail() (that collapsed this to Failed; rd-duck).
            phase_ = DdrIngestPhase::AwaitBankCredit;
            err_ = "begin: PLXD bank credit required but not held";
            return false;
        }
        phase_ = DdrIngestPhase::PayloadMoving;
        return true;
    }

    // Resume after AwaitBankCredit once PLXD credit is held.
    bool continueAfterBankCredit(bool credit_held) {
        if (phase_ != DdrIngestPhase::AwaitBankCredit) {
            fail("continueAfterBankCredit: not awaiting credit");
            return false;
        }
        if (!credit_held) {
            err_ = "continueAfterBankCredit: credit still not held";
            return false; // stay AwaitBankCredit
        }
        job_.plxd_credit_held = true;
        err_.clear();
        phase_ = DdrIngestPhase::PayloadMoving;
        return true;
    }

    // CpuSerial / CpuWriteCombine backend: blocking memcpy + real DMB.
    bool completeCpuMemcpy() {
        if (phase_ != DdrIngestPhase::PayloadMoving) {
            fail("completeCpuMemcpy: bad phase");
            return false;
        }
        if (!job_.src_virt) {
            fail("completeCpuMemcpy: null src");
            return false;
        }
        std::memcpy(job_.bank_virt, job_.src_virt, job_.len);
        ddrIngestPayloadVisibilityBarrier();
        timing_.move_us = 0; // caller may overwrite with wall time
        timing_.used_dma = false;
        phase_ = DdrIngestPhase::PayloadVisible;
        return true;
    }

    // DecodeIntoBank: decoder already filled bank_virt; mark visible after barrier.
    bool completeDecodeIntoBankBarrier() {
        if (phase_ != DdrIngestPhase::PayloadMoving) {
            fail("completeDecodeIntoBankBarrier: bad phase");
            return false;
        }
        ddrIngestPayloadVisibilityBarrier();
        timing_.decode_into_bank = true;
        timing_.used_dma = false;
        phase_ = DdrIngestPhase::PayloadVisible;
        return true;
    }

    // KernelDma (or lab PL330) success path after engine reports completion.
    bool completeDmaDone() {
        if (phase_ != DdrIngestPhase::PayloadMoving) {
            fail("completeDmaDone: bad phase");
            return false;
        }
        // DMA completion ≠ doorbell-safe without a visibility barrier on weakly
        // ordered observers; match fpga_spi publish path.
        ddrIngestPayloadVisibilityBarrier();
        timing_.used_dma = true;
        phase_ = DdrIngestPhase::PayloadVisible;
        return true;
    }

    // Returns false if doorbell would race payload (tearing class).
    bool mayRingDoorbell() const {
        if (!c_.enforce_payload_before_doorbell)
            return phase_ == DdrIngestPhase::PayloadVisible ||
                   phase_ == DdrIngestPhase::DoorbellRung;
        return phase_ == DdrIngestPhase::PayloadVisible;
    }

    bool markDoorbellRung() {
        if (!mayRingDoorbell()) {
            // Do not destroy session phase — caller may still complete payload.
            err_ = "markDoorbellRung: payload not visible";
            return false;
        }
        err_.clear();
        phase_ = DdrIngestPhase::DoorbellRung;
        return true;
    }

    // Bank virt for DecodeIntoBank producers (ffmpeg custom AVBuffer, etc.).
    uint8_t* bankWritePtr() const {
        return (phase_ == DdrIngestPhase::PayloadMoving ||
                phase_ == DdrIngestPhase::PayloadVisible)
                   ? job_.bank_virt
                   : nullptr;
    }

    size_t jobLen() const { return job_.len; }
    int jobBank() const { return job_.bank; }
    uint32_t jobBankPhys() const { return job_.bank_phys; }

private:
    void fail(const char* m) {
        err_ = m ? m : "ingest failed";
        phase_ = DdrIngestPhase::Failed;
    }

    DdrIngestContract c_{};
    DdrIngestJob job_{};
    DdrIngestPhase phase_ = DdrIngestPhase::Idle;
    DdrIngestTiming timing_{};
    std::string err_;
};

// Double-buffer plan: while DMA/copy moves bank A, decoder may fill RAM slot B.
struct DdrIngestDoubleBufferPlan {
    int decode_slot = 0; // 0/1 RAM slot being decoded into
    int move_bank = 0;   // FPGA bank receiving completed RAM slot
    bool overlap_armed = false;
};

inline DdrIngestDoubleBufferPlan ddrIngestAdvanceOverlap(DdrIngestDoubleBufferPlan p,
                                                         bool move_finished) {
    if (!move_finished)
        return p;
    p.decode_slot ^= 1;
    p.move_bank ^= 1;
    p.overlap_armed = true;
    return p;
}

// Ship vs bridge: fabric publish_bytes=0 is still the only hard ship gate (w-mem).
// Route A is a measured bridge if PL330 concurrency holds.
inline bool ddrIngestRouteAIsShipGate() { return false; }

inline bool ddrIngestRouteABridgeCandidate(double decodeMs, double copyMs = kParentBankCopy720pMs) {
    return !ddrIngestSerialDecodePlusCopyFits(decodeMs, copyMs) &&
           ddrIngestOverlapMaxFits(decodeMs, copyMs);
}

} // namespace misterplex
