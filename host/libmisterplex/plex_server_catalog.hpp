#pragma once
// PLEX_SERVERS picker. Friendly names come from plex.tv /api/v2/resources
// (matched on clientIdentifier / machineIdentifier or connection address:port).
// PLEX_SERVER_NAMES is used only when a resource has no name. Per-server
// tokens come from the resource accessToken, else PLEX_SERVER_TOKENS, else
// the conf token. Tokens stay in memory: never log them, never write them.

#include "libmisterplex/library_browser.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {

struct PlexBrowseTarget {
    std::string name;
    std::string baseUrl;
    std::string token; // memory only
    std::string machineId;
    std::string clientId;
    bool owned = true;
};

struct PlexTvConnection {
    std::string protocol;
    std::string address;
    std::string port;
    std::string uri;
    bool local = false;
    bool relay = false;
};

struct PlexTvDevice {
    std::string name;
    std::string sourceTitle;
    std::string clientIdentifier;
    std::string machineIdentifier;
    std::string accessToken;
    std::string provides;
    bool owned = false;
    std::vector<PlexTvConnection> connections;
};

struct ConfServerRef {
    std::string baseUrl;
    std::string machineId;
};

struct ExplicitServerToken {
    std::string key; // machine id, client id, or base URL
    std::string token;
};

// Optional conf labels: PLEX_SERVER_NAMES=machineId=Living Room,url=Name.
// Used only when plex.tv resources do not already name the server.
struct ExplicitServerName {
    std::string key;
    std::string name;
};

struct PlexResourceCache {
    bool loaded = false;
    std::vector<PlexTvDevice> devices;
};

inline bool plexMachineIdText(const std::string& s) {
    if (s.size() < 32)
        return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c))
            return false;
    }
    return true;
}

inline std::string plexTrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

inline bool splitPlexBase(const std::string& base, std::string& proto, std::string& host,
                          std::string& port) {
    proto = "http";
    host.clear();
    port = "32400";
    std::string s = plexTrim(base);
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        proto = s.substr(0, scheme);
        s = s.substr(scheme + 3);
    }
    const auto slash = s.find('/');
    if (slash != std::string::npos)
        s.resize(slash);
    if (!s.empty() && s.back() == '/')
        s.pop_back();
    if (s.empty())
        return false;
    if (s.front() == '[') {
        const auto end = s.find(']');
        host = end == std::string::npos ? s : s.substr(1, end - 1);
        if (end != std::string::npos && end + 1 < s.size() && s[end + 1] == ':')
            port = s.substr(end + 2);
        return !host.empty();
    }
    const auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = s.substr(colon + 1);
    } else {
        host = s;
    }
    for (char& c : host)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !host.empty();
}

inline std::string plexHostPortKey(const std::string& baseOrHost) {
    std::string proto, host, port;
    if (!splitPlexBase(baseOrHost, proto, host, port))
        return {};
    (void)proto;
    return host + ":" + port;
}

inline std::vector<ConfServerRef> parsePlexServerConf(const std::string& csv) {
    std::vector<ConfServerRef> out;
    std::string cur;
    auto flush = [&] {
        cur = plexTrim(cur);
        if (cur.empty())
            return;
        ConfServerRef ref;
        const auto bar = cur.rfind('|');
        if (bar != std::string::npos && plexMachineIdText(cur.substr(bar + 1))) {
            ref.baseUrl = plexTrim(cur.substr(0, bar));
            ref.machineId = cur.substr(bar + 1);
        } else {
            ref.baseUrl = cur;
        }
        if (!ref.baseUrl.empty())
            out.push_back(std::move(ref));
        cur.clear();
    };
    for (char c : csv) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)))
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return out;
}

inline std::string plexServerConfUrls(const std::vector<ConfServerRef>& refs) {
    std::string out;
    for (const auto& r : refs) {
        if (r.baseUrl.empty())
            continue;
        if (!out.empty())
            out += ',';
        out += r.baseUrl;
    }
    return out;
}

