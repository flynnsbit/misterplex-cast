// Gate 8: a MiSTer with no NTP boots at 1970. One clock-rescue module, in order:
//   1. hardware RTC (/dev/rtc0) when the optional board is present and sane
//   2. SNTP, two hosts, a few seconds total
//   3. HTTPS Date from a fixed host list, chain and hostname verified, certificate
//      time-validity skipped for that request only (the clock is not valid yet)
// Certificate verification for ordinary YouTube requests stays on. The Date fetch
// must not call back into this rescue or into a TLS client that requires a sane clock.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace misterplex {

// 2020-01-01 UTC. YouTube's current CA chain and this tree are both newer.
// int64, not time_t: Arm GNU's time_t is 32-bit, and 2100-01-01 (4102444800)
// wraps to a negative ceiling. Every later comparison then rejects a correct
// 2026 clock, so the rescue never stops and YouTube waits for a step that
// cannot happen.
inline constexpr std::int64_t kPlausibleWallEpoch = 1577836800;
inline constexpr std::int64_t kPlausibleWallCeiling = 4102444800LL; // 2100-01-01 UTC
inline constexpr std::time_t kHttpsDateSkewSec = 300;            // hosts must agree within 5 minutes

// Compile time of this binary, UTC. A rescued clock must not be older than the build.
inline std::time_t wallClockBuildFloor() {
    const char* d = __DATE__; // "Mmm dd yyyy"
    const char* tim = __TIME__; // "hh:mm:ss"
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon = 0;
    for (int i = 0; i < 12; ++i) {
        if (d[0] == months[i * 3] && d[1] == months[i * 3 + 1] && d[2] == months[i * 3 + 2]) {
            mon = i;
            break;
        }
    }
    const int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    const int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    const int hh = (tim[0] - '0') * 10 + (tim[1] - '0');
    const int mm = (tim[3] - '0') * 10 + (tim[4] - '0');
    const int ss = (tim[6] - '0') * 10 + (tim[7] - '0');
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    const std::time_t floor = timegm(&tm);
    if (floor > 0)
        return floor;
    return static_cast<std::time_t>(kPlausibleWallEpoch);
}

inline bool wallTimePlausible(std::time_t t) {
#if defined(MUT_WALL_TRUST_1970)
    (void)t;
    return true;
#else
    const std::int64_t v = static_cast<std::int64_t>(t);
    return v >= kPlausibleWallEpoch && v < kPlausibleWallCeiling &&
           v >= static_cast<std::int64_t>(wallClockBuildFloor());
#endif
}

enum class WallClockStatus { AlreadySane, Stepped, Unavailable };
enum class WallClockSource { None, Rtc, Sntp, HttpsDate };

inline const char* wallClockSourceName(WallClockSource s) {
    switch (s) {
    case WallClockSource::Rtc: return "RTC";
    case WallClockSource::Sntp: return "SNTP";
    case WallClockSource::HttpsDate: return "HTTPS-Date";
    case WallClockSource::None: break;
    }
    return "none";
}

// present: the device node opened. time: a decoded UTC reading (nullopt if the
// chip is missing, unreadable, or still holding garbage).
struct RtcSample {
    bool present = false;
    std::optional<std::time_t> time;
};

struct WallClockOps {
    std::function<std::time_t()> now;
    std::function<RtcSample()> readRtc;
    std::function<bool(std::time_t)> writeRtc;
    std::function<std::optional<std::time_t>(int timeoutMs)> queryNtp;
    std::function<std::optional<std::time_t>(int timeoutMs)> queryHttps;
    std::function<bool(std::time_t)> step;
};

// IMF-fix HTTP-date ("Sun, 06 Nov 1994 08:49:37 GMT"). Empty on a bad value.
inline std::optional<std::time_t> parseImfFixDate(const std::string& value) {
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {};
    if (std::sscanf(value.c_str(), "%*3s, %d %3s %d %d:%d:%d GMT", &day, mon, &year, &hh, &mm, &ss) != 6)
        return std::nullopt;
    int month = -1;
    for (int i = 0; i < 12; ++i) {
        if (mon[0] == months[i * 3] && mon[1] == months[i * 3 + 1] && mon[2] == months[i * 3 + 2]) {
            month = i;
            break;
        }
    }
    if (month < 0 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60)
        return std::nullopt;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    const std::time_t t = timegm(&tm);
    if (t < 0)
        return std::nullopt;
    return t;
}

