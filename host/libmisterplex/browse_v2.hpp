#pragma once
// ARM Plex browse P1 (backlog item 6), MPX_BROWSE_V2=1. Pure helpers: the env flag, the
// helper's MediaContainer XML (mpx-menu contract, rel-mpxmenu-contract-1004.md), helper
// URLs, play tickets and the paint snapshot. The worker is browse_controller.hpp.
//
// MPX_BROWSE_V2 unset, "" or "0": off, and nothing here runs (misterplexd behaves as
// 2ffd5c14, i.e. 2164c4ad + the w9 appendTags fix). "1": on. Anything else: off, plus one notice line.
//
// The helper (mpx-menu, lab-pc LAN-only port 18769) answers Plex-shaped XML: Directory rows
// (key = an absolute, path-only "/menu..." path on the helper; only the root fetch adds
// ?cols=), Video/Track play rows with
//   mpxServer   server slug ("local", "s<7 hex>"),
//   key         the play ticket on the helper ("/menu/ticket/<slug>/<rk>/o<ms>", contract §8;
//               P0's "/menu/resolve/..." also works). GET it for an MpxPlay ticket. A row with
//               mpxSource="local" and no key plays by ratingKey on the conf PMS,
//   mpxOffsetMs 0 = play from start, > 0 = resume offset (the RESUME FROM / PLAY FROM START
//               rows; doPlay then never applies the PMS viewOffset itself),
//   mpxKind     "info" / "header" / "offline": shown, not activatable (build kit §4.2),
// paging rows (type or mpxKind "page", or P0's type="mpxpage"; NEXT / PREV) whose key is the
// neighbouring page, and an optional MediaContainer@mpxHeader for the page header.

#include "browse_library_list.hpp"
#include "library_browser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {

constexpr const char* kBrowseV2Env = "MPX_BROWSE_V2";
// MVP-b (build kit §6): PLXI commands 6-11 (Up/Down/PageUp/PageDown/Back/Info) for a future
// RBF. Off by default even with MPX_BROWSE_V2=1, so the MVP reads exactly what prod RTL
// 4c210d99 emits (1-4; Space/Esc/Right/Left, J1 buttons 1-4) plus Browse (5).
constexpr const char* kBrowseNavEnv = "MPX_BROWSE_MVPB";
constexpr const char* kMenuUrlEnv = "MPX_MENU_URL";
constexpr const char* kMenuBaseEnv = "MPX_MENU_BASE"; // the contract's name: origin only
constexpr const char* kDefaultMenuUrl = ""; // rc2: no built-in helper IP; "" = PMS sections only (MPX_MENU_URL / MPX_LAB_HOST set it)
constexpr int kMenuTimeoutMs = 1500;       // helper unreachable -> fall back (A9)
constexpr int kPmsBrowseTimeoutMs = 15000; // plexHttpGet's default, now cancellable
constexpr int kTicketTimeoutMs = 5000;

enum class BrowseV2Setting { Off, On, Invalid };

inline BrowseV2Setting parseBrowseV2(const char* raw) {
    if (!raw || !*raw || std::strcmp(raw, "0") == 0)
        return BrowseV2Setting::Off;
    if (std::strcmp(raw, "1") == 0)
        return BrowseV2Setting::On;
    return BrowseV2Setting::Invalid;
}

inline BrowseV2Setting browseV2FromEnvironment() {
    return parseBrowseV2(std::getenv(kBrowseV2Env));
}

// "" when off (unset or 0): nothing to say, so the flag-off log is unchanged.
inline std::string formatBrowseV2Notice(BrowseV2Setting s, const char* raw,
                                        const std::string& menuUrl, bool navCommands = false) {
    if (s == BrowseV2Setting::On)
        return "misterplexd: LAB MPX_BROWSE_V2=1 -> browse via mpx-menu " + menuUrl +
               " (PMS sections fallback), mpx-browse worker, offset rows, off-lock paint and "
               "lock-free mailbox reads; input commands " +
               (navCommands ? std::string("1-11 (MPX_BROWSE_MVPB=1)") : std::string("1-5 (MVP-a)"));
    if (s == BrowseV2Setting::Invalid) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "misterplexd: LAB MPX_BROWSE_V2='%.32s' invalid (want 0 or 1) -> off",
                      raw ? raw : "");
        return buf;
    }
    return std::string();
}

