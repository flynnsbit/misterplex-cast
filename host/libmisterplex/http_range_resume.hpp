#pragma once
// Direct-path HTTP Range resume (#223). PMS Part files are served with a
// Content-Length. A mid-title close ("Stream ends prematurely at N, should be
// M") is not the end of the title: reopen with Range: bytes=<offset>- and keep
// the same auth headers / session. AirPlay, ladder, YouTube and a universal
// transcode stay off this path (live or already resumed another way).
//
// Pure: no sockets. The reader (http_range_reader.hpp) and the libav AVIO
// glue (http_range_avio.hpp) call these helpers.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {
namespace httprange {

constexpr unsigned kMaxResumes = 5;
// Wall-clock cap for resume + mid-stream waits. libav rw_timeout was 20 s;
// without this, a dead server is 20 s read + 5 × (15 s connect / 20 s head).
constexpr int64_t kResumeBudgetMs = 20000;

// #231 Glass Run F: a reopen during a ~5 s drop hit "connect failed" once and
// the session died. Transport failures on (re)open retry 1,2,4,8 s, every
// sleep and connect wait clamped to kResumeBudgetMs (whole window <= 20 s).
constexpr int64_t kReconnectBackoffMs[] = {1000, 2000, 4000, 8000};
constexpr int kReconnectRetries = static_cast<int>(sizeof kReconnectBackoffMs / sizeof kReconnectBackoffMs[0]);
// Per-attempt connect wait: short, polled for cancel every <=100 ms, so a
// black-holed host (SYN drop) costs seconds, not 30 s x attempts.
#ifdef MUT_HTTP_RANGE_LONG_CONNECT
constexpr int kConnectTimeoutMs = 15000;
#else
constexpr int kConnectTimeoutMs = 3000;
#endif

// Next retry sleep, clamped to what is left of the armed budget. 0 = stop.
inline int64_t reconnectDelayMs(int i, int64_t nowMs, int64_t startedMs, int64_t budgetMs) {
    if (i < 0 || i >= kReconnectRetries)
        return 0;
    const int64_t d = kReconnectBackoffMs[i];
#ifdef MUT_HTTP_RANGE_RETRY_NO_BUDGET
    (void)nowMs;
    (void)startedMs;
    (void)budgetMs;
    return d;
#else
    if (budgetMs <= 0 || startedMs <= 0)
        return d;
    const int64_t left = startedMs + budgetMs - nowMs;
    if (left <= 0)
        return 0;
    return left < d ? left : d;
#endif
}

inline bool startsHttp(const std::string& url) {
    return url.size() >= 7 && (url.compare(0, 7, "http://") == 0 || url.compare(0, 7, "HTTP://") == 0);
}

inline bool startsHttps(const std::string& url) {
    return url.size() >= 8 &&
           (url.compare(0, 8, "https://") == 0 || url.compare(0, 8, "HTTPS://") == 0);
}

// http:// only. plex.direct is rewritten to dotted IPv4 HTTP before libav
// opens it; https stays on libav's own TLS stack.
inline bool isHttpUrl(const std::string& url) { return startsHttp(url) && !startsHttps(url); }

inline bool isDirectPartUrl(const std::string& url) {
    return url.find("/library/parts/") != std::string::npos;
}

// Eligible: a PMS Part over plain HTTP. Not a live transcode, the lab ladder,
// YouTube, RAOP, or AirPlay remux.
inline bool urlEligible(const std::string& url) {
    if (!isHttpUrl(url) || !isDirectPartUrl(url))
        return false;
    if (url.find("/ladder.ts") != std::string::npos || url.find("/yt.ts") != std::string::npos)
        return false;
    if (url.find("googlevideo.com") != std::string::npos || url.find("/raop") != std::string::npos)
        return false;
    if (url.find("transcode/universal") != std::string::npos ||
        url.find("/video/:/transcode/") != std::string::npos)
        return false;
    return true;
}

inline std::string rangeBytesValue(int64_t offset) {
    if (offset < 0)
        offset = 0;
    return "bytes=" + std::to_string(offset) + "-";
}

inline std::string rangeHeaderLine(int64_t offset) { return "Range: " + rangeBytesValue(offset); }

inline bool shortOfContentLength(int64_t received, int64_t contentLength) {
    return contentLength > 0 && received >= 0 && received < contentLength;
}

// attempt is 1-based. First resume waits 100 ms; then 250, 500, 1000, 2000.
inline int64_t backoffMs(unsigned attempt) {
    static constexpr int64_t k[] = {0, 100, 250, 500, 1000, 2000};
    if (attempt == 0)
        return 0;
    if (attempt >= 5)
        return 2000;
    return k[attempt];
}

inline bool canResume(int64_t received, int64_t contentLength, unsigned resumesSoFar,
                      unsigned maxResumes = kMaxResumes) {
#ifdef MUT_HTTP_RANGE_NO_RESUME
    (void)received;
    (void)contentLength;
    (void)resumesSoFar;
    (void)maxResumes;
    return false;
#else
    return shortOfContentLength(received, contentLength) && resumesSoFar < maxResumes;
#endif
}

// Custom AVIO never runs libav's interrupt callback. The reader polls this
// on every wait so a stall abort or open deadline still fires.
inline bool deadlineHit(int64_t nowMs, int64_t deadlineMs) {
    return deadlineMs > 0 && nowMs >= deadlineMs;
}

// Armed resume/wait clock. budgetMs <= 0 means no wall cap. startedMs <= 0
// means the clock is not running yet.
inline bool resumeBudgetHit(int64_t nowMs, int64_t startedMs, int64_t budgetMs) {
#ifdef MUT_HTTP_RANGE_NO_BUDGET
    (void)nowMs;
    (void)startedMs;
    (void)budgetMs;
    return false;
#else
    return budgetMs > 0 && startedMs > 0 && nowMs >= startedMs + budgetMs;
#endif
}

inline int clampResumeWaitMs(int timeoutMs, int64_t nowMs, int64_t startedMs, int64_t budgetMs) {
#ifdef MUT_HTTP_RANGE_NO_BUDGET
    (void)nowMs;
    (void)startedMs;
    (void)budgetMs;
    return timeoutMs;
#else
    if (timeoutMs < 0)
        timeoutMs = 0;
    if (budgetMs <= 0 || startedMs <= 0)
        return timeoutMs;
    const int64_t left = startedMs + budgetMs - nowMs;
    if (left <= 0)
        return 0;
    if (left < timeoutMs)
        return static_cast<int>(left);
    return timeoutMs;
#endif
}

// HTTP_RANGE_RESUME / MPX_HTTP_RANGE_RESUME. Default on. 0/off/no/false
// falls back to libav HTTP (no custom AVIO).
inline int rangeResumeEnvTri(const char* e) {
    if (!e || !e[0])
        return -1;
    char buf[16];
    size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    if (std::strcmp(buf, "0") == 0 || std::strcmp(buf, "off") == 0 || std::strcmp(buf, "no") == 0 ||
        std::strcmp(buf, "false") == 0)
        return 0;
    if (std::strcmp(buf, "1") == 0 || std::strcmp(buf, "on") == 0 || std::strcmp(buf, "yes") == 0 ||
        std::strcmp(buf, "true") == 0)
        return 1;
    return -1;
}

inline bool rangeResumeEnabled() {
#ifdef MUT_HTTP_RANGE_IGNORE_OFF
    return true;
#else
    int v = rangeResumeEnvTri(std::getenv("MPX_HTTP_RANGE_RESUME"));
    if (v >= 0)
        return v == 1;
    v = rangeResumeEnvTri(std::getenv("HTTP_RANGE_RESUME"));
    if (v >= 0)
        return v == 1;
    return true;
#endif
}

inline bool transferIsChunked(const std::string& te) {
    std::string s = te;
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s.find("chunked") != std::string::npos;
}

// Content-Range: bytes <start>-<end>/<total|*>
inline bool parseContentRange(const std::string& v, int64_t* start, int64_t* end, int64_t* total) {
    if (!start || !end || !total)
        return false;
    *start = *end = *total = -1;
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t'))
        ++i;
    auto ci = [&](size_t p, const char* lit) {
        for (size_t n = 0; lit[n]; ++n) {
            if (p + n >= v.size())
                return false;
            char c = v[p + n];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != lit[n])
                return false;
        }
        return true;
    };
    if (!ci(i, "bytes"))
        return false;
    i += 5;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t'))
        ++i;
    if (i >= v.size() || v[i] == '*')
        return false;
    char* p = nullptr;
    const long long s = std::strtoll(v.c_str() + i, &p, 10);
    if (!p || p == v.c_str() + i || *p != '-')
        return false;
    *start = s;
    i = static_cast<size_t>(p - v.c_str()) + 1;
    if (i < v.size() && v[i] != '/') {
        const long long e = std::strtoll(v.c_str() + i, &p, 10);
        if (!p || p == v.c_str() + i)
            return false;
        *end = e;
        i = static_cast<size_t>(p - v.c_str());
    }
    if (i >= v.size() || v[i] != '/')
        return false;
    ++i;
    if (i < v.size() && v[i] == '*')
        return true;
    const long long t = std::strtoll(v.c_str() + i, &p, 10);
    if (!p || p == v.c_str() + i)
        return false;
    *total = t;
    return true;
}

