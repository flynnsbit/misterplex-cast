#include "plex_resolve.hpp"
#include "remote_start_probe.hpp"
#include "libmisterplex/track_select.hpp"
#include "libmisterplex/embedded_text_part.hpp"
#include "libmisterplex/opensubs_fallback.hpp"
#include "libmisterplex/remote_h264_transcode.hpp"
#include "libmisterplex/ob_decode_envelope.hpp"
#include "log_redact.hpp"
#include "libmisterplex/burn_subs_fix.hpp"
#include "libmisterplex/remember_tracks.hpp"
#include "libmisterplex/scrub_thumbs.hpp"
#include "ca_bundle.hpp"
#include "libmisterplex/tls_policy.hpp"
#if defined(MPX_HAVE_GCAST_TLS)
#include "tls_http.hpp"
#endif
#include "queue_step.hpp"
#include "url_query_codec.hpp"
#include "../../assets/plex-profiles/fpga_profile.hpp"
#include "libmisterplex/p720_transcode_vf.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/core_id_select.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <iomanip>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <netdb.h>
#include <numeric>
#include <optional>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "libmisterplex/curl_argv_token.hpp"

extern char** environ;

namespace misterplex {
namespace {

std::atomic<uintptr_t> metadataFetchHook{0};
std::atomic<uintptr_t> audioPartPutHook{0};

// ---- no-shell GET (plexHttpGet) ----
bool headerNameIsToken(const std::string& n) {
    if (n.empty())
        return false;
    for (unsigned char c : n) {
        if (std::isalnum(c) || std::strchr("!#$%&'*+-.^_`|~", c))
            continue;
        return false;   // ':' ';' ' ' '@' controls and anything else
    }
    return true;
}

bool headerValueIsClean(const std::string& v) {
    return v.find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
}

[[maybe_unused]] bool headersHavePlexToken(const std::vector<std::pair<std::string, std::string>>& h) {
    for (const auto& kv : h) {
        std::string low;
        for (unsigned char c : kv.first)
            low.push_back(static_cast<char>(std::tolower(c)));
        if (low == "x-plex-token")
            return true;
    }
    return false;
}

// TLS choices for a curl child (verify against the Mozilla bundle + TLS_TRUST_FILE, --resolve for plex.direct,
// TLS_VERIFY=0 the only insecure path), one argv item per value.
void appendTlsArgv(std::vector<std::string>& a, const std::string& url) {
    if (url.compare(0, 8, "https://") != 0)
        return;
    if (tlsVerifyOff()) {
        logTlsOptOutOnce();
        a.push_back("--insecure");
        return;
    }
    const TlsCurlPlan p = planTlsCurl(url);
    if (!p.https)
        return;
    if (p.optOut) {
        logTlsOptOutOnce();
        a.push_back("--insecure");
        return;
    }
    const std::string ca = materializeTrustBundle(p.caFile, p.extraTrust);
    if (!ca.empty())
        a.insert(a.end(), {"--cacert", ca});
    if (!p.resolveAddr.empty())
        a.insert(a.end(), {"--resolve", p.resolveHost + ":" + std::to_string(p.resolvePort) + ":" + p.resolveAddr});
}

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// argv prefix for every curl the daemon starts: "-q" FIRST (curl reads no ~/.curlrc / CURL_HOME rc file),
// -g, no -L when an X-Plex-Token header is sent (the token must not follow a redirect), TLS as discrete items.
std::vector<std::string> curlArgvPrefix(const std::string& url, const HeaderList& headers, bool followRedirects) {
    std::vector<std::string> a = {"curl", "-q", "-sS", "-g"};
    // #340: -L stays with a token header too. runArgv never hands curl -L with a token: it
    // follows the hops itself and keeps the token only for the original origin.
    (void)headers;
    if (followRedirects)
        a.push_back("-L");
    appendTlsArgv(a, url);
    a.push_back("--http1.1");
    return a;
}

// curl -K config line for one header. Backslash and double-quote are escaped; CR/LF/NUL (and
// non-token names) are dropped — same filter as the old -H path (MUT_PLEXGET_HDR_UNCHECKED skips it).
std::string curlHeaderConfigText(const HeaderList& headers) {
    std::string out;
    for (const auto& kv : headers) {
#ifndef MUT_PLEXGET_HDR_UNCHECKED
        if (!headerNameIsToken(kv.first) || !headerValueIsClean(kv.second))
            continue;
#endif
        out += "header = \"";
        const std::string line = kv.first + ": " + kv.second;
        for (char c : line) {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        out += "\"\n";
    }
    return out;
}

// Attach request headers for curl. Default: argv gets "-K" "-" and the config text is returned for
// runArgv to feed on stdin (or via /dev/fd/N when stdin already carries a body). Secrets never land
// on argv / ps. MUT_HDR_ON_ARGV restores the old -H argv leak so tests can kill it.
std::string appendHeaderArgv(std::vector<std::string>& a, const HeaderList& headers) {
#ifdef MUT_HDR_ON_ARGV
    for (const auto& kv : headers) {
#ifndef MUT_PLEXGET_HDR_UNCHECKED
        if (!headerNameIsToken(kv.first) || !headerValueIsClean(kv.second))
            continue;
#endif
        a.insert(a.end(), {"-H", kv.first + ": " + kv.second});
    }
    return {};
#else
    const std::string cfg = curlHeaderConfigText(headers);
    if (!cfg.empty())
        a.insert(a.end(), {"-K", "-"});
    return cfg;
#endif
}

// The URL goes last, after "--": a URL that starts with '-' is a URL, never an option.
void appendUrlArgv(std::vector<std::string>& a, const std::string& url) {
#ifndef MUT_PLEXGET_NO_DASHDASH
    a.push_back("--");
#endif
    a.push_back(url);
}

// FU9 (glass C5 wedge): every runArgv pipe is O_CLOEXEC. A plain pipe() leaked this call's stdin write end
// (and stdout read end) into any child another thread spawned meanwhile (the stream curl, a ping). A `curl -K -`
// then waits for EOF on a config pipe that child still holds; --max-time has not started, so the caller hangs.
// posix_spawn dup2 onto 0/1/2/kRunArgvConfigFd clears CLOEXEC on the child's copy. Parent fds sit at >= 10 so a
// dup2 target never aliases its source (dup2(fd, fd) would keep CLOEXEC).
constexpr int kRunArgvConfigFd = 3;
static int runArgvPipe(int fds[2]) {
#ifdef MUT_FU9_FD_LEAK   // test-only mutant: the FU8 pipes, inherited by every concurrent spawn
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
        if (a >= 0)
            ::close(a);
        if (b >= 0)
            ::close(b);
        return -1;
    }
    fds[0] = a;
    fds[1] = b;
    return 0;
#endif
}

// FU9 nit: the token scrub rewrites only a curl argv (basename of argv[0]).
static bool runArgvIsCurl(const std::vector<std::string>& a) {
    if (a.empty())
        return false;
#ifdef MUT_FU9_SCRUB_ANY_ARGV
    return true;
#else
    const auto slash = a[0].rfind('/');
    return (slash == std::string::npos ? a[0] : a[0].substr(slash + 1)) == "curl";
#endif
}

struct ArgvRun {
    bool spawned = false;
    std::string out;
    int exitCode = -1;   // WEXITSTATUS, or -1 when the child did not exit normally
};

static void writeAllFd(int fd, const std::string& data) {
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

// Boot (misterplexd_supervise.sh) starts the daemon with stdin closed or on /dev/null.
// curl -K - would read that fd and send the resources GET with no token. The config
// always goes to /dev/fd/N. The child's own stdin is /dev/null unless stdinData is set.
#ifdef MUT_KCONFIG_ON_STDIN
static bool parentStdinUnusable() {
    struct stat st;
    if (::fstat(0, &st) != 0)
        return true;
    if (!S_ISCHR(st.st_mode))
        return false;
    struct stat ns;
    if (::stat("/dev/null", &ns) != 0)
        return false;
    return st.st_rdev == ns.st_rdev;
}
#endif

// posix_spawnp, no shell. stdin /dev/null; stderr /dev/null, or the pipe when mergeStderr (the old "2>&1").
// cap > 0: stop reading at cap bytes and close the pipe (a still-streaming child ends on EPIPE).
// stdinData non-null: the child's stdin is a pipe fed with it (keeps secrets out of argv / ps). Keep it small (< pipe buf).
// curlKConfig non-null: curl -K config text on /dev/fd/N, never on the child's stdin. Parent stdin
// (closed, /dev/null, or a tty) cannot drop the token. MUT_KCONFIG_ON_STDIN keeps "-K -" and
// feeds nothing when parent stdin is closed or /dev/null, which is the boot failure.
constexpr int kRunArgvHeaderFd = 4;   // FU10 N4: curl -D dump for a followed token request
static ArgvRun runArgvOnce(const std::vector<std::string>& args, bool mergeStderr, size_t cap,
                           const std::string* stdinData, const std::string* curlKConfig,
                           std::string* headerOut) {
    ArgvRun r;
    for (const auto& s : args)
        if (s.find('\0') != std::string::npos)
            return r;
    int fds[2];
    if (args.empty() || runArgvPipe(fds) != 0)
        return r;
    std::vector<std::string> own = args;   // writable, stable storage for argv
    // FU8: the choke point for every curl in this file. A token in a URL query or a
    // -H pair moves into the -K config (stdin or /dev/fd/N), never argv (ps on  lab).
    // FU9 (FPGA Bug nit): only a curl argv is scrubbed; other tools keep their argv as built.
    std::string fu8Cfg = curlKConfig ? *curlKConfig : std::string{};
    if (runArgvIsCurl(own) && mpx::scrubPlexTokenArgv(own, fu8Cfg))
        curlKConfig = &fu8Cfg;
    const std::string* effectiveStdin = stdinData;
    int kfds[2] = {-1, -1};
#ifndef MUT_HDR_ON_ARGV
    if (curlKConfig && !curlKConfig->empty()) {
#ifdef MUT_KCONFIG_ON_STDIN
        // Boot-shaped stdin: -K - reads /dev/null or a closed fd, so the token never leaves.
        const bool dropOnParentStdin = !stdinData && parentStdinUnusable();
#else
        const bool dropOnParentStdin = false;
#endif
        if (!dropOnParentStdin) {
            if (runArgvPipe(kfds) != 0) {
                ::close(fds[0]);
                ::close(fds[1]);
                return r;
            }
            // FU9: the child gets the config read end at a fixed fd (dup2 clears CLOEXEC there);
            // the parent's copy stays CLOEXEC, so no concurrent spawn inherits it. fd 0 is not used.
            const std::string kPath = "/dev/fd/" + std::to_string(kRunArgvConfigFd);
            bool replaced = false;
            for (size_t i = 0; i + 1 < own.size(); ++i) {
                if (own[i] == "-K" && own[i + 1] == "-") {
                    own[i + 1] = kPath;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                own.insert(own.end(), {"-K", kPath});
        }
    }
#else
    (void)curlKConfig;
#endif
    int hfds[2] = {-1, -1};
    if (headerOut) {
        if (runArgvPipe(hfds) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            if (kfds[0] >= 0) {
                ::close(kfds[0]);
                ::close(kfds[1]);
            }
            return r;
        }
        // -q must stay argv[1] (curl reads ~/.curlrc unless -q comes first).
        own.insert(own.begin() + (own.size() > 1 && own[1] == "-q" ? 2 : 1),
                   {"-D", "/dev/fd/" + std::to_string(kRunArgvHeaderFd)});
    }
    int ifds[2] = {-1, -1};
    if (effectiveStdin && runArgvPipe(ifds) != 0) {
        if (hfds[0] >= 0) {
            ::close(hfds[0]);
            ::close(hfds[1]);
        }
        ::close(fds[0]);
        ::close(fds[1]);
        if (kfds[0] >= 0) {
            ::close(kfds[0]);
            ::close(kfds[1]);
        }
        return r;
    }
    std::vector<char*> argv;
    for (auto& s : own)
        argv.push_back(&s[0]);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (effectiveStdin) {
        posix_spawn_file_actions_adddup2(&fa, ifds[0], 0);
        posix_spawn_file_actions_addclose(&fa, ifds[0]);
        posix_spawn_file_actions_addclose(&fa, ifds[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    if (mergeStderr)
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    // Close the original pipe ends in the child. dup2 leaves the source open, and a
    // curl -K /dev/fd/N reads until EOF. If the child still holds the write end
    // (MUT_FU9_FD_LEAK builds the pipe without CLOEXEC), that read never finishes.
    if (kfds[0] >= 0) {
        posix_spawn_file_actions_adddup2(&fa, kfds[0], kRunArgvConfigFd);
        posix_spawn_file_actions_addclose(&fa, kfds[0]);
        posix_spawn_file_actions_addclose(&fa, kfds[1]);
    }
    if (hfds[1] >= 0) {
        posix_spawn_file_actions_adddup2(&fa, hfds[1], kRunArgvHeaderFd);
        posix_spawn_file_actions_addclose(&fa, hfds[0]);
        posix_spawn_file_actions_addclose(&fa, hfds[1]);
    }
    pid_t pid = -1;
    const int sp = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (effectiveStdin)
        ::close(ifds[0]);
    if (hfds[1] >= 0) {
        ::close(hfds[1]);
        hfds[1] = -1;
    }
    if (sp != 0) {
        ::close(fds[0]);
        if (hfds[0] >= 0)
            ::close(hfds[0]);
        if (effectiveStdin)
            ::close(ifds[1]);
        if (kfds[0] >= 0) {
            ::close(kfds[0]);
            ::close(kfds[1]);
        }
        return r;
    }
    r.spawned = true;
    if (effectiveStdin) {
        // Small body/config: fits the pipe buffer, so writing before reading stdout cannot deadlock.
        // main() sets SIGPIPE to SIG_IGN, so an early-exiting child gives EPIPE here, not a daemon kill.
        writeAllFd(ifds[1], *effectiveStdin);
        ::close(ifds[1]);
    }
    if (kfds[0] >= 0) {
        writeAllFd(kfds[1], *curlKConfig);
        ::close(kfds[1]);
        ::close(kfds[0]);
    }
    char buf[4096];
    while (cap == 0 || r.out.size() < cap) {
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0)
            r.out.append(buf, static_cast<size_t>(n));
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    ::close(fds[0]);
    if (hfds[0] >= 0) {   // headers precede the body and are small (< pipe buf)
        for (;;) {
            const ssize_t n = ::read(hfds[0], buf, sizeof buf);
            if (n > 0 && headerOut->size() < 65536)
                headerOut->append(buf, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR)
                continue;
            else if (n <= 0)
                break;
        }
        ::close(hfds[0]);
    }
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}
// #340: a curl with -L whose X-Plex-Token is a -K header (no URL query token). curl gets no -L;
// hops are followed here (up to 5). The token line stays in the -K config only while the hop
// has the first URL's origin; another host gets the config without it. A Location's own
// X-Plex-Token= query is dropped. No URL found: one run without -L (fail closed).
// #341 N4: when the URL query token and the header token differ, both lines end up in the -K config for
// the first origin only (the query one moved by scrubPlexTokenArgv); neither leaves that origin, and PMS
// uses the last X-Plex-Token header. Callers pass the same token in both.
static ArgvRun runArgvFollowHeaderToken(const std::vector<std::string>& args, bool mergeStderr, size_t cap,
                                        const std::string* stdinData, const std::string& cfgIn,
                                        std::string* headerOut) {
    std::vector<std::string> pre = args;
    std::string cfg = cfgIn;
    (void)mpx::scrubPlexTokenArgv(pre, cfg);   // an argv -H token joins the -K config first
    std::vector<std::string> cur;
    size_t urlIdx = 0;
    for (size_t i = 0; i < pre.size(); ++i) {
        if (i > 0 && (pre[i] == "-L" || pre[i] == "--location"))
            continue;
        cur.push_back(pre[i]);
        if (i > 0 && pre[i].find("://") != std::string::npos && pre[i - 1] != "--resolve")
            urlIdx = cur.size() - 1;
    }
    if (urlIdx == 0)
        return runArgvOnce(cur, mergeStderr, cap, stdinData, &cfg, headerOut);
    const std::string origin = mpx::curlUrlOrigin(cur[urlIdx]);
    std::string cfgNoTok = mpx::curlCfgWithoutPlexToken(cfg);
    if (cfgNoTok.empty())
        cfgNoTok = "# no token for this origin\n";   // keep the -K pipe: argv may already name -K -
    ArgvRun r;
    bool same = true;
    for (int hop = 0; hop <= 5; ++hop) {
        std::string hdr;
        r = runArgvOnce(cur, mergeStderr, cap, stdinData, same ? &cfg : &cfgNoTok, &hdr);
        if (headerOut)
            *headerOut = hdr;
        int status = 0;
        const std::string loc = mpx::curlRedirectFromHeaders(hdr, &status);
        const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!r.spawned || !redirect || loc.empty() || hop == 5)
            break;
        std::string next = mpx::curlResolveLocation(cur[urlIdx], loc);
        mpx::curlStripTokenQuery(next);
        const std::string o = mpx::curlUrlOrigin(next);
        if (o.rfind("http://", 0) != 0 && o.rfind("https://", 0) != 0)
            break;
        same = o == origin;
        std::fprintf(stderr, "misterplexd: curl redirect %d hop %d (%s origin)\n", status, hop + 1,
                     same ? "same" : "other");
        cur[urlIdx] = next;
    }
    return r;
}

// FU10 N4 (review should-fix): the token scrub drops -L so a redirect cannot carry the -K
// token header to another host; that broke WAN redirects (ladder probe). A curl with -L and
// a URL query token is now followed here, up to 5 hops: the token is re-attached only for
// the original origin (mpx::browseRedirectNext, the #273 rule); a hop to another host goes
// without it (and so keeps curl's own -L). No token or no -L: one run, as before.
ArgvRun runArgv(const std::vector<std::string>& args, bool mergeStderr = false, size_t cap = 0,
                const std::string* stdinData = nullptr, const std::string* curlKConfig = nullptr,
                std::string* headerOut = nullptr) {
    size_t urlIdx = 0;
    bool hasL = false;
    std::string token;
    if (runArgvIsCurl(args)) {
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-L" || args[i] == "--location") {
                hasL = true;
            } else if (urlIdx == 0 && args[i].find("://") != std::string::npos &&
                       misterplex::hasPlexTokenKey(args[i])) {
                std::string probe = args[i];
                bool found = false;
                token = mpx::curlStripTokenQuery(probe, &found);
                if (found && !token.empty())
                    urlIdx = i;
            }
        }
    }
#ifdef MUT_FU10_NO_FOLLOW   // test-only mutant: the FU8/FU9 behaviour, -L dropped, 30x returned
    hasL = false;
#endif
    // #341 B1: a header token with -L always takes the same-origin follower, also when the URL
    // carries a query token too (WAN start probe, plexMetadataHttpGet): the follower first moves the
    // query token into the -K config, drops -L, and re-sends the token only to the first origin.
#ifndef MUT_341_QUERY_HDR_FU10   // test-only mutant: query+header token takes the FU10 loop (-L, cfg every hop)
    const bool hdrFollowOk = true;
#else
    const bool hdrFollowOk = urlIdx == 0;
#endif
    if (hasL && hdrFollowOk &&
        ((curlKConfig && mpx::curlCfgHasPlexToken(*curlKConfig)) || mpx::curlArgvHasTokenHeader(args))) {
#ifdef MUT_PLEXGET_TOKEN_REDIRECT   // test-only mutant: curl -L carries the token header to any host
        return runArgvOnce(args, mergeStderr, cap, stdinData, curlKConfig, headerOut);
#else
        return runArgvFollowHeaderToken(args, mergeStderr, cap, stdinData,
                                        curlKConfig ? *curlKConfig : std::string{}, headerOut);
#endif
    }
    if (!hasL || urlIdx == 0)
        return runArgvOnce(args, mergeStderr, cap, stdinData, curlKConfig, headerOut);
    const std::string origin = mpx::curlUrlOrigin(args[urlIdx]);
    std::vector<std::string> cur = args;
    ArgvRun r;
    for (int hop = 0; hop <= 5; ++hop) {
        std::string hdr;
        r = runArgvOnce(cur, mergeStderr, cap, stdinData, curlKConfig, &hdr);
        int status = 0;
        const std::string loc = mpx::curlRedirectFromHeaders(hdr, &status);
        const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!r.spawned || !redirect || loc.empty() || hop == 5)
            break;
        const std::string next = mpx::browseRedirectNext(origin, mpx::curlResolveLocation(cur[urlIdx], loc), token);
        if (next.empty())
            break;
        std::fprintf(stderr, "misterplexd: curl redirect %d hop %d (%s origin)\n", status, hop + 1,
                     mpx::curlUrlOrigin(next) == origin ? "same" : "other");
        cur[urlIdx] = next;
    }
    return r;
}

std::string runArgvCapture(const std::vector<std::string>& args, const std::string* curlKConfig = nullptr) {
    return runArgv(args, false, 0, nullptr, curlKConfig).out;
}

// The identity block every daemon GET to a PMS carries (after Accept): one header per item, in the order the old
// shell command line used. An empty profileName leaves X-Plex-Client-Profile-Name out.
HeaderList plexIdentityHeaders(const std::string& profileName) {
    HeaderList h = {{"X-Plex-Client-Identifier", kPlexClientId},
                    {"X-Plex-Product", "Plex Web"},
                    {"X-Plex-Version", kPlexWebVersion},
                    {"X-Plex-Platform", "Chrome"},
                    {"X-Plex-Platform-Version", "120.0"},
                    {"X-Plex-Device", "Linux"},
                    {"X-Plex-Device-Name", "Chrome"}};
    if (!profileName.empty())
        h.push_back({"X-Plex-Client-Profile-Name", profileName});
    h.push_back({"X-Plex-Model", "bundled"});
    h.push_back({"X-Plex-Provides", "player"});
    return h;
}

std::string httpGet(const std::string& url, int timeoutSec = 15,
                    const HeaderList& extraHeaders = {},
                    const std::string& clientProfileName = defaultRequestProfileName(),
                    bool verifyTls = false, bool followRedirects = true, int* curlExitOut = nullptr,
                    bool* noCa = nullptr) {
    const auto fn = reinterpret_cast<MetadataFetchFn>(metadataFetchHook.load(std::memory_order_acquire));
    if (fn)
        return fn(url);
    // Prefer curl (present on MiSTer). PMS calls stay -k for plex.direct certs.
    // A verified call uses mpx::caBundlePath() and never -k; an empty bundle fails closed.
    // #340: redirects are followed; the X-Plex-Token header is re-sent only to the
    // same origin (runArgv for curl, tlsVerifiedGet for the OpenSSL client).
    const bool https = url.compare(0, 8, "https://") == 0;
    const bool optOut = https && tlsVerifyOff();
    (void)verifyTls;
#if defined(MPX_HAVE_GCAST_TLS)
    if (https && !optOut) {
        const std::string ca = mpx::caBundlePath();
        if (ca.empty()) {
            if (noCa)
                *noCa = true;
            return {};
        }
        HeaderList h = {{"Accept", "application/xml"}};
        for (const auto& kv : plexIdentityHeaders(clientProfileName))
            h.push_back(kv);
        for (const auto& kv : extraHeaders) {
#ifndef MUT_PLEXGET_HDR_UNCHECKED
            if (!headerNameIsToken(kv.first) || !headerValueIsClean(kv.second))
                continue;
#endif
            h.push_back(kv);
        }
        TlsVerifiedRequest q;
        q.url = url;
        q.headers = std::move(h);
        q.timeoutSec = timeoutSec;
        q.caFile = ca;
        q.followRedirects = followRedirects;   // #340: tlsVerifiedGet keeps the token same-origin only
        const TlsVerifiedResult got = tlsVerifiedGet(q);
        if (curlExitOut)
            *curlExitOut = got.transportOk ? 0 : 60;
        return got.transportOk ? got.body : std::string{};
    }
#endif
    if (https && !optOut) {
        const std::string ca = mpx::caBundlePath();
        if (ca.empty()) {
            if (noCa)
                *noCa = true;
            return {};
        }
    }
    std::vector<std::string> a = curlArgvPrefix(url, extraHeaders, followRedirects);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec), "-H", "Accept: application/xml"});
    HeaderList hdr = plexIdentityHeaders(clientProfileName);
    hdr.insert(hdr.end(), extraHeaders.begin(), extraHeaders.end());
    const std::string hdrCfg = appendHeaderArgv(a, hdr);
    appendUrlArgv(a, url);
    const ArgvRun run = runArgv(a, false, 0, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg);
    if (curlExitOut)
        *curlExitOut = run.spawned ? run.exitCode : 127;   // 127: what the old "sh -c" said for a missing curl
    return run.out;
}

std::string makeSessionId() {
    // Seed from time so the first cast after every daemon restart is not always
    // mplex-9e3779b1 (n=1 * golden). PMS keeps per-session transcoder state;
    // reusing that id after a restart stalls ffmpeg on /universal/start.
    static std::atomic<uint32_t> n{static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^
        (static_cast<uint64_t>(::getpid()) * 0x9E3779B9u))};
    std::ostringstream o;
    o << "mplex-" << std::hex << (n.fetch_add(1) * 2654435761u);
    return o.str();
}

std::string hostOnly(const std::string& hostOrUrl) {
    std::string h = hostOrUrl;
    if (auto p = h.find("://"); p != std::string::npos)
        h = h.substr(p + 3);
    if (auto p = h.find('/'); p != std::string::npos)
        h = h.substr(0, p);
    if (!h.empty() && h.front() == '[') {
        const auto rb = h.find(']');
        if (rb != std::string::npos)
            return h.substr(1, rb - 1);
    }
    // IPv4 or name:port — a single colon. Unbracketed IPv6 keeps every colon.
    const auto colon = h.rfind(':');
    if (colon != std::string::npos && h.find(':') == colon)
        h = h.substr(0, colon);
    return h;
}

bool hostLooksLoopback(const std::string& hostOrUrl) {
    const std::string h = hostOnly(hostOrUrl);
    if (h == "localhost" || h == "::1" || h == "0:0:0:0:0:0:0:1")
        return true;
    if (h.rfind("127.", 0) == 0)
        return true;
    return false;
}

bool hostLooksIpv6Special(const std::string& hostOrUrl) {
    std::string h = hostOnly(hostOrUrl);
    if (h.find("plex.direct") != std::string::npos) {
        const auto dot = h.find('.');
        std::string label = dot == std::string::npos ? h : h.substr(0, dot);
        for (char& c : label) {
            if (c == '-')
                c = ':';
        }
        h = label;
    }
    in6_addr a{};
    if (inet_pton(AF_INET6, h.c_str(), &a) != 1)
        return false;
    if ((a.s6_addr[0] & 0xfe) == 0xfc)
        return true; // ULA fc00::/7
    if (a.s6_addr[0] == 0xfe && (a.s6_addr[1] & 0xc0) == 0x80)
        return true; // link-local fe80::/10
    return false;
}

bool isUnreachableHost(const std::string& hostOrUrl) {
    if (isUnreachableIpv6Host(hostOrUrl))
        return true;
    const std::string h = hostOnly(hostOrUrl);
    if (h.rfind("172.17.", 0) == 0 || h.rfind("172.18.", 0) == 0 ||
        h.rfind("172.20.", 0) == 0 || h.rfind("172.21.", 0) == 0)
        return true;
    if (h.find("172-17-") != std::string::npos || h.find("172-18-") != std::string::npos ||
        h.find("172-20-") != std::string::npos || h.find("172-21-") != std::string::npos)
        return true;
    if (hostLooksIpv6Special(hostOrUrl))
        return true;
    return false;
}

// Parse IPv4 from dotted host or leading dashed plex.direct label (a-b-c-d.…).
bool parseIpv4Host(const std::string& hostOrUrl, int out[4]) {
    out[0] = out[1] = out[2] = out[3] = -1;
    std::string h = hostOnly(hostOrUrl);
    if (h.empty())
        return false;
    if (h.find("plex.direct") != std::string::npos) {
        const auto dot = h.find('.');
        h = dot == std::string::npos ? h : h.substr(0, dot);
    }
    // Accept either dotted or dashed separators.
    int idx = 0, cur = -1;
    for (size_t i = 0; i <= h.size(); ++i) {
        const char c = i < h.size() ? h[i] : '.';
        if (c >= '0' && c <= '9') {
            if (cur < 0)
                cur = 0;
            cur = cur * 10 + (c - '0');
            if (cur > 255)
                return false;
        } else if (c == '.' || c == '-') {
            if (cur < 0 || idx >= 4)
                return false;
            out[idx++] = cur;
            cur = -1;
        } else {
            return false;
        }
    }
    return idx == 4;
}

bool isPrivateRfc1918Host(const std::string& hostOrUrl) {
    int o[4];
    if (!parseIpv4Host(hostOrUrl, o))
        return false;
    if (o[0] == 10)
        return true;
    if (o[0] == 192 && o[1] == 168)
        return true;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31)
        return true;
    return false;
}

// Exact private IPv4 match (a PMS and a foreign home often share one private /24
// numbering on different physical LANs — /24 is NOT sufficient).
bool samePrivateHost(const std::string& a, const std::string& b) {
    int aa[4], bb[4];
    if (!parseIpv4Host(a, aa) || !parseIpv4Host(b, bb))
        return false;
    return aa[0] == bb[0] && aa[1] == bb[1] && aa[2] == bb[2] && aa[3] == bb[3];
}

bool samePrivateSlash24(const std::string& a, const std::string& b) {
    int aa[4], bb[4];
    if (!parseIpv4Host(a, aa) || !parseIpv4Host(b, bb))
        return false;
    return aa[0] == bb[0] && aa[1] == bb[1] && aa[2] == bb[2];
}

// LAN-origin playMedia often pins the player's VLAN gateway (203.0.113.1 next
// to player 203.0.113.20) instead of conf PMS (203.0.113.10). That host is
// reachable IPv4 so the ULA / plex.direct rewrite misses it. A foreign home
// LAN on a different /24 (203.0.113.70) is not this player's gateway.
bool isPlayerLanGateway(const std::string& host, const std::string& playerLan,
                        const std::string& conf) {
    if (playerLan.empty() || conf.empty() || host.empty())
        return false;
    if (!isPrivateRfc1918Host(host) || !isPrivateRfc1918Host(playerLan))
        return false;
    if (!samePrivateSlash24(host, playerLan))
        return false;
    if (samePrivateHost(host, conf))
        return false;
    return true;
}

