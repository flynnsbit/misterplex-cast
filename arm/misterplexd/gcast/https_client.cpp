// See https_client.hpp. Built for real only with MPX_HAVE_GCAST_TLS (OpenSSL linked).
#include "gcast/https_client.hpp"
#include "wall_clock.hpp"
#include "ca_bundle.hpp"
#include <cstdio>

namespace misterplex {
namespace gcast {

std::string caBundlePath() {
    return mpx::caBundlePath();
}

}  // namespace gcast
}  // namespace misterplex

#ifndef MPX_HAVE_GCAST_TLS
namespace misterplex {
namespace gcast {
HttpsClient::HttpsClient(const std::string&) { initErr_ = "built without TLS (MPX_HAVE_GCAST_TLS)"; }
HttpsClient::~HttpsClient() {}
HttpsResult HttpsClient::request(const std::string&, const std::string&, const std::string&, const std::string&,
                                 const std::string&, int, const std::function<bool(const char*, size_t)>&, size_t,
                                 const HttpsRequestFlags& flags) {
    HttpsResult r;
    if (flags.timeCheck != TlsTimeCheck::Skip) {
        std::string clockErr;
        if (misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &clockErr) ==
            misterplex::WallClockStatus::Unavailable) {
            r.err = clockErr;
            return r;
        }
    }
    r.err = initErr_;
    return r;
}
void HttpsClient::cancel() {}
}  // namespace gcast
}  // namespace misterplex
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace misterplex {
namespace gcast {
namespace {
std::string sslErr() {
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return e ? buf : "ssl error";
}
int connectTimeout(const std::string& host, int port, int timeoutSec, std::string& err) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int g = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (g != 0 || !res) { err = std::string("resolve ") + host + ": " + gai_strerror(g); return -1; }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            rc = ::poll(&p, 1, timeoutSec * 1000) == 1 ? 0 : -1;
            int so = 0; socklen_t sl = sizeof so;
            if (rc == 0 && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0)) rc = -1;
        }
        if (rc == 0) { fcntl(fd, F_SETFL, fl); break; }
        ::close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { err = "connect " + host + " failed"; return -1; }
    timeval tv{timeoutSec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}
}  // namespace

HttpsClient::HttpsClient(const std::string& caFile) {
    SSL_CTX* c = SSL_CTX_new(TLS_client_method());
    if (!c) { initErr_ = "SSL_CTX_new: " + sslErr(); return; }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
    if (caFile.empty() || SSL_CTX_load_verify_locations(c, caFile.c_str(), nullptr) != 1) {
        initErr_ = "no usable CA bundle (" + (caFile.empty() ? std::string("none found; set MPX_CA_BUNDLE") : caFile) + ")";
        SSL_CTX_free(c);
        return;
    }
    ctx_ = c;
}
HttpsClient::~HttpsClient() { if (ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_)); }

void HttpsClient::cancel() {
    cancelled_.store(true);
    std::lock_guard<std::mutex> lk(fdMu_);
    if (liveFd_ >= 0) ::shutdown(liveFd_, SHUT_RDWR);
}

