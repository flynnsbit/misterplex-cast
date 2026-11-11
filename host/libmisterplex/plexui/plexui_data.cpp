#include "../../../arm/misterplexd/log_redact.hpp"
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "plexui_data.hpp"

#include "plexui_api_cache.hpp"
#include "plexui_http.hpp"

#include "libmisterplex/browse_v2.hpp"
#include "libmisterplex/theme_music.hpp"
#include "libmisterplex/play_queue_write.hpp"
#include "libmisterplex/track_select.hpp"
#include "filter_4_3.hpp"
#include "plexui_audio.hpp"
#include "plexui_subtitles.hpp"

#include <cstdio>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <future>
#include <tuple>
#include <unordered_map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace misterplex {
namespace plexui {

namespace {

PlexUiApiCache g_apiCache;

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool isPlexDirectUri(const std::string& uri) {
    return uri.find(".plex.direct") != std::string::npos;
}

bool isHttpsConnection(const PlexConnection& c) {
    if (!c.uri.empty()) {
        const size_t p = c.uri.find("://");
        if (p != std::string::npos)
            return c.uri.compare(0, p, "https") == 0;
    }
    return c.protocol == "https";
}

int connectionRank(const PlexConnection& c, bool tlsBundle) {
    if (!tlsBundle) {
        if (c.relay)
            return 3;
        if (c.local)
            return 0;
        return 1;
    }
    if (c.relay)
        return 30;
    if (!c.uri.empty() && isPlexDirectUri(c.uri) && isHttpsConnection(c))
        return c.local ? 0 : 1;
    if (isHttpsConnection(c))
        return 10;
    if (!c.uri.empty() && c.uri.find("://") != std::string::npos) {
        const size_t p = c.uri.find("://");
        if (c.uri.compare(0, p, "http") == 0)
            return 40;
    }
    if (c.protocol == "http" || c.protocol.empty())
        return 40;
    return 20;
}

std::string connectionBase(const PlexConnection& c) {
    if (!c.uri.empty()) {
        const size_t path = c.uri.find('/', c.uri.find("://") + 3);
        return path == std::string::npos ? c.uri : c.uri.substr(0, path);
    }
    std::string p = c.protocol.empty() ? "http" : c.protocol;
    p += "://";
    p += c.address;
    if (!c.port.empty() && c.port != "80" && c.port != "443") {
        p += ':';
        p += c.port;
    }
    return p;
}

} // namespace

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string appendToken(std::string url, const std::string& token) {
    if (token.empty())
        return url;
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "X-Plex-Token=";
    url += urlEncode(token);
    return url;
}

std::string photoTranscodeUrl(const std::string& baseUrl, const std::string& thumbPath,
                              const std::string& token, int w, int h) {
    if (thumbPath.empty())
        return {};
    std::string url = baseUrl + "/photo/:/transcode?width=" + std::to_string(w) + "&height=" +
                      std::to_string(h) + "&minSize=1&upscale=1&url=" + urlEncode(thumbPath);
    return appendToken(url, token);
}

std::vector<PlexServer> parsePlexResources(const std::string& xml) {
    std::vector<PlexServer> servers;
    PlexServer* cur = nullptr;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name == "Device") {
            cur = nullptr;
            const std::string provides = el.get("provides");
            if (provides.find("server") == std::string::npos)
                continue;
            // plex.tv presence is "this PMS is connected to plex.tv", not "this
            // device can reach it". A shared server can be presence="0" and still
            // answer /identity on a public plex.direct URI (reviewer: LAN down,
            // WAN 200). Keep the row. resolveServerBases* probes every connection
            // and sets offline when nothing answers, so a dead host stays Offline.
            const std::string product = el.get("product");
            if (product != "Plex Media Server")
                continue;
            // reviewer: a Device with no accessToken is kept so it can still match a conf
            // seed (by id or any address) and use the conf token; unmatched ones are
            // dropped in mergeSeedsWithAccount().
#ifdef MUT_GWY_DROP_TOKENLESS
            if (el.get("accessToken").empty())
                continue;
#endif
            PlexServer s;
            s.name = el.get("name");
            s.sourceTitle = el.get("sourceTitle");
            s.clientIdentifier = el.get("clientIdentifier");
            s.machineIdentifier = el.get("machineIdentifier");
            if (s.machineIdentifier.empty())
                s.machineIdentifier = s.clientIdentifier;   // /api/resources: clientIdentifier only
            s.accessToken = el.get("accessToken");
            s.owned = el.get("owned") == "1";
            servers.push_back(std::move(s));
            cur = &servers.back();
            continue;
        }
        if (!cur)
            continue;
        if (el.name == "Connection") {
            PlexConnection c;
            c.protocol = el.get("protocol");
            c.address = el.get("address");
            c.port = el.get("port");
            c.local = el.get("local") == "1";
            c.relay = el.get("relay") == "1";
            c.uri = el.get("uri");
            cur->connections.push_back(std::move(c));
        }
    }
    return servers;
}

void pickServerConnections(std::vector<PlexServer>& servers) {
    for (auto& s : servers) {
        if (s.connections.empty())
            continue;
        const bool tlsBundle = !plexUiCaBundlePath().empty();
        std::stable_sort(s.connections.begin(), s.connections.end(),
                         [tlsBundle](const PlexConnection& a, const PlexConnection& b) {
                             return connectionRank(a, tlsBundle) < connectionRank(b, tlsBundle);
                         });
        s.baseUrl = connectionBase(s.connections.front());
    }
}

namespace {

std::string hostOfUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t host = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t slash = url.find('/', host);
    std::string hostport =
        slash == std::string::npos ? url.substr(host) : url.substr(host, slash - host);
    if (!hostport.empty() && hostport.front() == '[') {
        const size_t end = hostport.find(']');
        return end == std::string::npos ? hostport : hostport.substr(1, end - 1);
    }
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos)
        hostport.resize(colon);
    return hostport;
}

bool dottedPrivateLan(const std::string& h) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(h.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    if (a == 10)
        return true;
    if (a == 192 && b == 168)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    return false;
}

bool hostIsPrivateLan(const std::string& host) {
    if (dottedPrivateLan(host))
        return true;
    const size_t pd = host.find(".plex.direct");
    if (pd == std::string::npos || pd == 0)
        return false;
    std::string lab = host.substr(0, host.find('.'));
    for (char& ch : lab) {
        if (ch == '-')
            ch = '.';
    }
    return dottedPrivateLan(lab);
}

// GH#14: Native may still be Connecting... (baseUrl empty) while plex.tv
// connections already list a private LAN address. Home pick must not fall
// through to a WAN row.
bool serverHasPrivateLanReach(const PlexServer& s) {
    if (!s.baseUrl.empty() && hostIsPrivateLan(hostOfUrl(s.baseUrl)))
        return true;
    for (const auto& c : s.connections) {
        if (hostIsPrivateLan(c.address))
            return true;
        if (!c.uri.empty() && hostIsPrivateLan(hostOfUrl(c.uri)))
            return true;
    }
    return false;
}

} // namespace

std::string serverLinkLabel(const PlexServer& server) {
    if (server.offline)
        return "Offline";
    if (server.baseUrl.empty())
        return "Connecting...";
    const PlexConnection* match = nullptr;
    for (const auto& c : server.connections) {
        const std::string base = connectionBase(c);
        if (!base.empty() && base == server.baseUrl) {
            match = &c;
            break;
        }
    }
    if (!match) {
        const std::string host = hostOfUrl(server.baseUrl);
        for (const auto& c : server.connections) {
            if (!host.empty() && c.address == host) {
                match = &c;
                break;
            }
        }
    }
    if (!match)
        return "remote";
    if (match->relay)
        return "relay";
    if (match->local)
        return "local";
    return "remote";
}

