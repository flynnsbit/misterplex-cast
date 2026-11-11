#pragma once
// FU8: no X-Plex-Token on a curl argv (visible in ps). A token in a URL query
// (X-Plex-Token=...) or a "-H" "X-Plex-Token: v" pair moves into curl -K config
// text (stdin or /dev/fd/N), the #253 mechanism. A token moved off the URL drops
// -L so a redirect cannot carry it to another host. Pure: tests need no curl.
#include "../../arm/misterplexd/log_redact.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace mpx {

inline bool curlTokIeq(const std::string& a, size_t pos, const char* lit) {
    for (size_t i = 0; lit[i]; ++i) {
        if (pos + i >= a.size() ||
            std::tolower(static_cast<unsigned char>(a[pos + i])) != std::tolower(static_cast<unsigned char>(lit[i])))
            return false;
    }
    return true;
}

// Removes every X-Plex-Token=v query param from url; returns the last value seen.
inline std::string curlStripTokenQuery(std::string& url, bool* found = nullptr) {
    std::string val;
    bool any = false;
    const size_t q = url.find('?');
    if (found) *found = false;
    if (q == std::string::npos) return val;
    const size_t h = url.find('#', q);
    const std::string frag = h == std::string::npos ? "" : url.substr(h);
    const std::string query = url.substr(q + 1, h == std::string::npos ? std::string::npos : h - q - 1);
    std::string kept;
    size_t i = 0;
    while (i <= query.size()) {
        size_t amp = query.find('&', i);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(i, amp - i);
        if (curlTokIeq(part, 0, "x-plex-token=")) {
            any = true;
            val = part.substr(13);
        } else if (!part.empty()) {
            if (!kept.empty()) kept += '&';
            kept += part;
        }
        i = amp + 1;
    }
    if (any) url = url.substr(0, q) + (kept.empty() ? "" : "?" + kept) + frag;
    if (found) *found = any;
    return val;
}

inline bool curlTokenCfgSafe(const std::string& v) {
    for (unsigned char c : v)
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return false;
    return true;
}

// #272 N2: tokens dropped because they cannot go in a -K config (test hook).
inline int& curlTokenUnsafeDrops() {
    static int n = 0;
    return n;
}

// Moves tokens off argv into cfg. Returns true when argv changed.
inline bool scrubPlexTokenArgv(std::vector<std::string>& a, std::string& cfg) {
#ifdef MUT_FU8_TOKEN_ARGV   // test-only mutant: the old leak, argv left as built
    (void)a; (void)cfg;
    return false;
#else
    std::string tok;
    bool moved = false, fromUrl = false;
    std::vector<std::string> out;
    for (size_t i = 0; i < a.size(); ++i) {
        if ((a[i] == "-H" || a[i] == "--header") && i + 1 < a.size() && curlTokIeq(a[i + 1], 0, "x-plex-token:")) {
            std::string v = a[i + 1].substr(13);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            tok = v;
            moved = true;
            ++i;
            continue;
        }
        if (a[i].find("://") != std::string::npos) {
            bool f = false;
            std::string u = a[i];
            const std::string v = curlStripTokenQuery(u, &f);
            if (f) {
                if (!v.empty()) tok = v;
                moved = fromUrl = true;
                out.push_back(u);
                continue;
            }
        }
        out.push_back(a[i]);
    }
    if (!moved) return false;
    if (fromUrl) {
        std::vector<std::string> noL;
        for (auto& s : out)
            if (s != "-L" && s != "--location") noL.push_back(s);
        out.swap(noL);
    }
    if (!tok.empty() && !curlTokenCfgSafe(tok)) {
        // #272 N2: fail closed, but say so (never the value).
        ++curlTokenUnsafeDrops();
        std::fprintf(stderr, "misterplexd: X-Plex-Token dropped from curl request (unsafe characters, len=%zu)\n",
                     tok.size());
    }
    if (!tok.empty() && curlTokenCfgSafe(tok) && !misterplex::hasPlexTokenKey(cfg)) {
        cfg += "header = \"X-Plex-Token: " + tok + "\"\n";
        bool hasK = false;
        for (size_t i = 0; i + 1 < out.size(); ++i)
            if (out[i] == "-K") hasK = true;
        if (!hasK) {
            size_t at = out.size();
            for (size_t i = 0; i < out.size(); ++i)
                if (out[i] == "--") { at = i; break; }
            if (at == out.size() && !out.empty()) at = out.size() - 1;
            out.insert(out.begin() + static_cast<long>(at), {"-K", "-"});
        }
    }
    a.swap(out);
    return true;
#endif
}

// #340: does a curl -K config carry an X-Plex-Token header line?
inline bool curlCfgHasPlexToken(const std::string& cfg) {
    for (size_t i = 0; i + 13 <= cfg.size(); ++i)
        if (curlTokIeq(cfg, i, "x-plex-token:")) return true;
    return false;
}

// #340: the -K config without its X-Plex-Token line(s), for a redirect hop to another origin.
inline std::string curlCfgWithoutPlexToken(const std::string& cfg) {
    std::string out;
    size_t i = 0;
    while (i < cfg.size()) {
        size_t e = cfg.find('\n', i);
        e = e == std::string::npos ? cfg.size() : e + 1;
        const std::string line = cfg.substr(i, e - i);
        if (!curlCfgHasPlexToken(line)) out += line;
        i = e;
    }
    return out;
}

