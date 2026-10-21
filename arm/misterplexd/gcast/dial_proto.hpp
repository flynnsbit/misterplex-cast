// SSDP/DIAL pieces for dial_lounge_fe (GCAST P3). Pure string functions so the
// responder and the tiny DIAL HTTP server can be tested and fuzzed off-box.
#pragma once
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace misterplex {
namespace gcast {
namespace dial {

constexpr const char* kDialST = "urn:dial-multiscreen-org:service:dial:1";
constexpr size_t kMaxHttpHead = 8192;
constexpr size_t kMaxHttpBody = 4096;

inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Header block parser shared by SSDP and HTTP. Names are lower-cased.
struct Head {
    std::string method, target, version;
    std::map<std::string, std::string> h;
    std::string get(const std::string& k) const { auto it = h.find(k); return it == h.end() ? std::string() : it->second; }
};
inline bool parseHead(const std::string& raw, Head& out) {
    if (raw.size() > kMaxHttpHead) return false;
    size_t eol = raw.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string line = raw.substr(0, eol);
    size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return false;
    out.method = line.substr(0, s1);
    out.target = line.substr(s1 + 1, s2 - s1 - 1);
    out.version = line.substr(s2 + 1);
    if (out.version.rfind("HTTP/1.", 0) != 0) return false;
    size_t pos = eol + 2;
    int n = 0;
    while (pos < raw.size()) {
        size_t e = raw.find("\r\n", pos);
        if (e == std::string::npos) e = raw.size();
        if (e == pos) break;
        if (++n > 64) return false;
        std::string l = raw.substr(pos, e - pos);
        size_t c = l.find(':');
        if (c != std::string::npos) out.h[lower(trim(l.substr(0, c)))] = trim(l.substr(c + 1));
        pos = e + 2;
    }
    return true;
}

// M-SEARCH matching: DIAL ST or ssdp:all. Returns MX clamped to [1,5], 0 = ignore.
inline int dialSearchMx(const std::string& pkt) {
    Head h;
    if (!parseHead(pkt, h) || h.method != "M-SEARCH" || h.target != "*") return 0;
    if (lower(h.get("man")).find("ssdp:discover") == std::string::npos) return 0;
    std::string st = h.get("st");
#ifndef MUT_DIAL_ANY_ST
    if (st != kDialST && st != "ssdp:all") return 0;
#endif
    int mx = std::atoi(h.get("mx").c_str());
    if (mx < 1) mx = 1;
    if (mx > 5) mx = 5;
    return mx;
}

inline std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break; case '\'': o += "&apos;"; break;
        default: if ((unsigned char)c >= 0x20 || c == '\t') o += c;
        }
    }
    return o;
}

inline std::string ssdpResponse(const std::string& ip, int port, const std::string& uuid, long bootId) {
    return "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: http://" + ip + ":" + std::to_string(port) +
           "/dd.xml\r\nSERVER: Linux/3 UPnP/1.1 misterplexd/1\r\nST: " + kDialST + "\r\nUSN: uuid:" + uuid + "::" + kDialST +
           "\r\nBOOTID.UPNP.ORG: " + std::to_string(bootId) + "\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n";
}
inline std::string ssdpNotify(const std::string& ip, int port, const std::string& uuid, long bootId) {
    return "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\nLOCATION: http://" + ip + ":" +
           std::to_string(port) + "/dd.xml\r\nNT: " + kDialST + "\r\nNTS: ssdp:alive\r\nSERVER: Linux/3 UPnP/1.1 misterplexd/1\r\nUSN: uuid:" +
           uuid + "::" + kDialST + "\r\nBOOTID.UPNP.ORG: " + std::to_string(bootId) + "\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n";
}

inline std::string deviceDescXml(const std::string& name, const std::string& uuid) {
    return "<?xml version=\"1.0\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
           "<device><deviceType>urn:dial-multiscreen-org:device:dial:1</deviceType><friendlyName>" + xmlEscape(name) +
           "</friendlyName><manufacturer>MiSTer</manufacturer><modelName>misterplexd</modelName><UDN>uuid:" + xmlEscape(uuid) +
           "</UDN><serviceList><service><serviceType>urn:dial-multiscreen-org:service:dial:1</serviceType>"
           "<serviceId>urn:dial-multiscreen-org:serviceId:dial</serviceId><controlURL>/ssdp/notfound</controlURL>"
           "<eventSubURL>/ssdp/notfound</eventSubURL><SCPDURL>/ssdp/notfound</SCPDURL></service></serviceList></device></root>\n";
}
inline std::string appXml(bool running, const std::string& screenId, const std::string& deviceId) {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<service xmlns=\"urn:dial-multiscreen-org:schemas:dial\" dialVer=\"2.1\">"
                    "<name>YouTube</name><options allowStop=\"true\"/><state>";
    x += running ? "running" : "stopped";
    x += "</state>";
    if (running) x += "<link rel=\"run\" href=\"run\"/>";
    if (!screenId.empty())
        x += "<additionalData><screenId>" + xmlEscape(screenId) + "</screenId><theme>cl</theme><deviceId>" + xmlEscape(deviceId) +
             "</deviceId></additionalData>";
    return x + "</service>\n";
}

