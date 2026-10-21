#pragma once
// Virt → phys via /proc/self/pagemap. Heap is STUB (0). Fabric slots may be REAL.

#include <cstdint>
#include <fcntl.h>
#include <unistd.h>

namespace misterplex {

inline uint32_t resolveCachedSrcPhysFromPagemap(const void* virt, size_t len) {
    if (!virt || len == 0)
        return 0;
    const uintptr_t u = reinterpret_cast<uintptr_t>(virt);
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    if (page == 0 || (u % page) != 0)
        return 0;
    const int fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    const uint64_t idx = static_cast<uint64_t>(u / page);
    if (::lseek(fd, static_cast<off_t>(idx * 8ull), SEEK_SET) < 0) {
        ::close(fd);
        return 0;
    }
    uint64_t ent = 0;
    const ssize_t n = ::read(fd, &ent, sizeof(ent));
    ::close(fd);
    if (n != static_cast<ssize_t>(sizeof(ent)))
        return 0;
    if ((ent & (1ull << 63)) == 0)
        return 0;
    const uint64_t pfn = ent & ((1ull << 55) - 1ull);
    if (pfn == 0)
        return 0;
    const uint64_t phys = pfn * static_cast<uint64_t>(page);
    if (phys > 0xffffffffull)
        return 0;
    return static_cast<uint32_t>(phys);
}

// PA of any byte (page + offset). 0 if pagemap missing.
inline uint32_t resolveCachedPhysByte(const void* virt) {
    if (!virt)
        return 0;
    const uintptr_t u = reinterpret_cast<uintptr_t>(virt);
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    if (page == 0)
        return 0;
    const uintptr_t aligned = u & ~(page - 1u);
    const uint32_t base = resolveCachedSrcPhysFromPagemap(reinterpret_cast<const void*>(aligned),
                                                          page);
    if (base == 0)
        return 0;
    return base + static_cast<uint32_t>(u - aligned);
}

// First-page PA if every page in [virt, virt+len) is present and sequential.
inline uint32_t resolveContiguousCachedPhys(const void* virt, size_t len) {
    if (!virt || len == 0)
        return 0;
    const uintptr_t u = reinterpret_cast<uintptr_t>(virt);
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    if (page == 0 || (u % page) != 0)
        return 0;
    const int fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    const size_t np = (len + page - 1u) / page;
    uint64_t firstPfn = 0;
    for (size_t i = 0; i < np; ++i) {
        const uint64_t idx = static_cast<uint64_t>((u / page) + i);
        if (::lseek(fd, static_cast<off_t>(idx * 8ull), SEEK_SET) < 0) {
            ::close(fd);
            return 0;
        }
        uint64_t ent = 0;
        const ssize_t n = ::read(fd, &ent, sizeof(ent));
        if (n != static_cast<ssize_t>(sizeof(ent)) || (ent & (1ull << 63)) == 0) {
            ::close(fd);
            return 0;
        }
        const uint64_t pfn = ent & ((1ull << 55) - 1ull);
        if (pfn == 0) {
            ::close(fd);
            return 0;
        }
        if (i == 0)
            firstPfn = pfn;
        else if (pfn != firstPfn + i) {
            ::close(fd);
            return 0;
        }
    }
    ::close(fd);
    const uint64_t phys = firstPfn * static_cast<uint64_t>(page);
    if (phys > 0xffffffffull)
        return 0;
    return static_cast<uint32_t>(phys);
}

} // namespace misterplex
