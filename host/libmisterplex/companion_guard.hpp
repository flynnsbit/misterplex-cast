// companion_guard.hpp: pure checks for the Plex companion server (:3005), SEC-M2.
//  - peerAllowed: only LAN peers (loopback, RFC1918, link-local, CGNAT/Tailscale) may connect;
//    MPX_COMPANION_ALLOW = lan (default) | any | comma list of a.b.c.d[/n] (optionally with "lan" in the list).
//  - originAllowed: a browser request (Origin header, http/https) is served only from plex.tv, *.plex.direct, a
//    private-IP / localhost / *.local page or an extra host in MPX_COMPANION_ORIGINS; "any" restores accept-all except Origin "null", which is always refused.
//    Native clients (no Origin) and native-shell origins (app:, qrc:, ...) are unaffected; browser-extension, file:,
//    data:, blob: and similar web-ish schemes are refused (except under MPX_COMPANION_ORIGINS=any).
//  - playTargetOk: playMedia address/protocol/port must be a plain http(s) host[:port]; link-local/unspecified/multicast refused.
// No sockets and no env reads here (callers pass the env text), so it is host-testable. Compile-time mutants: MUT_GUARD_*.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex {
namespace guard {

struct Cidr {
    uint32_t net = 0, mask = 0;
};

inline std::string lowerStr(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool parseIpv4(const std::string& s, uint32_t& out) {
    uint32_t v = 0;
    int parts = 0;
    size_t i = 0;
    while (true) {
        size_t j = i;
        unsigned n = 0;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && j - i < 4)
            n = n * 10 + static_cast<unsigned>(s[j++] - '0');
        if (j == i || j - i > 3 || n > 255)
            return false;
        v = (v << 8) | n;
        ++parts;
        if (j == s.size())
            break;
        if (s[j] != '.')
            return false;
        i = j + 1;
    }
    if (parts != 4)
        return false;
    out = v;
    return true;
}

inline bool inCidr(uint32_t ip, const Cidr& c) { return (ip & c.mask) == c.net; }

inline const std::vector<Cidr>& lanCidrs() {
    static const std::vector<Cidr> v = {
        {0x7F000000u, 0xFF000000u},  // 127/8
        {0x0A000000u, 0xFF000000u},  // 10/8
        {0xAC100000u, 0xFFF00000u},  // 172.16/12
        {0xC0A80000u, 0xFFFF0000u},  // 192.168/16
        {0xA9FE0000u, 0xFFFF0000u},  // 169.254/16 link-local
#ifndef MUT_GUARD_NO_CGNAT
        {0x64400000u, 0xFFC00000u},  // 100.64/10 CGNAT (Tailscale)
#endif
    };
    return v;
}

inline bool isLanIpv4(uint32_t ip) {
#ifdef MUT_GUARD_ALL_PEERS
    (void)ip;
    return true;
#else
    for (const Cidr& c : lanCidrs())
        if (inCidr(ip, c))
            return true;
    return false;
#endif
}

struct PeerPolicy {
    bool any = false;
    bool lan = true;
    std::vector<Cidr> extra;
};

inline bool parseCidrTok(const std::string& tok, Cidr& c) {
    const size_t slash = tok.find('/');
    int bits = 32;
    if (slash != std::string::npos) {
        const std::string b = tok.substr(slash + 1);
        if (b.empty() || b.size() > 2)
            return false;
        bits = 0;
        for (char ch : b) {
            if (!std::isdigit(static_cast<unsigned char>(ch)))
                return false;
            bits = bits * 10 + (ch - '0');
        }
        if (bits > 32)
            return false;
    }
    uint32_t ip = 0;
    if (!parseIpv4(tok.substr(0, slash), ip))
        return false;
    c.mask = bits == 0 ? 0u : (0xFFFFFFFFu << (32 - bits));
    c.net = ip & c.mask;
    return true;
}

inline std::vector<std::string> splitCommaTrim(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find(',', i);
        if (j == std::string::npos)
            j = text.size();
        std::string tok = text.substr(i, j - i);
        while (!tok.empty() && tok.front() == ' ')
            tok.erase(tok.begin());
        while (!tok.empty() && tok.back() == ' ')
            tok.pop_back();
        if (!tok.empty())
            out.push_back(tok);
        i = j + 1;
    }
    return out;
}

