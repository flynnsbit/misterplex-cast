#pragma once
// MPX_BROWSE_V2: one HTTP GET for the mpx-browse worker that a newer input can cancel.
// Same curl flags and headers as plexHttpGet (plex_resolve.cpp), but curl is spawned with a
// pid this process owns: its stdout is read with poll() in 20 ms slices, and a cancel or the
// deadline kills it at once (SIGKILL, then reaped) instead of waiting out --max-time.
// The URL goes to curl's argv only; callers log urlForLog() (no query, so no token).

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace misterplex {

using BrowseCancelFn = std::function<bool()>;

struct BrowseHttpResult {
    int status = 0;          // HTTP status; 0 = no response
    std::string body;
    bool cancelled = false;  // a newer input (or shutdown) stopped it
    bool timedOut = false;   // the caller's deadline passed
    bool spawnFailed = false;
    int curlExit = -1;       // 0 ok; 6 resolve, 7 connect, 28 curl timeout, 52/56 no reply
    int64_t elapsedMs = 0;

    bool ok() const { return !cancelled && !timedOut && status >= 200 && status < 300; }
    // Nothing answered: the helper is down or off the network (vs an HTTP error from it).
    bool unreachable() const {
        return !cancelled && (timedOut || spawnFailed || status == 0);
    }
};

inline std::string browseSeconds(int ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", (ms > 0 ? ms : 1) / 1000.0);
    return b;
}

// connectMs: curl --connect-timeout; timeoutMs: the whole request (curl --max-time, and this
// loop's own deadline, which kills curl).
inline BrowseHttpResult browseHttpGet(const std::string& url, int connectMs, int timeoutMs,
                                      const BrowseCancelFn& cancelled,
                                      const char* curl = "curl",
                                      const char* accept = "application/xml",
                                      const char* caBundle = nullptr) {
    BrowseHttpResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count());
    };
    if (cancelled && cancelled()) {
        r.cancelled = true;
        return r;
    }
    int fds[2];
    if (::pipe(fds) != 0) {
        r.spawnFailed = true;
        return r;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    const std::string connectS = browseSeconds(std::min(connectMs, timeoutMs));
    const std::string totalS = browseSeconds(timeoutMs);
    const bool verifyTls = caBundle && *caBundle;
    std::vector<std::string> args = {curl, "-sS", "-g", "-L", "--http1.1"};
    if (verifyTls)
        args.insert(args.end(), {"--cacert", caBundle});
    else
        args.push_back("-k");
    args.insert(args.end(),
                {"--connect-timeout", connectS, "--max-time", totalS, "-H",
                 std::string("Accept: ") + accept, "-H", "X-Plex-Product: MiSTerPlex", "-H",
                 "X-Plex-Provides: player", "-H", "X-Plex-Client-Identifier: misterplex-browse",
                 "-o", "-", "-w", "\n%{http_code}", url});
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(&a[0]);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = -1;
    const int sp = ::posix_spawnp(&pid, curl, &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (sp != 0) {
        ::close(fds[0]);
        r.spawnFailed = true;
        r.elapsedMs = elapsed();
        return r;
    }
    std::string out;
    bool eof = false;
    while (!eof) {
        if (cancelled && cancelled()) {
            r.cancelled = true;
            break;
        }
        if (elapsed() >= timeoutMs + 250) { // curl's own --max-time normally ends it first
            r.timedOut = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, 20);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0)
                eof = true;
            break; // EAGAIN: wait for more
        }
    }
    ::close(fds[0]);
    int st = 0;
    if (!eof)
        ::kill(pid, SIGKILL);
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    r.elapsedMs = elapsed();
    if (!eof)
        return r;
    r.curlExit = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    const size_t nl = out.rfind('\n');
    if (nl != std::string::npos) {
        r.status = std::atoi(out.c_str() + nl + 1);
        out.resize(nl);
    }
    if (r.curlExit == 28)
        r.timedOut = true;
    if (r.curlExit != 0 && r.status >= 200 && r.status < 300)
        r.status = 0; // a cut transfer is no answer
    r.body.swap(out);
    return r;
}

} // namespace misterplex
