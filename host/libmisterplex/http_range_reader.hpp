#pragma once
// HTTP/1.1 GET with Range resume for a PMS Part. Numeric hosts only: the ARM
// static binary must not call getaddrinfo. Tokens ride the request (URL query
// and caller headers) and never the log.

#include "http_range_resume.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace misterplex {
namespace httprange {

struct ParsedHttpUrl {
    bool ok = false;
    std::string host;
    int port = 80;
    std::string path;
    int family = 0;
    sockaddr_storage addr{};
    socklen_t addrlen = 0;
};

// http:// only, inet_pton host. A hostname leaves ok=false so the caller
// keeps libav's HTTP (and its DNS).
inline ParsedHttpUrl parseHttpUrl(const std::string& url) {
    ParsedHttpUrl out;
    if (!isHttpUrl(url))
        return out;
    std::string rest = url.substr(7);
    std::string hostport, path;
    if (!rest.empty() && rest[0] == '[') {
        const auto rb = rest.find(']');
        if (rb == std::string::npos)
            return out;
        hostport = rest.substr(0, rb + 1);
        if (rb + 1 < rest.size() && rest[rb + 1] == ':') {
            const auto slash = rest.find('/', rb + 1);
            hostport = rest.substr(0, slash == std::string::npos ? rest.size() : slash);
            path = slash == std::string::npos ? "/" : rest.substr(slash);
        } else if (rb + 1 < rest.size() && rest[rb + 1] == '/') {
            path = rest.substr(rb + 1);
        } else {
            path = "/";
        }
    } else {
        const auto slash = rest.find('/');
        hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
        path = slash == std::string::npos ? "/" : rest.substr(slash);
    }
    std::string host;
    int port = 80;
    if (!hostport.empty() && hostport[0] == '[') {
        const auto rb = hostport.find(']');
        if (rb == std::string::npos)
            return out;
        host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':')
            port = std::atoi(hostport.c_str() + rb + 2);
    } else {
        const auto colon = hostport.find(':');
        if (colon != std::string::npos && hostport.find(':', colon + 1) == std::string::npos) {
            host = hostport.substr(0, colon);
            port = std::atoi(hostport.c_str() + colon + 1);
        } else {
            host = hostport;
        }
    }
    if (host.empty() || port <= 0 || port > 65535)
        return out;
    auto* a4 = reinterpret_cast<sockaddr_in*>(&out.addr);
    auto* a6 = reinterpret_cast<sockaddr_in6*>(&out.addr);
    if (inet_pton(AF_INET, host.c_str(), &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons(static_cast<uint16_t>(port));
        out.family = AF_INET;
        out.addrlen = sizeof(*a4);
    } else if (inet_pton(AF_INET6, host.c_str(), &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(static_cast<uint16_t>(port));
        out.family = AF_INET6;
        out.addrlen = sizeof(*a6);
    } else {
        return out;
    }
    out.ok = true;
    out.host = host;
    out.port = port;
    out.path = path.empty() ? "/" : path;
    return out;
}

inline bool shouldAttach(const std::string& url) {
    return rangeResumeEnabled() && urlEligible(url) && parseHttpUrl(url).ok;
}

class HttpRangeReader {
public:
    std::function<void(int64_t offset, unsigned attempt)> onResume;
    std::function<void(const std::string&)> onLog;
    std::function<void(int64_t ms)> onSleep;
    // Polled on every wait (custom AVIO never runs fmt->interrupt_callback).
    // Fold abort_, onInterrupt(), and ioDeadline into this.
    std::function<bool()> cancelled;
    std::function<int64_t()> nowMs;
    int64_t deadlineMs = 0;  // 0 = none; stop() when now() >= deadlineMs
    int64_t resumeBudgetMs = kResumeBudgetMs;  // 0 = no wall cap
    int connectTimeoutMs = kConnectTimeoutMs;  // per attempt, cancel-polled
    int sendTimeoutMs = 15000;
    // #231: reopen connect retry. MPX_HTTP_RANGE_RESUME=0 turns it off.
    bool connectRetry = retryDefault();
    int readTimeoutMs = 20000;
    unsigned maxResumes = kMaxResumes;

    ~HttpRangeReader() { closeSock(); }

    bool open(const std::string& url, const std::string& extraHeaders) {
        parsed_ = parseHttpUrl(url);
        if (!parsed_.ok)
            return false;
        extraHeaders_ = extraHeaders;
        offset_ = 0;
        contentLength_ = -1;
        resumes_ = 0;
        resumeStartedMs_ = 0;
        fatal_ = false;
        eof_ = false;
        const bool ok = getWithRetry(0, false);
        resumeStartedMs_ = 0;
        return ok;
    }

    // Sequential read.  >0 bytes, 0 clean EOF, -1 error (still short after
    // the resume budget, or a fatal status).
    int read(uint8_t* dst, int n) {
        if (!dst || n <= 0)
            return 0;
        if (fatal_)
            return -1;
        int got = 0;
        while (got < n) {
            if (stop())
                return got > 0 ? got : -1;
            if (skipLeft_ > 0) {
                if (discard(skipLeft_) < 0)
                    return got > 0 ? got : resumeOrFail();
                continue;
            }
            if (bodyLeft_ == 0 && contentLength_ >= 0 && offset_ >= contentLength_) {
                eof_ = true;
                return got;
            }
            if (fd_ < 0 || (bodyLeft_ == 0 && knownBody_ && !chunked_)) {
                const int r = resumeOrFail();
                if (r <= 0)
                    return got > 0 ? got : r;
                continue;
            }
            uint8_t tmp[4096];
            int want = n - got;
            if (knownBody_ && bodyLeft_ >= 0 && bodyLeft_ < want)
                want = static_cast<int>(bodyLeft_);
            if (want > static_cast<int>(sizeof tmp))
                want = static_cast<int>(sizeof tmp);
            if (want <= 0) {
                const int r = resumeOrFail();
                if (r <= 0)
                    return got > 0 ? got : r;
                continue;
            }
            const int r = recvSome(tmp, want);
            if (r < 0)
                return got > 0 ? got : (stop() ? -1 : resumeOrFail());
            if (r == 0) {
                if (stop())
                    return got > 0 ? got : -1;
                const int s = resumeOrFail();
                if (s <= 0)
                    return got > 0 ? got : s;
                continue;
            }
            std::memcpy(dst + got, tmp, static_cast<size_t>(r));
            got += r;
            offset_ += r;
            if (knownBody_ && bodyLeft_ > 0)
                bodyLeft_ -= r;
            resumes_ = 0;  // progress: a long title may drop more than once
            resumeStartedMs_ = 0;
        }
        return got;
    }

    bool seek(int64_t pos) {
        if (pos < 0)
            return false;
        if (pos == offset_ && fd_ >= 0)
            return true;
        skipLeft_ = 0;
        pending_.clear();
        closeSock();
        offset_ = pos;
        resumes_ = 0;
        resumeStartedMs_ = 0;
        eof_ = false;
        fatal_ = false;
        const bool ok = getWithRetry(pos, false);
        resumeStartedMs_ = 0;
        return ok;
    }

    int64_t tell() const { return offset_; }
    int64_t size() const { return contentLength_; }
    unsigned resumes() const { return resumes_; }
    bool fatal() const { return fatal_; }
    // Interrupt / ioDeadline only. A resume-budget expiry is EIO, not EXIT.
    bool aborted() const {
        if (cancelled && cancelled())
            return true;
        return deadlineHit(now(), deadlineMs);
    }

private:
    ParsedHttpUrl parsed_{};
    std::string extraHeaders_;
    int fd_ = -1;
    int64_t offset_ = 0;
    int64_t contentLength_ = -1;
    int64_t bodyLeft_ = -1;
    int64_t skipLeft_ = 0;
    bool knownBody_ = false;
    bool chunked_ = false;
    bool fatal_ = false;
    bool eof_ = false;
    unsigned resumes_ = 0;
    int64_t resumeStartedMs_ = 0;
    std::string pending_;

    int64_t now() const {
        if (nowMs)
            return nowMs();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void armResumeBudget() {
        if (resumeBudgetMs <= 0 || resumeStartedMs_ > 0)
            return;
        resumeStartedMs_ = now();
    }

    bool stop() const {
        if (cancelled && cancelled())
            return true;
        if (deadlineHit(now(), deadlineMs))
            return true;
        return resumeBudgetHit(now(), resumeStartedMs_, resumeBudgetMs);
    }

    void closeSock() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        knownBody_ = false;
        bodyLeft_ = -1;
        chunked_ = false;
        pending_.clear();
    }

    void logResume(int64_t off, unsigned attempt) {
        const std::string line = resumeLogLine(off, attempt);
        if (onLog)
            onLog(line);
        else
            std::fprintf(stderr, "misterplexd: %s\n", line.c_str());
        if (onResume)
            onResume(off, attempt);
    }

    void sleepMs(int64_t ms) {
        if (ms <= 0)
            return;
        if (onSleep) {
            onSleep(ms);
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until && !stop())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    int resumeOrFail() {
        armResumeBudget();
        for (;;) {
            closeSock();
            if (eof_ || fatal_ || stop())
                return (fatal_ || stop()) ? -1 : 0;
            if (!canResume(offset_, contentLength_, resumes_, maxResumes)) {
                if (shortOfContentLength(offset_, contentLength_))
                    return -1;
                eof_ = true;
                return 0;
            }
            const unsigned attempt = resumes_ + 1;
            ++resumes_;
            sleepMs(backoffMs(attempt));
            if (stop())
                return -1;
            if (getWithRetry(offset_, true)) {
                logResume(offset_, attempt);
                return 1;
            }
            if (fatal_)
                return -1;
        }
    }

    static bool retryDefault() {
#ifdef MUT_HTTP_RANGE_RETRY_IGNORE_OFF
        return true;
#else
        return rangeResumeEnabled();
#endif
    }

    // issueGet + #231 reconnect retry for transport failures (connect, send,
    // head). Status errors set fatal_ and return at once. The whole window
    // (connect waits + sleeps) is held to resumeBudgetMs via stop()/clamps.
    bool getWithRetry(int64_t off, bool isResume) {
        if (!connectRetry)
            return issueGet(off, isResume);
        armResumeBudget();
        if (issueGet(off, true))
            return true;
#ifndef MUT_HTTP_RANGE_NO_CONNECT_RETRY
        for (int i = 0; i < kReconnectRetries && !fatal_ && !eof_; ++i) {
            if (stop())
                break;
            const int64_t d = reconnectDelayMs(i, now(), resumeStartedMs_, resumeBudgetMs);
            if (d <= 0)
                break;
            const std::string line = "http range reopen retry offset=" + std::to_string(off) + " attempt=" +
                                     std::to_string(i + 1) + "/" + std::to_string(kReconnectRetries) +
                                     " in=" + std::to_string(d) + "ms";
            if (onLog)
                onLog(line);
            else
                std::fprintf(stderr, "misterplexd: %s\n", line.c_str());
            sleepMs(d);
            if (stop())
                break;
            if (issueGet(off, true))
                return true;
        }
#endif
        if (!isResume)
            fatal_ = true;
        return false;
    }

    bool issueGet(int64_t rangeStart, bool isResume) {
        closeSock();
        if (!connectFd()) {
            if (isResume)
                return false;
            fatal_ = true;
            return false;
        }
        std::string req = "GET " + parsed_.path + " HTTP/1.1\r\n";
        req += "Host: " + hostHeader() + "\r\n";
        req += "User-Agent: MisterPlex\r\n";
        req += "Accept: */*\r\n";
#ifndef MUT_HTTP_RANGE_NO_RANGE
        if (rangeStart > 0)
            req += rangeHeaderLine(rangeStart) + "\r\n";
#endif
        req += filteredExtraHeaders();
        req += "Connection: close\r\n\r\n";
        if (!sendAll(req.data(), req.size())) {
            closeSock();
            return false;
        }
        std::string head;
        if (!readHead(head)) {
            closeSock();
            return false;
        }
        int status = 0;
        int64_t clen = -1, cStart = -1, cEnd = -1, cTotal = -1;
        bool chunked = false;
        if (!parseHead(head, &status, &clen, &cStart, &cEnd, &cTotal, &chunked)) {
            closeSock();
            fatal_ = true;
            return false;
        }
        if (chunked) {
            closeSock();
            fatal_ = true;
            return false;
        }
        const ResponsePlan plan = planResponse(status, rangeStart, clen, cStart, cTotal);
        if (plan.fatal) {
            closeSock();
            fatal_ = true;
            return false;
        }
        if (contentLength_ > 0 && plan.totalLength > 0 && plan.totalLength != contentLength_) {
            closeSock();
            fatal_ = true;
            return false;
        }
        if (!plan.ok) {
            closeSock();
            if (status == 416) {
                eof_ = true;
                return true;
            }
            return false;
        }
        if (plan.totalLength > 0)
            contentLength_ = plan.totalLength;
        skipLeft_ = plan.skip;
        chunked_ = chunked;
        if (chunked_) {
            knownBody_ = false;
            bodyLeft_ = -1;
        } else if (plan.bodyLength >= 0) {
            knownBody_ = true;
            bodyLeft_ = plan.bodyLength;
        } else {
            knownBody_ = false;
            bodyLeft_ = -1;
        }
        return true;
    }

    std::string hostHeader() const {
        if (parsed_.family == AF_INET6)
            return "[" + parsed_.host + "]:" + std::to_string(parsed_.port);
        if (parsed_.port == 80)
            return parsed_.host;
        return parsed_.host + ":" + std::to_string(parsed_.port);
    }

    std::string filteredExtraHeaders() const {
        std::string out;
        size_t i = 0;
        const std::string& h = extraHeaders_;
        while (i < h.size()) {
            auto e = h.find('\n', i);
            std::string line = h.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? h.size() : e + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = line.substr(0, colon);
            for (char& c : key)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            if (key == "host" || key == "range" || key == "connection")
                continue;
            out += line;
            out += "\r\n";
        }
        return out;
    }

    bool connectFd() {
        fd_ = ::socket(parsed_.family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0)
            return false;
        const int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&parsed_.addr), parsed_.addrlen);
        if (rc == 0)
            return true;
        if (errno != EINPROGRESS) {
            closeSock();
            return false;
        }
        pollfd p{fd_, POLLOUT, 0};
#ifdef MUT_HTTP_RANGE_CONNECT_NO_CANCEL
        const bool connected = ::poll(&p, 1, connectTimeoutMs) > 0;
#else
        const bool connected = waitPoll(&p, connectTimeoutMs);
#endif
        if (!connected || !(p.revents & POLLOUT)) {
            closeSock();
            return false;
        }
        int err = 0;
        socklen_t el = sizeof err;
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
            closeSock();
            return false;
        }
        return true;
    }

