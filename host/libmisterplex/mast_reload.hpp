#pragma once

// MAST-WEDGE (review 21:14 read-only RCA, release 21:16/21:17, review 21:15 GO).
// After a fresh core load, the MrAudio session lane's publisher (audio_session_mailbox.sv) can
// stop: every wait in its FSM lacks a timeout, and reset_dec does not reset its state. Only an
// FPGA reconfiguration restarts it, so no begin retry helps. The daemon therefore:
//  1. probes MAST once per core-gate open: magic and ABI decode, publication counter
//     advancing; one log line;
//  2. on a wedge (at gate open, or at a begin) hands ONE core reload to the on-box launcher.
//     It runs `$MPX_MAST_RELOAD_CMD mast-reload <pid> <reason> <marker>` detached, with every
//     fd above 2 closed, then exits with kReloadExitCode. Exiting releases the /dev/mem and
//     mplex_ddr maps, the locks and the ports.
// The daemon never sends the reload command itself (the LOADCORE-GUARD 0-misterplexd rule).
// The launcher runs the guarded load only after this process is gone, then restarts the
// daemon its usual verified way.
// A marker file (tmpfs, so it is cleared on reboot) stops a second request until a probe has
// passed on the reloaded core. A wedge after an unrecovered reload is a clean fail, never a
// loop. After a recovery, a new episode still has to wait kReloadMinIntervalS.
// The launcher (scripts/misterplexd-boot.sh mast-reload) loads only while CORENAME is Plex (it
// never replaces a core or menu the user switched to). A launcher that runs no load writes
// state=refused why=<why>; the daemon treats that as terminal ('no reload ran (<why>)').
// A load that fails past the guard (loader rc other than the guard's 97/98) writes
// state=load-failed why=load-failed-rc<rc>, also terminal ('reload attempted, loader rc=<rc>';
// review 22:00 N1).
// Glass G1 23:49 / review 23:53: a hand-off made inside the start launcher's verify window
// was dropped on the launcher lock, and the next daemon called the lane recovered. Now the start
// launcher yields to a pending hand-off and the mast-reload instance waits for the lock (R1);
// 'recovered' is written only from 'attempted', and a 'requested' the launcher never took is
// 'no reload ran', never a failed cure (R2).

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MPX_MAST_RELOAD_MARKER_PATH
#define MPX_MAST_RELOAD_MARKER_PATH "/tmp/misterplexd.mast-reload"
#endif
// main.cpp guards its hand-off on this (the audio-recovery negative control builds this tree's
// main.cpp against older daemon sources, which have no mast_reload.hpp).
#define MPX_HAVE_MAST_RELOAD 1