namespace {

constexpr int kProbeConnectMs = 1500;
constexpr int kProbeTotalMs = 4000;

void pushUniqueBase(std::vector<std::string>& out, const std::string& base) {
    if (base.empty())
        return;
    for (const auto& u : out) {
        if (u == base)
            return;
    }
    out.push_back(base);
}

std::string httpAddressPort(const PlexConnection& c) {
    if (c.address.empty())
        return {};
    std::string p = "http://" + c.address;
    const std::string port = c.port.empty() ? "32400" : c.port;
    if (port != "80")
        p += ':' + port;
    return p;
}

std::vector<std::string> connectionProbeCandidates(const PlexConnection& c, bool tlsVerify) {
    std::vector<std::string> out;
    if (!c.uri.empty())
        pushUniqueBase(out, connectionBase(c));
    if (!tlsVerify) {
        pushUniqueBase(out, connectionBase(c));
        if (!c.local && !c.relay)
            pushUniqueBase(out, httpAddressPort(c));
        return out;
    }
    if (isHttpsConnection(c) && !c.relay)
        pushUniqueBase(out, connectionBase(c));
    if (c.relay && !c.uri.empty())
        pushUniqueBase(out, connectionBase(c));
    if (!c.local && !c.relay)
        pushUniqueBase(out, httpAddressPort(c));
    return out;
}

// The /identity machineIdentifier, "" when the body has none.
std::string identityMachineId(const std::string& body) {
    const std::string k = "machineIdentifier=\"";
    const size_t a = body.find(k);
    if (a == std::string::npos)
        return {};
    const size_t b = body.find('"', a + k.size());
    return b == std::string::npos ? std::string() : body.substr(a + k.size(), b - a - k.size());
}

// expectId: the server id from plex.tv resources (machineIdentifier, else clientIdentifier). A reachable address that answers for
// ANOTHER server (a Docker bridge 203.0.113.1, a reused LAN IP) is not this server: "studio" (the  lab PMS
// 1.43.4) was taken via 203.0.113.1 on  lab. An identity without a machineIdentifier is accepted as before.
// Synthetic local: ids are not expectId — that rejected the same PMS at 203.0.113.10:32400.
bool probeIdentity(HttpGetFn get, const std::string& baseUrl, const std::string& token,
                    const BrowseCancelFn& cancel, const std::string& serverName,
                    const std::string& expectId = std::string(), std::string* gotOut = nullptr,
                    bool strictId = false) {
    if (baseUrl.empty())
        return false;
    // #328: /identity is unauthenticated; the token is never sent before the id is known.
    (void)token;
    const std::string url = baseUrl + "/identity";
    const BrowseHttpResult r = get(url, kProbeConnectMs, kProbeTotalMs, cancel);
    const bool reached = !r.cancelled && r.ok();
    const std::string gotId = reached ? identityMachineId(r.body) : std::string();
    if (gotOut)
        *gotOut = gotId;
#ifdef MUT_reviewer_TOKEN_ON_ADDR
    strictId = false;
#endif
    const bool ok = reached && (strictId ? (!gotId.empty() && gotId == expectId)
                                         : (expectId.empty() || gotId.empty() || gotId == expectId));
    char buf[256];
    if (reached && !ok) {
        std::snprintf(buf, sizeof(buf), "plexui: probe %s base=%s fail identity mismatch (another server)",
                      serverName.empty() ? "?" : serverName.c_str(), misterplex::logUrl(baseUrl).c_str());
        plexuiProbeLogLine(buf);
        return false;
    }
    if (ok)
        std::snprintf(buf, sizeof(buf), "plexui: probe %s base=%s ok",
                      serverName.empty() ? "?" : serverName.c_str(), misterplex::logUrl(baseUrl).c_str());
    else
        std::snprintf(buf, sizeof(buf), "plexui: probe %s base=%s fail http=%d curl=%d",
                      serverName.empty() ? "?" : serverName.c_str(), misterplex::logUrl(baseUrl).c_str(), r.status,
                      r.curlExit);
    plexuiProbeLogLine(buf);
    return ok;
}

// OB port 0011: all candidate bases of one server at once. The winner is the best-ranked success (the
// order of connectionRank, plex.direct https first); a lower-ranked success waits at most kTieGraceMs for
// better-ranked probes still running, so a dead address never delays a live one by more than that.
constexpr int kTieGraceMs = 250;
std::string probeServerBaseParallel(HttpGetFn get, PlexServer s, const BrowseCancelFn& cancel, bool tlsVerify,
                                    const std::function<void(const std::string&)>& onWinner,
                                    std::string* outMachineId = nullptr) {
    std::stable_sort(s.connections.begin(), s.connections.end(),
                     [tlsVerify](const PlexConnection& a, const PlexConnection& b) {
                         return connectionRank(a, tlsVerify) < connectionRank(b, tlsVerify);
                     });
    std::vector<std::string> bases;
    for (const auto& c : s.connections)
        for (const std::string& b : connectionProbeCandidates(c, tlsVerify))
            pushUniqueBase(bases, b);
    if (bases.empty())
        return {};
#if !defined(MUT_PLEXUI_NO_PLEXDIRECT_FIRST)
    std::stable_partition(bases.begin(), bases.end(), [](const std::string& b) {   // ties: plex.direct https first
        return b.rfind("https://", 0) == 0 && b.find(".plex.direct") != std::string::npos;
    });
#endif
    const bool strict = !s.confTokenId.empty();
    const std::string expect = strict ? s.confTokenId : identityExpectId(s);
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> res(bases.size(), -1);   // -1 running, 0 fail, 1 ok
    std::vector<std::string> gotIds(bases.size());
    std::vector<std::thread> th;
    for (size_t i = 0; i < bases.size(); ++i)
        th.emplace_back([&, i] {
            const bool ok =
                probeIdentity(get, bases[i], s.accessToken, cancel, s.name, expect, &gotIds[i], strict);
            std::lock_guard<std::mutex> lk(m);
            res[i] = ok ? 1 : 0;
            cv.notify_all();
        });
    std::string win;
    {
        std::unique_lock<std::mutex> lk(m);
        auto firstOk = std::chrono::steady_clock::time_point{};
        for (;;) {
            int best = -1;
            bool betterRunning = false, anyRunning = false;
            for (size_t i = 0; i < res.size(); ++i) {
                anyRunning |= res[i] < 0;
                if (res[i] == 1) { best = static_cast<int>(i); break; }
                betterRunning |= res[i] < 0;
            }
            const auto now = std::chrono::steady_clock::now();
            if (best >= 0 && firstOk == std::chrono::steady_clock::time_point{})
                firstOk = now;
            if (best >= 0 && (!betterRunning || now - firstOk >= std::chrono::milliseconds(kTieGraceMs))) {
                win = bases[static_cast<size_t>(best)];
                break;
            }
            if (best < 0 && !anyRunning)
                break;
            if (cancel && cancel())
                break;
            cv.wait_for(lk, std::chrono::milliseconds(best >= 0 ? 20 : 50));
        }
    }
    if (!win.empty() && outMachineId) {
        for (size_t i = 0; i < bases.size(); ++i) {
            if (bases[i] == win) {
                *outMachineId = gotIds[i];
                break;
            }
        }
    }
    if (onWinner)
        onWinner(win);   // before the slower probes end
    for (auto& t : th)
        t.join();
    return win;
}

std::unordered_map<std::string, std::string>& serverBaseCache() {
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}

} // namespace

std::string probeServerBase(HttpGetFn get, PlexServer s, const BrowseCancelFn& cancel,
                            bool tlsVerify, std::string* outMachineId = nullptr) {
    if (s.connections.empty())
        return {};
    s.baseUrl.clear();
    std::stable_sort(s.connections.begin(), s.connections.end(),
                     [tlsVerify](const PlexConnection& a, const PlexConnection& b) {
                         return connectionRank(a, tlsVerify) < connectionRank(b, tlsVerify);
                     });
    for (const auto& c : s.connections) {
        if (cancel && cancel())
            return {};
        for (const std::string& base : connectionProbeCandidates(c, tlsVerify)) {
            std::string got;
            const bool strict = !s.confTokenId.empty();
            if (probeIdentity(get, base, s.accessToken, cancel, s.name,
                              strict ? s.confTokenId : identityExpectId(s), &got, strict)) {
                if (outMachineId)
                    *outMachineId = got;
                return base;
            }
        }
    }
    return {};
}

void resolveServerBases(HttpGetFn get, std::vector<PlexServer>& servers,
                        const BrowseCancelFn& cancel) {
    auto& cache = serverBaseCache();
    const bool tlsVerify = !plexUiCaBundlePath().empty();
    std::vector<std::future<std::tuple<std::size_t, std::string, std::string>>> jobs;
    for (size_t i = 0; i < servers.size(); ++i) {
        if (servers[i].connections.empty())
            continue;
        if (!servers[i].machineIdentifier.empty() &&
            !isSyntheticLocalServerId(servers[i].machineIdentifier)) {
            const auto it = cache.find(servers[i].machineIdentifier);
            if (it != cache.end()) {
                servers[i].baseUrl = it->second;
                continue;
            }
        }
        servers[i].baseUrl.clear();
        PlexServer copy = servers[i];
        jobs.push_back(std::async(std::launch::async, [get, copy, cancel, tlsVerify, i] {
            std::string got;
            const std::string base = probeServerBase(get, std::move(copy), cancel, tlsVerify, &got);
            return std::make_tuple(i, base, got);
        }));
    }
    for (auto& j : jobs) {
        const auto pr = j.get();
        if (cancel && cancel())
            return;
        PlexServer& s = servers[std::get<0>(pr)];
        if (!std::get<1>(pr).empty()) {
            s.baseUrl = std::get<1>(pr);
            s.offline = false;
            if (!std::get<2>(pr).empty() &&
                (s.machineIdentifier.empty() || isSyntheticLocalServerId(s.machineIdentifier)))
                s.machineIdentifier = std::get<2>(pr);
            if (!s.machineIdentifier.empty())
                cache[s.machineIdentifier] = std::get<1>(pr);
        } else {
            s.offline = true;
        }
    }
    collapseServersByIdentity(servers);
}

std::vector<PlexSection> parseLibrarySections(const std::string& xml) {
    std::vector<PlexSection> out;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name != "Directory")
            continue;
        if (el.get("type") == "folder")
            continue;
        PlexSection sec;
        sec.key = el.get("key");
        sec.title = el.get("title");
        sec.type = el.get("type");
        if (!sec.key.empty())
            out.push_back(std::move(sec));
    }
    return out;
}