// MPX_MENU_URL (full URL), else MPX_MENU_BASE + "/menu", else empty (no hardcoded lab host).
inline std::string menuUrlFromEnvironment(const char* url, const char* base) {
    if (url && *url)
        return url;
    if (base && *base) {
        std::string b = base;
        while (!b.empty() && b.back() == '/')
            b.pop_back();
        return b + "/menu";
    }
    return kDefaultMenuUrl;
}

// ---- XML ----

inline std::string xmlUnescape(const std::string& s) {
    if (s.find('&') == std::string::npos)
        return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        bool ok = true;
        if (ent == "amp")
            cp = '&';
        else if (ent == "lt")
            cp = '<';
        else if (ent == "gt")
            cp = '>';
        else if (ent == "quot")
            cp = '"';
        else if (ent == "apos")
            cp = '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            char* end = nullptr;
            cp = (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, &end, 16)
                                                  : std::strtoul(ent.c_str() + 1, &end, 10);
            ok = end && *end == '\0';
        } else {
            ok = false;
        }
        if (!ok) {
            out += s[i];
            continue;
        }
        // Plex titles use typographic punctuation as numeric entities (&#8217; for ’).
        // Keep them as UTF-8; the UI font draws the glyph or an ASCII stand-in.
        auto appendCp = [&](unsigned long u) {
            if (u < 0x20 || u > 0x10FFFF) {
                out += '?';
                return;
            }
            if (u < 0x80) {
                out += static_cast<char>(u);
                return;
            }
            if (u < 0x800) {
                out += static_cast<char>(0xC0 | (u >> 6));
                out += static_cast<char>(0x80 | (u & 0x3F));
                return;
            }
            if (u < 0x10000) {
                out += static_cast<char>(0xE0 | (u >> 12));
                out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (u & 0x3F));
                return;
            }
            out += static_cast<char>(0xF0 | (u >> 18));
            out += static_cast<char>(0x80 | ((u >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        };
        appendCp(cp);
        i = semi;
    }
    return out;
}

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;

    const std::string* find(const char* attr) const {
        for (const auto& a : attrs)
            if (a.first == attr)
                return &a.second;
        return nullptr;
    }
    std::string get(const char* attr) const {
        const std::string* v = find(attr);
        return v ? *v : std::string();
    }
};

// The start tags of xml in document order (no text, no end tags), with their attributes
// unescaped. Attribute names match whole (ratingKey never matches key).
inline std::vector<XmlElement> parseXmlStartTags(const std::string& xml) {
    std::vector<XmlElement> out;
    size_t i = 0;
    while ((i = xml.find('<', i)) != std::string::npos) {
        ++i;
        if (i >= xml.size())
            break;
        const char c = xml[i];
        if (c == '/' || c == '?' || c == '!') {
            const size_t e = xml.find('>', i);
            if (e == std::string::npos)
                break;
            i = e + 1;
            continue;
        }
        XmlElement el;
        while (i < xml.size() && !std::isspace(static_cast<unsigned char>(xml[i])) &&
               xml[i] != '>' && xml[i] != '/')
            el.name += xml[i++];
        for (;;) {
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i >= xml.size() || xml[i] == '>' || xml[i] == '/')
                break;
            std::string name;
            while (i < xml.size() && xml[i] != '=' && xml[i] != '>' && xml[i] != '/' &&
                   !std::isspace(static_cast<unsigned char>(xml[i])))
                name += xml[i++];
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i >= xml.size() || xml[i] != '=') {
                if (name.empty())
                    ++i; // stray byte: never loop on it
                continue;
            }
            ++i;
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i >= xml.size() || (xml[i] != '"' && xml[i] != '\''))
                continue;
            const char q = xml[i++];
            const size_t e = xml.find(q, i);
            if (e == std::string::npos) {
                i = xml.size();
                break;
            }
            el.attrs.emplace_back(name, xmlUnescape(xml.substr(i, e - i)));
            i = e + 1;
        }
        const size_t e = xml.find('>', i);
        if (e == std::string::npos)
            break;
        i = e + 1;
        if (!el.name.empty())
            out.push_back(std::move(el));
    }
    return out;
}