std::string attr(const std::string& xml, const char* tag, const char* name) {
    // First occurrence of <tag ... name="..."
    const std::string open = std::string("<") + tag;
    size_t tpos = 0;
    while ((tpos = xml.find(open, tpos)) != std::string::npos) {
        const size_t boundary = tpos + open.size();
        if (boundary >= xml.size() ||
            std::isspace(static_cast<unsigned char>(xml[boundary])) ||
            xml[boundary] == '>' || xml[boundary] == '/') {
            break;
        }
        tpos = boundary;
    }
    if (tpos == std::string::npos)
        return {};
    auto end = xml.find('>', tpos);
    if (end == std::string::npos)
        return {};
    const std::string slice = xml.substr(tpos, end - tpos);
    const std::string key = std::string(name) + "=\"";
    auto p = slice.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    auto e = slice.find('"', p);
    if (e == std::string::npos)
        return {};
    return slice.substr(p, e - p);
}

// Attr from a free-form XML slice (first name="..." only).
std::string attrIn(const std::string& slice, const char* name) {
    const std::string key = std::string(name) + "=\"";
    auto p = slice.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    auto e = slice.find('"', p);
    if (e == std::string::npos)
        return {};
    return slice.substr(p, e - p);
}

std::string lowerCopy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// H.264/AVC direct Part is preferred for STREAM host CAVLC recon (Baseline/Main).
bool videoCodecIsH264(const std::string& codecRaw) {
    const std::string c = lowerCopy(codecRaw);
    if (c.empty())
        return false;
    return c.find("h264") != std::string::npos || c.find("avc") != std::string::npos ||
           c.find("x264") != std::string::npos;
}

} // namespace

std::string plexMetadataHttpGet(const std::string& url) { return httpGet(url, 12); }
std::string plexMetadataHttpGet(const std::string& url, const std::string& token) {
    if (token.empty())
        return httpGet(url, 12);
    return httpGet(url, 12, HeaderList{{"X-Plex-Token", token}});   // #340: header, not query
}

// FU9 test seam: the single spawn point, argv as given (no shell).
std::string runArgvCaptureForTest(const std::vector<std::string>& args) { return runArgvCapture(args); }

namespace {

// plex.tv rejects /api/v2/resources with 400 when X-Plex-Client-Identifier is absent.
bool urlHostIsPlexTv(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return false;
    std::string host = url.substr(scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos)
        host = host.substr(0, slash);
    const auto at = host.find('@');
    if (at != std::string::npos)
        host = host.substr(at + 1);
    if (!host.empty() && host.front() == '[') {
        const auto rb = host.find(']');
        host = rb == std::string::npos ? host : host.substr(1, rb - 1);
    } else if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    for (char& c : host)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#ifdef MUT_PLEX_TV_HOST_SUBSTRING   // test-only: evilplex.tv would count as plex.tv
    return host.find("plex.tv") != std::string::npos;
#else
    return host == "plex.tv" ||
           (host.size() > 8 && host.compare(host.size() - 8, 8, ".plex.tv") == 0);
#endif
}

bool extraHasHeader(const std::vector<std::pair<std::string, std::string>>& headers, const char* name) {
    for (const auto& h : headers) {
        if (h.first == name && !h.second.empty())
            return true;
    }
    return false;
}

// Same client id the player GETs use (plexIdentityHeaders). Product is already
// MiSTerPlex on plexHttpGet. Version matches that identity block.
void appendPlexTvIdentityArgv(std::vector<std::string>& a, const std::string& url,
                              const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    if (!urlHostIsPlexTv(url))
        return;
#ifndef MUT_PLEX_TV_NO_CLIENT_ID
    if (!extraHasHeader(extraHeaders, "X-Plex-Client-Identifier"))
        a.insert(a.end(), {"-H", std::string("X-Plex-Client-Identifier: ") + kPlexClientId});
#endif
    // Product is already on the plexHttpGet argv (MiSTerPlex). Version is kPlexWebVersion.
    if (!extraHasHeader(extraHeaders, "X-Plex-Version")) {
#ifdef MUT_PLEX_VERSION_NOT_CONSTANT   // test-only: a second copy of the version
        a.insert(a.end(), {"-H", "X-Plex-Version: 9.9.9"});
#else
        a.insert(a.end(), {"-H", std::string("X-Plex-Version: ") + kPlexWebVersion});
#endif
    }
}

} // namespace

std::string plexHttpGet(const std::string& url, int timeoutSec,
                        const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    if (url.find('\0') != std::string::npos)
        return {};
    std::vector<std::string> a = curlArgvPrefix(url, extraHeaders, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec),
                       "-H", "Accept: application/xml", "-H", "X-Plex-Product: MiSTerPlex",
                       "-H", "X-Plex-Provides: player"});
    appendPlexTvIdentityArgv(a, url, extraHeaders);
    const std::string hdrCfg = appendHeaderArgv(a, extraHeaders);
    appendUrlArgv(a, url);
    return runArgvCapture(a, hdrCfg.empty() ? nullptr : &hdrCfg);
}

namespace {

std::string headerField(const std::string& hdr, const char* name) {
    const size_t nlen = std::strlen(name);
    size_t i = 0;
    while (i < hdr.size()) {
        size_t eol = hdr.find('\n', i);
        if (eol == std::string::npos)
            eol = hdr.size();
        std::string line = hdr.substr(i, eol - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        bool match = line.size() > nlen && line[nlen] == ':';
        for (size_t c = 0; match && c < nlen; ++c) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(line[c])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(name[c])));
            if (a != b)
                match = false;
        }
        if (match) {
            size_t v = nlen + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                ++v;
            return line.substr(v);
        }
        if (eol == hdr.size())
            break;
        i = eol + 1;
    }
    return {};
}

} // namespace

PlexHttpObserved plexHttpGetObserved(const std::string& url, int timeoutSec,
                                     const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    PlexHttpObserved r;
    if (url.find('\0') != std::string::npos)
        return r;
    std::vector<std::string> a = curlArgvPrefix(url, extraHeaders, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec),
                       "-H", "Accept: application/xml", "-H", "X-Plex-Product: MiSTerPlex",
                       "-H", "X-Plex-Provides: player"});
    appendPlexTvIdentityArgv(a, url, extraHeaders);
    const std::string hdrCfg = appendHeaderArgv(a, extraHeaders);
    appendUrlArgv(a, url);
    std::string hdr;
    const ArgvRun run = runArgv(a, false, 0, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg, &hdr);
    if (!run.spawned)
        return r;
    r.body = run.out;
    const remote_start::CurlHeaderDump parsed = remote_start::parseCurlHeaderDump(hdr);
    r.httpStatus = parsed.httpStatus;
    r.contentType = headerField(hdr, "Content-Type");
    return r;
}

std::string plexHttpGetBytes(const std::string& url, int timeoutSec,
                             const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                             size_t capBytes) {
    if (url.find('\0') != std::string::npos || capBytes == 0)
        return {};
    std::vector<std::string> a = curlArgvPrefix(url, extraHeaders, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec), "-f",
                       "--max-filesize", std::to_string(capBytes), "-H", "Accept: */*",
                       "-H", "X-Plex-Product: MiSTerPlex", "-H", "X-Plex-Provides: player"});
    appendPlexTvIdentityArgv(a, url, extraHeaders);
    const std::string hdrCfg = appendHeaderArgv(a, extraHeaders);
    appendUrlArgv(a, url);
    const ArgvRun run = runArgv(a, false, capBytes, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg);
    if (!run.spawned || run.exitCode != 0 || run.out.size() > capBytes)
        return {};
    return run.out;
}

namespace {

std::atomic<int> putDnsInFlight{0};
std::atomic<uintptr_t> putAddrInfoHook{0};
std::atomic<int> putStallFd{-1};
std::atomic<int> putConnectPolls{0};
constexpr int kPutDnsLookupCap = 2;

bool putCancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_acquire);
}

int callPutAddrInfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    const auto fn = reinterpret_cast<PutAddrInfoFn>(putAddrInfoHook.load(std::memory_order_acquire));
    if (!fn)
        return ::getaddrinfo(node, service, hints, res);
    return fn(node, service, hints, res);
}

void notePartPutDnsLimit() {
    std::fprintf(stderr, "misterplexd: part PUT skipped: dns lookup limit\n");
}

// Numeric hosts resolve in-process. Names go to a detached thread that only
// touches this job, so a timeout or cancel cannot outlive TrackIo.
addrinfo* resolvePutHost(const std::string& host, const std::string& port, bool numeric,
                         const std::atomic<bool>* cancel) {
    if (putCancelled(cancel))
        return nullptr;
    if (numeric) {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_NUMERICHOST;
        addrinfo* res = nullptr;
        if (callPutAddrInfo(host.c_str(), port.c_str(), &hints, &res) != 0)
            return nullptr;
        return res;
    }
    int cur = putDnsInFlight.load(std::memory_order_relaxed);
    for (;;) {
        if (cur >= kPutDnsLookupCap) {
            notePartPutDnsLimit();
            return nullptr;
        }
        if (putDnsInFlight.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                                std::memory_order_relaxed))
            break;
    }
    struct DnsJob {
        std::mutex mu;
        addrinfo* res = nullptr;
        int rc = -1;
        bool done = false;
        bool abandoned = false;
    };
    auto job = std::make_shared<DnsJob>();
    try {
        std::thread([job, host, port] {
            struct Slot {
                ~Slot() { putDnsInFlight.fetch_sub(1, std::memory_order_acq_rel); }
            } slot;
            addrinfo hints{};
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_family = AF_UNSPEC;
            addrinfo* res = nullptr;
            const int rc = callPutAddrInfo(host.c_str(), port.c_str(), &hints, &res);
            std::lock_guard<std::mutex> g(job->mu);
            if (job->abandoned) {
                if (res)
                    freeaddrinfo(res);
                job->done = true;
                return;
            }
            job->res = res;
            job->rc = rc;
            job->done = true;
        }).detach();
    } catch (...) {
        putDnsInFlight.fetch_sub(1, std::memory_order_acq_rel);
        return nullptr;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (putCancelled(cancel))
            break;
        {
            std::lock_guard<std::mutex> g(job->mu);
            if (job->done) {
                addrinfo* res = job->res;
                job->res = nullptr;
                if (job->rc != 0 || !res) {
                    if (res)
                        freeaddrinfo(res);
                    return nullptr;
                }
                return res;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::lock_guard<std::mutex> g(job->mu);
    job->abandoned = true;
    if (job->done && job->res) {
        freeaddrinfo(job->res);
        job->res = nullptr;
    }
    return nullptr;
}

// The 200 ms poll after a connect stays in progress. Cancel is checked on every
// slice. A test passes a filled pipe write end here so the wait does not need
// a routed blackhole.
bool pollConnectFd(int fd, int timeoutSec, const std::atomic<bool>* cancel) {
    putConnectPolls.fetch_add(1, std::memory_order_acq_rel);
    int waited = 0;
    const int limit = timeoutSec * 1000;
    while (waited < limit) {
        if (putCancelled(cancel))
            return false;
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const int slice = std::min(200, limit - waited);
        const int pr = ::poll(&pfd, 1, slice);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (pr == 1) {
            int so = 0;
            socklen_t sl = sizeof so;
            return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && so == 0;
        }
        waited += slice;
    }
    return false;
}

bool connectPut(int fd, const addrinfo* ai, int timeoutSec, const std::atomic<bool>* cancel) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0)
        return true;
    if (errno != EINPROGRESS)
        return false;
    return pollConnectFd(fd, timeoutSec, cancel);
}

} // namespace

void notePartPutHttpsSkip() {
    static std::atomic<int> once{0};
    if (once.exchange(1) == 0)
        std::fprintf(stderr, "misterplexd: part PUT skipped: https\n");
}

void setPutAddrInfoForTest(PutAddrInfoFn fn) {
    putAddrInfoHook.store(reinterpret_cast<uintptr_t>(fn), std::memory_order_release);
}

void setAudioPartPutForTest(AudioPartPutFn fn) {
    audioPartPutHook.store(reinterpret_cast<uintptr_t>(fn), std::memory_order_release);
}

void setMetadataFetchForTest(MetadataFetchFn fn) {
    metadataFetchHook.store(reinterpret_cast<uintptr_t>(fn), std::memory_order_release);
}

void setPutConnectStallFdForTest(int fd) {
    putStallFd.store(fd, std::memory_order_release);
}

int putConnectPollsForTest() {
    return putConnectPolls.load(std::memory_order_acquire);
}

int putDnsLookupsInFlight() {
    return putDnsInFlight.load(std::memory_order_acquire);
}

bool plexHttpPut(const std::string& urlIn, int timeoutSec, const std::atomic<bool>* cancel,
                 const std::string& tokenIn) {
    // #340: the token is an X-Plex-Token header line. A query token left in the URL by a
    // caller is moved there too, so the request line never carries X-Plex-Token=.
    std::string url = urlIn;
    std::string token = tokenIn;
    {
        bool found = false;
        const std::string q = mpx::curlStripTokenQuery(url, &found);
        if (found && token.empty())
            token = q;
    }
    if (url.find_first_of("\r\n") != std::string::npos || token.find_first_of("\r\n") != std::string::npos ||
        url.find('\0') != std::string::npos || token.find('\0') != std::string::npos)
        return false;
    if (timeoutSec < 1)
        timeoutSec = 1;
    if (putCancelled(cancel))
        return false;
    // No shell. TLS is fail-closed here; the part PUT is plain HTTP on the LAN.
    if (url.rfind("https://", 0) == 0) {
        notePartPutHttpsSkip();
        return false;
    }
    if (url.rfind("http://", 0) != 0)
        return false;
    const std::string rest = url.substr(7);
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    if (hostport.empty() || path.empty())
        return false;
    std::string host = hostport;
    int port = 80;
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos && colon + 1 < hostport.size()) {
        host = hostport.substr(0, colon);
        char* end = nullptr;
        const long p = std::strtol(hostport.c_str() + colon + 1, &end, 10);
        if (!end || *end != '\0' || p <= 0 || p > 65535)
            return false;
        port = static_cast<int>(p);
    }
    in_addr ipv4{};
    in6_addr ipv6{};
    const bool numeric = inet_pton(AF_INET, host.c_str(), &ipv4) == 1 || inet_pton(AF_INET6, host.c_str(), &ipv6) == 1;
    addrinfo* res = resolvePutHost(host, std::to_string(port), numeric, cancel);
    if (!res)
        return false;
    const int stall = putStallFd.load(std::memory_order_acquire);
    if (stall >= 0) {
        freeaddrinfo(res);
        return pollConnectFd(stall, timeoutSec, cancel);
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (putCancelled(cancel))
            break;
        fd = ::socket(ai->ai_family, SOCK_STREAM, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connectPut(fd, ai, timeoutSec, cancel))
            break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 || putCancelled(cancel)) {
        if (fd >= 0)
            ::close(fd);
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    const std::string req = "PUT " + path + " HTTP/1.0\r\nHost: " + hostport +
                            (token.empty() ? std::string{} : "\r\nX-Plex-Token: " + token) +
                            "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    size_t off = 0;
    while (off < req.size()) {
        if (putCancelled(cancel) || std::chrono::steady_clock::now() >= deadline) {
            ::close(fd);
            return false;
        }
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    std::string resp;
    char buf[128];
    while (resp.size() < 64) {
        if (putCancelled(cancel) || std::chrono::steady_clock::now() >= deadline)
            break;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (n == 0)
            break;
        resp.append(buf, buf + n);
        if (resp.find("\r\n") != std::string::npos)
            break;
    }
    ::close(fd);
    const auto sp = resp.find(' ');
    if (sp == std::string::npos || sp + 3 > resp.size())
        return false;
    const int code = std::atoi(resp.c_str() + sp + 1);
    return code >= 200 && code < 300;
}

namespace {

// One thread for prefs fsync and the part PUT. The PUT slot keeps only the latest URL.
struct TrackIo {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    bool save = false;
    std::optional<std::pair<std::string, std::string>> putUrl;   // url, token (#340)
    std::thread thr;
    TrackIo() : thr([this] { loop(); }) {}
    ~TrackIo() {
        {
            std::lock_guard<std::mutex> g(mu);
            stop.store(true);
            putUrl.reset();
        }
        cv.notify_all();
        if (thr.joinable())
            thr.join();
    }
    void loop() {
        for (;;) {
            bool doSave = false;
            std::string url;
            std::string tok;
            {
                std::unique_lock<std::mutex> g(mu);
                cv.wait(g, [&] { return stop.load() || save || putUrl.has_value(); });
                if (stop.load()) {
                    doSave = save;
                    save = false;
                    putUrl.reset();
                    if (!doSave)
                        return;
                } else {
                    doSave = save;
                    save = false;
                    if (putUrl) {
                        url = std::move(putUrl->first);
                        tok = std::move(putUrl->second);
                        putUrl.reset();
                    }
                }
            }
            if (doSave) {
                processShowTracks().flush();
                processRememberTracks().flush();
            }
            if (stop.load())
                return;
            if (!url.empty())
                (void)plexHttpPut(url, 8, &stop, tok);
        }
    }
};

TrackIo& trackIo() {
    static TrackIo io;
    return io;
}

} // namespace

void enqueueShowTrackSave() {
    TrackIo& io = trackIo();
    {
        std::lock_guard<std::mutex> g(io.mu);
        io.save = true;
    }
    io.cv.notify_one();
}

void enqueuePartStreamPut(const std::string& url, const std::string& token) {
    if (url.empty())
        return;
    TrackIo& io = trackIo();
    {
        std::lock_guard<std::mutex> g(io.mu);
        io.putUrl = std::make_pair(url, token);
    }
    io.cv.notify_one();
}

void stopTrackIoForTest() {
    TrackIo& io = trackIo();
    {
        std::lock_guard<std::mutex> g(io.mu);
        io.stop.store(true);
        io.putUrl.reset();
    }
    io.cv.notify_all();
    if (io.thr.joinable())
        io.thr.join();
}

const std::vector<ModeResolveRow>& modeResolveTable() {
    static const std::vector<ModeResolveRow> rows = {
        {"240p", "320x240", 2500, 55, "h264", "baseline", "baseline,main", 30, "aac", 2},
        {"480p", "640x480", 2500, 60, "h264", "baseline", "baseline,main", 30, "aac", 2},
        {"480i", "720x480", 8000, 40, "h264", "baseline", "baseline,main", 30, "aac", 2},
    };
    return rows;
}

const ModeResolveRow* modeResolveRow(const std::string& modeOrResolution) {
    for (const auto& r : modeResolveTable()) {
        if (modeOrResolution == r.mode || modeOrResolution == r.videoResolution)
            return &r;
    }
    return nullptr;
}

bool applyModeResolve(const std::string& modeOrResolution, WeakLadder& weak) {
    const ModeResolveRow* r = modeResolveRow(modeOrResolution);
    if (!r)
        return false;
    weak.profileName = r->mode;
    weak.videoResolution = r->videoResolution;
    weak.maxVideoBitrateKbps = r->maxVideoBitrateKbps;
    weak.videoQuality = r->videoQuality;
    weak.videoCodec = r->videoCodec;
    weak.audioCodec = r->audioCodec;
    weak.h264Profile = r->h264Profile;
    weak.h264ProfileParam = r->h264ProfileParam;
    weak.h264Level = r->h264Level;
    weak.audioChannels = r->audioChannels;
    return true;
}

const std::vector<PlexTranscodeProfile>& plexTranscodeProfiles() {
    static const std::vector<PlexTranscodeProfile> profiles = [] {
        std::vector<PlexTranscodeProfile> p;
        for (const auto& r : modeResolveTable())
            p.push_back({r.mode, r.videoResolution, r.maxVideoBitrateKbps, r.videoQuality,
                         r.h264Profile, r.h264Level});
        p.push_back({"720p", "1280x720", 8000, 100, "main", 31});
        return p;
    }();
    return profiles;
}

bool isPlexBasePlaceholder(const std::string& raw) {
    // Shipped examples use YOUR-PLEX-SERVER / ANOTHER-PLEX-SERVER / <your-plex-server>.
    // A value still holding one is unset, never a host to dial.
    std::string l;
    for (char c : raw)
        l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("your-plex-server") != std::string::npos ||
           l.find("another-plex-server") != std::string::npos ||
           l.find('<') != std::string::npos || l.find('>') != std::string::npos;
}

std::string normalizePlexBase(const std::string& raw) {
    if (isPlexBasePlaceholder(raw))
        return {};
    std::string s = raw;
    // trim
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                          s.front() == '\n'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n' || s.back() == '/'))
        s.pop_back();
    if (s.empty())
        return {};
    if (s.find("://") == std::string::npos) {
        // bare host or host:port → assume http
        s = "http://" + s;
    }
    // strip trailing slash again after scheme
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    // If no port in authority, default PMS :32400 (skip if user omitted intentionally
    // for reverse-proxy with scheme-only host — rare; LAN always wants 32400).
    auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        auto hostStart = scheme + 3;
        auto slash = s.find('/', hostStart);
        std::string auth =
            (slash == std::string::npos) ? s.substr(hostStart) : s.substr(hostStart, slash - hostStart);
        // IPv6 in brackets or host:port
        bool hasPort = false;
        if (!auth.empty() && auth.front() == '[') {
            auto br = auth.find(']');
            hasPort = (br != std::string::npos && br + 1 < auth.size() && auth[br + 1] == ':');
        } else {
            hasPort = (auth.rfind(':') != std::string::npos);
        }
        if (!hasPort && !auth.empty())
            s = s.substr(0, hostStart) + auth + ":32400" +
                (slash == std::string::npos ? std::string() : s.substr(slash));
    }
    return s;
}

std::vector<std::string> parsePlexServerList(const std::string& csvOrSingle) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        auto n = normalizePlexBase(cur);
        cur.clear();
        if (n.empty())
            return;
        for (const auto& e : out) {
            if (e == n)
                return;
        }
        out.push_back(n);
    };
    for (char c : csvOrSingle) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