namespace misterplex {
namespace mast {

constexpr int kReloadExitCode = 86;
constexpr int64_t kReloadMinIntervalS = 600;
constexpr const char* kMarkerPath = MPX_MAST_RELOAD_MARKER_PATH;

// One gate-open liveness probe (FpgaSpi::probeAudioLane fills it).
struct Probe {
    bool lane = false;       // the core advertises the MrAudio session lane
    bool mapped = false;     // the MAST page could be read at all
    unsigned reads = 0;      // MAST reads made
    unsigned decoded = 0;    // ... that decoded (magic, ABI, commit)
    uint32_t firstPub = 0;   // publication counter of the first decoded read
    uint32_t lastPub = 0;    // ... and of the last
    unsigned fresh = 0;      // publications after the first decoded read
    unsigned needed = 3;     // fresh publications that prove a live lane
    int windowMs = 250;
    int elapsedMs = 0;
    bool live = false;
};

inline std::string probeLine(const Probe& p, const char* when) {
    std::string s = std::string("core gate: MAST probe (") + when + "): ";
    if (!p.lane)
        return s + "no MrAudio session lane on this core; skipped";
    char buf[256];
    if (p.live) {
        std::snprintf(buf, sizeof buf, "live, publication %u -> %u (+%u in %d ms)", p.firstPub,
                      p.lastPub, p.fresh, p.elapsedMs);
        return s + buf;
    }
    if (!p.mapped)
        return s + "WEDGED: MAST unreadable (mailbox page not mapped)";
    if (p.decoded == 0) {
        std::snprintf(buf, sizeof buf,
                      "WEDGED: no valid MAST in %d ms (%u reads, none with the magic/ABI/commit): "
                      "the lane never published after this load",
                      p.elapsedMs, p.reads);
        return s + buf;
    }
    std::snprintf(buf, sizeof buf,
                  "WEDGED: publication stuck at %u (+%u of %u in %d ms, %u/%u reads decoded)",
                  p.lastPub, p.fresh, p.needed, p.elapsedMs, p.decoded, p.reads);
    return s + buf;
}

// ---- the reload marker ----
// Lines "key=value": state=requested|attempted|recovered|cure-failed|refused|load-failed,
// time=<unix s>, reason=<the daemon's text>, why=<the launcher's refusal or load failure, or the
// daemon's gate-silent:<reason> (R5a)>, r6=<the guarded loader's R6 quiesce outcome, appended
// before its load_core and kept by the daemon's later rewrites>.
// requested: the daemon handed a reload off; attempted: the launcher sent it; recovered: a probe
// passed on the reloaded core; cure-failed: wedged again after the reload; refused: the launcher
// ran no load (daemon still alive, core switched, guard refused with rc 97/98, loader missing);
// load-failed: the guard passed and the loader failed (any other rc), so a reload was attempted
// and the core state is unknown.
struct Marker {
    bool present = false;
    std::string state;
    int64_t timeS = 0;
    std::string reason;
    std::string why;
    // R6 (review 02:08): the guarded loader's quiesce outcome before its load_core
    // (quiesced (...) | not-quiesced (...) | no-feed (...)), kept on every rewrite after it.
    std::string r6;
};

inline Marker parseMarker(const std::string& text) {
    Marker m;
    m.present = true;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(pos, end - pos);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "state")
                m.state = v;
            else if (k == "time")
                m.timeS = std::strtoll(v.c_str(), nullptr, 10);
            else if (k == "reason")
                m.reason = v;
            else if (k == "why")
                m.why = v;
            else if (k == "r6")
                m.r6 = v;
        }
        pos = end + 1;
    }
    return m;
}

inline std::string formatMarker(const std::string& state, int64_t timeS,
                                const std::string& reason, const std::string& why = std::string(),
                                const std::string& r6 = std::string()) {
    return "state=" + state + "\ntime=" + std::to_string(timeS) + "\nreason=" + reason + "\n" +
           (why.empty() ? std::string() : "why=" + why + "\n") +
           (r6.empty() ? std::string() : "r6=" + r6 + "\n");
}

inline Marker readMarker(const char* path = kMarkerPath) {
    Marker m;
    FILE* f = std::fopen(path, "r");
    if (!f)
        return m;
    std::string text;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 4096)
        text.append(buf, n);
    std::fclose(f);
    return parseMarker(text);
}

inline bool writeMarkerWhy(const std::string& state, int64_t timeS, const std::string& reason,
                           const std::string& why, const char* path = kMarkerPath,
                           const std::string& r6 = std::string()) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    const std::string body = formatMarker(state, timeS, reason, why, r6);
    const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    if (std::fclose(f) != 0 || !ok) {
        std::remove(tmp.c_str());
        return false;
    }
    return std::rename(tmp.c_str(), path) == 0;
}

inline bool writeMarker(const std::string& state, int64_t timeS, const std::string& reason,
                        const char* path = kMarkerPath) {
    return writeMarkerWhy(state, timeS, reason, std::string(), path);
}

enum class Action {
    RequestReload,  // hand one reload to the launcher and exit
    NoHelper,       // no launcher configured: clean fail, as before (no blind retries)
    AfterReload,    // a reload was already handed off and no probe passed since: clean fail
    RateLimited,    // recovered less than kReloadMinIntervalS ago: clean fail
    NoReloadRan,    // no reload ran: state=refused (why=...) or a requested hand-off not taken
    LoadFailed,     // the load failed past the guard (state=load-failed)
    // Both are terminal: no auto reload until the MiSTer reboots (tmpfs marker). A reload by hand
    // cures the lane but not the marker (review 00:18 P3: only state=attempted becomes recovered).
};