// "" / "lan" -> LAN only (default). "any" -> everyone. Otherwise the listed a.b.c.d[/n] entries (+ LAN if "lan" is in the list).
inline PeerPolicy parsePeerPolicy(const std::string& textIn) {
    PeerPolicy p;
    const std::string text = lowerStr(textIn);
    if (text.empty() || text == "lan")
        return p;
    if (text == "any") {
        p.any = true;
        return p;
    }
    p.lan = false;
    for (const std::string& tok : splitCommaTrim(text)) {
        Cidr c;
        if (tok == "lan")
            p.lan = true;
        else if (parseCidrTok(tok, c))
            p.extra.push_back(c);
    }
    return p;
}

inline bool peerAllowed(uint32_t ipHostOrder, const PeerPolicy& p) {
    if (p.any)
        return true;
    if (p.lan && isLanIpv4(ipHostOrder))
        return true;
    for (const Cidr& c : p.extra)
        if (inCidr(ipHostOrder, c))
            return true;
    return false;
}

// ---- browser Origin policy -------------------------------------------------------------------
inline bool hostIsPrivateOrLocal(const std::string& hostIn) {
    const std::string h = lowerStr(hostIn);
    uint32_t ip = 0;
    if (parseIpv4(h, ip))
        return isLanIpv4(ip);
    if (h == "localhost" || h == "[::1]")
        return true;
    const std::string loc = ".local";
    if (h.size() > loc.size() && h.compare(h.size() - loc.size(), loc.size(), loc) == 0)
        return true;
    // Single-label LAN hostname (Plex Web Origin http://studio:32400, /etc/hosts or mDNS).
    if (h.find('.') == std::string::npos && !h.empty() && h[0] != '[' && h.size() <= 63) {
        if (h.front() == '-' || h.back() == '-')
            return false;
        for (char c : h) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-'))
                return false;
        }
        return true;
    }
    return false;
}

inline bool hostEndsWith(const std::string& h, const std::string& suffix) {
    return h.size() > suffix.size() && h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0 &&
           h[h.size() - suffix.size() - 1] == '.';
}

// true when the text before the first "://" is not a plain URL scheme token ([a-z][a-z0-9+.-]*), or when there is
// no "://" but a ':' is present (javascript:, about:, blob:, filesystem:). o is already lower-case.
inline bool originSchemeMalformed(const std::string& o) {
    const size_t se = o.find("://");
    if (se == std::string::npos)
        return o.find(':') != std::string::npos;
    if (se == 0 || !std::isalpha(static_cast<unsigned char>(o[0])))
        return true;
    for (size_t i = 0; i < se; ++i) {
        const unsigned char c = static_cast<unsigned char>(o[i]);
        if (!(std::isalnum(c) || c == '+' || c == '.' || c == '-'))
            return true;
    }
    // a second "scheme://" after the authority start (https://app.plex.tv:blob://x) is nested too
    return o.find("://", se + 3) != std::string::npos;
}

