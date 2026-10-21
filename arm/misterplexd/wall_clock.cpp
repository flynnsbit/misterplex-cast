#include "wall_clock.hpp"
#include "ca_bundle.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include <linux/rtc.h>

#ifdef MPX_HAVE_GCAST_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace misterplex {
namespace {

// Caller-facing poll budget. getaddrinfo is waited on for the same budget;
// a resolver that never returns does not hold the YouTube path.
int shortTimeoutMs(int timeoutMs) {
    if (timeoutMs <= 0)
        return 800;
    if (timeoutMs > 2000)
        return 2000;
    return timeoutMs;
}

struct DnsWait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool abandon = false;
    int rc = EAI_FAIL;
    addrinfo* res = nullptr;
};

addrinfo* lookupHost(const char* host, const char* service, int socktype, int timeoutMs) {
    auto dns = std::make_shared<DnsWait>();
    const std::string hostCopy = host ? host : "";
    const std::string serviceCopy = service ? service : "";
    std::thread([dns, hostCopy, serviceCopy, socktype] {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = socktype;
        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(hostCopy.c_str(), serviceCopy.c_str(), &hints, &res);
        std::lock_guard<std::mutex> g(dns->mu);
        if (dns->abandon) {
            if (res)
                ::freeaddrinfo(res);
        } else {
            dns->rc = rc;
            dns->res = res;
        }
        dns->done = true;
        dns->cv.notify_one();
    }).detach();
    std::unique_lock<std::mutex> lk(dns->mu);
    if (!dns->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return dns->done; })) {
        dns->abandon = true;
        return nullptr;
    }
    if (dns->rc != 0 || !dns->res)
        return nullptr;
    addrinfo* res = dns->res;
    dns->res = nullptr;
    return res;
}

void fillOrigin(unsigned char origin[8]) {
    static std::atomic<uint32_t> seq{1};
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t nonce = (ticks << 16) ^ seq.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i)
        origin[i] = static_cast<unsigned char>(nonce >> (8 * (7 - i)));
    if (origin[0] == 0 && origin[1] == 0 && origin[2] == 0 && origin[3] == 0 && origin[4] == 0 &&
        origin[5] == 0 && origin[6] == 0 && origin[7] == 0)
        origin[7] = 1;
}

std::optional<std::time_t> sntpQuery(const char* host, int timeoutMs) {
    timeoutMs = shortTimeoutMs(timeoutMs);
    addrinfo* res = lookupHost(host, "123", SOCK_DGRAM, timeoutMs);
    if (!res)
        return std::nullopt;
    const int fd = ::socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return std::nullopt;
    }
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    unsigned char origin[8];
    fillOrigin(origin);
    unsigned char pkt[48] = {};
    pkt[0] = 0x1b; // LI=0 VN=3 mode=3 (client)
    std::memcpy(pkt + 40, origin, 8);
    if (::sendto(fd, pkt, sizeof pkt, 0, res->ai_addr, res->ai_addrlen) != static_cast<ssize_t>(sizeof pkt)) {
        ::close(fd);
        ::freeaddrinfo(res);
        return std::nullopt;
    }
    ::freeaddrinfo(res);
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) <= 0) {
        ::close(fd);
        return std::nullopt;
    }
    unsigned char rsp[48];
    const ssize_t n = ::recv(fd, rsp, sizeof rsp, 0);
    ::close(fd);
    if (n < 48)
        return std::nullopt;
    return sntpAcceptReply(rsp, static_cast<std::size_t>(n), origin);
}

std::optional<std::time_t> sntpQueryAny(int timeoutMs) {
    const char* hosts[] = {"pool.ntp.org", "time.google.com"};
    const int each = shortTimeoutMs(timeoutMs);
    for (const char* h : hosts) {
        if (auto t = sntpQuery(h, each))
            return t;
    }
    return std::nullopt;
}

bool stepClock(std::time_t t) {
    timeval tv{};
    tv.tv_sec = t;
    return ::settimeofday(&tv, nullptr) == 0;
}

std::mutex gFailMu;
std::chrono::steady_clock::time_point gNextTry{};