inline bool parseMs(const std::string& s, int64_t& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (!end || *end != '\0' || v < 0)
        return false;
    out = v;
    return true;
}

struct BrowsePage {
    std::vector<LibraryRow> rows;
    std::string header;       // MediaContainer@mpxHeader, else @title1 (may be empty)
    std::string mpxPage;      // /a1 helper: MediaContainer@mpxPage ("21-40 / 812"), else empty
    bool isMediaContainer = false;
};

// Rows of a helper (fromMenu) or PMS MediaContainer in document order: Directory rows are
// directories; Video and Track rows play. Helper rows also carry the mpx* attributes.
inline BrowsePage parseBrowseXml(const std::string& xml, bool fromMenu) {
    BrowsePage page;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name == "MediaContainer") {
            if (!page.isMediaContainer) {
                page.isMediaContainer = true;
                page.header = el.get("mpxHeader");
                if (page.header.empty())
                    page.header = el.get("title1");
                if (fromMenu)
                    page.mpxPage = el.get("mpxPage");
            }
            continue;
        }
        const bool dir = el.name == "Directory";
        if (!dir && el.name != "Video" && el.name != "Track")
            continue;
        LibraryRow r;
        r.title = el.get("title");
        r.key = el.get("key");
        r.ratingKey = el.get("ratingKey");
        r.type = el.get("type");
        r.directory = dir;
        if (r.title.empty() && r.key.empty() && r.ratingKey.empty())
            continue;
        if (fromMenu) {
            r.fromMenu = true;
            r.mpxServer = el.get("mpxServer");
            r.mpxSource = el.get("mpxSource");
            r.mpxKind = el.get("mpxKind");
            r.offsetPresent = parseMs(el.get("mpxOffsetMs"), r.offsetMs);
            const std::string pct = el.get("mpxPct");
            if (!pct.empty() && pct.size() <= 3 &&
                pct.find_first_not_of("0123456789") == std::string::npos) {
                const int v = std::atoi(pct.c_str());
                if (v <= 100)
                    r.mpxPct = v;
            }
        }
        page.rows.push_back(std::move(r));
    }
    return page;
}

inline bool browseRowActivatable(const LibraryRow& r) {
    if (r.mpxKind == "info" || r.mpxKind == "offline" || r.mpxKind == "header")
        return false;
    if (r.directory)
        return !r.key.empty();
    return !r.ratingKey.empty() || (r.fromMenu && !r.key.empty());
}

// A helper paging row (contract "mpxpage"; the live helper: type/mpxKind "page", NEXT / PREV):
// it replaces the current page instead of opening a level, so Back goes to the list's parent.
inline bool browseRowIsPage(const LibraryRow& r) {
    return r.fromMenu && r.directory &&
           (r.type == "mpxpage" || r.type == "page" || r.mpxKind == "page");
}

inline bool browsePageRowIsNext(const LibraryRow& r) {
    return browseRowIsPage(r) && r.title.find("NEXT") != std::string::npos;
}
inline bool browsePageRowIsPrev(const LibraryRow& r) {
    return browseRowIsPage(r) && r.title.find("PREV") != std::string::npos;
}

// ---- helper URLs ----

// scheme://host[:port] of url ("" if it has none).
inline std::string urlOrigin(const std::string& url) {
    const size_t s = url.find("://");
    if (s == std::string::npos)
        return {};
    const size_t p = url.find('/', s + 3);
    return p == std::string::npos ? url : url.substr(0, p);
}

inline bool hasQueryParam(const std::string& url, const char* name) {
    const size_t q = url.find('?');
    if (q == std::string::npos)
        return false;
    const std::string key = std::string(name) + "=";
    size_t i = q + 1;
    while (i < url.size()) {
        if (url.compare(i, key.size(), key) == 0)
            return true;
        const size_t amp = url.find('&', i);
        if (amp == std::string::npos)
            break;
        i = amp + 1;
    }
    return false;
}