struct ResponsePlan {
    bool ok = false;
    bool fatal = false;
    int64_t skip = 0;
    int64_t totalLength = -1;
    int64_t bodyLength = -1;
};

// 206: body starts at the requested offset. 200 after a Range request: the
// server ignored Range; skip the bytes already received. 401/403/404 stay
// fatal so a bad token does not spin. 416 at/past the end is a clean EOF.
inline ResponsePlan planResponse(int status, int64_t requestedOffset, int64_t contentLength,
                                 int64_t rangeStart, int64_t rangeTotal) {
    ResponsePlan p;
    if (requestedOffset < 0)
        requestedOffset = 0;
    if (status == 401 || status == 403 || status == 404)
        p.fatal = true;
    else if (status == 416) {
        if (rangeTotal > 0 && requestedOffset >= rangeTotal)
            p.ok = false;
        else
            p.fatal = true;
    } else if (status == 206) {
        const int64_t start = rangeStart >= 0 ? rangeStart : requestedOffset;
        if (start > requestedOffset)
            p.fatal = true;
        else {
            p.ok = true;
            p.skip = requestedOffset - start;
            p.totalLength = rangeTotal > 0 ? rangeTotal : -1;
            if (contentLength >= 0)
                p.bodyLength = contentLength;
            else if (p.totalLength > 0)
                p.bodyLength = p.totalLength - start;
        }
    } else if (status == 200) {
        p.ok = true;
        p.totalLength = contentLength;
        p.bodyLength = contentLength;
#ifdef MUT_HTTP_RANGE_NO_200_SKIP
        p.skip = 0;
#else
        p.skip = requestedOffset;
#endif
        if (contentLength >= 0 && requestedOffset > contentLength)
            p.fatal = true, p.ok = false;
    } else if (status >= 500 && status <= 599) {
        p.ok = false;
    } else {
        p.fatal = true;
    }
    return p;
}

inline std::string resumeLogLine(int64_t offset, unsigned attempt) {
    char b[96];
    std::snprintf(b, sizeof b, "http range resume offset=%lld attempt=%u",
                  static_cast<long long>(offset), attempt);
    return b;
}

// The log line must never carry a URL, header block, or token-shaped key.
inline bool logLooksSafe(const std::string& line) {
    auto has = [&](const char* s) { return line.find(s) != std::string::npos; };
    if (has("http://") || has("https://") || has("X-Plex-") || has("x-plex-"))
        return false;
    if (has("Token") || has("token=") || has("X-Plex-Token"))
        return false;
    return has("offset=") && has("attempt=");
}

// Filename hint for avformat_open_input when custom AVIO owns the bytes.
// Strips the query (tokens live there) and keeps the leaf for the probe.
inline std::string probeName(const std::string& url) {
    std::string path = url;
    const auto q = path.find('?');
    if (q != std::string::npos)
        path.resize(q);
    const auto slash = path.rfind('/');
    std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    return leaf.empty() ? std::string("direct.bin") : leaf;
}

}  // namespace httprange
}  // namespace misterplex
