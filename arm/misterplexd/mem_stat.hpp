// Lab-only heap telemetry (P6-MEMORY). Off unless MPX_MEMSTAT_SEC=<1..3600> is set.
// A detached thread logs one "MEMSTAT" line per period. mallinfo2() is zero on the
// MiSTer's glibc 2.31 and under ASan (ASan replaces the allocator), so VmRSS, VmHWM,
// anon, and MemAvailable come from /proc. Queue depths are whatever the daemon last
// published (out_, ready_, and the demuxer probe-packet bound).
// No behaviour change when the variable is unset.
#pragma once
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <chrono>
#include <thread>

namespace misterplex::mem_stat {

// 0 = disabled. Accepts 1..3600 seconds; anything else (empty, junk, 0, negative, huge) disables.
inline int parsePeriodSec(const char* v) {
    if (!v || !*v)
        return 0;
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (!end || *end != '\0')
        return 0;
#ifdef MUT_MEMSTAT_NO_UPPER
    if (n < 1)
        return 0;
#else
    if (n < 1 || n > 3600)
        return 0;
#endif
    return static_cast<int>(n);
}

struct Sample {
    unsigned long long arenaBytes = 0;  // sbrk + arena heaps obtained from the kernel (mallinfo2.arena)
    unsigned long long mmapBytes = 0;   // chunks served by mmap (hblkhd)
    unsigned long long inuseBytes = 0;  // live malloc bytes in arenas (uordblks)
    unsigned long long freeBytes = 0;   // free bytes retained in arenas (fordblks)
    unsigned long long topPad = 0;      // releasable top (keepcost)
    unsigned long long vmRssKb = 0;     // /proc/self/status VmRSS
    unsigned long long vmHwmKb = 0;     // /proc/self/status VmHWM
    unsigned long long anonKb = 0;      // RssAnon, else smaps_rollup Anonymous
    unsigned long long memAvailableKb = 0;
    unsigned long long outQ = 0;        // owenb session out_ depth
    unsigned long long readyQ = 0;      // av source ready_ depth
    unsigned long long probeQ = 0;      // max_probe_packets bound applied to the open demuxer
    unsigned long long ytdlpKb = 0;     // peak VmHWM of the last yt-dlp resolve
};

struct QueueDepths {
    unsigned long long out = 0;
    unsigned long long ready = 0;
    unsigned long long probe = 0;
};

inline std::atomic<unsigned long long>& ytdlpPeakKbSlot() {
    static std::atomic<unsigned long long> v{0};
    return v;
}

inline void noteYtdlpPeakKb(unsigned long long kb) {
    auto& slot = ytdlpPeakKbSlot();
    unsigned long long cur = slot.load();
    while (kb > cur && !slot.compare_exchange_weak(cur, kb)) {
    }
}

inline std::mutex& queueProbeMu() {
    static std::mutex m;
    return m;
}

inline std::function<QueueDepths()>& queueProbeFn() {
    static std::function<QueueDepths()> fn;
    return fn;
}

inline void setQueueProbe(std::function<QueueDepths()> fn) {
    std::lock_guard<std::mutex> lk(queueProbeMu());
    queueProbeFn() = std::move(fn);
}

inline unsigned long long fieldKb(const std::string& text, const char* key) {
    const std::string needle = std::string(key) + ":";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.compare(0, needle.size(), needle) != 0)
            continue;
        const char* p = line.c_str() + needle.size();
        while (*p == ' ' || *p == '\t')
            ++p;
        char* end = nullptr;
        const unsigned long v = std::strtoul(p, &end, 10);
        if (end != p)
            return v;
    }
    return 0;
}

inline void parseProcStatus(const std::string& text, Sample& s) {
    s.vmRssKb = fieldKb(text, "VmRSS");
    s.vmHwmKb = fieldKb(text, "VmHWM");
    const unsigned long long anon = fieldKb(text, "RssAnon");
    if (anon)
        s.anonKb = anon;
}

inline void parseMeminfo(const std::string& text, Sample& s) {
    s.memAvailableKb = fieldKb(text, "MemAvailable");
}

inline void parseSmapsRollup(const std::string& text, Sample& s) {
    if (s.anonKb == 0)
        s.anonKb = fieldKb(text, "Anonymous");
}

inline std::string readProcFile(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string format(const Sample& s, long seq) {
    char buf[640];
    const unsigned long long live = s.inuseBytes + s.mmapBytes;
    std::snprintf(buf, sizeof buf,
                  "MEMSTAT seq=%ld live_kb=%llu inuse_kb=%llu mmap_kb=%llu free_kb=%llu arena_kb=%llu keep_kb=%llu"
                  " vmrss_kb=%llu vmhwm_kb=%llu anon_kb=%llu memavail_kb=%llu out_q=%llu ready_q=%llu probe_q=%llu ytdlp_kb=%llu",
                  seq, live / 1024, s.inuseBytes / 1024, s.mmapBytes / 1024, s.freeBytes / 1024,
                  s.arenaBytes / 1024, s.topPad / 1024, s.vmRssKb, s.vmHwmKb, s.anonKb, s.memAvailableKb,
                  s.outQ, s.readyQ, s.probeQ, s.ytdlpKb);
    return buf;
}

inline Sample sampleNow() {
    Sample s;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 m = ::mallinfo2();
    s.arenaBytes = m.arena;
    s.mmapBytes = m.hblkhd;
    s.inuseBytes = m.uordblks;
    s.freeBytes = m.fordblks;
    s.topPad = m.keepcost;
#endif
    parseProcStatus(readProcFile("/proc/self/status"), s);
    parseMeminfo(readProcFile("/proc/meminfo"), s);
    parseSmapsRollup(readProcFile("/proc/self/smaps_rollup"), s);
    {
        std::lock_guard<std::mutex> lk(queueProbeMu());
        if (queueProbeFn()) {
            const QueueDepths q = queueProbeFn()();
            s.outQ = q.out;
            s.readyQ = q.ready;
            s.probeQ = q.probe;
        }
    }
    s.ytdlpKb = ytdlpPeakKbSlot().load();
    return s;
}

// Starts the logger when MPX_MEMSTAT_SEC is valid; returns the period (0 = not started).
inline int startFromEnv() {
    const int period = parsePeriodSec(std::getenv("MPX_MEMSTAT_SEC"));
    if (period <= 0)
        return 0;
    std::thread([period] {
        for (long seq = 0;; ++seq) {
            std::fprintf(stderr, "misterplexd: %s\n", format(sampleNow(), seq).c_str());
            std::this_thread::sleep_for(std::chrono::seconds(period));
        }
    }).detach();
    return period;
}

} // namespace misterplex::mem_stat