std::vector<std::string> mergePlexServers(const std::string& serversCsv,
                                          const std::vector<std::string>& baseLines) {
    std::vector<std::string> out = parsePlexServerList(serversCsv);
    for (const auto& line : baseLines) {
        for (const auto& n : parsePlexServerList(line)) {
            bool seen = false;
            for (const auto& e : out) {
                if (e == n) {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                out.push_back(n);
        }
    }
    return out;
}

namespace {
// One label for 240p/480i/480p (shared noproxy path; not three copies).
const char* fpgaModeName(const std::string& mode) {
    if (mode == "480i")
        return "480i";
    if (mode == "480p")
        return "480p";
    return "240p";
}
} // namespace

// #275: the lab ladder helper is gone; misterplexd resolves against PMS only.
bool ladderProxyConfigured() { return false; }

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            auto hex = in.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string urlEncodeQuery(const std::string& s) {
    // The body is the shared codec (url_query_codec.hpp), so the ladder start retarget
    // (transcode_offset.hpp) re-encodes a src byte-identically (F-B).
    return urlEncodeQueryComponent(s);
}

namespace {

bool parseResolution(const std::string& res, int& w, int& h) {
    w = h = 0;
    return std::sscanf(res.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

} // namespace

std::string fpgaModeForLadder(const WeakLadder& weak) {
    // Own-core: operator pin, else the live CAPS raster, else conf DECODE.
    if (const char* modeEnv = std::getenv("MPX_FPGA_MODE")) {
        const std::string env = modeEnv;
        if (env == "480p" || env == "480i" || env == "240p")
            return env;
    }
    int confW = 0, confH = 0;
    parseResolution(weak.videoResolution, confW, confH);
    // Geometry wins over stale profileName (conf DECODE=480p while videoResolution=320x240).
    const bool geom240 =
        confW > 0 && confH > 0 && confH <= 240 && confW <= kPlex240pPresentedWidth;
    const bool tier480 = !geom240 &&
                         (weak.profileName == "480i" || weak.profileName == "480p" ||
                          weak.profileName == "fpga-h264-480p" || confW >= 480 || confH >= 480);
    const std::string live = CoreGate::process().pairedRaster();
    // F705: a 480-tier weak ladder on a live 240p core (reviewer 624x468 on prod Plex.rbf)
    // must keep the 480 PMS geometry until menu reload lands on Utility 240p.
    // F719/R16: once the decision maps to the 240p core (624x468, 320x240, …), use
    // the 320x240 ladder so the SPS gate never sees pic_width_in_mbs 39 on 0x24F1A2C4.
    if (tier480 && live == "240p") {
        if (selectedCoreIdForDecisionOutput(confW, confH) == kSelectedCoreId240p)
            return "240p";
        return weak.profileName == "480i" ? "480i" : "480p";
    }
    if (live == "480p" || live == "480i" || live == "240p")
        return live;
    std::string mode = "240p";
    if (weak.profileName == "480p" || weak.profileName == "fpga-h264-480p" || confW >= 480 ||
        confH >= 480)
        mode = "480p";
    return mode;
}

namespace {
std::mutex& clientProfileNameMu() { static std::mutex m; return m; }
std::string& clientProfileNameConfStore() { static std::string s; return s; }
bool validClientProfileNameValue(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}
} // namespace

void setClientProfileNameConf(const std::string& name) {
    std::lock_guard<std::mutex> l(clientProfileNameMu());
    clientProfileNameConfStore() = name;
}

std::string clientProfileNameOverride(std::string* source) {
    if (const char* e = std::getenv("MPX_CLIENT_PROFILE_NAME")) {
        if (validClientProfileNameValue(e)) {
            if (source) *source = "env";
            return e;
        }
        if (*e) std::fprintf(stderr, "misterplexd: MPX_CLIENT_PROFILE_NAME rejected (allowed [A-Za-z0-9 ._-], 1..64)\n");
    }
    std::string conf;
    {
        std::lock_guard<std::mutex> l(clientProfileNameMu());
        conf = clientProfileNameConfStore();
    }
    if (validClientProfileNameValue(conf)) {
        if (source) *source = "conf";
        return conf;
    }
    if (source) *source = "default";
    return {};
}

std::string baselineClientProfileName(std::string* source) {
    const std::string o = clientProfileNameOverride(source);
    return o.empty() ? std::string(kBaselineClientProfileName) : o;
}

namespace {
std::string& sendProfileNameConfStore() { static std::string s; return s; }
SendProfileNameMode parseSendProfileName(const std::string& v) {
    if (v == "fixed" || v == "1" || v == "on" || v == "yes") return SendProfileNameMode::Fixed;
    if (v == "variant") return SendProfileNameMode::Variant;
    return SendProfileNameMode::None;
}
} // namespace

void setSendProfileNameConf(const std::string& value) {
    std::lock_guard<std::mutex> l(clientProfileNameMu());
    sendProfileNameConfStore() = value;
}

SendProfileNameMode sendProfileNameMode() {
    if (const char* e = std::getenv("MPX_SEND_PROFILE_NAME"); e && *e)
        return parseSendProfileName(e);
    std::lock_guard<std::mutex> l(clientProfileNameMu());
    return parseSendProfileName(sendProfileNameConfStore());
}

const char* sendProfileNameModeName(SendProfileNameMode m) {
    return m == SendProfileNameMode::Fixed ? "fixed" : m == SendProfileNameMode::Variant ? "variant" : "none";
}

std::string profileNameSendDescription() {
    std::string src;
    const std::string o = clientProfileNameOverride(&src);
#ifndef MUT_PROFILE_NAME_LOG_OLD   // test-only mutant: the pre-Round-E line (send=<mode> next to an override)
    if (!o.empty())
        return "send=" + o + " (every request; source: " +
               (src == "env" ? std::string("env MPX_CLIENT_PROFILE_NAME") : std::string("conf CLIENT_PROFILE_NAME")) +
               " override; Extra without replace=true or VideoEncodeFlags)";
#endif
    const SendProfileNameMode m = sendProfileNameMode();
    const std::string mode = std::string(" (source: SEND_PROFILE_NAME=") + sendProfileNameModeName(m) + ")";
    if (m == SendProfileNameMode::Fixed)
        return "send=" + baselineClientProfileName() + mode;
    if (m == SendProfileNameMode::Variant)
        return "send=MiSTerPlex-FPGA-* variant to a private-LAN PMS, else " + baselineClientProfileName() + mode;
    return "send=none (no CLIENT_PROFILE_NAME / MPX_CLIENT_PROFILE_NAME override; SEND_PROFILE_NAME none)";
}

std::string plexRequestProfileName(const WeakLadder& weak, const std::string& targetUrl) {
    if (weak.owenb)
        return {};
    const std::string o = clientProfileNameOverride();
    if (!o.empty())
        return o;
    SendProfileNameMode mode = sendProfileNameMode();
#ifdef MUT_PROFILE_NAME_ALWAYS   // test-only mutant: 02e5e4595 (a fixed name to every server by default)
    if (mode == SendProfileNameMode::None)
        mode = SendProfileNameMode::Fixed;
#endif
    if (mode == SendProfileNameMode::None)
        return {};
#ifdef MUT_PROFILE_NAME_VARIANT   // test-only mutant: the pre-1011 variant name to every server
    if (!weak.fpgaProfileVariant.empty())
        return weak.fpgaProfileVariant;
#endif
#ifndef MUT_PROFILE_NAME_LAN_FIXED   // test-only mutant: variant mode sends the fixed name to a LAN PMS too
    if (mode == SendProfileNameMode::Variant && !weak.fpgaProfileVariant.empty() &&
        plexTargetIsPrivateLan(targetUrl))
        return weak.fpgaProfileVariant;
#endif
    (void)targetUrl;
    return baselineClientProfileName();
}

std::string defaultRequestProfileName() {
    if (clientProfileNameOverride().empty() && sendProfileNameMode() == SendProfileNameMode::None
#ifdef MUT_PROFILE_NAME_ALWAYS
        && false
#endif
    )
        return {};
    return baselineClientProfileName();
}

bool applyPlexTranscodeProfile(const std::string& nameOrResolution, WeakLadder& weak) {
    if (applyModeResolve(nameOrResolution, weak))
        return true;
    for (const auto& p : plexTranscodeProfiles()) {
        if (nameOrResolution == p.name || nameOrResolution == p.videoResolution) {
            weak.profileName = p.name;
            weak.videoResolution = p.videoResolution;
            weak.maxVideoBitrateKbps = p.maxVideoBitrateKbps;
            weak.videoQuality = p.videoQuality;
            weak.videoCodec = "h264";
            weak.audioCodec = "aac";
            weak.h264Profile = p.h264Profile;
            weak.h264Level = p.h264Level;
            return true;
        }
    }
    return false;
}

bool applyFpgaPlexProfile(WeakLadder& weak, const std::string& prototype,
                          int fpsNum, int fpsDen, bool filteringOff, std::string* why,
                          const std::string& mode, bool allowReservedExperiment) {
    FpgaPmsProfile profile;
    if (!selectFpgaPmsProfile(prototype, mode, fpsNum, fpsDen, filteringOff, profile, why,
                              allowReservedExperiment))
        return false;
    weak.profileName = (mode == "480p" || mode == "480i") ? "fpga-h264-480p" : "fpga-h264-240p";
    // Fixed request name; the variant stays internal (validation, LAN query keys, Extra limits).
    weak.clientProfileName = kBaselineClientProfileName;
    weak.fpgaProfileVariant = profile.clientProfileName;
    weak.videoResolution = profile.videoResolution;
    weak.maxVideoBitrateKbps = profile.maxVideoBitrateKbps;
    weak.videoQuality = 60;
    weak.videoCodec = "h264";
    weak.audioCodec = "aac";
    weak.h264Profile = "baseline";
    weak.h264Level = 30;
    weak.fpgaFpsNum = profile.fpsNum;
    weak.fpgaFpsDen = profile.fpsDen;
    return true;
}

namespace {
bool fpsIsTrue24(int num, int den) {
    return (num == 24 && den == 1) || (num == 24000 && den == 1001);
}
bool fpsIsTrue30(int num, int den) {
    return (num == 30 && den == 1) || (num == 30000 && den == 1001);
}
} // namespace

bool fpgaRasterNtsc(const std::string& mode) {
    if (mode == "240p" || mode == "480i")
        return true;
    if (mode.empty())
        return false;
    // Same truthy test as present_bank.hpp envFlagWanted("MPX_PRESENT_480I").
    const char* e = std::getenv("MPX_PRESENT_480I");
    if (!e || !*e)
        return false;
    return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || std::strcmp(e, "on") == 0 ||
           std::strcmp(e, "ON") == 0 || std::strcmp(e, "YES") == 0 ||
           std::strcmp(e, "yes") == 0;
}

bool fpgaContentFps(int srcNum, int srcDen, bool passthrough30, int& outNum, int& outDen) {
    (void)passthrough30;
    // Every FPGA raster: integer 24/1 is Plex 24p film (23.976); integer 30/1 is
    // Plex 30p NTSC (29.97). 240p 3:2 and 480i 2:2 land on the 60000/1001 tick.
    if (srcNum == 24 && srcDen == 1) {
        outNum = 24000;
        outDen = 1001;
        return true;
    }
    if (srcNum == 30 && srcDen == 1) {
        outNum = 30000;
        outDen = 1001;
        return true;
    }
    if (fpsIsTrue24(srcNum, srcDen) || fpsIsTrue30(srcNum, srcDen)) {
        outNum = srcNum;
        outDen = srcDen;
        return false;
    }
    if ((srcNum == 60 && srcDen == 1) || (srcNum == 60000 && srcDen == 1001)) {
        outNum = 30000;
        outDen = 1001;
        return true;
    }
    outNum = 24000;
    outDen = 1001;
    return true;
}

bool fpga480iMinFpsOk(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0)
        return false;
    return static_cast<long long>(fpsNum) * 1001 >= 24000LL * fpsDen;
}

bool fpga240pMinFpsOk(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0)
        return false;
    return static_cast<long long>(fpsNum) * 1001 >= 24000LL * fpsDen;
}

bool fpga480pMinFpsOk(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0)
        return false;
    return static_cast<long long>(fpsNum) * 1001 >= 24000LL * fpsDen;
}


std::string fpgaNtscSourceProfileName(const std::string& prototype, const std::string& raster,
                                      bool filteringOff) {
    return std::string("MiSTerPlex-FPGA-") + (prototype == "idr" ? "IDR-" : "IP-") + raster +
           "-src30" + (filteringOff ? "-filter-off" : "-filter-on");
}

int fpga480pCodedWidthCeiling() {
    const char* cw = std::getenv("MPX_FPGA_480P_CODED_W");
    return (cw && std::atoi(cw) == 640) ? 640 : 624;
}

FpgaAuSafeClamp fpgaAuSafeClamp(bool tier480, long long offsetMs, bool seekFlat) {
    FpgaAuSafeClamp cl =
        (seekFlat && offsetMs > 0) ? FpgaAuSafeClamp{350, 18}
        : (offsetMs > 0)
            ? (tier480 ? FpgaAuSafeClamp{1000, 30} : FpgaAuSafeClamp{900, 28})
            : (tier480 ? FpgaAuSafeClamp{2500, 60} : FpgaAuSafeClamp{2800, 55});
    // Keep every rung at or above its validateWeakLadder() bitrate floor, or
    // resolve falls through to Part with a misleading "remote transcoder" detail.
    const int floorKbps = tier480 ? kBitrateFloor480iKbps : kBitrateFloor240pKbps;
    if (cl.maxBitrateKbps < floorKbps)
        cl.maxBitrateKbps = floorKbps;
    return cl;
}

bool fpgaAuSafeTier480(const WeakLadder& weak) {
    if (weak.profileName == "480p" || weak.profileName == "480i" ||
        weak.profileName == "fpga-h264-480p")
        return true;
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    return h >= 480 || w >= 480;
}

bool validateWeakLadder(const WeakLadder& weak, std::string* why) {
    int w = 0, h = 0;
    auto fail = [&](const std::string& msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (!parseResolution(weak.videoResolution, w, h))
        return fail("videoResolution must be WxH");
    if (isFpgaPlexProfile(weak)) {
        const bool film =
            (weak.fpgaFpsNum == 24 && weak.fpgaFpsDen == 1) ||
            (weak.fpgaFpsNum == 24000 && weak.fpgaFpsDen == 1001);
        const bool tier240 = (w <= 320 && h <= 240);
        // TRUE640: 480p FPGA cores with PLEX_DECODE_CODED_640 decode 40 MB/row
        // (CAPS max_width 640). 624 stays legal for the older 39-MB cores.
        const bool tier480 = (w <= 640 && h <= 480 && (w > 320 || h > 240));
        // NTSC-raster source-rate profiles (no -r pin) carry 30/1 or 30000/1001 only.
        if (weak.fpgaProfileVariant.find("-src30-") != std::string::npos) {
            if (!fpsIsTrue30(weak.fpgaFpsNum, weak.fpgaFpsDen) || (!tier240 && !tier480))
                return fail("FPGA source-rate profile requires 30/1 or 30000/1001 at 240p or 624x480/640x480");
        } else if (!film || (!tier240 && !tier480)) {
            return fail("FPGA profile requires 240p or 624x480/640x480 film-rate ladder");
        }
    }
    if (weak.videoCodec != "h264")
        return fail("videoCodec must be h264");
    if (weak.audioCodec != "aac")
        return fail("audioCodec must be aac");
    // 720p PRESENT=fpga is host-ffmpeg → I420 DDR (not fabric CABAC). Allow
    // baseline|main. Keep baseline-only for ≤480p profiles (legacy ladder).
    if (weak.h264Profile != "baseline" && weak.h264Profile != "main")
        return fail("H.264 profile must be baseline or main");
    if (w >= 1280 || h >= 720) {
        if (w > 1280 || h > 720)
            return fail("current built-in profiles stop at 1280x720");
        if (weak.h264Level > 31)
            return fail("H.264 level must not exceed 3.1 for 720p ARM decode path");
        if (weak.h264Level < 31)
            return fail("720p requires H.264 level 3.1 (level 3.0 MaxFS is insufficient)");
        if (weak.maxVideoBitrateKbps < 1500)
            return fail("720p profile bitrate is too low");
    } else if (weak.h264Profile != "baseline" &&
               !(weak.highCabac && weak.h264Profile == "main")) {
        return fail("H.264 profile must be baseline for ≤480p profiles");
    } else if (weak.h264Level > 30) {
        return fail("H.264 level must not exceed 3.0 for 240p/480p profiles");
    } else if (w >= 640 || h >= 480) {
        // Host-paint 480i present bank is full BT.601 720×480 (not 640/624 coded
        // FPGA 480p). Allow exact 720×480; anything wider/taller still needs 720p.
        if (w > 720 || h > 480)
            return fail("use 720p profile for resolutions above 720x480");
        if (w > 640 && !(w == 720 && h == 480))
            return fail("use 720x480 (480i host) or 640x480 (480p) ladder");
        if (weak.maxVideoBitrateKbps < 2000)
            return fail("480p/480i profile bitrate is too low");
    } else if (weak.maxVideoBitrateKbps < kBitrateFloor240pKbps) {
        return fail("240p profile bitrate is too low");
    }
    if (weak.videoQuality <= 0 || weak.maxVideoBitrateKbps <= 0)
        return fail("videoQuality and maxVideoBitrate must be positive");
    return true;
}

namespace {
thread_local std::string tlsDecisionMismatch;
std::string xmlAttr(const std::string& tag, const char* name) {
    const std::string key = std::string(" ") + name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    const size_t e = tag.find('"', p);
    return e == std::string::npos ? std::string() : tag.substr(p, e - p);
}
int xmlInt(const std::string& tag, const char* name) {
    const std::string v = xmlAttr(tag, name);
    if (v.empty()) return 0;
    if (v.find('.') != std::string::npos) {   // "3.0" style level
        const double d = std::atof(v.c_str());
        return static_cast<int>(d * 10.0 + 0.5);
    }
    return std::atoi(v.c_str());
}
} // namespace

DecisionVideo parseDecisionVideo(const std::string& xml) {
    DecisionVideo v;
    size_t pos = 0;
    while ((pos = xml.find("<Stream ", pos)) != std::string::npos) {
        const size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        const std::string tag = xml.substr(pos, end - pos);
        pos = end;
        if (xmlAttr(tag, "streamType") != "1") continue;
        v.found = true;
        v.codec = lowerCopy(xmlAttr(tag, "codec"));
        v.profile = lowerCopy(xmlAttr(tag, "profile"));
        v.level = xmlInt(tag, "level");
        v.refFrames = xmlInt(tag, "refFrames");
        v.bitDepth = xmlInt(tag, "bitDepth");
        v.width = xmlInt(tag, "width");
        v.height = xmlInt(tag, "height");
        if (v.partDecision.empty())
            v.partDecision = lowerCopy(xmlAttr(tag, "decision"));
        break;
    }
    if (const size_t p = xml.find("<Part "); p != std::string::npos) {
        const size_t end = xml.find('>', p);
        const std::string tag = xml.substr(p, end == std::string::npos ? std::string::npos : end - p);
        if (v.partDecision.empty())
            v.partDecision = lowerCopy(xmlAttr(tag, "decision"));
        v.partKey = xmlAttr(tag, "key");
    }
    if (const size_t m = xml.find("<Media "); m != std::string::npos) {
        const std::string tag = xml.substr(m, xml.find('>', m) - m);
        if (v.codec.empty()) v.codec = lowerCopy(xmlAttr(tag, "videoCodec"));
        // Media@videoProfile is the SOURCE profile ("high" for the home PMS's Constrained Baseline variant
        // output, glass-runs/home-pms-hwenc-1011 variant-key6): never used. Stream@profile is logged only.
#ifdef MUT_DECISION_PROFILE_FALLBACK   // test-only mutant: 35c510937's Media@videoProfile fallback
        if (v.profile.empty()) v.profile = lowerCopy(xmlAttr(tag, "videoProfile"));
#endif
        if (!v.width) v.width = xmlInt(tag, "width");
        if (!v.height) v.height = xmlInt(tag, "height");
        if (!v.codec.empty()) v.found = true;
    }
    if (const size_t t = xml.find("<TranscodeSession "); t != std::string::npos) {
        const std::string tag = xml.substr(t, xml.find('>', t) - t);
        v.hwEncoding = xmlAttr(tag, "transcodeHwEncoding");
    }
    return v;
}

bool decisionIsDirectPlay(const DecisionVideo& v) {
    const std::string& d = v.partDecision;
    return d == "directplay" || d == "direct play" || d == "copy" || d == "directstream" ||
           d == "direct stream";
}

namespace {
thread_local DecisionVideo tlsLastDecisionVideo;
} // namespace

DecisionVideo lastDecisionVideo() { return tlsLastDecisionVideo; }

std::string decisionCoreMismatch(const DecisionVideo& v, const WeakLadder& weak) {
#ifdef MUT_DECISION_CHECK_OFF   // test-only mutant: the decision is never checked
    (void)v; (void)weak;
    return {};
#endif
    // Only codec and OUTPUT size are trustworthy in a transcode decision (home PMS 1011: Media@videoProfile is
    // the source's, Stream has no profile/level/refFrames). Profile, refs, level, bit depth: the first-AU SPS gate.
    if (!v.found) return {};
    if (!v.codec.empty() && v.codec != "h264")
        return "codec " + v.codec + " (want h264)";
#ifdef MUT_DECISION_PROFILE_FALLBACK
    if (!v.profile.empty() && v.profile != "baseline" && v.profile != "constrained baseline" &&
        weak.h264Profile != "high")
        return "profile " + v.profile + " (core takes baseline)";
#endif
#ifdef MUT_DECISION_ZERO_IS_MISMATCH   // test-only mutant: an unreported (0) level/refs counts as a mismatch
    if (v.level == 0 || v.refFrames == 0)
        return "level/refs " + std::to_string(v.level) + "/" + std::to_string(v.refFrames) + " (unreported)";
#endif
    // Round E 1011 (reviewer 29729): Stream level/refFrames/profile are absent or 0 (or the SOURCE's) in a transcode
    // decision: unknown, never a mismatch. Only codec and output size count; the first-AU SPS gate owns the rest.
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    if (w > 0 && v.width > w) return "width " + std::to_string(v.width) + " (limit " + std::to_string(w) + ")";
    if (h > 0 && v.height > h) return "height " + std::to_string(v.height) + " (limit " + std::to_string(h) + ")";
    return {};
}

WeakLadder lowerPmsLadder(const WeakLadder& weak) {
    WeakLadder w = weak;
    w.pmsLowered = true;
    // A forced transcode stays on the 30 fps cap. 60 is direct-play only.
#ifndef MUT_480P60_LOWER_KEEPS60
    w.directPlayFpsCap = 30;
#endif
    int rw = 0, rh = 0;
    parseResolution(w.videoResolution, rw, rh);
    const bool tier480 = rh >= 480 || rw >= 480;
    // Floors: validateWeakLadder's 480-tier 2000 kbps (Baseline), the High cap path's 1000,
    // 240p kBitrateFloor240pKbps.
    const int floorKbps = w.h264Profile == "high" ? 1000 : tier480 ? 2000 : kBitrateFloor240pKbps;
    const int lowered = w.maxVideoBitrateKbps * 3 / 4;
    w.maxVideoBitrateKbps = lowered > floorKbps ? lowered : (w.maxVideoBitrateKbps < floorKbps ? w.maxVideoBitrateKbps : floorKbps);
    if (w.videoQuality > 30) w.videoQuality = 30;
    return w;
}

WeakLadder owenbCavlcRetryLadder(const WeakLadder& weak, const std::string& reason, int cavlcMaxKbps) {
    WeakLadder w = weak;
    if (cavlcMaxKbps <= 0 || reason.find("CAVLC at ") == std::string::npos)
        return w;
#ifndef MUT_OWENB_CAVLC_RETRY_NO_CAP
    const int want = std::max(1000, cavlcMaxKbps * 85 / 100);
    // The OB-only cap (main's ladder is the legacy one; ladderForPlaySession carries obMaxKbps into the OB ladder).
    if (w.obMaxKbps <= 0 || w.obMaxKbps > want)
        w.obMaxKbps = want;
    if (w.owenb && (w.maxVideoBitrateKbps <= 0 || w.maxVideoBitrateKbps > want))
        w.maxVideoBitrateKbps = want;
#endif
    return w;
}

ObSourceEntropy parseObSourceEntropy(const std::string& xml) {
    ObSourceEntropy e;
    size_t sp = 0;
    while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
        const auto end = xml.find('>', sp);
        if (end == std::string::npos)
            break;
        const std::string slice = xml.substr(sp, end - sp);
        if (slice.find("streamType=\"1\"") != std::string::npos || slice.find("type=\"video\"") != std::string::npos) {
            const auto c = slice.find(" cabac=\"");
            if (c != std::string::npos && c + 8 < slice.size())
                e.cabac = slice[c + 8] == '1' ? 1 : slice[c + 8] == '0' ? 0 : -1;
            e.profile = lowerCopy(attrIn(slice, "profile"));
            e.codec = lowerCopy(attrIn(slice, "codec"));
            const std::string br = attrIn(slice, "bitrate");
            if (!br.empty())
                e.bitrateKbps = static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
            break;
        }
        sp = end + 1;
    }
    if (e.profile.empty())
        e.profile = lowerCopy(attr(xml, "Media", "videoProfile"));
    if (e.codec.empty())
        e.codec = lowerCopy(attr(xml, "Media", "videoCodec"));
    if (e.bitrateKbps <= 0) {
        const std::string br = attr(xml, "Media", "bitrate");
        e.bitrateKbps = br.empty() ? 0 : static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
    }
#ifndef MUT_OB_PREPLAY_PROFILE_IGNORED
    if (e.cabac < 0 && e.profile.find("baseline") != std::string::npos)
        e.cabac = 0;   // Baseline / Constrained Baseline: CAVLC only (H.264 A.2.1)
#endif
    return e;
}

bool owenbPrePlayCavlcLadder(WeakLadder& weak, const ObSourceEntropy& src, int hardKbps, int capKbps) {
#ifndef MUT_OB_PREPLAY_LEGACY_TOO
    if (!weak.owenb)
        return false;
#endif
    if (hardKbps <= 0 || capKbps <= 0)
        return false;
#ifndef MUT_OB_PREPLAY_ANY_ENTROPY
    if (src.cabac != 0)
        return false;
#endif
#ifndef MUT_OB_PREPLAY_NO_RATE_CHECK
    if (src.bitrateKbps <= hardKbps)
        return false;
#endif
    const int want = std::max(1000, capKbps * 85 / 100);
    if (weak.maxVideoBitrateKbps > 0 && weak.maxVideoBitrateKbps <= want)
        return false;
    weak.maxVideoBitrateKbps = want;
    return true;
}

bool owenbTranscodeSafeLadder(WeakLadder& weak, const ObSourceEntropy& src, int safeKbps) {
#ifndef MUT_OB_TSAFE_LEGACY_TOO
    if (!weak.owenb)
        return false;
#endif
    if (safeKbps <= 0 || src.codec.empty())
        return false;
#ifndef MUT_OB_TSAFE_H264_TOO
    if (src.codec == "h264" || src.codec == "avc" || src.codec == "avc1")
        return false;   // H.264 may direct-play (CABAC from the file); the in-session gate covers its CAVLC
#endif
    if (weak.maxVideoBitrateKbps > 0 && weak.maxVideoBitrateKbps <= safeKbps)
        return false;
    weak.maxVideoBitrateKbps = safeKbps;   // the resolution (720p) is kept
    return true;
}

std::string takeDecisionMismatch() {
    std::string m;
    m.swap(tlsDecisionMismatch);
    return m;
}

std::string plexVideoEncodeFlags(const WeakLadder& weak) {
    if (!isFpgaPlexProfile(weak)) return {};
    const std::string& v = weak.fpgaProfileVariant;   // MiSTerPlex-FPGA-<IP|IDR>-<tier>-<23976|24|src30>-filter-<on|off>
    const bool idr = v.find("-IDR-") != std::string::npos;
    const bool filterOff = v.find("-filter-off") != std::string::npos;
    std::string rate;
    if (v.find("-23976-") != std::string::npos) rate = " -r 24000/1001";
    else if (v.find("-24-") != std::string::npos) rate = " -r 24";
    else if (v.find("-src30-") != std::string::npos) rate = " -r 30000/1001";
    const int level = weak.h264Level > 0 ? weak.h264Level : 30;
    std::ostringstream f;
    // "-c:v libx264" first, as in the XMLs (Oct 10: PMS ran the software pipeline for that XML with HW on);
    // "-profile:v baseline -refs 1 -bf 0" also bind a hardware encoder that ignores -x264opts (H1/H2).
    // High core (MPX_HIGH_NO_CAVLC) cannot take the baseline CAVLC ladder.
    // Main + cabac=1, still one ref, no B, no 8x8, no weighted pred.
    // 101: reviewer still emitted CAVLC with these flags while they started with -c:v libx264.
    // A hardware transcoder drops that encoder pin and the rest of the block with it,
    // including -profile:v main. High leaves the encoder unnamed so -profile:v main binds.
    const char* profile = weak.highCabac ? "main" : "baseline";
    const char* cabac = weak.highCabac ? "cabac=1" : "cabac=0";
    if (!weak.highCabac)
        f << "-c:v libx264 ";
    f << "-profile:v " << profile << " -level " << level / 10 << "." << level % 10 << rate
      << " -refs 1 -bf 0 -x264opts " << cabac
      << ":bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none"
      << ":keyint=" << (idr ? 1 : 24) << ":min-keyint=" << (idr ? 1 : 24)
      << ":scenecut=0:open-gop=0:intra-refresh=0:threads=1:sliced-threads=0:slices=1"
         ":vbv-maxrate=4000:vbv-bufsize=1000:qpmin=10:qpmax=40:no-deblock=" << (filterOff ? 1 : 0);
#ifdef MUT_VEF_CABAC_ON   // test-only mutant: the flags lose the CAVLC constraint
    std::string out = f.str();
    const auto cabacAt = out.find("cabac=0");
    if (cabacAt != std::string::npos)
        out.replace(cabacAt, 7, "cabac=1");
    return out;
#endif
    return f.str();
}

WeakLadder owenbWeakLadder(bool wan) {
    WeakLadder w;
    w.owenb = true;
    w.profileName = "owenb";
    w.videoResolution = "1280x720";
    w.maxVideoBitrateKbps = wan ? 6000 : 8000;
    w.h264Profile = "high";
    w.h264ProfileParam = "baseline,main,high";
    w.h264Level = 41;
    w.clientProfileName.clear();
    w.directPlayFpsCap = 30;
    return w;
}

namespace {
bool envFlagOn(const char* e) {
    if (!e || !e[0])
        return false;
    std::string v = e;
    for (char& c : v)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Above 30 fps and at most 60 fps. 60/1 and 60000/1001 pass; 30-family and
// anything faster than 60 do not.
bool allow480p60Fps(int num, int den) {
    if (num <= 0 || den <= 0)
        return false;
    const long long n = num;
    const long long d = den;
#ifndef MUT_480P60_NO_FPS_MAX
    if (n > 60LL * d)
        return false;
#endif
#ifndef MUT_480P60_ANY_FPS
    if (n <= 30LL * d)
        return false;
#endif
    return true;
}

bool allow480p60Geometry(int width, int height) {
    if (width <= 0 || height <= 0)
        return false;
#ifdef MUT_480P60_NO_BOX
    return true;
#else
#ifndef MUT_480P60_NO_WIDTH
    if (width > kAllow480p60MaxWidth)
        return false;
#endif
#ifndef MUT_480P60_NO_HEIGHT
    if (height > kAllow480p60MaxHeight)
        return false;
#endif
    return true;
#endif
}
} // namespace

bool allow480p60Enabled() {
#ifdef MUT_ALLOW_480P60_IGNORED
    return false;
#endif
#ifdef MUT_ALLOW_480P60_FORCED
    return true;
#endif
    if (const char* e = std::getenv("MPX_ALLOW_480P60"); e && e[0])
        return envFlagOn(e);
    return envFlagOn(std::getenv("ALLOW_480P60"));
}

bool owenbApplyAllow480p60(WeakLadder& weak, const Allow480p60Source& src) {
#ifndef MUT_480P60_LEGACY_TOO
    if (!weak.owenb)
        return false;
#endif
#ifndef MUT_480P60_ON_LOWERED
    if (weak.pmsLowered)
        return false;
#endif
    if (!allow480p60Enabled())
        return false;
    if (!allow480p60Geometry(src.width, src.height))
        return false;
    if (!allow480p60Fps(src.fpsNum, src.fpsDen))
        return false;
#ifndef MUT_480P60_NO_CODEC
    if (!videoCodecIsH264(src.codec))
        return false;
#endif
#ifndef MUT_480P60_NO_BITDEPTH
    if (src.bitDepth > 8)
        return false;
#endif
#ifndef MUT_480P60_NO_BITRATE
    if (src.bitrateKbps > 0 && weak.maxVideoBitrateKbps > 0 &&
        src.bitrateKbps > weak.maxVideoBitrateKbps)
        return false;
#endif
    weak.directPlayFpsCap = 60;
    return true;
}

bool owenbApplyAllow480p60FromMeta(WeakLadder& weak, const std::string& metadataXml,
                                   int width, int height, int fpsNum, int fpsDen) {
    Allow480p60Source src;
    src.width = width;
    src.height = height;
    src.fpsNum = fpsNum;
    src.fpsDen = fpsDen;
    size_t sp = 0;
    while ((sp = metadataXml.find("<Stream", sp)) != std::string::npos) {
        const auto end = metadataXml.find('>', sp);
        if (end == std::string::npos)
            break;
        const std::string slice = metadataXml.substr(sp, end - sp);
        const bool video = slice.find("streamType=\"1\"") != std::string::npos ||
                           slice.find("type=\"video\"") != std::string::npos;
        if (video) {
            src.codec = lowerCopy(attrIn(slice, "codec"));
            const std::string br = attrIn(slice, "bitrate");
            if (!br.empty())
                src.bitrateKbps = static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
            const std::string bd = attrIn(slice, "bitDepth");
            if (!bd.empty())
                src.bitDepth = static_cast<int>(std::strtol(bd.c_str(), nullptr, 10));
            break;
        }
        sp = end + 1;
    }
    if (src.codec.empty())
        src.codec = lowerCopy(attr(metadataXml, "Media", "videoCodec"));
    if (src.bitrateKbps <= 0) {
        const std::string br = attr(metadataXml, "Media", "bitrate");
        if (!br.empty())
            src.bitrateKbps = static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
    }
    if (src.bitDepth <= 0) {
        const std::string bd = attr(metadataXml, "Media", "bitDepth");
        if (!bd.empty())
            src.bitDepth = static_cast<int>(std::strtol(bd.c_str(), nullptr, 10));
    }
    return owenbApplyAllow480p60(weak, src);
}

std::string plexClientProfileExtra(const WeakLadder& weak, bool namedProfile) {
    if (weak.owenb) {
        std::ostringstream o;
        const int level = weak.h264Level > 0 ? weak.h264Level : 41;
        o << "add-limitation(scope=videoCodec&scopeName=h264&type=match&name=video.profile"
          << "&list=baseline,main,high&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level"
          << "&value=" << level << "&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.bitDepth"
          << "&value=8&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.width"
          << "&value=1280&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.height"
          << "&value=720&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.frameRate"
#ifdef MUT_480P60_PROFILE_STALE
          << "&value=30&replace=true)+"
#else
          << "&value=" << (weak.directPlayFpsCap == 60 ? 60 : 30) << "&replace=true)+"
#endif
          << "add-limitation(scope=videoAudioCodec&scopeName=" << weak.audioCodec
          << "&type=upperBound&name=audio.channels&value=2&replace=true)";
        return o.str();
    }
#ifdef MUT_NAMED_EXTRA_DESTRUCTIVE   // test-only mutant: 35c510937..b349bc9d1, the named XML's target is replaced
    namedProfile = false;
#endif
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    std::ostringstream o;
    // Round E 1011: with a profile name the XML's TranscodeTarget (and its VideoEncodeFlags Setting) must survive.
    if (!namedProfile)
    o << "add-transcode-target(type=videoProfile&context=streaming&protocol=http"
      // PMS universal/start.mp4 is the HTTP entrypoint, but its working
      // streaming transcode target is TS; advertising mp4 here can make PMS
      // return an empty video/mp4 response.
      << "&container=mpegts&videoCodec=" << weak.videoCodec << "&audioCodec=" << weak.audioCodec
      << "&replace=true)+"
      << "add-transcode-target-audio-codec(type=videoProfile&context=streaming&protocol=http"
      << "&audioCodec=" << weak.audioCodec << "&replace=true)+";
    o << "add-limitation(scope=videoCodec&scopeName=h264&type=match&name=video.profile"
      << "&list=" << weak.h264Profile << "&replace=true)+"
      << "add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level"
      << "&value=" << weak.h264Level << "&replace=true)+"
      << "add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
      << "&context=streaming&protocol=http&type=match&name=video.profile"
      << "&list=" << weak.h264Profile << "&isRequired=true)+"
      << "add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
      << "&context=streaming&protocol=http&type=upperBound&name=video.level"
      << "&value=" << weak.h264Level << "&isRequired=true)";
    // FPGA Baseline variant contract (was only in the per-variant server XML the name selected):
    // refFrames 1, 8-bit, <=30 fps, stereo AAC. The encoder flags (baseline/CAVLC/ref 1/no B, keyint, deblock)
    // travel as add-transcode-target-settings VideoEncodeFlags (plexVideoEncodeFlags; home PMS 1011 S1).
#ifndef MUT_NO_VEF_TERM   // test-only mutant: the settings term is not sent
    if (isFpgaPlexProfile(weak) && !namedProfile)
        o << "+add-transcode-target-settings(type=videoProfile&context=streaming&protocol=http&VideoEncodeFlags="
          << urlEncodeQuery(plexVideoEncodeFlags(weak)) << ")";
#endif
    // F531d: per-request (client-sent, no server profile) geometry caps on the h264 transcode target.
    if (isFpgaPlexProfile(weak) && !weak.pmsLowered && w > 0 && h > 0) {
        const std::string ttg = "+add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
                                "&context=streaming&protocol=http&type=upperBound&name=";
        o << ttg << "video.width&value=" << w << "&isRequired=true)"
          << ttg << "video.height&value=" << h << "&isRequired=true)";
    }
    if (isFpgaPlexProfile(weak))
        o << "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.refFrames"
          << "&value=1&replace=true)"
          << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.bitDepth"
          << "&value=8&replace=true)"
          << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.frameRate"
          << "&value=30&replace=true)"
          << "+add-limitation(scope=videoAudioCodec&scopeName=" << weak.audioCodec
          << "&type=upperBound&name=audio.channels&value=2&replace=true)";
    // NO-REQUIRED-PROFILE lowered retry: every bound also isRequired on the h264 transcode target.
    if (weak.pmsLowered) {
        const std::string tt = "+add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
                               "&context=streaming&protocol=http&type=upperBound&name=";
        o << tt << "video.refFrames&value=" << (weak.h264Profile == "high" ? 4 : 1) << "&isRequired=true)"
          << tt << "video.bitDepth&value=8&isRequired=true)";
        if (w > 0 && h > 0)
            o << tt << "video.width&value=" << w << "&isRequired=true)"
              << tt << "video.height&value=" << h << "&isRequired=true)";
    }
    if (w > 0 && h > 0) {
        o << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.width"
          << "&value=" << w << "&replace=true)"
          << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.height"
          << "&value=" << h << "&replace=true)";
    }
    if (namedProfile) {
        std::string named = o.str();
        for (size_t p; (p = named.find("&replace=true")) != std::string::npos;) named.erase(p, 13);
        return named;
    }
    return o.str();
}

std::string plexClientCapabilities(const WeakLadder& weak) {
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    std::ostringstream o;
    o << "protocols=http-streaming-video,http-mp2t-video;"
      << "videoDecoders=h264{profile:" << weak.h264Profile << "&resolution:" << w << "x" << h
      << "&level:" << weak.h264Level << "};"
      << "audioDecoders=" << weak.audioCodec << "{channels:2}";
    return o.str();
}

namespace {
bool hostLooksPrivateLan(const std::string& host) {
    if (host == "localhost" || host.rfind("127.", 0) == 0)
        return true;
    if (host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0)
        return true;
    // RFC1918 172.16/12
    if (host.rfind("172.", 0) == 0) {
        const auto second = host.find('.', 4);
        if (second != std::string::npos) {
            const int oct = std::atoi(host.c_str() + 4);
            if (oct >= 16 && oct <= 31)
                return true;
        }
    }
    return false;
}

// lan for private PMS; wan for public/shared remote bases (location=lan on a
// public address confuses some shared PMS transcoders and is wrong metadata).
const char* plexLocationForBase(const std::string& base) {
    std::string host;
    auto scheme = base.find("://");
    if (scheme == std::string::npos)
        host = base;
    else {
        size_t hostStart = scheme + 3;
        size_t hostEnd = base.find('/', hostStart);
        std::string hostPort = hostEnd == std::string::npos
                                   ? base.substr(hostStart)
                                   : base.substr(hostStart, hostEnd - hostStart);
        auto at = hostPort.find('@');
        if (at != std::string::npos)
            hostPort = hostPort.substr(at + 1);
        auto colon = hostPort.rfind(':');
        host = (colon != std::string::npos && hostPort.find(':') == colon)
                   ? hostPort.substr(0, colon)
                   : hostPort;
    }
    if (host.empty())
        return "lan";
    return hostLooksPrivateLan(host) ? "lan" : "wan";
}
} // namespace

namespace {
std::string weakVideoProfileParam(const WeakLadder& weak) {
    return weak.h264ProfileParam.empty() ? weak.h264Profile : weak.h264ProfileParam;
}
int weakAudioChannels(const WeakLadder& weak) {
    return weak.audioChannels > 0 ? weak.audioChannels : 2;
}
} // namespace

bool plexTargetIsPrivateLan(const std::string& url) {
    return !url.empty() && std::strcmp(plexLocationForBase(url), "lan") == 0;
}

namespace {
thread_local std::function<bool()> tlsResolveAbort;
} // namespace

void setResolveAbortCheck(std::function<bool()> check) { tlsResolveAbort = std::move(check); }

bool resolveAborted() {
#ifdef MUT_RESOLVE_NO_ABORT   // test-only mutant: a superseded resolve still runs its decision
    return false;
#else
    return tlsResolveAbort && tlsResolveAbort();
#endif
}

bool plexPrewarmProbeEnabled() {
    // Unset is on. 075 reviewer: the status-only GET drains the universal start into
    // /dev/null for the full 12s max-time, then a second GET waits on the body.
    // That is the ~20s of idle chevron between the decision and "resolved".
    // MPX_PREWARM=0 keeps the two-GET path.
    const char* prewarm = std::getenv("MPX_PREWARM");
    if (!prewarm)
        return true;
    return prewarm[0] != '\0' && std::string(prewarm) != "0";
}

// Remote (location=wan) start/decision buffer hint. The 2026-09-26 snapshot added
// "&acceptWav=1&mediaBufferSize=102400": 102400 KiB at the 2500 kbps ladder is ~335 s, the
//  lab remote EOFs (Fifth Element 330.6 s, Quantum Leap 335.6 s from offset 0; reviewer's
// playbar-keepalive archaeology 1011). Every release before it sent neither. Default: none.
// MPX_REMOTE_MEDIA_BUFFER_KB=<KiB> re-adds mediaBufferSize only, for the A/B proof.
std::string remoteMediaBufferParams() {
#ifdef MUT_REMOTE_BUFFER_102400   // test-only mutant: the 2026-09-26 shape
    return "&acceptWav=1&mediaBufferSize=102400";
#else
    const char* kb = std::getenv("MPX_REMOTE_MEDIA_BUFFER_KB");
    if (!kb || !kb[0])
        return {};
    char* end = nullptr;
    const long v = std::strtol(kb, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 1048576)
        return {};
    return "&mediaBufferSize=" + std::to_string(v);
#endif
}

std::string buildUniversalTranscodeUrl(const std::string& base,
                                       const std::string& metadataKey,
                                       const std::string& token,
                                       const std::string& session,
                                       int64_t offsetMs,
                                       const WeakLadder& weak,
                                       std::string* why) {
    if (isFpgaPlexProfile(weak) && !validateWeakLadder(weak, why))
        return {};
    const char* location = plexLocationForBase(base);
    std::ostringstream q;
    const bool wan = std::strcmp(location, "wan") == 0;
    // C939: plain transcode params (codec/profile/channels) go to every PMS.
    // FPGA-only keys (container, videoFrameRate) stay LAN-only: a shared PMS
    // answers those with a bare HTTP 400.
    // directPlay=1: accept a direct Part or a transcode, whichever the server returns.
    // High WAN (098): that direct path handed back a Baseline CAVLC AU after the
    // decision had already asked for main. Match the decision and refuse direct play.
    // F531c: the FPGA lowered retry (R6p) never depends on a ladder helper: strict Baseline,
    // no direct stream, so any PMS (no client profile) re-encodes to what the core can decode.
    const bool strictBaseline =
        !weak.owenb && weak.pmsLowered && isFpgaPlexProfile(weak) && !weak.highCabac;
    // F531d: an FPGA Baseline profile never direct plays or direct streams. With no client profile
    // installed, PMS answers directPlay=1 with "Direct play OK" and serves the source part (High,
    // 1920x814 on 198.51.100.153), which the Baseline core refuses at the SPS gate (profile_idc 100).
    const bool fpgaBaselineOnly = !weak.owenb && isFpgaPlexProfile(weak) && !weak.highCabac;
    // #207 r3: the decision (built from this URL) always says directPlay=0&directStream=0; a forced
    // remote start repeats it (glass 347e48b: start directPlay=1 on the decided session -> PMS 400).
    // #201/#234: subtitles=burn is ignored on a direct stream. Any burn pick must
    // transcode the video (not only when BURN_SUBS_FIX is on).
    const bool forceTranscode = misterplex::remoteStartForcesTranscode(
        misterplex::remoteH264TranscodeEnabled(), wan,
        weak.burnSubtitles || burnSubsFixForcesTranscode(weak.burnSubtitles && weak.burnSubsFixImage) ||
        (weak.owenb ? weak.pmsLowered
                    : ((wan && weak.highCabac) || strictBaseline || fpgaBaselineOnly)));
    q << base << "/video/:/transcode/universal/start.mp4"
      << "?hasMDE=1"
      << "&path=" << urlEncodeQuery(metadataKey)
      << "&mediaIndex=0&partIndex=0"
      << "&protocol=http&fastSeek=" << (wan ? "1" : "0")
      << "&directPlay=" << (forceTranscode ? "0" : "1")
      << "&directStream=" << (forceTranscode ? "0" : "1")
      << "&subtitleSize=100&audioBoost=100&location=" << location
      << (wan ? "" : "&copyts=1")
      << "&autoAdjustQuality=0"
      << (wan ? remoteMediaBufferParams() : std::string())
      << "&session=" << urlEncodeQuery(session)
      << "&videoQuality=" << weak.videoQuality
      << "&videoResolution=" << urlEncodeQuery(weak.videoResolution)
      << "&maxVideoBitrate=" << weak.maxVideoBitrateKbps
      << "&videoCodec=" << urlEncodeQuery(weak.videoCodec)
      << "&audioCodec=" << urlEncodeQuery(weak.audioCodec)
      << "&audioChannels=" << weakAudioChannels(weak)
      << "&videoProfile=" << urlEncodeQuery((strictBaseline || fpgaBaselineOnly) ? std::string("baseline")
                                                                                : weakVideoProfileParam(weak))
      << "&videoLevel=" << weak.h264Level;
    if (!wan && isFpgaPlexProfile(weak))
        q << "&container=mpegts&videoFrameRate="
          << urlEncodeQuery(weak.fpgaFpsDen == 1 ? std::to_string(weak.fpgaFpsNum)
              : std::to_string(weak.fpgaFpsNum) + "/" + std::to_string(weak.fpgaFpsDen));
    // Phase 4: PMS-side burn-in (preferred over dual-A9 FFmpeg subtitles filter).
    if (weak.audioStreamId > 0)
        q << "&audioStreamID=" << weak.audioStreamId;
    if (weak.burnSubtitles) {
        q << "&subtitles=burn";
        if (weak.subtitleStreamId >= 0)
            q << "&subtitleStreamID=" << weak.subtitleStreamId;
    } else if (weak.subtitleStreamId >= 0) {
        // subtitleStreamID=0 is off (Plex clients send that to clear the default).
        q << "&subtitleStreamID=" << weak.subtitleStreamId;
    }
    // PMS universal offset is SECONDS (companion / scrubber use ms).
    const int64_t offSec = universalOffsetSeconds(offsetMs);
    if (offSec > 0)
        q << "&offset=" << offSec;
    // #340: token is r.httpHeaders (libav), never this URL. WAN used to require a
    // query token for subprocess ffmpeg; MUT_340_PLAYABLE_QUERY restores that.
#ifdef MUT_340_PLAYABLE_QUERY
    if (!token.empty() && (!isFpgaPlexProfile(weak) || wan))
        q << "&X-Plex-Token=" << urlEncodeQuery(token);
#else
    (void)token;
#endif
    return q.str();
}

bool plexSendProfileExtra() {
    if (const char* e = std::getenv("MPX_SEND_PROFILE_EXTRA"); e && *e) {
        const std::string v = e;
        if (v != "0" && v != "off" && v != "none" && v != "no")
            return true;
    }
    return false;
}

bool plexClientSendsProfileExtra(const WeakLadder& weak, bool forceLadderCaps,
                                 const std::string& profileName) {
#ifdef MUT_NO_EXTRA_LAN   // test-only mutant: never Extra, even when opted in
    (void)weak;
    (void)forceLadderCaps;
    (void)profileName;
    return false;
#else
    (void)forceLadderCaps; // WAN Extra is opt-in; plain URL params carry the ladder.
    if (weak.owenb)
        return true;
    if (!profileName.empty())
        return true;
    if (plexSendProfileExtra())
        return true;
    return plexSendClientLadderCaps(weak);
#endif
}

std::string plexFfmpegHeaders(const std::string& sessionId, const std::string& token) {
    return plexFfmpegHeaders(sessionId, token, WeakLadder{});
}

std::string plexFfmpegHeaders(const std::string& sessionId, const std::string& token,
                              const WeakLadder& weak, bool forceLadderCaps,
                              const std::string& targetUrl) {
    std::ostringstream o;
    o << "X-Plex-Client-Identifier: " << kPlexClientId << "\r\n"
      << "X-Plex-Product: Plex Web\r\n"
      << "X-Plex-Version: " << kPlexWebVersion << "\r\n"
      << "X-Plex-Platform: Chrome\r\n"
      << "X-Plex-Platform-Version: 120.0\r\n"
      << "X-Plex-Device: Linux\r\n"
      << "X-Plex-Device-Name: Chrome\r\n"
      << "X-Plex-Model: bundled\r\n"
      << "X-Plex-Provides: player\r\n";
    const std::string profileName = plexRequestProfileName(weak, targetUrl);
    if (!profileName.empty())
        o << "X-Plex-Client-Profile-Name: " << profileName << "\r\n";
    // Goal B: FPGA Extra (CAVLC/ref 1) with no Profile-Name / no MiSTerPlex.xml.
    // 720p caps Extra still opt-in via plexSendClientLadderCaps.
    if (plexClientSendsProfileExtra(weak, forceLadderCaps, profileName)) {
        o << "X-Plex-Client-Capabilities: " << plexClientCapabilities(weak) << "\r\n"
          << "X-Plex-Client-Profile-Extra: " << plexClientProfileExtra(weak, !profileName.empty()) << "\r\n";
    }
    o << "X-Plex-Session-Identifier: " << sessionId << "\r\n";
    if (!token.empty())
        o << "X-Plex-Token: " << token << "\r\n";
    return o.str();
}

// #340: a libav playable URL never carries X-Plex-Token=. owenb_av_source sends
// r.httpHeaders as libav "headers". MUT_340_PLAYABLE_QUERY restores the old query.
static void appendPlayableTokenQuery(std::string& url, const std::string& token) {
#ifdef MUT_340_PLAYABLE_QUERY
    if (token.empty())
        return;
    url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("X-Plex-Token=") +
           urlEncodeQuery(token);
#else
    (void)url;
    (void)token;
#endif
}

static void ensurePlayableTokenHeader(ResolveResult& r, const std::string& token, const WeakLadder& weak = {}) {
    if (token.empty())
        return;
    if (r.httpHeaders.find("X-Plex-Token:") != std::string::npos)
        return;
    if (r.httpHeaders.empty())
        r.httpHeaders = plexFfmpegHeaders(makeSessionId(), token, weak);
    else
        r.httpHeaders += "X-Plex-Token: " + token + "\r\n";
}

bool hasHeader(const std::vector<std::pair<std::string, std::string>>& headers,
               const char* name) {
    for (const auto& h : headers) {
        if (h.first == name)
            return true;
    }
    return false;
}

bool plexHttpStatusOk(int httpStatus) {
    return httpStatus >= 200 && httpStatus < 300;
}

int parseCurlHttpCode(const std::string& curlWriteOut) {
    std::string out = curlWriteOut;
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    size_t start = 0;
    while (start < out.size() &&
           (out[start] == ' ' || out[start] == '\t' || out[start] == '\n' || out[start] == '\r'))
        ++start;
    if (out.size() - start < 3)
        return 0;
    // Take the last 3 digits if curl appended noise; prefer exact 3-char body.
    std::string code = (out.size() - start == 3) ? out.substr(start) : out.substr(out.size() - 3);
    if (code.size() != 3)
        return 0;
    for (char c : code) {
        if (c < '0' || c > '9')
            return 0;
    }
    return (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
}

PlexHttpNoBodyResult plexHttpGetNoBodyResult(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    int timeoutSec) {
    // Prefer curl HTTP status over body non-empty: /:/timeline may return empty 200,
    // and 401 Unauthorized is a 91-byte HTML body — !body.empty() false-OKed auth fails
    // (docs/evidence/timeline-stuck-20260730T173000Z).
    PlexHttpNoBodyResult r;
    const bool defaultIdentity = !hasHeader(headers, "X-Plex-Client-Identifier");
    std::vector<std::string> a = curlArgvPrefix(url, headers, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec), "-o", "/dev/null", "-w",
                       "%{http_code}", "-H", "Accept: application/xml"});
    HeaderList hdr;
    if (defaultIdentity)
        hdr = plexIdentityHeaders(hasHeader(headers, "X-Plex-Client-Profile-Name") ? std::string()
                                                                                  : defaultRequestProfileName());
    hdr.insert(hdr.end(), headers.begin(), headers.end());
    const std::string hdrCfg = appendHeaderArgv(a, hdr);
    appendUrlArgv(a, url);
    const ArgvRun run = runArgv(a, false, 0, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg);
    if (!run.spawned)
        return r;
    const std::string& out = run.out;
    r.httpStatus = parseCurlHttpCode(out);
    // curl may exit non-zero on 404 with -f; we do not use -f, so status is authoritative.
    r.ok = plexHttpStatusOk(r.httpStatus);
    return r;
}

bool plexHttpGetNoBody(const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       int timeoutSec) {
    return plexHttpGetNoBodyResult(url, headers, timeoutSec).ok;
}


// MPX_PREWARM: collapse the two consecutive cold GETs against the remote universal
// start (status-only probe + tiny body check) into one GET carrying the status and
// the leading bytes over a single TCP connection. The playback demux still opens
// its own connection to the PMS; this only removes one handshake and one PMS
// dispatch from the resolve window.
PlexStartProbe plexHttpGetStatusAndBody(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& clientProfileName, int timeoutSec) {
    PlexStartProbe r;
    // httpGet() identity block plus the probe headers; status is appended by -w as
    // a bare 3-digit code directly after the body (no newline — body may be binary).
    std::vector<std::string> a = curlArgvPrefix(url, headers, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec), "-w", "%{http_code}", "-H",
                       "Accept: application/xml"});
    HeaderList hdr = plexIdentityHeaders(hasHeader(headers, "X-Plex-Client-Profile-Name") ? std::string()
                                                                                           : clientProfileName);
    hdr.insert(hdr.end(), headers.begin(), headers.end());
    const std::string hdrCfg = appendHeaderArgv(a, hdr);
    appendUrlArgv(a, url);