template <class Row>
inline std::vector<Row> parseKeyValueConf(const std::string& conf) {
    std::vector<Row> out;
    std::string cur;
    auto flush = [&] {
        cur = plexTrim(cur);
        if (cur.empty())
            return;
        const auto eq = cur.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 1 < cur.size()) {
            Row t;
            t.key = plexTrim(cur.substr(0, eq));
            t.token = cur.substr(eq + 1);
            if (!t.key.empty() && !t.token.empty())
                out.push_back(std::move(t));
        }
        cur.clear();
    };
    for (char c : conf) {
        if (c == ',' || c == ';' || c == '\n' || c == '\r')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return out;
}

inline std::vector<ExplicitServerToken> parseExplicitServerTokens(const std::string& conf) {
    return parseKeyValueConf<ExplicitServerToken>(conf);
}

inline std::vector<ExplicitServerName> parseExplicitServerNames(const std::string& conf) {
    const auto rows = parseKeyValueConf<ExplicitServerToken>(conf);
    std::vector<ExplicitServerName> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        ExplicitServerName n;
        n.key = row.key;
        n.name = plexTrim(row.token);
        if (!n.key.empty() && !n.name.empty())
            out.push_back(std::move(n));
    }
    return out;
}

namespace {

inline size_t jsonEnd(const std::string& s, size_t i) {
    if (i >= s.size())
        return std::string::npos;
    const char open = s[i];
    if (open != '{' && open != '[')
        return std::string::npos;
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool str = false;
    for (size_t k = i; k < s.size(); ++k) {
        const char c = s[k];
        if (str) {
            if (c == '\\')
                ++k;
            else if (c == '"')
                str = false;
            continue;
        }
        if (c == '"')
            str = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            if (--depth == 0 && c == close)
                return k + 1;
        }
    }
    return std::string::npos;
}

inline std::string jsonUnquote(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out.push_back(raw[i]);
            continue;
        }
        const char e = raw[++i];
        if (e == 'n')
            out.push_back('\n');
        else if (e == 't')
            out.push_back('\t');
        else if (e == 'r')
            out.push_back('\r');
        else
            out.push_back(e);
    }
    return out;
}

inline std::string jsonField(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = obj.find(pat, p)) != std::string::npos) {
        const bool boundary = p == 0 || obj[p - 1] != '\\';
        p += pat.size();
        if (!boundary)
            continue;
        while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p])))
            ++p;
        if (p >= obj.size() || obj[p] != ':')
            continue;
        ++p;
        while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p])))
            ++p;
        if (p >= obj.size())
            return {};
        if (obj[p] == '"') {
            std::string raw;
            for (++p; p < obj.size(); ++p) {
                if (obj[p] == '\\' && p + 1 < obj.size()) {
                    raw.push_back(obj[p]);
                    raw.push_back(obj[++p]);
                    continue;
                }
                if (obj[p] == '"')
                    break;
                raw.push_back(obj[p]);
            }
            return jsonUnquote(raw);
        }
        size_t e = p;
        while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']' &&
               !std::isspace(static_cast<unsigned char>(obj[e])))
            ++e;
        return obj.substr(p, e - p);
    }
    return {};
}

inline bool jsonFlag(const std::string& obj, const char* key) {
    const std::string v = jsonField(obj, key);
    return v == "true" || v == "1";
}

} // namespace

