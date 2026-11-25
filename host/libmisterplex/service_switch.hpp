#pragma once

#include <fstream>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "analog_raster.hpp"
#include "fpga_build_ids.hpp"
#include "status_telemetry.hpp"

namespace misterplex::service {
inline constexpr unsigned kWord = 87;
inline constexpr uint64_t kSelection = 1ull << 10;
inline constexpr uint64_t kCapability = 1ull << 11;
inline constexpr const char* kStatePath = "/tmp/misterplex-service-switch.state";
inline constexpr const char* kReadyPath = "/tmp/misterplex-service.ready";
inline constexpr const char* kRequestDirDefault = "/run/misterplex";
inline constexpr const char* kRequestBasename = "misterplex-service.request";
inline constexpr const char* kModeSwitchLockPath = "/tmp/misterplex-mode-switch.lock";
inline constexpr const char* kRequestPath = "/run/misterplex/misterplex-service.request";

inline std::string serviceRequestDir() {
    const char* env = std::getenv("MPX_SERVICE_REQUEST_DIR");
    if (env && env[0] == '/')
        return env;
    return kRequestDirDefault;
}
inline std::string serviceRequestPath() {
    return serviceRequestDir() + "/" + kRequestBasename;
}
inline bool ensureServiceRequestDir(const std::string& dir = serviceRequestDir()) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    return ::mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
}
inline int64_t& serviceRequestProcessStartNsStorage() {
    static int64_t ns = 0;
    return ns;
}
// Recorded at daemon main() before discard/consume: requests older than this are stale
// leftovers (e.g. when mode.sh still holds the mode-switch flock at startup).
inline void recordServiceRequestProcessStart() {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;
    serviceRequestProcessStartNsStorage() =
        static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
}
inline int64_t serviceRequestProcessStartNs() { return serviceRequestProcessStartNsStorage(); }
inline const char* modeSwitchLockPath() {
    const char* env = std::getenv("MPX_MODE_SWITCH_LOCK_PATH");
    if (env && env[0] == '/')
        return env;
    return kModeSwitchLockPath;
}
inline int openModeSwitchLock() {
    return ::open(modeSwitchLockPath(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
}
inline int64_t statMtimeNs(const struct stat& st) {
#if defined(st_mtim)
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
           static_cast<int64_t>(st.st_mtim.tv_nsec);
#else
    return static_cast<int64_t>(st.st_mtime) * 1000000000LL;
#endif
}
inline bool requestMtimeBeforeProcessStart(const struct stat& st) {
    const int64_t start = serviceRequestProcessStartNs();
    if (start <= 0)
        return false;
    // Second resolution avoids rejecting a request written in the same wall second as
    // recordServiceRequestProcessStart() on coarse mtime (tmpfs, SD).
    const int64_t fileSec = statMtimeNs(st) / 1000000000LL;
    const int64_t startSec = start / 1000000000LL;
    return fileSec < startSec;
}
inline bool tryModeSwitchLockForDiscard(int lockFd, int maxWaitMs = 200) {
#ifdef MUT_SERVICE_DISCARD_BLOCKING
    (void)maxWaitMs;
    return ::flock(lockFd, LOCK_EX) == 0;
#else
    const int stepMs = 20;
    for (int waited = 0; waited <= maxWaitMs; waited += stepMs) {
        if (::flock(lockFd, LOCK_EX | LOCK_NB) == 0)
            return true;
        if (errno != EWOULDBLOCK && errno != EAGAIN)
            return false;
        if (waited >= maxWaitMs)
            return false;
        struct timespec ts = {0, static_cast<long>(stepMs) * 1000000L};
        (void)::nanosleep(&ts, nullptr);
    }
    return false;
#endif
}
inline bool validRequestFileFd(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    return st.st_uid == ::geteuid();
}
inline void unlinkRequestIfPresent(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        ::unlink(path.c_str());
        return;
    }
    if (validRequestFileFd(fd))
        ::unlink(path.c_str());
    ::close(fd);
}
inline void discardServiceRequestAtStartup(const std::string& path = std::string()) {
    const std::string target = path.empty() ? serviceRequestPath() : path;
    (void)ensureServiceRequestDir();
    const int lockFd = openModeSwitchLock();
    if (lockFd < 0)
        return;
    // Never block on the mode-switch flock: mode.sh holds it across boot start
    // until service ready (up to ~60 s). If busy, skip unlink; consume ignores
    // pre-start mtimes instead.
    if (tryModeSwitchLockForDiscard(lockFd)) {
        unlinkRequestIfPresent(target);
        ::flock(lockFd, LOCK_UN);
    }
    ::close(lockFd);
}
inline std::optional<std::string> consumeRequest(const std::string& path = std::string()) {
    const std::string target = path.empty() ? serviceRequestPath() : path;
    const int fd = ::open(target.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !validRequestFileFd(fd))
        return ::close(fd), std::nullopt;
    if (requestMtimeBeforeProcessStart(st))
        return ::close(fd), std::nullopt;
    ::close(fd);
    const std::string scratch =
        target + ".consume-" + std::to_string(static_cast<unsigned long>(::getpid()));
    if (::rename(target.c_str(), scratch.c_str()) != 0)
        return std::nullopt;
    std::ifstream in(scratch);
    std::string mode;
    if (!(in >> mode) || (mode != "Plex" && mode != "Spelunkr"))
        mode.clear();
    ::unlink(scratch.c_str());
    if (mode.empty())
        return std::nullopt;
    return mode;
}
inline bool requestNeedsRingSnapshot(bool targetSpelunkr) {
#ifdef MUT_SERVICE_REQUEST_SKIP_SNAPSHOT
    (void)targetSpelunkr;
    return false;
#else
    return !targetSpelunkr;
#endif
}
struct ServiceRequestPolicy {
    enum class Action { Ignore, Noop, RejectBusy, Begin };
    static Action classify(const std::optional<std::string>& request, bool activeSpelunkr, bool busy) {
#ifdef MUT_SERVICE_REQUEST_IGNORE
        (void)request;
        (void)activeSpelunkr;
        (void)busy;
        return Action::Ignore;
#endif
        if (!request)
            return Action::Ignore;
        if (busy)
            return Action::RejectBusy;
        const bool wantSpelunkr = *request == "Spelunkr";
        return wantSpelunkr == activeSpelunkr ? Action::Noop : Action::Begin;
    }
};

// 683F: what the daemon does for an external --request-* once classify() has decided.
// B1: with the core gate open a Begin must launch the real OSD service switch
// (serviceRequested_ -> serviceSwitch_); with the gate closed there is no OSD to
// switch from, so it is a CFG-snapshot-only request. N1: every external response
// (Begin snapshot-only and Noop) publishes "pending" first and holds it for at
// least kExternalReadyMinHoldUs before "ready", so mode.sh (polls every 250 ms,
// requires an observed pending) always sees it. When the OSD switch is launched the
// daemon does not publish ready itself; the switch child does.
inline constexpr int64_t kModeShPollUs = 250000;
#ifdef MUT_SERVICE_EXTERNAL_READY_NO_HOLD
// Mutant == d834f744: pending and ready are written in the same serviceTick, no gap.
inline constexpr int64_t kExternalReadyMinHoldUs = 0;
inline constexpr bool kExternalReadySameTick = true;
#else
inline constexpr int64_t kExternalReadyMinHoldUs = 3 * kModeShPollUs;
inline constexpr bool kExternalReadySameTick = false;
#endif
inline bool externalReadyDue(int64_t nowUs, int64_t pendingPublishedUs) {
    return nowUs - pendingPublishedUs >= kExternalReadyMinHoldUs;
}
struct ExternalRequestPlan {
    bool launchOsdSwitch;   // set serviceRequested_ so the OSD notice + serviceSwitch_ run
    bool deferredReady;     // daemon publishes ready itself after the pending hold
};
inline ExternalRequestPlan planExternalRequest(ServiceRequestPolicy::Action action, bool gateOpen) {
    switch (action) {
    case ServiceRequestPolicy::Action::Noop:
        return {false, true};
    case ServiceRequestPolicy::Action::Begin:
#ifdef MUT_SERVICE_EXTERNAL_BEGIN_NO_LAUNCH
        (void)gateOpen;
        return {false, true};
#else
        return {gateOpen, !gateOpen};
#endif
    default:
        return {false, false};
    }
}

inline std::string name(bool spelunkr) { return spelunkr ? "Spelunkr" : "Plex"; }
inline std::string label(bool spelunkr) { return spelunkr ? "Live TV" : "Plex"; }
inline constexpr const char* kGuideTitle = "STREAMING GUIDE";
inline std::string notice(bool spelunkr) {
    return spelunkr ? "SWITCHING TO LIVE TV" : "SWITCHING TO PLEX";
}
inline std::string switchFailed(bool spelunkr) {
    return spelunkr ? "LIVE TV SWITCH FAILED" : "PLEX SWITCH FAILED";
}
// GH#649: keep the switching card on glass long enough for one overlay flip,
// but do not wait the 5 s fallback once the notice has been composed.
inline constexpr int64_t kServiceNoticeMinHoldUs = 300000;
inline constexpr int64_t kServiceNoticeDeadlineUs = 5000000;
inline bool serviceNoticeLaunchReady(bool shown, int64_t nowUs, int64_t readyUs,
                                     int64_t deadlineUs) {
    return (shown && nowUs >= readyUs) || nowUs >= deadlineUs;
}
// 683G N2: "ready <mode> snapshot-only" = external cross-mode Begin with the core gate closed: only the CFG
// snapshot was taken, no OSD switch ran (mode.sh --request-* must fail, nothing switched).
inline constexpr const char* kSnapshotOnlyDetail = "snapshot-only";
struct State {
    std::string phase, target, detail;
    bool pending() const { return phase == "pending"; }
    std::string overlay() const {
        if (pending())
            return notice(target == "Spelunkr");
        if (phase == "error")
            return (target == "Spelunkr" || target == "Plex") ? switchFailed(target == "Spelunkr")
                                                              : "SERVICE SWITCH STATE FAILED";
        return {};
    }
};
inline State readState(const std::string& path = kStatePath) {
    State s;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return errno == ENOENT ? s : State{"error", ""};
    std::ifstream in(path);
    if (!(in >> s.phase >> s.target) ||
        (s.phase != "pending" && s.phase != "ready" && s.phase != "error") ||
        (s.target != "Plex" && s.target != "Spelunkr"))
        return {"error", ""};
    return s;
}
inline bool writeState(const State& s, const std::string& path = kStatePath) {
    return plexob::writeFileAtomic(path, s.phase + " " + s.target + (s.detail.empty() ? "" : " " + s.detail) + "\n");
}
class FailureRecovery {
public:
    bool tick(bool failed, bool independentlyReady, int64_t nowUs) {
        if (!failed) {
            sinceUs_ = -1;
            return false;
        }
        if (sinceUs_ < 0)
            sinceUs_ = nowUs;
        return independentlyReady && nowUs - sinceUs_ >= 5000000;
    }
private:
    int64_t sinceUs_ = -1;
};
class PlexCfg {
public:
    bool observe(const std::string& path, bool spelunkr, bool selected, bool busy) {
        if (spelunkr)
            return restore(path);
        if (busy || selected)
            return true;
        bool big = false;
        std::string bytes;
        if (!plexob::readSmallFile(path, bytes, big) || bytes.size() < 4) {
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0 && errno == ENOENT)
                return true;
            return false;
        }
        cached_ = std::move(bytes);
        return true;
    }
    static void decodeLegacyValues(const std::string& img, unsigned values[7]) {
        uint32_t word = 0;
        const size_t n = img.size() < 4 ? img.size() : 4u;
        for (size_t i = 0; i < n; ++i)
            word |= static_cast<uint32_t>(static_cast<unsigned char>(img[i])) << (i * 8);
        const unsigned shifts[] = {1, 3, 5, 7, 8, 11, 15};
        for (unsigned i = 0; i < 7; ++i)
            values[i] = (word >> shifts[i]) & (i == 3 ? 1u : 3u);
    }
    static bool patchLegacyCfgOptions(std::string& img, const unsigned values[7]) {
        if (img.size() < 4)
            return false;
        uint32_t word = 0;
        for (unsigned i = 0; i < 4; ++i)
            word |= static_cast<uint32_t>(static_cast<unsigned char>(img[i])) << (i * 8);
        const unsigned shifts[] = {1, 3, 5, 7, 8, 11, 15};
        for (unsigned i = 0; i < 7; ++i) {
            const unsigned mask = (i == 3 ? 1u : 3u);
            word &= ~(mask << shifts[i]);
            word |= (values[i] & mask) << shifts[i];
        }
        for (unsigned i = 0; i < 4; ++i)
            img[i] = static_cast<char>((word >> (i * 8)) & 0xff);
        return true;
    }
    // ringMenu: live core OSD (h264ring status word 27). When set, .service-save is built from
    // it — PlexOB_v1.CFG on disk is not authoritative (Main often does not flush menu changes).
    bool save(const std::string& path, const unsigned ringMenu[7] = nullptr) const {
#ifdef MUT_PLEX_CFG_CACHE_ONLY_SAVE
        return cached_.size() >= 4 && plexob::writeFileAtomic(path + ".service-save", cached_);
#endif
#ifdef MUT_PLEX_CFG_SAVE_STALE_CACHE
        if (cached_.size() >= 4)
            return plexob::writeFileAtomic(path + ".service-save", cached_);
#endif
        if (ringMenu) {
#ifndef MUT_PLEX_CFG_SKIP_RING_PATCH
            bool big = false;
            std::string bytes;
            if (plexob::readSmallFile(path, bytes, big) && !bytes.empty()) {
                if (bytes.size() < 16)
                    bytes.resize(16, '\0');
            } else
                bytes.assign(16, '\0');
            unsigned snap[7]{};
            for (unsigned i = 0; i < 7; ++i)
                snap[i] = ringMenu[i];
            patchLegacyCfgOptions(bytes, snap);
            return plexob::writeFileAtomic(path + ".service-save", bytes);
#endif
        }
        bool big = false;
        std::string bytes;
        if (plexob::readSmallFile(path, bytes, big) && bytes.size() >= 4)
            return plexob::writeFileAtomic(path + ".service-save", bytes);
        return cached_.size() >= 4 && plexob::writeFileAtomic(path + ".service-save", cached_);
    }
    static bool restore(const std::string& path) {
        bool big = false;
        std::string saved, current;
        if (!plexob::readSmallFile(path + ".service-save", saved, big) || saved.size() < 4)
            return false;
        if (!plexob::readSmallFile(path, current, big))
            return false;
        return current == saved || plexob::writeFileAtomic(path, saved);
    }
    static bool settings(const std::string& path, unsigned values[7], uint32_t buildId = 0) {
        bool big = false;
        std::string bytes;
        const std::string saved = path + ".service-save";
        struct stat st {};
        if (::stat(saved.c_str(), &st) != 0) {
            if (errno != ENOENT || !plexob::readSmallFile(path, bytes, big))
                return false;
        } else if (!plexob::readSmallFile(saved, bytes, big)) {
            return false;
        }
        if (bytes.size() < 4)
            return false;
        uint32_t word = 0;
        for (unsigned i = 0; i < 4; ++i)
            word |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
        const unsigned shifts[] = {1, 3, 5, 7, 8, 11, 15};
        for (unsigned i = 0; i < 7; ++i)
            values[i] = (word >> shifts[i]) & (i == 3 ? 1u : 3u);
#ifndef MUT_PLEX_CFG_IGNORE_V3_ASPECT
        if (fpgaCfgStatusUsesV3Layout(buildId) &&
            bytes.size() > status_telemetry::kOsdFlagsByte) {
            uint8_t raw[16]{};
            const size_t n = bytes.size() < sizeof(raw) ? bytes.size() : sizeof(raw);
            std::memcpy(raw, bytes.data(), n);
            const auto v3 = status_telemetry::decodeOsdStatusV3(raw);
            if (v3.aspect != 0)
                values[0] = static_cast<unsigned>(v3.aspect);
        }
#endif
        return true;
    }
private:
    std::string cached_;
};
// A saved menu bit is not a request. First synchronize it with this process's
// actual mode, and wait for the ring echo before accepting deliberate changes.
// 683H N1: "Service menu sync deferred rc=-19" (glass 45bda5d5). rc=-19 is -ENODEV from OwenbPlayer::syncService:
// the daemon has no OwenB ring attached yet (a freshly exec'd Live TV daemon before its first play session),
// so the Service word-38 sync cannot be sent. It is benign: Selection::tick retries every serviceTick until
// syncService returns 0 (the HDMI OSD Service line did update). The only defect was the log: one line per tick
// (31 lines in one switch window) with a bare number. SyncDeferLog logs the first deferral of a streak (with
// the meaning of ENODEV), skips repeats of the same rc, and logs once when the sync finally succeeds.
// MUT_SERVICE_SYNC_LOG_EVERY restores one line per deferred tick.
class SyncDeferLog {
public:
    std::string onResult(int rc) {
        if (rc == 0) {
            const int n = ticks_;
            ticks_ = 0;
            last_ = 0;
            return n ? "media: Service menu sync sent after " + std::to_string(n) + " deferred tick(s)" : std::string();
        }
        ++ticks_;
#ifndef MUT_SERVICE_SYNC_LOG_EVERY
        if (rc == last_)
            return {};
#endif
        last_ = rc;
        if (rc == -ENODEV)
            return "media: Service menu sync deferred: no OwenB ring attached yet (rc=-19 ENODEV); retrying every tick";
        return "media: Service menu sync deferred rc=" + std::to_string(rc);
    }
private:
    int ticks_ = 0, last_ = 0;
};
class Selection {
public:
    enum class Action { None, Sync, Switch };
    Action tick(uint64_t word, bool active, bool busy, uint64_t generation) {
        if (!(word & kCapability))
            return Action::None;
        if (generation_ != generation) {
            generation_ = generation;
            synced_ = sent_ = false;
        }
        const bool selected = (word & kSelection) != 0;
        if (!synced_) {
            if (!sent_)
                return Action::Sync;
            if (selected == active)
                synced_ = true;
            return Action::None;
        }
        return !busy && selected != active ? Action::Switch : Action::None;
    }
    void syncSent(bool ok) { sent_ = ok; }
    void resync() { synced_ = sent_ = false; }
    bool synced() const { return synced_; }
private:
    uint64_t generation_ = UINT64_MAX;
    bool synced_ = false, sent_ = false;
};

