#pragma once
// Slow SPI windows and the hot-path error text (rel-480i-costall-1004, GLASS-A6).
//
// Every misterplexd SPI transaction runs inside fpga_spi.cpp SpiExclusive: spiMutex,
// the /tmp/misterplex_spi.lock flock, then MainSafeWindow. That window SIGSTOPs
// Main_MiSTer and polls /proc/<pid>/task/*/stat until every Main task reports 'T'
// (waitStopped: 250 polls of usleep(200), so "50 ms" of sleeps but ~100 ms of wall
// time with the /proc scans), up to 8 attempts, and backs off while Main is frozen
// mid-transaction. A window over kSpiWindowSlowUs is kept in slowSpiWindows() with
// its phases, the owner's CPU time and, after a park timeout, the Main task that
// would not stop (tid, state, comm, wchan).
//
// spiMutex also guards FpgaSpi::err_. The DDR-only hot paths (the presentation read
// every ~2 ms, every AU push) clear it on success, so they waited for any SPI
// window. With MPX_OVERLAY_DECOUPLE=1 they update it only when the lock is free
// (updateErrorUnlessBusy) and count the skips.
//
// The same header ships on every daemon line. A line without the SpiExclusive
// diagnostics (the 480p avsync and 240p ports) uses only updateErrorUnlessBusy; the
// window ring, RunningMax and labDiagnostics are then unused.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace misterplex {

constexpr int64_t kSpiWindowSlowUs = 20000;

// Lab diagnostics (review's review of 323780bb): the SPI-window phase stamps, the
// park-timeout /proc capture and the window ring/counters run while Main is stopped, and
// the extra log fields (present_lock tid/blocker, io_levels suffix, spi_window lines,
// thread roles) change the daemon's log. All of it is on only while a lab overlay flag
// (MPX_OVERLAY_DECOUPLE, MPX_OVERLAY_ONCHANGE) or MPX_SPI_DIAG=1 is set; off, logging and
// timing match the path before 323780bb. Set per session (media_player.cpp).
constexpr const char* kSpiDiagEnv = "MPX_SPI_DIAG";
inline std::atomic<bool>& labDiagnostics() {
    static std::atomic<bool> on{false};
    return on;
}
inline bool spiDiagFromEnvironment() {
    const char* v = std::getenv(kSpiDiagEnv);
    return v && v[0] == '1' && v[1] == 0;
}

inline int64_t spiMonoUs() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
// This thread's CPU time in us, -1 when unavailable.
inline int64_t spiThreadCpuUs() {
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
// This thread's kernel tid (as in /proc/<pid>/task and mon229), cached per thread.
inline int spiTid() {
    static thread_local const int tid = static_cast<int>(::syscall(SYS_gettid));
    return tid;
}

struct SpiWindowRecord {
    const char* caller = "?"; // the FpgaSpi method that opened the window
    int tid = 0;              // the thread that owned it
    int64_t startUs = 0;      // monotonic: when it asked for spiMutex
    int64_t mutexWaitUs = 0;  // spiMutex
    int64_t flockWaitUs = 0;  // /tmp/misterplex_spi.lock
    int64_t parkUs = 0;       // MainSafeWindow: Main stopped and checked
    int64_t bodyUs = 0;       // the transaction itself, Main parked
    int64_t cpuUs = -1;       // the owner's CPU time over the window; -1: unknown
    int attempts = 0, stopTimeouts = 0, busyRetries = 0;
    bool safe = false;
    int unparkedTid = 0;      // after a park timeout: a Main task still not stopped
    char unparkedState = 0;
    char unparkedComm[16] = {};
    char unparkedWchan[32] = {};
    int64_t totalUs() const { return mutexWaitUs + flockWaitUs + parkUs + bodyUs; }
};

inline std::string formatSpiWindowLine(const SpiWindowRecord& r) {
    char buf[400];
    int n = std::snprintf(
        buf, sizeof buf,
        "media: spi_window caller=%s tid=%d start_ms=%.1f total_ms=%.1f mutex_ms=%.1f "
        "flock_ms=%.1f park_ms=%.1f body_ms=%.1f cpu_ms=%.1f attempts=%d stop_timeouts=%d "
        "busy_retries=%d safe=%d",
        r.caller, r.tid, r.startUs / 1000.0, r.totalUs() / 1000.0, r.mutexWaitUs / 1000.0,
        r.flockWaitUs / 1000.0, r.parkUs / 1000.0, r.bodyUs / 1000.0,
        r.cpuUs < 0 ? -1.0 : r.cpuUs / 1000.0, r.attempts, r.stopTimeouts, r.busyRetries,
        r.safe ? 1 : 0);
    if (r.unparkedTid && n > 0 && n < static_cast<int>(sizeof buf))
        std::snprintf(buf + n, sizeof buf - static_cast<size_t>(n),
                      " unparked=%d:%c:%s:%s", r.unparkedTid,
                      r.unparkedState ? r.unparkedState : '?',
                      r.unparkedComm[0] ? r.unparkedComm : "?",
                      r.unparkedWchan[0] ? r.unparkedWchan : "?");
    return buf;
}

// The latest slow windows. take() hands them over oldest first and how many older
// ones were overwritten since the last take.
class SpiWindowLog {
public:
    static constexpr size_t kCapacity = 16;
    void push(const SpiWindowRecord& r) {
        std::lock_guard<std::mutex> lock(mu_);
        ring_[(head_ + count_) % kCapacity] = r;
        if (count_ < kCapacity) {
            ++count_;
        } else {
            head_ = (head_ + 1) % kCapacity;
            ++dropped_;
        }
        ++total_;
    }
    size_t take(SpiWindowRecord* out, size_t max, uint64_t& dropped) {
        std::lock_guard<std::mutex> lock(mu_);
        size_t n = 0;
        while (count_ && n < max) {
            out[n++] = ring_[head_];
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        dropped = dropped_;
        dropped_ = 0;
        return n;
    }
    uint64_t total() {
        std::lock_guard<std::mutex> lock(mu_);
        return total_;
    }

private:
    std::mutex mu_;
    SpiWindowRecord ring_[kCapacity];
    size_t head_ = 0, count_ = 0;
    uint64_t dropped_ = 0, total_ = 0;
};

inline SpiWindowLog& slowSpiWindows() {
    static SpiWindowLog log;
    return log;
}

// Sets (msg) or clears (nullptr) `err` only if `m` is free right now (or already
// held by this thread: m may be recursive). False: busy, nothing changed.
template <class Mutex>
bool updateErrorUnlessBusy(Mutex& m, std::string& err, const char* msg) {
    std::unique_lock<Mutex> lock(m, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    if (msg)
        err = msg;
    else
        err.clear();
    return true;
}

// The largest value noted since the last take(true); lock-free, any thread.
class RunningMax {
public:
    void note(int64_t v) {
        int64_t seen = v_.load(std::memory_order_relaxed);
        while (v > seen && !v_.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
        }
    }
    int64_t take(bool reset) {
        return reset ? v_.exchange(0, std::memory_order_relaxed) : v_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> v_{0};
};

} // namespace misterplex
