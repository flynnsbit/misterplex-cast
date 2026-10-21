#pragma once

// Cyclone V HPS PL330 mem2mem helper (w-path Route A).
//
// Programs the non-secure DMAC via /dev/mem debug instruction interface.
// Default OFF — parent measures concurrency; Linux may already own channels.
//
// Program scratch + staging live in the memmap reserved window at fixed phys
// (kPl330ProgScratchPhys / kPl330StagingPhys in ddr_frame_layout.hpp) so the
// PL330 can fetch microcode by PA without CMA/hugepages/pagemap.
//
// INCIDENT (parent 2026-08-03): prior bench reported 98k fps / 136 GB/s — impossible.
// Root cause: DBGINST0 DMAGO encoding was wrong (channel in bits[15:8], opcode in
// low byte). Linux pl330.c uses (opcode<<16)|(chan<<24) as manager. Channel never
// left STOPPED; wait loop treated already-stopped as success. ok_frac only checked
// CSR, never destination bytes. All prior "DMA works" timings are INVALID.
//
// Hard rules now:
//   - DMAGO encoding matches Linux _execute_DBGINSN(..., as_manager=true)
//   - ok requires saw_executing then stopped (not stopped-from-idle)
//   - bench must content-verify destination; throughput > ceiling → hard fail
//   - completion wait is usleep-backed (no tight spin that steals the decode core)
//
// Do NOT ring the frame doorbell from this header — caller owns ordering
// (DdrIngestSession::mayRingDoorbell).

#include "libmisterplex/ddr_frame_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace misterplex {

// Cyclone V HPS non-secure PL330 (Intel CV HPS TRM).
constexpr uint32_t kPl330NsBasePhys = 0xFFE01000u;
constexpr size_t kPl330MapLen = 0x1000u;

// Register offsets (ARM DDI0424)
constexpr uint32_t kPl330_DSR = 0x000;
constexpr uint32_t kPl330_FSRD = 0x030;
constexpr uint32_t kPl330_FSRC = 0x034;
constexpr uint32_t kPl330_FTRD = 0x038; // manager fault type
constexpr uint32_t kPl330_FTR0 = 0x040; // + n*4 channel fault type
constexpr uint32_t kPl330_CSR0 = 0x100; // + n*0x08
constexpr uint32_t kPl330_CPC0 = 0x104; // + n*0x08
constexpr uint32_t kPl330_DBGSTATUS = 0xD00;
constexpr uint32_t kPl330_DBGCMD = 0xD04;
constexpr uint32_t kPl330_DBGINST0 = 0xD08;
constexpr uint32_t kPl330_DBGINST1 = 0xD0C;
constexpr uint32_t kPl330_CR0 = 0xE00;
constexpr uint32_t kPl330_CRD = 0xE14;

struct Pl330Mem2MemRequest {
    uint32_t src_phys = 0;
    uint32_t dst_phys = 0;
    size_t len = 0;
    int channel = 0; // 0..7 prefer
    // 0 = auto (largest legal). 1..16 forces CCR burst beats (diag).
    unsigned force_burst = 0;
    // 0xFF = try ns=1 then 0. Else pin single NS bit for DMAGO.
    unsigned force_ns = 0xFFu;
    // Default false: never DBGINST-KILL a channel while kernel dma-pl330 owns the
    // controller (rd-duck; parent unbind hang). Lab-only opt-in for bare-metal diag.
    bool allow_channel_kill = false;
};

struct Pl330Mem2MemResult {
    bool ok = false;
    bool hardware_present = false;
    bool channel_started = false;
    bool saw_executing = false; // must leave STOPPED before STOPPED-is-done
    int channel_used = -1;
    std::string detail;
    uint32_t dsr = 0;
    uint32_t cr0 = 0;
    uint32_t crd = 0;
    uint32_t csr_final = 0;
    uint32_t cpc_final = 0;
    uint32_t dbginst0 = 0;
    uint32_t fsrd = 0;
    uint32_t fsrc = 0;
    uint32_t ftrd = 0;
    uint32_t ftr_ch = 0;
    unsigned ns_used = 0;
    unsigned burst_used = 0;
    unsigned mfifo_words = 0;
};

// DDI0424 CRD[24:20] = data_buffer_dep; MFIFO depth words = dep+1.
inline unsigned pl330MfifoWordsFromCrd(uint32_t crd) {
    return ((crd >> 20) & 0x1Fu) + 1u;
}

// Decode channel FTR bits (DDI0424 / parent ladder). Prefer first hit for tag;
// always print raw ftr_ch hex — ftr_other previously hid the real bit.
inline const char* pl330FtrChannelTag(uint32_t ftr) {
    if (ftr == 0)
        return "ftr_zero";
    if (ftr & (1u << 0))
        return "undef_instr";
    if (ftr & (1u << 1))
        return "operand_invalid";
    if (ftr & (1u << 5))
        return "ch_rdwr_err";
    if (ftr & (1u << 12))
        return "mfifo_err";
    if (ftr & (1u << 13))
        return "st_data_unavailable";
    if (ftr & (1u << 16))
        return "instr_fetch_err";
    if (ftr & (1u << 17))
        return "data_write_err";
    if (ftr & (1u << 18))
        return "data_read_err";
    if (ftr & (1u << 30))
        return "dbg_instr";
    if (ftr & (1u << 31))
        return "lockup_err";
    return "ftr_other";
}

