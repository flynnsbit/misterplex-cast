#pragma once
// Loaded-core gate for misterplexd's FPGA traffic, v2.
//
// review, review-hdmi-fixes-review-1004 Y1: v1 opened and closed on the core name
// alone. It failed open, and it never saw a Plex->Plex reload or the reprogram window.
// v2 rules:
//
// Closed at start, and closed by ANY reload signal. Every signal stays readable while the
// FPGA is being reprogrammed: an HPS register read or a tmpfs stat.
//   - FPGA-manager stat (0xFF706000) mode != 4 (user mode).
//   - GPO bit 30: Main's fpga_core_reset(1) in fpga_load_rbf / app_restart. It stays set
//     until the new Main's fpga_io_init clears GPO (Main_MiSTer fpga_io.cpp:474, :623,
//     :537).
//   - Main replaced: the oldest process named MiSTer changed. app_restart double-forks a
//     new Main (fpga_io.cpp:620-645). Main's own children (system(), a fork before its
//     exec) are newer than Main, so they never count.
//   - /tmp/CORENAME or /tmp/RBFNAME content or mtime changed. The new Main writes both
//     in SetUARTMode, after the reprogram (user_io.cpp:1237-1238). A Plex->Plex reload
//     rewrites "Plex" with a new mtime.
// FpgaSpi re-reads the two register signals right before every DDR map use, doorbell
// and SPI window, and closes the gate on the spot (closeNow).
//
// Opens only on a verified build. After a reload the gate waits for:
//   - fabric in user mode with the core out of reset;
//   - Main present;
//   - CORENAME rewritten since the reload (the old one is stale) and equal to Plex/PLEX;
//     at the daemon's first look, CORENAME and RBFNAME written at or after the current
//     Main's start (their mtime against btime + its start ticks; a name older than boot
//     was written before a clock step and is left to the probe);
//   - RBFNAME present and a Plex core (rbfNameIsPlex);
//   - no signal change for kSettleUs.
// Then it asks the daemon for one CAPS probe: an AU-ring reset plus one Probe record.
// While a probe runs, FpgaSpi lets nothing but the AU ring through: no frame bank,
// doorbell, mailbox or SPI. The core writes CAPS only in reply to a probe
// (ddr_bitstream_reader.sv:728-739), so a DDR read alone would return the last core's
// reply. Only a build ID on the allowlist (fpga_build_ids.hpp) opens the gate.
// A recognised build of another raster re-pairs this daemon to that build and
// opens: a menu core switch must not keep the previous BUILD_ID. The player
// applies that core's display raster when the gate reports the re-pair.
// Each of these keeps it closed:
//   - an empty, unreadable, unknown or non-Plex name;
//   - no CAPS reply (bounded retries);
//   - a build that is not allowed.
// Every reopen, Plex->Plex included, resets the per-core FPGA state (onCoreReturned).
//
// Lab tools that link FpgaSpi (push_frame, set_status) and tests that are not about the
// gate build with MPX_CORE_GATE_START_OPEN: an unmanaged, always-open gate.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "idle_pairing_error.hpp"
#include "fpga_build_ids.hpp"
#include "owenb_core_identity.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {

#ifndef MPX_CORENAME_PATH
#define MPX_CORENAME_PATH "/tmp/CORENAME"
#endif
#ifndef MPX_RBFNAME_PATH
#define MPX_RBFNAME_PATH "/tmp/RBFNAME"
#endif
constexpr const char* kCoreNamePath = MPX_CORENAME_PATH;
constexpr const char* kRbfNamePath = MPX_RBFNAME_PATH;

// Live MiSTer core name from Main. Empty = unreadable/transient.
inline std::string readCoreNameFile(const char* path = kCoreNamePath) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    char buf[64];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return {};
    buf[n] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

inline bool coreNameIsPlex(const std::string& name) {
    return name == "Plex" || name == "PLEX";
}

// /tmp/RBFNAME names the RBF's core: Main writes its CONF_STR name there ("Plex" on a lab MiSTer
// for every Plex RBF; CORENAME differs only under an MRA name override). Accept that, or
// an RBF file name: the basename, less any ".rbf", must start with Plex.
inline bool rbfNameIsPlex(const std::string& rbf) {
    std::string base = rbf.substr(rbf.find_last_of('/') == std::string::npos
                                      ? 0
                                      : rbf.find_last_of('/') + 1);
    if (base.size() > 4) {
        std::string ext = base.substr(base.size() - 4);
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".rbf")
            base.resize(base.size() - 4);
    }
    if (base.size() < 4)
        return false;
    std::string head = base.substr(0, 4);
    for (char& c : head)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return head == "plex";
}

// OWENB-P1B T3: which family the loaded core belongs to, from its two names (owenb_core_identity.hpp).
//   PlexLegacy  CORENAME Plex/PLEX + a Plex RBF that is not PlexOB*: the CAPS probe decides.
//   PlexOwenb   CORENAME PlexOB (the fork's CONF_STR rename hook) + RBFNAME PlexOB*: the ring identity
//               probe decides. FpgaSpi's legacy traffic stays closed (owenbOpen(), never open()).
//   RawH264     owenb's stock core (CONF_STR H264): always refused.
//   Mismatch    the two names disagree (e.g. CORENAME Plex with a PlexOB RBF): refused.
enum class CoreFamily : uint8_t { Unknown, NotPlex, PlexLegacy, PlexOwenb, RawH264, Mismatch };