// Both answers valid: they must agree within 5 minutes (the earlier one is kept).
// One valid answer is enough when the other host did not answer or was implausible.
inline std::optional<std::time_t> acceptHttpsDatePair(const std::optional<std::time_t>& a,
                                                     const std::optional<std::time_t>& b) {
    const bool av = a && wallTimePlausible(*a);
    const bool bv = b && wallTimePlausible(*b);
    if (av && bv) {
        const std::time_t d = *a > *b ? *a - *b : *b - *a;
        if (d > kHttpsDateSkewSec)
            return std::nullopt;
        return *a < *b ? a : b;
    }
    if (av)
        return a;
    if (bv)
        return b;
    return std::nullopt;
}

inline WallClockStatus ensurePlausibleWallClock(const WallClockOps& ops, std::string* err,
                                                WallClockSource* source = nullptr) {
    // The Date fetch and any TLS client it uses must not re-enter this function.
    // A re-entry returns immediately instead of waiting on itself.
    thread_local int depth = 0;
    if (depth > 0) {
        if (err)
            *err = "wall clock rescue re-entered";
        return WallClockStatus::Unavailable;
    }
    struct Depth {
        int& d;
        explicit Depth(int& d) : d(d) { ++d; }
        ~Depth() { --d; }
    } guard(depth);
    if (source)
        *source = WallClockSource::None;

    const std::time_t now = ops.now ? ops.now() : std::time(nullptr);
    if (wallTimePlausible(now))
        return WallClockStatus::AlreadySane;

    auto stepTo = [&](WallClockSource src, std::time_t t, bool writeBack) -> WallClockStatus {
        if (!wallTimePlausible(t))
            return WallClockStatus::Unavailable;
        if (!ops.step || !ops.step(t)) {
            if (err)
                *err = "wall clock is implausible and settimeofday failed";
            return WallClockStatus::Unavailable;
        }
        if (ops.now && !wallTimePlausible(ops.now())) {
            if (err)
                *err = "wall clock stayed implausible after the step";
            return WallClockStatus::Unavailable;
        }
        if (writeBack && ops.writeRtc)
            ops.writeRtc(t);
        if (source)
            *source = src;
        return WallClockStatus::Stepped;
    };

    bool rtcPresent = false;
    if (ops.readRtc) {
        const RtcSample rtc = ops.readRtc();
        rtcPresent = rtc.present;
        if (rtc.time && wallTimePlausible(*rtc.time))
            return stepTo(WallClockSource::Rtc, *rtc.time, false);
    }
    if (ops.queryNtp) {
        if (const auto got = ops.queryNtp(1500)) {
            if (wallTimePlausible(*got))
                return stepTo(WallClockSource::Sntp, *got, rtcPresent);
        }
    }
    if (ops.queryHttps) {
        if (const auto got = ops.queryHttps(2000)) {
            if (wallTimePlausible(*got))
                return stepTo(WallClockSource::HttpsDate, *got, rtcPresent);
        }
    }
    if (err)
        *err = "wall clock is implausible and NTP did not answer";
    return WallClockStatus::Unavailable;
}

// NTP delta (seconds) between the NTP epoch (1900) and Unix (1970).
inline constexpr uint32_t kNtpUnixDelta = 2208988800u;