// Append all set FTR bit names into buf (comma-separated). Returns buf.
inline char* pl330FtrChannelBitsStr(uint32_t ftr, char* buf, size_t cap) {
    if (!buf || cap < 4) {
        if (buf && cap)
            buf[0] = 0;
        return buf;
    }
    buf[0] = 0;
    if (ftr == 0) {
        std::snprintf(buf, cap, "none");
        return buf;
    }
    struct {
        uint32_t bit;
        const char* name;
    } map[] = {{0, "UNDEF_INSTR"},        {1, "OPERAND_INVALID"}, {5, "CH_RDWR_ERR"},
               {12, "MFIFO_ERR"},         {13, "ST_DATA_UNAVAIL"}, {16, "INSTR_FETCH_ERR"},
               {17, "DATA_WRITE_ERR"},    {18, "DATA_READ_ERR"},   {30, "DBG_INSTR"},
               {31, "LOCKUP_ERR"}};
    size_t used = 0;
    for (const auto& m : map) {
        if (!(ftr & (1u << m.bit)))
            continue;
        const int n = std::snprintf(buf + used, cap - used, "%s%s", used ? "," : "", m.name);
        if (n < 0 || static_cast<size_t>(n) >= cap - used)
            break;
        used += static_cast<size_t>(n);
    }
    if (used == 0)
        std::snprintf(buf, cap, "UNKNOWN_0x%x", ftr);
    return buf;
}

inline const char* pl330CsrStateTag(uint32_t csr) {
    switch (csr & 0xFu) {
    case 0x0:
        return "stopped";
    case 0x1:
        return "executing";
    case 0x2:
        return "cache_miss";
    case 0x3:
        return "updating_pc";
    case 0x4:
        return "wait_event";
    case 0x5:
        return "at_barrier";
    case 0x6:
        return "queue_busy";
    case 0x7:
        return "wait_periph";
    case 0x8:
        return "killing";
    case 0x9:
        return "completing";
    case 0xe:
        return "fault_completing";
    case 0xf:
        return "faulting";
    default:
        return "state_other";
    }
}

// Channel status low nibble (DS/CS) — DDI0424 / linux pl330.c
constexpr uint32_t kPl330StStopped = 0x0;
constexpr uint32_t kPl330StExecuting = 0x1;
constexpr uint32_t kPl330StFaulting = 0xf;

// DE10-Nano DDR3 controller theoretical peak ~3.2 GB/s (parent). Any single
// userspace mem2mem claim above this is instrument failure, not a result.
constexpr double kPl330MaxPlausibleThroughputGBs = 3.2;
// Floor: even an ideal controller needs >0.4 ms for one 720p I420 frame.
inline double pl330MinPlausibleMsForBytes(size_t nbytes) {
    if (nbytes == 0)
        return 0.0;
    return (static_cast<double>(nbytes) / (kPl330MaxPlausibleThroughputGBs * 1e9)) * 1000.0;
}
inline double pl330ThroughputGBs(size_t nbytes, double ms_per_frame) {
    if (ms_per_frame <= 0.0)
        return 1e99;
    return (static_cast<double>(nbytes) / (ms_per_frame / 1000.0)) / 1e9;
}
// true if measured rate is physically impossible on this SoC.
inline bool pl330ThroughputImpossible(size_t nbytes, double ms_per_frame) {
    if (ms_per_frame <= 0.0)
        return true;
    // 5% slack only for timer jitter; never allow 2x over peak.
    const double min_ms = pl330MinPlausibleMsForBytes(nbytes) * 0.95;
    return ms_per_frame < min_ms;
}

// Linux pl330.c _execute_DBGINSN(as_manager=true) for DMAGO:
//   insn[0]=CMD_DMAGO|(ns<<1), insn[1]=chan, insn[2..5]=pc LE
//   DBGINST0 = (insn[0]<<16) | (insn[1]<<24)
//   DBGINST1 = pc
// Prior bug put opcode in bits[7:0] and chan in [15:8] — never started channel.
inline uint32_t pl330DbgInst0GoManager(unsigned chan, unsigned ns) {
    const uint8_t op = static_cast<uint8_t>(0xA0u | ((ns & 1u) << 1));
    const uint8_t ch = static_cast<uint8_t>(chan & 7u);
    return (static_cast<uint32_t>(op) << 16) | (static_cast<uint32_t>(ch) << 24);
}

// Content check helpers (bench + unit). Sample ends + middle; full optional.
inline bool pl330BufferSamplesMatch(const uint8_t* dst, const uint8_t* src, size_t len) {
    if (!dst || !src || len == 0)
        return false;
    const size_t n = len < 64 ? len : 64;
    if (std::memcmp(dst, src, n) != 0)
        return false;
    if (std::memcmp(dst + (len - n), src + (len - n), n) != 0)
        return false;
    if (len > 128) {
        const size_t mid = len / 2;
        if (std::memcmp(dst + mid, src + mid, n) != 0)
            return false;
    }
    return true;
}
inline bool pl330BufferFullMatch(const uint8_t* dst, const uint8_t* src, size_t len) {
    if (!dst || !src || len == 0)
        return false;
    return std::memcmp(dst, src, len) == 0;
}