static PlexItem itemFromElement(const XmlElement& el) {
    PlexItem it;
    it.ratingKey = el.get("ratingKey");
    it.key = el.get("key");
    it.title = el.get("title");
    it.thumb = el.get("thumb");
    it.art = el.get("art");
    it.theme = el.get("theme");
    it.updatedAt = el.get("updatedAt");
    it.summary = el.get("summary");
    it.year = el.get("year");
    it.contentRating = el.get("contentRating");
    it.type = el.get("type");
    int64_t ms = 0;
    if (parseMs(el.get("duration"), ms))
        it.runtimeMs = static_cast<int>(ms);
    if (parseMs(el.get("viewOffset"), ms))
        it.viewOffsetMs = ms;
    auto fieldInt = [](const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); };
    it.index = fieldInt(el.get("index"));
    it.parentIndex = fieldInt(el.get("parentIndex"));
    it.leafCount = fieldInt(el.get("leafCount"));
    it.viewCount = fieldInt(el.get("viewCount"));
    it.parentTitle = el.get("parentTitle");
    it.grandparentTitle = el.get("grandparentTitle");
    it.grandparentRatingKey = el.get("grandparentRatingKey");
    const std::string rating = el.get("rating");
    if (!rating.empty())
        it.rating = static_cast<float>(std::strtod(rating.c_str(), nullptr));
    it.tagline = el.get("tagline");
    const std::string aud = el.get("audienceRating");
    if (!aud.empty())
        it.audienceRating = static_cast<float>(std::strtod(aud.c_str(), nullptr));
    storeItemAspect(it, el.get("aspectRatio"));
    return it;
}

static std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool looksLikeJson(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return i < s.size() && s[i] == '{';
}

static void skipJsonWs(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
}

static bool parseJsonString(const std::string& s, size_t& i, std::string& out) {
    skipJsonWs(s, i);
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"')
            return true;
        if (c == '\\' && i < s.size()) {
            const char e = s[i++];
            if (e == 'n')
                out += '\n';
            else if (e == 't')
                out += '\t';
            else if (e == 'r')
                out += '\r';
            else if (e == 'u') {
                if (i + 4 <= s.size())
                    i += 4;
            } else {
                out += e;
            }
        } else {
            out += c;
        }
    }
    return false;
}

static bool parseJsonObject(const std::string& s, size_t& i,
                            std::vector<std::pair<std::string, std::string>>& fields,
                            std::vector<std::pair<std::string, size_t>>& nested);

static bool skipJsonValue(const std::string& s, size_t& i) {
    skipJsonWs(s, i);
    if (i >= s.size())
        return false;
    if (s[i] == '"') {
        std::string tmp;
        return parseJsonString(s, i, tmp);
    }
    if (s[i] == '{') {
        std::vector<std::pair<std::string, std::string>> dummy;
        std::vector<std::pair<std::string, size_t>> nest;
        return parseJsonObject(s, i, dummy, nest);
    }
    if (s[i] == '[') {
        ++i;
        skipJsonWs(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        for (;;) {
            if (!skipJsonValue(s, i))
                return false;
            skipJsonWs(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            return false;
        }
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return true;
}

static bool parseJsonObject(const std::string& s, size_t& i,
                            std::vector<std::pair<std::string, std::string>>& fields,
                            std::vector<std::pair<std::string, size_t>>& nested) {
    skipJsonWs(s, i);
    if (i >= s.size() || s[i] != '{')
        return false;
    ++i;
    fields.clear();
    nested.clear();
    for (;;) {
        skipJsonWs(s, i);
        if (i >= s.size())
            return false;
        if (s[i] == '}') {
            ++i;
            return true;
        }
        std::string key;
        if (!parseJsonString(s, i, key))
            return false;
        skipJsonWs(s, i);
        if (i >= s.size() || s[i] != ':')
            return false;
        ++i;
        skipJsonWs(s, i);
        if (i >= s.size())
            return false;
        if (s[i] == '{' || s[i] == '[') {
            nested.emplace_back(key, i);
            if (!skipJsonValue(s, i))
                return false;
        } else if (s[i] == '"') {
            std::string val;
            if (!parseJsonString(s, i, val))
                return false;
            fields.emplace_back(std::move(key), std::move(val));
        } else {
            const size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            fields.emplace_back(std::move(key), s.substr(start, i - start));
        }
        skipJsonWs(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
    }
}

static XmlElement fieldsToEl(const char* name,
                             const std::vector<std::pair<std::string, std::string>>& fields) {
    XmlElement el;
    el.name = name;
    el.attrs = fields;
    return el;
}

static std::string fieldOf(const std::vector<std::pair<std::string, std::string>>& fields,
                           const char* key) {
    for (const auto& f : fields) {
        if (f.first == key)
            return f.second;
    }
    return {};
}

using JsonObjFn = void (*)(const std::vector<std::pair<std::string, std::string>>&,
                           const std::vector<std::pair<std::string, size_t>>&, void*);

static void walkJsonValue(const std::string& s, size_t& i, JsonObjFn onObj, void* ctx);

static void walkJsonValue(const std::string& s, size_t& i, JsonObjFn onObj, void* ctx) {
    skipJsonWs(s, i);
    if (i >= s.size())
        return;
    if (s[i] == '{') {
        std::vector<std::pair<std::string, std::string>> fields;
        std::vector<std::pair<std::string, size_t>> nested;
        if (!parseJsonObject(s, i, fields, nested))
            return;
        onObj(fields, nested, ctx);
        for (const auto& n : nested) {
            size_t p = n.second;
            walkJsonValue(s, p, onObj, ctx);
        }
        return;
    }
    if (s[i] == '[') {
        ++i;
        skipJsonWs(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return;
        }
        while (i < s.size()) {
            walkJsonValue(s, i, onObj, ctx);
            skipJsonWs(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                return;
            }
            return;
        }
        return;
    }
    skipJsonValue(s, i);
}

static void collectOnDeckJson(const std::vector<std::pair<std::string, std::string>>& fields,
                              const std::vector<std::pair<std::string, size_t>>&, void* ctx) {
    if (fieldOf(fields, "ratingKey").empty())
        return;
    // JSON leaves &#39; as text. XML attributes were already unescaped once.
    auto decoded = fields;
    decodePlexMetaFields(decoded);
    static_cast<std::vector<PlexItem>*>(ctx)->push_back(
        itemFromElement(fieldsToEl("Video", decoded)));
}

struct HubJsonWalk {
    std::vector<PlexHubRow> rows;
    PlexHubRow* row = nullptr;
};

static void collectHubsJson(const std::vector<std::pair<std::string, std::string>>& fields,
                            const std::vector<std::pair<std::string, size_t>>& nested, void* ctx) {
    auto* st = static_cast<HubJsonWalk*>(ctx);
    auto decoded = fields;
    decodePlexMetaFields(decoded);
    const std::string rk = fieldOf(decoded, "ratingKey");
    bool hasMetadata = false;
    for (const auto& n : nested) {
        if (n.first == "Metadata") {
            hasMetadata = true;
            break;
        }
    }
    const bool isHub = !fieldOf(decoded, "hubKey").empty() ||
                       !fieldOf(decoded, "hubIdentifier").empty() ||
                       (rk.empty() && hasMetadata && !fieldOf(decoded, "title").empty());
    if (isHub) {
        st->rows.emplace_back();
        st->row = &st->rows.back();
        st->row->title = fieldOf(decoded, "title");
        st->row->hubKey = fieldOf(decoded, "hubKey");
        if (st->row->title.empty())
            st->row->title = fieldOf(decoded, "type");
        return;
    }
    if (rk.empty())
        return;
    PlexItem it = itemFromElement(fieldsToEl("Video", decoded));
    if (it.title.empty() && it.ratingKey.empty())
        return;
    if (!st->row) {
        st->rows.emplace_back();
        st->row = &st->rows.back();
    }
    st->row->items.push_back(std::move(it));
}

static bool hubTitleWanted(const std::string& title,
                           const std::vector<PlexSection>& sections) {
    const std::string low = toLower(title);
    if (low.find("recommend") != std::string::npos || low.find("continue") != std::string::npos ||
        low.find("recent") != std::string::npos)
        return true;
    for (const auto& sec : sections)
        if (sec.title == title)
            return true;
    return false;
}

void keepContinueWatchingItems(std::vector<PlexItem>& items) {
    std::vector<PlexItem> kept;
    kept.reserve(items.size());
    for (auto& it : items) {
        if (continueWatchingEligible(it))
            kept.push_back(std::move(it));
    }
    items = std::move(kept);
}

PlexHubRow buildContinueWatchingRow(std::vector<PlexItem> items) {
    PlexHubRow row;
    row.title = "Continue Watching";
    row.hubKey = "/library/onDeck";
    keepContinueWatchingItems(items);
    row.items = std::move(items);
    return row;
}

std::vector<PlexItem> parseOnDeckItems(const std::string& body) {
    std::vector<PlexItem> items;
    if (looksLikeJson(body)) {
        size_t i = 0;
        walkJsonValue(body, i, collectOnDeckJson, &items);
        return items;
    }
    for (const auto& el : parseXmlStartTags(body)) {
        if (el.name == "Video" || el.name == "Movie" || el.name == "Episode") {
            PlexItem it = itemFromElement(el);
            if (!it.title.empty() || !it.ratingKey.empty())
                items.push_back(std::move(it));
        }
    }
    return items;
}

void filterHomeHubs(std::vector<PlexHubRow>& hubs, const std::vector<PlexSection>& sections) {
    std::vector<PlexHubRow> kept;
    for (auto& h : hubs) {
        if (continueWatchingTitle(h.title)) {
            keepContinueWatchingItems(h.items);
            if (h.items.empty())
                continue;
        }
        if (hubTitleWanted(h.title, sections))
            kept.push_back(std::move(h));
    }
    hubs = std::move(kept);
}

static std::vector<PlexHubRow> parseHubsHomeXml(const std::string& xml) {
    std::vector<PlexHubRow> rows;
    PlexHubRow* row = nullptr;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name == "Hub") {
            rows.emplace_back();
            row = &rows.back();
            row->title = el.get("title");
            row->hubKey = el.get("hubKey");
            if (row->title.empty())
                row->title = el.get("type");
            continue;
        }
        if (!row)
            continue;
        if (el.name == "Video" || el.name == "Directory" || el.name == "Episode" ||
            el.name == "Movie" || el.name == "Season" || el.name == "Show") {
            PlexItem it = itemFromElement(el);
            if (!it.title.empty() || !it.ratingKey.empty())
                row->items.push_back(std::move(it));
        }
    }
    return rows;
}

std::vector<PlexHubRow> parseHubsHome(const std::string& xml) {
    if (looksLikeJson(xml)) {
        HubJsonWalk st;
        size_t i = 0;
        walkJsonValue(xml, i, collectHubsJson, &st);
        return std::move(st.rows);
    }
    return parseHubsHomeXml(xml);
}

std::vector<PlexHubRow> parseSectionHubs(const std::string& xml) {
    return parseHubsHome(xml);
}

static std::string directoryFilterId(const XmlElement& el) {
    const std::string id = el.get("id");
    if (!id.empty())
        return id;
    const std::string key = el.get("key");
    if (key.empty())
        return {};
    const size_t slash = key.rfind('/');
    return slash == std::string::npos ? key : key.substr(slash + 1);
}

std::vector<GridCategoryChip> parseSectionCollections(const std::string& xml) {
    std::vector<GridCategoryChip> out;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name != "Directory")
            continue;
        GridCategoryChip chip;
        chip.kind = GridCategoryChip::Kind::Collection;
        chip.title = el.get("title");
        chip.filterId = el.get("ratingKey");
        if (chip.title.empty() || chip.filterId.empty())
            continue;
        out.push_back(std::move(chip));
    }
    return out;
}