std::time_t tmToUnix(const std::tm& in) {
    std::tm tm = in;
    tm.tm_isdst = 0;
    return timegm(&tm);
}

} // namespace

RtcSample readHardwareRtc(const char* path) {
    RtcSample out;
    if (!path || !*path)
        return out;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return out;
    out.present = true;
    rtc_time rt{};
    if (::ioctl(fd, RTC_RD_TIME, &rt) != 0) {
        ::close(fd);
        return out;
    }
    ::close(fd);
    std::tm tm{};
    tm.tm_sec = rt.tm_sec;
    tm.tm_min = rt.tm_min;
    tm.tm_hour = rt.tm_hour;
    tm.tm_mday = rt.tm_mday;
    tm.tm_mon = rt.tm_mon;
    tm.tm_year = rt.tm_year;
    const std::time_t t = tmToUnix(tm);
    // 1970 (time_t 0) and earlier are real readings. The caller rejects them
    // with wallTimePlausible and falls through to SNTP / HTTPS.
    if (t != static_cast<std::time_t>(-1))
        out.time = t;
    return out;
}

bool writeHardwareRtc(const char* path, std::time_t t) {
    if (!path || !*path || !wallTimePlausible(t))
        return false;
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::tm tm{};
    if (!gmtime_r(&t, &tm)) {
        ::close(fd);
        return false;
    }
    rtc_time rt{};
    rt.tm_sec = tm.tm_sec;
    rt.tm_min = tm.tm_min;
    rt.tm_hour = tm.tm_hour;
    rt.tm_mday = tm.tm_mday;
    rt.tm_mon = tm.tm_mon;
    rt.tm_year = tm.tm_year;
    rt.tm_wday = tm.tm_wday;
    rt.tm_yday = tm.tm_yday;
    rt.tm_isdst = 0;
    const int rc = ::ioctl(fd, RTC_SET_TIME, &rt);
    ::close(fd);
    return rc == 0;
}

