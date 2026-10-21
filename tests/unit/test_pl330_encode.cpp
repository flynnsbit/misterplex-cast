// Host-side PL330 contract gates (no /dev/mem). Pins Linux DMAGO encode,
// content-verify requirement, impossible-throughput reject, FTR tags, ABI fence.
//
// Negative cases a naive "DMA works if CSR stopped" implementation would fail.

#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/ddr_zero_copy_ingest.hpp"
#include "libmisterplex/pl330_mem2mem.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                     \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

int main() {
    using namespace misterplex;
    int fails = 0;

    // --- ABI fence (w-mem Option-C + PL330 scratch) ---
    CHECK(kPl330ProgScratchPhys == 0x30600000u, "scratch phys");
    CHECK(kPl330StagingPhys == 0x30601000u, "staging phys");
    CHECK(kPl330StagingBytes == kPlex720pYuv420pBankStride, "staging = one 720p bank");
    CHECK(Pl330Mem2Mem::abiProgScratchPhys() == kPl330ProgScratchPhys, "class ABI scratch");
    CHECK(!pl330AbiOverlapsOptionCBanks(kPl330AbiRegionPhys, kPl330AbiRegionBytes),
          "PL330 must not overlap Option-C banks");
    CHECK(pl330PhysInProgScratch(kPl330ProgScratchPhys, 32), "prog in scratch");
    CHECK(pl330PhysInStaging(kPl330StagingPhys, 1382400u), "frame fits staging");
    CHECK(!pl330PhysInStaging(kPl330StagingPhys, kPl330StagingBytes + 1u),
          "oversize staging reject");

    // --- Linux manager DMAGO encode (the 98k-fps incident root cause) ---
    // insn[0]=0xA0|ns<<1, insn[1]=chan → DBGINST0 = (op<<16)|(chan<<24)
    const uint32_t go_ch0_ns1 = pl330DbgInst0GoManager(0, 1);
    CHECK(go_ch0_ns1 == 0x00A20000u, "DMAGO ch0 ns1 linux manager");
    const uint32_t go_ch3_ns0 = pl330DbgInst0GoManager(3, 0);
    CHECK(go_ch3_ns0 == 0x03A00000u, "DMAGO ch3 ns0 linux manager");

    // NEGATIVE: the broken pre-fix packing put opcode in low byte / chan mid.
    // That encoding must NEVER equal the Linux manager form for any ns/chan pair.
    auto broken_pack = [](unsigned chan, unsigned ns) -> uint32_t {
        const uint8_t op = static_cast<uint8_t>(0xA0u | ((ns & 1u) << 1));
        return static_cast<uint32_t>(op) | (static_cast<uint32_t>(chan & 7u) << 8);
    };
    CHECK(broken_pack(0, 1) != go_ch0_ns1, "broken pack != linux pack (ch0)");
    CHECK(broken_pack(3, 0) != go_ch3_ns0, "broken pack != linux pack (ch3)");
    // Explicit known-bad constant from the incident write-up.
    CHECK(broken_pack(0, 1) == 0x000000A2u, "document broken low-byte form");

    // --- CCR NS AxPROT required (parent: secure CCR on NS channel → prog+6 fault) ---
    const uint32_t ccr_ns = pl330BuildCcrBurst(4, true);
    const uint32_t ccr_s = pl330BuildCcrBurst(4, false);
    CHECK(ccr_ns != 0 && ccr_s != 0, "CCR build ok");
    CHECK((ccr_ns & (2u << 8)) != 0, "src AxPROT NS set");
    CHECK((ccr_ns & (2u << 22)) != 0, "dst AxPROT NS set");
    CHECK((ccr_s & (2u << 8)) == 0, "secure CCR clears NS src");
    CHECK(pl330BuildCcrBurst(0, true) == 0, "burst 0 invalid");
    CHECK(pl330BuildCcrBurst(17, true) == 0, "burst 17 invalid");

    // --- Program encode for a 720p frame must produce bytes ---
    std::vector<uint8_t> prog(256, 0);
    size_t plen = 0;
    CHECK(Pl330Mem2Mem::encodeProgramForTest(prog.data(), prog.size(), &plen, kPl330StagingPhys,
                                             kPlex720pDdrFramePhysBase + kPlex720pYuv420pBankStride,
                                             1382400u),
          "encode 720p program");
    CHECK(plen >= 16 && plen < 200, "program length sane");
    // First opcode should be DMAMOV (0xBC) family for SAR — not empty.
    CHECK(prog[0] != 0, "program not all-zero");

    // --- Content match helpers (bench ok requires these, not CSR alone) ---
    std::vector<uint8_t> a(128, 0x5A), b(128, 0x5A), c(128, 0x5A);
    c[64] = 0x00; // middle diverge
    CHECK(pl330BufferSamplesMatch(a.data(), b.data(), a.size()), "match identical");
    CHECK(!pl330BufferSamplesMatch(a.data(), c.data(), a.size()), "mid mismatch fails");
    CHECK(pl330BufferFullMatch(a.data(), b.data(), a.size()), "full match");
    CHECK(!pl330BufferFullMatch(a.data(), c.data(), a.size()), "full mismatch");

    // --- Impossible throughput: CSR-stopped fake success class ---
    // 1.382400 MB in 0.01 ms → ~138 GB/s → hard fail (incident 98k fps).
    CHECK(pl330ThroughputImpossible(1382400u, 0.01), "0.01ms/frame impossible");
    // Real floor ~0.43 ms at 3.2 GB/s peak; 15 ms CPU copy class is always plausible.
    CHECK(!pl330ThroughputImpossible(1382400u, 15.0), "15ms/frame plausible");
    CHECK(pl330ThroughputImpossible(1382400u, 0.0), "zero time impossible");
    CHECK(pl330MinPlausibleMsForBytes(1382400u) > 0.3, "min ms > 0.3");

    // --- FTR decode (Sweep 20 channel_fault dump) ---
    CHECK(std::strcmp(pl330FtrChannelTag(0), "ftr_zero") == 0, "ftr zero");
    CHECK(std::strcmp(pl330FtrChannelTag(1u << 1), "operand_invalid") == 0, "operand");
    CHECK(std::strcmp(pl330FtrChannelTag(1u << 5), "ch_rdwr_err") == 0, "rdwr");
    CHECK(std::strcmp(pl330FtrChannelTag(1u << 16), "instr_fetch_err") == 0, "ifetch");
    char bits[128];
    pl330FtrChannelBitsStr((1u << 1) | (1u << 5), bits, sizeof(bits));
    CHECK(std::strstr(bits, "OPERAND_INVALID") != nullptr, "bits operand");
    CHECK(std::strstr(bits, "CH_RDWR_ERR") != nullptr, "bits rdwr");
    CHECK(std::strcmp(pl330CsrStateTag(0x0), "stopped") == 0, "csr stopped");
    CHECK(std::strcmp(pl330CsrStateTag(0x1), "executing") == 0, "csr executing");
    CHECK(std::strcmp(pl330CsrStateTag(0xf), "faulting") == 0, "csr faulting");

    // --- Ingest mode contract: userspace PL330 is LAB ONLY ---
    CHECK(std::strcmp(ddrIngestModeName(DdrIngestMode::CpuSerial), "cpu_serial") == 0,
          "mode name serial");
    CHECK(ddrIngestModeFromConf("pl330_userspace") == DdrIngestMode::Pl330Userspace, "parse pl330");
    CHECK(ddrIngestModeFromConf("kernel_dma") == DdrIngestMode::KernelDma, "parse kernel");
    CHECK(ddrIngestModeIsLabOnly(DdrIngestMode::Pl330Userspace), "userspace PL330 lab-only");
    CHECK(!ddrIngestModeOkForTrue720pSource(DdrIngestMode::Pl330Userspace),
          "lab PL330 not true-720p product");
    CHECK(ddrIngestModeOkForTrue720pSource(DdrIngestMode::KernelDma), "kernel DMA product");
    // Product default must remain CPU serial until DMA is proven.
    {
        DdrIngestContract c;
        CHECK(c.mode == DdrIngestMode::CpuSerial, "default CpuSerial");
        CHECK(c.enforce_payload_before_doorbell, "doorbell after payload");
    }

    // --- Unbind is forbidden (string pin so docs stay linked) ---
    // Parent hang: unbind dma-pl330 on live SoC. Product path = kernel client.
    const char* forbid = "Do NOT unbind dma-pl330 from userspace";
    CHECK(std::strstr(forbid, "unbind") != nullptr, "unbind ban documented in test");

    if (fails) {
        std::printf("test_pl330_encode: %d FAIL(s)\n", fails);
        return 1;
    }
    std::printf("test_pl330_encode: OK dmago=0x%08x scratch=0x%08x\n", go_ch0_ns1,
                kPl330ProgScratchPhys);
    return 0;
}