inline Action decide(bool haveHelper, const Marker& m, int64_t nowS) {
    if (!haveHelper)
        return Action::NoHelper;
#ifndef MUT_MAST_RELOAD_LOOP   // test-only mutant: ignore the marker (reload on every wedge)
    if (m.present) {
        // review 23:53 R2: 'requested' means the launcher never took the hand-off (glass G1
        // 23:49). No reload ran, so this is neither a failed cure nor a reason to ask again.
        if (m.state == "refused" || m.state == "requested")
            return Action::NoReloadRan;
        if (m.state == "load-failed")
            return Action::LoadFailed;
        if (m.state != "recovered")   // attempted, cure-failed or unreadable
            return Action::AfterReload;
        if (nowS - m.timeS < kReloadMinIntervalS)
            return Action::RateLimited;
    }
#else
    (void)m;
    (void)nowS;
#endif
    return Action::RequestReload;
}

// A wedge while the marker says attempted: the one reload ran and did not cure it (review 21:39:
// not yet proven that one reload cures a (B) wedge). The daemon then writes the terminal state
// cure-failed, logs cureFailedLine once and stays up; plays fail clean. Only a reboot (tmpfs)
// clears it, and nothing loops on it. A later escalation policy can key on cure-failed.
// 'requested' is not a failed cure: the launcher never took the hand-off (review 23:53 R2).
inline bool cureFailed(const Marker& m) {
    return m.present && m.state == "attempted";
}

inline std::string cureFailedLine(const Marker& m, int64_t nowS, const std::string& where) {
    return "MAST-WEDGE CURE FAILED (" + where +
           "): the MrAudio lane is still wedged after the auto core reload (" + m.reason +
           ", marker " + m.state + " " + std::to_string(nowS - m.timeS) +
           " s ago): no further reload; plays fail clean until the core is reloaded by hand or "
           "the MiSTer reboots (marker now cure-failed)";
}

// A live gate-open probe while a marker exists (review 23:53 R2). Only a reload that ran
// (state=attempted) becomes recovered. 'requested' means the launcher never took the hand-off,
// so no reload ran: say so, and leave the marker for the launcher (R1). Glass G1 23:49 logged
// 'live after the auto core reload' here with no reload. Terminal states stay until a reboot.
// review 01:10 R5c: a gate that opens on a cure-failed marker is a manual recovery (a reload by
// hand), never 'recovered'; the marker stays cure-failed.
enum class LiveAction { MarkRecovered, HandoffNotTaken, ManualRecovery, KeepTerminal };

inline LiveAction onLive(const Marker& m) {
    if (m.state == "attempted")
        return LiveAction::MarkRecovered;
#ifdef MUT_MAST_RECOVER_FROM_REQUESTED   // test-only mutant: the glass G1 23:49 defect
    if (m.state == "requested")
        return LiveAction::MarkRecovered;
#endif
    if (m.state == "requested")
        return LiveAction::HandoffNotTaken;
    if (m.state == "cure-failed")
        return LiveAction::ManualRecovery;
    return LiveAction::KeepTerminal;
}

inline std::string liveLine(LiveAction a, const Marker& m, int64_t nowS) {
    const std::string ago = std::to_string(nowS - m.timeS) + " s ago";
    switch (a) {
    case LiveAction::MarkRecovered:
        return "MAST lane live after the auto core reload (" + m.reason + ", marker state was " +
               m.state + " " + ago + "): marker now recovered";
    case LiveAction::HandoffNotTaken:
        return "MAST lane live; hand-off not taken: no reload ran (marker requested " + ago +
               ", reason " + m.reason + "): marker left requested for the launcher";
    case LiveAction::ManualRecovery:
        return "MAST lane live: manual recovery after cure-failed (marker cure-failed " + ago +
               (m.why.empty() ? std::string() : ", " + m.why) +
               "): the marker stays cure-failed: no further auto reload until the MiSTer reboots";
    case LiveAction::KeepTerminal:
        return "MAST lane live; marker state=" + (m.state.empty() ? std::string("?") : m.state) +
               " (" + ago + ") stays: no further auto reload until the MiSTer reboots";
    }
    return "MAST lane live";
}