// CCR: SA/DA inc, SS32/DS32, symmetric burst_len in 1..16 (encoded as len-1).
// nonsecure=true sets src/dst AxPROT NonSecure (value 2 in prot fields) — required
// when channel CNS=1 (parent: csr bit21). Secure prot on NS channel → operand/rdwr fault
// on first DMAMOV CCR / load (CPC often stops at prog+6 = second DMAMOV).
// Returns 0 on bad args. See ARM DDI0424 CCR; Linux pl330 sets NS the same way.
inline uint32_t pl330BuildCcrBurst(unsigned burst_len, bool nonsecure = true) {
    if (burst_len < 1 || burst_len > 16)
        return 0;
    const uint32_t SRC_INC = 1u << 0;
    const uint32_t DST_INC = 1u << 14;
    const uint32_t SRC_BURST_SIZE_32 = 2u << 1; // 2^2 = 4 bytes
    const uint32_t DST_BURST_SIZE_32 = 2u << 15;
    const uint32_t bl = burst_len - 1u;
    const uint32_t SRC_BURST_LEN = bl << 4;
    const uint32_t DST_BURST_LEN = bl << 18;
    // AxPROT: [1]=NonSecure → field value 2 at src_prot[10:8] and dst_prot[24:22]
    const uint32_t SRC_PROT_NS = 2u << 8;
    const uint32_t DST_PROT_NS = 2u << 22;
    uint32_t ccr = SRC_INC | DST_INC | SRC_BURST_SIZE_32 | DST_BURST_SIZE_32 | SRC_BURST_LEN |
                   DST_BURST_LEN;
    if (nonsecure)
        ccr |= SRC_PROT_NS | DST_PROT_NS;
    return ccr;
}

// Back-compat: burst-1 CCR when len is 4-byte aligned (len only validates).
inline uint32_t pl330BuildCcr(size_t len, bool nonsecure = true) {
    if (len == 0 || (len % 4) != 0)
        return 0;
    return pl330BuildCcrBurst(1, nonsecure);
}

// Human-readable program listing (offsets + bytes + opcode guess). out lines via callback-ish
// snprintf into caller buffer is awkward; bench prints via this helper into stdout-style string.
inline size_t pl330DisasmProgramLine(const uint8_t* prog, size_t plen, size_t off, char* line,
                                     size_t line_cap) {
    if (!prog || !line || line_cap < 8 || off >= plen) {
        if (line && line_cap)
            line[0] = 0;
        return 0;
    }
    const uint8_t op = prog[off];
    const char* name = "data";
    size_t nbytes = 1;
    if (op == 0xBC && off + 5 < plen) {
        name = (prog[off + 1] == 0) ? "DMAMOV_SAR" : (prog[off + 1] == 1) ? "DMAMOV_CCR"
                                                    : (prog[off + 1] == 2)   ? "DMAMOV_DAR"
                                                                            : "DMAMOV";
        nbytes = 6;
    } else if (op == 0x20 || op == 0x22) {
        name = (op == 0x20) ? "DMALP_LC0" : "DMALP_LC1";
        nbytes = 2;
    } else if (op == 0x04) {
        name = "DMALD";
        nbytes = 1;
    } else if (op == 0x08) {
        name = "DMAST";
        nbytes = 1;
    } else if (op == 0x38 || op == 0x3C) {
        name = (op == 0x38) ? "DMALPEND_LC0" : "DMALPEND_LC1";
        nbytes = 2;
    } else if (op == 0x34) {
        name = "DMASEV";
        nbytes = 2;
    } else if (op == 0x00) {
        name = "DMAEND";
        nbytes = 1;
    }
    if (off + nbytes > plen)
        nbytes = plen - off;
    size_t used = 0;
    int n = std::snprintf(line + used, line_cap - used, "+%02zu ", off);
    if (n > 0)
        used += static_cast<size_t>(n);
    for (size_t i = 0; i < nbytes && used + 3 < line_cap; ++i) {
        n = std::snprintf(line + used, line_cap - used, "%02x ", prog[off + i]);
        if (n > 0)
            used += static_cast<size_t>(n);
    }
    std::snprintf(line + used, line_cap - used, "; %s", name);
    return nbytes;
}

// Factor n_iters into inner*outer with both in 1..256. Prefer larger outer.
inline bool pl330FactorLoops(uint32_t n_iters, uint32_t* inner, uint32_t* outer) {
    if (!inner || !outer || n_iters == 0)
        return false;
    if (n_iters <= 256u) {
        *inner = n_iters;
        *outer = 1;
        return true;
    }
    if (n_iters > 256u * 256u)
        return false;
    for (uint32_t o = 256u; o >= 1u; --o) {
        if (n_iters % o)
            continue;
        const uint32_t i = n_iters / o;
        if (i >= 1u && i <= 256u) {
            *inner = i;
            *outer = o;
            return true;
        }
    }
    return false;
}