namespace {

#ifdef MPX_HAVE_GCAST_TLS
std::string sslErrText() {
    const unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return e ? buf : "ssl error";
}

int connectNumeric(const std::string& ip, int port, int timeoutMs, std::string& err) {
    in_addr addr{};
    if (::inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        err = "connect host is not an IPv4 address";
        return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = "socket failed";
        return -1;
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    sa.sin_addr = addr;
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    if (rc != 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        rc = ::poll(&p, 1, timeoutMs) == 1 ? 0 : -1;
        int so = 0;
        socklen_t sl = sizeof so;
        if (rc == 0 && (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0))
            rc = -1;
    }
    if (rc != 0) {
        ::close(fd);
        err = "connect " + ip + " failed";
        return -1;
    }
    ::fcntl(fd, F_SETFL, fl);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}
#endif

} // namespace

TlsHttpResult tlsHttpExchange(const HttpsDateTarget& target, int timeoutMs) {
    TlsHttpResult r;
#ifndef MPX_HAVE_GCAST_TLS
    (void)target;
    (void)timeoutMs;
    r.err = "built without TLS";
    return r;
#else
    if (timeoutMs <= 0)
        timeoutMs = 2000;
    if (timeoutMs > 3000)
        timeoutMs = 3000;
    if (target.verifyHost.empty() || target.caFile.empty() || target.port <= 0) {
        r.err = "https date target incomplete";
        return r;
    }
    const std::string ip = target.connectHost.empty() ? std::string() : target.connectHost;
    std::string err;
    int fd = -1;
    if (!ip.empty()) {
        fd = connectNumeric(ip, target.port, timeoutMs, err);
    } else {
        const std::string port = std::to_string(target.port);
        addrinfo* tcp = lookupHost(target.verifyHost.c_str(), port.c_str(), SOCK_STREAM, timeoutMs);
        if (!tcp) {
            r.err = "resolve " + target.verifyHost + " failed";
            return r;
        }
        fd = ::socket(tcp->ai_family, tcp->ai_socktype | SOCK_CLOEXEC, tcp->ai_protocol);
        if (fd >= 0) {
            const int fl = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int rc = ::connect(fd, tcp->ai_addr, tcp->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                pollfd p{fd, POLLOUT, 0};
                rc = ::poll(&p, 1, timeoutMs) == 1 ? 0 : -1;
                int so = 0;
                socklen_t sl = sizeof so;
                if (rc == 0 && (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0))
                    rc = -1;
            }
            if (rc != 0) {
                ::close(fd);
                fd = -1;
                err = "connect " + target.verifyHost + " failed";
            } else {
                ::fcntl(fd, F_SETFL, fl);
                timeval tv{};
                tv.tv_sec = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            }
        }
        ::freeaddrinfo(tcp);
    }
    if (fd < 0) {
        r.err = err.empty() ? "connect failed" : err;
        return r;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ::close(fd);
        r.err = "SSL_CTX_new";
        return r;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_load_verify_locations(ctx, target.caFile.c_str(), nullptr) != 1) {
        SSL_CTX_free(ctx);
        ::close(fd);
        r.err = "CA bundle rejected";
        return r;
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, target.verifyHost.c_str());
    if (SSL_set1_host(ssl, target.verifyHost.c_str()) != 1) {
        r.err = "SSL_set1_host failed for " + target.verifyHost;
        std::fprintf(stderr, "misterplexd: %s\n", r.err.c_str());
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return r;
    }
    if (target.skipTimeCheck) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_NO_CHECK_TIME);
    }
    if (SSL_connect(ssl) != 1) {
        const long vr = SSL_get_verify_result(ssl);
        r.err = "TLS handshake with " + target.verifyHost + ": " + sslErrText();
        if (vr != X509_V_OK)
            r.err += std::string(" (") + X509_verify_cert_error_string(vr) + ")";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return r;
    }
    const long vr = SSL_get_verify_result(ssl);
    if (vr != X509_V_OK) {
        r.err = std::string("certificate verify failed for ") + target.verifyHost + ": " +
                X509_verify_cert_error_string(vr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return r;
    }
    const std::string req = "GET " + (target.path.empty() ? std::string("/") : target.path) +
                            " HTTP/1.1\r\nHost: " + target.verifyHost +
                            "\r\nUser-Agent: misterplexd-clock/1\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    if (SSL_write(ssl, req.data(), static_cast<int>(req.size())) != static_cast<int>(req.size())) {
        r.err = "https date write failed";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return r;
    }
    std::string raw;
    char buf[2048];
    while (raw.size() < 8192) {
        const int n = SSL_read(ssl, buf, sizeof buf);
        if (n <= 0)
            break;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.find("\r\n\r\n") != std::string::npos && raw.size() > 16)
            break;
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string::npos) {
        r.err = "https date: no response head";
        return r;
    }
    const std::string head = raw.substr(0, end);
    r.body = raw.substr(end + 4);
    if (head.size() < 12 || head.compare(0, 5, "HTTP/") != 0) {
        r.err = "https date: bad status line";
        return r;
    }
    r.status = std::atoi(head.c_str() + 9);
    std::string dateLine;
    std::string low = head;
    for (char& c : low)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    const auto at = low.find("\r\ndate:");
    const auto at0 = low.compare(0, 5, "date:") == 0 ? 0 : std::string::npos;
    const auto pos = at0 == 0 ? 0 : (at == std::string::npos ? std::string::npos : at + 2);
    if (pos != std::string::npos) {
        auto eol = head.find("\r\n", pos);
        if (eol == std::string::npos)
            eol = head.size();
        dateLine = head.substr(pos, eol - pos);
        const auto colon = dateLine.find(':');
        if (colon != std::string::npos) {
            dateLine = dateLine.substr(colon + 1);
            while (!dateLine.empty() && dateLine.front() == ' ')
                dateLine.erase(dateLine.begin());
        }
    }
    r.date = parseImfFixDate(dateLine);
    r.ok = r.status > 0;
    return r;
#endif
}

