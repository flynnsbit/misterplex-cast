#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "plexui_data.hpp"

#include "plexui_api_cache.hpp"
#include "plexui_http.hpp"

#include "libmisterplex/browse_v2.hpp"

#include <cstdio>

#include <algorithm>
#include <cctype>
#include <future>
#include <unordered_map>

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
            if (el.get("presence") != "1")
                continue;
            const std::string product = el.get("product");
            if (product != "Plex Media Server")
                continue;
            if (el.get("accessToken").empty())
                continue;
            PlexServer s;
            s.name = el.get("name");
            s.sourceTitle = el.get("sourceTitle");
            s.clientIdentifier = el.get("clientIdentifier");
            s.machineIdentifier = el.get("machineIdentifier");
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
// ANOTHER server (a Docker bridge 198.51.100.17, a reused LAN IP) is not this server: "office" (the lab PMS
// 1.43.4) was taken via 198.51.100.17 on a lab MiSTer. An identity without a machineIdentifier is accepted as before.
bool probeIdentity(HttpGetFn get, const std::string& baseUrl, const std::string& token,
                    const BrowseCancelFn& cancel, const std::string& serverName,
                    const std::string& expectId = std::string()) {
    if (baseUrl.empty() || token.empty())
        return false;
    const std::string url = appendToken(baseUrl + "/identity", token);
    const BrowseHttpResult r = get(url, kProbeConnectMs, kProbeTotalMs, cancel);
    const bool reached = !r.cancelled && r.ok();
    const std::string gotId = reached ? identityMachineId(r.body) : std::string();
    const bool ok = reached && (expectId.empty() || gotId.empty() || gotId == expectId);
    char buf[256];
    if (reached && !ok) {
        std::snprintf(buf, sizeof(buf), "plexui: probe %s base=%s fail identity mismatch (another server)",
                      serverName.empty() ? "?" : serverName.c_str(), baseUrl.c_str());
        plexuiProbeLogLine(buf);
        return false;
    }
    if (ok)
        std::snprintf(buf, sizeof(buf), "plexui: probe %s base=%s ok",
                      serverName.empty() ? "?" : serverName.c_str(), baseUrl.c_str());
    else
        std::snprintf(buf, sizeof(buf), "plexui: probe %s base=%s fail http=%d curl=%d",
                      serverName.empty() ? "?" : serverName.c_str(), baseUrl.c_str(), r.status,
                      r.curlExit);
    plexuiProbeLogLine(buf);
    return ok;
}