inline const char* toString(CoreFamily f) {
    switch (f) {
    case CoreFamily::Unknown: return "unknown";
    case CoreFamily::NotPlex: return "not Plex";
    case CoreFamily::PlexLegacy: return "Plex (legacy)";
    case CoreFamily::PlexOwenb: return "PlexOB (owenb ring)";
    case CoreFamily::RawH264: return "H264 (owenb stock, refused)";
    case CoreFamily::Mismatch: return "names disagree";
    }
    return "?";
}

// The names the gate takes for a PlexOB core. MUT_OWENB_GATE_ACCEPT_H264 (test-only) also takes
// owenb's stock "H264" names; test_owenb_core_gate row G2 must catch that.
inline bool coreNameIsOwenbAccepted(const std::string& name) {
#ifdef MUT_OWENB_GATE_ACCEPT_H264
    return coreNameIsPlexOwenb(name) || coreNameIsRawH264(name);
#else
    return coreNameIsPlexOwenb(name);
#endif
}
inline bool rbfNameIsOwenbAccepted(const std::string& rbf) {
#ifdef MUT_OWENB_GATE_ACCEPT_H264
    return rbfNameIsPlexOwenb(rbf) || rbfNameIsRawH264(rbf);
#else
    return rbfNameIsPlexOwenb(rbf);
#endif
}

inline CoreFamily classifyLoadedCore(const std::string& coreName, const std::string& rbfName) {
    if (coreName.empty() || rbfName.empty())
        return CoreFamily::Unknown;
    if (coreNameIsOwenbAccepted(coreName))
        return rbfNameIsOwenbAccepted(rbfName) ? CoreFamily::PlexOwenb : CoreFamily::Mismatch;
    if (coreNameIsRawH264(coreName))
        return CoreFamily::RawH264;
    if (coreNameIsPlex(coreName))
        return rbfNameIsPlex(rbfName) && !rbfNameIsPlexOwenb(rbfName) ? CoreFamily::PlexLegacy
                                                                         : CoreFamily::Mismatch;
    return CoreFamily::NotPlex;
}

// ---------------------------------------------------------------- signals

constexpr uint32_t kHpsRegsMapBase = 0xFF000000u; // FpgaSpi's 16 MiB register map
constexpr uint32_t kFpgaMgrStatPhys = 0xFF706000u;
constexpr uint32_t kFpgaMgrGpoPhys = 0xFF706010u;
constexpr uint32_t kFpgaMgrModeMask = 0x7u;
constexpr uint32_t kFpgaMgrModeUser = 4u;
constexpr uint32_t kGpoCoreResetBit = 1u << 30;

struct FabricState {
    bool known = false;
    uint32_t mode = 0;
    bool coreReset = false;
    bool live() const { return known && mode == kFpgaMgrModeUser && !coreReset; }
    std::string describe() const {
        if (!known)
            return "FPGA manager registers not mapped";
        if (mode != kFpgaMgrModeUser)
            return "FPGA manager mode " + std::to_string(mode) + " (4 = user): reprogramming";
        if (coreReset)
            return "core held in reset (GPO bit 30): Main is loading a core";
        return "fabric live";
    }
};

// regs: FpgaSpi's map of 0xFF000000 (nullptr = not mapped). HPS-side reads only.
inline FabricState readFabricState(const volatile uint32_t* regs) {
    FabricState f;
    if (!regs)
        return f;
    f.known = true;
    f.mode = regs[(kFpgaMgrStatPhys - kHpsRegsMapBase) >> 2] & kFpgaMgrModeMask;
    f.coreReset = (regs[(kFpgaMgrGpoPhys - kHpsRegsMapBase) >> 2] & kGpoCoreResetBit) != 0;
    return f;
}

struct CoreFileStamp {
    bool exists = false;
    std::string text; // trimmed
    int64_t mtimeNs = 0;
    bool operator==(const CoreFileStamp& o) const {
        return exists == o.exists && text == o.text && mtimeNs == o.mtimeNs;
    }
    bool operator!=(const CoreFileStamp& o) const { return !(*this == o); }
};

// Main_MiSTer as one process: the oldest process named MiSTer (start time in clock
// ticks, /proc/<pid>/stat field 22). Main's own children (system(), fork() before
// exec) carry its argv0 for a moment, but are always newer, so they never change it.
// A reload replaces it: app_restart's parent _exits and a grandchild execs the new Main.
// pid 0 = no Main.
struct MainIdentity {
    int pid = 0;
    uint64_t startTicks = 0;
    bool operator==(const MainIdentity& o) const {
        return pid == o.pid && startTicks == o.startTicks;
    }
    bool operator!=(const MainIdentity& o) const { return !(*this == o); }
};

inline CoreFileStamp stampCoreFile(const char* path) {
    CoreFileStamp s;
    struct stat st {};
    if (::stat(path, &st) != 0)
        return s;
    s.exists = true;
    s.mtimeNs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    s.text = readCoreNameFile(path);
    return s;
}

struct CoreObservation {
    FabricState fabric;
    MainIdentity main;
    int64_t mainStartWallNs = 0; // Main's start on the wall clock, a lower bound (0: unknown)
    int64_t bootWallNs = -1;     // boot on today's wall clock: btime, rounded down (-1: unknown)
    CoreFileStamp coreName;
    CoreFileStamp rbfName;
    int64_t nowUs = 0; // monotonic
};

#ifndef MPX_PROC_STAT_PATH
#define MPX_PROC_STAT_PATH "/proc/stat"
#endif