// origin: the raw Origin header value; extra: MPX_COMPANION_ORIGINS text ("" | "any" | comma list of host names).
// Origin "null" (any case) is refused whatever extra says.
inline bool originAllowed(const std::string& origin, const std::string& extra) {
    if (origin.empty())
        return true;  // native Plex clients send no Origin
    const std::string x = lowerStr(extra);
    const std::string o = lowerStr(origin);
    // "null" (sandboxed iframe, data: URL, redirect chain, file: page) is never a Plex client and carries no
    // identity: refused always, even with MPX_COMPANION_ORIGINS=any or listing "null" (SEC-M5 follow-up, MFD).
#ifdef MUT_GUARD_NULL_OK
    if (o == "null")
        return true;
#else
    if (o == "null")
        return false;
#endif
    // Nested / malformed scheme forms are never a browser Origin (an Origin is scheme "://" host [":" port], no path):
    // blob:http://x/1, filesystem:http://x/t, javascript:..., about:blank, or "scheme:" with no "//". Refused always,
    // even under MPX_COMPANION_ORIGINS=any, because the first "://" would otherwise make "blob:http" look like a
    // native-shell scheme (rc-1026: blob:http://x/1 got 200).
#ifndef MUT_GUARD_NESTED_SCHEME_OK
    if (originSchemeMalformed(o))
        return false;
#endif
    if (x == "any")
        return true;
    size_t schemeEnd = o.find("://");
    if (schemeEnd == std::string::npos) {
        // Any other value without a scheme is not a Plex client either.
        return false;
    }
    const std::string scheme = o.substr(0, schemeEnd);
    if (scheme != "http" && scheme != "https") {
        // Native shells (app:, qrc:, ...) stay allowed. Browser-extension pages and file:/data:/blob:/ws: style
        // origins carry no Plex identity and are refused (N2).
#ifndef MUT_GUARD_EXT_SCHEME_OK
        static const char* const kWebSchemes[] = {
            "chrome-extension", "moz-extension", "safari-web-extension", "safari-extension", "ms-browser-extension",
            "extension", "file", "data", "blob", "filesystem", "about", "javascript", "ws", "wss", "ftp", "view-source",
            "chrome", "edge", "vscode-webview"};
        for (const char* w : kWebSchemes)
            if (scheme == w)
                return false;
#endif
        return true;
    }
    std::string host = o.substr(schemeEnd + 3);
    if (host.empty() || host.find('/') != std::string::npos || host.find('@') != std::string::npos)
        return false;
    if (host[0] == '[') {
        const size_t rb = host.find(']');
        if (rb == std::string::npos)
            return false;
        host = host.substr(0, rb + 1);
    } else {
        const size_t colon = host.find(':');
        if (colon != std::string::npos)
            host = host.substr(0, colon);
    }
#ifdef MUT_GUARD_ORIGIN_ANY
    return true;
#endif
    if (host == "plex.tv" || hostEndsWith(host, "plex.tv") || hostEndsWith(host, "plex.direct"))
        return true;
    if (hostIsPrivateOrLocal(host))
        return true;
    for (const std::string& e : splitCommaTrim(x))
        if (host == e || hostEndsWith(host, e))
            return true;
    return false;
}

inline bool validPortText(const std::string& p) {
    if (p.empty() || p.size() > 5)
        return false;
    unsigned v = 0;
    for (char c : p) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    return v >= 1 && v <= 65535;
}