    bool waitPoll(pollfd* p, int timeoutMs) {
        if (contentLength_ > 0 || offset_ > 0 || resumes_ > 0)
            armResumeBudget();
        timeoutMs = clampResumeWaitMs(timeoutMs, now(), resumeStartedMs_, resumeBudgetMs);
        if (timeoutMs <= 0)
            return false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (stop())
                return false;
            int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now())
                    .count());
            if (left <= 0)
                return false;
            if (left > 100)
                left = 100;
            const int r = ::poll(p, 1, left);
            if (r < 0 && errno == EINTR)
                continue;
            if (r > 0)
                return true;
        }
    }

    bool sendAll(const char* p, size_t n) {
        size_t off = 0;
        while (off < n) {
            pollfd pl{fd_, POLLOUT, 0};
            if (!waitPoll(&pl, sendTimeoutMs))
                return false;
            const ssize_t w = ::send(fd_, p + off, n - off, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return false;
            }
            off += static_cast<size_t>(w);
        }
        return true;
    }

    int recvSome(uint8_t* dst, int n) {
        if (!pending_.empty()) {
            const int take = std::min(n, static_cast<int>(pending_.size()));
            std::memcpy(dst, pending_.data(), static_cast<size_t>(take));
            pending_.erase(0, static_cast<size_t>(take));
            return take;
        }
        if (fd_ < 0)
            return 0;
        for (;;) {
            pollfd p{fd_, POLLIN, 0};
            if (!waitPoll(&p, readTimeoutMs))
                return -1;
            const ssize_t r = ::recv(fd_, dst, static_cast<size_t>(n), 0);
            if (r < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return -1;
            }
            return static_cast<int>(r);
        }
    }

    int discard(int64_t& left) {
        uint8_t b[1024];
        while (left > 0) {
            int want = left > static_cast<int64_t>(sizeof b) ? static_cast<int>(sizeof b)
                                                             : static_cast<int>(left);
            const int r = recvSome(b, want);
            if (r < 0)
                return -1;
            if (r == 0) {
                closeSock();
                return -1;
            }
            left -= r;
            if (knownBody_ && bodyLeft_ > 0)
                bodyLeft_ -= r;
        }
        return 0;
    }

    bool readHead(std::string& head) {
        head.clear();
        uint8_t b[512];
        while (head.size() < 65536) {
            const int r = recvSome(b, sizeof b);
            if (r < 0)
                return false;
            if (r == 0)
                return false;
            head.append(reinterpret_cast<char*>(b), static_cast<size_t>(r));
            const auto pos = head.find("\r\n\r\n");
            if (pos != std::string::npos) {
                pending_ = head.substr(pos + 4);
                head.resize(pos);
                return true;
            }
        }
        return false;
    }

    static std::string headerVal(const std::string& head, const char* name) {
        const size_t nlen = std::strlen(name);
        size_t i = 0;
        while (i < head.size()) {
            auto e = head.find("\r\n", i);
            const std::string line = head.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? head.size() : e + 2;
            if (line.size() >= nlen) {
                bool match = true;
                for (size_t k = 0; k < nlen; ++k) {
                    char c = line[k];
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c - 'A' + 'a');
                    if (c != name[k]) {
                        match = false;
                        break;
                    }
                }
                if (match && line.size() > nlen && line[nlen] == ':') {
                    size_t v = nlen + 1;
                    while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                        ++v;
                    return line.substr(v);
                }
            }
        }
        return {};
    }

    static bool parseHead(const std::string& head, int* status, int64_t* clen, int64_t* cStart,
                          int64_t* cEnd, int64_t* cTotal, bool* chunked) {
        if (head.size() < 12 || (head.compare(0, 7, "HTTP/1.") != 0 && head.compare(0, 5, "HTTP/") != 0))
            return false;
        const auto sp = head.find(' ');
        if (sp == std::string::npos || sp + 4 > head.size())
            return false;
        *status = std::atoi(head.c_str() + sp + 1);
        const std::string cl = headerVal(head, "content-length");
        *clen = cl.empty() ? -1 : std::strtoll(cl.c_str(), nullptr, 10);
        const std::string cr = headerVal(head, "content-range");
        if (!cr.empty())
            parseContentRange(cr, cStart, cEnd, cTotal);
        const std::string te = headerVal(head, "transfer-encoding");
        *chunked = transferIsChunked(te);
        return *status >= 100 && *status <= 599;
    }
};

}  // namespace httprange
}  // namespace misterplex