// Boot on the wall clock as it reads now: btime (/proc/stat, whole seconds, rounded down),
// or -1 when unreadable. The kernel derives it from the current clock, so it moves with
// every clock step. 0 is a real value: an RTC-less MiSTer boots at 1970-01-01 00:00 UTC
// and keeps btime 0 until ntpd steps the clock, and for good when it never syncs.
inline int64_t bootWallNs(const char* procStat = MPX_PROC_STAT_PATH) {
    const int fd = ::open(procStat, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        text.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    const size_t at = text.find("\nbtime ");
    if (at == std::string::npos)
        return -1;
    const char* digits = text.c_str() + at + 7;
    char* end = nullptr;
    const long long btime = std::strtoll(digits, &end, 10);
    if (end == digits || btime < 0)
        return -1;
    return static_cast<int64_t>(btime) * 1000000000LL;
}

// Main's start on the wall clock: btime plus its start ticks (0 when unknown). A lower
// bound of the true start, so a name Main wrote is never older, unless the clock stepped
// forward after the write (namesPredateMain handles the boot-time step).
inline int64_t mainStartWallNs(const MainIdentity& m, const char* procStat = MPX_PROC_STAT_PATH) {
    if (m.pid == 0)
        return 0;
    const long hz = ::sysconf(_SC_CLK_TCK);
    const int64_t boot = bootWallNs(procStat);
    if (hz <= 0 || boot < 0)
        return 0;
    return boot + static_cast<int64_t>(m.startTicks) * 1000000000LL / hz;
}

inline int64_t coreGateNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------- gate

enum class CoreGateState { Closed, Verifying, Open, Rejected };
enum class CoreGateEvent { None, Closed, ProbeWanted, Opened, Rejected };

class CoreGate {
public:
    static constexpr int64_t kSettleUs = 600000;
    static constexpr int kMaxProbeAttempts = 5;

    static CoreGate& process() {
        static CoreGate gate;
        return gate;
    }

    CoreGate() { resetForTest(kStartOpen); }

    // Hot path: FpgaSpi choke points and MediaPlayer callers.
    bool open() const { return open_.load(std::memory_order_acquire); }
    // open() plus a fresh read of the fabric through the registered register map, for
    // paths with no FpgaSpi call of their own (audio pump, session loop). A fabric that
    // is not live closes the gate; no registered map reads as closed.
    bool openAndLive() {
        if (!managed())
            return true;
        if (!open())
            return false;
        const FabricState fabric = readFabricState(regs_.load(std::memory_order_acquire));
        if (fabric.live())
            return true;
        if (fabric.known)
            closeNow(fabric.describe());
        return false;
    }
    // FpgaSpi::open/close register and drop their 0xFF000000 map (openAndLive reads it).
    void setFabricRegs(const volatile uint32_t* regs) {
        regs_.store(regs, std::memory_order_release);
    }
    void dropFabricRegs(const volatile uint32_t* regs) {
        regs_.compare_exchange_strong(regs, nullptr, std::memory_order_acq_rel);
    }
    // For a thread that may take no lock (the F1 PLXL beat, review 01:39 k1): the gate's
    // {open generation, build id} as of its last Open transition. A core change always closes the
    // gate and reopens it through probeResult, which publishes a new generation before open_.
    // Read open() first; equal snapshots read on both sides of an access mean the same opening.
    uint64_t openSnapshot() const { return openSnap_.load(std::memory_order_acquire); }
    static uint32_t snapshotBuildId(uint64_t snap) { return static_cast<uint32_t>(snap); }
    // openAndLive() without its side effect: a fabric that is not live (or no registered map) reads
    // false, and the gate is NOT closed here (closeNow takes the gate mutex); the gate's own tick
    // or an FpgaSpi choke point closes it.
    bool fabricLiveNoLock() const {
        return readFabricState(regs_.load(std::memory_order_acquire)).live();
    }
    bool openAndLiveNoLock() const {
        if (!open())
            return false;
        return fabricLiveNoLock();
    }
    // Rejected Plex core, fabric live enough for the idle painter to show both BUILD_IDs.
    // Lock-free (F1 PLXL beat). Menu/NES never set this.
    bool idleErrorPaintAllowed() const {
        return rejectedIdlePaint_.load(std::memory_order_acquire);
    }
    uint32_t lastCapsId() const { return lastCapsId_.load(std::memory_order_acquire); }
    uint32_t lastOpenId() const { return lastOpenId_.load(std::memory_order_acquire); }
    uint64_t idleErrorSnap() const { return idleErrorSnap_.load(std::memory_order_acquire); }
    std::string pairingErrorText() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != CoreGateState::Rejected)
            return {};
        const uint32_t caps = lastCapsId_.load(std::memory_order_relaxed);
        // An allowlisted core is not a pairing mismatch. The previous core's
        // lastOpenId_ must not be painted as this daemon.
        if (recognisedFpgaBuild(caps))
            return {};
        const uint32_t daemon =
            lastOpenId_.load(std::memory_order_relaxed)
                ? lastOpenId_.load(std::memory_order_relaxed)
                : daemonIdForPairedRaster(pairedRaster_);
        return formatPairingError(daemon, caps);
    }
    // True only on the thread running the CAPS probe, and only while it runs (AU ring
    // only). Any other thread still sees a closed gate.
    bool ringProbeAllowed() const { return probingThread(); }
    bool managed() const { return managed_.load(std::memory_order_acquire); }

    // One observation per idle tick.
    CoreGateEvent observe(const CoreObservation& o) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!managed_.load())
            return CoreGateEvent::None;
        const bool first = !haveLast_;
        // Reload signals: edges between this observation and the last.
        std::string why;
        bool reloadMarker = false; // fabric or Main: CORENAME must be rewritten after it
        if (!o.fabric.live() && (first || last_.fabric.live())) {
            why = o.fabric.describe();
            reloadMarker = true;
        } else if (!first && o.fabric.live() && !last_.fabric.live()) {
            why = "fabric live again";
        } else if (!first && o.main.pid != 0 && o.main != lastMain_) {
            // lastMain_ is the last Main seen; a moment with no Main readable is no signal.
            why = lastMain_.pid ? "Main restarted (pid " + std::to_string(lastMain_.pid) + " -> " +
                                      std::to_string(o.main.pid) + ")"
                                : "Main started (pid " + std::to_string(o.main.pid) + ")";
            reloadMarker = true;
        } else if (!first && o.coreName != last_.coreName) {
            why = "CORENAME rewritten ('" + last_.coreName.text + "' -> '" + o.coreName.text + "')";
        } else if (!first && o.rbfName != last_.rbfName) {
            why = "RBFNAME rewritten ('" + last_.rbfName.text + "' -> '" + o.rbfName.text + "')";
        }
        if (reloadMarker && !needFreshName_) {
            needFreshName_ = true;
            freshWhy_ = "waiting for the new Main to rewrite CORENAME";
            // The stamp before this tick predates the reload: only a later write is fresh.
            preReloadName_ = first ? o.coreName : last_.coreName;
        }
        // First look on a live fabric: no edge has been seen, so ask the names themselves.
        // Every Main writes both in user_io_init (user_io.cpp:1808), after it starts; a stamp
        // older than the current Main's start is the last core's.
        if (first && o.fabric.live() && o.main.pid != 0 && namesPredateMain(o) &&
            !needFreshName_) {
            needFreshName_ = true;
            freshWhy_ = "CORENAME/RBFNAME predate Main pid " + std::to_string(o.main.pid) +
                        " (the last core's names); waiting for Main to write them";
            preReloadName_ = o.coreName;
        }
        last_ = o;
        haveLast_ = true;
        family_ = classifyLoadedCore(o.coreName.exists ? o.coreName.text : std::string(),
                                     o.rbfName.exists ? o.rbfName.text : std::string());
        familyNow_.store(static_cast<uint8_t>(family_), std::memory_order_release);
        if (o.main.pid != 0)
            lastMain_ = o.main;
        if (needFreshName_ && o.fabric.live() && o.coreName != preReloadName_)
            needFreshName_ = false;
        if (first)
            lastChangeUs_ = o.nowUs; // settle from the first look

        if (!why.empty()) {
            lastChangeUs_ = o.nowUs;
            probeAttempts_ = 0;
            nextProbeUs_ = 0;
            holdLogged_.clear();
            if (state_ == CoreGateState::Open) {
                closeLocked("core gate closed: " + why);
                return CoreGateEvent::Closed;
            }
            state_ = CoreGateState::Closed;
        }
        if (state_ == CoreGateState::Open || state_ == CoreGateState::Rejected)
            return CoreGateEvent::None;
        if (state_ == CoreGateState::Verifying)
            return CoreGateEvent::ProbeWanted; // until probeResult arrives

        // Closed: is the loaded core ready to be asked who it is?
        std::string hold;
        if (!o.fabric.live())
            hold = o.fabric.describe();
        else if (o.main.pid == 0)
            hold = "Main is not running";
        else if (needFreshName_)
            hold = freshWhy_;
        else if (!o.coreName.exists || o.coreName.text.empty())
            hold = "CORENAME empty or unreadable";
        else if (coreNameIsRawH264(o.coreName.text) && !coreNameIsOwenbAccepted(o.coreName.text))
            hold = "loaded core '" + o.coreName.text + "' is owenb's stock H.264 core; misterplexd drives "
                   "only its PlexOB fork (CONF_STR " + std::string(kOwenbCoreName) + ")";
        else if (!coreNameIsPlex(o.coreName.text) && !coreNameIsOwenbAccepted(o.coreName.text))
            hold = "loaded core '" + o.coreName.text + "' is not Plex";
        else if (!o.rbfName.exists || o.rbfName.text.empty())
            hold = "RBFNAME empty or unreadable";
        else if (coreNameIsOwenbAccepted(o.coreName.text) && !rbfNameIsOwenbAccepted(o.rbfName.text))
            hold = "RBFNAME '" + o.rbfName.text + "' is not a PlexOB core (CORENAME '" + o.coreName.text + "')";
        else if (coreNameIsOwenbAccepted(o.coreName.text))
            hold.clear(); // OWENB-P1B T3: a PlexOB pair; the ring identity probe decides
        else if (!rbfNameIsPlex(o.rbfName.text))
            hold = "RBFNAME '" + o.rbfName.text + "' is not a Plex core";
        else if (rbfNameIsPlexOwenb(o.rbfName.text))
            hold = "RBFNAME '" + o.rbfName.text + "' is a PlexOB core but CORENAME is '" + o.coreName.text +
                   "' (the PlexOB fork names itself " + std::string(kOwenbCoreName) + ")";
        if (!hold.empty()) {
            if (hold != holdLogged_) {
                holdLogged_ = hold;
                pending_.push_back("core gate stays closed: " + hold);
            }
            return CoreGateEvent::None;
        }
        if (o.nowUs - lastChangeUs_ < kSettleUs || o.nowUs < nextProbeUs_)
            return CoreGateEvent::None;
        state_ = CoreGateState::Verifying;
        return CoreGateEvent::ProbeWanted;
    }

    void beginProbe() { probingThread() = true; }
    void endProbe() { probingThread() = false; }

    // replied: the core answered the CAPS probe with buildId.
    // refusal: empty when buildId is a row the daemon may play (the allowlist), else why not.
    // raster: that row's raster ("480p", "240p" or "480i").
    // Pairing starts from the FPGA mode (setPairedRaster, main.cpp) or, when unpaired,
    // the first verified build. Every later allowlisted load re-probes CAPS and
    // re-selects that core (takeRePair), including a same-raster build-id change.
    // Startup MPX_FPGA_MODE does not stick. An unknown build stays closed.
    CoreGateEvent probeResult(bool replied, uint32_t buildId, const std::string& refusal,
                              const std::string& raster, const std::string& row,
                              int64_t nowUs) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != CoreGateState::Verifying)
            return CoreGateEvent::None; // a reload landed during the probe
        char id[16];
        std::snprintf(id, sizeof id, "0x%08X", buildId);
        holdLogged_.clear();
        rePairPending_ = false;
        // Menu core switch: the previous core's BUILD_ID must not stay in the pairing OSD.
        // A new allowlisted build re-pairs even when the raster label is unchanged
        // (480p CAVLC and High are both labelled 480p).
        const bool rasterChange = replied && refusal.empty() && !raster.empty() &&
                                  !pairedRaster_.empty() && raster != pairedRaster_;
        const bool buildChange =
            replied && refusal.empty() && buildId_ != 0 && buildId_ != buildId;
        if (rasterChange) {
            pending_.push_back("core gate re-paired: " + pairedRaster_ + " -> " + raster +
                               " on core load build " + id);
            pairedRaster_ = raster;
            pairedWhy_ = std::string("core load build ") + id;
        }
        if (replied && refusal.empty() && (pairedRaster_.empty() || raster == pairedRaster_)) {
            if (rasterChange || buildChange)
                rePairPending_ = true;
            if (buildChange && !rasterChange) {
                pending_.push_back(std::string("core gate re-read build ") + id + " raster " +
                                   raster);
            }
            state_ = CoreGateState::Open;
            buildId_ = buildId;
            lastCapsId_.store(buildId, std::memory_order_release);
            lastOpenId_.store(buildId, std::memory_order_release);
            rejectedIdlePaint_.store(false, std::memory_order_release);
            ++changes_;
            ++openEpoch_;
            openSnap_.store(((openEpoch_ & 0xFFFFFFFFull) << 32) | buildId, std::memory_order_release);
            open_.store(true, std::memory_order_release);
            pending_.push_back("core gate open: '" + last_.coreName.text + "' build " + id + " (" +
                               row + ") verified by CAPS; resuming (doorbell re-probe, idle repaint)");
            if (pairedRaster_.empty()) {
                pairedRaster_ = raster;
                pairedWhy_ = std::string("first verified build ") + id;
                pending_.push_back(pairingLine());
            }
            return CoreGateEvent::Opened;
        }
        if (replied) {
            markRejectedLocked(buildId);
            std::string why = refusal;
            if (why.empty())
                why = "pairing mismatch: a " + raster + " core, but this daemon is paired with " +
                      pairedRaster_ + " cores (" + pairedWhy_ + "); to drive this core, restart " +
                      "misterplexd configured for " + raster + " (MPX_FPGA_MODE=" + raster + ")";
            pending_.push_back("core gate stays closed: '" + last_.coreName.text +
                               "' answered CAPS with build " + id + " (" + row + "), " + why);
            return CoreGateEvent::Rejected;
        }
        const std::string reason = row.empty() ? std::string() : " (" + row + ")";
        if (++probeAttempts_ >= kMaxProbeAttempts) {
            markRejectedLocked(0);
            pending_.push_back("core gate stays closed: no CAPS reply from '" +
                               last_.coreName.text + "' after " +
                               std::to_string(probeAttempts_) + " probes" + reason);
            return CoreGateEvent::Rejected;
        }
        state_ = CoreGateState::Closed;
        nextProbeUs_ = nowUs + (500000LL << probeAttempts_);
        if (probeAttempts_ == 1)
            pending_.push_back("core gate stays closed: no CAPS reply from '" +
                               last_.coreName.text + "'" + reason + "; retrying");
        return CoreGateEvent::None;
    }

    // OWENB-P1B T3: the ring identity probe's result for a PlexOB pair (CoreFamily::PlexOwenb).
    // refusal: owenbIdentityRefusal() (empty = the OB build table allows this BUILD_ID).
    // On Open, owenbOpen() is true and open() stays false, so every FpgaSpi choke point
    // (coreGateAllows) keeps legacy DDR/SPI traffic (PLXB, CAPS, present banks, idle FB, PLXL
    // beat, status heartbeat) closed. The OB core is driven only through the OwenbRing producer.
    // Pairing is untouched: OB picks its raster at run time.
    // Retries (as for CAPS): no map, no magic, a frozen heartbeat. Refused at once: a version,
    // geometry or BUILD_ID refusal.
    CoreGateEvent owenbProbeResult(const OwenbIdentity& id, const std::string& refusal, int64_t nowUs) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != CoreGateState::Verifying)
            return CoreGateEvent::None; // a reload landed during the probe
        holdLogged_.clear();
        rePairPending_ = false;
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", id.buildId);
        const bool alive = id.verdict == OwenbIdVerdict::Alive;
        if (alive && refusal.empty()) {
            state_ = CoreGateState::Open;
            buildId_ = id.buildId;
            // Not lastOpenId_: that is the legacy daemon id a pairing error paints, never an OB id.
            lastCapsId_.store(0, std::memory_order_release);
            rejectedIdlePaint_.store(false, std::memory_order_release);
            ++changes_;
            ++openEpoch_;
            openSnap_.store(((openEpoch_ & 0xFFFFFFFFull) << 32) | id.buildId, std::memory_order_release);
#ifdef MUT_OWENB_GATE_LEGACY_OPEN
            open_.store(true, std::memory_order_release); // test-only mutant: legacy traffic allowed on OB
#endif
            owenbOpen_.store(true, std::memory_order_release);
            std::string label;
            if (const OwenbBuildInfo* row = recognisedOwenbBuild(id.buildId)) {
                if (row->label && row->label[0])
                    label = row->label;
            }
            pending_.push_back("core gate open: '" + last_.coreName.text + "' build " + hex +
                               (label.empty() ? "" : " " + label) +
                               " verified by ring identity (" + id.describe() +
                               "); OwenbRing backend, legacy FPGA traffic stays closed");
            return CoreGateEvent::Opened;
        }
        const bool retry = id.verdict == OwenbIdVerdict::NoMap || id.verdict == OwenbIdVerdict::NoMagic ||
                           id.verdict == OwenbIdVerdict::Stale;
        if (!retry) {
            markRejectedLocked(alive ? id.buildId : 0);
            pending_.push_back("core gate stays closed: '" + last_.coreName.text + "' " +
                               (refusal.empty() ? std::string(toString(id.verdict)) : refusal) + " (" +
                               id.describe() + ")");
            return CoreGateEvent::Rejected;
        }
        if (++probeAttempts_ >= kMaxProbeAttempts) {
            markRejectedLocked(0);
            pending_.push_back("core gate stays closed: no live owenb ring from '" + last_.coreName.text +
                               "' after " + std::to_string(probeAttempts_) + " probes (" + id.describe() + ")");
            return CoreGateEvent::Rejected;
        }
        state_ = CoreGateState::Closed;
        nextProbeUs_ = nowUs + (500000LL << probeAttempts_);
        if (probeAttempts_ == 1)
            pending_.push_back("core gate stays closed: no live owenb ring from '" + last_.coreName.text +
                               "' (" + id.describe() + "); retrying");
        return CoreGateEvent::None;
    }

    // OWENB-P1B T3: the gate is open on a verified PlexOB core (owenb ring). open() stays false.
    bool owenbOpen() const { return owenbOpen_.load(std::memory_order_acquire); }
    // The family of the last observed core names (lock-free; Unknown before the first look).
    CoreFamily loadedFamily() const { return static_cast<CoreFamily>(familyNow_.load(std::memory_order_acquire)); }
    // The loaded core owns black and idle in fabric (a PlexOB core): the daemon never paints DDR,
    // the idle FB at 0x31600000 included. paintIdle returns on this before it takes any latch.
    bool ownsIdleInFabric() const { return owenbOpen() || loadedFamily() == CoreFamily::PlexOwenb; }

    // A choke point read the fabric as not live (reprogram, core reset): close now.
    void closeNow(const std::string& why, int64_t nowUs = coreGateNowUs()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!managed_.load())
            return;
        lastChangeUs_ = nowUs;
        probeAttempts_ = 0;
        nextProbeUs_ = 0;
        if (!needFreshName_) {
            needFreshName_ = true;
            freshWhy_ = "waiting for the new Main to rewrite CORENAME";
            preReloadName_ = last_.coreName;
        }
        if (state_ == CoreGateState::Open)
            closeLocked("core gate closed: " + why);
        else
            state_ = CoreGateState::Closed;
    }

    // Transition and hold lines not yet logged, oldest first.
    std::vector<std::string> takeLogs() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        out.swap(pending_);
        for (auto& s : out)
            s = "misterplexd: " + s;
        return out;
    }

    uint64_t changes() const {
        std::lock_guard<std::mutex> lk(mu_);
        return changes_;
    }
    uint32_t buildId() const {
        std::lock_guard<std::mutex> lk(mu_);
        return buildId_;
    }
    CoreGateState state() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }
    // review 01:10 R5a: true while the gate's own CAPS probe sequence still runs (a probe in
    // flight, or a retry scheduled after a failed one), so a failed-cure verdict waits for the
    // 5-probe give-up.
    bool probeSequenceActive() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_ == CoreGateState::Verifying ||
               (state_ == CoreGateState::Closed && probeAttempts_ > 0);
    }
    // Why the gate is not open, for a log line: a give-up, a refusal or the last hold.
    std::string closedWhy() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == CoreGateState::Rejected)
            return probeAttempts_ >= kMaxProbeAttempts
                       ? "no CAPS reply after " + std::to_string(probeAttempts_) + " probes"
                       : "the core's CAPS reply was refused";
        if (!holdLogged_.empty())
            return holdLogged_;
        if (probeAttempts_ > 0)
            return "no CAPS reply after " + std::to_string(probeAttempts_) + " probe(s) so far";
        return "the gate never probed";
    }
    std::string name() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_.coreName.text;
    }
    // The raster this daemon is paired with ("" until paired).
    std::string pairedRaster() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pairedRaster_;
    }
    // The daemon, before its first tick: pair with its FPGA mode ("480p", "240p" or "480i").
    // why: where the mode comes from, for the log.
    void setPairedRaster(const std::string& raster, const std::string& why) {
        std::lock_guard<std::mutex> lk(mu_);
        pairedRaster_ = raster;
        pairedWhy_ = why;
        pending_.push_back(pairingLine());
    }

    // True once, after probeResult re-paired on this open. The player applies the
    // new core's display raster and then drops the flag.
    bool takeRePair() {
        std::lock_guard<std::mutex> lk(mu_);
        const bool v = rePairPending_;
        rePairPending_ = false;
        return v;
    }

    // startOpen: an unmanaged gate (lab tools, tests not about the gate). Otherwise
    // the daemon's fail-closed gate, as a new daemon process has it (no pairing yet).
    void resetForTest(bool startOpen) {
        std::lock_guard<std::mutex> lk(mu_);
        managed_.store(!startOpen);
        state_ = startOpen ? CoreGateState::Open : CoreGateState::Closed;
        open_.store(startOpen, std::memory_order_release);
        owenbOpen_.store(false, std::memory_order_release);
        family_ = CoreFamily::Unknown;
        familyNow_.store(0, std::memory_order_release);
        probingThread() = false;
        haveLast_ = false;
        last_ = {};
        lastMain_ = {};
        needFreshName_ = false;
        freshWhy_.clear();
        preReloadName_ = {};
        lastChangeUs_ = 0;
        nextProbeUs_ = 0;
        probeAttempts_ = 0;
        changes_ = 0;
        buildId_ = 0;
        lastCapsId_.store(0, std::memory_order_release);
        lastOpenId_.store(0, std::memory_order_release);
        rejectedIdlePaint_.store(false, std::memory_order_release);
        idleErrorSnap_.store(0, std::memory_order_release);
        openSnap_.store(0, std::memory_order_release);
        pairedRaster_.clear();
        pairedWhy_.clear();
        rePairPending_ = false;
        pending_.clear();
        holdLogged_.clear();
    }