std::vector<GridCategoryChip> parseSectionGenres(const std::string& xml) {
    std::vector<GridCategoryChip> out;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name != "Directory")
            continue;
        GridCategoryChip chip;
        chip.kind = GridCategoryChip::Kind::Genre;
        chip.title = el.get("title");
        chip.filterId = directoryFilterId(el);
        if (chip.title.empty() || chip.filterId.empty())
            continue;
        out.push_back(std::move(chip));
    }
    return out;
}

std::vector<PlexItem> parseLibraryAll(const std::string& xml) {
    std::vector<PlexItem> items;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name == "Video" || el.name == "Movie" || el.name == "Episode" ||
            el.name == "Show" || el.name == "Season")
            items.push_back(itemFromElement(el));
        else if (el.name == "Directory") {
            // TV/music sections return <Directory type="show|season|artist|album">.
            const std::string t = el.get("type");
            if (t == "show" || t == "season" || t == "artist" || t == "album")
                items.push_back(itemFromElement(el));
        } else if (el.name == "Media" && !items.empty()) {
            // The item tag is often bare. The first Media child carries aspectRatio.
            storeItemAspect(items.back(), el.get("aspectRatio"));
        }
    }
    return items;
}

PlexItem parseMetadataDetail(const std::string& xml) {
    PlexItem out;
    std::vector<std::string> genres;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name == "Genre") {
            const std::string tag = el.get("tag");
            if (!tag.empty())
                genres.push_back(tag);
            continue;
        }
        if (el.name == "Video" || el.name == "Movie" || el.name == "Episode" ||
            el.name == "Directory" || el.name == "Show" || el.name == "Season") {
            const PlexItem row = itemFromElement(el);
            if (row.ratingKey.empty() && row.title.empty())
                continue;
            // A show's metadata is a Directory. Genres are sibling tags, not a second item.
            if (el.name == "Directory") {
                const std::string t = el.get("type");
                if (t != "show" && t != "season" && t != "episode")
                    continue;
            }
            out = row;
            continue;
        }
    }
    out.subtitles = plexSubtitlesFromCatalog(parseStreamCatalog(xml));
    out.audios = plexAudiosFromCatalog(parseStreamCatalog(xml), partIdFromXml(xml));
    if (!genres.empty()) {
        out.genres = genres.front();
        for (size_t i = 1; i < genres.size(); ++i) {
            out.genres += " | ";
            out.genres += genres[i];
        }
    }
    return out;
}

void resolveServerBasesProgressive(HttpGetFn get, std::vector<PlexServer>& servers,
                                   const BrowseCancelFn& cancel, const ServersUpdateFn& onUpdate) {
    auto& cache = serverBaseCache();
    const bool tlsVerify = !plexUiCaBundlePath().empty();
    std::mutex m;   // guards servers + cache while probe threads report
    std::vector<std::thread> jobs;
    bool cachedAny = false;
    for (size_t i = 0; i < servers.size(); ++i) {
        servers[i].offline = false;
        if (!servers[i].machineIdentifier.empty() &&
            !isSyntheticLocalServerId(servers[i].machineIdentifier)) {
            const auto it = cache.find(servers[i].machineIdentifier);
            if (it != cache.end()) {
                servers[i].baseUrl = it->second;
                cachedAny = true;
                continue;
            }
        }
        servers[i].baseUrl.clear();
        if (servers[i].connections.empty())
            servers[i].offline = true;
    }
    // Every server is prepared before the first probe thread starts (a thread snapshots all of them).
    for (size_t i = 0; i < servers.size(); ++i) {
        if (!servers[i].baseUrl.empty() || servers[i].offline)
            continue;
        PlexServer copy = servers[i];
        jobs.emplace_back([&, copy, i] {
            std::string gotId;
            probeServerBaseParallel(
                get, copy, cancel, tlsVerify,
                [&](const std::string& base) {
                    std::vector<PlexServer> snap;
                    {
                        std::lock_guard<std::mutex> lk(m);
                        servers[i].baseUrl = base;
                        servers[i].offline = base.empty();
                        if (!base.empty() && !gotId.empty() &&
                            (servers[i].machineIdentifier.empty() ||
                             isSyntheticLocalServerId(servers[i].machineIdentifier)))
                            servers[i].machineIdentifier = gotId;
                        if (!base.empty() && !servers[i].machineIdentifier.empty())
                            cache[servers[i].machineIdentifier] = base;
                        snap = servers;
                    }
                    if (onUpdate && !(cancel && cancel()))
                        onUpdate(snap);
                },
                &gotId);
        });
    }
    if (cachedAny && onUpdate) {
        std::vector<PlexServer> snap;
        {
            std::lock_guard<std::mutex> lk(m);
            snap = servers;
        }
        onUpdate(snap);
    }
    for (auto& t : jobs)
        t.join();
    collapseServersByIdentity(servers);
}

namespace {

std::string stripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

std::string hostFromBaseUrl(std::string url) {
    url = stripTrailingSlash(std::move(url));
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos)
        url = url.substr(scheme + 3);
    const size_t slash = url.find('/');
    if (slash != std::string::npos)
        url = url.substr(0, slash);
    return url;
}

bool hostPortMatch(const PlexServer& a, const PlexServer& b) {
    // #120: a conf seed (synthetic local: id) still matches plex.tv by WAN IP.
    // GH#14: two real machine IDs that only share a public :8443 stay two rows
    // (LAN vs WAN server names). LAN host:port still merges studio stale+live
    // on the same private address.
    const bool seedSide = isSyntheticLocalServerId(a.machineIdentifier) ||
                          isSyntheticLocalServerId(b.machineIdentifier) ||
                          a.machineIdentifier.empty() || b.machineIdentifier.empty();
    auto add = [seedSide](std::vector<std::string>& out, std::string hp) {
        hp = stripTrailingSlash(std::move(hp));
        if (hp.empty())
            return;
        std::string host = hp;
        const size_t pd = host.find(".plex.direct");
        if (pd != std::string::npos) {
            const size_t colon = host.find(':', pd);
            if (colon != std::string::npos)
                host.resize(colon);
        } else {
            const size_t colon = host.rfind(':');
            if (colon != std::string::npos && host.find(':') == colon)
                host.resize(colon);
        }
        if (!seedSide && !hostIsPrivateLan(host))
            return;
        for (const auto& e : out) {
            if (e == hp)
                return;
        }
        out.push_back(std::move(hp));
    };
    auto keys = [&](const PlexServer& s) {
        std::vector<std::string> k;
        if (!s.baseUrl.empty())
            add(k, hostFromBaseUrl(s.baseUrl));
        for (const auto& c : s.connections) {
            if (c.address.empty())
                continue;
            add(k, c.address + ":" + (c.port.empty() ? "32400" : c.port));
        }
#ifndef MUT_GWY_NO_URI_HOST
        // reviewer: a conf seed on a plex.direct name or relay host matches the uri of any
        // connection (local, remote, relay), not only the bare address.
        for (const auto& c : s.connections) {
            if (!c.uri.empty())
                add(k, hostFromBaseUrl(c.uri));
        }
#endif
        return k;
    };
    const auto ak = keys(a);
    const auto bk = keys(b);
    for (const auto& x : ak) {
        for (const auto& y : bk) {
            if (x == y)
                return true;
        }
    }
    return false;
}