// application/x-www-form-urlencoded -> map (bounded).
inline std::map<std::string, std::string> formDecode(const std::string& s) {
    std::map<std::string, std::string> m;
    auto dec = [](const std::string& in) {
        std::string o;
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '+') o += ' ';
            else if (in[i] == '%' && i + 2 < in.size() && std::isxdigit((unsigned char)in[i + 1]) && std::isxdigit((unsigned char)in[i + 2])) {
                o += char(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2;
            } else o += in[i];
        }
        return o;
    };
    size_t pos = 0; int n = 0;
    while (pos <= s.size() && n < 32) {
        size_t amp = s.find('&', pos); if (amp == std::string::npos) amp = s.size();
        std::string kv = s.substr(pos, amp - pos);
        if (!kv.empty()) { size_t eq = kv.find('='); m[dec(kv.substr(0, eq))] = eq == std::string::npos ? "" : dec(kv.substr(eq + 1)); ++n; }
        pos = amp + 1;
    }
    return m;
}

// DIAL HTTP routing. The server fills Ctx; handle() decides the reply and the
// side effect (launch with pairingCode / videoId, or stop).
struct Ctx {
    std::string name, uuid, screenId, appUrl;  // appUrl = "http://ip:port/apps/"
    bool running = false;
    // YouTube DIAL stays unanswered until Lounge has connected. Default true so a
    // caller that does not know about the gate still behaves as before.
    bool youtubeReady = true;
};
inline bool youtubeAppPath(const std::string& target) {
    const std::string path = target.substr(0, target.find('?'));
    return path == "/apps/YouTube" || path == "/apps/YouTube/" || path == "/apps/YouTube/run";
}
struct Reply {
    int status = 404;
    std::string contentType, body, location, applicationUrl;
    bool launch = false, stop = false;
    std::string pairingCode, videoId;
    long long startMs = 0;
};
inline bool validVid(const std::string& v) {
    if (v.size() != 11) return false;
    for (char c : v) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    return true;
}
inline Reply handle(const Head& h, const std::string& body, const Ctx& c) {
    Reply r;
    std::string path = h.target.substr(0, h.target.find('?'));
    // No YouTube answer and no launch until Lounge is up. Device description stays.
    if (!c.youtubeReady && youtubeAppPath(h.target)) {
        r.status = 404;
        return r;
    }
    if (h.method == "GET" && (path == "/dd.xml" || path == "/ssdp/device-desc.xml")) {
        r.status = 200; r.contentType = "text/xml; charset=\"utf-8\""; r.body = deviceDescXml(c.name, c.uuid);
        r.applicationUrl = c.appUrl; return r;
    }
    if (path == "/apps/YouTube" || path == "/apps/YouTube/") {
        if (h.method == "GET") { r.status = 200; r.contentType = "text/xml; charset=\"utf-8\""; r.body = appXml(c.running, c.screenId, c.uuid); return r; }
        if (h.method == "POST") {
#ifndef MUT_DIAL_NO_BODYCAP
            if (body.size() > kMaxHttpBody) { r.status = 413; return r; }
#endif
            auto f = formDecode(body);
            r.status = 201; r.location = c.appUrl + "YouTube/run"; r.launch = true;
            r.pairingCode = f.count("pairingCode") ? f["pairingCode"] : "";
            if (r.pairingCode.size() > 64) r.pairingCode.clear();
            if (f.count("v") && validVid(f["v"])) {
                r.videoId = f["v"];
                double t = std::atof(f.count("t") ? f["t"].c_str() : "0");
                r.startMs = (t > 0 && t < 604800) ? (long long)(t * 1000) : 0;
            }
            return r;
        }
        r.status = 405; return r;
    }
    if (path == "/apps/YouTube/run") {
        if (h.method == "DELETE") { r.status = c.running ? 200 : 404; r.stop = c.running; return r; }
        r.status = 405; return r;
    }
    return r;
}

}  // namespace dial
}  // namespace gcast
}  // namespace misterplex
