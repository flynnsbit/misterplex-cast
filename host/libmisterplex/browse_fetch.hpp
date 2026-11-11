#pragma once
// MPX_BROWSE_V2: one HTTP GET for the mpx-browse worker that a newer input can cancel.
// Same curl flags and headers as plexHttpGet (plex_resolve.cpp), but curl is spawned with a
// pid this process owns: its stdout is read with poll() in 20 ms slices, and a cancel or the
// deadline kills it at once (SIGKILL, then reaped) instead of waiting out --max-time.
// The URL goes to curl's argv only; callers log urlForLog() (no query, so no token).

#include "libmisterplex/tls_policy.hpp"
#include "libmisterplex/curl_argv_token.hpp"
#if defined(MPX_HAVE_GCAST_TLS)
#include "tls_http.hpp"
#include "wall_clock.hpp"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
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
    bool tlsFailed = false;  // verification required and no usable CA, or curl rejected the cert
    bool tlsNoCa = false;    // tlsFailed because no CA bundle was found
    int retryAfterSec = -1;  // Retry-After when the server sent one, else -1
    bool tooLarge = false;   // the response was larger than bodyCap

    bool ok() const {
        return !cancelled && !timedOut && !tooLarge && status >= 200 && status < 300;
    }
    // Nothing answered: the helper is down or off the network (vs an HTTP error from it).
    // An over-cap body is its own error: the peer did answer.
    bool unreachable() const {
        return !cancelled && !tooLarge && (timedOut || spawnFailed || status == 0);
    }
};

// Shared default for rc3 direct-PMS browse, which fetches an unpaged
// /library/sections/<k>/all. A 1,500-title library is a few megabytes.
// plexui callers pass a smaller per-call cap (kPlexUiHttpBodyCap).
constexpr std::size_t kBrowseHttpBodyCap = 32u * 1024u * 1024u;

// #332: browse PMS URLs never carry X-Plex-Token=. The token is an extra header.
inline std::string browsePmsUrl(const std::string& base, const std::string& path, const std::string& token) {
    std::string url = base;
    if (!path.empty() && path[0] == '/')
        url += path;
    else if (!path.empty())
        url += "/" + path;
#ifdef MUT_332_TOKEN_QUERY
    if (!token.empty())
        url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("X-Plex-Token=") + token;
#else
    (void)token;
#endif
    return url;
}
// curl -w appends "\n" and the status code. That trailer is not part of the body.
constexpr std::size_t kBrowseHttpTrailerSlack = 16;

inline bool browseAppendCapped(std::string& out, const char* data, std::size_t n,
                               std::size_t cap = kBrowseHttpBodyCap) {
    if (n > cap || out.size() > cap - n)
        return false;
    if (n > 0 && data)
        out.append(data, n);
    return true;
}

// curl 35 is a handshake or connect failure, not a certificate check.
inline bool browseCurlTlsHandshake(int curlExit) {
    return curlExit == 35;
}

// curl exits that mean the peer certificate was rejected or the CA file was unusable.
inline bool browseCurlTlsRejected(int curlExit) {
    return curlExit == 51 || curlExit == 58 || curlExit == 60 || curlExit == 77;
}

inline std::vector<std::string> browseHttpCurlPrefix(const char* curl, bool followRedirects) {
    // -q first: curl reads no ~/.curlrc (a "url =" / "next" / "output =" line there is another request or a file write).
#ifdef MUT_BROWSE_NO_Q   // test-only mutant: -q dropped
    std::vector<std::string> args = {curl && *curl ? curl : "curl", "-sS", "-g"};
#else
    std::vector<std::string> args = {curl && *curl ? curl : "curl", "-q", "-sS", "-g"};
#endif
    if (followRedirects)
        args.push_back("-L");
    args.push_back("--http1.1");
    return args;
}

inline std::string browseSeconds(int ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", (ms > 0 ? ms : 1) / 1000.0);
    return b;
}

// connectMs: curl --connect-timeout; timeoutMs: the whole request (curl --max-time, and this
// loop's own deadline, which kills curl).
// tipA-272-on-fu10 (FU9 review merge note 1): the browse curl pipes get the runArgvPipe
// semantics: O_CLOEXEC, moved to fd >= 10. A concurrent spawn elsewhere in the daemon must
// not inherit the -K config write end (curl would never see EOF and hang).
inline int browseSpawnPipe(int fds[2]) {
#ifdef MUT_FU10_BROWSE_FD_LEAK   // test-only mutant: plain pipe, inherited by every spawn
    return ::pipe(fds);
#else
    int raw[2];
    if (::pipe2(raw, O_CLOEXEC) != 0)
        return -1;
    const int a = ::fcntl(raw[0], F_DUPFD_CLOEXEC, 10);
    const int b = ::fcntl(raw[1], F_DUPFD_CLOEXEC, 10);
    ::close(raw[0]);
    ::close(raw[1]);
    if (a < 0 || b < 0) {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
        return -1;
    }
    fds[0] = a;
    fds[1] = b;
    return 0;
#endif
}