// Accept one SNTP reply. The 8-byte `origin` is the transmit timestamp this
// client put in the request; a server copies it to the originate field.
// Rejects a short packet, mode other than 4 (server), LI==3 (unsynchronised),
// stratum 0 (kiss-o-death) or >15, an originate timestamp that does not echo
// `origin`, and a transmit time outside 2020-2100.
// MUT_SNTP_SKIP_CHECKS drops the mode, stratum, LI and origin checks.
inline std::optional<std::time_t> sntpAcceptReply(const unsigned char* rsp, std::size_t n,
                                                  const unsigned char origin[8]) {
    if (!rsp || n < 48)
        return std::nullopt;
#if !defined(MUT_SNTP_SKIP_CHECKS)
    if (!origin)
        return std::nullopt;
    const unsigned li = rsp[0] >> 6;
    const unsigned mode = rsp[0] & 7u;
    const unsigned stratum = rsp[1];
    if (mode != 4 || li == 3 || stratum == 0 || stratum > 15)
        return std::nullopt;
    for (int i = 0; i < 8; ++i)
        if (rsp[24 + i] != origin[i])
            return std::nullopt;
#else
    (void)origin;
#endif
    const uint32_t sec = (uint32_t(rsp[40]) << 24) | (uint32_t(rsp[41]) << 16) | (uint32_t(rsp[42]) << 8) |
                         uint32_t(rsp[43]);
    if (sec < kNtpUnixDelta)
        return std::nullopt;
    const std::int64_t unix = static_cast<std::int64_t>(sec) - static_cast<std::int64_t>(kNtpUnixDelta);
    if (unix < kPlausibleWallEpoch || unix >= kPlausibleWallCeiling)
        return std::nullopt;
    if (unix > static_cast<std::int64_t>(std::numeric_limits<std::time_t>::max()))
        return std::nullopt;
    const std::time_t t = static_cast<std::time_t>(unix);
    if (!wallTimePlausible(t))
        return std::nullopt;
    return t;
}

// One-shot rescue. Production does not use this at boot: the daemon starts
// startWallClockRescue(), which retries until the clock is plausible.
WallClockStatus ensurePlausibleWallClockDefault(std::string* err, bool force = false,
                                                WallClockSource* source = nullptr);

// Background rescue. Each attempt: if the clock is already plausible, stop
// (an external ntpd/time source fixed it). Otherwise read rtc0 immediately,
// and only if the network is ready (default route or a resolvable host) try
// SNTP then HTTPS Date. Sleep 2s, 4s, 8s, … capped at 30s, and repeat.
// `monoMs` is optional; tests advance it from sleepMs. `stop` ends the loop.
struct WallClockRescueOps {
    std::function<std::time_t()> now;
    std::function<RtcSample()> readRtc;
    std::function<bool(std::time_t)> writeRtc;
    std::function<bool()> networkReady;
    std::function<std::optional<std::time_t>(int timeoutMs)> queryNtp;
    std::function<std::optional<std::time_t>(int timeoutMs)> queryHttps;
    std::function<bool(std::time_t)> step;
    std::function<void(int ms)> sleepMs;
    std::function<void(const char* line)> log;
    std::function<bool()> stop;
};

struct WallClockRescueResult {
    WallClockStatus status = WallClockStatus::Unavailable;
    WallClockSource source = WallClockSource::None;
    int attempts = 0;
};

// failedAttempts is 1-based: 1 -> 2000, 2 -> 4000, 3 -> 8000, then cap 30000.
inline int wallClockRescueBackoffMs(int failedAttempts) {
    int ms = 2000;
    for (int i = 1; i < failedAttempts; ++i) {
        if (ms >= 15000)
            return 30000;
        ms *= 2;
    }
    return ms > 30000 ? 30000 : ms;
}