// Pick largest burst in {16,8,4,2,1} that divides len, factors loop count, and
// does not exceed MFIFO depth (one DMALD pushes `burst` 32-bit words).
// max_burst_cap 0 → treat as 16. force_burst 1..16 pins exactly that size.
inline bool pl330PickBurstAndLoops(size_t len, unsigned* burst_len, uint32_t* inner,
                                   uint32_t* outer, unsigned max_burst_cap = 16,
                                   unsigned force_burst = 0) {
    if (!burst_len || !inner || !outer || len == 0 || (len % 4) != 0)
        return false;
    unsigned cap = max_burst_cap == 0 ? 16u : max_burst_cap;
    if (cap > 16)
        cap = 16;
    if (force_burst != 0) {
        if (force_burst < 1 || force_burst > 16 || force_burst > cap)
            return false;
        const uint32_t bpb = 4u * force_burst;
        if ((len % bpb) != 0)
            return false;
        const uint32_t n_iters = static_cast<uint32_t>(len / bpb);
        if (!pl330FactorLoops(n_iters, inner, outer))
            return false;
        *burst_len = force_burst;
        return true;
    }
    const unsigned cands[] = {16, 8, 4, 2, 1};
    for (unsigned b : cands) {
        if (b > cap)
            continue;
        const uint32_t bpb = 4u * b;
        if ((len % bpb) != 0)
            continue;
        const uint32_t n_iters = static_cast<uint32_t>(len / bpb);
        if (pl330FactorLoops(n_iters, inner, outer)) {
            *burst_len = b;
            return true;
        }
    }
    return false;
}

// Encode channel program. Loop body is one DMALD+DMAST per burst beat set.
// Nested LC1 (outer) then LC0 (inner). Returns byte length or 0.
inline size_t pl330BuildMem2MemProgramLoops(uint8_t* out, size_t out_cap, uint32_t src,
                                            uint32_t dst, uint32_t ccr, uint32_t inner,
                                            uint32_t outer) {
    if (!out || out_cap < 40 || inner == 0 || inner > 256 || outer == 0 || outer > 256)
        return 0;
    size_t o = 0;
    auto emit = [&](uint8_t b) {
        if (o < out_cap)
            out[o++] = b;
    };
    auto emit32 = [&](uint32_t v) {
        emit(static_cast<uint8_t>(v));
        emit(static_cast<uint8_t>(v >> 8));
        emit(static_cast<uint8_t>(v >> 16));
        emit(static_cast<uint8_t>(v >> 24));
    };
    // DMAMOV SAR / CCR / DAR
    emit(0xBC);
    emit(0x00);
    emit32(src);
    emit(0xBC);
    emit(0x01);
    emit32(ccr);
    emit(0xBC);
    emit(0x02);
    emit32(dst);

    // Outer first when nested (LC1), then LC0.
    if (outer > 1) {
        emit(0x22); // DMALP LC1
        emit(static_cast<uint8_t>(outer - 1));
    }
    emit(0x20); // DMALP LC0
    emit(static_cast<uint8_t>(inner - 1));
    emit(0x04); // DMALD
    emit(0x08); // DMAST
    // LPEND LC0: bjump over DMALD+DMAST = 2
    emit(0x38);
    emit(0x02);
    if (outer > 1) {
        // LPEND LC1: body = DMALP_LC0(2)+DMALD(1)+DMAST(1)+LPEND_LC0(2) = 6
        emit(0x3C);
        emit(0x06);
    }
    emit(0x34); // DMASEV 0
    emit(0x00);
    emit(0x00); // DMAEND
    return o;
}

// Back-compat: n_words of burst-1 (4 B) transfers.
inline size_t pl330BuildMem2MemProgram(uint8_t* out, size_t out_cap, uint32_t src, uint32_t dst,
                                       uint32_t ccr, uint32_t n_words) {
    uint32_t inner = 0, outer = 0;
    if (!pl330FactorLoops(n_words, &inner, &outer))
        return 0;
    return pl330BuildMem2MemProgramLoops(out, out_cap, src, dst, ccr, inner, outer);
}

// Full-frame encode: auto burst + nested loops. out_burst receives chosen burst.
// max_burst_cap from CRD MFIFO; force_burst for surgical diag (e.g. 4 KiB / burst1).
// nonsecure CCR prot must match channel CNS (default true for HPS NS DMAC).
inline size_t pl330BuildMem2MemProgramAuto(uint8_t* out, size_t out_cap, uint32_t src, uint32_t dst,
                                           size_t len, unsigned* out_burst = nullptr,
                                           unsigned max_burst_cap = 16, unsigned force_burst = 0,
                                           bool nonsecure_ccr = true, uint32_t* out_ccr = nullptr) {
    unsigned burst = 1;
    uint32_t inner = 0, outer = 0;
    if (!pl330PickBurstAndLoops(len, &burst, &inner, &outer, max_burst_cap, force_burst))
        return 0;
    const uint32_t ccr = pl330BuildCcrBurst(burst, nonsecure_ccr);
    if (!ccr)
        return 0;
    if (out_burst)
        *out_burst = burst;
    if (out_ccr)
        *out_ccr = ccr;
    return pl330BuildMem2MemProgramLoops(out, out_cap, src, dst, ccr, inner, outer);
}