private:
#ifdef MPX_CORE_GATE_START_OPEN
    static constexpr bool kStartOpen = true;
#else
    static constexpr bool kStartOpen = false;
#endif

    static bool& probingThread() {
        static thread_local bool probing = false;
        return probing;
    }

    void closeLocked(const std::string& line) {
        state_ = CoreGateState::Closed;
        open_.store(false, std::memory_order_release);
        owenbOpen_.store(false, std::memory_order_release);
        rejectedIdlePaint_.store(false, std::memory_order_release);
        ++changes_;
        pending_.push_back(line);
    }

    void markRejectedLocked(uint32_t capsId) {
        state_ = CoreGateState::Rejected;
        open_.store(false, std::memory_order_release);
        owenbOpen_.store(false, std::memory_order_release);
        lastCapsId_.store(capsId, std::memory_order_release);
        ++idleErrorEpoch_;
        idleErrorSnap_.store(idleErrorEpoch_, std::memory_order_release);
        rejectedIdlePaint_.store(coreNameIsPlex(last_.coreName.text), std::memory_order_release);
    }

    std::string pairingLine() const {
        return "core gate: this daemon is paired with " + pairedRaster_ + " cores (" +
               pairedWhy_ + "); a core of another raster stays closed until misterplexd restarts";
    }

    // A name that exists but was written before the current Main started (a lower bound)
    // is the last core's. A missing name is not stale: Main has yet to write it, and the
    // empty-name holds cover it. With Main's start unknown, an existing name is stale.
    // A name older than boot (on today's clock) cannot sit in /tmp, a tmpfs, unless the
    // clock stepped forward after it was written: ntpd -g steps an RTC-less MiSTer from
    // 1970 once the network is up, after the boot core's Main wrote its names. Such a
    // mtime says nothing about Main's start, so it does not hold; the CAPS probe decides.
    static bool namesPredateMain(const CoreObservation& o) {
        if (!o.coreName.exists && !o.rbfName.exists)
            return false;
        if (o.mainStartWallNs <= 0)
            return true;
        const auto stale = [&o](const CoreFileStamp& n) {
            if (!n.exists)
                return false;
            if (o.bootWallNs >= 0 && n.mtimeNs < o.bootWallNs)
                return false; // written before a clock step: not comparable
            return n.mtimeNs < o.mainStartWallNs;
        };
        return stale(o.coreName) || stale(o.rbfName);
    }

    mutable std::mutex mu_;
    std::atomic<bool> open_{false};
    std::atomic<bool> managed_{true};
    std::atomic<bool> owenbOpen_{false};       // OWENB-P1B T3: open on a PlexOB core (open_ stays false)
    CoreFamily family_ = CoreFamily::Unknown;  // the last observation's names (mu_)
    std::atomic<uint8_t> familyNow_{0};        // family_, for lock-free readers (paintIdle)
    std::atomic<const volatile uint32_t*> regs_{nullptr};
    CoreGateState state_ = CoreGateState::Closed;
    bool haveLast_ = false;
    CoreObservation last_;
    MainIdentity lastMain_; // the last Main seen (pid 0: none yet)
    bool needFreshName_ = false;
    std::string freshWhy_;  // the hold line while needFreshName_
    CoreFileStamp preReloadName_;
    int64_t lastChangeUs_ = 0;
    int64_t nextProbeUs_ = 0;
    int probeAttempts_ = 0;
    uint64_t changes_ = 0;
    uint32_t buildId_ = 0;
    std::atomic<uint32_t> lastCapsId_{0};
    std::atomic<uint32_t> lastOpenId_{0};
    std::atomic<bool> rejectedIdlePaint_{false};
    uint64_t idleErrorEpoch_ = 0;
    std::atomic<uint64_t> idleErrorSnap_{0};
    // openSnapshot(): {openEpoch_ (low 32 bits), buildId_}, published with release before open_. openEpoch_
    // counts every Open transition and is never reset (not even by resetForTest), so each opening has
    // its own snapshot. Only probeResult's Open transition and resetForTest write openSnap_.
    uint64_t openEpoch_ = 0;
    std::atomic<uint64_t> openSnap_{0};
    std::string pairedRaster_;
    std::string pairedWhy_;
    bool rePairPending_ = false;
    std::vector<std::string> pending_;
    std::string holdLogged_;
};