inline WallClockRescueResult runWallClockRescue(const WallClockRescueOps& ops) {
    WallClockRescueResult out;
    int attempt = 0;
    while (!ops.stop || !ops.stop()) {
        ++attempt;
        if (attempt > 10000) {
            out.status = WallClockStatus::Unavailable;
            return out;
        }
        out.attempts = attempt;
        const std::time_t now = ops.now ? ops.now() : 0;
#ifndef MUT_WALL_RESCUE_IGNORE_SANE
        if (wallTimePlausible(now)) {
            if (ops.log) {
                ops.log(attempt == 1 ? "wall clock already plausible; rescue stopped"
                                     : "wall clock became plausible from an external time source; rescue stopped");
            }
            out.status = WallClockStatus::AlreadySane;
            out.source = WallClockSource::None;
            return out;
        }
#else
        (void)now;
#endif
        if (ops.log) {
            char line[96];
            std::snprintf(line, sizeof line, "wall clock rescue attempt %d", attempt);
            ops.log(line);
        }
        bool rtcPresent = false;
        if (ops.readRtc) {
            const RtcSample rtc = ops.readRtc();
            rtcPresent = rtc.present;
            if (ops.log) {
                if (!rtc.present)
                    ops.log("wall clock rescue: rtc0 absent");
                else if (!rtc.time || !wallTimePlausible(*rtc.time))
                    ops.log("wall clock rescue: rtc0 present but not a plausible time");
                else
                    ops.log("wall clock rescue: rtc0 time plausible");
            }
            if (rtc.time && wallTimePlausible(*rtc.time) && ops.step && ops.step(*rtc.time) &&
                (!ops.now || wallTimePlausible(ops.now()))) {
                if (ops.log)
                    ops.log("wall clock stepped from RTC");
                out.status = WallClockStatus::Stepped;
                out.source = WallClockSource::Rtc;
                return out;
            }
        }
        const bool net = ops.networkReady && ops.networkReady();
        if (!net) {
            if (ops.log)
                ops.log("wall clock rescue: network not ready; skipping SNTP and HTTPS Date");
        } else {
            if (ops.queryNtp) {
                if (ops.log)
                    ops.log("wall clock rescue: SNTP");
                if (const auto got = ops.queryNtp(1500)) {
                    if (wallTimePlausible(*got) && ops.step && ops.step(*got) &&
                        (!ops.now || wallTimePlausible(ops.now()))) {
                        if (rtcPresent && ops.writeRtc)
                            ops.writeRtc(*got);
                        if (ops.log)
                            ops.log("wall clock stepped from SNTP");
                        out.status = WallClockStatus::Stepped;
                        out.source = WallClockSource::Sntp;
                        return out;
                    }
                }
                if (ops.log)
                    ops.log("wall clock rescue: SNTP did not answer");
            }
            if (ops.queryHttps) {
                if (ops.log)
                    ops.log("wall clock rescue: HTTPS Date");
                if (const auto got = ops.queryHttps(2000)) {
                    if (wallTimePlausible(*got) && ops.step && ops.step(*got) &&
                        (!ops.now || wallTimePlausible(ops.now()))) {
                        if (rtcPresent && ops.writeRtc)
                            ops.writeRtc(*got);
                        if (ops.log)
                            ops.log("wall clock stepped from HTTPS-Date");
                        out.status = WallClockStatus::Stepped;
                        out.source = WallClockSource::HttpsDate;
                        return out;
                    }
                }
                if (ops.log)
                    ops.log("wall clock rescue: HTTPS Date did not answer");
            }
        }
        const int wait = wallClockRescueBackoffMs(attempt);
        if (ops.log) {
            char line[96];
            std::snprintf(line, sizeof line, "wall clock rescue: retry in %d ms", wait);
            ops.log(line);
        }
        if (ops.sleepMs)
            ops.sleepMs(wait);
    }
    if (ops.log)
        ops.log("wall clock rescue stopped");
    out.status = WallClockStatus::Unavailable;
    return out;
}

// How long YouTube / TLS waits on the background rescue instead of running
// its own one-shot. The rescue thread keeps retrying after this returns.
inline constexpr int kWallClockRescueWaitMs = 5000;

// One process-wide "clock stepped (source)" line. Later calls are silent.
inline bool logClockSteppedOnce(const char* source) {
    static std::atomic<int> once{0};
    if (once.exchange(1) != 0)
        return false;
    std::fprintf(stderr, "misterplexd: clock stepped (%s)\n", source && source[0] ? source : "unknown");
    return true;
}

// True the first time and then at most once per intervalMs (default 5 minutes).
inline bool offlineClockLogDue(long long nowMs, long long& lastMs, long long intervalMs = 300000) {
    if (intervalMs < 0)
        intervalMs = 0;
    if (lastMs != 0 && nowMs - lastMs < intervalMs)
        return false;
    lastMs = nowMs;
    return true;
}

inline bool offlineClockLogAllowed(long long intervalMs = 300000) {
    static std::mutex mu;
    static long long lastMs = 0;
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    std::lock_guard<std::mutex> lock(mu);
    return offlineClockLogDue(nowMs, lastMs, intervalMs);
}

// Joinable rescue. sleepMs waits on waitCv_ so stopAndJoin returns during the
// backoff instead of after it. The worker is never detached.
class WallClockRescueThread {
public:
    std::function<void(const WallClockRescueResult&)> onDone;