class Pl330Mem2Mem {
public:
    ~Pl330Mem2Mem() { close(); }

    bool open(std::string* err = nullptr) {
        close();
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (fd_ < 0) {
            if (err)
                *err = "pl330: open /dev/mem failed";
            return false;
        }
        map_ = ::mmap(nullptr, kPl330MapLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                      static_cast<off_t>(kPl330NsBasePhys));
        if (map_ == MAP_FAILED) {
            map_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            if (err)
                *err = "pl330: mmap DMAC failed";
            return false;
        }
        // Fixed ABI scratch in reserved window (PA known by construction).
        scratch_map_ = ::mmap(nullptr, kPl330AbiRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                              fd_, static_cast<off_t>(kPl330AbiRegionPhys));
        if (scratch_map_ == MAP_FAILED) {
            scratch_map_ = nullptr;
            ::munmap(map_, kPl330MapLen);
            map_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            if (err)
                *err = "pl330: mmap ABI scratch/staging failed";
            return false;
        }
        prog_scratch_phys_ = kPl330ProgScratchPhys;
        prog_scratch_virt_ = scratch_map_;
        staging_phys_ = kPl330StagingPhys;
        staging_virt_ = static_cast<uint8_t*>(scratch_map_) + kPl330ProgScratchBytes;
        return true;
    }

    void close() {
        if (scratch_map_ && scratch_map_ != MAP_FAILED) {
            ::munmap(scratch_map_, kPl330AbiRegionBytes);
            scratch_map_ = nullptr;
        }
        prog_scratch_virt_ = nullptr;
        staging_virt_ = nullptr;
        prog_scratch_phys_ = 0;
        staging_phys_ = 0;
        if (map_ && map_ != MAP_FAILED) {
            ::munmap(map_, kPl330MapLen);
            map_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool isOpen() const { return map_ != nullptr && scratch_map_ != nullptr; }

    uint32_t progScratchPhys() const { return prog_scratch_phys_; }
    void* progScratchVirt() const { return prog_scratch_virt_; }
    uint32_t stagingPhys() const { return staging_phys_; }
    void* stagingVirt() const { return staging_virt_; }

    // Copy host bytes into contiguous staging (PA = kPl330StagingPhys).
    bool loadStaging(const void* src, size_t len, std::string* err = nullptr) {
        if (!staging_virt_ || !src || len == 0 || len > kPl330StagingBytes) {
            if (err)
                *err = "pl330: staging load rejected";
            return false;
        }
        std::memcpy(staging_virt_, src, len);
        __sync_synchronize();
        return true;
    }

    uint32_t read32(uint32_t off) const {
        if (!map_)
            return 0;
        return *reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(map_) + off);
    }

    void write32(uint32_t off, uint32_t v) {
        if (!map_)
            return;
        *reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(map_) + off) = v;
    }

    // Probe only — does not start a channel.
    Pl330Mem2MemResult probe() {
        Pl330Mem2MemResult r;
        if (!map_) {
            r.detail = "not open";
            return r;
        }
        r.dsr = read32(kPl330_DSR);
        r.cr0 = read32(kPl330_CR0);
        r.crd = read32(kPl330_CRD);
        r.mfifo_words = pl330MfifoWordsFromCrd(r.crd);
        r.fsrd = read32(kPl330_FSRD);
        r.fsrc = read32(kPl330_FSRC);
        r.ftrd = read32(kPl330_FTRD);
        // CR0 should report channel count; zero often means wrong map / powered down.
        r.hardware_present = (r.cr0 != 0 && r.cr0 != 0xFFFFFFFFu);
        r.detail = r.hardware_present ? "pl330 cr0 ok" : "pl330 cr0 empty";
        r.ok = r.hardware_present;
        return r;
    }

    // Snapshot fault/status regs for channel ch into result (parent surgical diag).
    void captureFaultRegs(Pl330Mem2MemResult& r, int ch) {
        r.fsrd = read32(kPl330_FSRD);
        r.fsrc = read32(kPl330_FSRC);
        r.ftrd = read32(kPl330_FTRD);
        const unsigned c = static_cast<unsigned>(ch & 7);
        r.ftr_ch = read32(kPl330_FTR0 + c * 4u);
        r.csr_final = read32(kPl330_CSR0 + c * 8u);
        r.cpc_final = read32(kPl330_CPC0 + c * 8u);
        r.crd = read32(kPl330_CRD);
        r.mfifo_words = pl330MfifoWordsFromCrd(r.crd);
    }