bool sameServerId(const PlexServer& a, const PlexServer& b) {
    if (!a.machineIdentifier.empty() && a.machineIdentifier == b.machineIdentifier &&
        !isSyntheticLocalServerId(a.machineIdentifier))
        return true;
#ifndef MUT_GWY_ID_MACHINE_ONLY
    const std::string aIds[2] = {a.machineIdentifier, a.clientIdentifier};
    const std::string bIds[2] = {b.machineIdentifier, b.clientIdentifier};
    for (const auto& x : aIds) {
        if (x.empty() || isSyntheticLocalServerId(x))
            continue;
        for (const auto& y : bIds) {
            if (x == y)
                return true;
        }
    }
#endif
    return false;
}

bool sameServer(const PlexServer& a, const PlexServer& b) {
    if (sameServerId(a, b))
        return true;
    if (!a.baseUrl.empty() && a.baseUrl == b.baseUrl)
        return true;
    return hostPortMatch(a, b);
}

void mergeInto(std::vector<PlexServer>& out, const PlexServer& incoming, bool preferIncoming) {
    for (auto& e : out) {
        if (!sameServer(e, incoming))
            continue;
        // tipA-local: two plex.tv Devices for one PMS (a stale re-claim, same LAN host:port) used to
        // merge into whichever came first: a live row kept offline=true (Browse "Offline", select
        // refused, no chevron), or the dead row's token/id overwrote the live one. Answered wins.
        const bool eLive = !e.baseUrl.empty() && !e.offline;
        const bool inLive = !incoming.baseUrl.empty() && !incoming.offline;
#ifndef MUT_MERGE_DEAD_WINS
        const bool keepE = eLive && incoming.offline;   // probed dead: never overwrites a live row
        if (inLive)
            e.offline = false;
#else
        const bool keepE = false;
        (void)eLive;
        (void)inLive;
#endif
        if (!keepE) {
            if (preferIncoming && !incoming.name.empty())
                e.name = incoming.name;
            if (!incoming.accessToken.empty() && incoming.accessToken != e.accessToken) {
                e.accessToken = incoming.accessToken;
                e.confTokenId = incoming.confTokenId;   // plex.tv token: no longer the conf token
            }
            if (!incoming.machineIdentifier.empty() &&
                incoming.machineIdentifier.compare(0, 6, "local:") != 0)
                e.machineIdentifier = incoming.machineIdentifier;
            if (!incoming.sourceTitle.empty())
                e.sourceTitle = incoming.sourceTitle;
            if (!incoming.baseUrl.empty())
                e.baseUrl = incoming.baseUrl;
        }
        // Union, not "first wins": a conf seed (PLEX_SERVERS, one https://<ip>:port connection) that
        // matches a plex.tv server by host:port used to leave it with only that IP literal, which fails
        // TLS (cert is *.plex.direct) -> reviewer/reviewer "Offline" on a device with PLEX_SERVERS (#120).
        for (const auto& ic : incoming.connections) {
            const std::string ib = connectionBase(ic);
            bool dup = false;
            for (const auto& ec : e.connections) {
                if ((!ib.empty() && connectionBase(ec) == ib) ||
                    (ec.uri.empty() && ic.uri.empty() && ec.protocol == ic.protocol &&
                     ec.address == ic.address && ec.port == ic.port && ec.relay == ic.relay)) {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                e.connections.push_back(ic);
        }
        e.owned = e.owned || incoming.owned;
        return;
    }
    out.push_back(incoming);
}

std::string gdmHeader(const std::string& packet, const char* key) {
    const std::string pat = std::string(key) + ":";
    size_t i = 0;
    while (i < packet.size()) {
        size_t end = packet.find('\n', i);
        if (end == std::string::npos)
            end = packet.size();
        std::string line = packet.substr(i, end - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() >= pat.size()) {
            bool match = true;
            for (size_t k = 0; k < pat.size(); ++k) {
                if (std::tolower(static_cast<unsigned char>(line[k])) !=
                    std::tolower(static_cast<unsigned char>(pat[k]))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string v = line.substr(pat.size());
                while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
                    v.erase(v.begin());
                while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
                    v.pop_back();
                return v;
            }
        }
        i = end + 1;
    }
    return {};
}

} // namespace

bool isSyntheticLocalServerId(const std::string& id) {
    return id.compare(0, 6, "local:") == 0;
}

std::string identityExpectId(const PlexServer& s) {
    if (!s.machineIdentifier.empty() && !isSyntheticLocalServerId(s.machineIdentifier))
        return s.machineIdentifier;
    if (!s.clientIdentifier.empty() && !isSyntheticLocalServerId(s.clientIdentifier))
        return s.clientIdentifier;
    return {};
}

PlexServer makeLocalPlexServer(std::string name, std::string baseUrl, const std::string& token) {
    PlexServer s;
    s.baseUrl = stripTrailingSlash(std::move(baseUrl));
    s.accessToken = token;
    s.owned = true;
    const std::string hostPort = hostFromBaseUrl(s.baseUrl);
    s.name = name.empty() ? (hostPort.empty() ? "Plex" : hostPort) : std::move(name);
    s.machineIdentifier = "local:" + s.baseUrl;
    PlexConnection c;
    if (s.baseUrl.compare(0, 8, "https://") == 0)
        c.protocol = "https";
    else
        c.protocol = "http";
    c.local = true;
    c.uri = s.baseUrl;
    std::string hp = hostPort;
    const size_t colon = hp.rfind(':');
    if (colon != std::string::npos && hp.find(']') == std::string::npos) {
        c.address = hp.substr(0, colon);
        c.port = hp.substr(colon + 1);
    } else {
        c.address = hp;
        c.port = c.protocol == "https" ? "443" : "32400";
    }
    s.connections.push_back(std::move(c));
    return s;
}

std::vector<PlexServer> mergePlexServerLists(const std::vector<PlexServer>& seeds,
                                             const std::vector<PlexServer>& account) {
    std::vector<PlexServer> out;
    for (const auto& s : seeds)
        mergeInto(out, s, false);
    for (const auto& s : account)
        mergeInto(out, s, true);
    return out;
}

std::vector<PlexServer> mergeSeedsWithAccount(const std::vector<PlexServer>& seeds,
                                              const std::vector<PlexServer>& account) {
    std::vector<PlexServer> withTok;
    std::vector<PlexServer> noTok;
    for (const auto& s : account)
        (s.accessToken.empty() ? noTok : withTok).push_back(s);
#ifdef MUT_331_TOKEN_ADDR_MERGE
    auto out = mergePlexServerLists(seeds, withTok);
#else
    // #331: a plex.tv row that carries its own token must not merge into a seed
    // by address/WAN IP (#120 still works via machine/client id after /identity).
    auto out = seeds;
    for (const auto& s : withTok) {
        bool merged = false;
        for (auto& e : out) {
            if (!sameServerId(e, s))
                continue;
            std::vector<PlexServer> one{e};
            mergeInto(one, s, true);
            if (one.size() == 1)
                e = one[0];
            merged = true;
            break;
        }
        if (!merged)
            out.push_back(s);
    }
#endif
    // A plex.tv row with no token helps only when it IS a conf server. #328: proven only by
    // machine id (the conf seed's tokenless /identity id == the row's machine/client id). An
    // address match (reused private IP, WAN IP) never adopts another id/connections or a token.
    for (const auto& s : noTok) {
#ifdef MUT_reviewer_ADDR_MERGE
        bool matched = false;
        for (const auto& e : out)
            if (!e.accessToken.empty() && sameServer(e, s))
                matched = true;
        if (matched)
            mergeInto(out, s, true);
#else
        for (auto& e : out) {
            const std::string& pid = e.confTokenId;
            if (e.accessToken.empty() || pid.empty())
                continue;
            if (pid != s.machineIdentifier && pid != s.clientIdentifier)
                continue;
            std::vector<PlexServer> one{e};
            mergeInto(one, s, true);
            if (one.size() == 1)
                e = one[0];
            break;
        }
#endif
    }
    return out;
}

void proveSeedIdentities(HttpGetFn get, std::vector<PlexServer>& seeds,
                         const std::string& accountToken, const BrowseCancelFn& cancel) {
    for (auto& s : seeds) {
        if (cancel && cancel())
            return;
        if (s.accessToken.empty() || s.accessToken == accountToken || !s.confTokenId.empty())
            continue;
        std::string want = identityExpectId(s);   // configured id, if the conf has one
        std::string got;
        const bool reached =
            probeIdentity(get, s.baseUrl, std::string(), cancel, s.name, want, &got, !want.empty());
        if (want.empty() && reached)
            want = got;   // learned tokenless from the conf server itself
        if (!reached || want.empty() || got != want) {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "plexui: conf server %s id not verified - token withheld",
                          s.name.empty() ? "?" : s.name.c_str());
            plexuiProbeLogLine(buf);
#ifndef MUT_reviewer_TOKEN_ON_ADDR
            s.accessToken.clear();
#endif
            continue;
        }
        s.confTokenId = want;
        s.machineIdentifier = want;
    }
}

std::string plexAuthErrorText(int status) {
#ifdef MUT_GWY_AUTH_GENERIC
    (void)status;
    return {};
#else
    if (status == 401)
        return "Plex server refused the token (401) - sign in again or check PLEX_TOKEN";
    if (status == 403)
        return "Plex server denied access (403) - this account has no library access";
    return {};
#endif
}

void collapseServersByIdentity(std::vector<PlexServer>& servers) {
    servers = mergePlexServerLists(servers, {});
}

bool parseGdmAdvertise(const std::string& packet, const std::string& fromAddr, PlexServer& out) {
    const std::string ctype = gdmHeader(packet, "Content-Type");
    if (ctype.find("plex/media-server") == std::string::npos)
        return false;
    const std::string name = gdmHeader(packet, "Name");
    const std::string port = gdmHeader(packet, "Port");
    const std::string id = gdmHeader(packet, "Resource-Identifier");
    if (fromAddr.empty() && name.empty())
        return false;
    std::string host = fromAddr;
    if (host.empty())
        host = "127.0.0.1";
    const std::string usePort = port.empty() ? "32400" : port;
    out = makeLocalPlexServer(name, "http://" + host + ":" + usePort, {});
    if (!id.empty())
        out.machineIdentifier = id;
    return true;
}

std::vector<PlexServer> discoverLocalPlexServers(int timeoutMs) {
    std::vector<PlexServer> found;
    if (timeoutMs <= 0)
        return found;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return found;
    const int yes = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        ::close(fd);
        return found;
    }
    const char probe[] = "M-SEARCH * HTTP/1.1\r\n\r\n";
    auto sendTo = [&](const char* ip, uint16_t port) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        if (::inet_aton(ip, &dst.sin_addr) != 1)
            return;
        (void)::sendto(fd, probe, sizeof(probe) - 1, 0, reinterpret_cast<sockaddr*>(&dst),
                       sizeof(dst));
    };
    sendTo("239.0.0.250", 32414);
    sendTo("255.255.255.255", 32412);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count());
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        if (::poll(&p, 1, std::max(1, left)) <= 0)
            continue;
        char buf[1024];
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0)
            continue;
        buf[n] = 0;
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        PlexServer s;
        if (!parseGdmAdvertise(std::string(buf, static_cast<size_t>(n)), ip, s))
            continue;
        mergeInto(found, s, true);
    }
    ::close(fd);
    return found;
}