    void start(WallClockRescueOps ops) {
        std::lock_guard<std::mutex> gate(lifeMu_);
        if (worker_.joinable())
            return;
        stop_.store(false);
        finished_.store(false);
        if (!ops.stop)
            ops.stop = [this] { return stop_.load(); };
        if (!ops.sleepMs) {
            ops.sleepMs = [this](int ms) {
                std::unique_lock<std::mutex> lk(waitMu_);
                waitCv_.wait_for(lk, std::chrono::milliseconds(ms > 0 ? ms : 0),
                                 [this] { return stop_.load(); });
            };
        }
        auto done = onDone;
        worker_ = std::thread([this, ops = std::move(ops), done = std::move(done)] {
            const WallClockRescueResult r = runWallClockRescue(ops);
            if (done)
                done(r);
            finished_.store(true);
            waitCv_.notify_all();
        });
    }

    void stopAndJoin() {
        stop_.store(true);
        waitCv_.notify_all();
        std::thread t;
        {
            std::lock_guard<std::mutex> gate(lifeMu_);
            if (worker_.joinable())
                t = std::move(worker_);
        }
        if (t.joinable())
            t.join();
    }

    bool finished() const { return finished_.load(); }

private:
    std::mutex lifeMu_;
    std::mutex waitMu_;
    std::condition_variable waitCv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};
    std::thread worker_;
};

// Start the rescue thread once. Safe to call again. Does not block startup.
void startWallClockRescue();
// Wake the backoff and join. Call this on the INT/TERM shutdown path.
void stopWallClockRescue();
// Block up to waitMs for the rescue thread (or any other time source) to make
// the clock plausible. Does not start a second rescue. TlsTimeCheck::Skip must
// not call this: the HTTPS Date fetch would deadlock on itself.
WallClockStatus waitForWallClockRescue(int waitMs, std::string* err = nullptr,
                                       WallClockSource* source = nullptr);

// ioctl RTC_RD_TIME / RTC_SET_TIME on path (normally /dev/rtc0), UTC, matching
// `hwclock -u` / `hwclock -wu`. Missing node: present=false. Opened but
// unreadable (a chip that was never set): present=true, time=nullopt.
//
// MiSTer sources for this order:
//   Wiki RTC-board and MkDocs advanced/rtc: optional LTC (or USB-hub) chip.
//   `hwclock -wu` stores the system clock, `hwclock -u` reads it. Most units
//   have no board; Ethernet time is the usual source.
//   Scripts_MiSTer/rtc.sh runs ntpdate, then `hwclock -wu` (write-back after
//   network time, UTC). A failed write is logged and does not undo the sync.
//   Main_MiSTer user_io.cpp send_rtc() does not open /dev/rtc0. It reads
//   time(NULL) and pushes BCD local time (UIO_RTC 0x22) and a Unix timestamp
//   (UIO_TIMESTAMP 0x24) to the core about once a minute. The HPS clock is
//   what cores see, so stepping it is the useful half. Write-back uses the
//   same UTC RTC_SET_TIME that `hwclock -wu` does, and only after SNTP or
//   HTTPS — never when the time we just applied came from the chip.
//   A fresh m41t80 answers RTC_RD_TIME with EINVAL until a valid time is
//   stored (misterfpga.org topic 1880). That is present + no time, then a
//   write-back once SNTP or HTTPS has a plausible clock.
RtcSample readHardwareRtc(const char* path);
bool writeHardwareRtc(const char* path, std::time_t t);

// One HTTPS GET that verifies the chain and the hostname. skipTimeCheck sets
// X509_V_FLAG_NO_CHECK_TIME and nothing else. This does not consult the wall
// clock rescue. Used by the Date fallback and by the 1970 screen-id proof.
struct HttpsDateTarget {
    std::string verifyHost;
    std::string connectHost;
    int port = 443;
    std::string caFile;
    bool skipTimeCheck = true;
    std::string path = "/";
};

struct TlsHttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
    std::optional<std::time_t> date;
};

TlsHttpResult tlsHttpExchange(const HttpsDateTarget& target, int timeoutMs);
std::optional<std::time_t> httpsDateFromTargets(const HttpsDateTarget* targets, std::size_t n,
                                               int timeoutMs);

} // namespace misterplex