inline std::string coreGateClosedError(const char* what) {
    return std::string(what) + ": core gate closed (loaded core '" + CoreGate::process().name() +
           "' not verified)";
}

// The daemon's per-path checks, one name per path so tests and the source checks can
// find them (test_core_gate.cpp).
// FPGA audio pump, before every MrAudio write.
inline bool coreGateAllowsAudioWrite() { return CoreGate::process().openAndLive(); }
// Cast session AU loop, every iteration.
inline bool coreGateAllowsSession() { return CoreGate::process().openAndLive(); }
// A retained epoch that began before the last gate change died with that core instance:
// nothing can ACK its reset fence, so its ownership is dropped instead of aborted.
inline bool coreGateEpochIsStale(uint64_t retained, uint64_t changesAtBegin) {
    return retained != 0 && changesAtBegin != CoreGate::process().changes();
}

// The daemon's idle-tick sequence (MediaPlayer::coreGateTick). The daemon's actions are
// hooks, so test_core_gate.cpp drives this exact sequence and checks its order and
// conditions:
//   observe -> probe only while nothing plays -> log -> on Opened, reset the per-core
//   FPGA state -> once open and idle (retried every tick), drop a stale epoch, repaint.
struct CoreGateTickHooks {
    std::function<CoreObservation()> observe;      // fabric, Main, names, time
    std::function<bool()> playing;                 // a session is running
    std::function<CoreGateEvent()> probe;          // FpgaSpi::runCoreGateProbe under locks
    std::function<void()> coreReturned;            // FpgaSpi::onCoreReturned
    std::function<void()> finishReturn;            // drop an epoch from before the change
    std::function<void()> repaint;                 // paintIdle
    std::function<void(const std::string&)> log;
    // OWENB-P1B T3, both optional (a tool or test without them never opens on a PlexOB core).
    std::function<CoreGateEvent()> ringProbe;      // ring identity probe -> CoreGate::owenbProbeResult
    std::function<void()> owenbOpened;             // opened on a PlexOB core: no legacy reset, no repaint
};

