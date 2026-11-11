#pragma once
// TLS for Plex, PlexUI, YouTube, and gcast. Verification is on unless
// TLS_VERIFY=0 (or MPX_TLS_VERIFY=0). Chain and hostname are never the part
// that gets skipped: a 1970 clock skips only X509_V_FLAG_NO_CHECK_TIME, and
// only after the clock rescue has already run.
//
// plex.direct certificates are wildcards for *.<id>.plex.direct. The first
// label is the server's IPv4 with dots written as dashes (10-1-2-3.<id>.plex.direct).
// TCP goes to that address; the name we verify is the full plex.direct host, so a
// local server matches without plex.direct DNS and without curl -k.
//
// A server that is genuinely self-signed is not a plex.direct name. Put its PEM
// (the server certificate, or the CA that signed it) in TLS_TRUST_FILE. That file
// is added to the Mozilla bundle. It is not a substitute for TLS_VERIFY=0.

#include "../../arm/misterplexd/ca_bundle.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace misterplex {

inline constexpr const char* kTlsOptOutWarning =
    "misterplexd: WARNING: TLS_VERIFY=0 turns off certificate chain and hostname "
    "checks for Plex, PlexUI, YouTube, and gcast. Connections are not authenticated.";

inline constexpr const char* kShippedCaBundle = "/media/fat/linux/misterplex-cacert.pem";

inline bool tlsTokenOff(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return s == "0" || s == "off" || s == "false" || s == "no";
}

inline bool tlsFileNonEmpty(const char* path) {
    struct stat st{};
    return path && *path && ::stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// Same order as mpx::caBundlePath(), including the two stock MiSTer files.
inline std::string tlsCaBundlePath() {
    return mpx::caBundlePath();
}

// When the shipped bundle is on the card and the operator did not override it,
// publish that path as MPX_CA_BUNDLE so curl and child processes use the same file.
inline void publishDefaultCaBundle() {
    const char* env = std::getenv("MPX_CA_BUNDLE");
    if (env && *env)
        return;
    if (tlsFileNonEmpty(kShippedCaBundle))
        ::setenv("MPX_CA_BUNDLE", kShippedCaBundle, 0);
}

inline std::string tlsConfPath() {
    if (const char* e = std::getenv("MPX_TLS_CONF"))
        if (*e)
            return e;
    return "/media/fat/misterplex/misterplex.conf";
}

inline std::string tlsConfValue(const char* key) {
    if (!key || !*key)
        return {};
    std::ifstream in(tlsConfPath());
    if (!in)
        return {};
    const std::string prefix = std::string(key) + "=";
    std::string line, found;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.compare(0, prefix.size(), prefix) == 0)
            found = line.substr(prefix.size());
    }
    return found;
}

// Env wins over the conf file, matching setenv(..., 0) in main.
inline bool tlsVerifyOff() {
    if (const char* e = std::getenv("MPX_TLS_VERIFY"))
        return tlsTokenOff(e);
    return tlsTokenOff(tlsConfValue("TLS_VERIFY"));
}

inline std::string tlsTrustFile() {
    if (const char* e = std::getenv("MPX_TLS_TRUST_FILE")) {
        if (tlsFileNonEmpty(e))
            return e;
        if (*e)
            return {};
    }
    const std::string conf = tlsConfValue("TLS_TRUST_FILE");
    return tlsFileNonEmpty(conf.c_str()) ? conf : std::string{};
}

inline void logTlsOptOutOnce() {
    static std::atomic<int> once{0};
    if (once.exchange(1))
        return;
    std::fprintf(stderr, "%s\n", kTlsOptOutWarning);
}

struct TlsUrl {
    bool ok = false;
    bool https = false;
    std::string host;         // name passed to SNI and the hostname check
    std::string connectHost;  // numeric IPv4 when the plex.direct label encodes one
    int port = 0;
    std::string pathQuery;    // begins with '/'
    bool plexDirect = false;
};

inline bool dashedIpv4Label(const std::string& label, std::string& dotted) {
    int o[4] = {-1, -1, -1, -1};
    int idx = 0;
    int cur = -1;
    for (size_t i = 0; i <= label.size(); ++i) {
        const char c = i < label.size() ? label[i] : '-';
        if (c >= '0' && c <= '9') {
            if (cur < 0)
                cur = 0;
            cur = cur * 10 + (c - '0');
            if (cur > 255)
                return false;
        } else if (c == '-') {
            if (cur < 0 || idx >= 4)
                return false;
            o[idx++] = cur;
            cur = -1;
        } else {
            return false;
        }
    }
    if (idx != 4)
        return false;
    dotted = std::to_string(o[0]) + "." + std::to_string(o[1]) + "." + std::to_string(o[2]) + "." +
             std::to_string(o[3]);
    return true;
}