// Sanitise the environment passed across fork/exec into misterplex_mode.sh.
// misterplexd populates ~25+ MPX_* and helper defaults into its own environ via setenv();
// without stripping them before exec, the launched daemon inherits stale MPX_* keys from
// the previous mode instead of a clean cold-mode environment (10 MPX_* keys).
inline bool serviceSwitchPreservesEnvEntry(const char* entry) {
#ifdef MUT_SERVICE_SWITCH_INHERITS_MPX_ENV
    (void)entry;
    return true;
#else
    if (!entry || !entry[0])
        return false;
    const char* eq = std::strchr(entry, '=');
    if (!eq || eq == entry)
        return false;
    const size_t klen = static_cast<size_t>(eq - entry);
    auto keyIs = [entry, klen](const char* name) {
        const size_t nlen = std::strlen(name);
        return klen == nlen && std::strncmp(entry, name, klen) == 0;
    };
    if (klen >= 4 && std::strncmp(entry, "MPX_", 4) == 0) {
        return keyIs("MPX_SERVICE_REQUEST_DIR") ||
               keyIs("MPX_MODE_SWITCH_LOCK_PATH") ||
               keyIs("MPX_MENU_LOG");
    }
    if (keyIs("SPELUNKR") || keyIs("NET_RESUME") ||
        keyIs("AIRPLAY_XCODE") || keyIs("GCAST_XCODE") ||
        keyIs("OPENSUBTITLES_API_KEY") || keyIs("OPENSUBTITLES_USERNAME") ||
        keyIs("OPENSUBTITLES_PASSWORD")) {
        return false;
    }
    return true;
#endif
}

inline std::vector<std::string> buildServiceSwitchChildEnv(const char* const* srcEnviron) {
    std::vector<std::string> out;
    if (!srcEnviron)
        return out;
    for (const char* const* p = srcEnviron; *p; ++p) {
        if (serviceSwitchPreservesEnvEntry(*p))
            out.emplace_back(*p);
    }
    return out;
}

inline std::vector<char*> envPointers(std::vector<std::string>& storage) {
    std::vector<char*> ptrs;
    ptrs.reserve(storage.size() + 1);
    for (auto& s : storage)
        ptrs.push_back(const_cast<char*>(s.c_str()));
    ptrs.push_back(nullptr);
    return ptrs;
}
} // namespace misterplex::service