inline std::vector<PlexTvDevice> parsePlexResourcesV2(const std::string& json) {
    std::vector<PlexTvDevice> out;
    const size_t arr = json.find('[');
    if (arr == std::string::npos)
        return out;
    const size_t arrEnd = jsonEnd(json, arr);
    if (arrEnd == std::string::npos)
        return out;
    for (size_t i = json.find('{', arr); i != std::string::npos && i < arrEnd;) {
        const size_t e = jsonEnd(json, i);
        if (e == std::string::npos || e > arrEnd)
            break;
        const std::string obj = json.substr(i, e - i);
        i = json.find('{', e);
        const std::string provides = jsonField(obj, "provides");
        if (!provides.empty() && provides.find("server") == std::string::npos)
            continue;
        const std::string product = jsonField(obj, "product");
        if (!product.empty() && product != "Plex Media Server")
            continue;
        PlexTvDevice d;
        d.name = jsonField(obj, "name");
        d.sourceTitle = jsonField(obj, "sourceTitle");
        d.clientIdentifier = jsonField(obj, "clientIdentifier");
        d.machineIdentifier = jsonField(obj, "machineIdentifier");
        if (d.machineIdentifier.empty())
            d.machineIdentifier = d.clientIdentifier;
        d.accessToken = jsonField(obj, "accessToken");
        d.provides = provides;
        d.owned = jsonFlag(obj, "owned");
        const size_t c = obj.find("\"connections\"");
        if (c != std::string::npos) {
            const size_t ca = obj.find('[', c);
            const size_t ce = ca == std::string::npos ? std::string::npos : jsonEnd(obj, ca);
            if (ca != std::string::npos && ce != std::string::npos) {
                for (size_t q = obj.find('{', ca); q != std::string::npos && q < ce;) {
                    const size_t qe = jsonEnd(obj, q);
                    if (qe == std::string::npos || qe > ce)
                        break;
                    const std::string conn = obj.substr(q, qe - q);
                    q = obj.find('{', qe);
                    PlexTvConnection pc;
                    pc.protocol = jsonField(conn, "protocol");
                    pc.address = jsonField(conn, "address");
                    pc.port = jsonField(conn, "port");
                    pc.uri = jsonField(conn, "uri");
                    pc.local = jsonFlag(conn, "local");
                    pc.relay = jsonFlag(conn, "relay");
                    if (!pc.address.empty() || !pc.uri.empty())
                        d.connections.push_back(std::move(pc));
                }
            }
        }
        if (d.clientIdentifier.empty() && d.machineIdentifier.empty() && d.connections.empty())
            continue;
        out.push_back(std::move(d));
    }
    return out;
}

template <class Fetch>
inline void loadPlexResourceCache(PlexResourceCache& cache, const std::string& token, Fetch fetch) {
    if (cache.loaded)
        return;
    cache.loaded = true;
    if (token.empty())
        return;
    const std::string body = fetch();
    cache.devices = parsePlexResourcesV2(body);
}

inline const PlexTvDevice* matchPlexDevice(const std::vector<PlexTvDevice>& devices,
                                           const std::string& machineId, const std::string& baseUrl) {
    const std::string want = plexHostPortKey(baseUrl);
    if (!machineId.empty()) {
        for (const auto& d : devices) {
            if (d.clientIdentifier == machineId || d.machineIdentifier == machineId)
                return &d;
        }
    }
    if (want.empty())
        return nullptr;
    for (const auto& d : devices) {
        for (const auto& c : d.connections) {
            // A plex.direct uri does not equal a plain-IP PLEX_SERVERS base.
            // Match either the uri or address:port.
            const std::string addr = plexHostPortKey(c.address + ":" + (c.port.empty() ? "32400" : c.port));
            const std::string uri = c.uri.empty() ? std::string() : plexHostPortKey(c.uri);
            if ((!addr.empty() && addr == want) || (!uri.empty() && uri == want))
                return &d;
        }
    }
    return nullptr;
}

inline std::string explicitTokenFor(const std::vector<ExplicitServerToken>& rows, const std::string& machineId,
                                    const std::string& clientId, const std::string& baseUrl) {
    const std::string host = plexHostPortKey(baseUrl);
    for (const auto& row : rows) {
        if (row.token.empty())
            continue;
        if (!machineId.empty() && row.key == machineId)
            return row.token;
        if (!clientId.empty() && row.key == clientId)
            return row.token;
        if (!baseUrl.empty() && (row.key == baseUrl || plexHostPortKey(row.key) == host))
            return row.token;
    }
    return {};
}