inline std::string withQueryParam(std::string url, const char* name, const std::string& value) {
    if (hasQueryParam(url, name))
        return url;
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += name;
    url += '=';
    url += value;
    return url;
}

// The helper URL for a row key: absolute URLs as they are, "/..." on the helper's origin,
// anything else under the menu URL. cols (the raster's title width) is added when missing.
inline std::string menuUrlForKey(const std::string& menuUrl, const std::string& key, int cols) {
    std::string url;
    if (key.rfind("http://", 0) == 0 || key.rfind("https://", 0) == 0)
        url = key;
    else if (!key.empty() && key[0] == '/')
        url = urlOrigin(menuUrl) + key;
    else if (key.empty())
        url = menuUrl;
    else
        url = menuUrl + (menuUrl.empty() || menuUrl.back() == '/' ? "" : "/") + key;
    return cols > 0 ? withQueryParam(url, "cols", std::to_string(cols)) : url;
}

// For logs: the URL without its query (helper paths carry no token, PMS URLs do).
inline std::string urlForLog(const std::string& url) {
    const size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q) + "?...";
}

inline std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- play tickets ----

// What a browse play hands to main.cpp's PlayRequest: a phone cast's fields (build kit §4.3).
// address/port/protocol/token empty = the conf PMS and token.
struct BrowsePlayTicket {
    std::string ratingKey;
    std::string key;          // "/library/metadata/<rk>"
    std::string title;
    std::string mpxServer;
    std::string source;       // "pms" (fallback list), "local", "ladder"
    bool offsetPresent = false;
    int64_t offsetMs = 0;
    std::string address, port, protocol, token, machineIdentifier;
    // The helper's ladder input (mpxLadderSrc, percent-decoded once): the original Part or,
    // above ~8 Mbps (lead Q4), the owning server's own transcode. Empty: doPlay uses the Part.
    std::string ladderSrc;
    // The ticket's mpxLadderInput (lead Q4): "part" or "server-transcode" (doPlay then asks
    // the owning server for its own transcode as the ladder input).
    std::string ladderInput;
    std::string ticketUrl;    // the helper URL this ticket came from ("" for local/PMS rows)
    std::string error;        // non-empty: not playable (shown in the status line)
};

// The server of a Part URL ("http://h:32400/library/parts/1/2/f.mkv?X-Plex-Token=T"): what a
// cast of that server carries. False when it is not an http(s) URL with a host.
inline bool serverFromPartUrl(const std::string& partUrl, BrowsePlayTicket& t) {
    const size_t s = partUrl.find("://");
    if (s == std::string::npos)
        return false;
    const std::string scheme = partUrl.substr(0, s);
    if (scheme != "http" && scheme != "https")
        return false;
    const size_t hostEnd = partUrl.find_first_of("/?", s + 3);
    std::string host = partUrl.substr(s + 3, hostEnd == std::string::npos ? std::string::npos
                                                                         : hostEnd - (s + 3));
    if (host.empty())
        return false;
    std::string port;
    if (host[0] == '[') { // [v6]:port: the address keeps its brackets, as a cast base needs
        const size_t rb = host.find(']');
        if (rb == std::string::npos)
            return false;
        if (rb + 1 < host.size() && host[rb + 1] == ':')
            port = host.substr(rb + 2);
        host = host.substr(0, rb + 1);
    } else {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos) {
            port = host.substr(colon + 1);
            host = host.substr(0, colon);
        }
    }
    if (port.empty())
        port = scheme == "https" ? "443" : "80";
    t.protocol = scheme;
    t.address = host;
    t.port = port;
    const size_t q = partUrl.find('?');
    if (q != std::string::npos) {
        size_t i = q + 1;
        while (i <= partUrl.size()) {
            const size_t amp = partUrl.find('&', i);
            const std::string kv =
                partUrl.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
            if (kv.rfind("X-Plex-Token=", 0) == 0)
                t.token = percentDecode(kv.substr(13));
            if (amp == std::string::npos)
                break;
            i = amp + 1;
        }
    }
    return true;
}