std::vector<PlexServer> fetchPlexServersProgressive(HttpGetFn get, const std::string& accountToken,
                                                    const BrowseCancelFn& cancel,
                                                    const ServersUpdateFn& onUpdate,
                                                    bool* tlsFailed,
                                                    const std::vector<PlexServer>& seeds,
                                                    int* resourcesStatus) {
    if (tlsFailed)
        *tlsFailed = false;
    if (resourcesStatus)
        *resourcesStatus = 0;
    std::vector<PlexServer> account;
    if (!accountToken.empty()) {
        const std::string url = appendToken(
            "https://plex.tv/api/resources?includeHttps=1&includeRelay=1", accountToken);
        const BrowseHttpResult r = get(url, 6000, 15000, cancel);
        if (resourcesStatus)
            *resourcesStatus = r.status;
        if (!r.ok()) {
            if (tlsFailed)
                *tlsFailed = r.tlsFailed;
        } else {
            account = parsePlexResources(r.body);
        }
    }
    std::vector<PlexServer> proven = seeds;
    proveSeedIdentities(get, proven, accountToken, cancel);
    auto servers = mergeSeedsWithAccount(proven, account);
    if (servers.empty())
        return {};
    resolveServerBasesProgressive(get, servers, cancel, onUpdate);
    return servers;
}

std::vector<PlexServer> fetchPlexServers(HttpGetFn get, const std::string& accountToken,
                                         const BrowseCancelFn& cancel,
                                         const std::vector<PlexServer>& seeds) {
    std::vector<PlexServer> account;
    if (!accountToken.empty()) {
        const std::string url = appendToken(
            "https://plex.tv/api/resources?includeHttps=1&includeRelay=1", accountToken);
        const BrowseHttpResult r = get(url, 6000, 15000, cancel);
        if (r.ok())
            account = parsePlexResources(r.body);
    }
    std::vector<PlexServer> proven = seeds;
    proveSeedIdentities(get, proven, accountToken, cancel);
    auto servers = mergeSeedsWithAccount(proven, account);
    resolveServerBases(get, servers, cancel);
    servers.erase(std::remove_if(servers.begin(), servers.end(),
                                 [](const PlexServer& s) { return s.baseUrl.empty(); }),
                  servers.end());
    return servers;
}

bool fetchLibrarySections(HttpGetFn get, const PlexServer& server, const BrowseCancelFn& cancel,
                          std::vector<PlexSection>& sections, std::string& err) {
    const std::string secUrl =
        appendToken(server.baseUrl + "/library/sections", server.accessToken);
    const std::string cacheKey = server.machineIdentifier + "|sections";
    std::string cached;
    if (g_apiCache.get(cacheKey, cached)) {
        sections = parseLibrarySections(cached);
        g_apiCache.refreshAsync(cacheKey, secUrl, get, 4000, 12000, cancel);
        return true;
    }
    const BrowseHttpResult r = get(secUrl, 4000, 12000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = plexAuthErrorText(r.status);
        if (err.empty()) {
            err = "sections http=" + std::to_string(r.status);
        } else {
            const std::string& who =
                !server.machineIdentifier.empty() ? server.machineIdentifier : server.name;
            plexuiProbeLogLine("plexui: library auth error server=" + who +
                               " http=" + std::to_string(r.status) + ": " + err);
        }
        return false;
    }
    g_apiCache.put(cacheKey, r.body);
    sections = parseLibrarySections(r.body);
    return true;
}

void ensureServerPickerRow(std::vector<SidebarLibraryEntry>& entries, size_t serverCount) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const SidebarLibraryEntry& e) {
                                     return e.kind == SidebarRowKind::ServerPicker || e.type == "servers";
                                 }),
                  entries.end());
    if (serverCount < 2)
        return;
    SidebarLibraryEntry row;
    row.title = "Servers";
    row.subtitle = std::to_string(serverCount) + " servers";
    row.type = "servers";
    row.sectionKey = "servers";
    row.serverIndex = -1;
    row.kind = SidebarRowKind::ServerPicker;
    entries.insert(entries.begin(), std::move(row));
}

void buildSidebarEntries(const std::vector<PlexServer>& servers,
                         const std::vector<std::vector<PlexSection>>& perServerSections,
                         std::vector<SidebarLibraryEntry>& out) {
    out.clear();
    const size_t n = std::min(servers.size(), perServerSections.size());
    for (size_t si = 0; si < n; ++si) {
        const PlexServer& srv = servers[si];
        SidebarLibraryEntry hdr;
        hdr.title = srv.name.empty() ? "Plex" : srv.name;
        if (srv.offline)
            hdr.subtitle = "Offline";
        else if (!srv.sourceTitle.empty())
            hdr.subtitle = srv.sourceTitle;
        else
            hdr.subtitle = srv.owned ? "Owned" : "Shared";
        hdr.type = "server";
        hdr.kind = SidebarRowKind::Server;
        hdr.serverIndex = static_cast<int>(si);
        out.push_back(std::move(hdr));
        std::vector<PlexSection> secs = perServerSections[si];
        std::sort(secs.begin(), secs.end(), [](const PlexSection& a, const PlexSection& b) {
            return a.title < b.title;
        });
        for (const PlexSection& sec : secs) {
            SidebarLibraryEntry e;
            e.title = sec.title;
            e.type = sec.type;
            e.sectionKey = sec.key;
            e.serverIndex = static_cast<int>(si);
            e.kind = SidebarRowKind::Library;
            out.push_back(std::move(e));
        }
    }
}

void appendSidebarSignIn(std::vector<SidebarLibraryEntry>& out) {
    SidebarLibraryEntry signIn;
    signIn.title = "Sign in";
    signIn.subtitle = "plex.tv/link";
    signIn.type = "signin";
    signIn.sectionKey = "signin";
    signIn.kind = SidebarRowKind::SignIn;
    out.push_back(std::move(signIn));
}

