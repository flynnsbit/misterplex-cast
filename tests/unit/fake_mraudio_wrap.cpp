// Host e2e builds of misterplexd: /dev/MrAudio as MiSTer's driver presents it to user space
// (sound/drivers/MiSTer-audio-spi.c), link-wrapped into the daemon
// (-Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close). Only that exact path
// is faked; every other path and descriptor goes to libc.
//   - open() formats one status line, "rptr: R, wptr: W, len: L, comp: 0", read back by
//     read() (the driver does this in device_open);
//   - write() copies PCM into the 512 KiB ring and advances wptr; it never blocks;
//   - open() and close() never move the ring origin.
// The consumer side (the FPGA's DMA read pointer, MAST, the consumed clock) belongs to the fake
// core, so rptr follows wptr here (an empty queue).
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" ssize_t __real_read(int fd, void* buffer, size_t size);
extern "C" ssize_t __real_write(int fd, const void* buffer, size_t size);
extern "C" int __real_close(int fd);

namespace {
constexpr const char* kPath = "/dev/MrAudio";
constexpr uint32_t kRingBytes = 512u * 1024u;
std::mutex mu;
std::map<int, std::string> open_; // fake descriptor -> status text not yet read
int nextFd = 1 << 20;             // far above any descriptor the kernel hands out here
uint32_t wptr = 0;
uint64_t totalWritten = 0;        // every PCM byte accepted (diagnostics for the n2 e2e)

// The fake core's model of the HPS SPI buf_info (sys/alsa.sv: the producer write pointer the
// consumer reads behind): the ring position, published where core_gate_responder's fake core
// reads it (fake_plex_core.hpp kProducerSlotAddr). A fake-only address, the last qword of the
// 0x30140000 mailbox page, which neither the RTL nor the daemon uses.
#ifdef MPX_DEVMEM_PATH
constexpr off_t kProducerSlotPage = 0x30140000;
constexpr size_t kProducerSlotOffset = 0xFF8;
constexpr uint32_t kProducerSlotMagic = 0x4950534Du; // "MSPI"
volatile uint64_t* producerSlot = nullptr;
bool producerSlotTried = false;
void publishProducer() { // under mu
    if (!producerSlotTried) {
        producerSlotTried = true;
        const int fd = __real_open(MPX_DEVMEM_PATH, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            void* p = ::mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                             kProducerSlotPage);
            __real_close(fd);
            if (p != MAP_FAILED)
                producerSlot = reinterpret_cast<volatile uint64_t*>(static_cast<uint8_t*>(p) +
                                                                    kProducerSlotOffset);
        }
    }
    if (producerSlot)
        *producerSlot = (static_cast<uint64_t>(wptr) << 32) | kProducerSlotMagic;
}
#else
void publishProducer() {}
#endif

void reportWritten(const char* when) {
    std::fprintf(stderr, "FAKE_MRAUDIO %s written_bytes=%llu pairs=%llu\n", when,
                 static_cast<unsigned long long>(totalWritten),
                 static_cast<unsigned long long>(totalWritten / 4));
}
struct ExitReport {
    ~ExitReport() { reportWritten("exit"); }
} exitReport;
} // namespace

extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    if (path && std::strcmp(path, kPath) == 0) {
        std::lock_guard<std::mutex> g(mu);
        const int fd = nextFd++;
        char text[96];
        std::snprintf(text, sizeof text, "rptr: %6u, wptr: %6u, len: %6u, comp: 0\n", wptr, wptr,
                      0u);
        open_[fd] = text;
        publishProducer();
        return fd;
    }
    return __real_open(path, flags, mode);
}

extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = open_.find(fd);
        if (it != open_.end()) {
            const size_t n = std::min(size, it->second.size());
            std::memcpy(buffer, it->second.data(), n);
            it->second.erase(0, n);
            return static_cast<ssize_t>(n);
        }
    }
    return __real_read(fd, buffer, size);
}

extern "C" ssize_t __wrap_write(int fd, const void* buffer, size_t size) {
    {
        std::lock_guard<std::mutex> g(mu);
        if (open_.count(fd)) {
            wptr = static_cast<uint32_t>((wptr + size) % kRingBytes);
            totalWritten += size;
            publishProducer();
            return static_cast<ssize_t>(size);
        }
    }
    return __real_write(fd, buffer, size);
}

extern "C" int __wrap_close(int fd) {
    {
        std::lock_guard<std::mutex> g(mu);
        if (open_.erase(fd)) {
            reportWritten("close");
            return 0;
        }
    }
    return __real_close(fd);
}