#ifdef MUT_341_PROBE_ONCE   // test-only mutant: the WAN start probe bypasses the redirect router (one curl, -L)
    const ArgvRun run = runArgvOnce(a, false, 0, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg, nullptr);
#else
    const ArgvRun run = runArgv(a, false, 0, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg);
#endif
    if (!run.spawned)
        return r;
    // The body is MPEG-TS (NUL bytes): it is read byte-exact, or the trailing -w status is lost (http=0,
    // "remote universal start dead" on  lab C1, 2 of 2 with MPX_PREWARM=1).
#ifdef MUT_PREWARM_FGETS   // test-only mutant: the C1 FAIL read (a C string stops at the first NUL of TS data)
    const std::string out = std::string(run.out.c_str());
#else
    const std::string& out = run.out;
#endif
    if (out.size() < 3)
        return r;
    r.httpStatus = parseCurlHttpCode(out.substr(out.size() - 3));
    r.body = out.substr(0, out.size() - 3);
    return r;
}

PlexStartProbe plexHttpGetStatusAndHead(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& clientProfileName, int timeoutSec, size_t capBytes) {
    PlexStartProbe r;
    if (url.find('\0') != std::string::npos || capBytes == 0)
        return r;
    std::vector<std::string> a = curlArgvPrefix(url, headers, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec), "-D", "-", "-H",
                       "Accept: application/xml"});
    // FU8: one -K - config carrying identity + caller headers (was two configs, both dropped).
    HeaderList hh = plexIdentityHeaders(hasHeader(headers, "X-Plex-Client-Profile-Name") ? std::string()
                                                                                       : clientProfileName);
    hh.insert(hh.end(), headers.begin(), headers.end());
    const std::string hdrCfg = appendHeaderArgv(a, hh);
    appendUrlArgv(a, url);
    const ArgvRun run = runArgv(a, false, capBytes, nullptr, hdrCfg.empty() ? nullptr : &hdrCfg);
    if (!run.spawned)
        return r;
    const remote_start::CurlHeaderDump parsed = remote_start::parseCurlHeaderDump(run.out);
    r.httpStatus = parsed.httpStatus;
    r.body = parsed.body;
    return r;
}

// rs9: one attempt of the remote universal start probe (the resolve's probeOnce, callable from a
// fake-PMS latency test). capBytes > 0 (the default: MPX_REMOTE_PROBE_CAP_KB unset = 64 KiB) returns as soon
// as the headers and the first capBytes of media arrive. capBytes == 0 is the legacy path: a 12 s GET that
// drains the endless transcode body until --max-time, then (MPX_PREWARM=0, the prod boot line) a second 12 s
// body GET = the ~24.1 s of every glass remote start (1025: ms=24101; gw2 24203 ms; gw3 24196 ms).
RemoteStartProbeResult remoteStartProbeOnce(const std::string& start,
                                            const std::vector<std::pair<std::string, std::string>>& probeHdr,
                                            const std::string& profileName, size_t capBytes, bool prewarm) {
    RemoteStartProbeResult r;
#ifdef MUT_RS9_IGNORE_CAP   // test-only mutant: the byte cap is dropped (legacy drain)
    capBytes = 0;
#endif
    int http = 0;
    std::string body;
    bool capped = capBytes > 0;
#ifdef MUT_RS9_CAP0_CAPPED   // test-only mutant (review M2): cap=0 still takes the capped GET (returns empty)
    capped = true;
#endif
    if (capped) {
        const auto probe = plexHttpGetStatusAndHead(start, probeHdr, profileName, 12, capBytes);
        http = probe.httpStatus;
        body = probe.body;
    } else if (prewarm) {
        const auto probe = plexHttpGetStatusAndBody(start, probeHdr, profileName, 12);
        http = probe.httpStatus;
        body = probe.body;
    } else {
        http = plexHttpGetNoBodyResult(start, probeHdr, 12).httpStatus;
    }
    // 206/200 with body is ideal; 0-byte 200 or 4xx/5xx => dead transcoder.
    bool alive = plexHttpStatusOk(http) && http != 204;
    if (alive) {
        if (capBytes == 0 && !prewarm)
            body = httpGet(start, 12, probeHdr, profileName);
        if (body.size() < 64 || body.find("<html") != std::string::npos ||
            body.find("Bad Request") != std::string::npos || body.find("MediaContainer") != std::string::npos)
            alive = false;
    }
    r.httpStatus = http;
    r.body = std::move(body);
    r.alive = alive;
    return r;
}

namespace {

// OpenSubtitles.com REST (Api-Key + optional Bearer). Never logs secrets.
std::string opensubsHttp(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& apiKey, const std::string& bearer, int timeoutSec,
                         int* httpCodeOut = nullptr) {
    if (httpCodeOut)
        *httpCodeOut = 0;
    if (url.find('\0') != std::string::npos || apiKey.empty())
        return {};
    HeaderList hdr = {{"Api-Key", apiKey},
                      {"User-Agent", "misterplexd v1"},
                      {"Accept", "application/json"},
                      {"Content-Type", "application/json"}};
    if (!bearer.empty())
        hdr.emplace_back("Authorization", "Bearer " + bearer);
    std::vector<std::string> a = curlArgvPrefix(url, hdr, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec)});
    const bool postBody = (method == "POST" && !body.empty());
    if (method == "POST") {
        a.push_back("-X");
        a.push_back("POST");
        if (postBody) {
            // Body (login JSON carries OPENSUBTITLES_PASSWORD) goes over stdin, never argv: not visible in ps.
            a.push_back("--data-binary");
            a.push_back("@-");
        }
    }
    // Api-Key / Bearer: curl -K via stdin, or -K /dev/fd/N when the body already owns stdin (no temp file).
#ifdef MUT_OPENSUBS_KEY_ON_ARGV
    for (const auto& kv : hdr)
        a.insert(a.end(), {"-H", kv.first + ": " + kv.second});
    const std::string hdrCfg;
#else
    const std::string hdrCfg = appendHeaderArgv(a, hdr);
#endif
    appendUrlArgv(a, url);
    if (httpCodeOut)   // status on its own last line, split off below (before "-- url")
        a.insert(a.end() - 2, {"-w", "\n%{http_code}"});
    std::string out =
        postBody ? runArgv(a, false, 0, &body, hdrCfg.empty() ? nullptr : &hdrCfg).out
                 : runArgvCapture(a, hdrCfg.empty() ? nullptr : &hdrCfg);
    if (httpCodeOut) {
        const auto nl = out.rfind('\n');
        if (nl != std::string::npos) {
            *httpCodeOut = parseCurlHttpCode(out.substr(nl + 1));
            out.erase(nl);
        }
    }
    return out;
}

std::string tryOpenSubsFallback(const std::string& metadataXml, ResolveResult& r) {
    using namespace opensubs;
    logKeyPresenceOnce();
    if (!fallbackEnabled())
        return {};
    // A PMS text stream is the soft-sidecar path. Explicit Off stays Off.
    if (!shouldQueryOpenSubs(r.tracks, r.partId, r.trackPick.subtitlesOff, pendingPlayMediaSubtitleId())) {
        std::fprintf(stderr, "misterplexd: opensubtitles: skipped (play has a text subtitle or subs are off)\n");
        return {};
    }

    Guids g = parseGuidsFromXml(metadataXml);
    if (g.title.empty() && !r.title.empty())
        g.title = r.title;
    const std::string path = cachePath(r.ratingKey, g.imdb, "en");
    if (shouldServeOpenSubsFromCache(fallbackEnabled(), true, cacheReadable(path))) {
        std::fprintf(stderr, "misterplexd: opensubtitles: source=cache plex_writes=0 path=%s\n", path.c_str());
        return path;
    }
    const std::string key = apiKey();
    if (key.empty())
        return {};
    // Quota backoff is checked before any request (login/search), not just before /download.
    static std::atomic<int64_t> quotaBackoffUntilSec{0};
    const int64_t nowSec = static_cast<int64_t>(::time(nullptr));
    if (nowSec < quotaBackoffUntilSec.load()) {
        std::fprintf(stderr, "misterplexd: opensubtitles: skipped (quota backoff)\n");
        return {};
    }
    std::string bearer;
    const std::string user = username();
    const std::string pass = password();
    // Login is optional: no user or password = anonymous download on the Api-Key alone.
    const bool anon = !shouldLogin(user, pass);
    if (!anon) {
        const std::string login =
            opensubsHttp("POST", "https://api.opensubtitles.com/api/v1/login", loginBodyJson(user, pass),
                         key, {}, 12);
        bearer = parseLoginToken(login);
    }
    const std::string searchUrl = buildSearchUrl(g, "en");
    if (searchUrl.empty())
        return {};
    const std::string searchJson = opensubsHttp("GET", searchUrl, {}, key, bearer, 15);
    const auto hits = parseSearchHits(searchJson);
    if (hits.empty()) {
        std::fprintf(stderr, "misterplexd: opensubtitles: search hits=0 pick file_id=0\n");
        return {};
    }
    const int64_t fileId = hits.front().fileId;
    std::fprintf(stderr, "misterplexd: opensubtitles: search hits=%zu pick file_id=%lld\n", hits.size(),
                 static_cast<long long>(fileId));
    int dlCode = 0;
    const std::string dlJson =
        opensubsHttp("POST", "https://api.opensubtitles.com/api/v1/download", downloadBodyJson(fileId), key,
                     bearer, 12, &dlCode);
    const DownloadStatus st = classifyDownload(dlCode, dlJson);
    if (st == DownloadStatus::QuotaExceeded) {
        // 406: daily download limit (anonymous: 5/day per IP). Back off until reset_time_utc (else 24 h).
        const int64_t until = quotaBackoffUntil(dlJson, nowSec);
        quotaBackoffUntilSec.store(until);
        std::fprintf(stderr, "misterplexd: opensubtitles: download limit reached (406, %s); backing off %llds\n",
                     anon ? "anonymous" : "account", static_cast<long long>(until - nowSec));
        return {};
    }
    if (st != DownloadStatus::Ok) {
        std::fprintf(stderr, "misterplexd: opensubtitles: download failed http=%d\n", dlCode);
        return {};
    }
    const std::string link = parseDownloadLink(dlJson);
    if (!subsLinkAllowed(link)) {
        std::fprintf(stderr, "misterplexd: opensubtitles: download link rejected (not https opensubtitles host)\n");
        return {};
    }
    // Download the srt (no Api-Key on the CDN hop — link is a signed URL).
    std::vector<std::string> a = curlArgvPrefix(link, {}, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", "20", "-H", "User-Agent: misterplexd v1",
                      "--proto", "=https", "--proto-redir", "=https", "--max-filesize", kMaxSrtBytesArg});
    appendUrlArgv(a, link);
    const std::string srt = runArgvCapture(a);
    if (srt.empty() || (srt.find("-->") == std::string::npos && srt.find("WEBVTT") == std::string::npos))
        return {};
    if (!writeCache(path, srt))
        return {};
    std::fprintf(stderr, "misterplexd: opensubtitles: source=download plex_writes=0 path=%s\n", path.c_str());
    return path;
}

// MPX_BROWSE_V2 (reviewer Q4): the ladder on buildhost fetches its src with ffprobe and ffmpeg and
// sends no X-Plex headers. An owning server's transcode handed to it therefore carries, as
// query parameters, the X-Plex set libav sends for the daemon's own universal fetches
// (plexFfmpegHeaders). Names already in the query are kept as they are.
std::string withPlexHeadersAsQuery(const std::string& url, const std::string& headerBlock) {
    std::string out = url;
    size_t i = 0;
    while (i < headerBlock.size()) {
        size_t e = headerBlock.find("\r\n", i);
        if (e == std::string::npos)
            e = headerBlock.size();
        const std::string line = headerBlock.substr(i, e - i);
        i = e + 2;
        const size_t c = line.find(": ");
        if (c == std::string::npos || c == 0)
            continue;
        const std::string name = line.substr(0, c);
        if (out.find("?" + name + "=") != std::string::npos ||
            out.find("&" + name + "=") != std::string::npos)
            continue;
        out += out.find('?') == std::string::npos ? '?' : '&';
        out += name + "=" + urlEncodeQuery(line.substr(c + 2));
    }
    return out;
}

struct LadderStartProbe {
    int httpStatus = 0; // 0: not read (the 16 KiB cap was reached first)
    size_t bytes = 0;
    bool media = false;
};

// GET url the way the ladder's ffmpeg does (no X-Plex headers, Range: bytes=0-) and read at
// most 16 KiB. media: a 2xx (or an unfinished stream past the cap) of >= 64 bytes that is not
// an XML/HTML error page.
// Daemon resolve no longer calls this (#275). The shell-path unit test still does.
[[maybe_unused]] LadderStartProbe probeStartLikeLadder(const std::string& url, int timeoutSec) {
    LadderStartProbe r;
    constexpr size_t kCap = 16384;
    std::vector<std::string> a = curlArgvPrefix(url, {}, true);
    a.insert(a.end(), {"--connect-timeout", "6", "--max-time", std::to_string(timeoutSec), "-A", "Lavf", "-H",
                       "Range: bytes=0-", "-o", "-", "-w", "\\n%{http_code}"});
    appendUrlArgv(a, url);
    // runArgv closes the pipe at the cap before it waits: a still-streaming curl ends on EPIPE.
    const ArgvRun run = runArgv(a, false, kCap);
    if (!run.spawned)
        return r;
    const std::string& out = run.out;
    const bool capped = out.size() >= kCap;
    std::string body = out;
    if (!capped) {
        const size_t nl = out.rfind('\n');
        if (nl != std::string::npos) {
            r.httpStatus = parseCurlHttpCode(out.substr(nl + 1));
            body = out.substr(0, nl);
        }
    }
    r.bytes = body.size();
    const std::string head = body.substr(0, 512);
    const bool errorPage = head.find("<html") != std::string::npos ||
                           head.find("<HTML") != std::string::npos ||
                           head.find("<?xml") != std::string::npos ||
                           head.find("MediaContainer") != std::string::npos ||
                           head.find("Bad Request") != std::string::npos;
    r.media = (capped || plexHttpStatusOk(r.httpStatus)) && r.httpStatus != 204 &&
              r.bytes >= 64 && !errorPage;
    return r;
}
} // namespace

