#pragma once
// Phone status page + one-tap update router (frontier item 3). Pure: no sockets, no files, no clock, no RNG.
//   GET  /misterplex/status  -> one small static HTML page (no JS, no external assets, readable at phone width)
//   POST /misterplex/update  -> body "nonce=<hex>[&confirm=1]"; only ever asks the caller to START item 5's flow
// Everything shown goes through field(): printable ASCII, capped, HTML-escaped, and a value that looks like a
// secret (token, URL, e-mail) is replaced by "[hidden]". The nonce is per boot and single use (the caller rotates it
// after a Start). A POST is refused for a wrong or missing nonce, a foreign Origin, while playing without
// confirm=1, or when no update is available. Binding spec: docs/PHONE-STATUS-PAGE.md.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace misterplex {
namespace phonestatus {

constexpr const char* kStatusPath = "/misterplex/status";
constexpr const char* kUpdatePath = "/misterplex/update";
constexpr size_t kFieldMax = 64;
constexpr size_t kBodyCap = 256;

// Checkable = updates are on (conf + a real key) but the daemon does not know yet whether a newer build exists;
// the button asks the daemon to check and update if one is newer.
enum class UpdateState { Unavailable, UpToDate, Available, Busy, Checkable };

struct Status {
    std::string version, commit, md5;     // the daemon
    std::string coreId, pairedRaster;     // the loaded core
    bool playing = false;
    std::string title, mode;              // only shown while playing
    int64_t positionSec = 0;
    std::string helper;                   // "up", "down", "off", else "unknown"
    UpdateState update = UpdateState::Unavailable;
    std::string latest;                   // newest daemon version when update == Available
    std::string autoPicture;              // auto-picture advice line ("" = row hidden); sanitised by field()
    std::string nonce;                    // per boot, hex
};

struct Reply {
    int code = 200;
    std::string contentType = "text/plain";
    std::string body;
    bool startUpdate = false;             // true only for an accepted POST /misterplex/update
};

inline bool lower(const std::string& s, const char* needle) {
    std::string l;
    for (const char c : s)
        l += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    return l.find(needle) != std::string::npos;
}

inline std::string escapeHtml(const std::string& s) {
    std::string o;
    for (const char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

inline std::string field(const std::string& v) {
    std::string o;
    for (const char c : v) {
        if (o.size() >= kFieldMax)
            break;
        const unsigned char u = static_cast<unsigned char>(c);
        o += (u >= 0x20 && u <= 0x7e) ? c : '?';
    }
#ifndef MUT_PS_NO_REDACT
    if (lower(o, "token") || lower(o, "://") || o.find('@') != std::string::npos)
        o = "[hidden]";
#endif
#ifdef MUT_PS_NO_ESCAPE
    return o;
#else
    return escapeHtml(o);
#endif
}

inline std::string hexNonce(const uint8_t* bytes, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; ++i) {
        o += d[bytes[i] >> 4];
        o += d[bytes[i] & 15];
    }
    return o;
}

inline const char* updateText(UpdateState u) {
    switch (u) {
    case UpdateState::Available: return "a newer daemon is available";
    case UpdateState::UpToDate: return "up to date";
    case UpdateState::Busy: return "update running";
    case UpdateState::Checkable: return "updates on (check and update from here)";
    default: return "update: not available";
    }
}

inline std::string render(const Status& s) {
    std::string h = "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
                    "content=\"width=device-width,initial-scale=1\"><title>MiSTer Plex</title></head><body>"
                    "<h1>MiSTer Plex</h1><table>";
    auto row = [&h](const char* k, const std::string& v) { h += std::string("<tr><th>") + k + "</th><td>" + v + "</td></tr>"; };
    row("daemon", field(s.version) + " " + field(s.commit));
    row("md5", field(s.md5));
    row("core", field(s.coreId) + " (" + field(s.pairedRaster) + ")");
    if (s.playing)
        row("playing", field(s.title) + " " + std::to_string(s.positionSec < 0 ? 0 : s.positionSec) + "s " + field(s.mode));
    else
        row("playing", "nothing");
    const std::string hp = (s.helper == "up" || s.helper == "down" || s.helper == "off") ? s.helper : "unknown";
    row("helper", hp);
    if (!s.autoPicture.empty()) row("auto-picture", field(s.autoPicture));
    row("update", std::string(updateText(s.update)) + (s.update == UpdateState::Available && !s.latest.empty() ? " (" + field(s.latest) + ")" : ""));
    h += "</table>";
#ifdef MUT_PS_BUTTON_ALWAYS
    if (true) {
#else
    if (s.update == UpdateState::Available || s.update == UpdateState::Checkable) {
#endif
        h += "<form method=\"post\" action=\"/misterplex/update\"><input type=\"hidden\" name=\"nonce\" value=\"" +
             field(s.nonce) + "\">";
        if (s.playing)
            h += "<label><input type=\"checkbox\" name=\"confirm\" value=\"1\"> update while playing</label> ";
        h += "<button type=\"submit\">Update</button></form>";
    }
    return h + "</body></html>\n";
}

inline std::string headerValue(const std::string& req, const char* name) {
    const std::string key = std::string("\n") + name + ":";
    std::string l;
    for (const char c : req.substr(0, req.find("\r\n\r\n")))
        l += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    const size_t p = l.find(key);
    if (p == std::string::npos)
        return std::string();
    const size_t b = p + key.size();
    const size_t e = req.find_first_of("\r\n", b);
    std::string v = req.substr(b, e == std::string::npos ? std::string::npos : e - b);
    while (!v.empty() && v.front() == ' ')
        v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.pop_back();
    return v;
}

inline bool originOk(const std::string& req) {
#ifdef MUT_PS_ORIGIN_IGNORED
    (void)req;
    return true;
#else
    const std::string o = headerValue(req, "origin");
    if (o.empty())
        return true;  // not a browser cross-site form post
    const std::string pre = "http://";
    const std::string host = headerValue(req, "host");
    return !host.empty() && o.compare(0, pre.size(), pre) == 0 && o.substr(pre.size()) == host;
#endif
}

inline bool nonceEq(const std::string& a, const std::string& b) {
    if (a.size() < 16 || a.size() != b.size())
        return false;
#ifdef MUT_PS_NONCE_PREFIX
    return a.compare(0, 8, b, 0, 8) == 0;
#else
    unsigned d = 0;
    for (size_t i = 0; i < a.size(); ++i)
        d |= static_cast<unsigned>(a[i] ^ b[i]);
    return d == 0;
#endif
}

// nullopt = not one of our paths (the companion carries on as before).
inline std::optional<Reply> route(const std::string& req, const Status& s) {
    const std::string line = req.substr(0, req.find_first_of("\r\n"));
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos)
        return std::nullopt;
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string verb = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, (sp2 == std::string::npos ? line.size() : sp2) - sp1 - 1);
    const size_t q = target.find_first_of("?#");
    const std::string path = target.substr(0, q);
    if (path != kStatusPath && path != kUpdatePath)
        return std::nullopt;
    Reply r;
    if (path == kStatusPath) {
        if (verb != "GET") { r.code = 405; r.body = "GET only\n"; return r; }
        if (q != std::string::npos) { r.code = 400; r.body = "no parameters\n"; return r; }
        r.contentType = "text/html";
        r.body = render(s);
        return r;
    }
#ifndef MUT_PS_ANY_VERB_UPDATE
    if (verb != "POST") { r.code = 405; r.body = "POST only\n"; return r; }
#endif
    if (q != std::string::npos) { r.code = 400; r.body = "no parameters\n"; return r; }
    const size_t hb = req.find("\r\n\r\n");
    const std::string body = hb == std::string::npos ? std::string() : req.substr(hb + 4);
    if (body.size() > kBodyCap) { r.code = 413; r.body = "too large\n"; return r; }
    if (!originOk(req)) { r.code = 403; r.body = "origin refused\n"; return r; }
    std::string nonce;
    bool confirm = false, haveN = false, haveC = false;
    size_t pos = 0;
    while (!body.empty()) {
        const size_t amp = body.find('&', pos);
        const std::string kv = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = kv.find('=');
        const std::string k = kv.substr(0, eq);
        const std::string v = eq == std::string::npos ? std::string() : kv.substr(eq + 1);
        if (k == "nonce" && !haveN && v.size() <= 64) { nonce = v; haveN = true; }
        else if (k == "confirm" && !haveC && v == "1") { confirm = true; haveC = true; }
        else { r.code = 400; r.body = "bad body\n"; return r; }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
#ifndef MUT_PS_NONCE_SKIP
    if (!nonceEq(nonce, s.nonce)) { r.code = 403; r.body = "bad nonce\n"; return r; }
#endif
    if (s.update == UpdateState::Busy) { r.code = 409; r.body = "update already running\n"; return r; }
    if (s.update != UpdateState::Available && s.update != UpdateState::Checkable) { r.code = 409; r.body = "update not available\n"; return r; }
#ifndef MUT_PS_PLAYING_NOCONFIRM
    if (s.playing && !confirm) { r.code = 409; r.body = "playing: confirm=1 to update anyway\n"; return r; }
#endif
    r.code = 202;
    r.body = "update started\n";
    r.startUpdate = true;
    return r;
}

}  // namespace phonestatus
}  // namespace misterplex
