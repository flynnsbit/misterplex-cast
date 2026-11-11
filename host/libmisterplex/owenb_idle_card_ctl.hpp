// The "daemon not running" idle card (colorbar removal 10-23). tools/owenb_idle_card is a tiny resident painter:
// it opens the owenb ring, arms the idle session and paints ErrorScreen::DaemonDown, and holds the session while
// no daemon runs. The ring lock admits one feeder, so the daemon releases the painter before it opens its own
// session (release() below, called first thing in main). No libav, no network, no video_mode write.
#pragma once
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace misterplex::owenb_idle_card {

inline constexpr char kComm[] = "owenb_idle_card";   // /proc/<pid>/comm of the painter (15 chars, the kernel's cap)

inline std::string pidPath() {
    const char* e = std::getenv("MPX_IDLE_CARD_PID");
    return e && *e ? e : "/tmp/mpx_idle_card.pid";
}

inline bool writePid(const std::string& path, pid_t pid) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "%ld\n", long(pid));
    return std::fclose(f) == 0;
}

inline pid_t readPid(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return 0;
    long v = 0;
    if (std::fscanf(f, "%ld", &v) != 1)
        v = 0;
    std::fclose(f);
    return v > 1 ? pid_t(v) : 0;
}

// True when `pid` is a live process whose comm is the painter (a recycled pid is not ours to signal).
inline bool isPainter(pid_t pid) {
    if (pid <= 1 || ::kill(pid, 0) != 0)
        return false;
    char p[64];
    {   // a zombie (exited, not yet reaped by its parent shell) is gone
        std::snprintf(p, sizeof p, "/proc/%ld/stat", long(pid));
        FILE* sf = std::fopen(p, "r");
        if (sf) {
            char sb[512] = {0};
            const size_t n = std::fread(sb, 1, sizeof sb - 1, sf);
            std::fclose(sf);
            sb[n] = 0;
            const char* rp = std::strrchr(sb, ')');
            if (rp && rp[1] == ' ' && rp[2] == 'Z')
                return false;
        }
    }
    std::snprintf(p, sizeof p, "/proc/%ld/comm", long(pid));
    FILE* f = std::fopen(p, "r");
    if (!f)
        return false;
    char buf[64] = {0};
    if (!std::fgets(buf, sizeof buf, f))
        buf[0] = 0;
    std::fclose(f);
    return std::strncmp(buf, kComm, sizeof kComm - 1) == 0;
}

// SIGTERM the painter named in the pidfile and wait for it to go (it closes its ring session on TERM).
// 0: none was running or it is gone; -1: still alive after waitMs (the caller goes on; its ring open reports it).
inline int release(const std::string& path, int waitMs) {
    const pid_t pid = readPid(path);
    if (pid == 0)
        return 0;
    if (!isPainter(pid)) {
        ::unlink(path.c_str());   // a stale pidfile
        return 0;
    }
    ::kill(pid, SIGTERM);
    for (int waited = 0; waited <= waitMs; waited += 20) {
        if (!isPainter(pid)) {
            ::unlink(path.c_str());
            return 0;
        }
        ::usleep(20 * 1000);
    }
    return -1;
}

// Fork a detached painter (setsid, no stdio). False when exe is not executable or the fork fails.
inline bool spawn(const std::string& exe, const std::string& card = "down") {
    if (::access(exe.c_str(), X_OK) != 0)
        return false;
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        ::setsid();
        const int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) {
            ::dup2(nul, 0);
            ::dup2(nul, 1);
            ::dup2(nul, 2);
        }
        // Close everything else: a daemon's listening sockets (:3005, :7000, :8008) must not outlive it in the painter.
        long mx = ::sysconf(_SC_OPEN_MAX);
        if (mx < 0 || mx > 4096)
            mx = 4096;
        for (int fd = 3; fd < int(mx); ++fd)
            ::close(fd);
        ::execl(exe.c_str(), kComm, "--card", card.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    return true;
}

} // namespace misterplex::owenb_idle_card