std::vector<std::pair<std::string, std::string>> plexStartProbeHeaders(const std::string& sessionId,
                                                                       const std::string& token,
                                                                       const WeakLadder& weak,
                                                                       const std::string& startUrl) {
    std::vector<std::pair<std::string, std::string>> h = {
        {"X-Plex-Session-Identifier", sessionId},
        {"Accept", "*/*"},
    };
    if (!token.empty())
        h.push_back({"X-Plex-Token", token});
    const std::string pn = plexRequestProfileName(weak, startUrl);
    // High core: never send Profile-Name on the start probe (Goal B). LAN variant
    // names used to skip Profile-Extra and PMS locked the session to CAVLC (F257 096).
    if (!pn.empty() && !weak.highCabac)
        h.push_back({"X-Plex-Client-Profile-Name", pn});
    // The WAN probe is the GET that starts the transcoder (Range 0-4095, same
    // session as the later read). 174 L3/L6: videoProfile=baseline on the URL
    // still returned profile_idc 100. A probe without Profile-Extra lets the
    // shared PMS encode its default (High). Baseline sends cabac=0; High sends
    // cabac=1. Main in the URL is not cabac=1.
    if (weak.owenb || weak.highCabac || isFpgaPlexProfile(weak)) {
        h.push_back({"X-Plex-Client-Capabilities", plexClientCapabilities(weak)});
        h.push_back({"X-Plex-Client-Profile-Extra", plexClientProfileExtra(weak, false)});
    }
    // Range keeps probe cheap when the transcoder does stream.
    h.push_back({"Range", "bytes=0-4095"});
    return h;
}

std::string ownerTranscodeLadderSrc(const std::string& plexBase, const std::string& key,
                                    const std::string& token, const std::string& session,
                                    int64_t offsetMs, const WeakLadder& fit, bool wan,
                                    std::string* startUrl) {
    const std::string start =
        buildUniversalTranscodeUrl(plexBase, key, token, session, offsetMs > 0 ? offsetMs : 0, fit);
    if (startUrl)
        *startUrl = start;
    return start.empty() ? std::string()
                         : withPlexHeadersAsQuery(start, plexFfmpegHeaders(session, token, fit,
                                                                           /*forceLadderCaps=*/wan, start));
}

std::string joinPlexHostPort(const std::string& protocol, const std::string& address,
                             const std::string& port) {
    const std::string addr = urlDecode(address);
    std::string proto = protocol.empty() ? "http" : protocol;
    const std::string p = port.empty() ? "32400" : port;
    std::string host = addr;
    std::string path;
    const auto scheme = addr.find("://");
    if (scheme != std::string::npos) {
        proto = addr.substr(0, scheme);
        host = addr.substr(scheme + 3);
    }
    const auto cut = host.find_first_of("/?");
    if (cut != std::string::npos) {
        path = host.substr(cut);
        host = host.substr(0, cut);
    }
    // [IPv6] has colons inside the brackets. A port is only the ':' after ']'.
    if (!host.empty() && host[0] == '[') {
        const auto rb = host.find(']');
        if (rb != std::string::npos) {
            if (rb + 1 < host.size() && host[rb + 1] == ':')
                return proto + "://" + host + path;
            if (rb + 1 == host.size())
                return proto + "://" + host + ":" + p + path;
        }
    }
    // host:port, or a percent-decoded address that already carried one.
    if (host.find(':') != std::string::npos)
        return proto + "://" + host + path;
    return proto + "://" + host + ":" + p + path;
}

PlexEndpoint splitPlexHostPort(const std::string& address, const std::string& port) {
    PlexEndpoint ep;
    const std::string p = port.empty() ? "32400" : port;
    std::string hostport = urlDecode(address);
    const auto scheme = hostport.find("://");
    if (scheme != std::string::npos) {
        ep.protocol = hostport.substr(0, scheme);
        hostport = hostport.substr(scheme + 3);
    }
    const auto slash = hostport.find('/');
    if (slash != std::string::npos)
        hostport = hostport.substr(0, slash);
    if (!hostport.empty() && hostport[0] == '[') {
        const auto rb = hostport.find(']');
        if (rb != std::string::npos) {
            ep.host = hostport.substr(0, rb + 1);
            if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') {
                ep.port = hostport.substr(rb + 2);
                ep.portInAddress = true;
            }
            if (ep.port.empty())
                ep.port = p;
            return ep;
        }
    }
    const auto first = hostport.find(':');
    const auto last = hostport.rfind(':');
    if (first != std::string::npos && first == last) {
        ep.host = hostport.substr(0, first);
        ep.port = hostport.substr(first + 1);
        ep.portInAddress = true;
        if (ep.port.empty())
            ep.port = p;
        return ep;
    }
    ep.host = hostport;
    ep.port = p;
    return ep;
}

bool ipv6FromPmsHost(const std::string& hostOrUrl, std::string& out) {
    out.clear();
    std::string h = hostOrUrl;
    if (auto p = h.find("://"); p != std::string::npos)
        h = h.substr(p + 3);
    if (auto p = h.find('/'); p != std::string::npos)
        h = h.substr(0, p);
    if (auto p = h.find('@'); p != std::string::npos)
        h = h.substr(p + 1);
    std::string cand;
    if (!h.empty() && h[0] == '[') {
        const auto close = h.find(']');
        if (close == std::string::npos)
            return false;
        cand = h.substr(1, close - 1);
    } else if (std::count(h.begin(), h.end(), ':') >= 2) {
        cand = h; // bare IPv6 literal: more than one colon, so no host:port reading
    } else {
        if (auto p = h.find(':'); p != std::string::npos)
            h = h.substr(0, p);
        if (h.find("plex.direct") == std::string::npos)
            return false;
        cand = h.substr(0, h.find('.'));
        if (cand.find('-') == std::string::npos)
            return false;
        for (char& c : cand)
            if (c == '-')
                c = ':'; // plex.direct writes "::" as "--"
    }
    if (auto z = cand.find('%'); z != std::string::npos)
        cand = cand.substr(0, z); // zone id (fe80::1%wlan0)
    unsigned char b[16];
    if (cand.empty() || inet_pton(AF_INET6, cand.c_str(), b) != 1)
        return false;
    char txt[INET6_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET6, b, txt, sizeof(txt)))
        return false;
    out = txt;
    return true;
}

bool isUnreachableIpv6Host(const std::string& hostOrUrl) {
    std::string v6;
    if (!ipv6FromPmsHost(hostOrUrl, v6))
        return false;
    unsigned char b[16];
    if (inet_pton(AF_INET6, v6.c_str(), b) != 1)
        return false;
    if ((b[0] & 0xfe) == 0xfc)
        return true; // ULA fc00::/7
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
        return true; // link-local fe80::/10
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0)
        return true; // site-local fec0::/10 (deprecated, still never routed)
    return false;
}

bool probePmsIdentity(const std::string& base, const std::string& machineId, int timeoutSec) {
    const std::string b = normalizePlexBase(base);
    if (b.empty())
        return false;
    const std::string body = httpGet(b + "/identity", timeoutSec < 1 ? 1 : timeoutSec, {},
                                     defaultRequestProfileName(), false, false);
    if (body.find("MediaContainer") == std::string::npos)
        return false;
    if (machineId.empty())
        return true;
#ifdef MUT_336_SUBSTRING_ID
    return body.find(machineId) != std::string::npos;
#else
    // #336 review: parse <MediaContainer machineIdentifier="..."> and compare exactly.
    return attr(body, "MediaContainer", "machineIdentifier") == machineId;
#endif
}

namespace {
// #329: the play server's machine id for the burn PUT owner check. Tokenless /identity
// (no token on argv/URL/log); cached per base, misses retried.
std::string burnPutIdentityFromPms(const std::string& base) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    const std::string b = normalizePlexBase(base);
    if (b.empty())
        return {};
    {
        std::lock_guard<std::mutex> g(mu);
        const auto it = cache.find(b);
        if (it != cache.end())
            return it->second;
    }
    const std::string body =
        httpGet(b + "/identity", 3, {}, defaultRequestProfileName(), false, false);
    const std::string k = "machineIdentifier=\"";
    const size_t a = body.find(k);
    const size_t e = a == std::string::npos ? a : body.find('"', a + k.size());
    if (e == std::string::npos)
        return {};
    const std::string id = body.substr(a + k.size(), e - a - k.size());
    std::lock_guard<std::mutex> g(mu);
    cache[b] = id;
    return id;
}
const bool kBurnPutIdentityInstalled = [] {
    if (!burnPutIdentityLookup())
        burnPutIdentityLookup() = burnPutIdentityFromPms;
    return true;
}();
}  // namespace

namespace {
// End of the JSON value (object/array/string/scalar) starting at s[i]; npos if unbalanced.
size_t jsonValueEnd(const std::string& s, size_t i) {
    int depth = 0;
    bool str = false;
    for (size_t k = i; k < s.size(); ++k) {
        const char c = s[k];
        if (str) {
            if (c == '\\')
                ++k;
            else if (c == '"')
                str = false;
            continue;
        }
        if (c == '"')
            str = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            if (--depth == 0)
                return k + 1;
        }
    }
    return std::string::npos;
}
// Raw scalar after "key": inside an object text (string quotes stripped); "" if absent.
std::string jsonScalar(const std::string& obj, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = obj.find(k);
    if (p == std::string::npos)
        return {};
    p = obj.find(':', p + k.size());
    if (p == std::string::npos)
        return {};
    ++p;
    while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p])))
        ++p;
    size_t e = p;
    if (p < obj.size() && obj[p] == '"') {
        e = obj.find('"', p + 1);
        return e == std::string::npos ? std::string() : obj.substr(p + 1, e - p - 1);
    }
    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' &&
           !std::isspace(static_cast<unsigned char>(obj[e])))
        ++e;
    return obj.substr(p, e - p);
}
} // namespace

std::vector<std::string> localIpv4BasesFromResources(const std::string& json,
                                                     const std::string& machineIdentifier) {
    std::vector<std::string> out;
    if (machineIdentifier.empty())
        return out;
    for (size_t i = json.find('{'); i != std::string::npos && i < json.size();) {
        const size_t e = jsonValueEnd(json, i);
        if (e == std::string::npos)
            break;
        const std::string res = json.substr(i, e - i);
        i = json.find('{', e);
        if (jsonScalar(res, "clientIdentifier") != machineIdentifier)
            continue;
        const size_t c = res.find("\"connections\"");
        const size_t arr = c == std::string::npos ? c : res.find('[', c);
        if (arr == std::string::npos)
            return out;
        const size_t arrEnd = jsonValueEnd(res, arr);
        for (size_t q = res.find('{', arr); q != std::string::npos && q < arrEnd;) {
            const size_t qe = jsonValueEnd(res, q);
            if (qe == std::string::npos)
                break;
            const std::string conn = res.substr(q, qe - q);
            q = res.find('{', qe);
            const std::string addr = jsonScalar(conn, "address");
            struct in_addr a4;
            if (jsonScalar(conn, "local") != "true" || jsonScalar(conn, "relay") == "true" ||
                jsonScalar(conn, "IPv6") == "true" || inet_pton(AF_INET, addr.c_str(), &a4) != 1)
                continue;
            const std::string port = jsonScalar(conn, "port");
            const std::string base = "http://" + addr + ":" + (port.empty() ? "32400" : port);
            if (std::find(out.begin(), out.end(), base) == out.end())
                out.push_back(base);
        }
        return out;
    }
    return out;
}

std::string fetchPlexTvResourcesJson(const std::string& token, int timeoutSec) {
    if (token.empty())
        return {};
    return httpGet(
        "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1",
        timeoutSec < 1 ? 1 : timeoutSec,
        HeaderList{{"Accept", "application/json"},
                   {"X-Plex-Token", token},
                   {"X-Plex-Client-Identifier", "misterplex"}},
        defaultRequestProfileName(), true, false);
}

std::vector<std::string> plexTvLocalIpv4Bases(const std::string& machineIdentifier,
                                              const std::string& token, int timeoutSec) {
    if (machineIdentifier.empty() || token.empty())
        return {};
    const std::string body = httpGet(
        "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=0",
        timeoutSec < 1 ? 1 : timeoutSec,
        HeaderList{{"Accept", "application/json"},
                   {"X-Plex-Token", token},
                   {"X-Plex-Client-Identifier", "misterplex-cast-lookup"}},
        defaultRequestProfileName(), true, false);
    return localIpv4BasesFromResources(body, machineIdentifier);
}

std::string buildPlexBase(const std::string& protocol, const std::string& address,
                          const std::string& port, const std::string& lanFallback) {
    const std::string addr = urlDecode(address);
    if (addr.empty() || isUnreachableHost(addr)) {
        if (!lanFallback.empty() && !isUnreachableHost(lanFallback))
            return joinPlexHostPort("http", lanFallback, port);
        return {};
    }
    return joinPlexHostPort(protocol, address, port);
}

namespace {

// plex.direct hosts encode IPv4 as a-b-c-d.<token>.plex.direct
bool dottedIpv4FromDashedHost(const std::string& host, std::string& outIp) {
    // Take label before first '.'
    const auto dot = host.find('.');
    const std::string label = dot == std::string::npos ? host : host.substr(0, dot);
    int o[4] = {0, 0, 0, 0};
    int idx = 0;
    int cur = -1;
    for (size_t i = 0; i <= label.size(); ++i) {
        const char c = i < label.size() ? label[i] : '-';
        if (c >= '0' && c <= '9') {
            if (cur < 0)
                cur = 0;
            cur = cur * 10 + (c - '0');
            if (cur > 255)
                return false;
        } else if (c == '-') {
            if (cur < 0 || idx >= 4)
                return false;
            o[idx++] = cur;
            cur = -1;
        } else {
            return false;
        }
    }
    if (idx != 4)
        return false;
    outIp = std::to_string(o[0]) + "." + std::to_string(o[1]) + "." + std::to_string(o[2]) +
            "." + std::to_string(o[3]);
    return true;
}

bool splitUrlBase(const std::string& base, std::string& scheme, std::string& host,
                  std::string& port) {
    scheme.clear();
    host.clear();
    port.clear();
    std::string s = base;
    while (!s.empty() && (s.back() == '/' || s.back() == ' '))
        s.pop_back();
    auto schemeEnd = s.find("://");
    if (schemeEnd == std::string::npos)
        return false;
    scheme = s.substr(0, schemeEnd);
    size_t hostStart = schemeEnd + 3;
    size_t hostEnd = s.find('/', hostStart);
    std::string hostPort =
        hostEnd == std::string::npos ? s.substr(hostStart) : s.substr(hostStart, hostEnd - hostStart);
    if (hostPort.empty())
        return false;
    // strip userinfo
    auto at = hostPort.find('@');
    if (at != std::string::npos)
        hostPort = hostPort.substr(at + 1);
    // [ipv6]:port not required for plex.direct path
    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(':') == colon) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    } else {
        host = hostPort;
    }
    return !host.empty();
}

} // namespace