    // Execute blocking mem2mem. len must be multiple of 4.
    // Prefers one microcode program (burst 8/16 + nested LC1/LC0). Falls back to
    // burst-1 1 KiB chunks only if auto encode fails.
    // force_burst/force_ns on req skip silent chunked fallback (diag must stay pure).
    Pl330Mem2MemResult transferBlocking(const Pl330Mem2MemRequest& req, int timeout_ms = 50) {
        Pl330Mem2MemResult r = probe();
        if (!r.hardware_present)
            return r;
        if (req.src_phys == 0 || req.dst_phys == 0 || req.len == 0 || (req.len % 4) != 0) {
            r.ok = false;
            r.detail = "bad src/dst/len";
            return r;
        }
        // Full-frame / large: single program when burst+loops factor.
        {
            auto one = transferOneBlockAuto(req, timeout_ms);
            if (one.ok || one.channel_started) {
                if (one.ok)
                    one.detail = std::string("auto ") + one.detail;
                return one;
            }
            // Fall through to chunked if encode/start failed before DMA.
            if (req.force_burst != 0 || req.force_ns != 0xFFu) {
                // Surgical diag: never silently change the program shape.
                return one;
            }
            if (one.detail.find("program") == std::string::npos &&
                one.detail != "needs_prog_scratch_phys") {
                // Fault/timeout on auto — do not silently chunk (could double-write).
                return one;
            }
            if (one.detail == "needs_prog_scratch_phys")
                return one;
        }
        // Chunked burst-1 fallback (256 words = 1024 B).
        size_t done = 0;
        int ch = req.channel;
        while (done < req.len) {
            size_t chunk = req.len - done;
            if (chunk > 256u * 4u)
                chunk = 256u * 4u;
            Pl330Mem2MemRequest sub = req;
            sub.src_phys = req.src_phys + static_cast<uint32_t>(done);
            sub.dst_phys = req.dst_phys + static_cast<uint32_t>(done);
            sub.len = chunk;
            sub.channel = ch;
            auto one = transferOneBlock(sub, timeout_ms);
            if (!one.ok) {
                r = one;
                return r;
            }
            done += chunk;
            r = one;
        }
        r.ok = true;
        r.detail = "chunked ok";
        return r;
    }

private:
    // Debug-instruction go: DMAGO for channel with PC in DBGINST1.
    // We place program in a small bounce buffer in *destination bank prefix* —
    // NOT used. Instead: use DMAMOV via debug for SAR/DAR/CCR then loop DMALD/DMAST
    // via repeated DBGINST — too slow.
    //
    // Practical v1: use first 64 bytes of a dedicated program buffer at
    // dst_phys is frame data — cannot overwrite. Require caller to supply
    // program buffer via transferOneBlock embedding in DMAC insn FIFO using
    // DMAGO with instructions fed through DBGINST0 as non-secure manager.
    //
    // Simpler reliable path used here: **CPU does not claim full microcode load**.
    // We expose descriptor build + probe; transferOneBlock attempts DMAGO with
    // inline DMAMOV sequence via debug for very small transfers only, else
    // returns not-implemented for full frame so bench reports honestly.
    void dbgWaitIdle(int spins = 10000) {
        for (int i = 0; i < spins; ++i) {
            if ((read32(kPl330_DBGSTATUS) & 1u) == 0)
                return;
            if (i > 100)
                usleep(10);
        }
    }

    // Debug-instruction inject (manager thread). Matches linux _execute_DBGINSN.
    void dbgExecManager(uint8_t insn0, uint8_t insn1, uint32_t inst1_imm) {
        dbgWaitIdle();
        const uint32_t inst0 =
            (static_cast<uint32_t>(insn0) << 16) | (static_cast<uint32_t>(insn1) << 24);
        write32(kPl330_DBGINST0, inst0);
        write32(kPl330_DBGINST1, inst1_imm);
        write32(kPl330_DBGCMD, 0);
    }

    // LAB ONLY. DBGINST KILL is unsafe while kernel dma-pl330 owns the controller.
    // goProgram calls this only when allow_channel_kill=true (default false).
    void killChannel(int ch) {
        // DMAKILL via debug as channel thread (not manager): linux uses as_manager=false
        // for KILL on channel. DBGINST0 low bits mark channel target.
        dbgWaitIdle();
        const uint32_t val = (0x01u << 16) | (1u << 0) | (static_cast<uint32_t>(ch & 7) << 8);
        write32(kPl330_DBGINST0, val);
        write32(kPl330_DBGINST1, 0);
        write32(kPl330_DBGCMD, 0);
        const uint32_t csr_off = kPl330_CSR0 + static_cast<uint32_t>(ch) * 8u;
        for (int i = 0; i < 1000; ++i) {
            if ((read32(csr_off) & 0xFu) == kPl330StStopped)
                return;
            usleep(10);
        }
    }

    void maybeKillChannel(int ch, bool allow) {
        if (allow)
            killChannel(ch);
    }

    // Fire DMAGO as manager with Linux encoding. Returns DBGINST0 written.
    uint32_t fireGo(int ch, unsigned ns) {
        // insn: [0]=DMAGO|ns<<1, [1]=chan, [2..5]=pc — DBGINST1 holds pc only
        const uint8_t op = static_cast<uint8_t>(0xA0u | ((ns & 1u) << 1));
        const uint8_t c = static_cast<uint8_t>(ch & 7);
        const uint32_t inst0 = pl330DbgInst0GoManager(static_cast<unsigned>(ch), ns);
        dbgWaitIdle();
        write32(kPl330_DBGINST0, inst0);
        write32(kPl330_DBGINST1, prog_scratch_phys_);
        write32(kPl330_DBGCMD, 0);
        (void)op;
        (void)c;
        return inst0;
    }