std::optional<std::time_t> httpsDateFromTargets(const HttpsDateTarget* targets, std::size_t n, int timeoutMs) {
    if (!targets || n == 0)
        return std::nullopt;
    std::optional<std::time_t> first, second;
    int got = 0;
    for (std::size_t i = 0; i < n && got < 2; ++i) {
        const TlsHttpResult r = tlsHttpExchange(targets[i], timeoutMs);
        if (!r.ok || !r.date)
            continue;
        if (got == 0)
            first = r.date;
        else
            second = r.date;
        ++got;
    }
    if (got == 0)
        return std::nullopt;
    if (got == 1)
        return acceptHttpsDatePair(first, std::nullopt);
    return acceptHttpsDatePair(first, second);
}

namespace {

std::optional<std::time_t> httpsDateQuery(int timeoutMs) {
    const std::string ca = mpx::caBundlePath();
    if (ca.empty())
        return std::nullopt;
    // Fixed list. One try each. The exchange skips only certificate time validity.
    HttpsDateTarget hosts[2];
    hosts[0].verifyHost = "www.google.com";
    hosts[0].caFile = ca;
    hosts[0].skipTimeCheck = true;
    hosts[1].verifyHost = "www.youtube.com";
    hosts[1].caFile = ca;
    hosts[1].skipTimeCheck = true;
    const int each = timeoutMs > 0 && timeoutMs < 3000 ? timeoutMs : 2000;
    return httpsDateFromTargets(hosts, 2, each);
}

const char* kRtcPath = "/dev/rtc0";

} // namespace

