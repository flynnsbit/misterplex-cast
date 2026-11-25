#pragma once
// Cast targets (Plex GDM, YouTube SSDP/DIAL/Lounge, AirPlay mDNS) are
// advertised only while a MiSTerPlex core is loaded. A cast never loads a
// core: one that still arrives with no MiSTerPlex core up is refused.
//
// Advertise as soon as the core is seen. Withdraw only after "not loaded" has
// lasted debounceMs, and never while one of the daemon's own core switches
// (menu bounce marker, in-play menu reload, misterplex_mode.sh lock) is in
// flight, so an in-play core change through the Menu core does not flap the
// targets. That hold is capped at holdCapMs so a stuck marker cannot keep the
// targets up forever.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {

// Plex, PLEX, PlexOB, PLEXOB (any case).
inline bool castCoreNameIsOurs(const std::string& name) {
    std::string lower;
    for (char c : name)
        lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return lower == "plex" || lower == "plexob";
}

inline bool castCoreLoaded(const std::string& coreName, bool gateOpen) {
    return gateOpen || castCoreNameIsOurs(coreName);
}

struct CastAdvertiseConfig {
    bool enabled = true;
    int64_t pollMs = 1000;
    int64_t debounceMs = 8000;
    int64_t holdCapMs = 120000;
};

inline int64_t castAdvertiseEnvMs(const char* name, int64_t fallback, int64_t floor) {
    const char* v = std::getenv(name);
    if (!v || !v[0])
        return fallback;
    char* end = nullptr;
    const long long n = std::strtoll(v, &end, 10);
    if (end == v || *end)
        return fallback;
    return n < floor ? floor : static_cast<int64_t>(n);
}

// MPX_ADVERTISE_GATE=0 advertises always (the behaviour before the gate).
inline CastAdvertiseConfig castAdvertiseConfigFromEnv() {
    CastAdvertiseConfig c;
    const char* gate = std::getenv("MPX_ADVERTISE_GATE");
    c.enabled = !(gate && std::string(gate) == "0");
    c.pollMs = castAdvertiseEnvMs("MPX_ADVERTISE_POLL_MS", 1000, 100);
    c.debounceMs = castAdvertiseEnvMs("MPX_ADVERTISE_DEBOUNCE_MS", 8000, 5000);
    return c;
}

inline std::string castAdvertiseConfigLine(const CastAdvertiseConfig& c) {
    return std::string("cast advertise gate: ") + (c.enabled ? "on" : "off") + " poll=" + std::to_string(c.pollMs) +
           " debounce=" + std::to_string(c.debounceMs);
}

struct CastAdvertiseGate {
    CastAdvertiseConfig cfg;
    bool advertised = false;
    int64_t notLoadedSinceMs = -1;   // first not-loaded sample while advertised
    int64_t debounceFromMs = -1;     // restarts when an own-switch hold ends
    int64_t holdSinceMs = -1;        // first sample of the current own-switch hold

    bool update(const std::string& coreName, bool gateOpen, bool ownSwitchInFlight, int64_t nowMs) {
        if (!cfg.enabled)
            return advertised = true;
#ifdef MUT_ADVERTISE_WITHOUT_CORE
        (void)coreName;
        (void)gateOpen;
        const bool loaded = true;
#else
        const bool loaded = castCoreLoaded(coreName, gateOpen);
#endif
        if (loaded) {
            notLoadedSinceMs = debounceFromMs = holdSinceMs = -1;
            return advertised = true;
        }
        if (!advertised)
            return false;
        if (notLoadedSinceMs < 0)
            notLoadedSinceMs = debounceFromMs = nowMs;
#ifndef MUT_NO_SWITCH_HOLD
        if (ownSwitchInFlight) {
            if (holdSinceMs < 0)
                holdSinceMs = nowMs;
#ifndef MUT_NO_HOLD_CAP
            if (nowMs - holdSinceMs < cfg.holdCapMs)
#endif
            {
                debounceFromMs = nowMs;
                return true;
            }
        } else {
            holdSinceMs = -1;
        }
#else
        (void)ownSwitchInFlight;
#endif
#ifdef MUT_NO_WITHDRAW_ON_UNLOAD
        return true;
#else
#ifdef MUT_NO_DEBOUNCE
        const bool due = true;
#else
        const bool due = nowMs - debounceFromMs >= cfg.debounceMs;
#endif
        if (due)
            advertised = false;
        return advertised;
#endif
    }

    int64_t notLoadedForMs(int64_t nowMs) const {
        return notLoadedSinceMs < 0 ? 0 : nowMs - notLoadedSinceMs;
    }
};