inline std::string explicitNameFor(const std::vector<ExplicitServerName>& rows, const std::string& machineId,
                                   const std::string& clientId, const std::string& baseUrl) {
    const std::string host = plexHostPortKey(baseUrl);
    for (const auto& row : rows) {
        if (row.name.empty())
            continue;
        if (!machineId.empty() && row.key == machineId)
            return row.name;
        if (!clientId.empty() && row.key == clientId)
            return row.name;
        if (!baseUrl.empty() && (row.key == baseUrl || plexHostPortKey(row.key) == host))
            return row.name;
    }
    return {};
}

// Resource name wins. Conf PLEX_SERVER_NAMES fills in only when resources have no name.
inline std::string plexServerLabel(const PlexTvDevice* dev, const std::string& machineId,
                                   const std::string& baseUrl,
                                   const std::vector<ExplicitServerName>& explicitNames) {
    if (dev && !dev->name.empty())
        return dev->name;
    if (dev && !dev->sourceTitle.empty())
        return dev->sourceTitle;
    const std::string id = dev ? dev->clientIdentifier : std::string();
    const std::string mapped = explicitNameFor(explicitNames, machineId, id, baseUrl);
    if (!mapped.empty())
        return mapped;
    const std::string host = plexHostPortKey(baseUrl);
    return host.empty() ? baseUrl : host;
}

inline std::vector<PlexBrowseTarget> resolvePlexServerCatalog(
    const std::vector<std::string>& bases, const std::vector<ConfServerRef>& refs,
    const std::vector<PlexTvDevice>& devices, const std::vector<ExplicitServerToken>& explicitTokens,
    const std::string& fallbackToken, const std::vector<ExplicitServerName>& explicitNames = {}) {
    std::vector<PlexBrowseTarget> out;
    for (const auto& base : bases) {
        if (base.empty())
            continue;
        std::string machineId;
        const std::string host = plexHostPortKey(base);
        for (const auto& ref : refs) {
            if (!ref.machineId.empty() && plexHostPortKey(ref.baseUrl) == host) {
                machineId = ref.machineId;
                break;
            }
        }
        const PlexTvDevice* dev = matchPlexDevice(devices, machineId, base);
        PlexBrowseTarget t;
        t.baseUrl = base;
        if (!t.baseUrl.empty() && t.baseUrl.back() == '/')
            t.baseUrl.pop_back();
        if (dev) {
            t.clientId = dev->clientIdentifier;
            if (machineId.empty())
                machineId = !dev->machineIdentifier.empty() ? dev->machineIdentifier : dev->clientIdentifier;
            t.owned = dev->owned;
        }
        t.machineId = machineId;
        t.name = plexServerLabel(dev, machineId, base, explicitNames);
        if (dev && !dev->accessToken.empty())
            t.token = dev->accessToken;
        else {
            const std::string id = dev ? dev->clientIdentifier : std::string();
            t.token = explicitTokenFor(explicitTokens, machineId, id, base);
            if (t.token.empty())
                t.token = fallbackToken;
        }
        out.push_back(std::move(t));
    }
    return out;
}

// Rows for the legacy library list and the browse-v2 root. Tokens are not copied onto the row.
inline std::vector<LibraryRow> plexServerPickerRows(const std::vector<PlexBrowseTarget>& servers) {
    std::vector<LibraryRow> rows;
    for (size_t i = 0; i < servers.size(); ++i) {
        LibraryRow r;
        r.title = servers[i].name.empty() ? servers[i].baseUrl : servers[i].name;
        r.type = "plex-server";
        r.key = std::to_string(i);
        r.directory = true;
        r.ratingKey = servers[i].machineId;
        rows.push_back(std::move(r));
    }
    return rows;
}

inline bool plexServerPickerRow(const LibraryRow& row) { return row.type == "plex-server"; }

} // namespace misterplex