WallClockStatus ensurePlausibleWallClockDefault(std::string* err, bool force, WallClockSource* source) {
    if (wallTimePlausible(std::time(nullptr))) {
        if (source)
            *source = WallClockSource::None;
        return WallClockStatus::AlreadySane;
    }
    if (!force) {
        std::lock_guard<std::mutex> g(gFailMu);
        if (std::chrono::steady_clock::now() < gNextTry) {
            if (err)
                *err = "wall clock is implausible and NTP did not answer";
            return WallClockStatus::Unavailable;
        }
    }
    WallClockOps ops;
    ops.now = [] { return std::time(nullptr); };
    ops.readRtc = [] { return readHardwareRtc(kRtcPath); };
    ops.writeRtc = [](std::time_t t) { return writeHardwareRtc(kRtcPath, t); };
    ops.queryNtp = [](int ms) { return sntpQueryAny(ms); };
    ops.queryHttps = [](int ms) { return httpsDateQuery(ms); };
    ops.step = [](std::time_t t) { return stepClock(t); };
    const WallClockStatus st = ensurePlausibleWallClock(ops, err, source);
    if (st == WallClockStatus::Unavailable && !force) {
        std::lock_guard<std::mutex> g(gFailMu);
        gNextTry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    } else if (st == WallClockStatus::Unavailable && force) {
        std::lock_guard<std::mutex> g(gFailMu);
        gNextTry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    if (st == WallClockStatus::Stepped && source)
        logClockSteppedOnce(wallClockSourceName(*source));
    return st;
}

bool defaultRoutePresent() {
    FILE* f = std::fopen("/proc/net/route", "r");
    if (!f)
        return false;
    char line[256];
    if (!std::fgets(line, sizeof line, f)) {
        std::fclose(f);
        return false;
    }
    bool found = false;
    while (std::fgets(line, sizeof line, f)) {
        char ifc[32] = {};
        unsigned dest = 1, gateway = 0, flags = 0;
        if (std::sscanf(line, "%31s %x %x %x", ifc, &dest, &gateway, &flags) == 4) {
            (void)gateway;
            if (dest == 0 && (flags & 0x1u)) {
                found = true;
                break;
            }
        }
    }
    std::fclose(f);
    return found;
}

bool networkReadyForClock() {
    if (defaultRoutePresent())
        return true;
    addrinfo* res = lookupHost("pool.ntp.org", "123", SOCK_DGRAM, 800);
    if (!res)
        return false;
    ::freeaddrinfo(res);
    return true;
}

struct RescueState {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    bool done = false;
    WallClockStatus status = WallClockStatus::Unavailable;
    WallClockSource source = WallClockSource::None;
    std::string err = "wall clock is implausible; background rescue has not stepped it yet";
};

RescueState gRescue;

WallClockRescueThread& rescueThread() {
    static WallClockRescueThread thread;
    return thread;
}

bool rescueLogLine(const char* line) {
    if (!line)
        return false;
    const char* stepped = std::strstr(line, "stepped from ");
    if (stepped) {
        logClockSteppedOnce(stepped + std::strlen("stepped from "));
        return true;
    }
    const bool noisy = std::strstr(line, "attempt") || std::strstr(line, "did not answer") ||
                       std::strstr(line, "not ready") || std::strstr(line, "retry in");
    if (noisy && !offlineClockLogAllowed())
        return true;
    std::fprintf(stderr, "misterplexd: %s\n", line);
    return true;
}

void startWallClockRescue() {
    bool expected = false;
    if (!gRescue.started.compare_exchange_strong(expected, true))
        return;
    WallClockRescueOps ops;
    ops.now = [] { return std::time(nullptr); };
    ops.readRtc = [] { return readHardwareRtc(kRtcPath); };
    ops.writeRtc = [](std::time_t t) { return writeHardwareRtc(kRtcPath, t); };
    ops.networkReady = [] { return networkReadyForClock(); };
    ops.queryNtp = [](int ms) { return sntpQueryAny(ms); };
    ops.queryHttps = [](int ms) { return httpsDateQuery(ms); };
    ops.step = [](std::time_t t) { return stepClock(t); };
    ops.log = [](const char* line) { rescueLogLine(line); };
    rescueThread().onDone = [](const WallClockRescueResult& r) {
        {
            std::lock_guard<std::mutex> g(gRescue.mu);
            gRescue.status = r.status;
            gRescue.source = r.source;
            gRescue.done = true;
            if (r.status == WallClockStatus::Stepped || r.status == WallClockStatus::AlreadySane)
                gRescue.err.clear();
            else
                gRescue.err = "wall clock is implausible and NTP did not answer";
        }
        gRescue.cv.notify_all();
        std::fprintf(stderr, "misterplexd: wall clock rescue finished source=%s attempts=%d\n",
                     wallClockSourceName(r.source), r.attempts);
    };
    std::fprintf(stderr, "misterplexd: wall clock rescue thread started (rtc0, then SNTP, then HTTPS Date)\n");
    rescueThread().start(std::move(ops));
}

void stopWallClockRescue() {
    gRescue.stop.store(true);
    gRescue.cv.notify_all();
    rescueThread().stopAndJoin();
}

WallClockStatus waitForWallClockRescue(int waitMs, std::string* err, WallClockSource* source) {
    if (waitMs < 0)
        waitMs = 0;
    if (wallTimePlausible(std::time(nullptr))) {
        std::lock_guard<std::mutex> g(gRescue.mu);
        if (gRescue.done && gRescue.status == WallClockStatus::Stepped) {
            if (source)
                *source = gRescue.source;
            if (err)
                err->clear();
            return WallClockStatus::Stepped;
        }
        if (source)
            *source = WallClockSource::None;
        if (err)
            err->clear();
        return WallClockStatus::AlreadySane;
    }
    {
        std::unique_lock<std::mutex> lk(gRescue.mu);
        gRescue.cv.wait_for(lk, std::chrono::milliseconds(waitMs), [] {
            return gRescue.done || gRescue.stop.load() || wallTimePlausible(std::time(nullptr));
        });
    }
    if (wallTimePlausible(std::time(nullptr))) {
        std::lock_guard<std::mutex> g(gRescue.mu);
        if (source)
            *source = gRescue.source;
        if (err)
            err->clear();
        if (gRescue.status == WallClockStatus::Stepped)
            return WallClockStatus::Stepped;
        return WallClockStatus::AlreadySane;
    }
    if (err) {
        std::lock_guard<std::mutex> g(gRescue.mu);
        *err = gRescue.err.empty() ? "wall clock is implausible; background rescue has not stepped it yet"
                                   : gRescue.err;
    }
    if (source)
        *source = WallClockSource::None;
    return WallClockStatus::Unavailable;
}

} // namespace misterplex