// R5a (review 01:09, review 01:10/01:28; glass G2 01:03: after the auto reload the core never
// answered CAPS, the gate stayed closed, the marker stayed 'attempted' and nothing was loud). A
// daemon that starts on an 'attempted' marker and whose core gate does not open is a failed cure:
// the marker goes to cure-failed (why=gate-silent:<the gate's reason>), loud, and no reload ever
// follows. Bound: kGateDeadlineS from this daemon's start AND the gate's own 5-probe give-up,
// whichever is later (the gate probe sequence must have ended). A normal come-up opens the gate a
// few seconds after start (glass G1: about 2 s; the e2e: under 1 s). The give-up lands about
// 16-20 s after start (0.6 s settle, then 1+2+4+8 s backoff plus the probes). 60 s is over 10x the
// normal come-up and about 3x the give-up. The gate keeps reacting to reload signals, so a reload
// by hand still opens it (then ManualRecovery above). The verdict needs CORENAME to say Plex: a
// user switch to MENU or another core (the gate is then held 'not Plex') is no failed cure (the
// M1 rule's spirit); the marker then waits, as before R5.
#ifndef MPX_MAST_GATE_DEADLINE_S
#define MPX_MAST_GATE_DEADLINE_S 60
#endif
constexpr int64_t kGateDeadlineS = MPX_MAST_GATE_DEADLINE_S;

inline bool silentGateCureFailed(const Marker& m, bool gateOpen, bool gateProbing, bool corePlex,
                                 int64_t sinceStartS) {
#ifdef MUT_MAST_SILENT_NO_GATE   // test-only mutant: no verdict, the glass G2 01:03 silence
    (void)m;
    (void)gateOpen;
    (void)gateProbing;
    (void)corePlex;
    (void)sinceStartS;
    return false;
#else
    return m.present && m.state == "attempted" && !gateOpen && !gateProbing && corePlex &&
           sinceStartS >= kGateDeadlineS;
#endif
}

// The loud line (the daemon log adds '!!! ', as the launcher's loud() does).
inline std::string silentGateLine(const Marker& m, int64_t nowS, int64_t sinceStartS,
                                  const std::string& gateWhy) {
    return "MAST cure failed: the core did not answer CAPS after the reload (" + gateWhy + "; " +
           std::to_string(sinceStartS) + " s since this daemon started, bound max(" +
           std::to_string(kGateDeadlineS) + " s, the gate's 5-probe give-up); reason " + m.reason +
           ", marker attempted " + std::to_string(nowS - m.timeS) + " s ago; R6 before the reload: " +
           (m.r6.empty() ? std::string("not recorded") : m.r6) +
           "): no further reload; terminal until the MiSTer reboots (marker now cure-failed)";
}

#ifndef MPX_MAST_BOOT_LOG_PATH
#define MPX_MAST_BOOT_LOG_PATH "/tmp/misterplexd-boot.log"
#endif
#ifndef MPX_MAST_BOOT_FAILED_PATH
#define MPX_MAST_BOOT_FAILED_PATH "/tmp/misterplexd-boot.FAILED"
#endif
// The launcher's loud channel (misterplexd-boot.sh loud()): '<date> up=<s> !!! <text>' in the
// boot log and '<date> <text>' in the FAILED file.
inline void appendBootLoud(const std::string& text, const char* blog = MPX_MAST_BOOT_LOG_PATH,
                           const char* failf = MPX_MAST_BOOT_FAILED_PATH) {
    char date[32] = "?";
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    if (localtime_r(&t, &tmv))
        std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tmv);
    char up[32] = "?";
    if (FILE* u = std::fopen("/proc/uptime", "r")) {
        if (std::fscanf(u, "%31s", up) != 1)
            std::snprintf(up, sizeof up, "?");
        std::fclose(u);
    }
    if (FILE* f = std::fopen(blog, "a")) {
        std::fprintf(f, "%s up=%s !!! %s\n", date, up, text.c_str());
        std::fclose(f);
    }
    if (FILE* f = std::fopen(failf, "a")) {
        std::fprintf(f, "%s %s\n", date, text.c_str());
        std::fclose(f);
    }
}

// Lab only (glass rows: a natural wedge is ~1/13 fresh loads, with no on-demand repro):
// MPX_MAST_FORCE_WEDGE=once makes the gate-open probe report a wedge while no marker exists
// (the reload path, then a live probe after it); =always makes every gate-open probe report one
// (the cure-failed path). The probe line says FORCED. Prod never sets it.
inline bool forceWedge(const char* mode, bool markerPresent) {
    if (!mode)
        return false;
    const std::string m = mode;
    return m == "always" || (m == "once" && !markerPresent);
}

// The loader rc from why=load-failed-rc<rc>; "?" if the launcher wrote none.
inline std::string loaderRc(const Marker& m) {
    static const std::string kPrefix = "load-failed-rc";
    if (m.why.size() > kPrefix.size() && m.why.compare(0, kPrefix.size(), kPrefix) == 0)
        return m.why.substr(kPrefix.size());
    return "?";
}

