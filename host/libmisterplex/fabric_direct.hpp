#pragma once
// 720p publish slots: contiguous cached System-RAM (pagemap PA) so PL330 can
// DMA publication into the reserved frame banks. 480p still uses memcpy.

#include "libmisterplex/cached_src_phys.hpp"
#include "libmisterplex/mplex_ddr_abi.hpp"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace misterplex {

struct FabricDirectSlot {
    uint32_t phys = 0;
    uint8_t* virt = nullptr;
    size_t map_bytes = 0;
};

struct FabricDirectAlloc {
    FabricDirectSlot slot[2]{};
    const char* how = "off";
    bool real() const { return slot[0].phys != 0 && slot[1].phys != 0; }
};

inline void releaseFabricDirectAlloc(FabricDirectAlloc& a);

inline bool allocateOneCachedSlot(FabricDirectSlot& s, size_t bytes) {
    s = FabricDirectSlot{};
    if (bytes == 0)
        return false;
    const size_t twoMeg = 2u * 1024u * 1024u;
    size_t need = bytes;
    if (need < twoMeg)
        need = twoMeg;
    need = (need + twoMeg - 1u) & ~(twoMeg - 1u);
    void* p = ::mmap(nullptr, need, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return false;
    std::memset(p, 0, bytes);
    (void)::mlock(p, need);
    const uint32_t phys = resolveContiguousCachedPhys(p, bytes);
    if (phys == 0) {
        ::munmap(p, need);
        return false;
    }
    s.virt = static_cast<uint8_t*>(p);
    s.phys = phys;
    s.map_bytes = need;
    return true;
}

inline bool allocateFabricDirectSlots(FabricDirectAlloc& a, size_t bytes, bool = false,
                                      int minSlots = 2) {
    releaseFabricDirectAlloc(a);
    if (minSlots < 1)
        minSlots = 1;
    if (minSlots > 2)
        minSlots = 2;
    if (minSlots == 2 && bytes <= kMplexCachedSlotBytes) {
        const int fd = ::open("/dev/mplex_ddr", O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            MplexCachedPhys p{};
            if (::ioctl(fd, MPLEX_IOC_CACHED_PHYS, &p) == 0 && p.phys[0] && p.phys[1] &&
                p.slot_bytes >= bytes) {
                const long pg = ::sysconf(_SC_PAGESIZE);
                const off_t off = static_cast<off_t>(kMplexCachedPgoff) *
                                  (pg > 0 ? pg : 4096);
                void* v = ::mmap(nullptr, 2u * kMplexCachedSlotBytes,
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
                if (v != MAP_FAILED) {
                    a.slot[0].virt = static_cast<uint8_t*>(v);
                    a.slot[0].phys = p.phys[0];
                    a.slot[0].map_bytes = 2u * kMplexCachedSlotBytes;
                    a.slot[1].virt = static_cast<uint8_t*>(v) + kMplexCachedSlotBytes;
                    a.slot[1].phys = p.phys[1];
                    a.slot[1].map_bytes = 0;
                    a.how = "kmod_cached";
                    ::close(fd);
                    return true;
                }
            }
            ::close(fd);
        }
    }
    int got = 0;
    for (int i = 0; i < minSlots; ++i) {
        if (!allocateOneCachedSlot(a.slot[i], bytes))
            break;
        ++got;
    }
    if (got < minSlots) {
        releaseFabricDirectAlloc(a);
        return false;
    }
    a.how = "anon_contig";
    return true;
}

inline void releaseFabricDirectAlloc(FabricDirectAlloc& a) {
    for (int i = 0; i < 2; ++i) {
        if (a.slot[i].virt && a.slot[i].map_bytes)
            ::munmap(a.slot[i].virt, a.slot[i].map_bytes);
        a.slot[i] = FabricDirectSlot{};
    }
    a.how = "off";
}

inline uint32_t refreshFabricSlotPhys(FabricDirectSlot& s, size_t len) {
    if (!s.virt)
        return 0;
    s.phys = resolveContiguousCachedPhys(s.virt, len);
    return s.phys;
}
inline const char* fabricDirectHow(const FabricDirectAlloc& a) { return a.how; }
inline bool failClosedFabricDirectPhysPair(uint32_t a, uint32_t b) { return a == 0 || b == 0; }
inline bool fabricDirectPhysPairReal(uint32_t a, uint32_t b) { return a != 0 && b != 0; }
inline int probePagemapPfnVisibility() { return 1; }
inline const char* pagemapPfnVisName(int) { return "pagemap"; }

// 720p present: DMA publication when src PA is known. 480p stays memcpy.
inline bool preferPl330720pPublish(int bankW, int bankH, uint32_t srcPhys) {
    return bankW == 1280 && bankH == 720 && srcPhys != 0;
}

// v0.3/v0.4 shipped 40 ms of video lead so vsync is never starved.
// Named so tests can pin the default (RED: 0 must fail; GREEN: 40).
// HEAD briefly left presentLeadMs_=0 while main logged 40(default).
constexpr int kDefaultPresentLeadMs = 40;

// 480p/240p/L4 DDR present uses hold-only (drop forced 0 via
// avResyncDropMsForPresent) so Star Trek-class lag cannot shred unique rate.
// Conf AV_RESYNC_DROP_MS stays 80 for non-hold-only paths; every-decoded
// present (480p/240p/L4) must not 2:1-drop. Do not change RequireReleased.
inline int avResyncDropMsForPresent(int confMs, bool holdOnlyPresent) {
    return holdOnlyPresent ? 0 : confMs;
}

constexpr int kPlex960Yuv420pBytes = 960 * 540 * 3 / 2;

} // namespace misterplex
