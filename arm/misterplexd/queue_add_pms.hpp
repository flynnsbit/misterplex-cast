#pragma once
// The real PMS transport for queue_add_http.hpp: no shell (posix_spawn curl through plexui_http), the token only as an
// X-Plex-Token header (never in a URL, log or OSD text), no redirects, short timeouts, no retry. The base URL and token
// are read per call from the daemon's current session (the caller's snapshot function).
#include "libmisterplex/plexui/plexui_http.hpp"
#include "queue_add_http.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {
namespace qadd {

struct PmsTarget {
    std::string base;   // http(s)://host:port, no trailing slash needed
    std::string token;  // PMS access token
};

inline Transport makePmsTransport(std::function<PmsTarget()> snapshot, plexui::HttpHeaderGetFn get,
                                  plexui::HttpPostFn put, std::string clientId) {
    return [snapshot, get, put, clientId](const std::string& method, const std::string& path) -> Reply {
        Reply none;
        const PmsTarget t = snapshot ? snapshot() : PmsTarget{};
#ifndef MUT_QPMS_NO_SCHEME_CHECK
        if (t.base.rfind("http://", 0) != 0 && t.base.rfind("https://", 0) != 0)
            return none;
#endif
        if (t.token.empty() || path.empty() || path[0] != '/')
            return none;
#ifdef MUT_QPMS_TOKEN_IN_URL
        const std::string tokenQuery = "&X-Plex-Token=" + t.token;
#else
        const std::string tokenQuery;
#endif
        std::string base = t.base;
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        const std::vector<std::pair<std::string, std::string>> headers = {
            {"X-Plex-Token", t.token},
            {"X-Plex-Client-Identifier", clientId.empty() ? "misterplex" : clientId},
            {"X-Plex-Product", "MiSTerPlex"},
            {"Accept", "application/xml"},
        };
        const auto never = [] { return false; };
        BrowseHttpResult r;
        if (method == "GET" && get)
            r = get(base + path + tokenQuery, headers, 2000, 5000, never);
        else if (method == "PUT" && put)
            r = put(base + path + tokenQuery, "application/x-www-form-urlencoded", std::string(), headers, 2000, 5000, never);
        else
            return none;
        none.http = r.status;
        none.body = std::move(r.body);
        return none;
    };
}

}  // namespace qadd
}  // namespace misterplex