// returnPending: the caller's flag, kept across ticks (set on Opened, cleared once the
// return is finished).
inline CoreGateEvent coreGateTickStep(CoreGate& gate, const CoreGateTickHooks& h,
                                      bool& returnPending) {
    if (!gate.managed())
        return CoreGateEvent::None;
    CoreGateEvent event = gate.observe(h.observe());
    if (event == CoreGateEvent::ProbeWanted && !h.playing())
        event = gate.loadedFamily() == CoreFamily::PlexOwenb ? (h.ringProbe ? h.ringProbe() : CoreGateEvent::None)
                                                             : h.probe();
    for (const auto& line : gate.takeLogs())
        h.log(line);
    if (event == CoreGateEvent::Opened && gate.owenbOpen()) {
        // OWENB-P1B T3: a PlexOB core owns black and idle in fabric. There is no FpgaSpi per-core reset
        // (its traffic stays closed) and no repaint (no daemon DDR paint on OB cores).
        returnPending = false;
        if (h.owenbOpened)
            h.owenbOpened();
        return event;
    }
    if (event == CoreGateEvent::Opened) {
        h.coreReturned();
        returnPending = true;
    }
    if (!returnPending || !gate.open() || h.playing())
        return event;
    returnPending = false;
    h.finishReturn();
    h.repaint();
    return event;
}

// OWENB-P1B T3: the daemon's CoreGateTickHooks::ringProbe. It reads the PlexOB core's ring
// identity through a read-only map of the control page at path (/dev/mem on the device, a sparse
// file in tests) and hands the result to the gate. The map is PROT_READ, 4 KiB at 0x31400000: no
// ring map, and the idle FB at 0x31600000 is never touched.
inline CoreGateEvent owenbGateRingProbe(CoreGate& gate, const char* path, unsigned heartbeatMs, int64_t nowUs,
                                        bool allowUnstamped) {
    OwenbIdentity id; // NoMap until the page maps
    {
        OwenbCtrlPageMap map;
        if (map.open(path) == 0)
            id = readOwenbIdentity(map.reader(), heartbeatMs);
    }
    return gate.owenbProbeResult(id, owenbIdentityRefusal(id, allowUnstamped), nowUs);
}

} // namespace misterplex