inline bool parseTlsUrl(const std::string& url, TlsUrl& out) {
    out = TlsUrl{};
    const auto scheme = url.find("://");
    if (scheme == std::string::npos || scheme == 0)
        return false;
    std::string sch = url.substr(0, scheme);
    for (char& c : sch)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (sch != "https" && sch != "http")
        return false;
    out.https = sch == "https";
    size_t i = scheme + 3;
    if (i >= url.size())
        return false;
    const size_t slash = url.find('/', i);
    std::string hostport = slash == std::string::npos ? url.substr(i) : url.substr(i, slash - i);
    out.pathQuery = slash == std::string::npos ? "/" : url.substr(slash);
    if (out.pathQuery.empty())
        out.pathQuery = "/";
    const auto at = hostport.rfind('@');
    if (at != std::string::npos)
        hostport = hostport.substr(at + 1);
    if (hostport.empty())
        return false;
    if (hostport[0] == '[') {
        const auto rb = hostport.find(']');
        if (rb == std::string::npos)
            return false;
        out.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':')
            out.port = std::atoi(hostport.c_str() + rb + 2);
    } else {
        const auto colon = hostport.rfind(':');
        if (colon != std::string::npos && hostport.find(':') == colon) {
            out.host = hostport.substr(0, colon);
            out.port = std::atoi(hostport.c_str() + colon + 1);
        } else {
            out.host = hostport;
        }
    }
    if (out.host.empty())
        return false;
    if (out.port <= 0)
        out.port = out.https ? 443 : 80;
    std::string low = out.host;
    for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string suffix = ".plex.direct";
    if (low.size() > suffix.size() && low.compare(low.size() - suffix.size(), suffix.size(), suffix) == 0) {
        out.plexDirect = true;
        const auto dot = out.host.find('.');
        std::string ip;
        if (dot != std::string::npos && dashedIpv4Label(out.host.substr(0, dot), ip))
            out.connectHost = ip;
    }
    out.ok = true;
    return true;
}

struct TlsCurlPlan {
    bool https = false;
    bool optOut = false;
    std::string caFile;
    std::string extraTrust;
    std::string resolveHost;
    int resolvePort = 0;
    std::string resolveAddr;
    std::string warning;
};

inline TlsCurlPlan planTlsCurl(const std::string& url) {
    TlsCurlPlan p;
    TlsUrl u;
    if (!parseTlsUrl(url, u) || !u.https)
        return p;
    p.https = true;
    p.optOut = tlsVerifyOff();
    if (p.optOut) {
        p.warning = kTlsOptOutWarning;
        return p;
    }
    p.caFile = tlsCaBundlePath();
    p.extraTrust = tlsTrustFile();
    if (!u.connectHost.empty()) {
        p.resolveHost = u.host;
        p.resolvePort = u.port;
        p.resolveAddr = u.connectHost;
    }
    return p;
}

// curl --cacert replaces the default store, so a per-server PEM has to sit in
// the same file as the Mozilla roots. OpenSSL callers load the two files
// separately and do not need this.
inline std::string materializeTrustBundle(const std::string& ca, const std::string& extra) {
    if (extra.empty())
        return ca;
    if (ca.empty())
        return extra;
    const std::string path = "/tmp/misterplex-trust-bundle.pem";
    std::ifstream a(ca, std::ios::binary);
    std::ifstream b(extra, std::ios::binary);
    if (!a || !b)
        return ca;
    std::ostringstream body;
    body << a.rdbuf() << "\n" << b.rdbuf() << "\n";
    const std::string merged = body.str();
    std::ifstream cur(path, std::ios::binary);
    std::ostringstream have;
    if (cur)
        have << cur.rdbuf();
    if (have.str() != merged) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return ca;
        out << merged;
    }
    return path;
}

// Four dotted octets, 0-255. `dashed` is the same address with dots written as dashes.
inline bool ipv4LiteralHost(const std::string& host, std::string& dashed) {
    dashed.clear();
    int octet = -1;
    int count = 0;
    std::string out;
    auto flush = [&]() -> bool {
        if (octet < 0 || octet > 255 || count >= 4)
            return false;
        if (!out.empty())
            out.push_back('-');
        out += std::to_string(octet);
        ++count;
        octet = -1;
        return true;
    };
    for (size_t i = 0; i <= host.size(); ++i) {
        const char c = i < host.size() ? host[i] : '.';
        if (c >= '0' && c <= '9') {
            if (octet < 0)
                octet = 0;
            octet = octet * 10 + (c - '0');
            if (octet > 255)
                return false;
        } else if (c == '.') {
            if (!flush())
                return false;
        } else {
            return false;
        }
    }
    if (count != 4)
        return false;
    dashed = out;
    return true;
}

// Plex's machine id is one DNS label on *.<id>.plex.direct. Hex or a UUID.
inline bool plexMachineLabelOk(const std::string& id) {
    if (id.size() < 8 || id.size() > 80 || id.front() == '-' || id.back() == '-')
        return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '-')
            return false;
    }
    return true;
}

// https://203.0.113.10:32400 is not a name on the plex.direct certificate, and the
// certificate the server presents for a raw IP is not in the Mozilla bundle
// (verify error 20). The same TCP address with SNI a-b-c-d.<id>.plex.direct is.
// Empty when `url` is not https to an IPv4 literal or `clientId` is not a label.
inline std::string plexDirectBaseForIpv4Https(const std::string& url, const std::string& clientId) {
    TlsUrl u;
    std::string dashed;
    if (!parseTlsUrl(url, u) || !u.ok || !u.https || u.plexDirect)
        return {};
    if (!ipv4LiteralHost(u.host, dashed) || !plexMachineLabelOk(clientId))
        return {};
    std::string out = "https://" + dashed + "." + clientId + ".plex.direct:" + std::to_string(u.port);
    if (!u.pathQuery.empty() && u.pathQuery != "/")
        out += u.pathQuery;
    return out;
}

inline bool httpsBareIpv4(const std::string& url) {
    TlsUrl u;
    std::string dashed;
    return parseTlsUrl(url, u) && u.ok && u.https && !u.plexDirect && ipv4LiteralHost(u.host, dashed);
}

} // namespace misterplex