inline BrowseHttpResult browseHttpGetOnce(
    const std::string& url, int connectMs, int timeoutMs, const BrowseCancelFn& cancelled,
    const char* curl, const char* accept, const char* caBundle, bool followRedirects,
    std::size_t bodyCap, std::string* redirectOut,
    const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}) {
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
    if (browseSpawnPipe(fds) != 0) {
        r.spawnFailed = true;
        return r;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    const std::string connectS = browseSeconds(std::min(connectMs, timeoutMs));
    const std::string totalS = browseSeconds(timeoutMs);
    [[maybe_unused]] const bool https = url.rfind("https://", 0) == 0;
#if defined(MPX_HAVE_GCAST_TLS)
    // curl cannot skip only the certificate time check. After the rescue, a clock
    // that is still implausible uses the OpenSSL client. A sane clock stays on
    // curl so a newer browse input can still kill the transfer.
    if (https && !wallTimePlausible(std::time(nullptr))) {
        std::string clockErr;
        (void)waitForWallClockRescue(kWallClockRescueWaitMs, &clockErr);
        if (!wallTimePlausible(std::time(nullptr))) {
            TlsVerifiedRequest q;
            q.url = url;
            q.followRedirects = followRedirects;
            q.timeoutSec = timeoutMs > 0 ? (timeoutMs + 999) / 1000 : 1;
            if (caBundle && *caBundle)
                q.caFile = caBundle;
            q.headers = {{"Accept", accept ? accept : "*/*"},
                         {"X-Plex-Product", "MiSTerPlex"},
                         {"X-Plex-Provides", "player"},
                         {"X-Plex-Client-Identifier", "misterplex-browse"}};
            const TlsVerifiedResult got = tlsVerifiedGet(q);
            ::close(fds[0]);
            ::close(fds[1]);
            r.status = got.status;
            r.body = got.body;
            r.elapsedMs = elapsed();
            if (got.status == 0)
                r.spawnFailed = true;
            return r;
        }
    }
    const TlsCurlPlan plan = https ? planTlsCurl(url) : TlsCurlPlan{};
    std::string caStore;
    std::string resolveArg;
    if (https && !plan.optOut) {
        const std::string chosen = (caBundle && *caBundle) ? caBundle : plan.caFile;
        caStore = materializeTrustBundle(chosen, plan.extraTrust);
        if (caStore.empty()) {
            ::close(fds[0]);
            ::close(fds[1]);
            r.tlsFailed = true;
            r.tlsNoCa = true;
            r.elapsedMs = elapsed();
            return r;
        }
        if (!plan.resolveAddr.empty())
            resolveArg = plan.resolveHost + ":" + std::to_string(plan.resolvePort) + ":" + plan.resolveAddr;
    }
#endif
    std::vector<std::string> args = browseHttpCurlPrefix(curl, followRedirects);
    // Set in either TLS branch. The handshake check below is outside the
    // branch, and the ship daemon defines MPX_HAVE_GCAST_TLS.
    bool verifyTls = false;
#if defined(MPX_HAVE_GCAST_TLS)
    if (https && plan.optOut) {
        logTlsOptOutOnce();
        args.push_back("-k");
    } else if (https) {
        verifyTls = true;
        args.insert(args.end(), {"--cacert", caStore});
        if (!resolveArg.empty())
            args.insert(args.end(), {"--resolve", resolveArg});
    } else {
        args.push_back("-k");
    }
#else
    verifyTls = caBundle && *caBundle;
    if (verifyTls)
        args.insert(args.end(), {"--cacert", caBundle});
    else
        args.push_back("-k");
#endif
    args.insert(args.end(),
                {"--connect-timeout", connectS, "--max-time", totalS, "-H",
                 std::string("Accept: ") + accept, "-H", "X-Plex-Product: MiSTerPlex", "-H",
                 "X-Plex-Provides: player", "-H", "X-Plex-Client-Identifier: misterplex-browse",
                 "-o", "-", "-w", "\n%{http_code}"});
#ifndef MUT_BROWSE_NO_DASHDASH   // test-only mutant: "--" dropped; a url that starts with "-" is an option
    args.push_back("--");
#endif
    args.push_back(url);
    // #332: extra X-Plex-Token is a header (never the URL). #272: a leftover URL query
    // token still moves to -K stdin.
    std::string tokCfg;
    for (const auto& h : extraHeaders) {
        if (h.second.find('\n') != std::string::npos || h.second.find('\r') != std::string::npos)
            continue;
        tokCfg += "header = \"" + h.first + ": " + h.second + "\"\n";
    }
    mpx::scrubPlexTokenArgv(args, tokCfg);
    if (redirectOut) {   // #272 N1: report the Location; the caller follows it (same-origin token only)
        for (auto& x : args)
            if (x == "\n%{http_code}") x = "\n%{redirect_url}\n%{http_code}";
    }
    int kin[2] = {-1, -1};
    if (!tokCfg.empty() && browseSpawnPipe(kin) != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.spawnFailed = true;
        r.elapsedMs = elapsed();
        return r;
    }
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(&a[0]);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (kin[0] >= 0) {
        posix_spawn_file_actions_adddup2(&fa, kin[0], 0);
        posix_spawn_file_actions_addclose(&fa, kin[0]);
        posix_spawn_file_actions_addclose(&fa, kin[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = -1;
    const int sp = ::posix_spawnp(&pid, curl, &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (kin[0] >= 0) {
        ::close(kin[0]);
        if (sp == 0) {   // small config (< PIPE_BUF): one write, no deadlock
            const char* p = tokCfg.data();
            size_t left = tokCfg.size();
            while (left > 0) {
                const ssize_t w = ::write(kin[1], p, left);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) break;
                p += w;
                left -= static_cast<size_t>(w);
            }
        }
        ::close(kin[1]);
    }
    if (sp != 0) {
        ::close(fds[0]);
        r.spawnFailed = true;
        r.elapsedMs = elapsed();
        return r;
    }
    std::string out;
    bool eof = false;
    bool overflow = false;
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
                const std::size_t readCap =
                    bodyCap > static_cast<std::size_t>(-1) - kBrowseHttpTrailerSlack
                        ? bodyCap
                        : bodyCap + kBrowseHttpTrailerSlack;
                if (!browseAppendCapped(out, buf, static_cast<size_t>(n), readCap)) {
                    overflow = true;
                    break;
                }
                continue;
            }
            if (n == 0)
                eof = true;
            break; // EAGAIN: wait for more
        }
        if (overflow)
            break;
    }
    ::close(fds[0]);
    int st = 0;
    if (!eof)
        ::kill(pid, SIGKILL);
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    r.elapsedMs = elapsed();
    if (!eof) {
        // The read stopped because the body, plus curl's status trailer, passed the cap.
        if (overflow && !r.cancelled && !r.timedOut) {
            r.tooLarge = true;
            std::fprintf(stderr,
                         "browseHttpGet: body over cap (read stopped at %zu, cap %zu)\n",
                         out.size(), bodyCap);
        }
        return r;
    }
    r.curlExit = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    const size_t nl = out.rfind('\n');
    if (nl != std::string::npos) {
        r.status = std::atoi(out.c_str() + nl + 1);
        out.resize(nl);
    }
    if (redirectOut) {
        const size_t nl2 = out.rfind('\n');
        if (nl2 != std::string::npos) {
            *redirectOut = out.substr(nl2 + 1);
            out.resize(nl2);
        }
    }
    if (out.size() > bodyCap) {
        r.tooLarge = true;
        r.status = 0;
        std::fprintf(stderr, "browseHttpGet: body over cap bytes=%zu cap=%zu\n", out.size(),
                     bodyCap);
        return r;
    }
    if (r.curlExit == 28)
        r.timedOut = true;
    if (verifyTls && (browseCurlTlsHandshake(r.curlExit) || browseCurlTlsRejected(r.curlExit)))
        r.tlsFailed = true;
    if (r.curlExit != 0 && r.status >= 200 && r.status < 300)
        r.status = 0; // a cut transfer is no answer
    r.body.swap(out);
    return r;
}

// #272 N1: a URL token is sent as a -K header, so curl -L could carry it to another host.
// With a token, redirects are followed here: up to 5 hops, the token re-attached only for
// the PMS origin (browseRedirectNext). No token: one curl with -L as before.
inline BrowseHttpResult browseHttpGet(
    const std::string& url, int connectMs, int timeoutMs, const BrowseCancelFn& cancelled,
    const char* curl = "curl", const char* accept = "application/xml", const char* caBundle = nullptr,
    bool followRedirects = true, std::size_t bodyCap = kBrowseHttpBodyCap,
    const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}) {
    std::string probe = url;
    bool hasTok = false;
    const std::string token = mpx::curlStripTokenQuery(probe, &hasTok);
    for (const auto& h : extraHeaders) {
        if (h.first.size() == 12 && mpx::curlTokIeq(h.first, 0, "x-plex-token"))
            hasTok = true;
    }
#ifdef MUT_272_NO_REDIRECT   // test-only mutant: the 20771af behaviour, a 30x is not followed
    followRedirects = followRedirects && !hasTok;
#endif
    if (!followRedirects || !hasTok)
        return browseHttpGetOnce(url, connectMs, timeoutMs, cancelled, curl, accept, caBundle,
                                 followRedirects, bodyCap, nullptr, extraHeaders);
    const std::string origin = mpx::curlUrlOrigin(url);
    std::string cur = url;
    BrowseHttpResult r;
    for (int hop = 0; hop <= 5; ++hop) {
        std::string loc;
        r = browseHttpGetOnce(cur, connectMs, timeoutMs, cancelled, curl, accept, caBundle, false,
                              bodyCap, &loc, extraHeaders);
        const bool redirect = r.status == 301 || r.status == 302 || r.status == 303 ||
                              r.status == 307 || r.status == 308;
        if (!redirect || loc.empty() || hop == 5)
            break;
        const std::string next = mpx::browseRedirectNext(origin, loc, token);
        if (next.empty())
            break;
        cur = next;
    }
    return r;
}

} // namespace misterplex