// A helper error answer (contract §9.2): MediaContainer@mpxError ("OFFLINE", "NOT FOUND",
// "UNPLAYABLE", ...), on any HTTP status. "" when there is none.
inline std::string browseContainerError(const std::string& xml) {
    for (const auto& e : parseXmlStartTags(xml))
        if (e.name == "MediaContainer")
            return e.get("mpxError");
    return {};
}

// The helper's answer to a "ladder" row's key (wT: /menu/resolve/<server>/<rk>; build kit:
// /menu/s/<mid>/ticket/<rk>). Explicit server fields (MpxPlay or Video with address/port/
// protocol/token) win; else the server of mpxLadderSrc (the Part URL the helper picked). Either
// way the play goes through doPlay unchanged: it resolves on that server, posts the timeline
// there and builds /ladder.ts with this raster's rules. row: the activated row.
inline BrowsePlayTicket parseBrowseTicket(const std::string& xml, const LibraryRow& row) {
    BrowsePlayTicket t;
    t.ratingKey = row.ratingKey;
    t.title = row.title;
    t.mpxServer = row.mpxServer;
    t.source = "ladder";
    t.offsetPresent = row.offsetPresent;
    t.offsetMs = row.offsetMs;
    const XmlElement* el = nullptr;
    const auto tags = parseXmlStartTags(xml);
    const std::string containerError = browseContainerError(xml);
    if (!containerError.empty()) {
        t.error = containerError;
        return t;
    }
    for (const auto& e : tags)
        if (e.name == "MpxPlay") {
            el = &e;
            break;
        }
    if (!el)
        for (const auto& e : tags)
            if (e.name == "Video" || e.name == "Track") {
                el = &e;
                break;
            }
    if (!el) {
        t.error = "NO PLAY TICKET";
        return t;
    }
    const std::string err = el->get("mpxError");
    if (!err.empty() || el->get("mpxUnplayable") == "1" || el->get("unplayable") == "1" ||
        el->get("mpxPlayable") == "no") {
        const std::string reason = el->get("mpxReason");
        t.error = !err.empty() ? err : (reason.empty() ? "UNPLAYABLE" : reason);
        return t;
    }
    t.ladderInput = el->get("mpxLadderInput");
    if (!el->get("ratingKey").empty())
        t.ratingKey = el->get("ratingKey");
    int64_t off = 0;
    if (parseMs(el->get("mpxOffsetMs"), off)) {
        t.offsetPresent = true;
        t.offsetMs = off;
    }
    t.address = el->get("address");
    t.port = el->get("port");
    t.protocol = el->get("protocol");
    t.token = el->get("token");
    if (t.token.empty())
        t.token = el->get("accessToken");
    t.machineIdentifier = el->get("machineIdentifier");
    const std::string key = el->get("key");
    if (key.rfind("/library/metadata/", 0) == 0)
        t.key = key;
    const std::string src = el->get("mpxLadderSrc");
    if (!src.empty())
        t.ladderSrc = percentDecode(src);
    // The helper's own server ("local", the conf PMS): its ticket address is how lab-pc reaches
    // that PMS (127.0.0.1 there), not how this MiSTer does. Play it on the conf PMS and token,
    // from the original Part as today (contract §9.3: a local server always takes the Part).
    if (row.mpxServer == "local") {
        t.source = "local";
        t.address.clear();
        t.port.clear();
        t.protocol.clear();
        t.token.clear();
        t.machineIdentifier.clear();
        t.ladderSrc.clear();
        t.ladderInput.clear();
    } else if (t.address.empty()) {
        BrowsePlayTicket fromPart;
        if (src.empty() || !serverFromPartUrl(t.ladderSrc, fromPart)) {
            t.error = "NO SERVER IN PLAY TICKET";
            return t;
        }
        t.address = fromPart.address;
        t.port = fromPart.port;
        t.protocol = fromPart.protocol;
        if (t.token.empty())
            t.token = fromPart.token;
    }
    if (t.ratingKey.empty()) {
        t.error = "NO RATINGKEY IN PLAY TICKET";
        return t;
    }
    if (t.key.empty())
        t.key = "/library/metadata/" + t.ratingKey;
    if (!t.offsetPresent) { // a resume prompt row always says where to start
        t.offsetPresent = true;
        t.offsetMs = 0;
    }
    return t;
}