    Pl330Mem2MemResult goProgram(const uint8_t* prog, size_t plen, int channel, int timeout_ms,
                                 const char* tag, unsigned force_ns = 0xFFu,
                                 unsigned burst_used = 0, bool allow_channel_kill = false) {
        Pl330Mem2MemResult r;
        r.hardware_present = true;
        r.cr0 = read32(kPl330_CR0);
        r.crd = read32(kPl330_CRD);
        r.mfifo_words = pl330MfifoWordsFromCrd(r.crd);
        r.dsr = read32(kPl330_DSR);
        r.burst_used = burst_used;
        if (!prog || plen == 0) {
            r.detail = "program encode failed";
            return r;
        }
        if (prog_scratch_phys_ == 0 || !prog_scratch_virt_) {
            r.ok = false;
            r.detail = "needs_prog_scratch_phys";
            r.channel_started = false;
            return r;
        }
        if (plen > kPl330ProgScratchBytes) {
            r.detail = "program exceeds scratch";
            return r;
        }
        std::memcpy(prog_scratch_virt_, prog, plen);
        if (plen < 64)
            std::memset(static_cast<uint8_t*>(prog_scratch_virt_) + plen, 0, 64 - plen);
        __sync_synchronize();

        const int ch = (channel >= 0 && channel < 8) ? channel : 0;
        r.channel_used = ch;
        // Default: do NOT kill — kernel owns dma-pl330 (rd-duck / parent incident).
        maybeKillChannel(ch, allow_channel_kill);
        captureFaultRegs(r, ch);

        // NS DMAC: default try ns=1 then 0. force_ns pins one value (diag).
        unsigned ns_try[2] = {1u, 0u};
        unsigned n_ns = 2;
        if (force_ns == 0u || force_ns == 1u) {
            ns_try[0] = force_ns;
            n_ns = 1;
        }
        const uint32_t csr_off = kPl330_CSR0 + static_cast<uint32_t>(ch) * 8u;
        // Completion wait: usleep-backed only. Tight spin steals the decode core and
        // makes Route A look like a CPU burner (parent incident: 98k fake fps).
        const int poll_us = 100;
        const int max_polls = (timeout_ms > 0 ? timeout_ms : 50) * 1000 / poll_us;
        const char* base_tag = tag ? tag : "dma";

        for (unsigned nsi = 0; nsi < n_ns; ++nsi) {
            const unsigned ns = ns_try[nsi];
            r.ns_used = ns;
            r.dbginst0 = fireGo(ch, ns);
            r.channel_started = true;
            r.saw_executing = false;

            bool fault = false;
            for (int i = 0; i < max_polls; ++i) {
                const uint32_t csr = read32(csr_off);
                const uint32_t st = csr & 0xFu;
                r.csr_final = csr;
                r.cpc_final = read32(kPl330_CPC0 + static_cast<uint32_t>(ch) * 8u);
                if (read32(kPl330_FSRC) & (1u << ch)) {
                    fault = true;
                    captureFaultRegs(r, ch);
                    r.detail = std::string(base_tag) + " channel_fault ftr=" +
                               pl330FtrChannelTag(r.ftr_ch) + " csr=" +
                               pl330CsrStateTag(r.csr_final);
                    r.ok = false;
                    break;
                }
                // Executing / updating_pc / completing only. Cache-miss alone is NOT
                // progress — stuck i-cache fill (e.g. unreadable prog PA) must not
                // count as saw_executing (parent: csr=0x2 + fault).
                if (st == kPl330StExecuting || st == 0x3 || st == 0x9) {
                    r.saw_executing = true;
                }
                // Done only if we observed work, then STOPPED.
                if (r.saw_executing && st == kPl330StStopped) {
                    captureFaultRegs(r, ch);
                    r.ok = true;
                    r.detail = std::string(base_tag) + " saw_exec+stopped ok";
                    return r;
                }
                // Still stopped with no execute after a few polls → GO ignored; try other ns.
                if (!r.saw_executing && st == kPl330StStopped && i >= 5) {
                    r.detail = std::string(base_tag) + " go_ignored_still_stopped";
                    break;
                }
                // Stuck in cache_miss with no FSRC yet — keep polling; may promote to fault.
                usleep(static_cast<useconds_t>(poll_us));
            }
            if (fault) {
                maybeKillChannel(ch, allow_channel_kill);
                captureFaultRegs(r, ch);
                return r;
            }
            if (r.saw_executing) {
                captureFaultRegs(r, ch);
                r.ok = false;
                r.detail = std::string(base_tag) + " timeout after exec csr=" +
                           pl330CsrStateTag(r.csr_final);
                maybeKillChannel(ch, allow_channel_kill);
                return r;
            }
            maybeKillChannel(ch, allow_channel_kill);
            // retry other ns
        }
        captureFaultRegs(r, ch);
        r.ok = false;
        if (r.detail.empty())
            r.detail = std::string(base_tag) + " never_executed (false-complete guard)";
        return r;
    }

