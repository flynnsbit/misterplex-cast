#include "tls_http.hpp"
#include "libmisterplex/tls_policy.hpp"
#include "libmisterplex/curl_argv_token.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef MPX_HAVE_GCAST_TLS
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace misterplex {

#ifndef MPX_HAVE_GCAST_TLS

TlsVerifiedResult tlsVerifiedGet(const TlsVerifiedRequest&) {
    TlsVerifiedResult r;
    r.err = "built without TLS";
    return r;
}

#else

namespace {

std::string sslErrText() {
    const unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return e ? buf : "ssl error";
}

bool ipv4Literal(const std::string& host, in_addr& out) {
    return inet_pton(AF_INET, host.c_str(), &out) == 1;
}

int connectNumeric(const std::string& ip, int port, int timeoutMs, std::string& err) {
    in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        err = "bad address " + ip;
        return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = "socket";
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
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

int connectHost(const std::string& host, int port, int timeoutMs, std::string& err) {
    in_addr lit{};
    if (ipv4Literal(host, lit))
        return connectNumeric(host, port, timeoutMs, err);
    const std::string portS = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int g = ::getaddrinfo(host.c_str(), portS.c_str(), &hints, &res);
    if (g != 0 || !res) {
        err = std::string("resolve ") + host + ": " + gai_strerror(g);
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0)
            continue;
        const int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            rc = ::poll(&p, 1, timeoutMs) == 1 ? 0 : -1;
            int so = 0;
            socklen_t sl = sizeof so;
            if (rc == 0 && (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0))
                rc = -1;
        }
        if (rc == 0) {
            ::fcntl(fd, F_SETFL, fl);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0)
        err = "connect " + host + " failed";
    return fd;
}

std::string headerValue(const std::string& head, const char* name) {
    std::string low = head;
    for (char& c : low)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    const std::string key = std::string("\r\n") + name + ":";
    auto at = low.find(key);
    size_t pos = std::string::npos;
    if (low.compare(0, key.size() - 2, key.substr(2)) == 0)
        pos = 0;
    else if (at != std::string::npos)
        pos = at + 2;
    if (pos == std::string::npos)
        return {};
    auto eol = head.find("\r\n", pos);
    if (eol == std::string::npos)
        eol = head.size();
    std::string line = head.substr(pos, eol - pos);
    const auto colon = line.find(':');
    if (colon == std::string::npos)
        return {};
    std::string v = line.substr(colon + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.erase(v.begin());
    return v;
}

std::string resolveRedirect(const std::string& current, const std::string& location) {
    if (location.empty())
        return {};
    if (location.find("://") != std::string::npos)
        return location;
    TlsUrl u;
    if (!parseTlsUrl(current, u))
        return {};
    const std::string scheme = u.https ? "https://" : "http://";
    const std::string port = ((u.https && u.port == 443) || (!u.https && u.port == 80))
                                 ? std::string{}
                                 : ":" + std::to_string(u.port);
    if (location[0] == '/')
        return scheme + u.host + port + location;
    const auto slash = u.pathQuery.rfind('/');
    const std::string dir = slash == std::string::npos ? "/" : u.pathQuery.substr(0, slash + 1);
    return scheme + u.host + port + dir + location;
}

bool decodeChunked(const std::string& raw, std::string& out, size_t cap) {
    size_t i = 0;
    while (i < raw.size()) {
        const auto eol = raw.find("\r\n", i);
        if (eol == std::string::npos)
            return false;
        char* end = nullptr;
        const unsigned long n = std::strtoul(raw.c_str() + i, &end, 16);
        if (end == raw.c_str() + i)
            return false;
        i = eol + 2;
        if (n == 0)
            return true;
        if (i + n > raw.size())
            return false;
        if (out.size() + n > cap)
            return false;
        out.append(raw, i, n);
        i += n;
        if (i + 2 > raw.size() || raw.compare(i, 2, "\r\n") != 0)
            return false;
        i += 2;
    }
    return false;
}

bool isPlexTokenHeader(const std::string& name) {
    static const char k[] = "x-plex-token";
    if (name.size() != sizeof(k) - 1)
        return false;
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        if (static_cast<char>(c) != k[i])
            return false;
    }
    return true;
}

// Later hops of one GET must not carry the Plex token. curl -L would, and so
// would replaying req.headers onto the Location target.
std::vector<std::pair<std::string, std::string>>
headersWithoutPlexToken(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& h : headers) {
        if (!isPlexTokenHeader(h.first))
            out.push_back(h);
    }
    return out;
}

struct Hop {
    TlsVerifiedResult result;
    std::string location;
};

bool tlsReqCancelled(const TlsVerifiedRequest& req) { return req.cancelled && req.cancelled(); }

Hop oneHop(const TlsUrl& u, const TlsVerifiedRequest& req,
           const std::vector<std::pair<std::string, std::string>>& headers, bool optOut, bool skipTime,
           int timeoutMs) {
    Hop hop;
    TlsVerifiedResult& r = hop.result;
    if (tlsReqCancelled(req)) {
        r.err = "cancelled";
        r.cancelled = true;
        return hop;
    }
    const std::string ca = !req.caFile.empty() ? req.caFile : tlsCaBundlePath();
    const std::string extra = !req.extraTrust.empty() ? req.extraTrust : tlsTrustFile();
    if (!optOut && ca.empty() && extra.empty()) {
        r.err = "no usable CA bundle (ship misterplex-cacert.pem or set MPX_CA_BUNDLE)";
        return hop;
    }
    std::string err;
    const std::string tcp = u.connectHost.empty() ? u.host : u.connectHost;
    const int fd = connectHost(tcp, u.port, timeoutMs, err);
    if (fd < 0) {
        r.err = err;
        return hop;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ::close(fd);
        r.err = "SSL_CTX_new";
        return hop;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // 0 is the opt-out (no chain, no hostname). The default is SSL_VERIFY_PEER.
    SSL_CTX_set_verify(ctx, optOut ? 0 : SSL_VERIFY_PEER, nullptr);
    if (!optOut) {
        if (!ca.empty() && SSL_CTX_load_verify_locations(ctx, ca.c_str(), nullptr) != 1) {
            SSL_CTX_free(ctx);
            ::close(fd);
            r.err = "CA bundle rejected";
            return hop;
        }
        if (!extra.empty() && SSL_CTX_load_verify_locations(ctx, extra.c_str(), nullptr) != 1) {
            SSL_CTX_free(ctx);
            ::close(fd);
            r.err = "TLS_TRUST_FILE rejected";
            return hop;
        }
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    in_addr lit{};
    const bool ipHost = ipv4Literal(u.host, lit);
    if (!ipHost)
        SSL_set_tlsext_host_name(ssl, u.host.c_str());
    if (!optOut) {
        if (ipHost)
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), u.host.c_str());
        else
            SSL_set1_host(ssl, u.host.c_str());
        if (skipTime)
            X509_VERIFY_PARAM_set_flags(SSL_get0_param(ssl), X509_V_FLAG_NO_CHECK_TIME);
    }
    if (tlsReqCancelled(req)) {
        r.err = "cancelled";
        r.cancelled = true;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return hop;
    }
    if (SSL_connect(ssl) != 1) {
        const long vr = SSL_get_verify_result(ssl);
        r.err = "TLS handshake with " + u.host + ": " + sslErrText();
        if (!optOut && vr != X509_V_OK)
            r.err += std::string(" (") + X509_verify_cert_error_string(vr) + ")";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return hop;
    }
    if (!optOut && SSL_get_verify_result(ssl) != X509_V_OK) {
        r.err = std::string("certificate verify failed for ") + u.host + ": " +
                X509_verify_cert_error_string(SSL_get_verify_result(ssl));
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return hop;
    }
    std::string msg = "GET " + u.pathQuery + " HTTP/1.1\r\nHost: " + u.host + "\r\nConnection: close\r\n";
    for (const auto& h : headers) {
        if (h.first.empty() || h.first.find('\r') != std::string::npos || h.first.find('\n') != std::string::npos)
            continue;
        if (h.second.find('\r') != std::string::npos || h.second.find('\n') != std::string::npos)
            continue;
        msg += h.first + ": " + h.second + "\r\n";
    }
    msg += "\r\n";
    size_t off = 0;
    while (off < msg.size()) {
        const int w = SSL_write(ssl, msg.data() + off, static_cast<int>(msg.size() - off));
        if (w <= 0) {
            r.err = "https write failed";
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(fd);
            return hop;
        }
        off += static_cast<size_t>(w);
    }
    if (tlsReqCancelled(req)) {
        timeval slice{};
        slice.tv_sec = 0;
        slice.tv_usec = 200000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof slice);
    }
    std::string raw;
    char buf[4096];
    const auto t0 = std::chrono::steady_clock::now();
    while (raw.size() < req.maxBody + 65536) {
        if (tlsReqCancelled(req)) {
            r.err = "cancelled";
            r.cancelled = true;
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(fd);
            return hop;
        }
        if (req.cancelled) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
            if (elapsed >= timeoutMs) {
                r.err = "https read timeout";
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                ::close(fd);
                return hop;
            }
        }
        const int n = SSL_read(ssl, buf, sizeof buf);
        if (n <= 0)
            break;
        raw.append(buf, static_cast<size_t>(n));
        const auto end = raw.find("\r\n\r\n");
        if (end != std::string::npos) {
            const std::string head = raw.substr(0, end);
            const std::string cl = headerValue(head, "content-length");
            if (!cl.empty()) {
                const size_t need = end + 4 + static_cast<size_t>(std::strtoul(cl.c_str(), nullptr, 10));
                if (raw.size() >= need)
                    break;
            }
        }
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string::npos) {
        r.err = "https: no response head";
        return hop;
    }
    const std::string head = raw.substr(0, end);
    if (head.size() < 12 || head.compare(0, 5, "HTTP/") != 0) {
        r.err = "https: bad status line";
        return hop;
    }
    r.status = std::atoi(head.c_str() + 9);
    std::string body = raw.substr(end + 4);
    std::string enc = headerValue(head, "transfer-encoding");
    for (char& c : enc)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (enc.find("chunked") != std::string::npos) {
        std::string decoded;
        if (!decodeChunked(body, decoded, req.maxBody)) {
            r.err = "https: bad chunked body";
            return hop;
        }
        body.swap(decoded);
    } else if (body.size() > req.maxBody) {
        body.resize(req.maxBody);
    }
    r.body.swap(body);
    r.transportOk = r.status > 0;
    hop.location = headerValue(head, "location");
    r.skippedTime = skipTime;
    r.optedOut = optOut;
    return hop;
}

} // namespace