// OB port 0011: all candidate bases of one server at once. The winner is the best-ranked success (the
// order of connectionRank, plex.direct https first); a lower-ranked success waits at most kTieGraceMs for
// better-ranked probes still running, so a dead address never delays a live one by more than that.
constexpr int kTieGraceMs = 250;
std::string probeServerBaseParallel(HttpGetFn get, PlexServer s, const BrowseCancelFn& cancel, bool tlsVerify,
                                    const std::function<void(const std::string&)>& onWinner) {
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
    const std::string expect = s.machineIdentifier.empty() ? s.clientIdentifier : s.machineIdentifier;
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> res(bases.size(), -1);   // -1 running, 0 fail, 1 ok
    std::vector<std::thread> th;
    for (size_t i = 0; i < bases.size(); ++i)
        th.emplace_back([&, i] {
            const bool ok = probeIdentity(get, bases[i], s.accessToken, cancel, s.name, expect);
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
                            bool tlsVerify) {
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
            if (probeIdentity(get, base, s.accessToken, cancel, s.name,
                              s.machineIdentifier.empty() ? s.clientIdentifier : s.machineIdentifier)) {
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
    std::vector<std::future<std::pair<std::size_t, std::string>>> jobs;
    for (size_t i = 0; i < servers.size(); ++i) {
        if (servers[i].connections.empty())
            continue;
        if (!servers[i].machineIdentifier.empty()) {
            const auto it = cache.find(servers[i].machineIdentifier);
            if (it != cache.end()) {
                servers[i].baseUrl = it->second;
                continue;
            }
        }
        servers[i].baseUrl.clear();
        PlexServer copy = servers[i];
        jobs.push_back(std::async(std::launch::async, [get, copy, cancel, tlsVerify, i] {
            return std::make_pair(i, probeServerBase(get, std::move(copy), cancel, tlsVerify));
        }));
    }
    for (auto& j : jobs) {
        const auto pr = j.get();
        if (cancel && cancel())
            return;
        PlexServer& s = servers[pr.first];
        if (!pr.second.empty()) {
            s.baseUrl = pr.second;
            if (!s.machineIdentifier.empty())
                cache[s.machineIdentifier] = pr.second;
        }
    }
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
    const std::string rating = el.get("rating");
    if (!rating.empty())
        it.rating = static_cast<float>(std::strtod(rating.c_str(), nullptr));
    it.tagline = el.get("tagline");
    const std::string aud = el.get("audienceRating");
    if (!aud.empty())
        it.audienceRating = static_cast<float>(std::strtod(aud.c_str(), nullptr));
    return it;
}

static std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

void filterHomeHubs(std::vector<PlexHubRow>& hubs, const std::vector<PlexSection>& sections) {
    std::vector<PlexHubRow> kept;
    for (auto& h : hubs) {
        if (hubTitleWanted(h.title, sections))
            kept.push_back(std::move(h));
    }
    hubs = std::move(kept);
}

std::vector<PlexHubRow> parseHubsHome(const std::string& xml) {
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
        if (el.name == "Video" || el.name == "Movie" || el.name == "Episode") {
            out = itemFromElement(el);
            continue;
        }
    }
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
        if (!servers[i].machineIdentifier.empty()) {
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
            probeServerBaseParallel(get, copy, cancel, tlsVerify, [&](const std::string& base) {
                std::vector<PlexServer> snap;
                {
                    std::lock_guard<std::mutex> lk(m);
                    servers[i].baseUrl = base;
                    servers[i].offline = base.empty();
                    if (!base.empty() && !servers[i].machineIdentifier.empty())
                        cache[servers[i].machineIdentifier] = base;
                    snap = servers;
                }
                if (onUpdate && !(cancel && cancel()))
                    onUpdate(snap);
            });
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
}

std::vector<PlexServer> fetchPlexServersProgressive(HttpGetFn get, const std::string& accountToken,
                                                    const BrowseCancelFn& cancel,
                                                    const ServersUpdateFn& onUpdate) {
    if (accountToken.empty())
        return {};
    const std::string url = appendToken(
        "https://plex.tv/api/resources?includeHttps=1&includeRelay=1", accountToken);
    const BrowseHttpResult r = get(url, 6000, 15000, cancel);
    if (!r.ok())
        return {};
    auto servers = parsePlexResources(r.body);
    resolveServerBasesProgressive(get, servers, cancel, onUpdate);
    return servers;
}

std::vector<PlexServer> fetchPlexServers(HttpGetFn get, const std::string& accountToken,
                                         const BrowseCancelFn& cancel) {
    if (accountToken.empty())
        return {};
    const std::string url = appendToken(
        "https://plex.tv/api/resources?includeHttps=1&includeRelay=1", accountToken);
    const BrowseHttpResult r = get(url, 6000, 15000, cancel);
    if (!r.ok())
        return {};
    auto servers = parsePlexResources(r.body);
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
        err = "sections http=" + std::to_string(r.status);
        return false;
    }
    g_apiCache.put(cacheKey, r.body);
    sections = parseLibrarySections(r.body);
    return true;
}

void buildSidebarEntries(const std::vector<PlexServer>& servers,
                         const std::vector<std::vector<PlexSection>>& perServerSections,
                         std::vector<SidebarLibraryEntry>& out) {
    out.clear();
    std::vector<SidebarLibraryEntry> shared;
    std::vector<SidebarLibraryEntry> owned;
    const size_t n = std::min(servers.size(), perServerSections.size());
    for (size_t si = 0; si < n; ++si) {
        const PlexServer& srv = servers[si];
        const std::string sub =
            srv.sourceTitle.empty() ? srv.name : srv.sourceTitle;
        for (const PlexSection& sec : perServerSections[si]) {
            SidebarLibraryEntry e;
            e.title = sec.title;
            e.subtitle = sub;
            e.type = sec.type;
            e.sectionKey = sec.key;
            e.serverIndex = static_cast<int>(si);
            if (srv.owned)
                owned.push_back(std::move(e));
            else
                shared.push_back(std::move(e));
        }
    }
    const auto cmp = [](const SidebarLibraryEntry& a, const SidebarLibraryEntry& b) {
        if (a.title != b.title)
            return a.title < b.title;
        return a.subtitle < b.subtitle;
    };
    std::sort(shared.begin(), shared.end(), cmp);
    std::sort(owned.begin(), owned.end(), cmp);
    out.reserve(shared.size() + owned.size());
    for (auto& e : shared)
        out.push_back(std::move(e));
    for (auto& e : owned)
        out.push_back(std::move(e));
}

int pickHomeServerIndex(const std::vector<PlexServer>& servers, const std::string& preferred) {
    if (!preferred.empty()) {
        for (size_t i = 0; i < servers.size(); ++i) {
            if (servers[i].offline)
                continue;
            const PlexServer& s = servers[i];
            if (s.sourceTitle == preferred || s.name == preferred)
                return static_cast<int>(i);
            for (const auto& c : s.connections) {
                if (c.address == preferred)
                    return static_cast<int>(i);
            }
            if (!s.baseUrl.empty() && s.baseUrl.find(preferred) != std::string::npos)
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

std::vector<std::vector<PlexSection>> fetchAllServerSectionsParallel(
    HttpGetFn get, const std::vector<PlexServer>& servers, const BrowseCancelFn& cancel) {
    std::vector<std::vector<PlexSection>> out(servers.size());
    std::vector<std::future<void>> jobs;
    for (size_t i = 0; i < servers.size(); ++i) {
        jobs.push_back(std::async(std::launch::async, [&, i] {
            if (cancel() || servers[i].baseUrl.empty())   // 0011: an offline server has no sections
                return;
            std::string err;
            fetchLibrarySections(get, servers[i], cancel, out[i], err);
        }));
    }
    for (auto& j : jobs)
        j.get();
    return out;
}

bool fetchMergedSharedContinueWatching(HttpGetFn get, const std::vector<PlexServer>& servers,
                                       const BrowseCancelFn& cancel, PlexHubRow& deck,
                                       std::unordered_map<std::string, int>& itemServerIndex) {
    deck.title = "Continue Watching";
    deck.items.clear();
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
        for (const auto& el : parseXmlStartTags(r.body)) {
            if (el.name == "Video" || el.name == "Movie" || el.name == "Episode") {
                PlexItem it = itemFromElement(el);
                if (!it.ratingKey.empty())
                    itemServerIndex[it.ratingKey] = static_cast<int>(si);
                deck.items.push_back(std::move(it));
            }
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
        g_apiCache.refreshAsync(hubsCacheKey, hubsUrl, get, 4000, 12000, cancel);
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
        PlexHubRow deck;
        deck.title = "Continue Watching";
        for (const auto& el : parseXmlStartTags(r.body)) {
            if (el.name == "Video" || el.name == "Movie" || el.name == "Episode")
                deck.items.push_back(itemFromElement(el));
        }
        if (!deck.items.empty()) {
            bool haveCw = false;
            for (const auto& h : hubs) {
                if (toLower(h.title).find("continue") != std::string::npos)
                    haveCw = true;
            }
            if (!haveCw)
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

#ifdef MPX_PLEXUI_UNIT_TEST
void plexUiApiCacheClear() { g_apiCache.testClear(); }
bool plexUiApiCacheDrain() { return g_apiCache.testDrainRefreshes(); }
#endif

bool fetchSectionGrid(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                      const BrowseCancelFn& cancel, std::vector<PlexItem>& items,
                      std::string& err) {
    // First page only (Cortex-A9 RAM/parse time): 200 items, sorted by title.
    // Key includes the machine id, not only the URL, so two servers cannot share a page.
    std::string url = server.baseUrl + "/library/sections/" + sectionKey +
                      "/all?sort=titleSort&X-Plex-Container-Start=0&X-Plex-Container-Size=200";
    url = appendToken(url, server.accessToken);
    const std::string cacheKey = server.machineIdentifier + "|all|" + sectionKey;
    std::string cached;
    if (g_apiCache.get(cacheKey, cached)) {
        items = parseLibraryAll(cached);
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
    if (!r.ok()) {
        // Chips are best-effort. A timeout or 5xx must not block the items page.
        logLibraryRequest("collections", server, sectionKey, ms, r.status == 404 ? 1 : 0);
        chips.clear();
        (void)err;
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
    if (!r.ok()) {
        logLibraryRequest("genres", server, sectionKey, ms, r.status == 404 ? 1 : 0);
        chips.clear();
        (void)err;
        return true;
    }
    logLibraryRequest("genres", server, sectionKey, ms, 1);
    chips = parseSectionGenres(r.body);
    return true;
}

bool fetchSectionGridGenre(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                           const std::string& genreId, const BrowseCancelFn& cancel,
                           std::vector<PlexItem>& items, std::string& err) {
    std::string url = server.baseUrl + "/library/sections/" + sectionKey + "/all?genre=" +
                      urlEncode(genreId) + "&X-Plex-Container-Start=0&X-Plex-Container-Size=200";
    url = appendToken(url, server.accessToken);
    const BrowseHttpResult r = get(url, 4000, 20000, cancel);
    if (r.cancelled)
        return false;
    if (!r.ok()) {
        err = "grid genre http=" + std::to_string(r.status);
        return false;
    }
    items = parseLibraryAll(r.body);
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

} // namespace plexui
} // namespace misterplex
