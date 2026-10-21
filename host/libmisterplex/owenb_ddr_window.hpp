#pragma once
// MPX OwenbRing: the DDR window that holds the owenb h264ring v1.x control block and ring.
// The window is mockable:
//   - HeapDdrWindow: for host tests.
//   - DevMemDdrWindow: for MiSTer /dev/mem, or for a sparse file that stands in for it in tests.
// Upstream's h264ring_open_devmem() maps 0x31400000..0x31FFFFFF in one piece. This window maps
// only the control page and the ring, so it never touches our idle FB window at 0x31600000
// (idle_fb_map.hpp).

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include "h264ring.h"

namespace misterplex::owenb {

inline constexpr uint32_t kCtrlPhys = H264RING_CTRL_BYTE;     // 0x31400000
inline constexpr uint32_t kRingPhys = H264RING_RING_BYTE;     // 0x31800000
inline constexpr uint32_t kRingBytes = H264RING_RING_SIZE;    // 8 MiB
inline constexpr size_t kCtrlWords = 512;                     // one 4 KiB page; the layout uses fewer than 0x58 words
inline constexpr const char* kRingLockPath = "/tmp/h264ring.lock";   // upstream's single-writer lock

class OwenbDdrWindow {
public:
    virtual ~OwenbDdrWindow() = default;
    virtual volatile uint64_t* ctrl() = 0;
    virtual volatile uint64_t* ring() = 0;
    virtual uint32_t ringSize() const = 0;
    virtual bool mapped() const = 0;
    // The library's waits and clock go through the window, so a test's fake core advances when the library sleeps.
    virtual void sleepUs(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
    virtual uint64_t nowUs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }
    virtual void barrier() { __sync_synchronize(); }
};

// Host memory with a simulated clock. onStep runs on every sleep (this is the fake core's poll).
class HeapDdrWindow final : public OwenbDdrWindow {
public:
    explicit HeapDdrWindow(uint32_t ringBytes = kRingBytes)
        : ctrl_(kCtrlWords, 0), ring_(ringBytes / 8u, 0), ringBytes_(ringBytes) {}
    volatile uint64_t* ctrl() override { return ctrl_.data(); }
    volatile uint64_t* ring() override { return ring_.data(); }
    uint32_t ringSize() const override { return ringBytes_; }
    bool mapped() const override { return true; }
    void sleepUs(unsigned us) override {
        now_ += us;
        if (onStep)
            onStep();
    }
    uint64_t nowUs() override { return now_; }
    uint64_t* ctrlWords() { return ctrl_.data(); }
    uint64_t* ringWords() { return ring_.data(); }
    std::function<void()> onStep;

private:
    std::vector<uint64_t> ctrl_;
    std::vector<uint64_t> ring_;
    uint32_t ringBytes_;
    uint64_t now_ = 1;
};

// /dev/mem, opened O_SYNC (uncached), as upstream does. path and lockPath are parameters so a host
// test can map a sparse file at the same physical offsets. open() takes upstream's flock, so only
// one program feeds the core at a time.
class DevMemDdrWindow final : public OwenbDdrWindow {
public:
    DevMemDdrWindow() = default;
    DevMemDdrWindow(const DevMemDdrWindow&) = delete;
    DevMemDdrWindow& operator=(const DevMemDdrWindow&) = delete;
    ~DevMemDdrWindow() override { close(); }

    // Returns 0 or -errno (-EBUSY: another program holds the ring lock).
    int open(const char* path = "/dev/mem", const char* lockPath = kRingLockPath) {
        close();
        if (lockPath && lockPath[0]) {
            lockFd_ = ::open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
            if (lockFd_ < 0)
                return fail(-errno);
            if (::flock(lockFd_, LOCK_EX | LOCK_NB) < 0)
                return fail(-EBUSY);
        }
        memFd_ = ::open(path, O_RDWR | O_SYNC | O_CLOEXEC);
        if (memFd_ < 0)
            return fail(-errno);
        void* c = ::mmap(nullptr, kCtrlWords * 8u, PROT_READ | PROT_WRITE, MAP_SHARED, memFd_,
                         static_cast<off_t>(kCtrlPhys));
        if (c == MAP_FAILED)
            return fail(-errno);
        ctrl_ = static_cast<volatile uint64_t*>(c);
        void* r = ::mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, memFd_,
                         static_cast<off_t>(kRingPhys));
        if (r == MAP_FAILED)
            return fail(-errno);
        ring_ = static_cast<volatile uint64_t*>(r);
        return 0;
    }

    void close() {
        if (ring_)
            ::munmap(const_cast<uint64_t*>(ring_), kRingBytes);
        if (ctrl_)
            ::munmap(const_cast<uint64_t*>(ctrl_), kCtrlWords * 8u);
        ring_ = nullptr;
        ctrl_ = nullptr;
        if (memFd_ >= 0)
            ::close(memFd_);
        if (lockFd_ >= 0)
            ::close(lockFd_);   // releases the flock
        memFd_ = lockFd_ = -1;
    }

    volatile uint64_t* ctrl() override { return ctrl_; }
    volatile uint64_t* ring() override { return ring_; }
    uint32_t ringSize() const override { return kRingBytes; }
    bool mapped() const override { return ctrl_ && ring_; }

private:
    int fail(int rc) {
        close();
        return rc;
    }
    volatile uint64_t* ctrl_ = nullptr;
    volatile uint64_t* ring_ = nullptr;
    int memFd_ = -1;
    int lockFd_ = -1;
};

}  // namespace misterplex::owenb