// #340: an argv "-H X-Plex-Token: ..." pair (before the FU8 scrub moves it to -K).
inline bool curlArgvHasTokenHeader(const std::vector<std::string>& a) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if ((a[i] == "-H" || a[i] == "--header") && curlTokIeq(a[i + 1], 0, "x-plex-token:")) return true;
    return false;
}

// FU8 test predicate: no argv element names the token or holds its value.
inline bool argvFreeOfToken(const std::vector<std::string>& a, const std::string& token) {
    for (const auto& s : a) {
        for (size_t i = 0; i < s.size(); ++i)
            if (curlTokIeq(s, i, "x-plex-token")) return false;
        if (!token.empty() && s.find(token) != std::string::npos) return false;
    }
    return true;
}

// #272: a URL query token becomes an X-Plex-Token header for callers that already send
// headers through a -K config (plexui_http). Returns true when the URL changed.
inline bool moveUrlTokenToHeaders(std::string& url, std::vector<std::pair<std::string, std::string>>& h) {
#ifdef MUT_272_URL_TOKEN_KEPT   // test-only mutant: the token stays in the URL on argv
    (void)url; (void)h;
    return false;
#else
    bool found = false;
    const std::string tok = curlStripTokenQuery(url, &found);
    if (!found) return false;
    bool have = false;
    for (const auto& kv : h)
        if (curlTokIeq(kv.first, 0, "x-plex-token") && kv.first.size() == 12) have = true;
    if (!have && !tok.empty()) h.emplace_back("X-Plex-Token", tok);
    return true;
#endif
}

// #272 N1: origin (lowercased scheme://host:port, default port filled) of an absolute URL; "" if none.
inline std::string curlUrlOrigin(const std::string& u) {
    const size_t se = u.find("://");
    if (se == std::string::npos || se == 0) return {};
    std::string scheme;
    for (size_t i = 0; i < se; ++i) scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(u[i]))));
    size_t b = se + 3, e = u.find_first_of("/?#", b);
    std::string auth = u.substr(b, e == std::string::npos ? std::string::npos : e - b);
    const size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    for (auto& c : auth) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (auth.empty()) return {};
    const size_t rb = auth.rfind(']');
    const size_t colon = auth.rfind(':');
    if (colon == std::string::npos || (rb != std::string::npos && colon < rb))
        auth += scheme == "https" ? ":443" : scheme == "http" ? ":80" : ":";
    return scheme + "://" + auth;
}

// #272 N1: next hop for a redirect of a request whose URL token went to a header. The token
// is re-attached (and so re-scrubbed into -K) only when the target has the PMS origin;
// another host gets the redirect without it. "" when the target is not absolute http(s).
inline std::string browseRedirectNext(const std::string& origin, const std::string& location,
                                      const std::string& token) {
    std::string next = location;
    curlStripTokenQuery(next);
    const std::string o = curlUrlOrigin(next);
    if (o.empty() || (o.rfind("http://", 0) != 0 && o.rfind("https://", 0) != 0)) return {};
#ifndef MUT_272_REDIRECT_ANY_HOST   // test-only mutant: token follows to any host
    if (o != origin || token.empty()) return next;
#else
    (void)origin;
    if (token.empty()) return next;
#endif
    const size_t h = next.find('#');
    const std::string frag = h == std::string::npos ? "" : next.substr(h);
    if (h != std::string::npos) next.resize(h);
    next += (next.find('?') == std::string::npos ? "?" : "&") + std::string("X-Plex-Token=") + token;
    return next + frag;
}

// FU10 N4 (#270): runArgv follows a token request's redirects itself (curl -D headers).
// Last response block of a -D dump: status code and Location ("" if none).
inline std::string curlRedirectFromHeaders(const std::string& hdr, int* status) {
    int st = 0;
    std::string loc;
    size_t p = 0;
    while (p < hdr.size()) {
        size_t e = hdr.find('\n', p);
        if (e == std::string::npos) e = hdr.size();
        std::string line = hdr.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("HTTP/", 0) == 0) {   // a new response block (redirect hop or 100-continue)
            const size_t sp = line.find(' ');
            st = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
            loc.clear();
        } else if (line.size() > 9 && curlTokIeq(line, 0, "location:")) {
            size_t v = 9;
            while (v < line.size() && line[v] == ' ') ++v;
            loc = line.substr(v);
        }
    }
    if (status) *status = st;
    return loc;
}

// Absolute form of a Location against the request's origin ("/x" and "//host/x" handled).
inline std::string curlResolveLocation(const std::string& requestUrl, const std::string& loc) {
    if (loc.find("://") != std::string::npos) return loc;
    const std::string o = curlUrlOrigin(requestUrl);
    if (o.empty() || loc.empty()) return {};
    if (loc.rfind("//", 0) == 0) return o.substr(0, o.find("://") + 1) + loc;
    if (loc[0] == '/') return o + loc;
    return {};   // relative-path redirects: not followed with a token
}

}  // namespace mpx
