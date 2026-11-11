#pragma once
// After a plex.tv link, pick the account's PMS from /api/v2/resources.
// Owned servers come first. On a server, a local connection comes before a
// remote one, and a relay is last. The result is a base URL with no token.

#include "tls_policy.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace misterplex {

struct LinkConnection {
    std::string uri;
    std::string protocol;
    std::string address;
    std::string port;
    bool local = false;
    bool relay = false;
};

struct LinkDevice {
    std::string name;
    std::string clientIdentifier;
    std::string machineIdentifier;
    std::string accessToken;
    bool owned = false;
    bool presence = false;
    bool server = false;
    std::vector<LinkConnection> connections;
};

inline std::string linkBaseForLog(std::string base) {
    const auto q = base.find('?');
    if (q != std::string::npos)
        base.resize(q);
    return base;
}

namespace link_discover_detail {

inline void skipWs(const std::string& s, std::size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
}

inline std::string jsonStringAt(const std::string& s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"')
        return {};
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            out.push_back(s[i + 1]);
            i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    if (i < s.size() && s[i] == '"')
        ++i;
    return out;
}

// Value of `key` at brace depth 1 (the object's own fields, not nested ones).
inline bool jsonFieldAtDepth(const std::string& obj, const std::string& key, std::string& value,
                             bool& isString) {
    int depth = 0;
    for (std::size_t i = 0; i < obj.size(); ++i) {
        const char c = obj[i];
        if (c == '"') {
            std::size_t j = i;
            const std::string lit = jsonStringAt(obj, j);
            if (depth == 1 && lit == key) {
                std::size_t k = j;
                skipWs(obj, k);
                if (k < obj.size() && obj[k] == ':') {
                    ++k;
                    skipWs(obj, k);
                    if (k < obj.size() && obj[k] == '"') {
                        value = jsonStringAt(obj, k);
                        isString = true;
                        return true;
                    }
                    std::size_t e = k;
                    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']')
                        ++e;
                    value = obj.substr(k, e - k);
                    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                        value.pop_back();
                    isString = false;
                    return true;
                }
            }
            i = j - 1;
            continue;
        }
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
            --depth;
    }
    return false;
}

inline std::string jsonStringField(const std::string& obj, const std::string& key) {
    std::string v;
    bool str = false;
    if (!jsonFieldAtDepth(obj, key, v, str) || !str)
        return {};
    return v;
}

inline bool jsonBoolField(const std::string& obj, const std::string& key) {
    std::string v;
    bool str = false;
    if (!jsonFieldAtDepth(obj, key, v, str))
        return false;
    if (str)
        return v == "1" || v == "true";
    return v == "true" || v == "1";
}

inline std::string jsonRawField(const std::string& obj, const std::string& key) {
    int depth = 0;
    for (std::size_t i = 0; i < obj.size(); ++i) {
        const char c = obj[i];
        if (c == '"') {
            std::size_t j = i;
            const std::string lit = jsonStringAt(obj, j);
            if (depth == 1 && lit == key) {
                std::size_t k = j;
                skipWs(obj, k);
                if (k >= obj.size() || obj[k] != ':') {
                    i = j - 1;
                    continue;
                }
                ++k;
                skipWs(obj, k);
                if (k >= obj.size())
                    return {};
                if (obj[k] != '{' && obj[k] != '[')
                    return {};
                const char open = obj[k];
                const char close = open == '{' ? '}' : ']';
                int d = 0;
                for (std::size_t e = k; e < obj.size(); ++e) {
                    if (obj[e] == '"') {
                        jsonStringAt(obj, e);
                        --e;
                        continue;
                    }
                    if (obj[e] == open || obj[e] == '{' || obj[e] == '[')
                        ++d;
                    else if (obj[e] == close || obj[e] == '}' || obj[e] == ']') {
                        --d;
                        if (d == 0)
                            return obj.substr(k, e - k + 1);
                    }
                }
            }
            i = j - 1;
            continue;
        }
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
            --depth;
    }
    return {};
}