// rc-1035: a Plex Web select reaches the daemon through the PMS with Host "<a-b-c-d>.<hash>.plex.direct[:port]".
// Narrow rule: exactly four labels, label 1 = a dashed IPv4 (1-3 digit groups that parse), label 2 = the server hash
// ([a-z0-9]+), then "plex" "direct". plex.direct DNS answers with the IPv4 in label 1 (no rebinding to another
// address), "plex.direct.evil.com", "evilplex.direct", userinfo, IPv6-dashed and deeper names are not this form.
// hostLower has no port. Callers also require the request peer to be on the LAN (see hostHeaderAllowed).
inline bool plexDirectHost(const std::string& hostLower) {
#ifdef MUT_GUARD_PLEXDIRECT_SUFFIX
    return hostLower.size() > 12 && hostLower.find("plex.direct") != std::string::npos;  // mutant: substring match
#endif
    std::vector<std::string> lab;
    size_t i = 0;
    while (true) {
        const size_t j = hostLower.find('.', i);
        lab.push_back(hostLower.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos)
            break;
        i = j + 1;
    }
    if (lab.size() != 4 || lab[2] != "plex" || lab[3] != "direct")
        return false;
    if (lab[1].empty() || lab[1].size() > 63)
        return false;
    for (char c : lab[1])
        if (!std::isalnum(static_cast<unsigned char>(c)))
            return false;
    std::string dotted = lab[0];
    int dashes = 0;
    for (char& c : dotted) {
        if (c == '-') {
            c = '.';
            ++dashes;
        } else if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    uint32_t ip = 0;
    return dashes == 3 && parseIpv4(dotted, ip);
}

// ---- Host header (DNS rebinding) ---------------------------------------------------------------
// A rebinding page reaches the daemon with its own DNS name in Host. Allowed: the daemon's own IPv4 on the
// accepted socket, 127/8, localhost, [::1], *.local, <ipv4-dashed>.<hash>.plex.direct from a LAN peer (rc-1035),
// a private IPv4 Host from a LAN peer (Plex Web PMS-proxied playMedia keeps Host=<pms-lan>:32400), and
// MPX_COMPANION_HOSTS entries (exact name/IP, or a dot-suffix; the word "ipv4" admits any IPv4 literal, for
// NAT/port-forward setups). No Host (HTTP/1.0): allowed, a browser always sends one.
inline bool hostHeaderAllowed(const std::string& hostHeader, const std::string& ownIpv4, const std::string& extra,
                              bool peerIsLan = false) {
    if (hostHeader.empty())
        return true;
    std::string h = lowerStr(hostHeader);
    for (char c : h)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']'))
            return false;
    if (h[0] == '[') {
        const size_t rb = h.find(']');
        if (rb == std::string::npos)
            return false;
        const std::string rest = h.substr(rb + 1);
        if (!(rest.empty() || (rest[0] == ':' && validPortText(rest.substr(1)))))
            return false;
        h = h.substr(0, rb + 1);
    } else {
        const size_t colon = h.find(':');
        if (colon != std::string::npos) {
            if (!validPortText(h.substr(colon + 1)))
                return false;
            h = h.substr(0, colon);
        }
    }
    if (h.empty() || h.back() == '.')
        return false;
#ifdef MUT_GUARD_HOST_ANY
    return true;
#endif
    if (h == "localhost" || h == "[::1]")
        return true;
    uint32_t ip = 0;
    if (parseIpv4(h, ip)) {
        if ((ip >> 24) == 127u || (!ownIpv4.empty() && h == ownIpv4))
            return true;
        // PMS-proxied Plex Web: the server forwards with Host=<pms-lan-ip>:32400, peer is LAN.
        if (peerIsLan && isLanIpv4(ip))
            return true;
    } else if (h.size() > 6 && h.compare(h.size() - 6, 6, ".local") == 0) {
        return true;
#ifndef MUT_GUARD_PLEXDIRECT_OFF
    } else if (
#ifndef MUT_GUARD_PLEXDIRECT_ANYPEER
               peerIsLan &&
#endif
               plexDirectHost(h)) {
        return true;
#endif
    }
    for (const std::string& e : splitCommaTrim(lowerStr(extra))) {
        if (e == "ipv4" ? parseIpv4(h, ip) : (h == e || hostEndsWith(h, e)))
            return true;
    }
    return false;
}

// Sec-Fetch-Site: cross-site with NO Origin is a browser subresource/navigation from another site (img tag,
// form GET). With an Origin the Origin policy decides (Plex Web is cross-site by design). Browsers only send
// Sec-Fetch-* to potentially trustworthy targets (https, localhost), so this narrows rather than closes the gap.
inline bool fetchSiteAllowed(const std::string& origin, const std::string& secFetchSite) {
#ifdef MUT_GUARD_FETCH_SITE_OK
    return true;
#endif
    return !(origin.empty() && lowerStr(secFetchSite) == "cross-site");
}

