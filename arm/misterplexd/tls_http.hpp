#pragma once
// One HTTPS GET used for Plex when the daemon is built with OpenSSL
// (MPX_HAVE_GCAST_TLS, the glass arm-plexd recipe). Chain and hostname stay
// on. The clock rescue runs first when the wall clock is implausible; if it
// is still implausible, only the certificate time check is skipped.

#include "wall_clock.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {

struct TlsVerifiedRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutSec = 15;
    size_t maxBody = 2u << 20;
    // Empty caFile / extraTrust: tlsCaBundlePath() and tlsTrustFile().
    std::string caFile;
    std::string extraTrust;
    // Empty: waitForWallClockRescue. Tests pass a stub so a 1970 case does
    // not query the network or step the real clock.
    std::function<WallClockStatus(std::string*)> rescue;
    // A redirect is followed, but X-Plex-Token is not sent on the next hop.
    // Callers that must not leave the original host set this false.
    bool followRedirects = true;
    // If set and returns true, the GET aborts with err "cancelled".
    std::function<bool()> cancelled;
};

struct TlsVerifiedResult {
    bool transportOk = false;
    int status = 0;
    std::string body;
    std::string err;
    bool skippedTime = false;
    bool optedOut = false;
    std::string warning;
    bool cancelled = false;
};

TlsVerifiedResult tlsVerifiedGet(const TlsVerifiedRequest& req);

} // namespace misterplex
