#pragma once
// FEATURE-GAP E14: a Zaparoo / NFC card starts a Plex title.
//
// ZAPAROO (misterplex.conf) / MPX_ZAPAROO (env wins). Default off.
// Unset, empty, 0, off, false, and no stay off.
//
// Off: GET or POST /misterplex/zaparoo answers 404 "zaparoo off" and does
// not start play. The cast play path is unchanged.
// On: the same route reads one card text and, when that text names a Plex
// library item, rewrites the request into playMedia for
// /library/metadata/<ratingKey>. Companion then runs the existing cast
// path (onPlay_ → resolve → playOwenbRoute) against the configured PMS.
// The card text is not logged. A token in the text is not copied onto the
// key. Anything that is not a library item is 400 "zaparoo refuse" and
// does not play.
//
// Card text (one of):
//   **launch.plex:/library/metadata/<digits>
//   **media.plex:/library/metadata/<digits>
//   plex:/library/metadata/<digits>
//   plex://library/metadata/<digits>
//   plex:<digits>
//   /library/metadata/<digits>
//   <digits>
// A query or fragment on the library path is dropped. http(s) URLs and
// other launchers (**launch.nes:...) are refused.

#include "../../arm/misterplexd/log_redact.hpp"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {
namespace zap {

inline bool zaparooEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// Default off. MPX_ZAPAROO wins over ZAPAROO when it is set.
inline bool zaparooEnabled() {
#ifdef MUT_ZAPAROO_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_ZAPAROO"); e && e[0])
        return zaparooEnvOn(e);
    return zaparooEnvOn(std::getenv("ZAPAROO"));
#endif
}

inline std::string pctDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

inline std::string pctEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

inline std::string asciiLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trimCopy(std::string s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

inline bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 16)
        return false;
    for (unsigned char c : s)
        if (!std::isdigit(c))
            return false;
    return true;
}

inline bool startsWith(const std::string& lower, const char* pfx) {
    const std::string p(pfx);
    return lower.size() >= p.size() && lower.compare(0, p.size(), p) == 0;
}

// Library key, or empty when the card is not a Plex item.
inline std::string cardToKey(std::string text) {
    text = trimCopy(std::move(text));
    if (text.empty() || text.size() > 256)
        return {};
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7f)
            return {};
    const std::string lower = asciiLower(text);
    if (startsWith(lower, "http://") || startsWith(lower, "https://"))
        return {};
    auto strip = [&](const char* pfx) {
        if (!startsWith(lower, pfx))
            return false;
        text = text.substr(std::strlen(pfx));
        return true;
    };
    (void)(strip("**launch.plex:") || strip("**media.plex:") || strip("zaparoo:plex:") ||
           strip("plex://") || strip("plex:"));
    text = trimCopy(std::move(text));
    const auto cut = text.find_first_of("?#");
    if (cut != std::string::npos)
        text.resize(cut);
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    while (text.size() > 1 && text[0] == '/' && text[1] == '/')
        text.erase(text.begin());
    if (text.rfind("library/metadata/", 0) == 0)
        text.insert(text.begin(), '/');
    if (text.rfind("/library/metadata/", 0) == 0) {
        const std::string rk = text.substr(std::string("/library/metadata/").size());
        if (!allDigits(rk) || text.find("..") != std::string::npos)
            return {};
        return "/library/metadata/" + rk;
    }
    if (allDigits(text))
        return "/library/metadata/" + text;
    return {};
}

struct Decision {
    bool matched = false;
    bool startPlay = false;
    int code = 0;
    std::string body;
    std::string log;
    std::string key;
    std::string ratingKey;
};

inline bool queryOnce(const std::string& q, const std::string& key, std::string& out) {
    int hits = 0;
    std::size_t i = 0;
    while (i <= q.size()) {
        std::size_t amp = q.find('&', i);
        if (amp == std::string::npos)
            amp = q.size();
        const std::string kv = q.substr(i, amp - i);
        if (kv.compare(0, key.size() + 1, key + "=") == 0) {
            out = kv.substr(key.size() + 1);
            ++hits;
        }
        if (amp == q.size())
            break;
        i = amp + 1;
    }
    return hits == 1;
}

inline Decision refuse(int code) {
    Decision d;
    d.matched = true;
    d.code = code;
    d.body = "zaparoo refuse\n";
    d.log = "zaparoo: refuse";
    return d;
}

// Pure. The companion sends the body when startPlay is false, and rewrites
// the request into playMedia when startPlay is true.
inline Decision decide(const std::string& req) {
    Decision d;
    const std::string line = req.substr(0, req.find("\r\n"));
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
        return d;
    const std::string method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t qm = target.find('?');
    const std::string path = target.substr(0, qm);
    if (path != "/misterplex/zaparoo")
        return d;
    d.matched = true;
#ifndef MUT_ZAPAROO_PLAY_WHEN_OFF
    if (!zaparooEnabled()) {
        d.code = 404;
        d.body = "zaparoo off\n";
        d.log = "zaparoo: off";
        return d;
    }
#else
    (void)zaparooEnabled();
#endif
    if (method != "GET" && method != "POST")
        return refuse(405);
    const std::string q = qm == std::string::npos ? std::string() : target.substr(qm + 1);
    std::string text;
    if (!queryOnce(q, "text", text))
        return refuse(400);
    text = pctDecode(text);
#ifdef MUT_ZAPAROO_OPEN_URL
    {
        const std::string lower = asciiLower(trimCopy(text));
        if (startsWith(lower, "http://") || startsWith(lower, "https://")) {
            d.startPlay = true;
            d.code = 200;
            d.key = trimCopy(text);
            d.log = "zaparoo: play key=" + d.key;
            return d;
        }
    }
#endif
    std::string key = cardToKey(text);
#ifdef MUT_ZAPAROO_IGNORE_CARD
    key.clear();
#endif
#ifdef MUT_ZAPAROO_FOREIGN_SYSTEM
    if (key.empty()) {
        const std::string lower = asciiLower(trimCopy(text));
        if (startsWith(lower, "**launch.") && !startsWith(lower, "http://") &&
            !startsWith(lower, "https://")) {
            d.startPlay = true;
            d.code = 200;
            d.key = trimCopy(text);
            d.log = "zaparoo: play key=" + d.key;
            return d;
        }
    }
#endif
    if (key.empty() || key.rfind("/library/metadata/", 0) != 0)
        return refuse(400);
    d.key = key;
    d.ratingKey = key.substr(std::string("/library/metadata/").size());
    d.startPlay = true;
    d.code = 200;
    d.log = "zaparoo: play key=" + d.key;
#ifdef MUT_ZAPAROO_TOKEN_LEAK
    if (misterplex::hasPlexTokenKey(text) || text.find("token=") != std::string::npos)
        d.log = "zaparoo: play text=" + text;
#endif
    return d;
}

// Request line only. Key is percent-encoded. No token, no card text.
inline std::string playMediaRequestLine(const Decision& d) {
    return "GET /player/playback/playMedia?key=" + pctEncode(d.key) + "&ratingKey=" + d.ratingKey +
           "&offset=0 HTTP/1.1";
}

} // namespace zap

using zap::zaparooEnabled;

} // namespace misterplex