// The Origin to echo in Access-Control-Allow-Origin ("" = no header). Only called for an Origin that
// originAllowed() accepted; never "*".
inline std::string corsEchoOrigin(const std::string& origin) {
    if (origin.empty() || origin.size() > 200)
        return "";
    for (char c : origin)
        if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7E)
            return "";
    return origin;
}

// ---- request path class (no-Origin policy) -------------------------------------------------------
// ReadOnly = answers without changing the player: /resources, /identity, /misterplex/mast, /misterplex/status, timeline poll.
// Everything else (playback/*, navigation/*, timeline subscribe/unsubscribe, mirror, unknown) is state-changing.
inline bool pathIsReadOnly(const std::string& requestLine) {
    const size_t sp = requestLine.find(' ');
    if (sp == std::string::npos || requestLine.compare(0, sp, "GET") != 0)
        return false;
    size_t e = requestLine.find(' ', sp + 1);
    if (e == std::string::npos)
        e = requestLine.size();
    std::string path = requestLine.substr(sp + 1, e - sp - 1);
    const size_t q = path.find('?');
    if (q != std::string::npos)
        path.resize(q);
#ifdef MUT_GUARD_MISTERPLEX_PREFIX_READ
    if (path.rfind("/misterplex/", 0) == 0)
        return true;
#endif
    return path == "/resources" || path == "/identity" || path == "/misterplex/mast" || path == "/misterplex/status" ||
           path == "/player/timeline/poll" || path == "/player/proxy/timeline";
}

// A request header a cross-site page cannot add without a CORS preflight (the preflight carries an Origin, which
// originAllowed() refuses): a plain <img>/navigation/no-cors GET cannot set X-Plex-* or X-MPX-Client. Native Plex
// controllers send X-Plex-Client-Identifier on every command. HEADER values only: a query parameter of the same name
// is attacker-controllable and does not count. Non-empty, printable ASCII, at most 128 chars.
inline bool clientHeaderOk(const std::string& v) {
#ifdef MUT_GUARD_CLIENT_HDR_EMPTY_OK
    return true;
#endif
    if (v.empty() || v.size() > 128)
        return false;
    for (char c : v)
        if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7E)
            return false;
    return true;
}

// MPX_COMPANION_NO_ORIGIN (daemon process environment only):
//   "" | "lan" (default): a no-Origin LAN peer may use the read-only paths freely; on every state-changing path
//        (playback, navigation, timeline subscribe, queue/update, anything unknown, any non-GET) it must send a client
//        header (clientHeaderOk: X-Plex-Client-Identifier, X-Plex-Target-Client-Identifier, X-Plex-Token or
//        X-MPX-Client). Native Plex controllers and the phone web UI pass; a drive-by web page cannot (SecEng M-N0).
//   "reads": no-Origin only on read-only paths, no matter what headers it sends (strictest; native casting stops).
//   "legacy": the pre-guard behaviour, no-Origin LAN peers may use every path with no header at all.
inline bool noOriginAllowed(const std::string& requestLine, const std::string& mode, bool hasClientHeader = false) {
#ifdef MUT_GUARD_NO_ORIGIN_READS_OPEN
    return true;
#endif
    const std::string m = lowerStr(mode);
    if (m == "legacy")
        return true;
    if (pathIsReadOnly(requestLine))
        return true;
#ifdef MUT_GUARD_LAN_NEEDS_NO_HEADER
    if (m != "reads")
        return true;
#endif
    if (m == "reads")
        return false;
    return hasClientHeader;
}

// ---- playMedia target ------------------------------------------------------------------------
inline bool validPort(const std::string& p) {
    if (p.empty() || p.size() > 5)
        return false;
    unsigned v = 0;
    for (char c : p) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    return v >= 1 && v <= 65535;
}

