#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "libmisterplex/ddr_frame_layout.hpp"

namespace {

constexpr uint32_t kDdrFrameBase = 0x30000000u;
constexpr size_t kDefaultFrameBytes = 320 * 240 * 3 / 2;

const char* formatName(misterplex::DdrFrameFormat) {
    return "yuv420p";
}

double nowSec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

double threadCpuSec() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
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

void fillPattern(uint8_t* buf, size_t len) {
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = static_cast<uint8_t>(x);
    }
}

void usage(const char* argv0) {
    std::printf(
        "Usage: %s [--sync|--no-sync] [--flush] [--host-copy]\n"
        "          [--format yuv420p] [--geometry auto|exact|plex480p]\n"
        "          [--width W --height H | --len BYTES]\n"
        "          [--loops N] [--bank 0|1]\n"
        "Writes a DDR frame window only; it does not touch SPI or kick the frame reader.\n"
        "--host-copy avoids /dev/mem and measures memcpy scaling on the build host.\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    bool useSync = true;
    bool flush = false;
    bool hostCopy = false;
    int loops = 1000;
    size_t len = kDefaultFrameBytes;
    int bank = 0;
    int width = 320;
    int height = 240;
    bool lenSet = false;
    std::string geometryMode = "auto";
    misterplex::DdrFrameFormat format = misterplex::DdrFrameFormat::Yuv420p;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sync") {
            useSync = true;
        } else if (a == "--no-sync") {
            useSync = false;
        } else if (a == "--flush") {
            flush = true;
        } else if (a == "--host-copy") {
            hostCopy = true;
        } else if (a == "--width" && i + 1 < argc) {
            width = std::atoi(argv[++i]);
        } else if (a == "--height" && i + 1 < argc) {
            height = std::atoi(argv[++i]);
        } else if (a == "--format" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "yuv420p" || v == "yuv420" || v == "i420") {
                format = misterplex::DdrFrameFormat::Yuv420p;
            } else {
                std::fprintf(stderr, "unsupported DDR format: %s (C3 frame store is yuv420p only)\n",
                             v.c_str());
                return 2;
            }
        } else if (a == "--geometry" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "plex480p" || v == "real480p") {
                geometryMode = "plex480p";
            } else if (v == "exact" || v == "coded") {
                geometryMode = "exact";
            } else if (v == "auto") {
                geometryMode = "auto";
            } else {
                std::fprintf(stderr, "unknown geometry: %s\n", v.c_str());
                return 2;
            }
        } else if (a == "--loops" && i + 1 < argc) {
            loops = std::atoi(argv[++i]);
        } else if (a == "--len" && i + 1 < argc) {
            len = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 0));
            lenSet = true;
        } else if (a == "--bank" && i + 1 < argc) {
            bank = std::atoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    const bool plex480pGeometry =
        geometryMode == "plex480p" ||
        (geometryMode == "auto" && width == misterplex::kPlex480pPresentedWidth &&
         height == misterplex::kPlex480pPresentedHeight);
    const misterplex::DdrFrameGeometry geometry =
        plex480pGeometry ? misterplex::plex480pDdrFrameGeometry()
                         : misterplex::makeDdrFrameGeometry(width, height);
    misterplex::DdrFrameLayout layout =
        lenSet ? misterplex::makeDdrFrameLayout(320, static_cast<int>(len / (320 * 2)),
                                                kDdrFrameBase)
               : misterplex::makeDdrFrameLayout(geometry, kDdrFrameBase, 0x40000u, format);
    if (lenSet) {
        layout.width = 0;
        layout.height = 0;
        layout.line_bytes = 0;
        layout.line_qwords = 0;
        layout.chroma_line_bytes = 0;
        layout.chroma_line_qwords = 0;
        layout.frame_bytes = len;
        layout.format = format;
        layout.bank_stride = misterplex::alignUpU32(static_cast<uint32_t>(len), 0x40000u);
        layout.doorbell_phys = kDdrFrameBase + layout.bank_stride * 2u - 0x1000u;
        layout.map_bytes = layout.bank_stride * 2u;
    } else {
        len = layout.frame_bytes;
    }

    if (loops <= 0 || len == 0 || bank < 0 || bank > 1 ||
        layout.bank_stride < len ||
        static_cast<size_t>(bank) * layout.bank_stride + len > layout.map_bytes) {
        std::fprintf(stderr, "bad loops/size/bank\n");
        return 2;
    }

    void* srcRaw = nullptr;
    if (posix_memalign(&srcRaw, 64, len) != 0) {
        std::perror("posix_memalign");
        return 1;
    }
    uint8_t* src = static_cast<uint8_t*>(srcRaw);
    fillPattern(src, len);

    const auto printResult = [&](double wallSec, double cpuSec) {
        const double mib = (static_cast<double>(len) * loops) / (1024.0 * 1024.0);
        const double frameMs = (wallSec * 1000.0) / static_cast<double>(loops);
        const double frameCpuMs = (cpuSec * 1000.0) / static_cast<double>(loops);
        std::printf("ddr_write_bench host_copy=%d sync=%d flush=%d loops=%d len=%zu bank=%d "
                    "format=%s coded=%dx%d display=%dx%d presented=%dx%d "
                    "present_x=%d present_y=%d line_bytes=%d line_qwords=%d "
                    "chroma_line_bytes=%d chroma_line_qwords=%d bank_stride=0x%X "
                    "map_bytes=0x%X seconds=%.6f cpu_seconds=%.6f MiB=%.3f MiBps=%.3f "
                    "frame_ms=%.3f frame_cpu_ms=%.3f fps30_budget_pct=%.1f "
                    "fps60_budget_pct=%.1f\n",
                    hostCopy ? 1 : 0, useSync ? 1 : 0, flush ? 1 : 0, loops, len, bank,
                    formatName(layout.format), layout.coded_width, layout.coded_height,
                    layout.display_width, layout.display_height, layout.presented_width,
                    layout.presented_height, layout.present_x, layout.present_y,
                    layout.line_bytes, layout.line_qwords, layout.chroma_line_bytes,
                    layout.chroma_line_qwords,
                    layout.bank_stride, layout.map_bytes, wallSec, cpuSec, mib, mib / wallSec,
                    frameMs, frameCpuMs, 100.0 * frameMs / 33.333333,
                    100.0 * frameMs / 16.666667);
    };

    if (hostCopy) {
        void* dstRaw = nullptr;
        if (posix_memalign(&dstRaw, 64, len) != 0) {
            std::perror("posix_memalign dst");
            std::free(srcRaw);
            return 1;
        }
        uint8_t* dst = static_cast<uint8_t*>(dstRaw);
        std::memcpy(dst, src, len);
        const double t0 = nowSec();
        const double c0 = threadCpuSec();
        uint64_t checksum = 0;
        for (int i = 0; i < loops; ++i) {
            src[static_cast<size_t>(i) % len] ^= static_cast<uint8_t>(i);
            std::memcpy(dst, src, len);
            checksum += dst[(static_cast<size_t>(i) * 257) % len];
        }
        const double c1 = threadCpuSec();
        const double t1 = nowSec();
        printResult(t1 - t0, c1 - c0);
        std::printf("checksum=%llu\n", static_cast<unsigned long long>(checksum));
        std::free(dstRaw);
        std::free(srcRaw);
        return 0;
    }

    int flags = O_RDWR | O_CLOEXEC;
    if (useSync)
        flags |= O_SYNC;
    int fd = ::open("/dev/mem", flags);
    if (fd < 0) {
        std::perror("open /dev/mem");
        std::free(srcRaw);
        return 1;
    }

    void* map = mmap(nullptr, layout.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     kDdrFrameBase);
    if (map == MAP_FAILED) {
        std::perror("mmap");
        ::close(fd);
        std::free(srcRaw);
        return 1;
    }

    uint8_t* dst = static_cast<uint8_t*>(map) + static_cast<size_t>(bank) * layout.bank_stride;
    std::memcpy(dst, src, len);
    __sync_synchronize();
    if (flush && !cleanDcacheRange(dst, len)) {
        std::perror("cacheflush warmup");
        munmap(map, layout.map_bytes);
        ::close(fd);
        std::free(srcRaw);
        return 1;
    }

    const double t0 = nowSec();
    const double c0 = threadCpuSec();
    for (int i = 0; i < loops; ++i) {
        src[static_cast<size_t>(i) % len] ^= static_cast<uint8_t>(i);
        std::memcpy(dst, src, len);
        __sync_synchronize();
        if (flush && !cleanDcacheRange(dst, len)) {
            std::perror("cacheflush");
            munmap(map, layout.map_bytes);
            ::close(fd);
            std::free(srcRaw);
            return 1;
        }
    }
    __sync_synchronize();
    const double c1 = threadCpuSec();
    const double t1 = nowSec();

    printResult(t1 - t0, c1 - c0);

    munmap(map, layout.map_bytes);
    ::close(fd);
    std::free(srcRaw);
    return 0;
}