TlsVerifiedResult tlsVerifiedGet(const TlsVerifiedRequest& req) {
    TlsVerifiedResult r;
    TlsUrl first;
    if (!parseTlsUrl(req.url, first) || !first.https) {
        r.err = "tlsVerifiedGet: not an https url";
        return r;
    }
    const bool optOut = tlsVerifyOff();
    if (optOut) {
        logTlsOptOutOnce();
        r.warning = kTlsOptOutWarning;
        r.optedOut = true;
    }
    bool skipTime = false;
    if (!optOut && !wallTimePlausible(std::time(nullptr))) {
        std::string clockErr;
        if (req.rescue)
            (void)req.rescue(&clockErr);
        else
            (void)waitForWallClockRescue(kWallClockRescueWaitMs, &clockErr);
        skipTime = !wallTimePlausible(std::time(nullptr));
    }
    // #340: a URL query token never goes on the wire; it becomes the X-Plex-Token header.
    std::string url = req.url;
    std::vector<std::pair<std::string, std::string>> firstHeaders = req.headers;
#ifndef MUT_340_TLS_QUERY_TOKEN   // test-only mutant: the query token stays in the request line
    mpx::moveUrlTokenToHeaders(url, firstHeaders);
#endif
    std::vector<std::pair<std::string, std::string>> hopHeaders = firstHeaders;
    for (int hop = 0; hop < 5; ++hop) {
        if (req.cancelled && req.cancelled()) {
            r.err = "cancelled";
            r.cancelled = true;
            r.transportOk = false;
            return r;
        }
        TlsUrl u;
        if (!parseTlsUrl(url, u) || !u.https) {
            r.err = "redirect left https";
            r.transportOk = false;
            return r;
        }
        Hop got = oneHop(u, req, hopHeaders, optOut, skipTime,
                         req.timeoutSec > 0 ? req.timeoutSec * 1000 : 15000);
        const int st = got.result.status;
        const bool redir = st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
        if (!redir || got.location.empty() || !req.followRedirects) {
            got.result.warning = r.warning;
            got.result.optedOut = optOut;
            got.result.skippedTime = skipTime && !optOut;
            return got.result;
        }
        std::string next = resolveRedirect(url, got.location);
        if (next.empty())
            return got.result;
        mpx::curlStripTokenQuery(next);   // a Location's own token query is not sent
        // Later hops never carry the token (stricter than same-origin), including one
        // that came from the URL query (#340).
        hopHeaders = headersWithoutPlexToken(firstHeaders);
        url = next;
    }
    r.err = "too many redirects";
    return r;
}

#endif

} // namespace misterplex