bool isLoopbackPmsHost(const std::string& hostOrUrl) {
    std::string h = hostOnly(hostOrUrl);
    for (char& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "0.0.0.0" ||
           h.rfind("127.", 0) == 0;
}

std::string rewriteLoopbackPmsBase(const std::string& selectedBase,
                                   const std::string& lanFallback) {
    if (!isLoopbackPmsHost(selectedBase))
        return selectedBase;
    const std::string fb = normalizePlexBase(lanFallback);
    if (fb.empty() || isLoopbackPmsHost(fb))
        return selectedBase;
    return fb;
}

bool castHostOnPlayerLan(const std::string& base, const std::string& playerLan,
                         const std::string& conf) {
    const std::string n = normalizePlexBase(base);
    if (n.empty())
        return false;
    const PlexEndpoint ep = splitPlexHostPort(n, "32400");
    return !ep.host.empty() && isPlayerLanGateway(ep.host, playerLan, normalizePlexBase(conf));
}

std::string preferReachablePmsBase(const std::vector<std::string>& candidates,
                                   const std::string& confLanFallback,
                                   const std::string& playerLan) {
    const std::string conf = normalizePlexBase(confLanFallback);
    const bool confLoop = !conf.empty() && hostLooksLoopback(conf);
    // Local Plex player rule: talk to the local PMS on 127.0.0.1, not the
    // ULA / link-local address Plex Web published from another NIC.
    if (confLoop)
        return conf;
    for (const std::string& raw : candidates) {
        if (raw.empty() || isUnreachableHost(raw) || hostLooksLoopback(raw))
            continue;
        const std::string open = companionOpenablePmsBase(raw, conf, playerLan);
        if (open.empty() || isUnreachableHost(open) || hostLooksLoopback(open))
            continue;
        return open;
    }
    if (!conf.empty())
        return conf;
    return {};
}

bool applyPmsBaseToEndpoint(const std::string& base, std::string& address, std::string& port,
                            std::string& protocol) {
    const std::string n = normalizePlexBase(base);
    if (n.empty())
        return false;
    const PlexEndpoint ep = splitPlexHostPort(n, "32400");
    if (ep.host.empty())
        return false;
    address = ep.host;
    port = ep.port.empty() ? "32400" : ep.port;
    protocol = !ep.protocol.empty() ? ep.protocol : "http";
    return true;
}

PlexEndpoint reachableTimelinePms(const std::string& address, const std::string& port,
                                  const std::string& protocol, const std::string& lanFallback,
                                  const std::string& playerLan) {
    PlexEndpoint raw = splitPlexHostPort(address, port);
    if (!protocol.empty())
        raw.protocol = protocol;
    if (raw.protocol.empty())
        raw.protocol = "http";
    if (raw.port.empty())
        raw.port = port.empty() ? "32400" : port;

    const bool rewrite = raw.host.empty() || isUnreachableHost(raw.host) ||
                         raw.host.find("plex.direct") != std::string::npos ||
                         isPlayerLanGateway(raw.host, playerLan, lanFallback);
    if (!rewrite)
        return raw;

    const std::string joined = joinPlexHostPort(raw.protocol, raw.host, raw.port);
    const std::string open = companionOpenablePmsBase(joined, lanFallback, playerLan);
    if (open.empty())
        return raw;
    PlexEndpoint out = splitPlexHostPort(open, raw.port);
    if (out.protocol.empty())
        out.protocol = "http";
    if (out.port.empty())
        out.port = raw.port;
    return out;
}

std::string companionOpenablePmsBase(const std::string& selectedBase,
                                     const std::string& confLanFallback,
                                     const std::string& playerLan) {
    const std::string conf = normalizePlexBase(confLanFallback);
    std::string selected = selectedBase;
    while (!selected.empty() && (selected.back() == '/' || selected.back() == ' '))
        selected.pop_back();
    if (selected.empty())
        return conf;

    // Bare host[:port] → assume http
    if (selected.find("://") == std::string::npos) {
        if (isUnreachableHost(selected))
            return conf;
        if (isPlayerLanGateway(selected, playerLan, conf))
            return conf;
        if (selected.find(':') != std::string::npos)
            return std::string("http://") + selected;
        return std::string("http://") + selected + ":32400";
    }

    std::string scheme, host, port;
    if (!splitUrlBase(selected, scheme, host, port))
        return conf;
    if (port.empty())
        port = "32400";

    // Plex Web on the PMS host publishes 127.0.0.1 / ULA. That is this box's
    // loopback only when conf says so (local player rule).
    if (hostLooksLoopback(host) && !conf.empty() && !hostLooksLoopback(conf))
        return conf;

    // Already plain HTTP and reachable from MiSTer — keep (remote shared PMS),
    // unless it is a *foreign* private host (home 203.0.113.70 while conf is  lab)
    // or the player's own VLAN gateway (203.0.113.1 next to  lab).
    if (scheme == "http" && !isUnreachableHost(host)) {
        if (isPlayerLanGateway(host, playerLan, conf))
            return conf;
        if (isPrivateRfc1918Host(host) && !conf.empty() && isPrivateRfc1918Host(conf) &&
            !samePrivateHost(host, conf)) {
            // Not openable from this MiSTer; caller should resolve public IP.
            return {};
        }
        return std::string("http://") + host + ":" + port;
    }

    // Docker-bridge / unreachable → conf LAN only (local PMS identity rewrite).
    if (isUnreachableHost(host) || isUnreachableHost(selected)) {
        if (!conf.empty())
            return conf;
        return {};
    }

    // https or plex.direct: public TLS stays on the plex.direct name (#368). Private
    // same-LAN still opens dotted IPv4 HTTP.
    std::string ip;
    if (host.find("plex.direct") != std::string::npos && dottedIpv4FromDashedHost(host, ip)) {
        if (isUnreachableHost(ip)) {
            if (!conf.empty())
                return conf;
            return {};
        }
        // Foreign private plex.direct (owner home LAN) is not reachable from lab MiSTer.
        // Do NOT collapse onto conf PLEX_BASE — that 404s foreign ratingKeys.
        if (isPlayerLanGateway(ip, playerLan, conf))
            return conf;
        if (isPrivateRfc1918Host(ip) && !conf.empty() && isPrivateRfc1918Host(conf) &&
            !samePrivateHost(ip, conf)) {
            return {};
        }
        // Public plex.direct: keep TLS so SNI matches the certificate (#368).
        // Private same-LAN still opens http://ip (companionOpenable LAN tests).
        if (scheme == "https" && !isPrivateRfc1918Host(ip))
            return std::string("https://") + host + ":" + port;
        return std::string("http://") + ip + ":" + port;
    }

    // Non-plex https host: keep TLS on a DNS name (#368). Raw IPv4 has no SNI.
    if (scheme == "https" && !host.empty()) {
        if (isPlayerLanGateway(host, playerLan, conf))
            return conf;
        if (isPrivateRfc1918Host(host) && !conf.empty() && isPrivateRfc1918Host(conf) &&
            !samePrivateHost(host, conf)) {
            return {};
        }
        in_addr a{};
        if (inet_pton(AF_INET, host.c_str(), &a) == 1)
            return std::string("http://") + host + ":" + port;
        return std::string("https://") + host + ":" + port;
    }

    // Last resort: conf LAN only when selected was already that LAN family.
    return conf;
}

std::string companionPublicPmsBaseFromResources(const std::string& machineIdentifier,
                                                const std::string& token) {
    if (machineIdentifier.empty() || token.empty())
        return {};
    // plex.tv JSON: find matching clientIdentifier, prefer non-local non-relay
    // connection address (public IPv4) as plain HTTP.
    const std::string url =
        "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1";
    // Verify with the daemon CA and do not follow redirects. The token header
    // must not be sent on to another host, and must not travel with -k.
    int curlExit = -1;
    bool noCa = false;
    const std::string body = httpGet(
        url, 15,
        HeaderList{{"Accept", "application/json"},
                   {"X-Plex-Token", token},
                   {"X-Plex-Client-Identifier", "misterplex-cast-lookup"}},
        defaultRequestProfileName(), true, false, &curlExit, &noCa);
    if (noCa) {
        std::fprintf(stderr, "misterplexd: plex.tv resources lookup: no CA bundle\n");
        return {};
    }
    // 35 is a handshake or connect failure. 51/58/60/77 are certificate checks.
    if (curlExit == 35) {
        std::fprintf(stderr,
                     "misterplexd: plex.tv resources lookup: TLS handshake/connect error (curl %d)\n",
                     curlExit);
        return {};
    }
    if (curlExit == 51 || curlExit == 58 || curlExit == 60 || curlExit == 77) {
        std::fprintf(stderr, "misterplexd: plex.tv resources lookup: TLS verify failed (curl %d)\n",
                     curlExit);
        return {};
    }
    // 6 resolve, 7 connect, 28 timeout. These are not a lookup miss.
    if (curlExit == 6 || curlExit == 7 || curlExit == 28) {
        std::fprintf(stderr, "misterplexd: plex.tv resources lookup: network error (curl %d)\n",
                     curlExit);
        return {};
    }
    if (body.empty() || body.find(machineIdentifier) == std::string::npos) {
        std::fprintf(stderr,
                     "misterplexd: plex.tv resources lookup miss machine=%s body=%zu\n",
                     machineIdentifier.c_str(), body.size());
        return {};
    }

    // Prefer whole-body scan near the machine id, then whole body. JSON may be
    // pretty-printed or minified; also accept XML resources (Device attributes).
    const size_t idPos = body.find(machineIdentifier);
    const size_t windowStart = (idPos > 4000) ? idPos - 4000 : 0;
    const size_t windowEnd = std::min(body.size(), idPos + 12000);
    const std::string block = body.substr(windowStart, windowEnd - windowStart);

    auto extractJson = [](const std::string& s, const char* key, size_t from = 0) -> std::string {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = s.find(k, from);
        if (p == std::string::npos)
            return {};
        p = s.find(':', p + k.size());
        if (p == std::string::npos)
            return {};
        ++p;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
            ++p;
        if (p >= s.size())
            return {};
        if (s[p] == '"') {
            ++p;
            size_t e = p;
            while (e < s.size() && s[e] != '"') {
                if (s[e] == '\\' && e + 1 < s.size())
                    e += 2;
                else
                    ++e;
            }
            if (e >= s.size())
                return {};
            return s.substr(p, e - p);
        }
        size_t e = p;
        while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']' &&
               !std::isspace(static_cast<unsigned char>(s[e])))
            ++e;
        return s.substr(p, e - p);
    };

    // 1) publicAddress near machine id
    std::string publicHint = extractJson(block, "publicAddress");
    if (publicHint.empty())
        publicHint = extractJson(body, "publicAddress", idPos > 200 ? idPos - 200 : 0);

    // 2) Walk non-local non-relay connections. Prefer plain HTTP; kube and other
    // reverse-proxied servers often publish on non-32400 ports (e.g. 32401).
    auto scanConnections = [&](const std::string& s, bool httpOnly) -> std::string {
        size_t pos = 0;
        while ((pos = s.find("\"address\"", pos)) != std::string::npos) {
            size_t c0 = s.rfind('{', pos);
            size_t c1 = s.find('}', pos);
            if (c0 == std::string::npos || c1 == std::string::npos || c1 <= c0) {
                pos += 9;
                continue;
            }
            // Keep connection objects near this machine id when scanning whole body.
            if (&s == &body) {
                if (c1 + 200 < idPos || c0 > idPos + 10000) {
                    pos = c1 + 1;
                    continue;
                }
            }
            const std::string conn = s.substr(c0, c1 - c0 + 1);
            pos = c1 + 1;
            const std::string local = extractJson(conn, "local");
            const std::string relay = extractJson(conn, "relay");
            if (local == "true" || relay == "true")
                continue;
            std::string proto = extractJson(conn, "protocol");
            for (char& c : proto) {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            if (httpOnly && !proto.empty() && proto != "http")
                continue;
            std::string addr = extractJson(conn, "address");
            std::string port = extractJson(conn, "port");
            if (addr.empty())
                continue;
            if (isPrivateRfc1918Host(addr) || isUnreachableHost(addr))
                continue;
            if (port.empty())
                port = "32400";
            return std::string("http://") + addr + ":" + port;
        }
        return {};
    };

    std::string best = scanConnections(block, true);
    if (best.empty())
        best = scanConnections(body, true);
    if (best.empty())
        best = scanConnections(block, false);
    if (best.empty())
        best = scanConnections(body, false);

    // Also harvest a non-default public port for publicAddress fallback (kube :32401).
    auto firstPublicPort = [&](const std::string& s) -> std::string {
        size_t pos = 0;
        while ((pos = s.find("\"port\"", pos)) != std::string::npos) {
            size_t c0 = s.rfind('{', pos);
            size_t c1 = s.find('}', pos);
            if (c0 == std::string::npos || c1 == std::string::npos || c1 <= c0) {
                pos += 6;
                continue;
            }
            const std::string conn = s.substr(c0, c1 - c0 + 1);
            pos = c1 + 1;
            if (extractJson(conn, "local") == "true" || extractJson(conn, "relay") == "true")
                continue;
            std::string port = extractJson(conn, "port");
            if (!port.empty() && port != "32400")
                return port;
        }
        return {};
    };

    // 3) publicAddress fallback — never hardcode 32400 when connections show another port.
    if (best.empty() && !publicHint.empty() && !isPrivateRfc1918Host(publicHint) &&
        !isUnreachableHost(publicHint)) {
        std::string port = firstPublicPort(block);
        if (port.empty())
            port = firstPublicPort(body);
        if (port.empty())
            port = "32400";
        best = std::string("http://") + publicHint + ":" + port;
    }

    // 4) XML Device form (plex.tv default Accept): clientIdentifier=… publicAddress=…
    //    Nested <connection address=… local=0 relay=0 …/> preferred.
    if (best.empty()) {
        const std::string needle = "clientIdentifier=\"" + machineIdentifier + "\"";
        size_t dpos = body.find(needle);
        if (dpos == std::string::npos)
            dpos = body.find("machineIdentifier=\"" + machineIdentifier + "\"");
        if (dpos != std::string::npos) {
            // Device open tag may end at '>' after attributes; connections follow.
            size_t tag0 = body.rfind('<', dpos);
            size_t regionEnd = body.find("</Device>", dpos);
            if (regionEnd == std::string::npos)
                regionEnd = body.find("<Device ", dpos + 10);
            if (regionEnd == std::string::npos)
                regionEnd = std::min(body.size(), dpos + 4000);
            const std::string region = body.substr(tag0 == std::string::npos ? dpos : tag0,
                                                   regionEnd - (tag0 == std::string::npos ? dpos : tag0));
            auto xmlAttrIn = [](const std::string& s, const char* name, size_t from = 0) -> std::string {
                const std::string k = std::string(name) + "=\"";
                size_t p = s.find(k, from);
                if (p == std::string::npos)
                    return {};
                p += k.size();
                size_t e = s.find('"', p);
                return e == std::string::npos ? std::string{} : s.substr(p, e - p);
            };
            // Prefer non-local non-relay HTTP connection addresses (kube uses :32401).
            auto tryXmlConns = [&](bool httpOnly) -> std::string {
                size_t cpos = 0;
                while ((cpos = region.find("<connection ", cpos)) != std::string::npos) {
                    size_t cend = region.find("/>", cpos);
                    if (cend == std::string::npos)
                        break;
                    const std::string conn = region.substr(cpos, cend - cpos);
                    cpos = cend + 2;
                    const std::string local = xmlAttrIn(conn, "local");
                    const std::string relay = xmlAttrIn(conn, "relay");
                    if (local == "1" || local == "true" || relay == "1" || relay == "true")
                        continue;
                    std::string proto = xmlAttrIn(conn, "protocol");
                    for (char& c : proto) {
                        if (c >= 'A' && c <= 'Z')
                            c = static_cast<char>(c - 'A' + 'a');
                    }
                    if (httpOnly && !proto.empty() && proto != "http")
                        continue;
                    std::string addr = xmlAttrIn(conn, "address");
                    std::string port = xmlAttrIn(conn, "port");
                    if (addr.empty() || isPrivateRfc1918Host(addr) || isUnreachableHost(addr))
                        continue;
                    if (port.empty())
                        port = "32400";
                    return std::string("http://") + addr + ":" + port;
                }
                return {};
            };
            best = tryXmlConns(true);
            if (best.empty())
                best = tryXmlConns(false);
            if (best.empty()) {
                std::string pa = xmlAttrIn(region, "publicAddress");
                std::string portHint;
                size_t cpos = 0;
                while ((cpos = region.find("<connection ", cpos)) != std::string::npos) {
                    size_t cend = region.find("/>", cpos);
                    if (cend == std::string::npos)
                        break;
                    const std::string conn = region.substr(cpos, cend - cpos);
                    cpos = cend + 2;
                    const std::string local = xmlAttrIn(conn, "local");
                    const std::string relay = xmlAttrIn(conn, "relay");
                    if (local == "1" || local == "true" || relay == "1" || relay == "true")
                        continue;
                    std::string port = xmlAttrIn(conn, "port");
                    if (!port.empty() && port != "32400") {
                        portHint = port;
                        break;
                    }
                }
                if (portHint.empty())
                    portHint = "32400";
                if (!pa.empty() && !isPrivateRfc1918Host(pa) && !isUnreachableHost(pa))
                    best = std::string("http://") + pa + ":" + portHint;
            }
        }
    }

    if (best.empty()) {
        std::fprintf(stderr,
                     "misterplexd: plex.tv resources parsed no public HTTP base "
                     "machine=%s body=%zu json=%d\n",
                     machineIdentifier.c_str(), body.size(),
                     body.find("\"clientIdentifier\"") != std::string::npos ? 1 : 0);
    } else {
        std::fprintf(stderr, "misterplexd: plex.tv public base %s for machine=%s\n",
                     misterplex::logUrl(best).c_str(), machineIdentifier.c_str());
    }
    return best;
}

bool ensureUniversalDecision(const std::string& startUrl, const std::string& sessionId,
                             const std::string& token) {
    return ensureUniversalDecision(startUrl, sessionId, token, WeakLadder{});
}

std::string buildUniversalDecisionUrl(const std::string& startUrl,
                                      const std::string& sessionId,
                                      const std::string& token, const WeakLadder& weak) {
    // #340: the decision URL never carries the token (it goes as a header via decisionHeaders). The
    // parameter stays so tests can pass a real token and assert it is absent (E6t, test_remote_buffer_params).
    (void)token;
    if (startUrl.find("/video/:/transcode/universal/") == std::string::npos)
        return {};
    // Derive base + path from start URL
    auto pathPos = startUrl.find("/video/:/transcode/universal/start");
    if (pathPos == std::string::npos)
        return {};
    const std::string base = startUrl.substr(0, pathPos);
    // Extract path= from query
    auto pathEq = startUrl.find("path=");
    if (pathEq == std::string::npos)
        return {};
    pathEq += 5;
    auto pathEnd = startUrl.find('&', pathEq);
    std::string path = startUrl.substr(pathEq, pathEnd == std::string::npos ? std::string::npos
                                                                             : pathEnd - pathEq);

    // Mirror ladder params from start URL when present
    auto qparam = [&](const char* name) -> std::string {
        std::string key = std::string(name) + "=";
        // Anchor on '?' or '&' so a key ending in name= (e.g. xsubtitles=) does not alias.
        size_t p = 0;
        for (;;) {
            p = startUrl.find(key, p);
            if (p == std::string::npos)
                return {};
            if (p > 0 && (startUrl[p - 1] == '&' || startUrl[p - 1] == '?'))
                break;
            ++p;
        }
        p += key.size();
        auto e = startUrl.find('&', p);
        return startUrl.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    std::string vq = qparam("videoQuality");
    if (vq.empty())
        vq = "40";
    std::string vres = qparam("videoResolution");
    if (vres.empty())
        vres = "320x240";
    std::string br = qparam("maxVideoBitrate");
    if (br.empty())
        br = std::to_string(weak.maxVideoBitrateKbps);
    std::string vcodec = qparam("videoCodec");
    if (vcodec.empty())
        vcodec = weak.videoCodec;
    std::string acodec = qparam("audioCodec");
    if (acodec.empty())
        acodec = weak.audioCodec;
    std::string vprofile = qparam("videoProfile");
    if (vprofile.empty())
        vprofile = weak.h264Profile;
    std::string vlevel = qparam("videoLevel");
    if (vlevel.empty())
        vlevel = std::to_string(weak.h264Level);

    std::string location = qparam("location");
    if (location.empty())
        location = "lan";
    const bool wan = location == "wan";
    std::ostringstream decisionUrl;
    decisionUrl << base << "/video/:/transcode/universal/decision?hasMDE=1&path=" << path
                << "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=" << (wan ? "1" : "0")
                << "&directPlay=0&directStream=0"
                << "&location=" << location << "&session=" << urlEncodeQuery(sessionId)
                << "&videoQuality=" << vq << "&videoResolution=" << vres
                << "&maxVideoBitrate=" << br;
    if (wan) {
        decisionUrl << remoteMediaBufferParams();
        // reviewer decides the session on this request. The start URL's videoProfile
        // arrives too late (096: start said main, the AU was still Baseline CAVLC).
        // container and videoFrameRate stay off: a shared PMS answers those with HTTP 400.
        // Baseline cores need the same early pin. A WAN decision that omits
        // videoProfile lets the shared PMS encode the source (profile_idc 100);
        // the start URL's videoProfile arrives after that session exists.
        const bool fpgaBaseline = isFpgaPlexProfile(weak) && !weak.highCabac;
        // #207 REMOTE_H264_TRANSCODE: always pin H.264 on a remote decision.
        if (misterplex::remoteDecisionPinsCodec(misterplex::remoteH264TranscodeEnabled(), wan,
                                                weak.highCabac || fpgaBaseline))
            decisionUrl << "&videoCodec=" << vcodec << "&audioCodec=" << acodec
                        << "&videoProfile=" << (fpgaBaseline ? "baseline" : vprofile)
                        << "&videoLevel=" << vlevel;
    } else {
        // Home PMS is LAN. The start URL's videoProfile=baseline,main lets that
        // server encode the source (profile_idc 100) before the play URL is read.
        // The decision creates the session, so a Baseline core pins baseline here.
        const bool fpgaBaselineLan = isFpgaPlexProfile(weak) && !weak.highCabac;
        if (fpgaBaselineLan)
            vprofile = "baseline";
        decisionUrl << "&videoCodec=" << vcodec << "&audioCodec=" << acodec
                    << "&videoProfile=" << vprofile << "&videoLevel=" << vlevel;
        if (isFpgaPlexProfile(weak)) {
            if (!validateWeakLadder(weak))
                return {};
            decisionUrl << "&container=mpegts&audioChannels=2&videoFrameRate="
                        << urlEncodeQuery(weak.fpgaFpsDen == 1
                                              ? std::to_string(weak.fpgaFpsNum)
                                              : std::to_string(weak.fpgaFpsNum) + "/" +
                                                    std::to_string(weak.fpgaFpsDen));
        }
    }
    // #340: the decision GET sends the token as a header (ensureUniversalDecision's
    // decisionHeaders), never in this URL.
#ifdef MUT_340_DECISION_QUERY   // test-only mutant: the old query token
    if (!token.empty() && (!isFpgaPlexProfile(weak) || wan))
        decisionUrl << "&X-Plex-Token=" << urlEncodeQuery(token);
#endif
    // The decision creates the session. Start-URL subtitle params arrive too
    // late (same as videoProfile). Copy the burn only when the fix is on, so
    // an unset flag still sends today's decision.
#ifndef MUT_BURN_SUBS_FIX_DECISION_OMIT
    if (burnSubsFixEnabled() && weak.burnSubsFixImage) {
        const std::string burn = qparam("subtitles");
        if (burn == "burn") {
            decisionUrl << "&subtitles=burn";
            const std::string sid = qparam("subtitleStreamID");
            if (!sid.empty())
                decisionUrl << "&subtitleStreamID=" << sid;
        }
    }
#endif
    return decisionUrl.str();
}

bool ensureUniversalDecision(const std::string& startUrl, const std::string& sessionId,
                             const std::string& token, const WeakLadder& weak,
                             bool forceLadderCaps) {
    const std::string decisionUrl = buildUniversalDecisionUrl(startUrl, sessionId, token, weak);
    if (decisionUrl.empty())
        return !isFpgaPlexProfile(weak);
    const bool wan [[maybe_unused]] = startUrl.find("location=wan") != std::string::npos ||
                     decisionUrl.find("location=wan") != std::string::npos;
    std::vector<std::pair<std::string, std::string>> decisionHeaders = {
        {"X-Plex-Session-Identifier", sessionId},
        {"X-Plex-Product", "Plex Web"},
        {"X-Plex-Client-Identifier", "misterplex"},
        {"X-Plex-Platform", "Chrome"},
        {"X-Plex-Device", "Linux"},
    };
    if (!token.empty())
        decisionHeaders.push_back({"X-Plex-Token", token});
    // WAN/shared has no MiSTerPlex.xml — Profile-Extra must carry the FPGA ladder.
    // LAN FPGA keeps XML-only (Extra can fight Baseline/CAVLC on lab PMS).
    // forceLadderCaps: the caller's src already carries the caps (plexFfmpegHeaders with
    // forceLadderCaps), so the decision for the same session must say the same thing (T-E2E-416).
    const std::string decisionProfileName = plexRequestProfileName(weak, startUrl);
    if (plexClientSendsProfileExtra(weak, forceLadderCaps, decisionProfileName)) {
        decisionHeaders.push_back({"X-Plex-Client-Capabilities", plexClientCapabilities(weak)});
        decisionHeaders.push_back({"X-Plex-Client-Profile-Extra",
                                   plexClientProfileExtra(weak, !decisionProfileName.empty())});
    }
    // PMS uses the first profile header when duplicates are sent and caches that
    // decision for the stream; the selected XML must replace the default here.
    if (resolveAborted()) {
        std::fprintf(stderr, "misterplexd: universal decision skipped: superseded (a later seek/play owns it)\n");
        return false;
    }
    if (weak.highCabac && wan) {
        const bool askedMain = decisionUrl.find("videoProfile=main") != std::string::npos;
        const bool noDirect = decisionUrl.find("directPlay=0") != std::string::npos;
        bool sawCabac = false;
        for (const auto& hv : decisionHeaders) {
            if (hv.second.find("cabac=1") != std::string::npos ||
                hv.second.find("cabac%3D1") != std::string::npos ||
                hv.second.find("cabac%3d1") != std::string::npos)
                sawCabac = true;
        }
        std::fprintf(stderr,
                     "misterplexd: High WAN decision videoProfile=%s directPlay=%s extra=%s\n",
                     askedMain ? "main" : "missing", noDirect ? "0" : "other",
                     sawCabac ? "cabac=1" : "no-cabac=1");
    }
    const std::string body = httpGet(decisionUrl, 20, decisionHeaders,
                                    decisionProfileName);
    if (body.empty())
        return false;
    if (body.find("unable to find a matching profile") != std::string::npos)
        return false;
    if (body.find("Bad Request") != std::string::npos &&
        body.find("MediaContainer") == std::string::npos)
        return false;
    const bool accepted = body.find("MediaContainer") != std::string::npos ||
                          body.find("transcodeDecisionCode") != std::string::npos;
    if (accepted) {
        const DecisionVideo dv = parseDecisionVideo(body);
        tlsLastDecisionVideo = dv;
        const bool checkCore = isFpgaPlexProfile(weak) || weak.h264Profile == "high" ||
                               modeResolveRow(weak.profileName) != nullptr;
        const std::string mm = checkCore ? decisionCoreMismatch(dv, weak) : std::string();
        std::fprintf(stderr,
                     "misterplexd: decision video codec=%s profile=%s level=%d refs=%d %dx%d bitDepth=%d hw=%s "
                     "part=%s check=%s%s%s profileName=%s\n",
                     dv.codec.empty() ? "-" : dv.codec.c_str(), dv.profile.empty() ? "-" : dv.profile.c_str(),
                     dv.level, dv.refFrames, dv.width, dv.height, dv.bitDepth,
                     dv.hwEncoding.empty() ? "-" : dv.hwEncoding.c_str(),
                     dv.partDecision.empty() ? "-" : dv.partDecision.c_str(),
                     mm.empty() ? "ok" : "mismatch: ",
                     mm.c_str(), weak.pmsLowered ? " (lowered retry)" : "",
                     decisionProfileName.empty() ? "none" : decisionProfileName.c_str());
        if (!mm.empty() && !weak.pmsLowered) {
            // One lowered retry (the caller: takeDecisionMismatch), for a codec or output-size mismatch. The lowered
            // request proceeds even if its decision still mismatches: the SPS gate decides from the first AU.
            tlsDecisionMismatch = mm;
            return false;
        }
    }
    return accepted;
}

bool mediaVideoIsH264(const std::string& plexMetadataXml) {
    if (plexMetadataXml.empty())
        return false;
    // Prefer Stream video codec when present; else Media@videoCodec.
    size_t pos = 0;
    while ((pos = plexMetadataXml.find("<Stream", pos)) != std::string::npos) {
        auto end = plexMetadataXml.find('>', pos);
        if (end == std::string::npos)
            break;
        const std::string tag = plexMetadataXml.substr(pos, end - pos);
        // PMS uses streamType="1" (video) and/or type="video"
        const bool isVideo = tag.find("streamType=\"1\"") != std::string::npos ||
                             tag.find("type=\"video\"") != std::string::npos;
        if (isVideo) {
            auto c = attrIn(tag, "codec");
            if (c.empty())
                c = attrIn(tag, "codecID");
            if (videoCodecIsH264(c))
                return true;
        }
        pos = end + 1;
    }
    return videoCodecIsH264(attr(plexMetadataXml, "Media", "videoCodec"));
}

SourceAspect sourceAspectFromPlexMetadata(const std::string& xml,
                                         int codedWidth, int codedHeight) {
    std::string displayAspect = attr(xml, "Video", "displayAspectRatio");
    if (displayAspect.empty())
        displayAspect = attr(xml, "Video", "aspectRatio");
    if (displayAspect.empty())
        displayAspect = attr(xml, "Media", "displayAspectRatio");
    if (displayAspect.empty())
        displayAspect = attr(xml, "Media", "aspectRatio");

    std::string sampleAspect = attr(xml, "Media", "sampleAspectRatio");
    if (sampleAspect.empty())
        sampleAspect = attr(xml, "Media", "pixelAspectRatio");
    std::string anamorphic = attr(xml, "Media", "anamorphic");

    size_t sp = 0;
    while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
        const auto end = xml.find('>', sp);
        if (end == std::string::npos)
            break;
        const std::string slice = xml.substr(sp, end - sp);
        const bool isVideo = slice.find("streamType=\"1\"") != std::string::npos ||
                             slice.find("type=\"video\"") != std::string::npos;
        if (isVideo) {
            if (displayAspect.empty())
                displayAspect = attrIn(slice, "displayAspectRatio");
            if (displayAspect.empty())
                displayAspect = attrIn(slice, "aspectRatio");
            if (sampleAspect.empty())
                sampleAspect = attrIn(slice, "sampleAspectRatio");
            if (sampleAspect.empty())
                sampleAspect = attrIn(slice, "pixelAspectRatio");
            if (sampleAspect.empty())
                sampleAspect = attrIn(slice, "sar");
            if (anamorphic.empty())
                anamorphic = attrIn(slice, "anamorphic");
            break;
        }
        sp = end + 1;
    }

    const std::string normalizedAnamorphic = lowerCopy(anamorphic);
    const bool squarePixelsKnown =
        normalizedAnamorphic == "0" || normalizedAnamorphic == "false" ||
        normalizedAnamorphic == "no";
    return sourceAspectFromMetadata(displayAspect, sampleAspect, codedWidth,
                                    codedHeight, squarePixelsKnown);
}

WeakLadder fitWeakLadderToAspect(const WeakLadder& weak,
                                 const SourceAspect& aspect) {
    WeakLadder fitted = weak;
    // FPGA requests preserve the profile's maximum rectangle; SPS supplies actual geometry.
    if (isFpgaPlexProfile(weak))
        return fitted;
    if (!aspect.valid || aspect.x == 0 || aspect.y == 0)
        return fitted;

    int maxW = 0;
    int maxH = 0;
    if (std::sscanf(weak.videoResolution.c_str(), "%dx%d", &maxW, &maxH) != 2 ||
        maxW < 2 || maxH < 2) {
        return fitted;
    }

    // L4 1280×720 HDMI is 16:9. Fitting 4:3 Trek to 960×720 then nearest-up
    // to the bank costs ~2 Hz (21.7 vs unique24). Keep the 1280×720 request.
    if (maxW >= 1280 && maxH >= 720)
        return fitted;

    int width = maxW;
    int height = maxH;
    if (static_cast<int64_t>(aspect.x) * maxH >=
        static_cast<int64_t>(aspect.y) * maxW) {
        height = static_cast<int>(
            (static_cast<int64_t>(maxW) * aspect.y) / aspect.x);
    } else {
        width = static_cast<int>(
            (static_cast<int64_t>(maxH) * aspect.x) / aspect.y);
    }
    width = std::max(2, width & ~1);
    height = std::max(2, height & ~1);

    // The dual-A9 path is proven realtime near 640x384, while 640x480 24p
    // transcodes fall below rate once AAC, scaling, and DDR presentation run
    // together. Keep the 480p ladder inside that pixel budget; MiSTer still
    // receives the full 640x480 presentation bank.
    constexpr int64_t kTrue480DecodePixelBudget = 640LL * 384LL;
    if (maxW <= 640 && maxH <= 480 &&
        static_cast<int64_t>(width) * height > kTrue480DecodePixelBudget) {
        const double dar = static_cast<double>(aspect.x) / aspect.y;
        width = static_cast<int>(
                    std::sqrt(static_cast<double>(kTrue480DecodePixelBudget) * dar)) &
                ~1;
        height = static_cast<int>(width / dar) & ~1;
        width = std::max(2, width);
        height = std::max(2, height);
    }
    fitted.videoResolution = std::to_string(width) + "x" + std::to_string(height);
    return fitted;
}

void applyTrackPickToLadder(WeakLadder& playWeak, const ResolveResult& r, bool havePref) {
    if (!r.trackPick.valid)
        return;
    // Soft text stays Direct Play when the overlay will paint it (softOverlayAllowed
    // is always on for SRT/ASS/SSA). Image / embedded that the user explicitly
    // chose still burn. Default off / skipped PGS/VobSub never silently burns (G10),
    // even when SUBTITLES=burn is in conf. Audio is named. A remembered subtitle is
    // named on the PLAY URL the way setStreams names an explicit pick.
    // BURN_SUBS_FIX on: a pick the overlay will not paint (PGS/VobSub/image)
    // keeps subtitles=burn, and the URL builder forces a video transcode.
    // mov_text overlays even with SOFT_SUBS off (#260) and is never burned.
    // The resolve log prints the pick id either way; this is what the URL reads.
    if (r.trackPick.rememberedSubtitle) {
#ifdef MUT_R3_REMEMBER_CODEC_ONLY
        const RememberWire w = rememberSubtitleWire(r.trackPick);
#else
        const RememberWire w = rememberSubtitleWire(r.trackPick, &r.tracks, r.partId);
#endif
        playWeak.burnSubtitles = w.burn;
        playWeak.subtitleStreamId = w.subtitleStreamId;
        playWeak.burnSubsFixImage = w.burn && burnSubsFixBurnsImage(r.trackPick);
        (void)havePref;
    } else {
        const bool subOn = r.trackPick.subtitleStreamId > 0 && !r.trackPick.subtitlesOff;
        if (subOn) {
#if defined(MUT_BURN_SUBS_FIX_DROP_TEXT)
            // Test-only: flag on still drops burn for overlay-unpaintable picks
            // (PGS / mov_text). Soft text correctly stays off the burn URL.
            if (subtitleCodecKeepsDirectPlay(r.trackPick.subtitleCodec)) {
                playWeak.burnSubtitles = false;
            } else if (burnSubsFixEnabled()) {
                playWeak.burnSubtitles = false;
            } else {
                playWeak.burnSubtitles = true;
                playWeak.subtitleStreamId = r.trackPick.subtitleStreamId;
            }
#else
            // #201-r3 (final spec): SRT/ASS go to the overlay and never burn. A pick the
            // band will not paint (playerSubtitleKind) burns as on the tip; BURN_SUBS_FIX
            // forces the transcode for image subs only. Never both painted and burned.
            if (subtitlePickMayBurn(r.tracks, r.trackPick, r.partId)) {
                playWeak.burnSubtitles = true;
                playWeak.subtitleStreamId = r.trackPick.subtitleStreamId;
                playWeak.burnSubsFixImage = burnSubsFixBurnsImage(r.trackPick);
            } else {
                playWeak.burnSubtitles = false;
            }
#endif
        } else {
            playWeak.subtitleStreamId = 0;
            playWeak.burnSubtitles = false;
            (void)havePref;
        }
    }
    if (r.trackPick.audioStreamId > 0)
        playWeak.audioStreamId = r.trackPick.audioStreamId;
}

bool directH264Allowed(bool wantDirect, bool isH264, bool burnSubtitles) {
    return wantDirect && isH264 && !burnSubtitles;
}

ResolveResult resolvePlayTarget(const std::string& rawKeyOrPath, const std::string& plexBase,
                                const std::string& token, int64_t offsetMs, bool weakAlways,
                                const WeakLadder& weakIn, bool preferDirectH264, int decodeW,
                                int decodeH, bool forceLadder, const std::string& gateDetail,
                                bool includeMarkers) {
    ResolveResult r;
    // The configured backend stays FpgaH264 while a verified PlexOB core is loaded.
    // That path rewrites the ladder to 624x480 baseline and directPlay=0. The ring
    // session uses the Owenb ladder instead.
    const VideoBackend session = sessionVideoBackend(
        configuredVideoBackend(), CoreGate::process().ownsIdleInFabric());
    const bool fpgaBackend = session == VideoBackend::FpgaH264;
    WeakLadder playWeak =   // BUG1 pre-play: lowered below once the metadata shows a CAVLC source above the hard cap
        ladderForPlaySession(session, weakIn, !plexTargetIsPrivateLan(plexBase));
    // #207 r3: a forced remote PlexOB play asks for the FPGA 640x480 profile, not 1280x720/6000k.
    const bool remoteBaseEarly = !plexTargetIsPrivateLan(plexBase);
    if (playWeak.owenb && misterplex::remoteUsesFpga640(misterplex::remoteH264TranscodeEnabled(),
                                                        remoteBaseEarly)) {
        playWeak.videoResolution = "640x480";
        playWeak.maxVideoBitrateKbps = misterplex::remoteFpgaKbps(playWeak.maxVideoBitrateKbps);
        std::fprintf(stderr, "misterplexd: remote ladder FPGA 640x480 %dk (REMOTE_H264_TRANSCODE=1)\n",
                     playWeak.maxVideoBitrateKbps);
    } else if (playWeak.owenb && remoteBaseEarly) {
        const int beforeCap = playWeak.maxVideoBitrateKbps;
        misterplex::applyRemoteOwenbCap(playWeak.videoResolution, playWeak.maxVideoBitrateKbps, true,
                                        true);
        std::fprintf(stderr, "misterplexd: remote OwenB cap 640x480 %dk (was %dk)\n",
                     playWeak.maxVideoBitrateKbps, beforeCap);
    }
    const WeakLadder& weak = playWeak;
    bool audioNeedsTranscode = false;   // explicit non-default audio: never hand back a direct Part
    if (session == VideoBackend::OwenbRing && !weakIn.owenb) {
        std::fprintf(stderr,
                     "misterplexd: PlexOB session ladder %s %dk (not FPGA %s)\n",
                     weak.videoResolution.c_str(), weak.maxVideoBitrateKbps,
                     weakIn.videoResolution.c_str());
    }
    // forceLadder with no helper: PMS universal of the same item (never a direct Part,
    // never a hard refuse).
    // FPGA raster mode chosen by the weak-universal block below; "" off the FPGA path.
    std::string fpgaMode;
    // No ladder helper (#275): a title with no numeric rate uses PMS universal at
    // 24000/1001. fps=src is not probed.
    std::string key = urlDecode(rawKeyOrPath);
    if (key.empty()) {
        r.ok = false;
        r.detail = "empty play key";
        return r;
    }

    if (playableIsFarpointIdentityCache(key)) {
        r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
        return r;
    }
    // playMedia 40868 / ratingKey 40868 must never take the local-path
    // identify branch (that is the farpoint_1280x720.mp4 cache soak).
    if (key == "40868")
        key = "/library/metadata/40868";

    // Direct http(s) or absolute non-library path
    if (key.rfind("http://", 0) == 0 || key.rfind("https://", 0) == 0) {
        r.ok = true;
        r.playable = key;
        r.detail = "direct URL";
        return r;
    }
    if (!metadataKeyIsFarpoint40868(key) && !key.empty() && key[0] == '/' &&
        key.rfind("/library", 0) != 0 &&
        key.rfind("/playQueues", 0) != 0) {
        r.ok = true;
        r.playable = key;
        r.detail = "local path";
        if (playableIsFarpointIdentityCache(key)) {
            r.ok = false;
            r.playable.clear();
            r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
            return r;
        }
        if (fpgaBackend) {
            r.detail = "local path; metadata deferred to the sole compressed demux";
            return r;
        }
        // Local files must publish DAR + 24000/1001 or PLAY is rejected / paced
        // at 24/1. Library rk 40868 must use PMS universal, not this path.
        const char* ff = "/media/fat/misterplex/bin/ffmpeg";
        if (::access(ff, X_OK) != 0)
            ff = "ffmpeg";
        // argv + posix_spawnp, stdout and stderr on one pipe (the old "2>&1"), stdin /dev/null; no shell.
        // An ffmpeg that cannot be started leaves ident empty, as a failed popen did.
        const std::vector<std::string> ffArgv = {ff, "-hide_banner", "-nostdin", "-i", key, "-f", "null", "-t", "0", "-"};
        const std::string ident = runArgv(ffArgv, true).out;
        int w = 0, h = 0, n = 0, d = 0, ax = 0, ay = 0;
        if (parseFfmpegIdentify(ident.c_str(), w, h, n, d, ax, ay)) {
            r.mediaWidth = w;
            r.mediaHeight = h;
            r.fpsNum = n;
            r.fpsDen = d;
            r.sourceFpsHint = (n > 0 && d > 0) ? (n + d / 2) / d : 0;
            if (ax > 0 && ay > 0)
                r.sourceAspect = {static_cast<uint16_t>(ax), static_cast<uint16_t>(ay), true};
            else if (w == 1280 && h == 720)
                r.sourceAspect = {16, 9, true};
            r.detail = localFileIsTrue720p24(w, h) ? "local path 1280x720" : "local path";
        } else if (r.sourceAspect.valid == false) {
            r.sourceAspect = defaultSourceAspectForBank(1280, 720);
        }
        return r;
    }

    // Normalize metadata key
    if (key.rfind("/library", 0) != 0 && key.find("library") != std::string::npos) {
        // sometimes "library/metadata/N"
        if (key[0] != '/')
            key = "/" + key;
    }

    if (playableIsFarpointIdentityCache(key)) {
        r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
        return r;
    }

    if (plexBase.empty()) {
        r.detail = "no PMS base for key=" + key;
        return r;
    }
    if (token.empty()) {
        r.detail = "no token for key=" + key;
        // Still try without token (local unauth servers)
    }

    // Fetch metadata for title/duration/viewOffset. Markers ride this GET (no second round trip).
    std::string metaUrl = plexBase + key;
    if (includeMarkers) {
        metaUrl += (metaUrl.find('?') == std::string::npos) ? "?" : "&";
        metaUrl += "includeMarkers=1";
    }
    // CHAPTERS unset/off: the metadata GET stays as it is (no includeChapters).
    if (chaptersEnabled()) {
        metaUrl += (metaUrl.find('?') == std::string::npos) ? "?" : "&";
        metaUrl += "includeChapters=1";
    }
    // #340: every backend sends the metadata token as a header.
    HeaderList metaHeaders;
    if (!token.empty())
        metaHeaders = HeaderList{{"X-Plex-Token", token}};
#ifdef MUT_340_META_QUERY   // test-only mutant: the old non-FPGA query token
    if (!fpgaBackend && !token.empty())
        metaUrl += std::string(metaUrl.find('?') == std::string::npos ? "?" : "&") + "X-Plex-Token=" +
                   urlEncodeQuery(token);
#endif
    std::string xml = httpGet(metaUrl, 12, metaHeaders);
    // A 404 or 401 from PMS still returns a BODY (an HTML error page), so
    // "response is not empty" does not mean "item exists". Without this check a
    // ratingKey that was deleted or renumbered by a library re-scan sails
    // through every branch below, finds no Part, and ends in a refused resolve —
    // which looks like a playback bug instead of a missing item, and silently
    // invalidates any measurement taken against it.
    // Show/season cast keys (Plex Web details Play on a series) have no Media —
    // expand via allLeaves to the first episode before FPGA clock/ladder work.
    if (xml.find("<MediaContainer") != std::string::npos &&
        xml.find("<Video") == std::string::npos &&
        xml.find("<Directory") != std::string::npos) {
        const std::string dirType = attr(xml, "Directory", "type");
        if (dirType == "show" || dirType == "season" || dirType.empty()) {
            std::string leavesUrl = plexBase + key;
            if (!leavesUrl.empty() && leavesUrl.back() == '/')
                leavesUrl.pop_back();
            leavesUrl += "/allLeaves?X-Plex-Container-Start=0&X-Plex-Container-Size=1";
            const std::string leaves = httpGet(leavesUrl, 12, metaHeaders);
            const std::string epKey = attr(leaves, "Video", "ratingKey");
            const std::string epPath = attr(leaves, "Video", "key");
            std::string nextKey;
            if (!epPath.empty() && epPath.rfind("/library/", 0) == 0)
                nextKey = epPath;
            else if (!epKey.empty())
                nextKey = std::string("/library/metadata/") + epKey;
            if (!nextKey.empty() && nextKey != key) {
                std::fprintf(stderr,
                             "misterplexd: cast container %s (%s) → first episode %s\n",
                             key.c_str(), dirType.empty() ? "directory" : dirType.c_str(),
                             nextKey.c_str());
                return resolvePlayTarget(nextKey, plexBase, token, offsetMs, weakAlways, weak,
                                         preferDirectH264, decodeW, decodeH, forceLadder, gateDetail,
                                         includeMarkers);
            }
        }
    }
    const bool metaFound = xml.find("<MediaContainer") != std::string::npos;
    if (metaFound) {
        // One unescape. Titles arrive as THE &#39;BURBS; a second pass would
        // turn a stored `&amp;lt;` into `<`.
        r.title = plexDisplayText(attr(xml, "Video", "title"));
        if (r.title.empty())
            r.title = plexDisplayText(attr(xml, "Directory", "title"));
        auto d = attr(xml, "Video", "duration");
        if (d.empty())
            d = attr(xml, "Media", "duration");
        if (!d.empty())
            r.durationMs = std::strtoll(d.c_str(), nullptr, 10);
        auto vo = attr(xml, "Video", "viewOffset");
        if (!vo.empty())
            r.viewOffsetMs = std::strtoll(vo.c_str(), nullptr, 10);
        r.ratingKey = attr(xml, "Video", "ratingKey");
        r.type = attr(xml, "Video", "type");
        r.parentKey = attr(xml, "Video", "parentKey");
        r.grandparentKey = attr(xml, "Video", "grandparentKey");
        if (const std::string pi = attr(xml, "Video", "parentIndex"); !pi.empty())
            r.parentIndex = static_cast<int>(std::strtol(pi.c_str(), nullptr, 10));
        if (const std::string ix = attr(xml, "Video", "index"); !ix.empty())
            r.index = static_cast<int>(std::strtol(ix.c_str(), nullptr, 10));
        if (includeMarkers)
            r.markers = markers::parseMarkers(xml, r.durationMs);
        if (chaptersEnabled())
            r.chapters = chapters::parseChapters(xml, r.durationMs);
        // Match-source-Hz / Content FPS: Media@videoFrameRate + video Stream@frameRate
        r.videoFrameRate = attr(xml, "Media", "videoFrameRate");
        if (r.videoFrameRate.empty())
            r.videoFrameRate = attr(xml, "Video", "videoFrameRate");
        // Fallback: any videoFrameRate="..." in the metadata payload
        if (r.videoFrameRate.empty()) {
            const std::string key = "videoFrameRate=\"";
            auto p = xml.find(key);
            if (p != std::string::npos) {
                p += key.size();
                auto e = xml.find('"', p);
                if (e != std::string::npos)
                    r.videoFrameRate = xml.substr(p, e - p);
            }
        }
        r.frameRate = attr(xml, "Stream", "frameRate");
        // Prefer video stream frameRate when Stream tags are mixed (first may be audio).
        // Scan all Stream open-tags for type="video" frameRate.
        {
            size_t sp = 0;
            while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
                auto end = xml.find('>', sp);
                if (end == std::string::npos)
                    break;
                const std::string slice = xml.substr(sp, end - sp);
                if (slice.find("type=\"video\"") != std::string::npos) {
                    auto fr = attrIn(slice, "frameRate");
                    if (!fr.empty())
                        r.frameRate = fr;
                    break;
                }
                sp = end + 1;
            }
        }
        r.sourceFpsHint = contentFpsHint(r.videoFrameRate, r.frameRate);
        parseExactFps(r.videoFrameRate, r.frameRate, r.fpsNum, r.fpsDen);

        // Source coded size: prefer video Stream width/height, else Media@.
        // Used by media_player to skip FOAR+pad only when source covers DECODE
        // (Grid720 1280×720 @720p bank). FOAR 720×480 must keep on-device pad (L38).
        {
            int mw = 0, mh = 0;
            size_t sp = 0;
            while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
                auto end = xml.find('>', sp);
                if (end == std::string::npos)
                    break;
                const std::string slice = xml.substr(sp, end - sp);
                const bool isVideo = slice.find("streamType=\"1\"") != std::string::npos ||
                                     slice.find("type=\"video\"") != std::string::npos;
                if (isVideo) {
                    auto w = attrIn(slice, "width");
                    auto h = attrIn(slice, "height");
                    if (w.empty())
                        w = attrIn(slice, "codedWidth");
                    if (h.empty())
                        h = attrIn(slice, "codedHeight");
                    if (!w.empty() && !h.empty()) {
                        mw = static_cast<int>(std::strtol(w.c_str(), nullptr, 10));
                        mh = static_cast<int>(std::strtol(h.c_str(), nullptr, 10));
                    }
                    break;
                }
                sp = end + 1;
            }
            if (mw <= 0 || mh <= 0) {
                auto w = attr(xml, "Media", "width");
                auto h = attr(xml, "Media", "height");
                if (!w.empty() && !h.empty()) {
                    mw = static_cast<int>(std::strtol(w.c_str(), nullptr, 10));
                    mh = static_cast<int>(std::strtol(h.c_str(), nullptr, 10));
                }
            }
            r.mediaWidth = mw > 0 ? mw : 0;
            r.mediaHeight = mh > 0 ? mh : 0;
        }
        {
            auto scan = attr(xml, "Media", "videoScanType");
            if (scan.empty())
                scan = attr(xml, "Media", "scanType");
            std::string scanLower = scan;
            for (char& c : scanLower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (scanLower == "interlaced")
                r.mediaInterlaced = true;
            if (scanLower == "progressive")
                r.mediaProgressive = true;
            if (!r.videoFrameRate.empty()) {
                const char last = r.videoFrameRate.back();
                if (last == 'i' || last == 'I')
                    r.mediaInterlaced = true;
                if (last == 'p' || last == 'P')
                    r.mediaProgressive = true;
            }
        }

        r.sourceAspect =
            sourceAspectFromPlexMetadata(xml, r.mediaWidth, r.mediaHeight);

        // Audio presence: dual-output ffmpeg (pipe:1 video + pipe:3 audio) aborts
        // with "Output file does not contain any stream" when the source has no
        // audio track (Grid720). Detect streamType=2 / type=audio up front.
        {
            bool foundAudio = false;
            size_t sp = 0;
            while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
                auto end = xml.find('>', sp);
                if (end == std::string::npos)
                    break;
                const std::string slice = xml.substr(sp, end - sp);
                if (slice.find("streamType=\"2\"") != std::string::npos ||
                    slice.find("type=\"audio\"") != std::string::npos) {
                    foundAudio = true;
                    break;
                }
                sp = end + 1;
            }
            // Only force false when we successfully parsed Stream tags at all.
            // Empty/malformed metadata keeps hasAudio=true (fail open).
            if (xml.find("<Stream") != std::string::npos)
                r.hasAudio = foundAudio;
        }
        r.tracks = parseStreamCatalog(xml);
        r.showKey = showMemoryKeyFromXml(xml);
        r.partId = partIdFromXml(xml);
        // SCRUB_THUMBS unset/off: no index kind, no extra GET. The metadata URL
        // above is unchanged either way.
        r.scrubIndexKind.clear();
        if (scrubThumbsEnabled()) {
            const scrub_thumbs::PartIndex ix = scrub_thumbs::partIndexFromXml(xml);
            if (ix.partId > 0 && r.partId <= 0)
                r.partId = ix.partId;
            r.scrubIndexKind = ix.kind;
        }
        r.partKey = partKeyFromXml(xml, r.partId);
        const std::optional<ShowPreference> pref =
            r.showKey.empty() ? std::nullopt : processShowTracks().find(r.showKey);
        r.trackPick = pickTracks(r.tracks, pref ? &*pref : nullptr, r.partId);
        // REMEMBER_TRACKS on: restore this show's saved Program-menu / setStreams
        // choice. Unset leaves the pickTracks result. playMedia ids below still
        // name the streams for this play.
        if (rememberTracksEnabled() && !r.showKey.empty()) {
            if (const std::optional<RememberedChoice> remembered = processRememberTracks().find(r.showKey)) {
                r.trackPick = applyRememberedTracks(r.tracks, *remembered, r.trackPick, r.partId);
                std::fprintf(stderr, "misterplexd: remember tracks show=%s audio=%d subtitle=%d off=%d\n",
                             r.showKey.c_str(), r.trackPick.audioStreamId, r.trackPick.subtitleStreamId,
                             r.trackPick.subtitlesOff ? 1 : 0);
            }
        }
        if (pendingPlayMediaAudioId() > 0) {
            const TrackPick defaultPick = r.trackPick;
            r.trackPick = pickAudioForPlay(r.tracks, r.trackPick, pendingPlayMediaAudioId(), r.partId);
            // Also non-default when PMS already marks the pick selected (an earlier part PUT) but the container's
            // first audio stream differs: a direct Part would still play the first stream.
            audioNeedsTranscode = audioPickIsNonDefault(defaultPick, r.trackPick) ||
                                  (r.trackPick.audioStreamId > 0 &&
                                   r.trackPick.audioStreamId != firstAudioStreamId(r.tracks, r.partId));
            std::fprintf(stderr, "misterplexd: audio pick id=%d applied=%d codec=%s lang=%s nonDefault=%d\n",
                         pendingPlayMediaAudioId(), r.trackPick.audioStreamId, r.trackPick.audioCodec.c_str(),
                         r.trackPick.audioLanguage.c_str(), audioNeedsTranscode ? 1 : 0);
        }
        {
            // PMS universal start.mp4 ignores the audioStreamID URL param and transcodes the part's selected
            // audio, so write the pick to the part BEFORE playback starts (what Plex clients do). A later
            // play of the part without a pick puts the original selection back.
            const int want = pendingPlayMediaAudioId();
            const int pmsSel = pmsSelectedAudioId(r.tracks, r.partId);
            const AudioPartPutPlan pl =
                processAudioSelection().plan(r.partId, pmsSel, want > 0 ? r.trackPick.audioStreamId : 0);
            if (pl.putAudioId > 0) {
                std::string putUrl = plexBase + partAudioSelectionPath(r.partId, pl.putAudioId);
                // #340: the token goes as an X-Plex-Token header, never in the PUT URL.
#ifdef MUT_340_PUT_TOKEN_QUERY   // test-only mutant: the old query token
                if (!token.empty())
                    putUrl += "&X-Plex-Token=" + urlEncodeQuery(token);
#endif
                const auto putFn = reinterpret_cast<AudioPartPutFn>(audioPartPutHook.load(std::memory_order_acquire));
                const bool ok = putFn ? putFn(putUrl, token) : plexHttpPut(putUrl, 8, nullptr, token);
                std::fprintf(stderr, "misterplexd: audio part PUT /library/parts/%d audioStreamID=%d restore=%d ok=%d\n",
                             r.partId, pl.putAudioId, pl.restore ? 1 : 0, ok ? 1 : 0);
                if (ok && pl.restore) {
                    r.trackPick = pickAudioForPlay(r.tracks, r.trackPick, pl.putAudioId, r.partId);
                    audioNeedsTranscode = false;
                } else if (!ok && !pl.restore) {
                    processAudioSelection().forget(r.partId);
                }
            }
        }
        if (pendingPlayMediaSubtitleId() >= 0) {
            SetStreamsRequest forced;
            forced.hasSubtitle = true;
            forced.subtitleStreamId = pendingPlayMediaSubtitleId();
            r.trackPick = pickFromSetStreams(r.tracks, r.trackPick, forced, r.partId);
        }
        applyTrackPickToLadder(playWeak, r, static_cast<bool>(pref) || pendingPlayMediaSubtitleId() >= 0);
        // FU14: BURN_SUBS_FIX follows the painter already on main (#201-r3). The part PUT
        // (#234) runs only when that flag is explicitly on. Off: this block does not run.
        // #324: the PUT also needs a server the signed-in user owns (burnPartPutGate).
        // Skipped: no plan, so no restore record and no PUT; one log line when a PUT
        // would have run. Allowed: restore-record behaviour as before.
        const BurnPutGate burnGate = burnPartPutGate(plexBase);
        if (burnGate != BurnPutGate::Allowed) {
            const int burnPick =
                playWeak.burnSubtitles && playWeak.subtitleStreamId > 0 ? playWeak.subtitleStreamId : 0;
            if (burnPick > 0 && burnPick != pmsSelectedSubtitleId(r.tracks, r.partId))
                std::fprintf(stderr, "misterplexd: %s part=%d subtitleStreamID=%d\n",
                             burnPutSkipReason(burnGate), r.partId, burnPick);
        } else {
            const int burnPick =
                playWeak.burnSubtitles && playWeak.subtitleStreamId > 0 ? playWeak.subtitleStreamId : 0;
            const SubtitlePartPutPlan pl = processSubtitleSelection().plan(
                r.partId, pmsSelectedSubtitleId(r.tracks, r.partId), burnPick);
#ifndef MUT_BURN_SUBS_SELECT_NO_PUT
            if (pl.put) {
                std::string putUrl = plexBase + partSubtitleSelectionPath(r.partId, pl.putSubtitleId);
                if (const std::string lan = plexDirectLanHttp(putUrl); !lan.empty())
                    putUrl = lan;
                // #340: the token goes as an X-Plex-Token header, never in the PUT URL.
#ifdef MUT_340_PUT_TOKEN_QUERY   // test-only mutant: the old query token
                if (!token.empty())
                    putUrl += "&X-Plex-Token=" + urlEncodeQuery(token);
#endif
                const auto putFn = reinterpret_cast<AudioPartPutFn>(audioPartPutHook.load(std::memory_order_acquire));
                const bool ok = putFn ? putFn(putUrl, token) : plexHttpPut(putUrl, 8, nullptr, token);
                std::fprintf(stderr,
                             "misterplexd: subtitle part PUT /library/parts/%d subtitleStreamID=%d restore=%d ok=%d\n",
                             r.partId, pl.putSubtitleId, pl.restore ? 1 : 0, ok ? 1 : 0);
                if (!ok && !pl.restore)
                    processSubtitleSelection().forget(r.partId);
            }
#else
            (void)pl;
#endif
        }
        r.softSubPath = tryOpenSubsFallback(xml, r);
    }

    // STREAM product path: prefer direct H.264 Part (elementary after demux) so host
    // CAVLC recon can work on Baseline/Main. PMS Chrome universal often emits High/CABAC.
    // STREAM=0 rate path: direct only when source Media *exactly matches* DECODE bank
    // (true 720 into 720 — skip PMS re-transcode). Do NOT treat FOAR 720×480 as covering
    // 640×480/320×240: that skipped the weak ladder and broke G0b (mode→PMS encode).
    // Never direct FOAR 720x480 into HD either (not an exact match).
    const bool metaOk = metaFound;
    if (metaOk && playWeak.owenb) {   // BUG1 pre-play: a known-CAVLC source above OB_CAVLC_HARD_MAX_KBPS
        auto envKbps = [](const char* a, const char* b, int def) {
            for (const char* k : {a, b})
                if (const char* v = std::getenv(k); v && *v && std::atoi(v) > 0)
                    return std::atoi(v);
            return def;
        };
        const ObSourceEntropy src = parseObSourceEntropy(xml);
        const int hard = envKbps("OB_CAVLC_HARD_MAX_KBPS", "MPX_OB_CAVLC_HARD_MAX_KBPS", 7000);
        const int cap = envKbps("OB_CAVLC_MAX_KBPS", "MPX_OB_CAVLC_MAX_KBPS", 4000);
        const int tsafe = envKbps("OB_TRANSCODE_SAFE_KBPS", "MPX_OB_TRANSCODE_SAFE_KBPS", 3200);
        const int beforeTs = playWeak.maxVideoBitrateKbps;
        if (owenbTranscodeSafeLadder(playWeak, src, tsafe))
            std::fprintf(stderr,
                         "misterplexd: OB pre-play: source %s (not H.264): PMS must transcode, first request "
                         "maxVideoBitrate %d -> %d (OB_TRANSCODE_SAFE_KBPS)\n",
                         src.codec.c_str(), beforeTs, playWeak.maxVideoBitrateKbps);
        const int before = playWeak.maxVideoBitrateKbps;
        if (owenbPrePlayCavlcLadder(playWeak, src, hard, cap))
            std::fprintf(stderr,
                         "misterplexd: OB pre-play: source CAVLC (profile=%s cabac=%d) at %d kbps > "
                         "OB_CAVLC_HARD_MAX_KBPS=%d: first request maxVideoBitrate %d -> %d\n",
                         src.profile.c_str(), src.cabac, src.bitrateKbps, hard, before,
                         playWeak.maxVideoBitrateKbps);
        else
            std::fprintf(stderr, "misterplexd: OB pre-play: source %s profile=%s cabac=%d %d kbps: ladder %d kbps\n",
                         src.codec.empty() ? "-" : src.codec.c_str(), src.profile.empty() ? "-" : src.profile.c_str(),
                         src.cabac, src.bitrateKbps, playWeak.maxVideoBitrateKbps);
    }
    const bool isH264 = metaOk && mediaVideoIsH264(xml);
    const bool mediaMatchesDecode = decodeW > 0 && decodeH > 0 && r.mediaWidth == decodeW &&
                                    r.mediaHeight == decodeH;
    // Optional profile tag for operator logs (High often implies CABAC sticky skip).
    auto videoProfileNote = [&]() -> std::string {
        if (!metaOk)
            return {};
        // Prefer video Stream profile, then Media@videoProfile / videoCodec.
        size_t sp = 0;
        while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
            auto end = xml.find('>', sp);
            if (end == std::string::npos)
                break;
            const std::string slice = xml.substr(sp, end - sp);
            const bool isVideo = slice.find("streamType=\"1\"") != std::string::npos ||
                                 slice.find("type=\"video\"") != std::string::npos;
            if (isVideo) {
                auto p = attrIn(slice, "profile");
                if (p.empty())
                    p = attrIn(slice, "videoProfile");
                if (!p.empty())
                    return p;
                break;
            }
            sp = end + 1;
        }
        auto p = attr(xml, "Media", "videoProfile");
        if (p.empty())
            p = attr(xml, "Media", "videoCodec");
        return p;
    };
    // STREAM=0 rate: direct Part only when Media exactly matches DECODE *and* profile
    // is baseline/main. High@L4 Grid720 measured ~17 pfps direct vs ~18.6 via Main
    // universal + scale bypass — keep weak ladder for High.
    const std::string srcProfEarly = videoProfileNote();
    auto profLower = srcProfEarly;
    for (char& c : profLower) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    const bool lightDirectProfile =
        profLower.find("baseline") != std::string::npos ||
        profLower == "main" || profLower.find("constrained baseline") != std::string::npos ||
        (profLower.find("main") != std::string::npos &&
         profLower.find("high") == std::string::npos);
    const bool wantDirect = !fpgaBackend &&
        key.rfind("/library", 0) == 0 &&
        (preferDirectH264 || (mediaMatchesDecode && lightDirectProfile));
    const bool directH264 = directH264Allowed(wantDirect, isH264, playWeak.burnSubtitles || audioNeedsTranscode);
    if (directH264) {
        const std::string prof = videoProfileNote();
        const std::string profSuffix = prof.empty() ? "" : (" profile=" + prof);
        auto partKey = attr(xml, "Part", "key");
        if (!partKey.empty()) {
            if (partKey[0] != '/')
                partKey = "/" + partKey;
            r.playable = plexBase + partKey;
            appendPlayableTokenQuery(r.playable, token);
            ensurePlayableTokenHeader(r, token, playWeak);
            r.ok = true;
            r.transcoded = false;
            r.transcodeUrl = buildUniversalTranscodeUrl(plexBase, key, token, makeSessionId(), 0, playWeak);
            r.detail = std::string("direct H.264 Part (") +
                       (preferDirectH264 ? "STREAM" : "media_covers_decode") + profSuffix + ")";
            return r;
        }
        auto file = attr(xml, "Part", "file");
        if (!file.empty()) {
            r.playable = urlDecode(file);
            if (libraryKeyMustNotSpawnLocalFile(key, r.playable)) {
                r.ok = false;
                r.playable.clear();
                r.transcoded = false;
                r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
                return r;
            }
            r.ok = true;
            r.transcoded = false;
            r.transcodeUrl = buildUniversalTranscodeUrl(plexBase, key, token, makeSessionId(), 0, playWeak);
            r.detail = std::string("direct H.264 Part file (") +
                       (preferDirectH264 ? "STREAM" : "media_covers_decode") + profSuffix + ")";
            return r;
        }
        // Fall through to universal if Part missing
    }

    // Reason the universal ladder was rejected by validateWeakLadder(); surfaces
    // in r.detail instead of the old misleading "remote transcoder" text.
    std::string whyBuild;
    // ALLOW_480P60: raise the owenb profile frame-rate cap before the universal
    // request is built. Flag off leaves directPlayFpsCap at 30 (tip behavior).
