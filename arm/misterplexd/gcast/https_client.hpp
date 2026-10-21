// Minimal blocking HTTPS/1.1 client for the YouTube Lounge API (GCAST P3).
// OpenSSL (static libssl/libcrypto in the gcast build), peer + hostname verify
// against a shipped CA bundle so the MiSTer needs nothing at runtime:
//   MPX_CA_BUNDLE env > /media/fat/linux/misterplex-cacert.pem >
//   /etc/ssl/certs/ca-certificates.crt > /etc/ssl/certs/cacert.pem > /etc/ssl/cert.pem
// One request per connection (Connection: close). cancel() from another thread
// shuts the socket down so a long-poll returns promptly on stop.
#pragma once
#include "gcast/http_util.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace misterplex {
namespace gcast {

std::string caBundlePath();  // mpx::caBundlePath(); "" if none

struct HttpsResult {
    bool ok = false;            // transport + TLS + response head parsed
    int status = 0;
    std::string reason, body, err;
    http::RespHead head;
};

// Enforce: wall-clock gate, then full certificate checks including notBefore/notAfter.
// Skip: do not consult the wall-clock gate (it may itself be doing TLS) and set
// X509_V_FLAG_NO_CHECK_TIME on this request only. Chain and hostname stay on.
// Used by generate_screen_id, which has no request timestamp and no token expiry.
enum class TlsTimeCheck { Enforce, Skip };

struct HttpsRequestFlags {
    TlsTimeCheck timeCheck = TlsTimeCheck::Enforce;
    int port = 443;
    std::string connectHost;  // empty: resolve `host`. Set: TCP goes here, TLS name stays `host`.
    // 0 keeps SO_RCVTIMEO at timeoutSec. A positive value is the idle gap between body
    // reads (a stalled long-poll), not the connect deadline.
    int idleSec = 0;
};

class HttpsClient {
public:
    explicit HttpsClient(const std::string& caFile);
    ~HttpsClient();
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;
    bool ready() const { return ctx_ != nullptr; }
    const std::string& initError() const { return initErr_; }
    // onBody (optional) gets decoded body bytes as they arrive; return false to stop.
    // Without onBody the body is collected into result.body (capped at maxBody).
    HttpsResult request(const std::string& method, const std::string& host, const std::string& pathQuery,
                        const std::string& contentType, const std::string& body, int timeoutSec,
                        const std::function<bool(const char*, size_t)>& onBody = {}, size_t maxBody = 2u << 20,
                        const HttpsRequestFlags& flags = {});
    void cancel();  // aborts the in-flight request on this client (if any)
private:
    void* ctx_ = nullptr;  // SSL_CTX*
    std::string initErr_;
    std::mutex fdMu_;
    int liveFd_ = -1;
    std::atomic<bool> cancelled_{false};
};

}  // namespace gcast
}  // namespace misterplex