HttpsResult HttpsClient::request(const std::string& method, const std::string& host, const std::string& pathQuery,
                                 const std::string& contentType, const std::string& body, int timeoutSec,
                                 const std::function<bool(const char*, size_t)>& onBody, size_t maxBody,
                                 const HttpsRequestFlags& flags) {
    HttpsResult r;
    // Skip does not call the wall-clock rescue. That rescue's HTTPS Date path is a
    // separate TLS exchange; waiting here would deadlock on a clock that is not valid yet.
    if (flags.timeCheck != TlsTimeCheck::Skip) {
        std::string clockErr;
        if (misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &clockErr) ==
            misterplex::WallClockStatus::Unavailable) {
            r.err = clockErr;
            return r;
        }
    }
    if (!ctx_) { r.err = initErr_; return r; }
    cancelled_.store(false);
    const int port = flags.port > 0 ? flags.port : 443;
    const std::string& tcpHost = flags.connectHost.empty() ? host : flags.connectHost;
    int fd = connectTimeout(tcpHost, port, timeoutSec, r.err);
    if (fd < 0) return r;
    { std::lock_guard<std::mutex> lk(fdMu_); liveFd_ = fd; }
    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx_));
    auto finish = [&]() {
        { std::lock_guard<std::mutex> lk(fdMu_); liveFd_ = -1; }
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        ::close(fd);
    };
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_set1_host(ssl, host.c_str()) != 1) {
        r.err = "SSL_set1_host failed for " + host;
        std::fprintf(stderr, "misterplexd: %s\n", r.err.c_str());
        finish();
        return r;
    }
    if (flags.timeCheck == TlsTimeCheck::Skip) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_NO_CHECK_TIME);
    }
    if (SSL_connect(ssl) != 1) {
        const long vr = SSL_get_verify_result(ssl);
        r.err = "TLS handshake with " + host + ": " + sslErr();
        if (vr != X509_V_OK)
            r.err += std::string(" (") + X509_verify_cert_error_string(vr) + ")";
        finish();
        return r;
    }
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        r.err = std::string("certificate verify failed for ") + host + ": " +
                X509_verify_cert_error_string(SSL_get_verify_result(ssl));
        finish();
        return r;
    }
    if (flags.idleSec > 0) {
        timeval idle{flags.idleSec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
    }
    std::string req = method + " " + pathQuery + " HTTP/1.1\r\nHost: " + host +
                      "\r\nUser-Agent: misterplexd-gcast/1\r\nAccept: */*\r\nConnection: close\r\n";
    if (method == "POST") {
        req += "Content-Type: " + (contentType.empty() ? std::string("application/x-www-form-urlencoded") : contentType) + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;
    for (size_t off = 0; off < req.size();) {
        int w = SSL_write(ssl, req.data() + off, int(req.size() - off));
        if (w <= 0) { r.err = "write: " + sslErr(); finish(); return r; }
        off += size_t(w);
    }
    std::string headBuf, dec;
    bool inBody = false, chunked = false;
    long long clen = -1, got = 0;
    http::ChunkedDecoder cd;
    char buf[8192];
    auto deliver = [&](const char* p, size_t n) -> bool {
        if (onBody) return onBody(p, n);
        if (r.body.size() + n > maxBody) { r.err = "body over cap"; return false; }
        r.body.append(p, n);
        return true;
    };
    for (;;) {
        if (cancelled_.load()) { r.err = "cancelled"; break; }
        int n = SSL_read(ssl, buf, sizeof buf);
        if (n <= 0) {
            const int se = SSL_get_error(ssl, n);
            if ((se == SSL_ERROR_WANT_READ || se == SSL_ERROR_SYSCALL || se == SSL_ERROR_WANT_WRITE) &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
                r.err = "idle";
                break;
            }
            if (!(inBody && !chunked && clen < 0) && r.err.empty()) r.err = "read ended";
            break;
        }
        const char* p = buf; size_t len = size_t(n);
        if (!inBody) {
            headBuf.append(p, len);
            size_t end = headBuf.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (headBuf.size() > http::kMaxHead) { r.err = "response head too large"; break; }
                continue;
            }
            if (!http::parseRespHead(headBuf.substr(0, end), r.head)) { r.err = "bad response head"; break; }
            r.ok = true; r.status = r.head.status; r.reason = r.head.reason;
            chunked = http::lc(r.head.get("transfer-encoding")).find("chunked") != std::string::npos;
            std::string cl = r.head.get("content-length");
            if (!chunked && !cl.empty()) clen = std::atoll(cl.c_str());
            inBody = true;
            std::string rest = headBuf.substr(end + 4);
            headBuf.clear();
            p = nullptr; len = 0;
            if (!rest.empty()) { std::memcpy(buf, rest.data(), rest.size()); p = buf; len = rest.size(); }
        }
        if (len == 0) { if (clen == 0) break; continue; }
        if (chunked) {
            dec.clear();
            if (!cd.feed(p, len, dec)) { r.err = "bad chunked framing"; break; }
            if (!dec.empty() && !deliver(dec.data(), dec.size())) break;
            if (cd.done()) break;
        } else {
            if (!deliver(p, len)) break;
            got += (long long)len;
            if (clen >= 0 && got >= clen) break;
        }
    }
    if (r.ok && r.err == "read ended" && ((chunked && cd.done()) || (clen >= 0 && got >= clen))) r.err.clear();
    finish();
    return r;
}

}  // namespace gcast
}  // namespace misterplex
#endif