#ifndef MUT_480P60_RESOLVE_SKIP
    if (metaOk && owenbApplyAllow480p60FromMeta(playWeak, xml, r.mediaWidth, r.mediaHeight,
                                                r.fpsNum, r.fpsDen)) {
        std::fprintf(stderr,
                     "misterplexd: ALLOW_480P60 direct-play %dx%d @ %d/%d "
                     "(frameRate cap 60; 1280x720, 8-bit, bitrate, codec unchanged)\n",
                     r.mediaWidth, r.mediaHeight, r.fpsNum, r.fpsDen);
    }
#endif
    // reviewer-DIRECT-1030: REMOTE_H264_TRANSCODE on + remote + the source already fits the OB core
    // (8-bit H.264, <= 1280x720 and <= 108k MB/s, <= 8000k, known fps/refs): direct-play the Part.
    const bool rdirForced = metaFound && misterplex::remoteDirectForced(key);
    if (rdirForced)
        std::fprintf(stderr, "misterplexd: remote direct fallback: %s forced to the 480p transcode\n",
                     key.c_str());
    if (metaFound && !rdirForced && key.rfind("/library", 0) == 0 && misterplex::remoteH264TranscodeEnabled() &&
        std::strcmp(plexLocationForBase(plexBase), "wan") == 0) {
        const misterplex::RemoteDirectPmsCheck rd = misterplex::remoteDirectCheckFromPmsXml(xml);
        const bool fits = rd.fits;
        std::string pk = attr(xml, "Part", "key");
        std::fprintf(stderr, "misterplexd: remote direct check %s %s %dbit %dx%d L%d %dk %dfps refs=%d fits=%d\n",
                     rd.codec.c_str(), rd.profile.c_str(), rd.bitDepth, rd.mw, rd.mh, rd.level, rd.bitrateKbps,
                     rd.fpsHint, rd.refFrames, fits ? 1 : 0);
        if (misterplex::remoteDirectPlay(true, true, fits,
                                         !playWeak.burnSubtitles && !audioNeedsTranscode && !pk.empty())) {
            if (pk[0] != '/')
                pk = "/" + pk;
            r.ok = true;
            r.transcoded = false;
            r.playable = plexBase + pk;
            appendPlayableTokenQuery(r.playable, token);
            ensurePlayableTokenHeader(r, token, playWeak);
            r.detail = "remote direct Part (fits core: " + rd.codec + " " + rd.profile + " " +
                       std::to_string(rd.mw) + "x" + std::to_string(rd.mh) + " L" + std::to_string(rd.level) + " " +
                       std::to_string(rd.bitrateKbps) + "k) part=" + pk;
            misterplex::setRemoteDirectLastKey(key);
            return r;
        }
    }
    // Prefer weak universal for dual A9 (STREAM=0 cast path / non-H.264 STREAM)
    if ((weakAlways || fpgaBackend) && key.rfind("/library", 0) == 0) {
        WeakLadder requested = weak;
        if (fpgaBackend) {
            fpgaMode = "240p";
            const char* prototype = std::getenv("MPX_H264_PROTOTYPE");
            const char* filtering = std::getenv("MPX_H264_FILTER");
            if (filtering && std::string(filtering) != "on" && std::string(filtering) != "off") {
                r.detail = "FPGA H.264 filtering must be explicitly on or off";
                return r;
            }
            int exactNum = 0, exactDen = 0;
            // Prefer Stream@frameRate; also accept Media@videoFrameRate (PAL/NTSC/24p).
            // No numeric rate is decided after the raster mode: NTSC rasters must not guess.
            const bool fpsMissing =
                !parseExactFps(r.videoFrameRate, r.frameRate, exactNum, exactDen);
            // The ladder's FPGA mode (fpgaModeForLadder); 480p and 480i may use the
            // reserved 480 experiment ladders.
            fpgaMode = fpgaModeForLadder(weak);
            const bool allowReserved = fpgaMode != "240p";
            if (fpsMissing) {
                {
                    // No helper, or 480p: PMS universal at the product clock. Remote
                    // metadata often omits numeric fps; 24000/1001 is the noproxy default
                    // for every raster.
                    exactNum = 24000;
                    exactDen = 1001;
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s fps meta missing; default 24000/1001 "
                                 "for direct PMS (vfr=%s frameRate=%s)\n",
                                 fpgaModeName(fpgaMode),
                                 r.videoFrameRate.c_str(), r.frameRate.c_str());
                }
            }
            // Every FPGA raster: 24-family is 24000/1001, 30-family is 30000/1001.
            // 60-family becomes 30000/1001 (2:2 on 59.94). PAL dumps to film on
            // 480p; 240p/480i refuse anything outside 24/30/60-family.
            const bool ntscRaster = fpgaRasterNtsc(fpgaMode);
            const char* raster = fpgaModeName(fpgaMode);
            // No ladder proxy: apply the product clock directly.
            {
                int outNum = 0, outDen = 0;
                const bool fpsChanged =
                    fpgaContentFps(exactNum, exactDen, ntscRaster, outNum, outDen);
                const bool src24 = fpsIsTrue24(exactNum, exactDen);
                const bool src30 = fpsIsTrue30(exactNum, exactDen);
                const bool src60 = (exactNum == 60 && exactDen == 1) ||
                                   (exactNum == 60000 && exactDen == 1001);
                if (fpgaMode == "480p" && !fpga480pMinFpsOk(exactNum, exactDen)) {
                    r.fpsNum = exactNum;
                    r.fpsDen = exactDen;
                    r.detail = std::string("FPGA 480p raster: source fps ") +
                               std::to_string(exactNum) + "/" +
                               std::to_string(exactDen) +
                               " is below 480p min_fps " +
                               std::to_string(kMinFps480p) +
                               "; refusing (no remap)";
                    std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                    return r;
                }
                if (ntscRaster) {
                    const std::string src =
                        std::to_string(exactNum) + "/" + std::to_string(exactDen);
                    std::string refusal;
                    if (fpsChanged && !src24 && !src30 && !src60)
                        refusal = "source fps " + src + " is not 24/30/60-family";
                    if (refusal.empty() && fpgaMode == "480i" &&
                        !fpga480iMinFpsOk(exactNum, exactDen))
                        refusal = "source fps " + src + " is below 480i min_fps " +
                                  std::to_string(kMinFps480i);
                    if (refusal.empty() && fpgaMode == "240p" &&
                        !fpga240pMinFpsOk(exactNum, exactDen))
                        refusal = "source fps " + src + " is below 240p min_fps " +
                                  std::to_string(kMinFps240p);
                    if (!refusal.empty()) {
                        r.fpsNum = exactNum;
                        r.fpsDen = exactDen;
                        r.detail = std::string("FPGA ") + raster + " raster: " + refusal +
                                   "; refusing (no remap)";
                        std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                        return r;
                    }
                }
                const char* route =
                    std::strcmp(plexLocationForBase(plexBase), "wan") == 0
                        ? "no ladder proxy; remote PMS universal"
                        : "no ladder proxy; PMS source-rate profile";
                if (fpsChanged && src60)
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s raster fps halve %d/%d → %d/%d (%s)\n",
                                 raster, exactNum, exactDen, outNum, outDen, route);
                else if (fpsChanged)
                    std::fprintf(stderr,
                                 "misterplexd: FPGA remap source fps %d/%d → %d/%d "
                                 "(PMS transcode)\n",
                                 exactNum, exactDen, outNum, outDen);
                else if (fpsIsTrue30(outNum, outDen))
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s raster fps passthrough %d/%d (%s)\n",
                                 raster, outNum, outDen, route);
                exactNum = outNum;
                exactDen = outDen;
            }
            r.fpsNum = exactNum;
            r.fpsDen = exactDen;
            // Film-rate profile still sets the ladder geometry. A 30-family rate then
            // swaps in the raster-specific src30 variant; noprofile Extra pins
            // -r 30000/1001 (videoFrameRate on the universal URL).
            const bool fpsFilm = fpsIsTrue24(exactNum, exactDen);
            const int priorBr = weak.maxVideoBitrateKbps;
            const int priorQ = weak.videoQuality;
            if (!applyFpgaPlexProfile(requested, prototype ? prototype : "ip",
                                      fpsFilm ? exactNum : 24000, fpsFilm ? exactDen : 1001,
                                      filtering && std::string(filtering) == "off", &r.detail,
                                      fpgaMode, allowReserved))
                return r;
            // Overlay codec/profile/channels only. Leave FPGA videoResolution at
            // applyFpgaPlexProfile / fpga480pCodedWidthCeiling (624x480, not host-paint 720).
            if (const ModeResolveRow* row = modeResolveRow(fpgaMode)) {
                requested.videoCodec = row->videoCodec;
                requested.audioCodec = row->audioCodec;
                requested.h264ProfileParam = row->h264ProfileParam;
                requested.audioChannels = row->audioChannels;
            }
            if (requested.highCabac) {
                // Mode row is baseline,main. Plex_high rejects that PPS (error 16).
                requested.h264Profile = "main";
                requested.h264ProfileParam = "main";
                std::fprintf(stderr,
                             "misterplexd: High core CABAC ladder profile=main cabac=1 "
                             "ref=1 no-B no-8x8 (no named profile)\n");
            }
            if (fpsIsTrue30(exactNum, exactDen)) {
                requested.fpgaProfileVariant = fpgaNtscSourceProfileName(
                    prototype ? prototype : "ip", raster,
                    filtering && std::string(filtering) == "off");
                requested.fpgaFpsNum = exactNum;
                requested.fpgaFpsDen = exactDen;
            }
            // applyFpgaPlexProfile resets to profile defaults (4 Mbps / q60). Preserve
            // operator WEAK_* and always keep 240p under the 8KiB AU ring budget.
            if (priorBr > 0 && priorBr < requested.maxVideoBitrateKbps)
                requested.maxVideoBitrateKbps = priorBr;
            if (priorQ > 0 && priorQ < requested.videoQuality)
                requested.videoQuality = priorQ;
            if (fpgaMode == "240p") {
                constexpr int kStartMaxBitrateKbps = 1500;
                constexpr int kStartMaxQuality = 40;
                if (requested.maxVideoBitrateKbps > kStartMaxBitrateKbps)
                    requested.maxVideoBitrateKbps = kStartMaxBitrateKbps;
                if (requested.videoQuality > kStartMaxQuality)
                    requested.videoQuality = kStartMaxQuality;
            }
            // Seek/offset restarts: default FPGA ladder IDRs measured 10489 > 8192 AU.
            // Keep geometry/profile; only clamp bitrate/quality so PMS emits smaller AUs.
            if (offsetMs > 0) {
                // Live seek: 10489@4M, 8592@1M, 8s@750 PASS, cold@30s 8882@750 FAIL.
                // Drop further so complex-scene IDRs fit 8KiB AU ring.
                // Resume complex-scene IDRs still measured >8KiB at 500k; 350 keeps ring.
                const FpgaAuSafeClamp auClamp =
                    fpgaAuSafeClamp(fpgaAuSafeTier480(requested) || fpgaMode == "480p" ||
                                   fpgaMode == "480i",
                                   offsetMs, true);
                if (requested.maxVideoBitrateKbps > auClamp.maxBitrateKbps)
                    requested.maxVideoBitrateKbps = auClamp.maxBitrateKbps;
                if (requested.videoQuality > auClamp.maxQuality)
                    requested.videoQuality = auClamp.maxQuality;
            }
        }
        const WeakLadder transcodeWeak = fitWeakLadderToAspect(requested, r.sourceAspect);
        const std::string session = makeSessionId();
        r.sessionId = session;
