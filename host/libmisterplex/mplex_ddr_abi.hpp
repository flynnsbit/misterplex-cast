#pragma once
#include <cstdint>
#include <sys/ioctl.h>

// Cached 2×2MiB contig slots from kmod/mplex_ddr (alloc_pages order 9).
// FPGA L4 DYN_BASE_EN=1 reads these PAs. Userspace PL330 poke stays banned.
namespace misterplex {

constexpr unsigned long kMplexCachedPgoff = 0x800UL; // pages; mmap offset 8MiB
constexpr uint32_t kMplexCachedSlotBytes = 2u * 1024u * 1024u;

struct MplexCachedPhys {
    uint32_t phys[2];
    uint32_t slot_bytes;
};

constexpr unsigned kMplexIocMagic = 'M';
#define MPLEX_IOC_CACHED_PHYS _IOR('M', 1, misterplex::MplexCachedPhys)

// K1 (k1-kmod-wc-window-note-1004 §1): the R1 host idle FB window, IDLE0 0x31600000 + IDLE1 0x31700000,
// one 2 MiB write-combining mapping at mmap offset kMplexIdleFbPgoff pages (= phys - 0x30000000). An old
// kmod answers MPLEX_IOC_CAPS with ENOTTY. Mirrors kmod/mplex_ddr/mplex_ddr.c (tests/unit/test_mplex_abi_sync.sh).
constexpr uint32_t kMplexIdleFbPhys = 0x31600000u;
constexpr uint32_t kMplexIdleFbBytes = 0x200000u;
constexpr unsigned long kMplexIdleFbPgoff = 0x1600UL; // pages (4 KiB)
constexpr uint32_t kMplexCapIdleFbWc = 1u;            // MPLEX_IOC_CAPS bit0
#define MPLEX_IOC_CAPS _IOR('M', 2, uint32_t)

static_assert(kMplexIdleFbPhys == 0x30000000u + (kMplexIdleFbPgoff << 12), "idle-FB pgoff = phys - window base");
static_assert(kMplexIdleFbPgoff >= kMplexCachedPgoff + ((2u * kMplexCachedSlotBytes) >> 12),
              "idle-FB pgoff is disjoint from the cached slots (0x800..0xBFF)");
static_assert(kMplexIdleFbPgoff >= (0x800000u >> 12), "idle-FB pgoff is past the 8 MiB offset-0 window");
static_assert((kMplexIdleFbPhys & 0xFFFu) == 0 && (kMplexIdleFbBytes & 0xFFFu) == 0, "page aligned");

} // namespace misterplex