inline std::vector<std::string> splitTopObjects(const std::string& arrayOrBody) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i < arrayOrBody.size(); ++i) {
        if (arrayOrBody[i] == '"') {
            jsonStringAt(arrayOrBody, i);
            --i;
            continue;
        }
        if (arrayOrBody[i] == '{') {
            if (depth == 0)
                start = i;
            ++depth;
        } else if (arrayOrBody[i] == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                out.push_back(arrayOrBody.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

// Double or single quotes. A longer attribute (username=) is not "name".
inline std::string xmlAttr(const std::string& tag, const std::string& key) {
    const std::string pat = key + "=";
    std::size_t at = 0;
    while ((at = tag.find(pat, at)) != std::string::npos) {
        if (at > 0) {
            const unsigned char prev = static_cast<unsigned char>(tag[at - 1]);
            if (std::isalnum(prev) || prev == '_' || prev == '-' || prev == ':') {
                at += pat.size();
                continue;
            }
        }
        std::size_t b = at + pat.size();
        if (b >= tag.size())
            return {};
        const char q = tag[b];
        if (q != '"' && q != '\'') {
            at += pat.size();
            continue;
        }
        ++b;
        const auto e = tag.find(q, b);
        if (e == std::string::npos)
            return {};
        return tag.substr(b, e - b);
    }
    return {};
}

// plex.tv XML uses owned="1". A JSON-shaped dump uses owned="true".
inline bool xmlTrue(const std::string& v) {
    return v == "1" || v == "true" || v == "True" || v == "TRUE";
}

// First byte of the resources body after a UTF-8 BOM and whitespace.
// '<' is XML (what plex.tv returns for this URL even when Accept is JSON).
// '{' or '[' is JSON. 0 is empty.
inline char resourcesLead(const std::string& body, std::size_t& i) {
    i = 0;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
        i = 3;
    skipWs(body, i);
    if (i >= body.size())
        return 0;
    return body[i];
}

inline std::string connectionUri(const LinkConnection& c) {
    if (!c.uri.empty())
        return c.uri;
    if (c.protocol.empty() || c.address.empty())
        return {};
    const std::string port = c.port.empty() ? "32400" : c.port;
    return c.protocol + "://" + c.address + ":" + port;
}

inline bool connectionIsHttpsBareIp(const LinkConnection& c) {
    return httpsBareIpv4(connectionUri(c));
}

// A raw https://IP is not a name on the plex.direct certificate. When the
// device id is a DNS label, use https://a-b-c-d.<id>.plex.direct so TLS
// checks that name and connects to the IP. MUT_BARE_IP_HTTPS_KEPT leaves the
// raw IP in place.
inline std::string verifiableConnectionUri(const LinkConnection& c, const std::string& clientId) {
    const std::string uri = connectionUri(c);
#ifdef MUT_BARE_IP_HTTPS_KEPT
    (void)clientId;
    return uri;
#else
    if (!connectionIsHttpsBareIp(c))
        return uri;
    const std::string fixed = plexDirectBaseForIpv4Https(uri, clientId);
    return fixed.empty() ? uri : fixed;
#endif
}

inline int connectionRank(const LinkConnection& c, const std::string& clientId) {
    if (connectionUri(c).empty())
        return 100;
    if (c.relay)
        return 40;
    // An https IP we cannot rewrite fails the certificate check. Prefer any
    // plex.direct (or http) connection on the same server over it.
    if (connectionIsHttpsBareIp(c) && !plexMachineLabelOk(clientId))
        return c.local ? 30 : 31;
    if (c.local && c.protocol == "http")
        return 0;
    if (c.local)
        return 1;
    if (c.protocol == "https")
        return 3;
    return 2;
}

inline std::string bestUriForDevice(const LinkDevice& d) {
    if (!d.server)
        return {};
    int best = 100;
    std::string uri;
    for (const LinkConnection& c : d.connections) {
        const int rank = connectionRank(c, d.clientIdentifier);
        if (rank < best) {
            best = rank;
            uri = verifiableConnectionUri(c, d.clientIdentifier);
        }
    }
    return uri;
}

inline std::string bestBase(const std::vector<LinkDevice>& devices) {
    const LinkDevice* chosen = nullptr;
    int best = 100;
    std::string uri;
    auto consider = [&](const LinkDevice& d) {
        if (!d.server)
            return;
        for (const LinkConnection& c : d.connections) {
            const int rank = connectionRank(c, d.clientIdentifier);
            if (rank < best) {
                best = rank;
                chosen = &d;
                uri = verifiableConnectionUri(c, d.clientIdentifier);
            }
        }
    };
    for (const LinkDevice& d : devices)
        if (d.owned)
            consider(d);
    if (!chosen) {
        for (const LinkDevice& d : devices)
            consider(d);
    }
    return uri;
}

inline std::vector<LinkDevice> parseJsonResources(const std::string& body) {
    std::vector<LinkDevice> devices;
    for (const std::string& obj : splitTopObjects(body)) {
        LinkDevice d;
        const std::string provides = jsonStringField(obj, "provides");
        const std::string product = jsonStringField(obj, "product");
        d.server = provides.find("server") != std::string::npos && product == "Plex Media Server";
        d.owned = jsonBoolField(obj, "owned");
        d.name = jsonStringField(obj, "name");
        d.clientIdentifier = jsonStringField(obj, "clientIdentifier");
        d.machineIdentifier = jsonStringField(obj, "machineIdentifier");
        d.accessToken = jsonStringField(obj, "accessToken");
        const std::string conns = jsonRawField(obj, "connections");
        for (const std::string& cobj : splitTopObjects(conns)) {
            LinkConnection c;
            c.protocol = jsonStringField(cobj, "protocol");
            c.address = jsonStringField(cobj, "address");
            c.port = jsonStringField(cobj, "port");
            if (c.port.empty()) {
                std::string raw;
                bool str = false;
                if (jsonFieldAtDepth(cobj, "port", raw, str) && !str)
                    c.port = raw;
            }
            c.uri = jsonStringField(cobj, "uri");
            c.local = jsonBoolField(cobj, "local");
            c.relay = jsonBoolField(cobj, "relay");
            d.connections.push_back(std::move(c));
        }
        devices.push_back(std::move(d));
    }
    return devices;
}

// Element names only. "resources" and "connections" are wrappers, not devices.
inline std::string xmlElementKey(std::string name) {
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

inline std::vector<LinkDevice> parseXmlResources(const std::string& body) {
    std::vector<LinkDevice> devices;
    LinkDevice* cur = nullptr;
    std::size_t i = 0;
    while (i < body.size()) {
        const auto lt = body.find('<', i);
        if (lt == std::string::npos)
            break;
        const auto gt = body.find('>', lt);
        if (gt == std::string::npos)
            break;
        std::string tag = body.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '/' || tag[0] == '?' || tag[0] == '!')
            continue;
        if (!tag.empty() && tag.back() == '/')
            tag.pop_back();
        const auto sp = tag.find_first_of(" \t\r\n");
        const std::string name = sp == std::string::npos ? tag : tag.substr(0, sp);
        // /api/resources uses <Device>/<Connection>. /api/v2/resources uses
        // <resource>/<connection> inside <resources>/<connections>.
#ifdef MUT_LINK_XML_DEVICE_ONLY   // test-only: legacy <Device> tags, so a v2 body is empty
        const bool resource = name == "Device";
        const bool connection = name == "Connection";
#else
        const std::string key = xmlElementKey(name);
        const bool resource = key == "device" || key == "resource";
        const bool connection = key == "connection";
#endif
        if (resource) {
            LinkDevice d;
            const std::string provides = xmlAttr(tag, "provides");
            d.server = provides.find("server") != std::string::npos &&
                       xmlAttr(tag, "product") == "Plex Media Server";
            d.owned = xmlTrue(xmlAttr(tag, "owned"));
            d.presence = xmlTrue(xmlAttr(tag, "presence"));
            d.name = xmlAttr(tag, "name");
            d.clientIdentifier = xmlAttr(tag, "clientIdentifier");
            d.machineIdentifier = xmlAttr(tag, "machineIdentifier");
            d.accessToken = xmlAttr(tag, "accessToken");
            devices.push_back(std::move(d));
            cur = &devices.back();
            continue;
        }
        if (connection && cur) {
            LinkConnection c;
            c.protocol = xmlAttr(tag, "protocol");
            c.address = xmlAttr(tag, "address");
            c.port = xmlAttr(tag, "port");
            c.uri = xmlAttr(tag, "uri");
            c.local = xmlTrue(xmlAttr(tag, "local"));
            c.relay = xmlTrue(xmlAttr(tag, "relay"));
            cur->connections.push_back(std::move(c));
        }
    }
    return devices;
}

} // namespace link_discover_detail

// Empty when the body has no usable PMS connection. Does not read or return a token.
inline std::string chooseLinkedServerBase(const std::string& body) {
#ifdef MUT_LINK_DISCOVER_SKIP
    (void)body;
    return {};
#else
    if (body.empty())
        return {};
    std::size_t i = 0;
    const char lead = link_discover_detail::resourcesLead(body, i);
    if (lead == 0)
        return {};
    const auto devices = lead == '<' ? link_discover_detail::parseXmlResources(body)
                                     : link_discover_detail::parseJsonResources(body);
    return link_discover_detail::bestBase(devices);
#endif
}

// One PMS from plex.tv /api/v2/resources, for a server picker.
// name, machineIdentifier, uri, and owned are safe to show and to log.
// accessToken is that server's resources token. Browse and play send it as
// X-Plex-Token. It is not part of the log line. Do not write it to a log,
// a URL query, or a status string.
struct LinkedServer {
    std::string name;
    std::string machineIdentifier;
    std::string uri;
    bool owned = false;
    std::string accessToken;
};

inline std::string canonicalServerUri(std::string uri) {
    const auto q = uri.find('?');
    if (q != std::string::npos)
        uri.resize(q);
    while (uri.size() > 1 && uri.back() == '/')
        uri.pop_back();
    return uri;
}

// Log line: name, machine id, uri, owned. The per-server token is omitted.
inline std::string linkedServerLogLine(const LinkedServer& s) {
    return "name=" + s.name + " machine=" + s.machineIdentifier + " uri=" + canonicalServerUri(s.uri) +
           " owned=" + (s.owned ? "1" : "0");
}

// GH#366: burn PUT owners come from the same plex.tv roster as the picker
// (owned=1 machine ids + uris). Catalog JSON can be empty while this XML/JSON
// roster already lists owned servers.
inline void ownedBurnIdsHosts(const std::vector<LinkedServer>& roster, std::vector<std::string>& ids,
                              std::vector<std::string>& hosts) {
#ifdef MUT_324_LINKED_OWNERS_SKIP
    (void)roster;
    (void)ids;
    (void)hosts;
#else
    for (const auto& s : roster) {
        if (!s.owned)
            continue;
        if (!s.machineIdentifier.empty())
            ids.push_back(s.machineIdentifier);
        if (!s.uri.empty())
            hosts.push_back(canonicalServerUri(s.uri));
    }
#endif
}

inline bool sameServerUri(const std::string& a, const std::string& b) {
    if (canonicalServerUri(a) == canonicalServerUri(b))
        return true;
    TlsUrl ua, ub;
    if (!parseTlsUrl(a, ua) || !parseTlsUrl(b, ub) || ua.port != ub.port)
        return false;
    const std::string ha = ua.connectHost.empty() ? ua.host : ua.connectHost;
    const std::string hb = ub.connectHost.empty() ? ub.host : ub.connectHost;
    return !ha.empty() && ha == hb;
}

// "xml", "json", "empty", or "other". Safe to log: it is not the body.
inline std::string resourcesBodyFormat(const std::string& body) {
    std::size_t i = 0;
    const char lead = link_discover_detail::resourcesLead(body, i);
    if (lead == 0)
        return "empty";
    if (lead == '<')
        return "xml";
    if (lead == '{' || lead == '[')
        return "json";
    return "other";
}

// Every PMS in the resources body, in body order. uri is that server's best
// connection (local http, then a verifiable https name, relay last). The
// per-server accessToken stays on the struct and nowhere else.
// XML and JSON are both accepted. plex.tv /api/v2/resources answers with
// lowercase <resource> XML even when the request asks for JSON. The legacy
// /api/resources <Device> document is the same fields.
inline std::vector<LinkedServer> linkedServersFromResources(const std::string& body) {
#ifdef MUT_LINK_DISCOVER_SKIP
    (void)body;
    return {};
#else
    if (body.empty())
        return {};
    std::size_t i = 0;
    const char lead = link_discover_detail::resourcesLead(body, i);
    if (lead == 0)
        return {};
#ifdef MUT_LINK_XML_SKIP   // test-only: treat an XML resources body as no servers
    if (lead == '<')
        return {};
#endif
    const auto devices = lead == '<' ? link_discover_detail::parseXmlResources(body)
                                     : link_discover_detail::parseJsonResources(body);
    std::vector<LinkedServer> out;
    for (const LinkDevice& d : devices) {
        if (!d.server)
            continue;
        LinkedServer s;
        s.name = d.name;
        s.machineIdentifier = !d.machineIdentifier.empty() ? d.machineIdentifier : d.clientIdentifier;
        s.uri = link_discover_detail::bestUriForDevice(d);
        s.owned = d.owned;
        s.accessToken = d.accessToken;
        if (s.uri.empty() && s.machineIdentifier.empty())
            continue;
        out.push_back(std::move(s));
    }
    return out;
#endif
}

inline std::string accessTokenForServer(const std::vector<LinkedServer>& servers, const std::string& uri) {
    for (const LinkedServer& s : servers) {
        if (sameServerUri(s.uri, uri))
            return s.accessToken;
    }
    return {};
}

inline const LinkedServer* linkedServerByMachine(const std::vector<LinkedServer>& servers,
                                                 const std::string& machineIdentifier) {
    if (machineIdentifier.empty())
        return nullptr;
    for (const LinkedServer& s : servers) {
        if (s.machineIdentifier == machineIdentifier)
            return &s;
    }
    return nullptr;
}

// Browse and play. Linked: that server's resources accessToken (not the
// plex.tv account token, and not PLEX_TOKEN). Not linked: PLEX_TOKEN.
// MUT_LINK_TOKEN_IGNORED always returns PLEX_TOKEN.
inline std::string libraryToken(bool linked, const std::string& serverAccessToken, const std::string& plexToken) {
#ifdef MUT_LINK_TOKEN_IGNORED
    (void)linked;
    (void)serverAccessToken;
    return plexToken;
#else
    if (linked)
        return serverAccessToken;
    return plexToken;
#endif
}

// Linked browse/play token for the server the picker selected.
// machineId first: the picker's address is often not the resources "best" uri
// (another NIC, or https vs the local http connection). Then the address.
// Then the picker's own token. MUT_LINK_TOKEN_SKIP_MACHINE skips the machine
// id and fails when the address does not match.
inline std::string credentialForLinkedTarget(const std::vector<LinkedServer>& roster,
                                            const std::string& machineId, const std::string& address,
                                            const std::string& pickerToken, const std::string& plexToken) {
    std::string serverTok;
#ifndef MUT_LINK_TOKEN_SKIP_MACHINE
    if (const LinkedServer* byId = linkedServerByMachine(roster, machineId)) {
        if (!byId->accessToken.empty())
            serverTok = byId->accessToken;
    }
#endif
    if (serverTok.empty())
        serverTok = accessTokenForServer(roster, address);
    if (serverTok.empty())
        serverTok = pickerToken;
    return libraryToken(true, serverTok, plexToken);
}

// Browse GET. The token is the X-Plex-Token header. The URL never carries it.
// MUT_LINK_TOKEN_IN_URL puts it on the query instead, and the header test fails.
struct LibraryFetchPlan {
    std::string url;
    std::string headerToken;
};

inline LibraryFetchPlan planLibraryFetch(const std::string& base, const std::string& path,
                                        const std::string& token) {
    LibraryFetchPlan f;
    f.url = base;
    if (!path.empty() && path[0] == '/')
        f.url += path;
    else if (!path.empty())
        f.url += "/" + path;
#ifdef MUT_LINK_TOKEN_IN_URL
    if (!token.empty()) {
        f.url += (f.url.find('?') == std::string::npos ? "?" : "&");
        f.url += "X-Plex-Token=" + token;
    }
#else
    f.headerToken = token;
#endif
    return f;
}

// When PLEX_BASE is https://<ipv4>, swap in the matching plex.direct name from
// the account's resources. Empty when there is nothing to change. The result
// has no token. MUT_BARE_IP_HTTPS_KEPT leaves the raw IP alone.
inline std::string repairBareIpHttpsBase(const std::string& configured, const std::string& body) {
#ifdef MUT_BARE_IP_HTTPS_KEPT
    (void)configured;
    (void)body;
    return {};
#else
    if (!httpsBareIpv4(configured) || body.empty())
        return {};
    TlsUrl cfg;
    if (!parseTlsUrl(configured, cfg))
        return {};
    std::size_t i = 0;
    const char lead = link_discover_detail::resourcesLead(body, i);
    if (lead == 0)
        return {};
    const auto devices = lead == '<' ? link_discover_detail::parseXmlResources(body)
                                     : link_discover_detail::parseJsonResources(body);
    std::string uri;
    bool found = false;
    auto consider = [&](const LinkDevice& d) {
        if (found)
            return;
        for (const LinkConnection& c : d.connections) {
            if (c.relay)
                continue;
            const std::string raw = link_discover_detail::connectionUri(c);
            TlsUrl u;
            if (!parseTlsUrl(raw, u) || u.port != cfg.port)
                continue;
            const bool sameIp = u.host == cfg.host || (u.plexDirect && u.connectHost == cfg.host) ||
                                c.address == cfg.host;
            if (!sameIp)
                continue;
            const std::string next = link_discover_detail::verifiableConnectionUri(c, d.clientIdentifier);
            TlsUrl got;
            if (!parseTlsUrl(next, got) || !got.plexDirect)
                continue;
            uri = next;
            found = true;
            return;
        }
    };
    for (const LinkDevice& d : devices)
        if (d.owned)
            consider(d);
    if (!found) {
        for (const LinkDevice& d : devices)
            if (!d.owned)
                consider(d);
    }
    if (!found || uri == configured)
        return {};
    return uri;
#endif
}

} // namespace misterplex