    Pl330Mem2MemResult transferOneBlock(const Pl330Mem2MemRequest& req, int timeout_ms) {
        uint8_t prog[64];
        // force_ns==0 → secure CCR; else NS (matches HPS NS channel CNS=1).
        const bool ns_ccr = (req.force_ns != 0u);
        const uint32_t ccr = pl330BuildCcr(req.len, ns_ccr);
        const uint32_t n_words = static_cast<uint32_t>(req.len / 4);
        const size_t plen =
            pl330BuildMem2MemProgram(prog, sizeof(prog), req.src_phys, req.dst_phys, ccr, n_words);
        return goProgram(prog, plen, req.channel, timeout_ms, "burst1 stopped ok", req.force_ns, 1,
                         req.allow_channel_kill);
    }

    Pl330Mem2MemResult transferOneBlockAuto(const Pl330Mem2MemRequest& req, int timeout_ms) {
        uint8_t prog[64];
        unsigned burst = 0;
        uint32_t ccr_used = 0;
        const uint32_t crd = read32(kPl330_CRD);
        const unsigned mfifo = pl330MfifoWordsFromCrd(crd);
        // One DMALD of burst B @ 32-bit needs B MFIFO words free.
        const unsigned cap = mfifo == 0 ? 1u : (mfifo < 16u ? mfifo : 16u);
        const bool ns_ccr = (req.force_ns != 0u);
        const size_t plen = pl330BuildMem2MemProgramAuto(
            prog, sizeof(prog), req.src_phys, req.dst_phys, req.len, &burst, cap, req.force_burst,
            ns_ccr, &ccr_used);
        if (plen == 0) {
            Pl330Mem2MemResult r;
            r.hardware_present = true;
            r.crd = crd;
            r.mfifo_words = mfifo;
            r.detail = "program encode failed";
            return r;
        }
        char tag[80];
        std::snprintf(tag, sizeof(tag), "burst%u mfifo%u ccr=0x%08x ns_ccr=%u kill=%u", burst, mfifo,
                      ccr_used, ns_ccr ? 1u : 0u, req.allow_channel_kill ? 1u : 0u);
        return goProgram(prog, plen, req.channel, timeout_ms, tag, req.force_ns, burst,
                         req.allow_channel_kill);
    }

public:
    void setProgramScratch(uint32_t phys, void* virt) {
        prog_scratch_phys_ = phys;
        prog_scratch_virt_ = virt;
    }

    // Descriptor-only path for unit tests (no /dev/mem).
    static bool encodeProgramForTest(uint8_t* out, size_t cap, size_t* out_len, uint32_t src,
                                     uint32_t dst, size_t len) {
        unsigned burst = 0;
        const size_t n = pl330BuildMem2MemProgramAuto(out, cap, src, dst, len, &burst);
        if (!n) {
            // Tiny lens may only fit burst-1 single LC0.
            const uint32_t ccr = pl330BuildCcr(len);
            if (!ccr)
                return false;
            const size_t n2 = pl330BuildMem2MemProgram(out, cap, src, dst, ccr,
                                                       static_cast<uint32_t>(len / 4));
            if (!n2)
                return false;
            if (out_len)
                *out_len = n2;
            return true;
        }
        if (out_len)
            *out_len = n;
        (void)burst;
        return true;
    }

    // ABI constants visible to tests without /dev/mem.
    static uint32_t abiProgScratchPhys() { return kPl330ProgScratchPhys; }
    static uint32_t abiStagingPhys() { return kPl330StagingPhys; }

private:
    int fd_ = -1;
    void* map_ = nullptr;
    void* scratch_map_ = nullptr;
    uint32_t prog_scratch_phys_ = 0;
    void* prog_scratch_virt_ = nullptr;
    uint32_t staging_phys_ = 0;
    void* staging_virt_ = nullptr;
};

// Resolve first page phys for a user pointer via /proc/self/pagemap.
// Returns 0 on failure. Requires CAP_SYS_ADMIN on many kernels.
inline uint32_t pl330ResolveUserPhys(const void* virt) {
    if (!virt)
        return 0;
    const uintptr_t v = reinterpret_cast<uintptr_t>(virt);
    const uintptr_t page = v & ~static_cast<uintptr_t>(4095);
    const uintptr_t off = v - page;
    const long idx = static_cast<long>(page / 4096u);
    const int fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    uint64_t entry = 0;
    if (::lseek(fd, idx * static_cast<off_t>(sizeof(entry)), SEEK_SET) < 0) {
        ::close(fd);
        return 0;
    }
    if (::read(fd, &entry, sizeof(entry)) != static_cast<ssize_t>(sizeof(entry))) {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    if ((entry & (1ull << 63)) == 0)
        return 0; // not present
    const uint64_t pfn = entry & ((1ull << 55) - 1);
    if (pfn == 0)
        return 0;
    return static_cast<uint32_t>((pfn << 12) | off);
}

// True if len spans a single page-aligned physically contiguous region
// (simple check: all pages present and sequential PFNs) — v1: single page only.
inline bool pl330UserRangeSinglePage(const void* virt, size_t len) {
    if (!virt || len == 0)
        return false;
    const uintptr_t v = reinterpret_cast<uintptr_t>(virt);
    return ((v & 4095u) + len) <= 4096u;
}

} // namespace misterplex