void appendSidebarHomeUser(std::vector<SidebarLibraryEntry>& out, const std::string& profileTitle) {
    SidebarLibraryEntry row;
    if (!profileTitle.empty()) {
        row.title = profileTitle;
        row.subtitle = "Switch user";
    } else {
        row.title = "Switch user";
        row.subtitle = "Plex Home";
    }
    row.type = "homeuser";
    row.sectionKey = "homeuser";
    row.kind = SidebarRowKind::HomeUser;
    out.push_back(std::move(row));
}

int pickHomeServerIndex(const std::vector<PlexServer>& servers) {
    // GH#14: an owned RFC1918 / plex.direct-LAN row (e.g. 192.0.2.10) beats a
    // WAN "local" owned row (e.g. 203.0.113.47).
    for (size_t i = 0; i < servers.size(); ++i) {
        if (servers[i].owned && !servers[i].offline && !servers[i].baseUrl.empty() &&
            hostIsPrivateLan(hostOfUrl(servers[i].baseUrl)))
            return static_cast<int>(i);
    }
    for (size_t i = 0; i < servers.size(); ++i) {
        if (servers[i].owned && !servers[i].offline && serverHasPrivateLanReach(servers[i]))
            return static_cast<int>(i);
    }
    static const char* kPrefer[] = {"serverB", "serverA"};
    for (const char* pref : kPrefer) {
        for (size_t i = 0; i < servers.size(); ++i) {
            if (servers[i].owned)
                continue;
            const std::string st = servers[i].sourceTitle;
            if (st == pref && !servers[i].offline)
                return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < servers.size(); ++i) {
        if (!servers[i].owned && !servers[i].baseUrl.empty())
            return static_cast<int>(i);
    }
    for (size_t i = 0; i < servers.size(); ++i) {
        if (!servers[i].baseUrl.empty())
            return static_cast<int>(i);
    }
    return servers.empty() ? -1 : 0;
}

int rotatePickedServerToFront(std::vector<PlexServer>& servers, int index,
                              std::vector<std::vector<PlexSection>>* perSec,
                              std::vector<std::string>* errs) {
    if (index <= 0 || index >= static_cast<int>(servers.size()))
        return index < 0 ? index : 0;
    std::swap(servers[0], servers[static_cast<std::size_t>(index)]);
    if (perSec && index < static_cast<int>(perSec->size()))
        std::swap((*perSec)[0], (*perSec)[static_cast<std::size_t>(index)]);
    if (errs && index < static_cast<int>(errs->size()))
        std::swap((*errs)[0], (*errs)[static_cast<std::size_t>(index)]);
    return 0;
}

std::vector<std::vector<PlexSection>> fetchAllServerSectionsParallel(
    HttpGetFn get, const std::vector<PlexServer>& servers, const BrowseCancelFn& cancel,
    std::vector<std::string>* errs) {
    std::vector<std::vector<PlexSection>> out(servers.size());
    std::vector<std::string> errv(servers.size());
    std::vector<std::future<void>> jobs;
    for (size_t i = 0; i < servers.size(); ++i) {
        jobs.push_back(std::async(std::launch::async, [&, i] {
            if (cancel() || servers[i].baseUrl.empty())   // 0011: an offline server has no sections
                return;
            fetchLibrarySections(get, servers[i], cancel, out[i], errv[i]);
        }));
    }
    for (auto& j : jobs)
        j.get();
    if (errs)
        *errs = std::move(errv);
    return out;
}

bool fetchMergedSharedContinueWatching(HttpGetFn get, const std::vector<PlexServer>& servers,
                                       const BrowseCancelFn& cancel, PlexHubRow& deck,
                                       std::unordered_map<std::string, int>& itemServerIndex) {
    deck = {};
    deck.title = "Continue Watching";
    deck.hubKey = "/library/onDeck";
    itemServerIndex.clear();
    for (size_t si = 0; si < servers.size(); ++si) {
        const PlexServer& srv = servers[si];
        if (srv.owned || srv.baseUrl.empty())
            continue;
        const std::string deckUrl =
            appendToken(srv.baseUrl + "/library/onDeck", srv.accessToken);
        const BrowseHttpResult r = get(deckUrl, 4000, 12000, cancel);
        if (r.cancelled)
            return false;
        if (!r.ok())
            continue;
        for (auto& it : parseOnDeckItems(r.body)) {
            if (!continueWatchingEligible(it))
                continue;
            if (!it.ratingKey.empty())
                itemServerIndex[it.ratingKey] = static_cast<int>(si);
            deck.items.push_back(std::move(it));
        }
    }
    return !deck.items.empty();
}

bool fetchServerHome(HttpGetFn get, const PlexServer& server, const BrowseCancelFn& cancel,
                     std::vector<PlexSection>& sections, std::vector<PlexHubRow>& hubs,
                     std::string& err) {
    if (!fetchLibrarySections(get, server, cancel, sections, err))
        return false;

    const std::string hubsUrl = appendToken(server.baseUrl + "/hubs?count=12", server.accessToken);
    const std::string hubsCacheKey = server.machineIdentifier + "|hubs";
    std::string hubsBody;
    BrowseHttpResult r;
    if (g_apiCache.get(hubsCacheKey, hubsBody)) {
        hubs = parseHubsHome(hubsBody);
        if (!hubs.empty()) {
            g_apiCache.refreshAsync(hubsCacheKey, hubsUrl, get, 4000, 12000, cancel);
        } else {
            // A cached empty /hubs document leaves the Home pane blank. The async
            // refresh updates the cache only, so fetch live once before giving up.
            r = get(hubsUrl, 4000, 12000, cancel);
            if (r.cancelled)
                return false;
            if (r.ok()) {
                g_apiCache.put(hubsCacheKey, r.body);
                hubs = parseHubsHome(r.body);
            }
        }
    } else {
        r = get(hubsUrl, 4000, 12000, cancel);
        if (r.cancelled)
            return false;
        if (r.ok()) {
            g_apiCache.put(hubsCacheKey, r.body);
            hubs = parseHubsHome(r.body);
        }
    }

    const std::string deckUrl = appendToken(server.baseUrl + "/library/onDeck", server.accessToken);
    r = get(deckUrl, 4000, 12000, cancel);
    if (!r.cancelled && r.ok()) {
        PlexHubRow deck = buildContinueWatchingRow(parseOnDeckItems(r.body));
        if (!deck.items.empty()) {
            bool replaced = false;
            for (auto& h : hubs) {
                if (continueWatchingTitle(h.title)) {
                    h.title = deck.title;
                    h.hubKey = deck.hubKey;
                    h.items = std::move(deck.items);
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                hubs.insert(hubs.begin(), std::move(deck));
        }
    }
    filterHomeHubs(hubs, sections);
    return true;
}

namespace {

void logLibraryRequest(const char* which, const PlexServer& server, const std::string& sectionKey,
                       long long ms, int ok) {
    const std::string& who = !server.machineIdentifier.empty() ? server.machineIdentifier
                                                              : server.name;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "plexui: library %s server=%s section=%s ms=%lld ok=%d", which ? which : "",
                  who.c_str(), sectionKey.c_str(), ms, ok);
    plexuiProbeLogLine(buf);
}

long long elapsedMs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
        .count();
}

} // namespace

void plexUiApiCacheDrain() { g_apiCache.drainRefreshes(); }
#ifdef MPX_PLEXUI_UNIT_TEST
void plexUiApiCacheClear() { g_apiCache.testClear(); }
#endif

static void noteGridTotal(int* totalSize, const std::string& body) {
    if (totalSize)
        *totalSize = gridPageTotalSize(body);
}

bool fetchSectionGrid(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                      const BrowseCancelFn& cancel, std::vector<PlexItem>& items,
                      std::string& err, int* totalSize) {
    // First page (Cortex-A9 RAM/parse time): 200 items, sorted by title.
    // GRID_PAGE off stops here. On, fetchSectionGridPage asks for a later offset.
    // Key includes the machine id, not only the URL, so two servers cannot share a page.
    std::string url = server.baseUrl + "/library/sections/" + sectionKey + "/all?" +
                      gridPageAllQuery(0);
    url = appendToken(url, server.accessToken);
    const std::string cacheKey = server.machineIdentifier + "|all|" + sectionKey;
    std::string cached;
    if (g_apiCache.get(cacheKey, cached)) {
        items = parseLibraryAll(cached);
        noteGridTotal(totalSize, cached);
        g_apiCache.refreshAsync(cacheKey, url, get, 4000, 20000, cancel);
        return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const BrowseHttpResult r = get(url, 4000, 20000, cancel);
    const long long ms = elapsedMs(t0);
    if (r.cancelled) {
        logLibraryRequest("items", server, sectionKey, ms, 0);
        return false;
    }
    if (!r.ok()) {
        logLibraryRequest("items", server, sectionKey, ms, 0);
        err = "grid http=" + std::to_string(r.status);
        return false;
    }
    logLibraryRequest("items", server, sectionKey, ms, 1);
    g_apiCache.put(cacheKey, r.body);
    items = parseLibraryAll(r.body);
    noteGridTotal(totalSize, r.body);
    return true;
}

bool fetchSectionCollections(HttpGetFn get, const PlexServer& server,
                             const std::string& sectionKey, const BrowseCancelFn& cancel,
                             std::vector<GridCategoryChip>& chips, std::string& err) {
    std::string url =
        server.baseUrl + "/library/sections/" + sectionKey + "/collections";
    url = appendToken(url, server.accessToken);
    const auto t0 = std::chrono::steady_clock::now();
    const BrowseHttpResult r = get(url, 4000, 12000, cancel);
    const long long ms = elapsedMs(t0);
    if (r.cancelled) {
        logLibraryRequest("collections", server, sectionKey, ms, 0);
        return false;
    }
    if (r.unreachable()) {
        // Connect failure or the open's deadline. Do not try genres or items.
        logLibraryRequest("collections", server, sectionKey, ms, 0);
        err = "server unreachable";
        return false;
    }
    if (!r.ok()) {
        // An HTTP error from a server that answered is best-effort: empty chips, keep going.
        logLibraryRequest("collections", server, sectionKey, ms, r.status == 404 ? 1 : 0);
        chips.clear();
        return true;
    }
    logLibraryRequest("collections", server, sectionKey, ms, 1);
    chips = parseSectionCollections(r.body);
    return true;
}

bool fetchSectionGenres(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                        const BrowseCancelFn& cancel, std::vector<GridCategoryChip>& chips,
                        std::string& err) {
    std::string url = server.baseUrl + "/library/sections/" + sectionKey + "/genre";
    url = appendToken(url, server.accessToken);
    const auto t0 = std::chrono::steady_clock::now();
    const BrowseHttpResult r = get(url, 4000, 12000, cancel);
    const long long ms = elapsedMs(t0);
    if (r.cancelled) {
        logLibraryRequest("genres", server, sectionKey, ms, 0);
        return false;
    }
    if (r.unreachable()) {
        logLibraryRequest("genres", server, sectionKey, ms, 0);
        err = "server unreachable";
        return false;
    }
    if (!r.ok()) {
        logLibraryRequest("genres", server, sectionKey, ms, r.status == 404 ? 1 : 0);
        chips.clear();
        return true;
    }
    logLibraryRequest("genres", server, sectionKey, ms, 1);
    chips = parseSectionGenres(r.body);
    return true;
}

bool fetchSectionGridGenre(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                           const std::string& genreId, const BrowseCancelFn& cancel,
                           std::vector<PlexItem>& items, std::string& err, int* totalSize) {
    std::string url = server.baseUrl + "/library/sections/" + sectionKey + "/all?" +
                      gridPageGenreQuery(urlEncode(genreId), 0);
    url = appendToken(url, server.accessToken);
    const BrowseHttpResult r = get(url, 4000, 20000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "grid genre http=" + std::to_string(r.status);
        return false;
    }
    items = parseLibraryAll(r.body);
    noteGridTotal(totalSize, r.body);
    return true;
}

bool fetchSectionGridPage(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                          int start, const std::string& genreId, const BrowseCancelFn& cancel,
                          std::vector<PlexItem>& items, int* totalSize, std::string& err,
                          char letter) {
    if (!gridPageRequestAllowed(start)) {
        err = "grid page off";
        if (totalSize)
            *totalSize = -1;
        return false;
    }
    const int use = gridPageStart(start);
    // A letter is a different result set. Skip the unfiltered first-page cache.
    const char wire = azJumpWireLetter(letter);
    if (wire == 0) {
        if (use == 0 && genreId.empty())
            return fetchSectionGrid(get, server, sectionKey, cancel, items, err, totalSize);
        if (use == 0)
            return fetchSectionGridGenre(get, server, sectionKey, genreId, cancel, items, err,
                                         totalSize);
    }
    // Later pages, and any letter page, are not stored under the first-page cache key.
    std::string url = server.baseUrl + "/library/sections/" + sectionKey + "/all?";
    if (genreId.empty())
        url += azJumpDecorate(gridPageAllQuery(use), letter);
    else
        url += azJumpDecorate(gridPageGenreQuery(urlEncode(genreId), use), letter);
    url = appendToken(url, server.accessToken);
    const auto t0 = std::chrono::steady_clock::now();
    const BrowseHttpResult r = get(url, 4000, 20000, cancel);
    const long long ms = elapsedMs(t0);
    if (r.cancelled) {
        logLibraryRequest("page", server, sectionKey, ms, 0);
        return false;
    }
    if (!r.ok()) {
        logLibraryRequest("page", server, sectionKey, ms, 0);
        err = "grid page http=" + std::to_string(r.status);
        return false;
    }
    logLibraryRequest("page", server, sectionKey, ms, 1);
    items = parseLibraryAll(r.body);
    noteGridTotal(totalSize, r.body);
    return true;
}

bool fetchCollectionChildren(HttpGetFn get, const PlexServer& server,
                             const std::string& collectionRatingKey, const BrowseCancelFn& cancel,
                             std::vector<PlexItem>& items, std::string& err) {
    std::string url =
        server.baseUrl + "/library/collections/" + collectionRatingKey + "/children";
    url = appendToken(url, server.accessToken);
    const BrowseHttpResult r = get(url, 4000, 20000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "collection http=" + std::to_string(r.status);
        return false;
    }
    items = parseLibraryAll(r.body);
    return true;
}

bool fetchMetadata(HttpGetFn get, const PlexServer& server, const std::string& ratingKey,
                   const BrowseCancelFn& cancel, PlexItem& item, std::string& err) {
    std::string url = server.baseUrl + "/library/metadata/" + ratingKey;
    url = appendToken(url, server.accessToken);
    const BrowseHttpResult r = get(url, 4000, 12000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "metadata http=" + std::to_string(r.status);
        return false;
    }
    item = parseMetadataDetail(r.body);
    return !item.ratingKey.empty();
}

bool fetchShowTheme(HttpHeaderGetFn get, const PlexServer& server, const PlexItem& item,
                    const BrowseCancelFn& cancel, std::string& body, std::string& err) {
    body.clear();
    const ::misterplex::ThemeShowFetch plan = ::misterplex::themeShowFetchPlan(
        ::misterplex::themeMusicEnabled(), true, true, item.type == "show", false, server.baseUrl,
        item.ratingKey, item.theme, server.accessToken);
    if (!plan.fetch) {
        err = "no theme";
        return false;
    }
    if (plan.url.find("X-Plex-Token") != std::string::npos) {
        err = "token in url";
        return false;
    }
    if (!get) {
        err = "no get";
        return false;
    }
    const BrowseHttpResult r = get(plan.url, plan.headers, 4000, 15000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "theme http=" + std::to_string(r.status);
        return false;
    }
    if (r.body.empty() || r.body.size() > 512u * 1024u) {
        err = "theme body";
        return false;
    }
    body = r.body;
    return true;
}

std::vector<PlexItem> parseMetadataChildren(const std::string& xml) {
    return parseLibraryAll(xml);
}

bool fetchMetadataChildren(HttpGetFn get, const PlexServer& server, const std::string& ratingKey,
                           const BrowseCancelFn& cancel, std::vector<PlexItem>& items,
                           std::string& err) {
    if (ratingKey.empty() || server.baseUrl.empty()) {
        err = "no metadata";
        return false;
    }
    std::string url = server.baseUrl + "/library/metadata/" + ratingKey + "/children";
    url = appendToken(url, server.accessToken);
    const BrowseHttpResult r = get(url, 4000, 12000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "children http=" + std::to_string(r.status);
        return false;
    }
    items = parseMetadataChildren(r.body);
    return true;
}

std::string playQueueFormBody(const std::string& machineId, const std::string& ratingKey) {
    const QueueHttpRequest req = buildQueueCreate("http://unused", machineId, ratingKey);
    const size_t q = req.url.find('?');
    return q == std::string::npos ? req.body : req.url.substr(q + 1);
}

bool createPlayQueue(HttpPostFn post, const PlexServer& server, const std::string& ratingKey,
                     const std::string& clientId, const BrowseCancelFn& cancel, PlexPlayQueue& out,
                     std::string& err) {
    out = {};
    if (!post) {
        err = "no post";
        return false;
    }
    if (server.baseUrl.empty() || ratingKey.empty()) {
        err = "no metadata";
        return false;
    }
    const QueueHttpRequest req =
        buildQueueCreate(server.baseUrl, server.machineIdentifier, ratingKey);
    if (!req.body.empty() || req.url.find('?') == std::string::npos) {
        err = "playQueue bad request";
        return false;
    }
    const std::string ident = clientId.empty() ? "misterplex" : clientId;
    std::vector<std::pair<std::string, std::string>> headers = {
        {"X-Plex-Token", server.accessToken},
        {"X-Plex-Client-Identifier", ident},
        {"X-Plex-Product", "MiSTerPlex"},
        {"Accept", "application/xml"},
    };
    const BrowseHttpResult r = post(req.url, "", "", headers, 4000, 12000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "playQueue http=" + std::to_string(r.status);
        return false;
    }
    out = parsePlayQueueXml(r.body);
    if (!out.ok) {
        err = "playQueue empty";
        return false;
    }
    return true;
}

} // namespace plexui
} // namespace misterplex
