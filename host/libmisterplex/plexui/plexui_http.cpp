#include "libmisterplex/header_int.hpp"
#include "plexui_http.hpp"
#include "plexui_curl_cfg.hpp"

#include "ca_bundle.hpp"
#include "libmisterplex/browse_fetch.hpp"
#include "libmisterplex/tls_policy.hpp"
#include "libmisterplex/curl_argv_token.hpp"

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace misterplex {
namespace plexui {

namespace {

ProbeLogFn gProbeLog;

} // namespace

void setPlexuiProbeLog(ProbeLogFn log) { gProbeLog = std::move(log); }

void plexuiProbeLogLine(const std::string& line) {
    if (gProbeLog)
        gProbeLog(line);
}

std::string plexUiCaBundlePath() {
    return tlsCaBundlePath();
}

namespace {

bool insecureTlsOptIn() {
    const char* raw = std::getenv(kPlexUiInsecureTlsEnv);
    if (!raw)
        return false;
    const std::string v(raw);
    return v == "1" || v == "true" || v == "TRUE";
}

int headerInt(const std::string& headers, const char* key) {
    const std::string want(key);
    size_t i = 0;
    while (i < headers.size()) {
        size_t end = headers.find('\n', i);
        if (end == std::string::npos)
            end = headers.size();
        std::string line = headers.substr(i, end - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() >= want.size() &&
            std::equal(want.begin(), want.end(), line.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            size_t v = want.size();
            while (v < line.size() && std::isspace(static_cast<unsigned char>(line[v])))
                ++v;
            return parseHeaderNonNegInt(line.c_str() + v);
        }
        i = end + 1;
    }
    return -1;
}

} // namespace

PlexUiTlsPlan plexUiTlsPlanFrom(bool plexHost, bool insecureOptIn, const std::string& bundle,
                                const std::string& systemCa) {
    PlexUiTlsPlan plan;
    if (plexHost && insecureOptIn) {
        plan.insecure = true;
        return plan;
    }
    if (!bundle.empty()) {
        plan.ca = bundle;
        return plan;
    }
    if (plexHost && !systemCa.empty()) {
        plan.ca = systemCa;
        return plan;
    }
    if (plexHost) {
        plan.failClosed = true;
        return plan;
    }
    plan.insecure = true;
    return plan;
}

PlexUiTlsPlan plexUiTlsPlan(const std::string& url) {
    const std::string host = httpUrlHost(url);
    const bool plex = host == "plex.tv" ||
                      (host.size() > 8 && host.compare(host.size() - 8, 8, ".plex.tv") == 0);
    // plex.tv uses MPX_CA_BUNDLE when set, otherwise mpx::caBundlePath(), and fail-closes
    // if neither exists. PMS GET and POST verify when MPX_CA_BUNDLE is set.
    return plexUiTlsPlanFrom(plex, insecureTlsOptIn(), plexUiCaBundlePath(), mpx::caBundlePath());
}

CurlDump parseCurlDump(const std::string& raw) {
    CurlDump out;
    std::string msg = raw;
    const size_t trailer = msg.rfind('\n');
    if (trailer != std::string::npos) {
        out.status = std::atoi(msg.c_str() + trailer + 1);
        msg.resize(trailer);
    }
    size_t bodyAt = std::string::npos;
    std::string headers;
    const size_t crlf = msg.find("\r\n\r\n");
    if (crlf != std::string::npos) {
        headers = msg.substr(0, crlf);
        bodyAt = crlf + 4;
    } else {
        const size_t lf = msg.find("\n\n");
        if (lf != std::string::npos) {
            headers = msg.substr(0, lf);
            bodyAt = lf + 2;
        }
    }
    if (bodyAt != std::string::npos)
        out.body = msg.substr(bodyAt);
    else
        out.body = std::move(msg);
    if (!headers.empty())
        out.retryAfterSec = headerInt(headers, "retry-after:");
    return out;
}

std::string httpUrlHost(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return {};
    size_t i = scheme + 3;
    size_t end = url.find('/', i);
    if (end == std::string::npos)
        end = url.size();
    std::string hostport = url.substr(i, end - i);
    const size_t at = hostport.rfind('@');
    if (at != std::string::npos)
        hostport = hostport.substr(at + 1);
    const size_t colon = hostport.find(':');
    if (colon == std::string::npos)
        return hostport;
    return hostport.substr(0, colon);
}

HttpGetFn makePlexuiHttpGet() {
    return [](const std::string& url, int connectMs, int timeoutMs, const BrowseCancelFn& cancel) {
        const std::string host = httpUrlHost(url);
        const bool plex = host == "plex.tv" ||
                          (host.size() > 8 && host.compare(host.size() - 8, 8, ".plex.tv") == 0);
        const PlexUiPreparedGet prep = plexUiPrepareGet(plex, plexUiTlsPlan(url));
        if (prep.skip) {
            BrowseHttpResult r;
            r.tlsFailed = true;
            r.tlsNoCa = prep.tlsNoCa;
            return r;
        }
        return browseHttpGet(url, connectMs, timeoutMs, cancel, "curl", "application/xml",
                             prep.insecure ? nullptr : prep.ca.c_str(), prep.followRedirects,
                             kPlexUiHttpBodyCap);
    };
}

namespace {

static void plexuiWriteAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n > 0)
            off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
}

// POST/PUT: request body on curl stdin; token headers via -K /dev/fd/N (stdin already used).
BrowseHttpResult plexuiHttpPost(const std::string& url, const std::string& contentType,
                                const std::string& body,
                                const std::vector<std::pair<std::string, std::string>>& headers,
                                int connectMs, int timeoutMs, const BrowseCancelFn& cancelled,
                                const std::string& ca, const char* method = "POST") {
    BrowseHttpResult r;
    if (cancelled && cancelled()) {
        r.cancelled = true;
        return r;
    }
    int outfd[2] = {-1, -1};
    int infd[2] = {-1, -1};
    if (::pipe(outfd) != 0 || ::pipe(infd) != 0) {
        if (outfd[0] >= 0) {
            ::close(outfd[0]);
            ::close(outfd[1]);
        }
        r.spawnFailed = true;
        return r;
    }
    ::fcntl(outfd[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(outfd[0], F_SETFL, ::fcntl(outfd[0], F_GETFL) | O_NONBLOCK);
    const PlexUiTlsPlan tls = plexUiTlsPlan(url);
    if (tls.failClosed) {
        ::close(outfd[0]);
        ::close(outfd[1]);
        ::close(infd[0]);
        ::close(infd[1]);
        r.tlsFailed = true;
        r.tlsNoCa = true;
        return r;
    }
    (void)ca;
    const std::string connectS = browseSeconds(std::min(connectMs, timeoutMs));
    const std::string totalS = browseSeconds(timeoutMs);
    const bool put = method != nullptr && std::strcmp(method, "PUT") == 0;
    std::vector<std::string> args = plexuiCurlPrefix(method != nullptr ? method : "POST");
    if (tls.insecure)
        args.push_back("-k");
    else
        args.insert(args.end(), {"--cacert", tls.ca});
    bool callerAccept = false;
    bool callerProduct = false;
    for (const auto& h : headers) {
        if (h.first == "Accept")
            callerAccept = true;
        if (h.first == "X-Plex-Product")
            callerProduct = true;
    }
    args.insert(args.end(), {"--connect-timeout", connectS, "--max-time", totalS});
    if (!callerAccept)
        args.insert(args.end(), {"-H", "Accept: application/xml"});
    if (!callerProduct)
        args.insert(args.end(), {"-H", "X-Plex-Product: MiSTerPlex"});
    // Empty content type and an empty body must not attach a form body. POST /playQueues
    // is a query string; PMS returns HTTP 400 when type/continuous/uri are a form body.
    // A PUT that names a content type and sends an empty body still uses --data-binary
    // so curl emits Content-Length: 0. That is the companion queue-add PUT. It has no
    // form fields. Play-queue create and update pass an empty content type and skip it.
    if (plexuiHttpSendsBody(put, contentType, body) ||
        (put && body.empty() && !contentType.empty()))
        args.insert(args.end(), {"-H", "Content-Type: " + contentType, "--data-binary", "@-"});
    // #272: a token in the URL query rides the -K config, never argv.
    std::string url272 = url;
    auto headers272 = headers;
    mpx::moveUrlTokenToHeaders(url272, headers272);
    const std::string hdrCfg = plexuiCurlHeaderConfig(headers272);
    int kfds[2] = {-1, -1};
    std::string kPath;
    if (!hdrCfg.empty()) {
        if (::pipe(kfds) != 0) {
            ::close(outfd[0]);
            ::close(outfd[1]);
            ::close(infd[0]);
            ::close(infd[1]);
            r.spawnFailed = true;
            return r;
        }
        ::fcntl(kfds[1], F_SETFD, FD_CLOEXEC);
        kPath = "/dev/fd/" + std::to_string(kfds[0]);
        args.insert(args.end(), {"-K", kPath});
    }
    args.insert(args.end(), {"-o", "-", "-w", "\n%{http_code}"});
#ifndef MUT_PLEXUI_NO_DASHDASH   // test-only mutant: "--" dropped
    args.push_back("--");
#endif
    args.push_back(url272);
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(&a[0]);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, infd[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outfd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, outfd[0]);
    posix_spawn_file_actions_addclose(&fa, outfd[1]);
    posix_spawn_file_actions_addclose(&fa, infd[0]);
    posix_spawn_file_actions_addclose(&fa, infd[1]);
    if (kfds[1] >= 0)
        posix_spawn_file_actions_addclose(&fa, kfds[1]);
    pid_t pid = -1;
    const int sp = ::posix_spawnp(&pid, "curl", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(outfd[1]);
    ::close(infd[0]);
    if (sp != 0) {
        ::close(outfd[0]);
        ::close(infd[1]);
        if (kfds[0] >= 0) {
            ::close(kfds[0]);
            ::close(kfds[1]);
        }
        r.spawnFailed = true;
        return r;
    }
    if (kfds[0] >= 0) {
        plexuiWriteAll(kfds[1], hdrCfg);
        ::close(kfds[1]);
        ::close(kfds[0]);
    }
    plexuiWriteAll(infd[1], body);
    ::close(infd[1]);
    std::string out;
    bool eof = false;
    bool overflow = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count());
    };
    while (!eof) {
        if (cancelled && cancelled()) {
            r.cancelled = true;
            break;
        }
        if (elapsed() >= timeoutMs + 250) {
            r.timedOut = true;
            break;
        }
        pollfd p{outfd[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, 20);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(outfd[0], buf, sizeof buf);
            if (n > 0) {
                if (!appendCapped(out, buf, static_cast<size_t>(n), kPlexUiHttpBodyCap)) {
                    overflow = true;
                    break;
                }
                continue;
            }
            if (n == 0)
                eof = true;
            break;
        }
        if (overflow)
            break;
    }
    ::close(outfd[0]);
    int st = 0;
    if (!eof)
        ::kill(pid, SIGKILL);
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
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
    if (!tls.insecure && (browseCurlTlsHandshake(r.curlExit) || browseCurlTlsRejected(r.curlExit)))
        r.tlsFailed = true;
    if (r.curlExit != 0 && r.status >= 200 && r.status < 300)
        r.status = 0;
    r.body.swap(out);
    return r;
}

} // namespace

BrowseHttpResult plexuiHttpHeaderGet(const std::string& url,
                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                     int connectMs, int timeoutMs, const BrowseCancelFn& cancelled,
                                     const std::string& ca) {
    BrowseHttpResult r;
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
    const PlexUiTlsPlan tls = plexUiTlsPlan(url);
    if (tls.failClosed) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.tlsFailed = true;
        r.tlsNoCa = true;
        return r;
    }
    (void)ca;
    const std::string connectS = browseSeconds(std::min(connectMs, timeoutMs));
    const std::string totalS = browseSeconds(timeoutMs);
    // No -L, and headers are dumped so Retry-After can be read without a second request.
#ifdef MUT_PLEXUI_NO_Q   // test-only mutant: -q dropped
    std::vector<std::string> args = {"curl", "-sS", "-g", "--http1.1", "-D", "-"};
#else
    std::vector<std::string> args = {"curl", "-q", "-sS", "-g", "--http1.1", "-D", "-"};   // -q first: no ~/.curlrc
#endif
    if (tls.insecure)
        args.push_back("-k");
    else
        args.insert(args.end(), {"--cacert", tls.ca});
    args.insert(args.end(), {"--connect-timeout", connectS, "--max-time", totalS});
    // #272: a token in the URL query rides the -K config, never argv.
    std::string url272 = url;
    auto headers272 = headers;
    mpx::moveUrlTokenToHeaders(url272, headers272);
    const std::string hdrCfg = plexuiCurlHeaderConfig(headers272);
    int ifds[2] = {-1, -1};
    if (!hdrCfg.empty()) {
        if (::pipe(ifds) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            r.spawnFailed = true;
            return r;
        }
        args.insert(args.end(), {"-K", "-"});
    }
    args.insert(args.end(), {"-o", "-", "-w", "\n%{http_code}"});
#ifndef MUT_PLEXUI_NO_DASHDASH   // test-only mutant: "--" dropped
    args.push_back("--");
#endif
    args.push_back(url272);
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(&a[0]);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (ifds[0] >= 0) {
        posix_spawn_file_actions_adddup2(&fa, ifds[0], 0);
        posix_spawn_file_actions_addclose(&fa, ifds[0]);
        posix_spawn_file_actions_addclose(&fa, ifds[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = -1;
    const int sp = ::posix_spawnp(&pid, "curl", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (ifds[0] >= 0)
        ::close(ifds[0]);
    if (sp != 0) {
        ::close(fds[0]);
        if (ifds[1] >= 0)
            ::close(ifds[1]);
        r.spawnFailed = true;
        return r;
    }
    if (ifds[1] >= 0) {
        plexuiWriteAll(ifds[1], hdrCfg);
        ::close(ifds[1]);
    }
    std::string out;
    bool eof = false;
    bool overflow = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count());
    };
    while (!eof) {
        if (cancelled && cancelled()) {
            r.cancelled = true;
            break;
        }
        if (elapsed() >= timeoutMs + 250) {
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
                if (!appendCapped(out, buf, static_cast<size_t>(n), kPlexUiHttpBodyCap)) {
                    overflow = true;
                    break;
                }
                continue;
            }
            if (n == 0)
                eof = true;
            break;
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
    if (!eof)
        return r;
    r.curlExit = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    const CurlDump dumped = parseCurlDump(out);
    r.status = dumped.status;
    r.retryAfterSec = dumped.retryAfterSec;
    r.body = std::move(dumped.body);
    if (r.curlExit == 28)
        r.timedOut = true;
    if (!tls.insecure && (browseCurlTlsHandshake(r.curlExit) || browseCurlTlsRejected(r.curlExit)))
        r.tlsFailed = true;
    if (r.curlExit != 0 && r.status >= 200 && r.status < 300)
        r.status = 0;
    return r;
}

HttpHeaderGetFn makePlexuiHeaderGet() {
    const std::string ca = plexUiCaBundlePath();
    return [ca](const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                int connectMs, int timeoutMs, const BrowseCancelFn& cancel) {
        return plexuiHttpHeaderGet(url, headers, connectMs, timeoutMs, cancel, ca);
    };
}

HttpPostFn makePlexuiHttpPost() {
    const std::string ca = plexUiCaBundlePath();
    return [ca](const std::string& url, const std::string& contentType, const std::string& body,
                const std::vector<std::pair<std::string, std::string>>& headers, int connectMs,
                int timeoutMs, const BrowseCancelFn& cancel) {
        return plexuiHttpPost(url, contentType, body, headers, connectMs, timeoutMs, cancel, ca, "POST");
    };
}

HttpPostFn makePlexuiHttpPut() {
    const std::string ca = plexUiCaBundlePath();
    return [ca](const std::string& url, const std::string& contentType, const std::string& body,
                const std::vector<std::pair<std::string, std::string>>& headers, int connectMs,
                int timeoutMs, const BrowseCancelFn& cancel) {
        return plexuiHttpPost(url, contentType, body, headers, connectMs, timeoutMs, cancel, ca, "PUT");
    };
}

HttpGetFn makePlexuiImageHttpGet() {
    const std::string ca = plexUiCaBundlePath();
    return [ca](const std::string& url, int connectMs, int timeoutMs,
                const BrowseCancelFn& cancel) {
        return browseHttpGet(url, connectMs, timeoutMs, cancel, "curl", "*/*",
                             ca.empty() ? nullptr : ca.c_str(), true, kPlexUiHttpBodyCap);
    };
}

} // namespace plexui
} // namespace misterplex
