#pragma once

#include "plexui_data.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace misterplex {
namespace plexui {

constexpr const char* kPlexUiCaBundleEnv = "MPX_CA_BUNDLE";
// Explicit opt-in. plex.tv PIN exchange verifies TLS unless this is 1 or true.
constexpr const char* kPlexUiInsecureTlsEnv = "MPX_PLEXUI_INSECURE_TLS";
// A play-queue document for a long season stays well under this. Larger bodies are dropped.
constexpr std::size_t kPlexUiHttpBodyCap = 2u * 1024u * 1024u;

inline bool appendCapped(std::string& out, const char* data, std::size_t n, std::size_t cap) {
    if (n > cap || out.size() > cap - n)
        return false;
    if (n > 0)
        out.append(data, n);
    return true;
}

// No -L: curl would forward the X-Plex-Token header across a redirect.
inline std::vector<std::string> plexuiCurlPrefix(const char* method) {
#ifdef MUT_PLEXUI_NO_Q   // test-only mutant (#89): -q dropped
    return {"curl", "-sS", "-g", "--http1.1", "-X", method};
#else
    return {"curl", "-q", "-sS", "-g", "--http1.1", "-X", method};   // -q first: no ~/.curlrc (#89)
#endif
}
inline std::vector<std::string> plexuiPostCurlPrefix() {
    return plexuiCurlPrefix("POST");
}

// Query PUT /playQueues/{id}. -q first, same as POST (#89). The token stays in a header.
inline std::vector<std::string> plexuiPutCurlPrefix() {
    return plexuiCurlPrefix("PUT");
}

// A non-empty body is sent. PUT with an empty body is not. POST with an empty body
// is sent only when a content type is named (plex.tv pins). POST /playQueues passes
// an empty content type so curl does not attach a form body.
inline bool plexuiHttpSendsBody(bool put, const std::string& contentType, const std::string& body) {
    if (!body.empty())
        return true;
    if (put)
        return false;
    return !contentType.empty();
}

// Readable path from MPX_CA_BUNDLE only. plex.tv verification uses mpx::caBundlePath().
std::string plexUiCaBundlePath();

std::string httpUrlHost(const std::string& url);

struct PlexUiTlsPlan {
    bool failClosed = false;
    bool insecure = false;
    std::string ca;
};

// How a plexui GET should call curl. plex.tv does not follow redirects.
struct PlexUiPreparedGet {
    bool skip = false;
    bool tlsNoCa = false;
    bool followRedirects = true;
    bool insecure = true;
    std::string ca;
};

inline PlexUiPreparedGet plexUiPrepareGet(bool plexHost, const PlexUiTlsPlan& plan) {
    PlexUiPreparedGet g;
    if (!plexHost) {
        if (!plan.ca.empty() && !plan.insecure && !plan.failClosed) {
            g.insecure = false;
            g.ca = plan.ca;
        }
        return g;
    }
    g.followRedirects = false;
    if (plan.failClosed) {
        g.skip = true;
        g.tlsNoCa = true;
        g.insecure = false;
        return g;
    }
    g.insecure = plan.insecure;
    if (!plan.insecure)
        g.ca = plan.ca;
    return g;
}

// plex.tv verifies with the bundle (MPX_CA_BUNDLE) or the system CA, and fail-closes
// when both are empty. PMS GET and POST verify when the bundle is set, and stay
// unverified otherwise.
PlexUiTlsPlan plexUiTlsPlanFrom(bool plexHost, bool insecureOptIn, const std::string& bundle,
                                const std::string& systemCa);
PlexUiTlsPlan plexUiTlsPlan(const std::string& url);

// Split a curl -D - body plus a trailing "\n%{http_code}" line.
struct CurlDump {
    int status = 0;
    int retryAfterSec = -1;
    std::string body;
};
CurlDump parseCurlDump(const std::string& raw);

HttpGetFn makePlexuiHttpGet();
HttpGetFn makePlexuiImageHttpGet();
HttpPostFn makePlexuiHttpPost();
// Same no-shell curl path with -X PUT. Headers carry the token. An empty body is not a form body.
HttpPostFn makePlexuiHttpPut();
HttpHeaderGetFn makePlexuiHeaderGet();

using ProbeLogFn = std::function<void(const std::string&)>;
void setPlexuiProbeLog(ProbeLogFn log);
void plexuiProbeLogLine(const std::string& line);

} // namespace plexui
} // namespace misterplex