// In-play menu reloads the daemon issues itself (issueMenuReloadForPair,
// tryMenuReloadForTargetBuild, reloadCoreForYcMode) hold one of these.
inline std::atomic<int>& ownCoreSwitchCount() {
    static std::atomic<int> n{0};
    return n;
}
struct OwnCoreSwitchScope {
    OwnCoreSwitchScope() { ownCoreSwitchCount().fetch_add(1); }
    ~OwnCoreSwitchScope() { ownCoreSwitchCount().fetch_sub(1); }
    OwnCoreSwitchScope(const OwnCoreSwitchScope&) = delete;
    OwnCoreSwitchScope& operator=(const OwnCoreSwitchScope&) = delete;
};

// misterplex_mode.sh holds the lock exclusively for the whole switch. The probe
// never creates the file.
inline bool modeSwitchLockHeld(const char* lockPath) {
    if (!lockPath || !lockPath[0])
        return false;
    const int fd = ::open(lockPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool held = ::flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    if (!held)
        ::flock(fd, LOCK_UN);
    ::close(fd);
    return held;
}

inline bool ownCoreSwitchInFlight(const char* markerPath, const char* lockPath) {
    struct stat st {};
    if (markerPath && markerPath[0] && ::stat(markerPath, &st) == 0)
        return true;
    return ownCoreSwitchCount().load() > 0 || modeSwitchLockHeld(lockPath);
}

// What a cast entry point sees. writeCmd is the MiSTer_cmd writer: a refusal
// never calls it (the daemon leaves it empty; tests pass a recorder).
struct CastAdmitIo {
    std::string coreName;
    bool gateOpen = false;
    // One of the daemon's own in-play reloads is running: the core is between
    // two of ours, so the cast waits for it like any play does.
    bool ownSwitchInFlight = false;
    std::function<void(const std::string&)> log;
    std::function<bool(const std::string&)> writeCmd;
};

// True: play the cast. False: refused and logged; the caller answers the
// sender with its usual failure and starts nothing.
inline bool castAdmitted(const char* kind, const CastAdmitIo& io) {
    if (castCoreLoaded(io.coreName, io.gateOpen) || io.ownSwitchInFlight)
        return true;
    if (io.log)
        io.log(std::string("cast refused: MiSTerPlex core not loaded (") + (kind ? kind : "?") +
               ", core '" + io.coreName + "')");
#ifdef MUT_CAST_LOADS_CORE
    if (io.writeCmd)
        io.writeCmd("load_core /media/fat/_Utility/MiSTerPlex.rbf");
    return true;
#else
    return false;
#endif
}

struct CastAdvertiseSample {
    std::string coreName;
    bool gateOpen = false;
    bool ownSwitchInFlight = false;
};

// Polls sample() every pollMs and calls apply(on) for each target change. The
// first tick always applies, so every target starts in the gate's state.
class CastAdvertiseWatcher {
public:
    using SampleFn = std::function<CastAdvertiseSample()>;
    using ApplyFn = std::function<void(bool)>;
    using LogFn = std::function<void(const std::string&)>;

    CastAdvertiseWatcher(const CastAdvertiseConfig& cfg, SampleFn sample, ApplyFn apply, LogFn log)
        : sample_(std::move(sample)), apply_(std::move(apply)), log_(std::move(log)) {
        gate_.cfg = cfg;
    }
    ~CastAdvertiseWatcher() { stop(); }
    CastAdvertiseWatcher(const CastAdvertiseWatcher&) = delete;
    CastAdvertiseWatcher& operator=(const CastAdvertiseWatcher&) = delete;

    bool tick(int64_t nowMs) {
        const CastAdvertiseSample s = sample_ ? sample_() : CastAdvertiseSample{};
        const bool on = gate_.update(s.coreName, s.gateOpen, s.ownSwitchInFlight, nowMs);
        const bool first = !applied_;
        if (first || on != advertised_.load()) {
            applied_ = true;
            advertised_.store(on);
            if (log_ && (on || !first))
                log_(on ? "cast targets: advertise (core '" + s.coreName + "')"
                        : "cast targets: withdraw (core '" + s.coreName + "' for " +
                              std::to_string(gate_.notLoadedForMs(nowMs)) + " ms)");
            if (apply_)
                apply_(on);
        }
        return on;
    }

    // The first tick runs on the caller's thread: the targets hold the gate's
    // state before start() returns.
    void start() {
        tick(steadyMs());
        std::lock_guard<std::mutex> lk(mu_);
        if (thr_.joinable())
            return;
        stop_ = false;
        thr_ = std::thread([this] { run(); });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thr_.joinable())
            thr_.join();
    }
    bool advertised() const { return advertised_.load(); }

    static int64_t steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!stop_) {
            cv_.wait_for(lk, std::chrono::milliseconds(gate_.cfg.pollMs), [this] { return stop_; });
            if (stop_)
                break;
            lk.unlock();
            tick(steadyMs());
            lk.lock();
        }
    }

    CastAdvertiseGate gate_;
    SampleFn sample_;
    ApplyFn apply_;
    LogFn log_;
    bool applied_ = false;
    std::atomic<bool> advertised_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thr_;
};

}  // namespace misterplex