const std::string start = buildUniversalTranscodeUrl(plexBase, key, token, session,
                                                              offsetMs > 0 ? offsetMs : 0,
                                                              transcodeWeak, &whyBuild);
        if (!start.empty())
            r.transcodeUrl = start;
        const bool remoteBase = std::strcmp(plexLocationForBase(plexBase), "wan") == 0;
        // #275: no ladder helper; FPGA always takes PMS universal.
        if (fpgaBackend && metaOk) {
            if (ensureUniversalDecision(start, session, token, transcodeWeak)) {
                goto remote_universal_decision_ok;
            } else {
                if (misterplex::remoteTranscodeStopApplies(misterplex::remoteH264TranscodeEnabled(),
                                                           remoteBase, metaFound)) {
                    r.detail = "remote PMS H.264 transcode decision failed; no direct Part on remote "
                               "(REMOTE_H264_TRANSCODE=1)";
                    if (!whyBuild.empty())
                        r.detail += " (" + whyBuild + ")";
                    std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                    return r;
                }
                r.detail = remoteBase ? "remote universal decision failed; checking the original Part"
                                      : "universal decision failed; checking the original Part";
                if (forceLadder)
                    r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
                if (!whyBuild.empty())
                    r.detail += " (" + whyBuild + ")";
            }
        } else if (ensureUniversalDecision(start, session, token, transcodeWeak)) {
        remote_universal_decision_ok:
            // Shared/remote PMS often advertises Conversion OK then closes start with
            // 0 body (no working transcoder for the share). Probe before claiming ok.
            bool startAlive = true;
            if (remoteBase) {
                const std::vector<std::pair<std::string, std::string>> probeHdr =
                    plexStartProbeHeaders(session, token, transcodeWeak, start);
                // #207 r3: log the exact start request (tokens REDACTED) so a 400 can be diffed.
                std::string startLog = misterplex::redactUrlTokens(start);
                if (startLog.size() > 4096)
                    startLog.resize(4096);
                std::fprintf(stderr, "misterplexd: remote universal start url=%s\n", startLog.c_str());
                for (const auto& hv : probeHdr) {
                    const std::string val = hv.first.find("Token") != std::string::npos
                                                ? std::string("REDACTED")
                                                : misterplex::redactPlexBody(hv.second, 16384);
                    const auto parts = misterplex::remoteHdrLogChunks(val, 400);
                    for (std::size_t pi = 0; pi < parts.size(); ++pi)
                        std::fprintf(stderr, "misterplexd: remote universal start hdr %s [%zu/%zu]: %s\n",
                                     hv.first.c_str(), pi + 1, parts.size(), parts[pi].c_str());
                }
                if (transcodeWeak.highCabac) {
                    // VideoEncodeFlags is percent-encoded, so the header holds cabac%3D1.
                    bool sawHeader = false;
                    bool sawCabac = false;
                    for (const auto& hv : probeHdr) {
                        if (hv.first == "X-Plex-Client-Profile-Extra")
                            sawHeader = true;
                        if (hv.second.find("cabac=1") != std::string::npos ||
                            hv.second.find("cabac%3D1") != std::string::npos ||
                            hv.second.find("cabac%3d1") != std::string::npos)
                            sawCabac = true;
                    }
                    std::fprintf(stderr, "misterplexd: start probe Profile-Extra %s\n",
                                 sawCabac ? "cabac=1"
                                          : sawHeader ? "header-without-cabac=1"
                                                      : "no-Profile-Extra");
                } else if (isFpgaPlexProfile(transcodeWeak)) {
                    bool sawCabac0 = false;
                    for (const auto& hv : probeHdr) {
                        if (hv.second.find("cabac=0") != std::string::npos ||
                            hv.second.find("cabac%3D0") != std::string::npos ||
                            hv.second.find("cabac%3d0") != std::string::npos)
                            sawCabac0 = true;
                    }
                    std::fprintf(stderr, "misterplexd: start probe Profile-Extra %s\n",
                                 sawCabac0 ? "cabac=0" : "no-cabac=0");
                }
                // Unset: one ranged GET (status + leading bytes). MPX_PREWARM=0
                // keeps the two-GET drain that held 075 for ~20s after the decision.
                const bool prewarmProbe = plexPrewarmProbeEnabled();
                int probeCapKb =
                    remote_start::probeCapKb(std::getenv("MPX_REMOTE_PROBE_CAP_KB"));
                // #276: never drain 37–72 MB. "0" is not a rollback here.
                if (probeCapKb <= 0)
                    probeCapKb = remote_start::kDefaultProbeCapKb;
                const size_t probeCapBytes = static_cast<size_t>(probeCapKb) * 1024u;
                // fr-resume-1025: a cold remote transcoder needs more than one 24 s try (glass 1025: dead at
                // 24.1 s, the repeat from idle showed picture at +31.7 s). Retry inside a budget.
                const int probeBudgetS =
                    remote_start::budgetSeconds(std::getenv("MPX_REMOTE_START_BUDGET_S"));
                const int probeTries = remote_start::attemptsForBudget(probeBudgetS);
                int probeHttp = 0;
                std::string probeBody;
                const auto probeStart = std::chrono::steady_clock::now();
                auto probeOnce = [&]() -> remote_start::Attempt {
                    const RemoteStartProbeResult pr = remoteStartProbeOnce(
                        start, probeHdr, plexRequestProfileName(transcodeWeak, start), probeCapBytes, prewarmProbe);
                    probeHttp = pr.httpStatus;
                    probeBody = pr.body;
                    remote_start::Attempt a;
                    a.http = pr.httpStatus;
                    a.bytes = pr.body.size();
                    a.alive = pr.alive;
                    // #276: stop this attempt's session when it is dead so a retry
                    // does not leave a live transcoder (only the first session used to stop).
                    if (!pr.alive) {
                        const std::string stopUrl = remote_start::abandonedStopUrl(plexBase, session);
                        if (!stopUrl.empty())
                            (void)plexHttpGetNoBodyResult(stopUrl, probeHdr, 4);
                    }
                    return a;
                };
                const remote_start::Outcome probeRun = remote_start::runProbe(
                    probeTries, probeOnce, [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
                startAlive = probeRun.alive;
                const auto probeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - probeStart)
                                         .count();
                // bytes= is from the capped read when cap_kb>0 (see probeCapBytes), not full entity length.
                std::fprintf(stderr,
                             "misterplexd: remote start probe ms=%lld http=%d bytes=%zu bytes_capped=%d "
                             "prewarm=%d attempts=%d/%d budget_s=%d cap_kb=%d\n",
                             static_cast<long long>(probeMs), probeHttp, probeBody.size(),
                             probeCapKb > 0 ? 1 : 0, prewarmProbe ? 1 : 0, probeRun.attempts,
                             probeTries, probeBudgetS, probeCapKb);
                if (!startAlive) {
                    r.remoteUniversalStartHttp = probeHttp;
                    std::fprintf(stderr,
                                 "misterplexd: remote universal start dead http=%d — "
                                 "shared PMS transcoder produced no media\n",
                                 probeHttp);
                    // #207: log WHY PMS refused the start (body truncated, tokens REDACTED).
                    std::string why = probeBody;
                    if (why.empty() && probeHttp >= 400)
                        why = plexHttpGetStatusAndBody(start, probeHdr,
                                                       plexRequestProfileName(transcodeWeak, start), 8)
                                  .body;
                    std::fprintf(stderr, "misterplexd: remote universal start body http=%d: %s\n", probeHttp,
                                 misterplex::redactPlexBody(why).c_str());
                    // The probe started a transcode session on that server; nothing will play it. Stop it
                    // (best effort, token in a header only, never fatal) instead of leaving it to PMS's idle kill.
                    const std::string stopUrl = remote_start::abandonedStopUrl(plexBase, session);
                    if (!stopUrl.empty()) {
                        const auto stop = plexHttpGetNoBodyResult(stopUrl, probeHdr, 4);
                        std::fprintf(stderr, "misterplexd: abandoned remote start stop http=%d ok=%d\n",
                                     stop.httpStatus, stop.ok ? 1 : 0);
                    }
                }
            }
            {
                const DecisionVideo dv = lastDecisionVideo();
                if (dv.width > 0 && dv.height > 0) {
                    r.decisionWidth = dv.width;
                    r.decisionHeight = dv.height;
                }
                const std::string partMm = decisionCoreMismatch(dv, transcodeWeak);
                std::string partKey = dv.partKey;
                if (partKey.empty())
                    partKey = attr(xml, "Part", "key");
                // #207: a remote play never takes the source Part when REMOTE_H264_TRANSCODE is on.
                const bool takePart = misterplex::remoteMayTakePart(
                    misterplex::remoteH264TranscodeEnabled(), remoteBase,
                    !transcodeWeak.burnSubtitles && !audioNeedsTranscode && decisionIsDirectPlay(dv) &&
                        partMm.empty() && !partKey.empty());
                if (takePart || startAlive) {
                r.ok = true;
                r.httpHeaders = plexFfmpegHeaders(session, token, transcodeWeak,
                                                  /*forceLadderCaps=*/remoteBase, start);
                if (transcodeWeak.highCabac) {
                    const bool playCabac = r.httpHeaders.find("cabac=1") != std::string::npos ||
                                           r.httpHeaders.find("cabac%3D1") != std::string::npos ||
                                           r.httpHeaders.find("cabac%3d1") != std::string::npos;
                    std::fprintf(stderr, "misterplexd: PLAY Profile-Extra %s\n",
                                 playCabac ? "cabac=1" : "no-cabac=1");
                }
                r.clientProfileName = plexRequestProfileName(transcodeWeak, start);
                if (takePart) {
                    if (partKey[0] != '/')
                        partKey = "/" + partKey;
                    r.transcoded = false;
                    r.playable = plexBase + partKey;
                    appendPlayableTokenQuery(r.playable, token);
                    ensurePlayableTokenHeader(r, token, transcodeWeak);
                    r.detail = "PMS direct " + transcodeWeak.profileName + " " +
                               transcodeWeak.videoResolution + " " + key +
                               " part=" + partKey + " part_mismatch=";
                } else {
                    r.transcoded = true;
                    r.playable = start;
                    r.detail = "PMS universal " + transcodeWeak.profileName + " " +
                               transcodeWeak.videoResolution + " " + key +
                               " part_mismatch=" + partMm;
                }
                if (forceLadder)
                    r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
                if (playableIsFarpointIdentityCache(r.playable) ||
                    libraryKeyMustNotSpawnLocalFile(key, r.playable)) {
                    r.ok = false;
                    r.playable.clear();
                    r.transcoded = false;
                    r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
                    return r;
                }
                // STREAM preferDirect fallthrough: operator can see why recon may hit CABAC.
                if (preferDirectH264) {
                    if (!metaOk)
                        r.detail += " (STREAM preferDirect: no metadata)";
                    else if (!isH264)
                        r.detail += " (STREAM preferDirect: source not H.264)";
                    else
                        r.detail += " (STREAM preferDirect: H.264 Part missing → universal may be High/CABAC)";
                }
                return r;
                }
            }
            // Remote decision OK but start body empty: try direct Part / lab proxy below.
            r.detail = "remote PMS transcoder returned no media; checking the original Part";
        } else {
            r.detail = remoteBase ? "remote universal decision failed; checking the original Part"
                                  : "universal decision failed; checking the original Part";
            if (forceLadder)
                r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
        }
    }

    // #207 ( lab log 10-30: decision=transcode, start probe http=400, then "resolved direct Part" of the
    // HEVC original): with REMOTE_H264_TRANSCODE on, a remote play with no ladder helper never falls back
    // to the original Part. It errors with the PMS reason instead (HEVC refused / H.264 black screen).
    if (misterplex::remoteTranscodeStopApplies(
            misterplex::remoteH264TranscodeEnabled(),
            std::strcmp(plexLocationForBase(plexBase), "wan") == 0, metaFound)) {
        r.detail = "remote PMS H.264 transcode unavailable (" + r.detail +
                   "); no original Part on a remote server (REMOTE_H264_TRANSCODE=1)";
        std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
        return r;
    }
    // Fallback: direct Part stream URL from metadata (no ladder proxy since #275).
    if (metaFound) {
        // #207: never hand the core a direct Part outside its decode envelope on a remote server (LAN unchanged).
        const ObSourceEntropy se = parseObSourceEntropy(xml);
        misterplex::ObDirectSource ds;
        ds.codec = se.codec;
        ds.profile = se.profile;
        ds.bitrateKbps = se.bitrateKbps;
        ds.width = std::atoi(attr(xml, "Media", "width").c_str());
        ds.height = std::atoi(attr(xml, "Media", "height").c_str());
        const auto vs = xml.find("streamType=\"1\"");
        if (vs != std::string::npos) {
            const auto ts = xml.rfind("<Stream", vs);
            const auto te = xml.find('>', vs);
            if (ts != std::string::npos && te != std::string::npos)
                ds.level = std::atoi(attrIn(xml.substr(ts, te - ts), "level").c_str());
        }
        const bool decodable = misterplex::obDirectDecodable(ds);
        if (!misterplex::directFallbackAllowed(misterplex::remoteH264TranscodeEnabled(),
                                               std::strcmp(plexLocationForBase(plexBase), "wan") == 0, decodable)) {
            r.ok = false;
            r.detail = "direct Part outside the OB decode envelope (" + ds.codec + " " + ds.profile + " " +
                       std::to_string(ds.width) + "x" + std::to_string(ds.height) + " L" +
                       std::to_string(ds.level) + " " + std::to_string(ds.bitrateKbps) +
                       "k) after: " + r.detail + " (REMOTE_H264_TRANSCODE=1)";
            std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
            return r;
        }
    }
    if (metaFound) {
        auto partKey = attr(xml, "Part", "key");
        if (!partKey.empty()) {
            if (partKey[0] != '/')
                partKey = "/" + partKey;
            std::string partUrl = plexBase + partKey;
            appendPlayableTokenQuery(partUrl, token);


            // FPGA needs annex-B H.264 AUs. Direct Part is only viable when the
            // source is already H.264 (remote shared PMS often has a dead transcoder).
            if (fpgaBackend && !mediaVideoIsH264(xml)) {
                r.ok = false;
                r.playable.clear();
                if (!whyBuild.empty())
                    r.detail = "FPGA ladder rejected (" + whyBuild +
                               "); remote/shared PMS had no stream to fall back to "
                               "(source is not H.264)";
                else
                    r.detail =
                        "remote/shared PMS transcoder produced no stream and source is not "
                        "H.264";
                return r;
            }
            // Gate fallback already tried PMS universal; the Part is the stream the
            // gate refused. Never replay it as a "direct Part".
            if (forceLadder) {
                r.ok = false;
                r.playable.clear();
                r.detail = "gate fallback: " + gateDetail +
                           "; PMS universal failed (never a direct Part)";
                return r;
            }
            // The early direct-H.264 gate already refused a burn. This fallback
            // must not hand the Part back: a direct Part does not burn subtitles.
            if (!playWeak.burnSubtitles && !audioNeedsTranscode) {
                r.playable = partUrl;
                r.ok = true;
                r.transcoded = false;
                r.httpHeaders = plexFfmpegHeaders(makeSessionId(), token, weak);
                r.detail = fpgaBackend ? "direct H.264 Part (remote transcoder unavailable)"
                                       : "direct Part stream";
                return r;
            }
        }
        auto file = attr(xml, "Part", "file");
        if (!file.empty() && !playWeak.burnSubtitles && !audioNeedsTranscode) {
            r.playable = urlDecode(file);
            r.ok = true;
            r.detail = "Part file path";
            return r;
        }
    }

    if (!metaFound)
        r.detail = "no such item on PMS (" + key + " returned no MediaContainer — "
                   "deleted, renumbered by a library re-scan, or bad token)";
    if (r.detail.empty())
        r.detail = "resolve failed for " + key;
    return r;
}

PlayQueue fetchPlayQueue(const std::string& queueIdOrContainerKey, const std::string& plexBase,
                         const std::string& token, const std::string& currentKey,
                         const std::string& playQueueItemId) {
    PlayQueue q;
    q.currentIndex = -1;
    std::string raw = urlDecode(queueIdOrContainerKey);
    std::string extraQuery;
    auto qmark = raw.find('?');
    if (qmark != std::string::npos) {
        extraQuery = raw.substr(qmark + 1);
        raw = raw.substr(0, qmark);
    }
    std::string id = raw;
    auto slash = id.find_last_of('/');
    if (slash != std::string::npos)
        id = id.substr(slash + 1);
    while (!id.empty() && !std::isdigit(static_cast<unsigned char>(id.back())))
        id.pop_back();
    if (id.empty() || !std::isdigit(static_cast<unsigned char>(id.front()))) {
        q.detail = "empty/invalid play queue id from '" + queueIdOrContainerKey + "'";
        return q;
    }

    std::string base = normalizePlexBase(plexBase);
    if (base.empty()) {
        q.detail = "no PMS base for play queue";
        return q;
    }

    std::string url = base + "/playQueues/" + id + "?";
    if (!extraQuery.empty())
        url += extraQuery;
    else
        url += "own=1";
    HeaderList pqHeaders;   // #340: token as a header
    if (!token.empty())
        pqHeaders.push_back({"X-Plex-Token", token});

    const std::string xml = httpGet(url, 20, pqHeaders);
    if (xml.empty() || xml.find("MediaContainer") == std::string::npos) {
        q.detail = "play queue fetch failed id=" + id + " bytes=" + std::to_string(xml.size());
        return q;
    }
    return parsePlayQueueXml(xml, id, currentKey, playQueueItemId);
}

namespace {
// An attribute of the opening tag only (not "subtype=" nor a child element's attribute).
std::string openTagAttr(const std::string& slice, const char* name) {
    const auto tagEnd = slice.find('>');
    const std::string tag = slice.substr(0, tagEnd == std::string::npos ? slice.size() : tagEnd);
    const std::string key = std::string(" ") + name + "=\"";
    auto p = tag.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    const auto e = tag.find('"', p);
    return e == std::string::npos ? std::string() : tag.substr(p, e - p);
}
} // namespace

PlayQueue parsePlayQueueXml(const std::string& xml, const std::string& id, const std::string& currentKey,
                            const std::string& playQueueItemId) {
    PlayQueue q;
    q.currentIndex = -1;
    if (xml.find("MediaContainer") == std::string::npos) {
        q.detail = "play queue fetch failed id=" + id + " bytes=" + std::to_string(xml.size());
        return q;
    }
    q.containerKey = "/playQueues/" + id;
    q.playQueueId = id;
    {
        auto a = xml.find("playQueueID=\"");
        if (a != std::string::npos) {
            a += 13;
            auto b = xml.find('"', a);
            if (b != std::string::npos)
                q.playQueueId = xml.substr(a, b - a);
        }
        a = xml.find("playQueueVersion=\"");
        if (a != std::string::npos) {
            a += 18;
            auto b = xml.find('"', a);
            if (b != std::string::npos)
                q.playQueueVersion = xml.substr(a, b - a);
        }
    }

    size_t pos = 0;
    while ((pos = xml.find("<Video", pos)) != std::string::npos) {
        auto tagEnd = xml.find('>', pos);
        if (tagEnd == std::string::npos)
            break;
        size_t sliceEnd = tagEnd;
        auto end2 = xml.find("</Video>", pos);
        auto endSlash = xml.find("/>", pos);
        if (endSlash != std::string::npos && endSlash < tagEnd + 8)
            sliceEnd = endSlash;
        if (end2 != std::string::npos)
            sliceEnd = std::max(sliceEnd, end2);
        const std::string slice = xml.substr(pos, std::min<size_t>(4000, sliceEnd - pos + 8));

        QueueItem item;
        item.key = attrIn(slice, "key");
        item.ratingKey = attrIn(slice, "ratingKey");
        item.playQueueItemId = attrIn(slice, "playQueueItemID");
        if (item.playQueueItemId.empty())
            item.playQueueItemId = attrIn(slice, "playQueueItemId");
        item.title = plexDisplayText(attrIn(slice, "title"));
        item.type = openTagAttr(slice, "type");
        auto d = attrIn(slice, "duration");
        if (!d.empty())
            item.durationMs = std::strtoll(d.c_str(), nullptr, 10);
        if (!item.key.empty())
            q.items.push_back(item);
        pos = tagEnd + 1;
    }

    if (q.items.empty()) {
        q.detail = "play queue had no Video items index=-1 none";
        return q;
    }

    const char* by = "none";
    q.currentIndex = selectQueueIndex(q.items, currentKey, playQueueItemId, &by);

    q.ok = true;
    q.detail = "queue size=" + std::to_string(q.items.size()) +
               (q.currentIndex >= 0
                    ? " index=" + std::to_string(q.currentIndex) + " by=" + by
                    : " index=-1 none");
    return q;
}

namespace {

int bucketFps(double fps) {
    if (fps <= 0.0)
        return 0;
    // Nearest of OSD Content FPS options 12 / 24 / 30 / 60
    const int opts[] = {12, 24, 30, 60};
    int best = 24;
    double bestDiff = 1e9;
    for (int o : opts) {
        double d = std::fabs(fps - static_cast<double>(o));
        if (d < bestDiff) {
            bestDiff = d;
            best = o;
        }
    }
    // 23.976 → 24, 29.97 → 30, 59.94 → 60
    return best;
}

} // namespace

int contentFpsHint(const std::string& videoFrameRate, const std::string& frameRate) {
    // Prefer numeric stream frameRate
    if (!frameRate.empty()) {
        char* end = nullptr;
        double v = std::strtod(frameRate.c_str(), &end);
        if (end != frameRate.c_str() && v > 0.0)
            return bucketFps(v);
    }
    if (videoFrameRate.empty())
        return 0;
    std::string s = videoFrameRate;
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Common PMS tokens
    if (s == "ntsc" || s.find("29.97") != std::string::npos || s == "30p" || s == "30")
        return 30;
    if (s == "pal" || s.find("25") == 0 || s == "25p")
        return 24; // nearest OSD bucket (no 25); cadence at 24 is less wrong than 30 for film-ish
    if (s == "24p" || s == "24" || s.find("23.9") != std::string::npos ||
        s.find("film") != std::string::npos)
        return 24;
    if (s == "60p" || s == "60" || s.find("59.9") != std::string::npos || s == "120p")
        return 60;
    if (s == "12p" || s == "12")
        return 12;
    // Strip trailing 'p' and parse number
    if (!s.empty() && (s.back() == 'p' || s.back() == 'i'))
        s.pop_back();
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && v > 0.0)
        return bucketFps(v);
    return 0;
}

int applySourceFpsConf(const std::string& sourceFpsConf, int resolvedHint) {
    std::string s = sourceFpsConf;
    // trim
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.empty() || s == "auto")
        return resolvedHint;
    if (s == "off" || s == "0" || s == "none")
        return 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end != s.c_str() && v > 0)
        return bucketFps(static_cast<double>(v));
    return resolvedHint;
}

namespace {

struct FpsRational {
    int num;
    int den;
};

// Standard broadcast rates. 1001-denominator entries must snap exactly: treating
// 23.976 as 24 costs ~1 ms/s of lipsync drift (~234 ms by 3:54, ~5.5 s over 91 min).
constexpr FpsRational kStdFps[] = {
    {12, 1},        {15, 1},        {24000, 1001}, {24, 1},       {25, 1},
    {30000, 1001},  {30, 1},        {48, 1},       {50, 1},       {60000, 1001},
    {60, 1},
};

// Snap a decimal rate onto the standard family. Tolerance is well under the
// 23.976↔24 gap (0.024) so the NTSC pairs never collapse into each other.
bool snapStdFps(double v, int& num, int& den) {
    if (!(v > 0.0) || v > 1000.0)
        return false;
    for (const auto& f : kStdFps) {
        const double std_v = static_cast<double>(f.num) / static_cast<double>(f.den);
        if (std::fabs(v - std_v) <= 0.010) {
            num = f.num;
            den = f.den;
            return true;
        }
    }
    // Not a standard rate — keep it exact to 3 decimals rather than rounding to int.
    long n = std::lround(v * 1000.0);
    if (n <= 0)
        return false;
    num = static_cast<int>(n);
    den = 1000;
    return true;
}

std::string lowerTrim(const std::string& in) {
    std::string s = in;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool parseExactFps(const std::string& videoFrameRate, const std::string& frameRate,
                   int& num, int& den) {
    num = 0;
    den = 0;
    // Prefer the numeric video Stream@frameRate — it carries the real 23.976/29.97.
    if (!frameRate.empty()) {
        const std::string rate = lowerTrim(frameRate);
        const auto slash = rate.find('/');
        if (slash != std::string::npos) {
            const std::string numerator = rate.substr(0, slash);
            const std::string denominator = rate.substr(slash + 1);
            char *nEnd = nullptr, *dEnd = nullptr;
            errno = 0;
            const long n = std::strtol(numerator.c_str(), &nEnd, 10);
            const bool nOk = errno == 0;
            errno = 0;
            const long d = std::strtol(denominator.c_str(), &dEnd, 10);
            if (nOk && errno == 0 && nEnd != numerator.c_str() && !*nEnd &&
                dEnd != denominator.c_str() && !*dEnd && n > 0 && d > 0 &&
                n <= std::numeric_limits<int>::max() && d <= std::numeric_limits<int>::max() &&
                static_cast<double>(n) / d <= 1000.0) {
                const long divisor = std::gcd(n, d);
                num = static_cast<int>(n / divisor);
                den = static_cast<int>(d / divisor);
                return true;
            }
        } else {
            char* end = nullptr;
            const double v = std::strtod(rate.c_str(), &end);
            if (end != rate.c_str() && !*end && v > 0.0 && snapStdFps(v, num, den)) {
                // PMS 24p/30p with Stream@frameRate "24.000"/"30.000" is NTSC
                // (048 H1 paced 24/1 and glass dupfps fell to 22.81).
                const std::string vfr = lowerTrim(videoFrameRate);
                if ((vfr == "24p" || vfr == "film") && num == 24 && den == 1) {
                    num = 24000;
                    den = 1001;
                } else if (vfr == "30p" && num == 30 && den == 1) {
                    num = 30000;
                    den = 1001;
                }
                return true;
            }
        }
    }
    if (videoFrameRate.empty())
        return false;

    std::string s = lowerTrim(videoFrameRate);
    if (s.empty())
        return false;
    // PMS Media@videoFrameRate tokens. NTSC/PAL are film/broadcast families, not integers.
    if (s == "ntsc") {
        num = 30000;
        den = 1001;
        return true;
    }
    if (s == "pal") {
        num = 25;
        den = 1;
        return true;
    }
    // PMS Media@videoFrameRate="24p" is NTSC film on Trek/Blu-ray (23.976).
    // True 24.000 still wins when Stream@frameRate is "24" / "24.000".
    if (s == "film" || s == "24p") {
        num = 24000;
        den = 1001;
        return true;
    }
    if (s == "30p") {
        num = 30000;
        den = 1001;
        return true;
    }
    // Strip a trailing progressive/interlaced marker ("24p", "60i").
    if (s.back() == 'p' || s.back() == 'i')
        s.pop_back();
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && v > 0.0)
        return snapStdFps(v, num, den);
    return false;
}

bool capExactFpsToMaxHz(int& num, int& den, double maxHz) {
    if (num <= 0 || den <= 0 || !(maxHz > 0.0))
        return false;
    const double v = static_cast<double>(num) / static_cast<double>(den);
    if (v <= maxHz + 0.010)
        return false;
    if (num == 60 && den == 1 && maxHz >= 30.0 - 0.010) {
        num = 30;
        den = 1;
        return true;
    }
    if (num == 60000 && den == 1001 && maxHz >= 30000.0 / 1001.0 - 0.010) {
        num = 30000;
        den = 1001;
        return true;
    }
    if (num == 50 && den == 1 && maxHz >= 25.0 - 0.010) {
        num = 25;
        den = 1;
        return true;
    }
    if (num == 48 && den == 1 && maxHz >= 24.0 - 0.010) {
        num = 24;
        den = 1;
        return true;
    }
    num = static_cast<int>(maxHz);
    den = 1;
    if (num < 1)
        num = 1;
    return true;
}

bool applyContentFpsConf(const std::string& conf, int& num, int& den) {
    const std::string s = lowerTrim(conf);
    if (s.empty() || s == "auto")
        return false;
    if (s == "off" || s == "0" || s == "none") {
        num = 0;
        den = 0;
        return true;
    }
    // "24000/1001" rational form
    const auto slash = s.find('/');
    if (slash != std::string::npos) {
        char* e1 = nullptr;
        char* e2 = nullptr;
        const std::string ns = s.substr(0, slash);
        const std::string ds = s.substr(slash + 1);
        long n = std::strtol(ns.c_str(), &e1, 10);
        long d = std::strtol(ds.c_str(), &e2, 10);
        if (e1 != ns.c_str() && e2 != ds.c_str() && n > 0 && d > 0) {
            num = static_cast<int>(n);
            den = static_cast<int>(d);
            return true;
        }
        return false;
    }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && v > 0.0)
        return snapStdFps(v, num, den);
    return false;
}

} // namespace misterplex
