#pragma once
// Playbar no-proxy commit d: the Access-Control-Expose-Headers the companion sends on every response.
// A browser client (Plex Web) reads a cross-origin response header only if it is listed here. PHT/PMS expose
// X-Plex-Client-Identifier; we add the session identifier and protocol headers so a client that relays or
// reads them directly sees them. Caveat (glass, 10-23): a PMS relay rewrites this header on its own response
// ("Location, Date"), so this only helps clients that talk to the daemon directly: a hedge, not the fix.
namespace misterplex {
constexpr const char* kCorsExposeHeaders =
    "X-Plex-Client-Identifier, X-Plex-Session-Identifier, X-Plex-Protocol, Content-Type, Content-Length, Date";
} // namespace misterplex