inline std::string actionLine(Action a, const Marker& m, int64_t nowS, const std::string& where) {
    const std::string age = m.present ? std::to_string(nowS - m.timeS) + " s" : "-";
    switch (a) {
    case Action::RequestReload:
        return "MAST wedge (" + where +
               "): handing ONE guarded core reload to the launcher and exiting (the launcher "
               "reloads the core once this process is gone, then restarts it)";
    case Action::NoHelper:
        return "MAST wedge (" + where +
               "): no auto reload configured (MPX_MAST_RELOAD_CMD unset); reload the core";
    case Action::AfterReload:
        return "MAST wedge (" + where + ") again after the auto core reload (marker state=" +
               (m.state.empty() ? std::string("?") : m.state) + ", " + age +
               " ago): no further reload, clean fail; reload the core by hand";
    case Action::RateLimited:
        return "MAST wedge (" + where + "): the last auto reload recovered only " + age +
               " ago (minimum " + std::to_string(kReloadMinIntervalS) +
               " s): no reload now, clean fail; reload the core by hand";
    case Action::NoReloadRan: {
        std::string why = m.why;
        if (why.empty())
            why = m.state == "requested" ? "hand-off not taken" : "?";
        return "MAST wedge (" + where + "): no reload ran (" + why + "; marker " +
               (m.state.empty() ? std::string("?") : m.state) + " " + age +
               " ago): no auto reload until the MiSTer reboots; reload the core by hand to play; "
               "clean fail";
    }
    case Action::LoadFailed:
        return "MAST wedge (" + where + "): reload attempted, loader rc=" + loaderRc(m) + " (" +
               (m.why.empty() ? std::string("?") : m.why) + "; marker load-failed " + age +
               " ago): no auto reload until the MiSTer reboots; reload the core by hand to play; "
               "clean fail";
    }
    return "MAST wedge";
}

// Runs `cmd mast-reload <daemonPid> <reason> <marker>` in a new session, detached from this
// process (double fork). stdin is /dev/null, stdout/stderr are inherited, and every other fd is
// closed before exec. Returns true once exec has succeeded (a CLOEXEC pipe reports any failure).
inline bool spawnHelper(const std::string& cmd, pid_t daemonPid, const std::string& reason,
                        const std::string& marker, std::string* err) {
    const std::string pidText = std::to_string(daemonPid);
    const char* argv[] = {cmd.c_str(), "mast-reload", pidText.c_str(), reason.c_str(),
                          marker.c_str(), nullptr};
    struct rlimit rl {};
    long maxFd = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        maxFd = static_cast<long>(rl.rlim_cur);
    if (maxFd > 65536)
        maxFd = 65536;
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0) {
        if (err)
            *err = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    const pid_t mid = fork();
    if (mid < 0) {
        if (err)
            *err = std::string("fork: ") + std::strerror(errno);
        close(report[0]);
        close(report[1]);
        return false;
    }
    if (mid == 0) { // only async-signal-safe calls from here to exec
        close(report[0]);
        setsid();
        const pid_t helper = fork();
        if (helper != 0)
            _exit(helper < 0 ? 1 : 0);
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0 && nul != 0) {
            dup2(nul, 0);
            close(nul);
        }
#ifndef MUT_MAST_KEEP_FDS   // test-only mutant: the helper inherits the daemon's fds
        for (long fd = 3; fd < maxFd; ++fd)
            if (fd != report[1])
                close(static_cast<int>(fd));
#endif
        execv(cmd.c_str(), const_cast<char* const*>(argv));
        const int e = errno;
        ssize_t w = write(report[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(report[1]);
    int status = 0;
    while (waitpid(mid, &status, 0) < 0 && errno == EINTR) {
    }
    int e = 0;
    ssize_t r;
    do {
        r = read(report[0], &e, sizeof e);
    } while (r < 0 && errno == EINTR);
    close(report[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (err)
            *err = "the detaching fork failed";
        return false;
    }
    if (r == static_cast<ssize_t>(sizeof e)) {
        if (err)
            *err = "exec " + cmd + ": " + std::strerror(e);
        return false;
    }
    return true;
}

}  // namespace mast
}  // namespace misterplex