// A host that inet_aton/getaddrinfo would read as an IPv4 number but that is not a strict decimal dotted quad:
// last label all digits or 0x..., or a dotted quad with a leading-zero octet (octal to inet_aton).
inline bool numericHostForm(const std::string& h) {
    const size_t dot = h.rfind('.');
    const std::string last = dot == std::string::npos ? h : h.substr(dot + 1);
    bool digits = !last.empty();
    for (char c : last)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            digits = false;
    uint32_t ip = 0;
    const bool quad = parseIpv4(h, ip);
    if (digits && !quad)
        return true;
    if (last.size() > 1 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X'))
        return true;
    if (quad)
        for (size_t i = 0; i < h.size();) {
            size_t j = h.find('.', i);
            if (j == std::string::npos)
                j = h.size();
            if (j - i > 1 && h[i] == '0')
                return true;
            i = j + 1;
        }
    return false;
}

inline bool hostNameOk(const std::string& h) {
    if (h.empty() || h.size() > 253)
        return false;
    for (char c : h)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
            return false;
    uint32_t ip = 0;
#ifndef MUT_GUARD_NUMERIC_HOST_OK
    if (numericHostForm(h))
        return false;  // N2: 2852039166, 0xa9fea9fe, 0251.0376.0251.0376, 010.0.0.1 (inet_aton reads these as IPv4)
#endif
    if (parseIpv4(h, ip)) {
#ifndef MUT_GUARD_LINKLOCAL_OK
        if ((ip >> 16) == 0xA9FEu || (ip >> 24) == 0u || (ip >> 28) >= 0xEu)
            return false;  // 169.254/16 (cloud metadata), 0/8, multicast/reserved
#endif
    }
    return true;
}

// The playMedia fields as the companion receives them (address already percent-decoded). address may be
// "host", "host:port" or "http(s)://host[:port][/]"; protocol "" | http | https; port "" or 1..65535.
inline bool playTargetOk(const std::string& addressIn, const std::string& protocol, const std::string& port) {
    const std::string pr = lowerStr(protocol);
    if (!pr.empty() && pr != "http" && pr != "https")
        return false;
    if (!port.empty() && !validPort(port))
        return false;
    std::string a = addressIn;
    if (a.empty())
        return true;
    const std::string la = lowerStr(a);
    if (la.compare(0, 7, "http://") == 0)
        a = a.substr(7);
    else if (la.compare(0, 8, "https://") == 0)
        a = a.substr(8);
    else if (la.find("://") != std::string::npos)
        return false;
    if (!a.empty() && a.back() == '/')
        a.pop_back();
    if (a.empty())
        return false;
    if (a[0] == '[') {
        const size_t rb = a.find(']');
        if (rb == std::string::npos || rb < 3)
            return false;
        for (size_t i = 1; i < rb; ++i)
            if (!(std::isxdigit(static_cast<unsigned char>(a[i])) || a[i] == ':' || a[i] == '.'))
                return false;
#ifndef MUT_GUARD_V6_FORMS_OK
        {   // N2: ::, ::ffff:x:y and embedded-dotted-quad (reach refused IPv4), fe80::/10, ff00::/8
            const std::string b = lowerStr(a.substr(1, rb - 1));
            if (b.find('.') != std::string::npos || b == "::" || b.compare(0, 6, "::ffff") == 0 ||
                b.compare(0, 2, "ff") == 0 ||
                (b.size() > 3 && b.compare(0, 2, "fe") == 0 && std::strchr("89ab", b[2]) != nullptr))
                return false;
        }
#endif
        const std::string rest = a.substr(rb + 1);
        return rest.empty() || (rest[0] == ':' && validPort(rest.substr(1)));
    }
    const size_t colon = a.find(':');
    if (colon == std::string::npos)
        return hostNameOk(a);
    if (a.find(':', colon + 1) != std::string::npos)
        return false;
    return hostNameOk(a.substr(0, colon)) && validPort(a.substr(colon + 1));
}

}  // namespace guard
}  // namespace misterplex