// A "local" helper row or a PMS fallback row: play by ratingKey on the conf PMS. Helper rows
// carry the start (mpxOffsetMs); PMS rows keep today's automatic resume (offset absent).
inline BrowsePlayTicket localBrowseTicket(const LibraryRow& row) {
    BrowsePlayTicket t;
    std::string id = row.ratingKey;
    if (id.empty() && !row.key.empty() && librarylist::browseListPlayMediaKey(row.key) == row.key)
        id = row.key;
    if (id.empty()) {
        t.key = "/library/metadata/";
    } else {
        t.key = librarylist::browseListPlayMediaKey(id);
        t.ratingKey = librarylist::browseListPlayRatingKey(id);
    }
    t.title = row.title;
    t.mpxServer = row.mpxServer;
    t.source = row.fromMenu ? "local" : "pms";
    if (row.fromMenu) {
        t.offsetPresent = true;
        t.offsetMs = row.offsetPresent ? row.offsetMs : 0;
    }
    return t;
}

// The helper ticket URL for another start: its "o<ms>" path segment (contract §8,
// /menu/ticket/<slug>/<rk>/o<ms>, with the /a1 helper's trailing "/a1" after it) set to
// offMs, any query kept. "" when there is no such segment, so the caller falls back instead of
// asking for a wrong ticket.
inline std::string browseTicketUrlAtOffset(const std::string& ticketUrl, int64_t offMs) {
    const size_t q = ticketUrl.find('?');
    std::string path = q == std::string::npos ? ticketUrl : ticketUrl.substr(0, q);
    std::string tail;
    if (path.size() > 3 && path.compare(path.size() - 3, 3, "/a1") == 0) {
        tail = "/a1";
        path.resize(path.size() - 3);
    }
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash + 2 >= path.size() || path[slash + 1] != 'o')
        return {};
    for (size_t i = slash + 2; i < path.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(path[i])))
            return {};
    return path.substr(0, slash + 2) + std::to_string(offMs < 0 ? 0 : offMs) + tail +
           (q == std::string::npos ? std::string() : ticketUrl.substr(q));
}

// ---- paint snapshot ----

// What the paint needs, copied under the browse lock: only the rows on screen, with the
// cursor inside them. renderLibraryI420Titled draws it pixel for pixel like the full list.
struct BrowseView {
    LibraryBrowser window;
    std::string header;
};

// R1 (hostIdleFb) renderer input: the whole current level (browse_render.hpp scrolls its own
// 15-row viewport), the level URL (servers/root/list/detail) and the /a1 page indicator.
struct BrowseR1View {
    bool visible = false;
    std::vector<LibraryRow> rows;
    int cursor = 0;
    std::string status;
    std::string header;
    std::string mpxPage;
    std::string levelUrl;
    bool fromMenu = false;
};

inline BrowseView browseWindow(const LibraryBrowser& br, const std::string& header, int w,
                               int h) {
    BrowseView v;
    v.header = header;
    v.window.visible = br.visible;
    v.window.status = br.status;
    if (br.stack.empty())
        return v;
    const auto& rows = br.stack.back();
    const int maxRows = libraryRowsOnScreen(w, h);
    const int cur = br.cursor();
    const int start = cur >= maxRows ? cur - maxRows + 1 : 0;
    std::vector<LibraryRow> win;
    for (int i = start; i < static_cast<int>(rows.size()) && i < start + maxRows; ++i)
        win.push_back(rows[static_cast<size_t>(i)]);
    v.window.stack.push_back(std::move(win));
    v.window.cursors.push_back(cur - start);
    return v;
}

// Characters a row title may use on this raster (the line is "> " + title).
inline int browseTitleCols(int w) { return std::max(8, libraryCharsPerLine(w) - 2); }

} // namespace misterplex
