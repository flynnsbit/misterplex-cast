#include "plex_resolve.hpp"
#include "queue_step.hpp"
#include "url_query_codec.hpp"
#include "../../assets/plex-profiles/fpga_profile.hpp"
#include "libmisterplex/p720_transcode_vf.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/core_id_select.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace misterplex {
namespace {

std::string shellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'')
            o += "'\\''";
        else
            o += c;
    }
    o += "'";
    return o;
}

std::string httpGet(const std::string& url, int timeoutSec = 15,
                    const std::string& extraHeaders = {},
                    const std::string& clientProfileName = defaultRequestProfileName()) {
    // Prefer curl (present on MiSTer); -k for plex.direct certs.
    std::ostringstream cmd;
    cmd << "curl -sS -g -k -L --http1.1 --connect-timeout 6 --max-time " << timeoutSec
        << " -H 'Accept: application/xml'"
        << " -H 'X-Plex-Client-Identifier: misterplex'"
        << " -H 'X-Plex-Product: Plex Web'"
        << " -H 'X-Plex-Version: 4.125.0'"
        << " -H 'X-Plex-Platform: Chrome'"
        << " -H 'X-Plex-Platform-Version: 120.0'"
        << " -H 'X-Plex-Device: Linux'"
        << " -H 'X-Plex-Device-Name: Chrome'"
        << (clientProfileName.empty() ? std::string()
                                      : " -H " + shellQuote("X-Plex-Client-Profile-Name: " + clientProfileName))
        << " -H 'X-Plex-Model: bundled'"
        << " -H 'X-Plex-Provides: player'";
    if (!extraHeaders.empty())
        cmd << extraHeaders;
    cmd << " " << shellQuote(url) << " 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p)
        return {};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    return out;
}

std::string curlHeaderArgs(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::ostringstream args;
    for (const auto& h : headers) {
        if (h.first.empty())
            continue;
        args << " -H " << shellQuote(h.first + ": " + h.second);
    }
    return args.str();
}

std::string makeSessionId() {
    // Seed from time so the first cast after every daemon restart is not always
    // mplex-9e3779b1 (n=1 * golden). PMS keeps per-session transcoder state;
    // reusing that id after a restart stalls ffmpeg on /universal/start.
    static std::atomic<uint32_t> n{static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^
        (static_cast<uint64_t>(::getpid()) * 0x9E3779B9u))};
    std::ostringstream o;
    o << "mplex-" << std::hex << (n.fetch_add(1) * 2654435761u);
    return o.str();
}

std::string hostOnly(const std::string& hostOrUrl) {
    std::string h = hostOrUrl;
    if (auto p = h.find("://"); p != std::string::npos)
        h = h.substr(p + 3);
    if (auto p = h.find('/'); p != std::string::npos)
        h = h.substr(0, p);
    if (auto p = h.find(':'); p != std::string::npos)
        h = h.substr(0, p);
    return h;
}

bool isUnreachableHost(const std::string& hostOrUrl) {
    const std::string h = hostOnly(hostOrUrl);
    if (h.rfind("172.17.", 0) == 0 || h.rfind("172.18.", 0) == 0 ||
        h.rfind("172.20.", 0) == 0 || h.rfind("172.21.", 0) == 0)
        return true;
    if (h.find("172-17-") != std::string::npos || h.find("172-18-") != std::string::npos ||
        h.find("172-20-") != std::string::npos || h.find("172-21-") != std::string::npos)
        return true;
    return false;
}

// Parse IPv4 from dotted host or leading dashed plex.direct label (a-b-c-d.…).
bool parseIpv4Host(const std::string& hostOrUrl, int out[4]) {
    out[0] = out[1] = out[2] = out[3] = -1;
    std::string h = hostOnly(hostOrUrl);
    if (h.empty())
        return false;
    if (h.find("plex.direct") != std::string::npos) {
        const auto dot = h.find('.');
        h = dot == std::string::npos ? h : h.substr(0, dot);
    }
    // Accept either dotted or dashed separators.
    int idx = 0, cur = -1;
    for (size_t i = 0; i <= h.size(); ++i) {
        const char c = i < h.size() ? h[i] : '.';
        if (c >= '0' && c <= '9') {
            if (cur < 0)
                cur = 0;
            cur = cur * 10 + (c - '0');
            if (cur > 255)
                return false;
        } else if (c == '.' || c == '-') {
            if (cur < 0 || idx >= 4)
                return false;
            out[idx++] = cur;
            cur = -1;
        } else {
            return false;
        }
    }
    return idx == 4;
}

bool isPrivateRfc1918Host(const std::string& hostOrUrl) {
    int o[4];
    if (!parseIpv4Host(hostOrUrl, o))
        return false;
    if (o[0] == 10)
        return true;
    if (o[0] == 192 && o[1] == 168)
        return true;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31)
        return true;
    return false;
}

// Exact private IPv4 match (lab PMS vs foreign home often share the same private /24
// numbering on different physical LANs — /24 is NOT sufficient).
bool samePrivateHost(const std::string& a, const std::string& b) {
    int aa[4], bb[4];
    if (!parseIpv4Host(a, aa) || !parseIpv4Host(b, bb))
        return false;
    return aa[0] == bb[0] && aa[1] == bb[1] && aa[2] == bb[2] && aa[3] == bb[3];
}

std::string attr(const std::string& xml, const char* tag, const char* name) {
    // First occurrence of <tag ... name="..."
    const std::string open = std::string("<") + tag;
    size_t tpos = 0;
    while ((tpos = xml.find(open, tpos)) != std::string::npos) {
        const size_t boundary = tpos + open.size();
        if (boundary >= xml.size() ||
            std::isspace(static_cast<unsigned char>(xml[boundary])) ||
            xml[boundary] == '>' || xml[boundary] == '/') {
            break;
        }
        tpos = boundary;
    }
    if (tpos == std::string::npos)
        return {};
    auto end = xml.find('>', tpos);
    if (end == std::string::npos)
        return {};
    const std::string slice = xml.substr(tpos, end - tpos);
    const std::string key = std::string(name) + "=\"";
    auto p = slice.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    auto e = slice.find('"', p);
    if (e == std::string::npos)
        return {};
    return slice.substr(p, e - p);
}

// Attr from a free-form XML slice (first name="..." only).
std::string attrIn(const std::string& slice, const char* name) {
    const std::string key = std::string(name) + "=\"";
    auto p = slice.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    auto e = slice.find('"', p);
    if (e == std::string::npos)
        return {};
    return slice.substr(p, e - p);
}

std::string lowerCopy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// H.264/AVC direct Part is preferred for STREAM host CAVLC recon (Baseline/Main).
bool videoCodecIsH264(const std::string& codecRaw) {
    const std::string c = lowerCopy(codecRaw);
    if (c.empty())
        return false;
    return c.find("h264") != std::string::npos || c.find("avc") != std::string::npos ||
           c.find("x264") != std::string::npos;
}

} // namespace

std::string plexHttpGet(const std::string& url, int timeoutSec,
                        const std::string& extraHeaders) {
    std::ostringstream cmd;
    cmd << "curl -sS -g -k -L --http1.1 --connect-timeout 6 --max-time " << timeoutSec
        << " -H 'Accept: application/xml'"
        << " -H 'X-Plex-Product: MiSTerPlex'"
        << " -H 'X-Plex-Provides: player'";
    if (!extraHeaders.empty())
        cmd << extraHeaders;
    cmd << " '" << url << "' 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p)
        return {};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    return out;
}

const std::vector<ModeResolveRow>& modeResolveTable() {
    static const std::vector<ModeResolveRow> rows = {
        {"240p", "320x240", 2500, 55, "h264", "baseline", "baseline,main", 30, "aac", 2},
        {"480p", "640x480", 2500, 60, "h264", "baseline", "baseline,main", 30, "aac", 2},
        {"480i", "720x480", 8000, 40, "h264", "baseline", "baseline,main", 30, "aac", 2},
    };
    return rows;
}

const ModeResolveRow* modeResolveRow(const std::string& modeOrResolution) {
    for (const auto& r : modeResolveTable()) {
        if (modeOrResolution == r.mode || modeOrResolution == r.videoResolution)
            return &r;
    }
    return nullptr;
}

bool applyModeResolve(const std::string& modeOrResolution, WeakLadder& weak) {
    const ModeResolveRow* r = modeResolveRow(modeOrResolution);
    if (!r)
        return false;
    weak.profileName = r->mode;
    weak.videoResolution = r->videoResolution;
    weak.maxVideoBitrateKbps = r->maxVideoBitrateKbps;
    weak.videoQuality = r->videoQuality;
    weak.videoCodec = r->videoCodec;
    weak.audioCodec = r->audioCodec;
    weak.h264Profile = r->h264Profile;
    weak.h264ProfileParam = r->h264ProfileParam;
    weak.h264Level = r->h264Level;
    weak.audioChannels = r->audioChannels;
    return true;
}

const std::vector<PlexTranscodeProfile>& plexTranscodeProfiles() {
    static const std::vector<PlexTranscodeProfile> profiles = [] {
        std::vector<PlexTranscodeProfile> p;
        for (const auto& r : modeResolveTable())
            p.push_back({r.mode, r.videoResolution, r.maxVideoBitrateKbps, r.videoQuality,
                         r.h264Profile, r.h264Level});
        p.push_back({"720p", "1280x720", 8000, 100, "main", 31});
        return p;
    }();
    return profiles;
}

std::string normalizePlexBase(const std::string& raw) {
    std::string s = raw;
    // trim
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                          s.front() == '\n'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n' || s.back() == '/'))
        s.pop_back();
    if (s.empty())
        return {};
    if (s.find("://") == std::string::npos) {
        // bare host or host:port → assume http
        s = "http://" + s;
    }
    // strip trailing slash again after scheme
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    // If no port in authority, default PMS :32400 (skip if user omitted intentionally
    // for reverse-proxy with scheme-only host — rare; LAN always wants 32400).
    auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        auto hostStart = scheme + 3;
        auto slash = s.find('/', hostStart);
        std::string auth =
            (slash == std::string::npos) ? s.substr(hostStart) : s.substr(hostStart, slash - hostStart);
        // IPv6 in brackets or host:port
        bool hasPort = false;
        if (!auth.empty() && auth.front() == '[') {
            auto br = auth.find(']');
            hasPort = (br != std::string::npos && br + 1 < auth.size() && auth[br + 1] == ':');
        } else {
            hasPort = (auth.rfind(':') != std::string::npos);
        }
        if (!hasPort && !auth.empty())
            s = s.substr(0, hostStart) + auth + ":32400" +
                (slash == std::string::npos ? std::string() : s.substr(slash));
    }
    return s;
}

std::vector<std::string> parsePlexServerList(const std::string& csvOrSingle) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        auto n = normalizePlexBase(cur);
        cur.clear();
        if (n.empty())
            return;
        for (const auto& e : out) {
            if (e == n)
                return;
        }
        out.push_back(n);
    };
    for (char c : csvOrSingle) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

std::vector<std::string> mergePlexServers(const std::string& serversCsv,
                                          const std::vector<std::string>& baseLines) {
    std::vector<std::string> out = parsePlexServerList(serversCsv);
    for (const auto& line : baseLines) {
        for (const auto& n : parsePlexServerList(line)) {
            bool seen = false;
            for (const auto& e : out) {
                if (e == n) {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                out.push_back(n);
        }
    }
    return out;
}

namespace {
// Debug opt-in lab FFmpeg ladder. Product default is empty: resolve remote
// Parts/transcode URLs directly against the PMS (token, relay/https).
std::string ladderProxyUrl() {
    // MPX_LADDER_URL preferred (lab/product); then legacy names. No hardcoded host.
    if (const char* v = std::getenv("MPX_LADDER_URL"))
        return v[0] ? std::string(v) : std::string();
    const char* mpx = std::getenv("MPX_REMOTE_LADDER_PROXY");
    if (mpx)
        return mpx[0] ? std::string(mpx) : std::string();
    const char* proxy = std::getenv("REMOTE_LADDER_PROXY");
    return (proxy && proxy[0]) ? std::string(proxy) : std::string();
}

// One label for 240p/480i/480p (shared noproxy path; not three copies).
const char* fpgaModeName(const std::string& mode) {
    if (mode == "480i")
        return "480i";
    if (mode == "480p")
        return "480p";
    return "240p";
}
} // namespace

bool ladderProxyConfigured() {
    return !ladderProxyUrl().empty();
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            auto hex = in.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string urlEncodeQuery(const std::string& s) {
    // The body is the shared codec (url_query_codec.hpp), so the ladder start retarget
    // (transcode_offset.hpp) re-encodes a src byte-identically (F-B).
    return urlEncodeQueryComponent(s);
}

namespace {

bool parseResolution(const std::string& res, int& w, int& h) {
    w = h = 0;
    return std::sscanf(res.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

} // namespace

std::string fpgaModeForLadder(const WeakLadder& weak) {
    // Own-core: operator pin, else the live CAPS raster, else conf DECODE.
    if (const char* modeEnv = std::getenv("MPX_FPGA_MODE")) {
        const std::string env = modeEnv;
        if (env == "480p" || env == "480i" || env == "240p")
            return env;
    }
    int confW = 0, confH = 0;
    parseResolution(weak.videoResolution, confW, confH);
    // Geometry wins over stale profileName (conf DECODE=480p while videoResolution=320x240).
    const bool geom240 =
        confW > 0 && confH > 0 && confH <= 240 && confW <= kPlex240pPresentedWidth;
    const bool tier480 = !geom240 &&
                         (weak.profileName == "480i" || weak.profileName == "480p" ||
                          weak.profileName == "fpga-h264-480p" || confW >= 480 || confH >= 480);
    const std::string live = CoreGate::process().pairedRaster();
    // F705: a 480-tier weak ladder on a live 240p core (remote PMS 624x468 on prod Plex.rbf)
    // must keep the 480 PMS geometry until menu reload lands on Utility 240p.
    // F719/R16: once the decision maps to the 240p core (624x468, 320x240, …), use
    // the 320x240 ladder so the SPS gate never sees pic_width_in_mbs 39 on 0x24F1A2C4.
    if (tier480 && live == "240p") {
        if (selectedCoreIdForDecisionOutput(confW, confH) == kSelectedCoreId240p)
            return "240p";
        return weak.profileName == "480i" ? "480i" : "480p";
    }
    if (live == "480p" || live == "480i" || live == "240p")
        return live;
    std::string mode = "240p";
    if (weak.profileName == "480p" || weak.profileName == "fpga-h264-480p" || confW >= 480 ||
        confH >= 480)
        mode = "480p";
    return mode;
}

namespace {
std::mutex& clientProfileNameMu() { static std::mutex m; return m; }
std::string& clientProfileNameConfStore() { static std::string s; return s; }
bool validClientProfileNameValue(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}
} // namespace

void setClientProfileNameConf(const std::string& name) {
    std::lock_guard<std::mutex> l(clientProfileNameMu());
    clientProfileNameConfStore() = name;
}

std::string clientProfileNameOverride(std::string* source) {
    if (const char* e = std::getenv("MPX_CLIENT_PROFILE_NAME")) {
        if (validClientProfileNameValue(e)) {
            if (source) *source = "env";
            return e;
        }
        if (*e) std::fprintf(stderr, "misterplexd: MPX_CLIENT_PROFILE_NAME rejected (allowed [A-Za-z0-9 ._-], 1..64)\n");
    }
    std::string conf;
    {
        std::lock_guard<std::mutex> l(clientProfileNameMu());
        conf = clientProfileNameConfStore();
    }
    if (validClientProfileNameValue(conf)) {
        if (source) *source = "conf";
        return conf;
    }
    if (source) *source = "default";
    return {};
}

std::string baselineClientProfileName(std::string* source) {
    const std::string o = clientProfileNameOverride(source);
    return o.empty() ? std::string(kBaselineClientProfileName) : o;
}

namespace {
std::string& sendProfileNameConfStore() { static std::string s; return s; }
SendProfileNameMode parseSendProfileName(const std::string& v) {
    if (v == "fixed" || v == "1" || v == "on" || v == "yes") return SendProfileNameMode::Fixed;
    if (v == "variant") return SendProfileNameMode::Variant;
    return SendProfileNameMode::None;
}
} // namespace

void setSendProfileNameConf(const std::string& value) {
    std::lock_guard<std::mutex> l(clientProfileNameMu());
    sendProfileNameConfStore() = value;
}

SendProfileNameMode sendProfileNameMode() {
    if (const char* e = std::getenv("MPX_SEND_PROFILE_NAME"); e && *e)
        return parseSendProfileName(e);
    std::lock_guard<std::mutex> l(clientProfileNameMu());
    return parseSendProfileName(sendProfileNameConfStore());
}

const char* sendProfileNameModeName(SendProfileNameMode m) {
    return m == SendProfileNameMode::Fixed ? "fixed" : m == SendProfileNameMode::Variant ? "variant" : "none";
}

std::string profileNameSendDescription() {
    std::string src;
    const std::string o = clientProfileNameOverride(&src);
#ifndef MUT_PROFILE_NAME_LOG_OLD   // test-only mutant: the pre-Round-E line (send=<mode> next to an override)
    if (!o.empty())
        return "send=" + o + " (every request; source: " +
               (src == "env" ? std::string("env MPX_CLIENT_PROFILE_NAME") : std::string("conf CLIENT_PROFILE_NAME")) +
               " override; Extra without replace=true or VideoEncodeFlags)";
#endif
    const SendProfileNameMode m = sendProfileNameMode();
    const std::string mode = std::string(" (source: SEND_PROFILE_NAME=") + sendProfileNameModeName(m) + ")";
    if (m == SendProfileNameMode::Fixed)
        return "send=" + baselineClientProfileName() + mode;
    if (m == SendProfileNameMode::Variant)
        return "send=MiSTerPlex-FPGA-* variant to a private-LAN PMS, else " + baselineClientProfileName() + mode;
    return "send=none (no CLIENT_PROFILE_NAME / MPX_CLIENT_PROFILE_NAME override; SEND_PROFILE_NAME none)";
}

std::string plexRequestProfileName(const WeakLadder& weak, const std::string& targetUrl) {
    if (weak.owenb)
        return {};
    const std::string o = clientProfileNameOverride();
    if (!o.empty())
        return o;
    SendProfileNameMode mode = sendProfileNameMode();
#ifdef MUT_PROFILE_NAME_ALWAYS   // test-only mutant: 02e5e4595 (a fixed name to every server by default)
    if (mode == SendProfileNameMode::None)
        mode = SendProfileNameMode::Fixed;
#endif
    if (mode == SendProfileNameMode::None)
        return {};
#ifdef MUT_PROFILE_NAME_VARIANT   // test-only mutant: the pre-1011 variant name to every server
    if (!weak.fpgaProfileVariant.empty())
        return weak.fpgaProfileVariant;
#endif
#ifndef MUT_PROFILE_NAME_LAN_FIXED   // test-only mutant: variant mode sends the fixed name to a LAN PMS too
    if (mode == SendProfileNameMode::Variant && !weak.fpgaProfileVariant.empty() &&
        plexTargetIsPrivateLan(targetUrl))
        return weak.fpgaProfileVariant;
#endif
    (void)targetUrl;
    return baselineClientProfileName();
}

std::string defaultRequestProfileName() {
    if (clientProfileNameOverride().empty() && sendProfileNameMode() == SendProfileNameMode::None
#ifdef MUT_PROFILE_NAME_ALWAYS
        && false
#endif
    )
        return {};
    return baselineClientProfileName();
}

bool applyPlexTranscodeProfile(const std::string& nameOrResolution, WeakLadder& weak) {
    if (applyModeResolve(nameOrResolution, weak))
        return true;
    for (const auto& p : plexTranscodeProfiles()) {
        if (nameOrResolution == p.name || nameOrResolution == p.videoResolution) {
            weak.profileName = p.name;
            weak.videoResolution = p.videoResolution;
            weak.maxVideoBitrateKbps = p.maxVideoBitrateKbps;
            weak.videoQuality = p.videoQuality;
            weak.videoCodec = "h264";
            weak.audioCodec = "aac";
            weak.h264Profile = p.h264Profile;
            weak.h264Level = p.h264Level;
            return true;
        }
    }
    return false;
}

bool applyFpgaPlexProfile(WeakLadder& weak, const std::string& prototype,
                          int fpsNum, int fpsDen, bool filteringOff, std::string* why,
                          const std::string& mode, bool allowReservedExperiment) {
    FpgaPmsProfile profile;
    if (!selectFpgaPmsProfile(prototype, mode, fpsNum, fpsDen, filteringOff, profile, why,
                              allowReservedExperiment))
        return false;
    weak.profileName = (mode == "480p" || mode == "480i") ? "fpga-h264-480p" : "fpga-h264-240p";
    // Fixed request name; the variant stays internal (validation, LAN query keys, Extra limits).
    weak.clientProfileName = kBaselineClientProfileName;
    weak.fpgaProfileVariant = profile.clientProfileName;
    weak.videoResolution = profile.videoResolution;
    weak.maxVideoBitrateKbps = profile.maxVideoBitrateKbps;
    weak.videoQuality = 60;
    weak.videoCodec = "h264";
    weak.audioCodec = "aac";
    weak.h264Profile = "baseline";
    weak.h264Level = 30;
    weak.fpgaFpsNum = profile.fpsNum;
    weak.fpgaFpsDen = profile.fpsDen;
    return true;
}

namespace {
bool fpsIsTrue24(int num, int den) {
    return (num == 24 && den == 1) || (num == 24000 && den == 1001);
}
bool fpsIsTrue30(int num, int den) {
    return (num == 30 && den == 1) || (num == 30000 && den == 1001);
}
} // namespace

bool fpgaRasterNtsc(const std::string& mode) {
    if (mode == "240p" || mode == "480i")
        return true;
    if (mode.empty())
        return false;
    // Same truthy test as present_bank.hpp envFlagWanted("MPX_PRESENT_480I").
    const char* e = std::getenv("MPX_PRESENT_480I");
    if (!e || !*e)
        return false;
    return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || std::strcmp(e, "on") == 0 ||
           std::strcmp(e, "ON") == 0 || std::strcmp(e, "YES") == 0 ||
           std::strcmp(e, "yes") == 0;
}

bool fpgaContentFps(int srcNum, int srcDen, bool passthrough30, int& outNum, int& outDen) {
    (void)passthrough30;
    // Every FPGA raster: integer 24/1 is Plex 24p film (23.976); integer 30/1 is
    // Plex 30p NTSC (29.97). 240p 3:2 and 480i 2:2 land on the 60000/1001 tick.
    if (srcNum == 24 && srcDen == 1) {
        outNum = 24000;
        outDen = 1001;
        return true;
    }
    if (srcNum == 30 && srcDen == 1) {
        outNum = 30000;
        outDen = 1001;
        return true;
    }
    if (fpsIsTrue24(srcNum, srcDen) || fpsIsTrue30(srcNum, srcDen)) {
        outNum = srcNum;
        outDen = srcDen;
        return false;
    }
    if ((srcNum == 60 && srcDen == 1) || (srcNum == 60000 && srcDen == 1001)) {
        outNum = 30000;
        outDen = 1001;
        return true;
    }
    outNum = 24000;
    outDen = 1001;
    return true;
}

bool fpga480iMinFpsOk(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0)
        return false;
    return static_cast<long long>(fpsNum) * 1001 >= 24000LL * fpsDen;
}

bool fpga240pMinFpsOk(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0)
        return false;
    return static_cast<long long>(fpsNum) * 1001 >= 24000LL * fpsDen;
}

bool fpga480pMinFpsOk(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0)
        return false;
    return static_cast<long long>(fpsNum) * 1001 >= 24000LL * fpsDen;
}

namespace {
// MPX_BROWSE_V2, the helper's server-transcode relay (mpx-menu /menu/tsrc/...): the explicit fps
// the ladder must get instead of fps=src (a probe of the relay would open a second transcode on
// the owning server). It is the rate the ladder itself picks for fps=src (content_fps in
// remote_fpga_ladder_proxy: 24 and 30 families pass, the 60 family halves); any other rate, or
// none, is false and the play takes the original Part with fps=src as before.
bool tsrcRelayLadderFps(int num, int den, int& outNum, int& outDen) {
    if (fpsIsTrue24(num, den) || fpsIsTrue30(num, den)) {
        outNum = num;
        outDen = den;
        return true;
    }
    if ((num == 60 && den == 1) || (num == 60000 && den == 1001)) {
        outNum = num / 2;
        outDen = den;
        return true;
    }
    return false;
}
} // namespace

std::string fpgaNtscSourceProfileName(const std::string& prototype, const std::string& raster,
                                      bool filteringOff) {
    return std::string("MiSTerPlex-FPGA-") + (prototype == "idr" ? "IDR-" : "IP-") + raster +
           "-src30" + (filteringOff ? "-filter-off" : "-filter-on");
}

int fpga480pCodedWidthCeiling() {
    const char* cw = std::getenv("MPX_FPGA_480P_CODED_W");
    return (cw && std::atoi(cw) == 640) ? 640 : 624;
}

FpgaAuSafeClamp fpgaAuSafeClamp(bool tier480, long long offsetMs, bool seekFlat) {
    FpgaAuSafeClamp cl =
        (seekFlat && offsetMs > 0) ? FpgaAuSafeClamp{350, 18}
        : (offsetMs > 0)
            ? (tier480 ? FpgaAuSafeClamp{1000, 30} : FpgaAuSafeClamp{900, 28})
            : (tier480 ? FpgaAuSafeClamp{2500, 60} : FpgaAuSafeClamp{2800, 55});
    // Keep every rung at or above its validateWeakLadder() bitrate floor, or
    // resolve falls through to Part with a misleading "remote transcoder" detail.
    const int floorKbps = tier480 ? kBitrateFloor480iKbps : kBitrateFloor240pKbps;
    if (cl.maxBitrateKbps < floorKbps)
        cl.maxBitrateKbps = floorKbps;
    return cl;
}

bool fpgaAuSafeTier480(const WeakLadder& weak) {
    if (weak.profileName == "480p" || weak.profileName == "480i" ||
        weak.profileName == "fpga-h264-480p")
        return true;
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    return h >= 480 || w >= 480;
}

bool validateWeakLadder(const WeakLadder& weak, std::string* why) {
    int w = 0, h = 0;
    auto fail = [&](const std::string& msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (!parseResolution(weak.videoResolution, w, h))
        return fail("videoResolution must be WxH");
    if (isFpgaPlexProfile(weak)) {
        const bool film =
            (weak.fpgaFpsNum == 24 && weak.fpgaFpsDen == 1) ||
            (weak.fpgaFpsNum == 24000 && weak.fpgaFpsDen == 1001);
        const bool tier240 = (w <= 320 && h <= 240);
        // TRUE640: 480p FPGA cores with PLEX_DECODE_CODED_640 decode 40 MB/row
        // (CAPS max_width 640). 624 stays legal for the older 39-MB cores.
        const bool tier480 = (w <= 640 && h <= 480 && (w > 320 || h > 240));
        // NTSC-raster source-rate profiles (no -r pin) carry 30/1 or 30000/1001 only.
        if (weak.fpgaProfileVariant.find("-src30-") != std::string::npos) {
            if (!fpsIsTrue30(weak.fpgaFpsNum, weak.fpgaFpsDen) || (!tier240 && !tier480))
                return fail("FPGA source-rate profile requires 30/1 or 30000/1001 at 240p or 624x480/640x480");
        } else if (!film || (!tier240 && !tier480)) {
            return fail("FPGA profile requires 240p or 624x480/640x480 film-rate ladder");
        }
    }
    if (weak.videoCodec != "h264")
        return fail("videoCodec must be h264");
    if (weak.audioCodec != "aac")
        return fail("audioCodec must be aac");
    // 720p PRESENT=fpga is host-ffmpeg → I420 DDR (not fabric CABAC). Allow
    // baseline|main. Keep baseline-only for ≤480p profiles (legacy ladder).
    if (weak.h264Profile != "baseline" && weak.h264Profile != "main")
        return fail("H.264 profile must be baseline or main");
    if (w >= 1280 || h >= 720) {
        if (w > 1280 || h > 720)
            return fail("current built-in profiles stop at 1280x720");
        if (weak.h264Level > 31)
            return fail("H.264 level must not exceed 3.1 for 720p ARM decode path");
        if (weak.h264Level < 31)
            return fail("720p requires H.264 level 3.1 (level 3.0 MaxFS is insufficient)");
        if (weak.maxVideoBitrateKbps < 1500)
            return fail("720p profile bitrate is too low");
    } else if (weak.h264Profile != "baseline" &&
               !(weak.highCabac && weak.h264Profile == "main")) {
        return fail("H.264 profile must be baseline for ≤480p profiles");
    } else if (weak.h264Level > 30) {
        return fail("H.264 level must not exceed 3.0 for 240p/480p profiles");
    } else if (w >= 640 || h >= 480) {
        // Host-paint 480i present bank is full BT.601 720×480 (not 640/624 coded
        // FPGA 480p). Allow exact 720×480; anything wider/taller still needs 720p.
        if (w > 720 || h > 480)
            return fail("use 720p profile for resolutions above 720x480");
        if (w > 640 && !(w == 720 && h == 480))
            return fail("use 720x480 (480i host) or 640x480 (480p) ladder");
        if (weak.maxVideoBitrateKbps < 2000)
            return fail("480p/480i profile bitrate is too low");
    } else if (weak.maxVideoBitrateKbps < kBitrateFloor240pKbps) {
        return fail("240p profile bitrate is too low");
    }
    if (weak.videoQuality <= 0 || weak.maxVideoBitrateKbps <= 0)
        return fail("videoQuality and maxVideoBitrate must be positive");
    return true;
}

namespace {
thread_local std::string tlsDecisionMismatch;
std::string xmlAttr(const std::string& tag, const char* name) {
    const std::string key = std::string(" ") + name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    const size_t e = tag.find('"', p);
    return e == std::string::npos ? std::string() : tag.substr(p, e - p);
}
int xmlInt(const std::string& tag, const char* name) {
    const std::string v = xmlAttr(tag, name);
    if (v.empty()) return 0;
    if (v.find('.') != std::string::npos) {   // "3.0" style level
        const double d = std::atof(v.c_str());
        return static_cast<int>(d * 10.0 + 0.5);
    }
    return std::atoi(v.c_str());
}
} // namespace

DecisionVideo parseDecisionVideo(const std::string& xml) {
    DecisionVideo v;
    size_t pos = 0;
    while ((pos = xml.find("<Stream ", pos)) != std::string::npos) {
        const size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        const std::string tag = xml.substr(pos, end - pos);
        pos = end;
        if (xmlAttr(tag, "streamType") != "1") continue;
        v.found = true;
        v.codec = lowerCopy(xmlAttr(tag, "codec"));
        v.profile = lowerCopy(xmlAttr(tag, "profile"));
        v.level = xmlInt(tag, "level");
        v.refFrames = xmlInt(tag, "refFrames");
        v.bitDepth = xmlInt(tag, "bitDepth");
        v.width = xmlInt(tag, "width");
        v.height = xmlInt(tag, "height");
        if (v.partDecision.empty())
            v.partDecision = lowerCopy(xmlAttr(tag, "decision"));
        break;
    }
    if (const size_t p = xml.find("<Part "); p != std::string::npos) {
        const size_t end = xml.find('>', p);
        const std::string tag = xml.substr(p, end == std::string::npos ? std::string::npos : end - p);
        if (v.partDecision.empty())
            v.partDecision = lowerCopy(xmlAttr(tag, "decision"));
        v.partKey = xmlAttr(tag, "key");
    }
    if (const size_t m = xml.find("<Media "); m != std::string::npos) {
        const std::string tag = xml.substr(m, xml.find('>', m) - m);
        if (v.codec.empty()) v.codec = lowerCopy(xmlAttr(tag, "videoCodec"));
        // Media@videoProfile is the SOURCE profile ("high" for the home PMS's Constrained Baseline variant
        // output, lab-runs/home-pms-hwenc-1011 variant-key6): never used. Stream@profile is logged only.
#ifdef MUT_DECISION_PROFILE_FALLBACK   // test-only mutant: 35c510937's Media@videoProfile fallback
        if (v.profile.empty()) v.profile = lowerCopy(xmlAttr(tag, "videoProfile"));
#endif
        if (!v.width) v.width = xmlInt(tag, "width");
        if (!v.height) v.height = xmlInt(tag, "height");
        if (!v.codec.empty()) v.found = true;
    }
    if (const size_t t = xml.find("<TranscodeSession "); t != std::string::npos) {
        const std::string tag = xml.substr(t, xml.find('>', t) - t);
        v.hwEncoding = xmlAttr(tag, "transcodeHwEncoding");
    }
    return v;
}

bool decisionIsDirectPlay(const DecisionVideo& v) {
    const std::string& d = v.partDecision;
    return d == "directplay" || d == "direct play" || d == "copy" || d == "directstream" ||
           d == "direct stream";
}

namespace {
thread_local DecisionVideo tlsLastDecisionVideo;
} // namespace

DecisionVideo lastDecisionVideo() { return tlsLastDecisionVideo; }

std::string decisionCoreMismatch(const DecisionVideo& v, const WeakLadder& weak) {
#ifdef MUT_DECISION_CHECK_OFF   // test-only mutant: the decision is never checked
    (void)v; (void)weak;
    return {};
#endif
    // Only codec and OUTPUT size are trustworthy in a transcode decision (home PMS 1011: Media@videoProfile is
    // the source's, Stream has no profile/level/refFrames). Profile, refs, level, bit depth: the first-AU SPS gate.
    if (!v.found) return {};
    if (!v.codec.empty() && v.codec != "h264")
        return "codec " + v.codec + " (want h264)";
#ifdef MUT_DECISION_PROFILE_FALLBACK
    if (!v.profile.empty() && v.profile != "baseline" && v.profile != "constrained baseline" &&
        weak.h264Profile != "high")
        return "profile " + v.profile + " (core takes baseline)";
#endif
#ifdef MUT_DECISION_ZERO_IS_MISMATCH   // test-only mutant: an unreported (0) level/refs counts as a mismatch
    if (v.level == 0 || v.refFrames == 0)
        return "level/refs " + std::to_string(v.level) + "/" + std::to_string(v.refFrames) + " (unreported)";
#endif
    // Round E 1011 (remote PMS 29729): Stream level/refFrames/profile are absent or 0 (or the SOURCE's) in a transcode
    // decision: unknown, never a mismatch. Only codec and output size count; the first-AU SPS gate owns the rest.
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    if (w > 0 && v.width > w) return "width " + std::to_string(v.width) + " (limit " + std::to_string(w) + ")";
    if (h > 0 && v.height > h) return "height " + std::to_string(v.height) + " (limit " + std::to_string(h) + ")";
    return {};
}

WeakLadder lowerPmsLadder(const WeakLadder& weak) {
    WeakLadder w = weak;
    w.pmsLowered = true;
    int rw = 0, rh = 0;
    parseResolution(w.videoResolution, rw, rh);
    const bool tier480 = rh >= 480 || rw >= 480;
    // Floors: validateWeakLadder's 480-tier 2000 kbps (Baseline), the High cap path's 1000,
    // 240p kBitrateFloor240pKbps.
    const int floorKbps = w.h264Profile == "high" ? 1000 : tier480 ? 2000 : kBitrateFloor240pKbps;
    const int lowered = w.maxVideoBitrateKbps * 3 / 4;
    w.maxVideoBitrateKbps = lowered > floorKbps ? lowered : (w.maxVideoBitrateKbps < floorKbps ? w.maxVideoBitrateKbps : floorKbps);
    if (w.videoQuality > 30) w.videoQuality = 30;
    return w;
}

WeakLadder owenbCavlcRetryLadder(const WeakLadder& weak, const std::string& reason, int cavlcMaxKbps) {
    WeakLadder w = weak;
    if (cavlcMaxKbps <= 0 || reason.find("CAVLC at ") == std::string::npos)
        return w;
#ifndef MUT_OWENB_CAVLC_RETRY_NO_CAP
    const int want = std::max(1000, cavlcMaxKbps * 85 / 100);
    // The OB-only cap (main's ladder is the legacy one; ladderForPlaySession carries obMaxKbps into the OB ladder).
    if (w.obMaxKbps <= 0 || w.obMaxKbps > want)
        w.obMaxKbps = want;
    if (w.owenb && (w.maxVideoBitrateKbps <= 0 || w.maxVideoBitrateKbps > want))
        w.maxVideoBitrateKbps = want;
#endif
    return w;
}

ObSourceEntropy parseObSourceEntropy(const std::string& xml) {
    ObSourceEntropy e;
    size_t sp = 0;
    while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
        const auto end = xml.find('>', sp);
        if (end == std::string::npos)
            break;
        const std::string slice = xml.substr(sp, end - sp);
        if (slice.find("streamType=\"1\"") != std::string::npos || slice.find("type=\"video\"") != std::string::npos) {
            const auto c = slice.find(" cabac=\"");
            if (c != std::string::npos && c + 8 < slice.size())
                e.cabac = slice[c + 8] == '1' ? 1 : slice[c + 8] == '0' ? 0 : -1;
            e.profile = lowerCopy(attrIn(slice, "profile"));
            e.codec = lowerCopy(attrIn(slice, "codec"));
            const std::string br = attrIn(slice, "bitrate");
            if (!br.empty())
                e.bitrateKbps = static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
            break;
        }
        sp = end + 1;
    }
    if (e.profile.empty())
        e.profile = lowerCopy(attr(xml, "Media", "videoProfile"));
    if (e.codec.empty())
        e.codec = lowerCopy(attr(xml, "Media", "videoCodec"));
    if (e.bitrateKbps <= 0) {
        const std::string br = attr(xml, "Media", "bitrate");
        e.bitrateKbps = br.empty() ? 0 : static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
    }
#ifndef MUT_OB_PREPLAY_PROFILE_IGNORED
    if (e.cabac < 0 && e.profile.find("baseline") != std::string::npos)
        e.cabac = 0;   // Baseline / Constrained Baseline: CAVLC only (H.264 A.2.1)
#endif
    return e;
}

bool owenbPrePlayCavlcLadder(WeakLadder& weak, const ObSourceEntropy& src, int hardKbps, int capKbps) {
#ifndef MUT_OB_PREPLAY_LEGACY_TOO
    if (!weak.owenb)
        return false;
#endif
    if (hardKbps <= 0 || capKbps <= 0)
        return false;
#ifndef MUT_OB_PREPLAY_ANY_ENTROPY
    if (src.cabac != 0)
        return false;
#endif
#ifndef MUT_OB_PREPLAY_NO_RATE_CHECK
    if (src.bitrateKbps <= hardKbps)
        return false;
#endif
    const int want = std::max(1000, capKbps * 85 / 100);
    if (weak.maxVideoBitrateKbps > 0 && weak.maxVideoBitrateKbps <= want)
        return false;
    weak.maxVideoBitrateKbps = want;
    return true;
}

bool owenbTranscodeSafeLadder(WeakLadder& weak, const ObSourceEntropy& src, int safeKbps) {
#ifndef MUT_OB_TSAFE_LEGACY_TOO
    if (!weak.owenb)
        return false;
#endif
    if (safeKbps <= 0 || src.codec.empty())
        return false;
#ifndef MUT_OB_TSAFE_H264_TOO
    if (src.codec == "h264" || src.codec == "avc" || src.codec == "avc1")
        return false;   // H.264 may direct-play (CABAC from the file); the in-session gate covers its CAVLC
#endif
    if (weak.maxVideoBitrateKbps > 0 && weak.maxVideoBitrateKbps <= safeKbps)
        return false;
    weak.maxVideoBitrateKbps = safeKbps;   // the resolution (720p) is kept
    return true;
}

std::string takeDecisionMismatch() {
    std::string m;
    m.swap(tlsDecisionMismatch);
    return m;
}

std::string plexVideoEncodeFlags(const WeakLadder& weak) {
    if (!isFpgaPlexProfile(weak)) return {};
    const std::string& v = weak.fpgaProfileVariant;   // MiSTerPlex-FPGA-<IP|IDR>-<tier>-<23976|24|src30>-filter-<on|off>
    const bool idr = v.find("-IDR-") != std::string::npos;
    const bool filterOff = v.find("-filter-off") != std::string::npos;
    std::string rate;
    if (v.find("-23976-") != std::string::npos) rate = " -r 24000/1001";
    else if (v.find("-24-") != std::string::npos) rate = " -r 24";
    else if (v.find("-src30-") != std::string::npos) rate = " -r 30000/1001";
    const int level = weak.h264Level > 0 ? weak.h264Level : 30;
    std::ostringstream f;
    // "-c:v libx264" first, as in the XMLs (Oct 10: PMS ran the software pipeline for that XML with HW on);
    // "-profile:v baseline -refs 1 -bf 0" also bind a hardware encoder that ignores -x264opts (H1/H2).
    // High core (MPX_HIGH_NO_CAVLC) cannot take the baseline CAVLC ladder.
    // Main + cabac=1, still one ref, no B, no 8x8, no weighted pred.
    // 101: remote PMS still emitted CAVLC with these flags while they started with -c:v libx264.
    // A hardware transcoder drops that encoder pin and the rest of the block with it,
    // including -profile:v main. High leaves the encoder unnamed so -profile:v main binds.
    const char* profile = weak.highCabac ? "main" : "baseline";
    const char* cabac = weak.highCabac ? "cabac=1" : "cabac=0";
    if (!weak.highCabac)
        f << "-c:v libx264 ";
    f << "-profile:v " << profile << " -level " << level / 10 << "." << level % 10 << rate
      << " -refs 1 -bf 0 -x264opts " << cabac
      << ":bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none"
      << ":keyint=" << (idr ? 1 : 24) << ":min-keyint=" << (idr ? 1 : 24)
      << ":scenecut=0:open-gop=0:intra-refresh=0:threads=1:sliced-threads=0:slices=1"
         ":vbv-maxrate=4000:vbv-bufsize=1000:qpmin=10:qpmax=40:no-deblock=" << (filterOff ? 1 : 0);
#ifdef MUT_VEF_CABAC_ON   // test-only mutant: the flags lose the CAVLC constraint
    std::string out = f.str();
    const auto cabacAt = out.find("cabac=0");
    if (cabacAt != std::string::npos)
        out.replace(cabacAt, 7, "cabac=1");
    return out;
#endif
    return f.str();
}

WeakLadder owenbWeakLadder(bool wan) {
    WeakLadder w;
    w.owenb = true;
    w.profileName = "owenb";
    w.videoResolution = "1280x720";
    w.maxVideoBitrateKbps = wan ? 6000 : 8000;
    w.h264Profile = "high";
    w.h264ProfileParam = "baseline,main,high";
    w.h264Level = 41;
    w.clientProfileName.clear();
    return w;
}

std::string plexClientProfileExtra(const WeakLadder& weak, bool namedProfile) {
    if (weak.owenb) {
        std::ostringstream o;
        const int level = weak.h264Level > 0 ? weak.h264Level : 41;
        o << "add-limitation(scope=videoCodec&scopeName=h264&type=match&name=video.profile"
          << "&list=baseline,main,high&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level"
          << "&value=" << level << "&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.bitDepth"
          << "&value=8&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.width"
          << "&value=1280&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.height"
          << "&value=720&replace=true)+"
          << "add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.frameRate"
          << "&value=30&replace=true)+"
          << "add-limitation(scope=videoAudioCodec&scopeName=" << weak.audioCodec
          << "&type=upperBound&name=audio.channels&value=2&replace=true)";
        return o.str();
    }
#ifdef MUT_NAMED_EXTRA_DESTRUCTIVE   // test-only mutant: 35c510937..b349bc9d1, the named XML's target is replaced
    namedProfile = false;
#endif
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    std::ostringstream o;
    // Round E 1011: with a profile name the XML's TranscodeTarget (and its VideoEncodeFlags Setting) must survive.
    if (!namedProfile)
    o << "add-transcode-target(type=videoProfile&context=streaming&protocol=http"
      // PMS universal/start.mp4 is the HTTP entrypoint, but its working
      // streaming transcode target is TS; advertising mp4 here can make PMS
      // return an empty video/mp4 response.
      << "&container=mpegts&videoCodec=" << weak.videoCodec << "&audioCodec=" << weak.audioCodec
      << "&replace=true)+"
      << "add-transcode-target-audio-codec(type=videoProfile&context=streaming&protocol=http"
      << "&audioCodec=" << weak.audioCodec << "&replace=true)+";
    o << "add-limitation(scope=videoCodec&scopeName=h264&type=match&name=video.profile"
      << "&list=" << weak.h264Profile << "&replace=true)+"
      << "add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level"
      << "&value=" << weak.h264Level << "&replace=true)+"
      << "add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
      << "&context=streaming&protocol=http&type=match&name=video.profile"
      << "&list=" << weak.h264Profile << "&isRequired=true)+"
      << "add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
      << "&context=streaming&protocol=http&type=upperBound&name=video.level"
      << "&value=" << weak.h264Level << "&isRequired=true)";
    // FPGA Baseline variant contract (was only in the per-variant server XML the name selected):
    // refFrames 1, 8-bit, <=30 fps, stereo AAC. The encoder flags (baseline/CAVLC/ref 1/no B, keyint, deblock)
    // travel as add-transcode-target-settings VideoEncodeFlags (plexVideoEncodeFlags; home PMS 1011 S1).
#ifndef MUT_NO_VEF_TERM   // test-only mutant: the settings term is not sent
    if (isFpgaPlexProfile(weak) && !namedProfile)
        o << "+add-transcode-target-settings(type=videoProfile&context=streaming&protocol=http&VideoEncodeFlags="
          << urlEncodeQuery(plexVideoEncodeFlags(weak)) << ")";
#endif
    // F531d: per-request (client-sent, no server profile) geometry caps on the h264 transcode target.
    if (isFpgaPlexProfile(weak) && !weak.pmsLowered && w > 0 && h > 0) {
        const std::string ttg = "+add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
                                "&context=streaming&protocol=http&type=upperBound&name=";
        o << ttg << "video.width&value=" << w << "&isRequired=true)"
          << ttg << "video.height&value=" << h << "&isRequired=true)";
    }
    if (isFpgaPlexProfile(weak))
        o << "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.refFrames"
          << "&value=1&replace=true)"
          << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.bitDepth"
          << "&value=8&replace=true)"
          << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.frameRate"
          << "&value=30&replace=true)"
          << "+add-limitation(scope=videoAudioCodec&scopeName=" << weak.audioCodec
          << "&type=upperBound&name=audio.channels&value=2&replace=true)";
    // NO-REQUIRED-PROFILE lowered retry: every bound also isRequired on the h264 transcode target.
    if (weak.pmsLowered) {
        const std::string tt = "+add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec"
                               "&context=streaming&protocol=http&type=upperBound&name=";
        o << tt << "video.refFrames&value=" << (weak.h264Profile == "high" ? 4 : 1) << "&isRequired=true)"
          << tt << "video.bitDepth&value=8&isRequired=true)";
        if (w > 0 && h > 0)
            o << tt << "video.width&value=" << w << "&isRequired=true)"
              << tt << "video.height&value=" << h << "&isRequired=true)";
    }
    if (w > 0 && h > 0) {
        o << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.width"
          << "&value=" << w << "&replace=true)"
          << "+add-limitation(scope=videoCodec&scopeName=*&type=upperBound&name=video.height"
          << "&value=" << h << "&replace=true)";
    }
    if (namedProfile) {
        std::string named = o.str();
        for (size_t p; (p = named.find("&replace=true")) != std::string::npos;) named.erase(p, 13);
        return named;
    }
    return o.str();
}

std::string plexClientCapabilities(const WeakLadder& weak) {
    int w = 0, h = 0;
    parseResolution(weak.videoResolution, w, h);
    std::ostringstream o;
    o << "protocols=http-streaming-video,http-mp2t-video;"
      << "videoDecoders=h264{profile:" << weak.h264Profile << "&resolution:" << w << "x" << h
      << "&level:" << weak.h264Level << "};"
      << "audioDecoders=" << weak.audioCodec << "{channels:2}";
    return o.str();
}

namespace {
bool hostLooksPrivateLan(const std::string& host) {
    if (host == "localhost" || host.rfind("127.", 0) == 0)
        return true;
    if (host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0)
        return true;
    // RFC1918 172.16/12
    if (host.rfind("172.", 0) == 0) {
        const auto second = host.find('.', 4);
        if (second != std::string::npos) {
            const int oct = std::atoi(host.c_str() + 4);
            if (oct >= 16 && oct <= 31)
                return true;
        }
    }
    return false;
}

// lan for private PMS; wan for public/shared remote bases (location=lan on a
// public address confuses some shared PMS transcoders and is wrong metadata).
const char* plexLocationForBase(const std::string& base) {
    std::string host;
    auto scheme = base.find("://");
    if (scheme == std::string::npos)
        host = base;
    else {
        size_t hostStart = scheme + 3;
        size_t hostEnd = base.find('/', hostStart);
        std::string hostPort = hostEnd == std::string::npos
                                   ? base.substr(hostStart)
                                   : base.substr(hostStart, hostEnd - hostStart);
        auto at = hostPort.find('@');
        if (at != std::string::npos)
            hostPort = hostPort.substr(at + 1);
        auto colon = hostPort.rfind(':');
        host = (colon != std::string::npos && hostPort.find(':') == colon)
                   ? hostPort.substr(0, colon)
                   : hostPort;
    }
    if (host.empty())
        return "lan";
    return hostLooksPrivateLan(host) ? "lan" : "wan";
}
} // namespace

namespace {
std::string weakVideoProfileParam(const WeakLadder& weak) {
    return weak.h264ProfileParam.empty() ? weak.h264Profile : weak.h264ProfileParam;
}
int weakAudioChannels(const WeakLadder& weak) {
    return weak.audioChannels > 0 ? weak.audioChannels : 2;
}
} // namespace

bool plexTargetIsPrivateLan(const std::string& url) {
    return !url.empty() && std::strcmp(plexLocationForBase(url), "lan") == 0;
}

namespace {
thread_local std::function<bool()> tlsResolveAbort;
} // namespace

void setResolveAbortCheck(std::function<bool()> check) { tlsResolveAbort = std::move(check); }

bool resolveAborted() {
#ifdef MUT_RESOLVE_NO_ABORT   // test-only mutant: a superseded resolve still runs its decision
    return false;
#else
    return tlsResolveAbort && tlsResolveAbort();
#endif
}

bool plexPrewarmProbeEnabled() {
    // Unset is on. 075 remote PMS: the status-only GET drains the universal start into
    // /dev/null for the full 12s max-time, then a second GET waits on the body.
    // That is the ~20s of idle chevron between the decision and "resolved".
    // MPX_PREWARM=0 keeps the two-GET path.
    const char* prewarm = std::getenv("MPX_PREWARM");
    if (!prewarm)
        return true;
    return prewarm[0] != '\0' && std::string(prewarm) != "0";
}

// Remote (location=wan) start/decision buffer hint. The 2026-09-26 snapshot added
// "&acceptWav=1&mediaBufferSize=102400": 102400 KiB at the 2500 kbps ladder is ~335 s, the
// a lab MiSTer remote EOFs (Fifth Element 330.6 s, Quantum Leap 335.6 s from offset 0; lead's
// playbar-keepalive archaeology 1011). Every release before it sent neither. Default: none.
// MPX_REMOTE_MEDIA_BUFFER_KB=<KiB> re-adds mediaBufferSize only, for the A/B proof.
std::string remoteMediaBufferParams() {
#ifdef MUT_REMOTE_BUFFER_102400   // test-only mutant: the 2026-09-26 shape
    return "&acceptWav=1&mediaBufferSize=102400";
#else
    const char* kb = std::getenv("MPX_REMOTE_MEDIA_BUFFER_KB");
    if (!kb || !kb[0])
        return {};
    char* end = nullptr;
    const long v = std::strtol(kb, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 1048576)
        return {};
    return "&mediaBufferSize=" + std::to_string(v);
#endif
}

std::string buildUniversalTranscodeUrl(const std::string& base,
                                       const std::string& metadataKey,
                                       const std::string& token,
                                       const std::string& session,
                                       int64_t offsetMs,
                                       const WeakLadder& weak,
                                       std::string* why) {
    if (isFpgaPlexProfile(weak) && !validateWeakLadder(weak, why))
        return {};
    const char* location = plexLocationForBase(base);
    std::ostringstream q;
    const bool wan = std::strcmp(location, "wan") == 0;
    // C939: plain transcode params (codec/profile/channels) go to every PMS.
    // FPGA-only keys (container, videoFrameRate) stay LAN-only: a shared PMS
    // answers those with a bare HTTP 400.
    // directPlay=1: accept a direct Part or a transcode, whichever the server returns.
    // High WAN (098): that direct path handed back a Baseline CAVLC AU after the
    // decision had already asked for main. Match the decision and refuse direct play.
    // F531c: the FPGA lowered retry (R6p) never depends on a ladder helper: strict Baseline,
    // no direct stream, so any PMS (no client profile) re-encodes to what the core can decode.
    const bool strictBaseline =
        !weak.owenb && weak.pmsLowered && isFpgaPlexProfile(weak) && !weak.highCabac;
    // F531d: an FPGA Baseline profile never direct plays or direct streams. With no client profile
    // installed, PMS answers directPlay=1 with "Direct play OK" and serves the source part (High,
    // 1920x814 on 198.51.100.153), which the Baseline core refuses at the SPS gate (profile_idc 100).
    const bool fpgaBaselineOnly = !weak.owenb && isFpgaPlexProfile(weak) && !weak.highCabac;
    const bool forceTranscode =
        weak.owenb ? weak.pmsLowered
                   : ((wan && weak.highCabac) || strictBaseline || fpgaBaselineOnly);
    q << base << "/video/:/transcode/universal/start.mp4"
      << "?hasMDE=1"
      << "&path=" << urlEncodeQuery(metadataKey)
      << "&mediaIndex=0&partIndex=0"
      << "&protocol=http&fastSeek=" << (wan ? "1" : "0")
      << "&directPlay=" << (forceTranscode ? "0" : "1")
      << "&directStream=" << (forceTranscode ? "0" : "1")
      << "&subtitleSize=100&audioBoost=100&location=" << location
      << (wan ? "" : "&copyts=1")
      << "&autoAdjustQuality=0"
      << (wan ? remoteMediaBufferParams() : std::string())
      << "&session=" << urlEncodeQuery(session)
      << "&videoQuality=" << weak.videoQuality
      << "&videoResolution=" << urlEncodeQuery(weak.videoResolution)
      << "&maxVideoBitrate=" << weak.maxVideoBitrateKbps
      << "&videoCodec=" << urlEncodeQuery(weak.videoCodec)
      << "&audioCodec=" << urlEncodeQuery(weak.audioCodec)
      << "&audioChannels=" << weakAudioChannels(weak)
      << "&videoProfile=" << urlEncodeQuery((strictBaseline || fpgaBaselineOnly) ? std::string("baseline")
                                                                                : weakVideoProfileParam(weak))
      << "&videoLevel=" << weak.h264Level;
    if (!wan && isFpgaPlexProfile(weak))
        q << "&container=mpegts&videoFrameRate="
          << urlEncodeQuery(weak.fpgaFpsDen == 1 ? std::to_string(weak.fpgaFpsNum)
              : std::to_string(weak.fpgaFpsNum) + "/" + std::to_string(weak.fpgaFpsDen));
    // Phase 4: PMS-side burn-in (preferred over dual-A9 FFmpeg subtitles filter).
    if (weak.burnSubtitles) {
        q << "&subtitles=burn";
        if (weak.subtitleStreamId >= 0)
            q << "&subtitleStreamID=" << weak.subtitleStreamId;
    }
    // PMS universal offset is SECONDS (companion / scrubber use ms).
    const int64_t offSec = universalOffsetSeconds(offsetMs);
    if (offSec > 0)
        q << "&offset=" << offSec;
    // FPGA ladders prefer header token (local PMS). Shared/remote servers often
    // require query token on the start URL for libav (no custom auth hooks).
    if (!token.empty() && (!isFpgaPlexProfile(weak) || wan))
        q << "&X-Plex-Token=" << urlEncodeQuery(token);
    return q.str();
}

bool plexSendProfileExtra() {
    if (const char* e = std::getenv("MPX_SEND_PROFILE_EXTRA"); e && *e) {
        const std::string v = e;
        if (v != "0" && v != "off" && v != "none" && v != "no")
            return true;
    }
    return false;
}

bool plexClientSendsProfileExtra(const WeakLadder& weak, bool forceLadderCaps,
                                 const std::string& profileName) {
#ifdef MUT_NO_EXTRA_LAN   // test-only mutant: never Extra, even when opted in
    (void)weak;
    (void)forceLadderCaps;
    (void)profileName;
    return false;
#else
    (void)forceLadderCaps; // WAN Extra is opt-in; plain URL params carry the ladder.
    if (weak.owenb)
        return true;
    if (!profileName.empty())
        return true;
    if (plexSendProfileExtra())
        return true;
    return plexSendClientLadderCaps(weak);
#endif
}

std::string plexFfmpegHeaders(const std::string& sessionId, const std::string& token) {
    return plexFfmpegHeaders(sessionId, token, WeakLadder{});
}

std::string plexFfmpegHeaders(const std::string& sessionId, const std::string& token,
                              const WeakLadder& weak, bool forceLadderCaps,
                              const std::string& targetUrl) {
    std::ostringstream o;
    o << "X-Plex-Client-Identifier: misterplex\r\n"
      << "X-Plex-Product: Plex Web\r\n"
      << "X-Plex-Version: 4.125.0\r\n"
      << "X-Plex-Platform: Chrome\r\n"
      << "X-Plex-Platform-Version: 120.0\r\n"
      << "X-Plex-Device: Linux\r\n"
      << "X-Plex-Device-Name: Chrome\r\n"
      << "X-Plex-Model: bundled\r\n"
      << "X-Plex-Provides: player\r\n";
    const std::string profileName = plexRequestProfileName(weak, targetUrl);
    if (!profileName.empty())
        o << "X-Plex-Client-Profile-Name: " << profileName << "\r\n";
    // Goal B: FPGA Extra (CAVLC/ref 1) with no Profile-Name / no MiSTerPlex.xml.
    // 720p caps Extra still opt-in via plexSendClientLadderCaps.
    if (plexClientSendsProfileExtra(weak, forceLadderCaps, profileName)) {
        o << "X-Plex-Client-Capabilities: " << plexClientCapabilities(weak) << "\r\n"
          << "X-Plex-Client-Profile-Extra: " << plexClientProfileExtra(weak, !profileName.empty()) << "\r\n";
    }
    o << "X-Plex-Session-Identifier: " << sessionId << "\r\n";
    if (!token.empty())
        o << "X-Plex-Token: " << token << "\r\n";
    return o.str();
}

bool hasHeader(const std::vector<std::pair<std::string, std::string>>& headers,
               const char* name) {
    for (const auto& h : headers) {
        if (h.first == name)
            return true;
    }
    return false;
}

bool plexHttpStatusOk(int httpStatus) {
    return httpStatus >= 200 && httpStatus < 300;
}

int parseCurlHttpCode(const std::string& curlWriteOut) {
    std::string out = curlWriteOut;
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    size_t start = 0;
    while (start < out.size() &&
           (out[start] == ' ' || out[start] == '\t' || out[start] == '\n' || out[start] == '\r'))
        ++start;
    if (out.size() - start < 3)
        return 0;
    // Take the last 3 digits if curl appended noise; prefer exact 3-char body.
    std::string code = (out.size() - start == 3) ? out.substr(start) : out.substr(out.size() - 3);
    if (code.size() != 3)
        return 0;
    for (char c : code) {
        if (c < '0' || c > '9')
            return 0;
    }
    return (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
}

PlexHttpNoBodyResult plexHttpGetNoBodyResult(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    int timeoutSec) {
    // Prefer curl HTTP status over body non-empty: /:/timeline may return empty 200,
    // and 401 Unauthorized is a 91-byte HTML body — !body.empty() false-OKed auth fails
    // (docs/evidence/timeline-stuck-20260730T173000Z).
    PlexHttpNoBodyResult r;
    const bool defaultIdentity = !hasHeader(headers, "X-Plex-Client-Identifier");
    std::ostringstream cmd;
    cmd << "curl -sS -g -k -L --http1.1 --connect-timeout 6 --max-time " << timeoutSec
        << " -o /dev/null -w '%{http_code}' -H 'Accept: application/xml'";
    if (defaultIdentity) {
        cmd << " -H 'X-Plex-Client-Identifier: misterplex'"
            << " -H 'X-Plex-Product: Plex Web'"
            << " -H 'X-Plex-Version: 4.125.0'"
            << " -H 'X-Plex-Platform: Chrome'"
            << " -H 'X-Plex-Platform-Version: 120.0'"
            << " -H 'X-Plex-Device: Linux'"
            << " -H 'X-Plex-Device-Name: Chrome'"
            << ((hasHeader(headers, "X-Plex-Client-Profile-Name") || defaultRequestProfileName().empty())
                    ? std::string()
                    : " -H " + shellQuote("X-Plex-Client-Profile-Name: " + defaultRequestProfileName()))
            << " -H 'X-Plex-Model: bundled'"
            << " -H 'X-Plex-Provides: player'";
    }
    cmd << curlHeaderArgs(headers) << " " << shellQuote(url) << " 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p)
        return r;
    std::string out;
    char buf[64];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    (void)pclose(p);
    r.httpStatus = parseCurlHttpCode(out);
    // curl may exit non-zero on 404 with -f; we do not use -f, so status is authoritative.
    r.ok = plexHttpStatusOk(r.httpStatus);
    return r;
}

bool plexHttpGetNoBody(const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       int timeoutSec) {
    return plexHttpGetNoBodyResult(url, headers, timeoutSec).ok;
}


// MPX_PREWARM: collapse the two consecutive cold GETs against the remote universal
// start (status-only probe + tiny body check) into one GET carrying the status and
// the leading bytes over a single TCP connection. The playback demux still opens
// its own connection to the PMS; this only removes one handshake and one PMS
// dispatch from the resolve window.
PlexStartProbe plexHttpGetStatusAndBody(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& clientProfileName, int timeoutSec) {
    PlexStartProbe r;
    // httpGet() identity block plus the probe headers; status is appended by -w as
    // a bare 3-digit code directly after the body (no newline — body may be binary).
    std::ostringstream cmd;
    cmd << "curl -sS -g -k -L --http1.1 --connect-timeout 6 --max-time " << timeoutSec
        << " -w '%{http_code}'"
        << " -H 'Accept: application/xml'"
        << " -H 'X-Plex-Client-Identifier: misterplex'"
        << " -H 'X-Plex-Product: Plex Web'"
        << " -H 'X-Plex-Version: 4.125.0'"
        << " -H 'X-Plex-Platform: Chrome'"
        << " -H 'X-Plex-Platform-Version: 120.0'"
        << " -H 'X-Plex-Device: Linux'"
        << " -H 'X-Plex-Device-Name: Chrome'"
        << ((clientProfileName.empty() || hasHeader(headers, "X-Plex-Client-Profile-Name"))
                ? std::string()
                : " -H " + shellQuote("X-Plex-Client-Profile-Name: " + clientProfileName))
        << " -H 'X-Plex-Model: bundled'"
        << " -H 'X-Plex-Provides: player'"
        << curlHeaderArgs(headers) << " " << shellQuote(url) << " 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p)
        return r;
    std::string out;
    char buf[4096];
#ifdef MUT_PREWARM_FGETS   // test-only mutant: the C1 FAIL read (fgets/strcat stops at the first NUL of TS data)
    while (fgets(buf, sizeof(buf), p))
        out += buf;
#else
    // The body is MPEG-TS (NUL bytes): read it byte-exact, or the trailing -w status is lost (http=0,
    // "remote universal start dead" on a lab MiSTer C1, 2 of 2 with MPX_PREWARM=1).
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
#endif
    (void)pclose(p);
    if (out.size() < 3)
        return r;
    r.httpStatus = parseCurlHttpCode(out.substr(out.size() - 3));
    r.body = out.substr(0, out.size() - 3);
    return r;
}

namespace {
// MPX_BROWSE_V2 (lead Q4): the ladder on lab-pc fetches its src with ffprobe and ffmpeg and
// sends no X-Plex headers. An owning server's transcode handed to it therefore carries, as
// query parameters, the X-Plex set libav sends for the daemon's own universal fetches
// (plexFfmpegHeaders). Names already in the query are kept as they are.
std::string withPlexHeadersAsQuery(const std::string& url, const std::string& headerBlock) {
    std::string out = url;
    size_t i = 0;
    while (i < headerBlock.size()) {
        size_t e = headerBlock.find("\r\n", i);
        if (e == std::string::npos)
            e = headerBlock.size();
        const std::string line = headerBlock.substr(i, e - i);
        i = e + 2;
        const size_t c = line.find(": ");
        if (c == std::string::npos || c == 0)
            continue;
        const std::string name = line.substr(0, c);
        if (out.find("?" + name + "=") != std::string::npos ||
            out.find("&" + name + "=") != std::string::npos)
            continue;
        out += out.find('?') == std::string::npos ? '?' : '&';
        out += name + "=" + urlEncodeQuery(line.substr(c + 2));
    }
    return out;
}

struct LadderStartProbe {
    int httpStatus = 0; // 0: not read (the 16 KiB cap was reached first)
    size_t bytes = 0;
    bool media = false;
};

// GET url the way the ladder's ffmpeg does (no X-Plex headers, Range: bytes=0-) and read at
// most 16 KiB. media: a 2xx (or an unfinished stream past the cap) of >= 64 bytes that is not
// an XML/HTML error page.
LadderStartProbe probeStartLikeLadder(const std::string& url, int timeoutSec) {
    LadderStartProbe r;
    std::ostringstream cmd;
    cmd << "curl -sS -g -k -L --http1.1 --connect-timeout 6 --max-time " << timeoutSec
        << " -A Lavf -H 'Range: bytes=0-' -o - -w '\\n%{http_code}' " << shellQuote(url)
        << " 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p)
        return r;
    constexpr size_t kCap = 16384;
    std::string out;
    char buf[4096];
    size_t n = 0;
    while (out.size() < kCap && (n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    const bool capped = out.size() >= kCap;
    (void)pclose(p); // closes the pipe first: a still-streaming curl ends on EPIPE
    std::string body = out;
    if (!capped) {
        const size_t nl = out.rfind('\n');
        if (nl != std::string::npos) {
            r.httpStatus = parseCurlHttpCode(out.substr(nl + 1));
            body = out.substr(0, nl);
        }
    }
    r.bytes = body.size();
    const std::string head = body.substr(0, 512);
    const bool errorPage = head.find("<html") != std::string::npos ||
                           head.find("<HTML") != std::string::npos ||
                           head.find("<?xml") != std::string::npos ||
                           head.find("MediaContainer") != std::string::npos ||
                           head.find("Bad Request") != std::string::npos;
    r.media = (capped || plexHttpStatusOk(r.httpStatus)) && r.httpStatus != 204 &&
              r.bytes >= 64 && !errorPage;
    return r;
}
} // namespace

std::vector<std::pair<std::string, std::string>> plexStartProbeHeaders(const std::string& sessionId,
                                                                       const std::string& token,
                                                                       const WeakLadder& weak,
                                                                       const std::string& startUrl) {
    std::vector<std::pair<std::string, std::string>> h = {
        {"X-Plex-Session-Identifier", sessionId},
        {"Accept", "*/*"},
    };
    if (!token.empty())
        h.push_back({"X-Plex-Token", token});
    const std::string pn = plexRequestProfileName(weak, startUrl);
    // High core: never send Profile-Name on the start probe (Goal B). LAN variant
    // names used to skip Profile-Extra and PMS locked the session to CAVLC (F257 096).
    if (!pn.empty() && !weak.highCabac)
        h.push_back({"X-Plex-Client-Profile-Name", pn});
    // The WAN probe is the GET that starts the transcoder (Range 0-4095, same
    // session as the later read). 174 L3/L6: videoProfile=baseline on the URL
    // still returned profile_idc 100. A probe without Profile-Extra lets the
    // shared PMS encode its default (High). Baseline sends cabac=0; High sends
    // cabac=1. Main in the URL is not cabac=1.
    if (weak.owenb || weak.highCabac || isFpgaPlexProfile(weak)) {
        h.push_back({"X-Plex-Client-Capabilities", plexClientCapabilities(weak)});
        h.push_back({"X-Plex-Client-Profile-Extra", plexClientProfileExtra(weak, false)});
    }
    // Range keeps probe cheap when the transcoder does stream.
    h.push_back({"Range", "bytes=0-4095"});
    return h;
}

std::string ownerTranscodeLadderSrc(const std::string& plexBase, const std::string& key,
                                    const std::string& token, const std::string& session,
                                    int64_t offsetMs, const WeakLadder& fit, bool wan,
                                    std::string* startUrl) {
    const std::string start =
        buildUniversalTranscodeUrl(plexBase, key, token, session, offsetMs > 0 ? offsetMs : 0, fit);
    if (startUrl)
        *startUrl = start;
    return start.empty() ? std::string()
                         : withPlexHeadersAsQuery(start, plexFfmpegHeaders(session, token, fit,
                                                                           /*forceLadderCaps=*/wan, start));
}

std::string joinPlexHostPort(const std::string& protocol, const std::string& address,
                             const std::string& port) {
    const std::string addr = urlDecode(address);
    std::string proto = protocol.empty() ? "http" : protocol;
    const std::string p = port.empty() ? "32400" : port;
    std::string host = addr;
    std::string path;
    const auto scheme = addr.find("://");
    if (scheme != std::string::npos) {
        proto = addr.substr(0, scheme);
        host = addr.substr(scheme + 3);
    }
    const auto cut = host.find_first_of("/?");
    if (cut != std::string::npos) {
        path = host.substr(cut);
        host = host.substr(0, cut);
    }
    // [IPv6] has colons inside the brackets. A port is only the ':' after ']'.
    if (!host.empty() && host[0] == '[') {
        const auto rb = host.find(']');
        if (rb != std::string::npos) {
            if (rb + 1 < host.size() && host[rb + 1] == ':')
                return proto + "://" + host + path;
            if (rb + 1 == host.size())
                return proto + "://" + host + ":" + p + path;
        }
    }
    // host:port, or a percent-decoded address that already carried one.
    if (host.find(':') != std::string::npos)
        return proto + "://" + host + path;
    return proto + "://" + host + ":" + p + path;
}

PlexEndpoint splitPlexHostPort(const std::string& address, const std::string& port) {
    PlexEndpoint ep;
    const std::string p = port.empty() ? "32400" : port;
    std::string hostport = urlDecode(address);
    const auto scheme = hostport.find("://");
    if (scheme != std::string::npos) {
        ep.protocol = hostport.substr(0, scheme);
        hostport = hostport.substr(scheme + 3);
    }
    const auto slash = hostport.find('/');
    if (slash != std::string::npos)
        hostport = hostport.substr(0, slash);
    if (!hostport.empty() && hostport[0] == '[') {
        const auto rb = hostport.find(']');
        if (rb != std::string::npos) {
            ep.host = hostport.substr(0, rb + 1);
            if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') {
                ep.port = hostport.substr(rb + 2);
                ep.portInAddress = true;
            }
            if (ep.port.empty())
                ep.port = p;
            return ep;
        }
    }
    const auto first = hostport.find(':');
    const auto last = hostport.rfind(':');
    if (first != std::string::npos && first == last) {
        ep.host = hostport.substr(0, first);
        ep.port = hostport.substr(first + 1);
        ep.portInAddress = true;
        if (ep.port.empty())
            ep.port = p;
        return ep;
    }
    ep.host = hostport;
    ep.port = p;
    return ep;
}

std::string buildPlexBase(const std::string& protocol, const std::string& address,
                          const std::string& port, const std::string& lanFallback) {
    const std::string addr = urlDecode(address);
    if (addr.empty() || isUnreachableHost(addr)) {
        if (!lanFallback.empty() && !isUnreachableHost(lanFallback))
            return joinPlexHostPort("http", lanFallback, port);
        return {};
    }
    return joinPlexHostPort(protocol, address, port);
}

namespace {

// plex.direct hosts encode IPv4 as a-b-c-d.<token>.plex.direct
bool dottedIpv4FromDashedHost(const std::string& host, std::string& outIp) {
    // Take label before first '.'
    const auto dot = host.find('.');
    const std::string label = dot == std::string::npos ? host : host.substr(0, dot);
    int o[4] = {0, 0, 0, 0};
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
    outIp = std::to_string(o[0]) + "." + std::to_string(o[1]) + "." + std::to_string(o[2]) +
            "." + std::to_string(o[3]);
    return true;
}

bool splitUrlBase(const std::string& base, std::string& scheme, std::string& host,
                  std::string& port) {
    scheme.clear();
    host.clear();
    port.clear();
    std::string s = base;
    while (!s.empty() && (s.back() == '/' || s.back() == ' '))
        s.pop_back();
    auto schemeEnd = s.find("://");
    if (schemeEnd == std::string::npos)
        return false;
    scheme = s.substr(0, schemeEnd);
    size_t hostStart = schemeEnd + 3;
    size_t hostEnd = s.find('/', hostStart);
    std::string hostPort =
        hostEnd == std::string::npos ? s.substr(hostStart) : s.substr(hostStart, hostEnd - hostStart);
    if (hostPort.empty())
        return false;
    // strip userinfo
    auto at = hostPort.find('@');
    if (at != std::string::npos)
        hostPort = hostPort.substr(at + 1);
    // [ipv6]:port not required for plex.direct path
    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(':') == colon) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    } else {
        host = hostPort;
    }
    return !host.empty();
}

} // namespace

std::string companionOpenablePmsBase(const std::string& selectedBase,
                                     const std::string& confLanFallback) {
    const std::string conf = normalizePlexBase(confLanFallback);
    std::string selected = selectedBase;
    while (!selected.empty() && (selected.back() == '/' || selected.back() == ' '))
        selected.pop_back();
    if (selected.empty())
        return conf;

    // Bare host[:port] → assume http
    if (selected.find("://") == std::string::npos) {
        if (isUnreachableHost(selected))
            return conf;
        if (selected.find(':') != std::string::npos)
            return std::string("http://") + selected;
        return std::string("http://") + selected + ":32400";
    }

    std::string scheme, host, port;
    if (!splitUrlBase(selected, scheme, host, port))
        return conf;
    if (port.empty())
        port = "32400";

    // Already plain HTTP and reachable from MiSTer — keep (remote shared PMS),
    // unless it is a *foreign* private host (home 192.0.2.67 while conf is lab PMS).
    if (scheme == "http" && !isUnreachableHost(host)) {
        if (isPrivateRfc1918Host(host) && !conf.empty() && isPrivateRfc1918Host(conf) &&
            !samePrivateHost(host, conf)) {
            // Not openable from this MiSTer; caller should resolve public IP.
            return {};
        }
        return std::string("http://") + host + ":" + port;
    }

    // Docker-bridge / unreachable → conf LAN only (local PMS identity rewrite).
    if (isUnreachableHost(host) || isUnreachableHost(selected)) {
        if (!conf.empty())
            return conf;
        return {};
    }

    // https or plex.direct: prefer dotted IPv4 HTTP so static libav can open the URL.
    std::string ip;
    if (host.find("plex.direct") != std::string::npos && dottedIpv4FromDashedHost(host, ip)) {
        if (isUnreachableHost(ip)) {
            if (!conf.empty())
                return conf;
            return {};
        }
        // Foreign private plex.direct (owner home LAN) is not reachable from lab MiSTer.
        // Do NOT collapse onto conf PLEX_BASE — that 404s foreign ratingKeys.
        if (isPrivateRfc1918Host(ip) && !conf.empty() && isPrivateRfc1918Host(conf) &&
            !samePrivateHost(ip, conf)) {
            return {};
        }
        return std::string("http://") + ip + ":" + port;
    }

    // Non-plex https host: try http on same host:port (many PMS set httpsRequired=0).
    if (scheme == "https" && !host.empty()) {
        if (isPrivateRfc1918Host(host) && !conf.empty() && isPrivateRfc1918Host(conf) &&
            !samePrivateHost(host, conf)) {
            return {};
        }
        return std::string("http://") + host + ":" + port;
    }

    // Last resort: conf LAN only when selected was already that LAN family.
    return conf;
}

std::string companionPublicPmsBaseFromResources(const std::string& machineIdentifier,
                                                const std::string& token) {
    if (machineIdentifier.empty() || token.empty())
        return {};
    // plex.tv JSON: find matching clientIdentifier, prefer non-local non-relay
    // connection address (public IPv4) as plain HTTP.
    const std::string url =
        "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1";
    // httpGet already sends Accept: application/xml; override with JSON + token.
    const std::string body = httpGet(
        url, 15,
        curlHeaderArgs({{"Accept", "application/json"},
                        {"X-Plex-Token", token},
                        {"X-Plex-Client-Identifier", "misterplex-cast-lookup"}}));
    if (body.empty() || body.find(machineIdentifier) == std::string::npos) {
        std::fprintf(stderr,
                     "misterplexd: plex.tv resources lookup miss machine=%s body=%zu\n",
                     machineIdentifier.c_str(), body.size());
        return {};
    }

    // Prefer whole-body scan near the machine id, then whole body. JSON may be
    // pretty-printed or minified; also accept XML resources (Device attributes).
    const size_t idPos = body.find(machineIdentifier);
    const size_t windowStart = (idPos > 4000) ? idPos - 4000 : 0;
    const size_t windowEnd = std::min(body.size(), idPos + 12000);
    const std::string block = body.substr(windowStart, windowEnd - windowStart);

    auto extractJson = [](const std::string& s, const char* key, size_t from = 0) -> std::string {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = s.find(k, from);
        if (p == std::string::npos)
            return {};
        p = s.find(':', p + k.size());
        if (p == std::string::npos)
            return {};
        ++p;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
            ++p;
        if (p >= s.size())
            return {};
        if (s[p] == '"') {
            ++p;
            size_t e = p;
            while (e < s.size() && s[e] != '"') {
                if (s[e] == '\\' && e + 1 < s.size())
                    e += 2;
                else
                    ++e;
            }
            if (e >= s.size())
                return {};
            return s.substr(p, e - p);
        }
        size_t e = p;
        while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']' &&
               !std::isspace(static_cast<unsigned char>(s[e])))
            ++e;
        return s.substr(p, e - p);
    };

    // 1) publicAddress near machine id
    std::string publicHint = extractJson(block, "publicAddress");
    if (publicHint.empty())
        publicHint = extractJson(body, "publicAddress", idPos > 200 ? idPos - 200 : 0);

    // 2) Walk non-local non-relay connections. Prefer plain HTTP; kube and other
    // reverse-proxied servers often publish on non-32400 ports (e.g. 32401).
    auto scanConnections = [&](const std::string& s, bool httpOnly) -> std::string {
        size_t pos = 0;
        while ((pos = s.find("\"address\"", pos)) != std::string::npos) {
            size_t c0 = s.rfind('{', pos);
            size_t c1 = s.find('}', pos);
            if (c0 == std::string::npos || c1 == std::string::npos || c1 <= c0) {
                pos += 9;
                continue;
            }
            // Keep connection objects near this machine id when scanning whole body.
            if (&s == &body) {
                if (c1 + 200 < idPos || c0 > idPos + 10000) {
                    pos = c1 + 1;
                    continue;
                }
            }
            const std::string conn = s.substr(c0, c1 - c0 + 1);
            pos = c1 + 1;
            const std::string local = extractJson(conn, "local");
            const std::string relay = extractJson(conn, "relay");
            if (local == "true" || relay == "true")
                continue;
            std::string proto = extractJson(conn, "protocol");
            for (char& c : proto) {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            if (httpOnly && !proto.empty() && proto != "http")
                continue;
            std::string addr = extractJson(conn, "address");
            std::string port = extractJson(conn, "port");
            if (addr.empty())
                continue;
            if (isPrivateRfc1918Host(addr) || isUnreachableHost(addr))
                continue;
            if (port.empty())
                port = "32400";
            return std::string("http://") + addr + ":" + port;
        }
        return {};
    };

    std::string best = scanConnections(block, true);
    if (best.empty())
        best = scanConnections(body, true);
    if (best.empty())
        best = scanConnections(block, false);
    if (best.empty())
        best = scanConnections(body, false);

    // Also harvest a non-default public port for publicAddress fallback (kube :32401).
    auto firstPublicPort = [&](const std::string& s) -> std::string {
        size_t pos = 0;
        while ((pos = s.find("\"port\"", pos)) != std::string::npos) {
            size_t c0 = s.rfind('{', pos);
            size_t c1 = s.find('}', pos);
            if (c0 == std::string::npos || c1 == std::string::npos || c1 <= c0) {
                pos += 6;
                continue;
            }
            const std::string conn = s.substr(c0, c1 - c0 + 1);
            pos = c1 + 1;
            if (extractJson(conn, "local") == "true" || extractJson(conn, "relay") == "true")
                continue;
            std::string port = extractJson(conn, "port");
            if (!port.empty() && port != "32400")
                return port;
        }
        return {};
    };

    // 3) publicAddress fallback — never hardcode 32400 when connections show another port.
    if (best.empty() && !publicHint.empty() && !isPrivateRfc1918Host(publicHint) &&
        !isUnreachableHost(publicHint)) {
        std::string port = firstPublicPort(block);
        if (port.empty())
            port = firstPublicPort(body);
        if (port.empty())
            port = "32400";
        best = std::string("http://") + publicHint + ":" + port;
    }

    // 4) XML Device form (plex.tv default Accept): clientIdentifier=… publicAddress=…
    //    Nested <connection address=… local=0 relay=0 …/> preferred.
    if (best.empty()) {
        const std::string needle = "clientIdentifier=\"" + machineIdentifier + "\"";
        size_t dpos = body.find(needle);
        if (dpos == std::string::npos)
            dpos = body.find("machineIdentifier=\"" + machineIdentifier + "\"");
        if (dpos != std::string::npos) {
            // Device open tag may end at '>' after attributes; connections follow.
            size_t tag0 = body.rfind('<', dpos);
            size_t regionEnd = body.find("</Device>", dpos);
            if (regionEnd == std::string::npos)
                regionEnd = body.find("<Device ", dpos + 10);
            if (regionEnd == std::string::npos)
                regionEnd = std::min(body.size(), dpos + 4000);
            const std::string region = body.substr(tag0 == std::string::npos ? dpos : tag0,
                                                   regionEnd - (tag0 == std::string::npos ? dpos : tag0));
            auto xmlAttrIn = [](const std::string& s, const char* name, size_t from = 0) -> std::string {
                const std::string k = std::string(name) + "=\"";
                size_t p = s.find(k, from);
                if (p == std::string::npos)
                    return {};
                p += k.size();
                size_t e = s.find('"', p);
                return e == std::string::npos ? std::string{} : s.substr(p, e - p);
            };
            // Prefer non-local non-relay HTTP connection addresses (kube uses :32401).
            auto tryXmlConns = [&](bool httpOnly) -> std::string {
                size_t cpos = 0;
                while ((cpos = region.find("<connection ", cpos)) != std::string::npos) {
                    size_t cend = region.find("/>", cpos);
                    if (cend == std::string::npos)
                        break;
                    const std::string conn = region.substr(cpos, cend - cpos);
                    cpos = cend + 2;
                    const std::string local = xmlAttrIn(conn, "local");
                    const std::string relay = xmlAttrIn(conn, "relay");
                    if (local == "1" || local == "true" || relay == "1" || relay == "true")
                        continue;
                    std::string proto = xmlAttrIn(conn, "protocol");
                    for (char& c : proto) {
                        if (c >= 'A' && c <= 'Z')
                            c = static_cast<char>(c - 'A' + 'a');
                    }
                    if (httpOnly && !proto.empty() && proto != "http")
                        continue;
                    std::string addr = xmlAttrIn(conn, "address");
                    std::string port = xmlAttrIn(conn, "port");
                    if (addr.empty() || isPrivateRfc1918Host(addr) || isUnreachableHost(addr))
                        continue;
                    if (port.empty())
                        port = "32400";
                    return std::string("http://") + addr + ":" + port;
                }
                return {};
            };
            best = tryXmlConns(true);
            if (best.empty())
                best = tryXmlConns(false);
            if (best.empty()) {
                std::string pa = xmlAttrIn(region, "publicAddress");
                std::string portHint;
                size_t cpos = 0;
                while ((cpos = region.find("<connection ", cpos)) != std::string::npos) {
                    size_t cend = region.find("/>", cpos);
                    if (cend == std::string::npos)
                        break;
                    const std::string conn = region.substr(cpos, cend - cpos);
                    cpos = cend + 2;
                    const std::string local = xmlAttrIn(conn, "local");
                    const std::string relay = xmlAttrIn(conn, "relay");
                    if (local == "1" || local == "true" || relay == "1" || relay == "true")
                        continue;
                    std::string port = xmlAttrIn(conn, "port");
                    if (!port.empty() && port != "32400") {
                        portHint = port;
                        break;
                    }
                }
                if (portHint.empty())
                    portHint = "32400";
                if (!pa.empty() && !isPrivateRfc1918Host(pa) && !isUnreachableHost(pa))
                    best = std::string("http://") + pa + ":" + portHint;
            }
        }
    }

    if (best.empty()) {
        std::fprintf(stderr,
                     "misterplexd: plex.tv resources parsed no public HTTP base "
                     "machine=%s body=%zu json=%d\n",
                     machineIdentifier.c_str(), body.size(),
                     body.find("\"clientIdentifier\"") != std::string::npos ? 1 : 0);
    } else {
        std::fprintf(stderr, "misterplexd: plex.tv public base %s for machine=%s\n",
                     best.c_str(), machineIdentifier.c_str());
    }
    return best;
}

bool ensureUniversalDecision(const std::string& startUrl, const std::string& sessionId,
                             const std::string& token) {
    return ensureUniversalDecision(startUrl, sessionId, token, WeakLadder{});
}

std::string buildUniversalDecisionUrl(const std::string& startUrl,
                                      const std::string& sessionId,
                                      const std::string& token, const WeakLadder& weak) {
    if (startUrl.find("/video/:/transcode/universal/") == std::string::npos)
        return {};
    // Derive base + path from start URL
    auto pathPos = startUrl.find("/video/:/transcode/universal/start");
    if (pathPos == std::string::npos)
        return {};
    const std::string base = startUrl.substr(0, pathPos);
    // Extract path= from query
    auto pathEq = startUrl.find("path=");
    if (pathEq == std::string::npos)
        return {};
    pathEq += 5;
    auto pathEnd = startUrl.find('&', pathEq);
    std::string path = startUrl.substr(pathEq, pathEnd == std::string::npos ? std::string::npos
                                                                             : pathEnd - pathEq);

    // Mirror ladder params from start URL when present
    auto qparam = [&](const char* name) -> std::string {
        std::string key = std::string(name) + "=";
        auto p = startUrl.find(key);
        if (p == std::string::npos)
            return {};
        p += key.size();
        auto e = startUrl.find('&', p);
        return startUrl.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    std::string vq = qparam("videoQuality");
    if (vq.empty())
        vq = "40";
    std::string vres = qparam("videoResolution");
    if (vres.empty())
        vres = "320x240";
    std::string br = qparam("maxVideoBitrate");
    if (br.empty())
        br = std::to_string(weak.maxVideoBitrateKbps);
    std::string vcodec = qparam("videoCodec");
    if (vcodec.empty())
        vcodec = weak.videoCodec;
    std::string acodec = qparam("audioCodec");
    if (acodec.empty())
        acodec = weak.audioCodec;
    std::string vprofile = qparam("videoProfile");
    if (vprofile.empty())
        vprofile = weak.h264Profile;
    std::string vlevel = qparam("videoLevel");
    if (vlevel.empty())
        vlevel = std::to_string(weak.h264Level);

    std::string location = qparam("location");
    if (location.empty())
        location = "lan";
    const bool wan = location == "wan";
    std::ostringstream decisionUrl;
    decisionUrl << base << "/video/:/transcode/universal/decision?hasMDE=1&path=" << path
                << "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=" << (wan ? "1" : "0")
                << "&directPlay=0&directStream=0"
                << "&location=" << location << "&session=" << urlEncodeQuery(sessionId)
                << "&videoQuality=" << vq << "&videoResolution=" << vres
                << "&maxVideoBitrate=" << br;
    if (wan) {
        decisionUrl << remoteMediaBufferParams();
        // remote PMS decides the session on this request. The start URL's videoProfile
        // arrives too late (096: start said main, the AU was still Baseline CAVLC).
        // container and videoFrameRate stay off: a shared PMS answers those with HTTP 400.
        // Baseline cores need the same early pin. A WAN decision that omits
        // videoProfile lets the shared PMS encode the source (profile_idc 100);
        // the start URL's videoProfile arrives after that session exists.
        const bool fpgaBaseline = isFpgaPlexProfile(weak) && !weak.highCabac;
        if (weak.highCabac || fpgaBaseline)
            decisionUrl << "&videoCodec=" << vcodec << "&audioCodec=" << acodec
                        << "&videoProfile=" << (fpgaBaseline ? "baseline" : vprofile)
                        << "&videoLevel=" << vlevel;
    } else {
        // Home PMS is LAN. The start URL's videoProfile=baseline,main lets that
        // server encode the source (profile_idc 100) before the play URL is read.
        // The decision creates the session, so a Baseline core pins baseline here.
        const bool fpgaBaselineLan = isFpgaPlexProfile(weak) && !weak.highCabac;
        if (fpgaBaselineLan)
            vprofile = "baseline";
        decisionUrl << "&videoCodec=" << vcodec << "&audioCodec=" << acodec
                    << "&videoProfile=" << vprofile << "&videoLevel=" << vlevel;
        if (isFpgaPlexProfile(weak)) {
            if (!validateWeakLadder(weak))
                return {};
            decisionUrl << "&container=mpegts&audioChannels=2&videoFrameRate="
                        << urlEncodeQuery(weak.fpgaFpsDen == 1
                                              ? std::to_string(weak.fpgaFpsNum)
                                              : std::to_string(weak.fpgaFpsNum) + "/" +
                                                    std::to_string(weak.fpgaFpsDen));
        }
    }
    // Query token for non-FPGA always; for FPGA WAN (libav/shared) also.
    if (!token.empty() && (!isFpgaPlexProfile(weak) || wan))
        decisionUrl << "&X-Plex-Token=" << urlEncodeQuery(token);
    return decisionUrl.str();
}

bool ensureUniversalDecision(const std::string& startUrl, const std::string& sessionId,
                             const std::string& token, const WeakLadder& weak,
                             bool forceLadderCaps) {
    const std::string decisionUrl = buildUniversalDecisionUrl(startUrl, sessionId, token, weak);
    if (decisionUrl.empty())
        return !isFpgaPlexProfile(weak);
    const bool wan [[maybe_unused]] = startUrl.find("location=wan") != std::string::npos ||
                     decisionUrl.find("location=wan") != std::string::npos;
    std::vector<std::pair<std::string, std::string>> decisionHeaders = {
        {"X-Plex-Session-Identifier", sessionId},
        {"X-Plex-Product", "Plex Web"},
        {"X-Plex-Client-Identifier", "misterplex"},
        {"X-Plex-Platform", "Chrome"},
        {"X-Plex-Device", "Linux"},
    };
    if (!token.empty())
        decisionHeaders.push_back({"X-Plex-Token", token});
    // WAN/shared has no MiSTerPlex.xml — Profile-Extra must carry the FPGA ladder.
    // LAN FPGA keeps XML-only (Extra can fight Baseline/CAVLC on lab PMS).
    // forceLadderCaps: the caller's src already carries the caps (plexFfmpegHeaders with
    // forceLadderCaps), so the decision for the same session must say the same thing (T-E2E-416).
    const std::string decisionProfileName = plexRequestProfileName(weak, startUrl);
    if (plexClientSendsProfileExtra(weak, forceLadderCaps, decisionProfileName)) {
        decisionHeaders.push_back({"X-Plex-Client-Capabilities", plexClientCapabilities(weak)});
        decisionHeaders.push_back({"X-Plex-Client-Profile-Extra",
                                   plexClientProfileExtra(weak, !decisionProfileName.empty())});
    }
    // PMS uses the first profile header when duplicates are sent and caches that
    // decision for the stream; the selected XML must replace the default here.
    if (resolveAborted()) {
        std::fprintf(stderr, "misterplexd: universal decision skipped: superseded (a later seek/play owns it)\n");
        return false;
    }
    if (weak.highCabac && wan) {
        const bool askedMain = decisionUrl.find("videoProfile=main") != std::string::npos;
        const bool noDirect = decisionUrl.find("directPlay=0") != std::string::npos;
        bool sawCabac = false;
        for (const auto& hv : decisionHeaders) {
            if (hv.second.find("cabac=1") != std::string::npos ||
                hv.second.find("cabac%3D1") != std::string::npos ||
                hv.second.find("cabac%3d1") != std::string::npos)
                sawCabac = true;
        }
        std::fprintf(stderr,
                     "misterplexd: High WAN decision videoProfile=%s directPlay=%s extra=%s\n",
                     askedMain ? "main" : "missing", noDirect ? "0" : "other",
                     sawCabac ? "cabac=1" : "no-cabac=1");
    }
    const std::string body = httpGet(decisionUrl, 20, curlHeaderArgs(decisionHeaders),
                                    decisionProfileName);
    if (body.empty())
        return false;
    if (body.find("unable to find a matching profile") != std::string::npos)
        return false;
    if (body.find("Bad Request") != std::string::npos &&
        body.find("MediaContainer") == std::string::npos)
        return false;
    const bool accepted = body.find("MediaContainer") != std::string::npos ||
                          body.find("transcodeDecisionCode") != std::string::npos;
    if (accepted) {
        const DecisionVideo dv = parseDecisionVideo(body);
        tlsLastDecisionVideo = dv;
        const bool checkCore = isFpgaPlexProfile(weak) || weak.h264Profile == "high" ||
                               modeResolveRow(weak.profileName) != nullptr;
        const std::string mm = checkCore ? decisionCoreMismatch(dv, weak) : std::string();
        std::fprintf(stderr,
                     "misterplexd: decision video codec=%s profile=%s level=%d refs=%d %dx%d bitDepth=%d hw=%s "
                     "part=%s check=%s%s%s profileName=%s\n",
                     dv.codec.empty() ? "-" : dv.codec.c_str(), dv.profile.empty() ? "-" : dv.profile.c_str(),
                     dv.level, dv.refFrames, dv.width, dv.height, dv.bitDepth,
                     dv.hwEncoding.empty() ? "-" : dv.hwEncoding.c_str(),
                     dv.partDecision.empty() ? "-" : dv.partDecision.c_str(),
                     mm.empty() ? "ok" : "mismatch: ",
                     mm.c_str(), weak.pmsLowered ? " (lowered retry)" : "",
                     decisionProfileName.empty() ? "none" : decisionProfileName.c_str());
        if (!mm.empty() && !weak.pmsLowered) {
            // One lowered retry (the caller: takeDecisionMismatch), for a codec or output-size mismatch. The lowered
            // request proceeds even if its decision still mismatches: the SPS gate decides from the first AU.
            tlsDecisionMismatch = mm;
            return false;
        }
    }
    return accepted;
}

bool mediaVideoIsH264(const std::string& plexMetadataXml) {
    if (plexMetadataXml.empty())
        return false;
    // Prefer Stream video codec when present; else Media@videoCodec.
    size_t pos = 0;
    while ((pos = plexMetadataXml.find("<Stream", pos)) != std::string::npos) {
        auto end = plexMetadataXml.find('>', pos);
        if (end == std::string::npos)
            break;
        const std::string tag = plexMetadataXml.substr(pos, end - pos);
        // PMS uses streamType="1" (video) and/or type="video"
        const bool isVideo = tag.find("streamType=\"1\"") != std::string::npos ||
                             tag.find("type=\"video\"") != std::string::npos;
        if (isVideo) {
            auto c = attrIn(tag, "codec");
            if (c.empty())
                c = attrIn(tag, "codecID");
            if (videoCodecIsH264(c))
                return true;
        }
        pos = end + 1;
    }
    return videoCodecIsH264(attr(plexMetadataXml, "Media", "videoCodec"));
}

SourceAspect sourceAspectFromPlexMetadata(const std::string& xml,
                                         int codedWidth, int codedHeight) {
    std::string displayAspect = attr(xml, "Video", "displayAspectRatio");
    if (displayAspect.empty())
        displayAspect = attr(xml, "Video", "aspectRatio");
    if (displayAspect.empty())
        displayAspect = attr(xml, "Media", "displayAspectRatio");
    if (displayAspect.empty())
        displayAspect = attr(xml, "Media", "aspectRatio");

    std::string sampleAspect = attr(xml, "Media", "sampleAspectRatio");
    if (sampleAspect.empty())
        sampleAspect = attr(xml, "Media", "pixelAspectRatio");
    std::string anamorphic = attr(xml, "Media", "anamorphic");

    size_t sp = 0;
    while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
        const auto end = xml.find('>', sp);
        if (end == std::string::npos)
            break;
        const std::string slice = xml.substr(sp, end - sp);
        const bool isVideo = slice.find("streamType=\"1\"") != std::string::npos ||
                             slice.find("type=\"video\"") != std::string::npos;
        if (isVideo) {
            if (displayAspect.empty())
                displayAspect = attrIn(slice, "displayAspectRatio");
            if (displayAspect.empty())
                displayAspect = attrIn(slice, "aspectRatio");
            if (sampleAspect.empty())
                sampleAspect = attrIn(slice, "sampleAspectRatio");
            if (sampleAspect.empty())
                sampleAspect = attrIn(slice, "pixelAspectRatio");
            if (sampleAspect.empty())
                sampleAspect = attrIn(slice, "sar");
            if (anamorphic.empty())
                anamorphic = attrIn(slice, "anamorphic");
            break;
        }
        sp = end + 1;
    }

    const std::string normalizedAnamorphic = lowerCopy(anamorphic);
    const bool squarePixelsKnown =
        normalizedAnamorphic == "0" || normalizedAnamorphic == "false" ||
        normalizedAnamorphic == "no";
    return sourceAspectFromMetadata(displayAspect, sampleAspect, codedWidth,
                                    codedHeight, squarePixelsKnown);
}

WeakLadder fitWeakLadderToAspect(const WeakLadder& weak,
                                 const SourceAspect& aspect) {
    WeakLadder fitted = weak;
    // FPGA requests preserve the profile's maximum rectangle; SPS supplies actual geometry.
    if (isFpgaPlexProfile(weak))
        return fitted;
    if (!aspect.valid || aspect.x == 0 || aspect.y == 0)
        return fitted;

    int maxW = 0;
    int maxH = 0;
    if (std::sscanf(weak.videoResolution.c_str(), "%dx%d", &maxW, &maxH) != 2 ||
        maxW < 2 || maxH < 2) {
        return fitted;
    }

    // L4 1280×720 HDMI is 16:9. Fitting 4:3 Trek to 960×720 then nearest-up
    // to the bank costs ~2 Hz (21.7 vs unique24). Keep the 1280×720 request.
    if (maxW >= 1280 && maxH >= 720)
        return fitted;

    int width = maxW;
    int height = maxH;
    if (static_cast<int64_t>(aspect.x) * maxH >=
        static_cast<int64_t>(aspect.y) * maxW) {
        height = static_cast<int>(
            (static_cast<int64_t>(maxW) * aspect.y) / aspect.x);
    } else {
        width = static_cast<int>(
            (static_cast<int64_t>(maxH) * aspect.x) / aspect.y);
    }
    width = std::max(2, width & ~1);
    height = std::max(2, height & ~1);

    // The dual-A9 path is proven realtime near 640x384, while 640x480 24p
    // transcodes fall below rate once AAC, scaling, and DDR presentation run
    // together. Keep the 480p ladder inside that pixel budget; MiSTer still
    // receives the full 640x480 presentation bank.
    constexpr int64_t kTrue480DecodePixelBudget = 640LL * 384LL;
    if (maxW <= 640 && maxH <= 480 &&
        static_cast<int64_t>(width) * height > kTrue480DecodePixelBudget) {
        const double dar = static_cast<double>(aspect.x) / aspect.y;
        width = static_cast<int>(
                    std::sqrt(static_cast<double>(kTrue480DecodePixelBudget) * dar)) &
                ~1;
        height = static_cast<int>(width / dar) & ~1;
        width = std::max(2, width);
        height = std::max(2, height);
    }
    fitted.videoResolution = std::to_string(width) + "x" + std::to_string(height);
    return fitted;
}

ResolveResult resolvePlayTarget(const std::string& rawKeyOrPath, const std::string& plexBase,
                                const std::string& token, int64_t offsetMs, bool weakAlways,
                                const WeakLadder& weakIn, bool preferDirectH264, int decodeW,
                                int decodeH, bool forceLadder, const std::string& gateDetail) {
    ResolveResult r;
    // The configured backend stays FpgaH264 while a verified PlexOB core is loaded.
    // That path rewrites the ladder to 624x480 baseline and directPlay=0. The ring
    // session uses the Owenb ladder instead.
    const VideoBackend session = sessionVideoBackend(
        configuredVideoBackend(), CoreGate::process().ownsIdleInFabric());
    const bool fpgaBackend = session == VideoBackend::FpgaH264;
    WeakLadder playWeak =   // BUG1 pre-play: lowered below once the metadata shows a CAVLC source above the hard cap
        ladderForPlaySession(session, weakIn, !plexTargetIsPrivateLan(plexBase));
    const WeakLadder& weak = playWeak;
    if (session == VideoBackend::OwenbRing && !weakIn.owenb) {
        std::fprintf(stderr,
                     "misterplexd: PlexOB session ladder %s %dk (not FPGA %s)\n",
                     weak.videoResolution.c_str(), weak.maxVideoBitrateKbps,
                     weakIn.videoResolution.c_str());
    }
    // forceLadder with no helper: PMS universal of the same item (never a direct Part,
    // never a hard refuse). The lab ladder remains debug opt-in.
    // FPGA raster mode chosen by the weak-universal block below; "" off the FPGA path.
    std::string fpgaMode;
    // 240p/480i title without a numeric rate: the ladder proxy probes it (&fps=src) when
    // configured; without a helper, PMS universal uses 24000/1001.
    bool fpgaFpsSrc = false;
    std::string key = urlDecode(rawKeyOrPath);
    if (key.empty() || key == "test" || key == "testsrc") {
        r.ok = true;
        r.playable = "testsrc";
        r.detail = "test pattern";
        r.durationMs = 120000;
        r.sourceFpsHint = 30;
        r.fpsNum = 30;
        r.fpsDen = 1;
        r.sourceAspect = {4, 3, true};
        return r;
    }

    if (playableIsFarpointIdentityCache(key)) {
        r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
        return r;
    }
    // playMedia 40868 / ratingKey 40868 must never take the local-path
    // identify branch (that is the farpoint_1280x720.mp4 cache soak).
    if (key == "40868")
        key = "/library/metadata/40868";

    // Direct http(s) or absolute non-library path
    if (key.rfind("http://", 0) == 0 || key.rfind("https://", 0) == 0) {
        r.ok = true;
        r.playable = key;
        r.detail = "direct URL";
        return r;
    }
    if (!metadataKeyIsFarpoint40868(key) && !key.empty() && key[0] == '/' &&
        key.rfind("/library", 0) != 0 &&
        key.rfind("/playQueues", 0) != 0) {
        r.ok = true;
        r.playable = key;
        r.detail = "local path";
        if (playableIsFarpointIdentityCache(key)) {
            r.ok = false;
            r.playable.clear();
            r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
            return r;
        }
        if (fpgaBackend) {
            r.detail = "local path; metadata deferred to the sole compressed demux";
            return r;
        }
        // Local files must publish DAR + 24000/1001 or PLAY is rejected / paced
        // at 24/1. Library rk 40868 must use PMS universal, not this path.
        const char* ff = "/media/fat/misterplex/bin/ffmpeg";
        if (::access(ff, X_OK) != 0)
            ff = "ffmpeg";
        std::string cmd = std::string(ff) + " -hide_banner -nostdin -i " + shellQuote(key) +
                          " -f null -t 0 - 2>&1";
        FILE* p = ::popen(cmd.c_str(), "r");
        std::string ident;
        if (p) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), p))
                ident.append(buf);
            ::pclose(p);
        }
        int w = 0, h = 0, n = 0, d = 0, ax = 0, ay = 0;
        if (parseFfmpegIdentify(ident.c_str(), w, h, n, d, ax, ay)) {
            r.mediaWidth = w;
            r.mediaHeight = h;
            r.fpsNum = n;
            r.fpsDen = d;
            r.sourceFpsHint = (n > 0 && d > 0) ? (n + d / 2) / d : 0;
            if (ax > 0 && ay > 0)
                r.sourceAspect = {static_cast<uint16_t>(ax), static_cast<uint16_t>(ay), true};
            else if (w == 1280 && h == 720)
                r.sourceAspect = {16, 9, true};
            r.detail = localFileIsTrue720p24(w, h) ? "local path 1280x720" : "local path";
        } else if (r.sourceAspect.valid == false) {
            r.sourceAspect = defaultSourceAspectForBank(1280, 720);
        }
        return r;
    }

    // Normalize metadata key
    if (key.rfind("/library", 0) != 0 && key.find("library") != std::string::npos) {
        // sometimes "library/metadata/N"
        if (key[0] != '/')
            key = "/" + key;
    }

    if (playableIsFarpointIdentityCache(key)) {
        r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
        return r;
    }

    if (plexBase.empty()) {
        r.detail = "no PMS base for key=" + key;
        return r;
    }
    if (token.empty()) {
        r.detail = "no token for key=" + key;
        // Still try without token (local unauth servers)
    }

    // Fetch metadata for title/duration/viewOffset
    std::string metaUrl = plexBase + key;
    std::string metaHeaders;
    if (fpgaBackend) {
        if (!token.empty())
            metaHeaders = curlHeaderArgs({{"X-Plex-Token", token}});
    } else {
        if (metaUrl.find('?') == std::string::npos)
            metaUrl += "?";
        else
            metaUrl += "&";
        if (!token.empty())
            metaUrl += "X-Plex-Token=" + urlEncodeQuery(token);
    }
    std::string xml = httpGet(metaUrl, 12, metaHeaders);
    // A 404 or 401 from PMS still returns a BODY (an HTML error page), so
    // "response is not empty" does not mean "item exists". Without this check a
    // ratingKey that was deleted or renumbered by a library re-scan sails
    // through every branch below, finds no Part, and lands on the test pattern —
    // which looks like a playback bug instead of a missing item, and silently
    // invalidates any measurement taken against it.
    // Show/season cast keys (Plex Web details Play on a series) have no Media —
    // expand via allLeaves to the first episode before FPGA clock/ladder work.
    if (xml.find("<MediaContainer") != std::string::npos &&
        xml.find("<Video") == std::string::npos &&
        xml.find("<Directory") != std::string::npos) {
        const std::string dirType = attr(xml, "Directory", "type");
        if (dirType == "show" || dirType == "season" || dirType.empty()) {
            std::string leavesUrl = plexBase + key;
            if (!leavesUrl.empty() && leavesUrl.back() == '/')
                leavesUrl.pop_back();
            leavesUrl += "/allLeaves?X-Plex-Container-Start=0&X-Plex-Container-Size=1";
            const std::string leaves = httpGet(leavesUrl, 12, metaHeaders);
            const std::string epKey = attr(leaves, "Video", "ratingKey");
            const std::string epPath = attr(leaves, "Video", "key");
            std::string nextKey;
            if (!epPath.empty() && epPath.rfind("/library/", 0) == 0)
                nextKey = epPath;
            else if (!epKey.empty())
                nextKey = std::string("/library/metadata/") + epKey;
            if (!nextKey.empty() && nextKey != key) {
                std::fprintf(stderr,
                             "misterplexd: cast container %s (%s) → first episode %s\n",
                             key.c_str(), dirType.empty() ? "directory" : dirType.c_str(),
                             nextKey.c_str());
                return resolvePlayTarget(nextKey, plexBase, token, offsetMs, weakAlways, weak,
                                         preferDirectH264, decodeW, decodeH, forceLadder, gateDetail);
            }
        }
    }
    const bool metaFound = xml.find("<MediaContainer") != std::string::npos;
    if (metaFound) {
        r.title = attr(xml, "Video", "title");
        if (r.title.empty())
            r.title = attr(xml, "Directory", "title");
        auto d = attr(xml, "Video", "duration");
        if (d.empty())
            d = attr(xml, "Media", "duration");
        if (!d.empty())
            r.durationMs = std::strtoll(d.c_str(), nullptr, 10);
        auto vo = attr(xml, "Video", "viewOffset");
        if (!vo.empty())
            r.viewOffsetMs = std::strtoll(vo.c_str(), nullptr, 10);
        r.ratingKey = attr(xml, "Video", "ratingKey");
        // Match-source-Hz / Content FPS: Media@videoFrameRate + video Stream@frameRate
        r.videoFrameRate = attr(xml, "Media", "videoFrameRate");
        if (r.videoFrameRate.empty())
            r.videoFrameRate = attr(xml, "Video", "videoFrameRate");
        // Fallback: any videoFrameRate="..." in the metadata payload
        if (r.videoFrameRate.empty()) {
            const std::string key = "videoFrameRate=\"";
            auto p = xml.find(key);
            if (p != std::string::npos) {
                p += key.size();
                auto e = xml.find('"', p);
                if (e != std::string::npos)
                    r.videoFrameRate = xml.substr(p, e - p);
            }
        }
        r.frameRate = attr(xml, "Stream", "frameRate");
        // Prefer video stream frameRate when Stream tags are mixed (first may be audio).
        // Scan all Stream open-tags for type="video" frameRate.
        {
            size_t sp = 0;
            while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
                auto end = xml.find('>', sp);
                if (end == std::string::npos)
                    break;
                const std::string slice = xml.substr(sp, end - sp);
                if (slice.find("type=\"video\"") != std::string::npos) {
                    auto fr = attrIn(slice, "frameRate");
                    if (!fr.empty())
                        r.frameRate = fr;
                    break;
                }
                sp = end + 1;
            }
        }
        r.sourceFpsHint = contentFpsHint(r.videoFrameRate, r.frameRate);
        parseExactFps(r.videoFrameRate, r.frameRate, r.fpsNum, r.fpsDen);

        // Source coded size: prefer video Stream width/height, else Media@.
        // Used by media_player to skip FOAR+pad only when source covers DECODE
        // (Grid720 1280×720 @720p bank). FOAR 720×480 must keep on-device pad (L38).
        {
            int mw = 0, mh = 0;
            size_t sp = 0;
            while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
                auto end = xml.find('>', sp);
                if (end == std::string::npos)
                    break;
                const std::string slice = xml.substr(sp, end - sp);
                const bool isVideo = slice.find("streamType=\"1\"") != std::string::npos ||
                                     slice.find("type=\"video\"") != std::string::npos;
                if (isVideo) {
                    auto w = attrIn(slice, "width");
                    auto h = attrIn(slice, "height");
                    if (w.empty())
                        w = attrIn(slice, "codedWidth");
                    if (h.empty())
                        h = attrIn(slice, "codedHeight");
                    if (!w.empty() && !h.empty()) {
                        mw = static_cast<int>(std::strtol(w.c_str(), nullptr, 10));
                        mh = static_cast<int>(std::strtol(h.c_str(), nullptr, 10));
                    }
                    break;
                }
                sp = end + 1;
            }
            if (mw <= 0 || mh <= 0) {
                auto w = attr(xml, "Media", "width");
                auto h = attr(xml, "Media", "height");
                if (!w.empty() && !h.empty()) {
                    mw = static_cast<int>(std::strtol(w.c_str(), nullptr, 10));
                    mh = static_cast<int>(std::strtol(h.c_str(), nullptr, 10));
                }
            }
            r.mediaWidth = mw > 0 ? mw : 0;
            r.mediaHeight = mh > 0 ? mh : 0;
        }
        {
            auto scan = attr(xml, "Media", "videoScanType");
            if (scan.empty())
                scan = attr(xml, "Media", "scanType");
            std::string scanLower = scan;
            for (char& c : scanLower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (scanLower == "interlaced")
                r.mediaInterlaced = true;
            if (scanLower == "progressive")
                r.mediaProgressive = true;
            if (!r.videoFrameRate.empty()) {
                const char last = r.videoFrameRate.back();
                if (last == 'i' || last == 'I')
                    r.mediaInterlaced = true;
                if (last == 'p' || last == 'P')
                    r.mediaProgressive = true;
            }
        }

        r.sourceAspect =
            sourceAspectFromPlexMetadata(xml, r.mediaWidth, r.mediaHeight);

        // Audio presence: dual-output ffmpeg (pipe:1 video + pipe:3 audio) aborts
        // with "Output file does not contain any stream" when the source has no
        // audio track (Grid720). Detect streamType=2 / type=audio up front.
        {
            bool foundAudio = false;
            size_t sp = 0;
            while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
                auto end = xml.find('>', sp);
                if (end == std::string::npos)
                    break;
                const std::string slice = xml.substr(sp, end - sp);
                if (slice.find("streamType=\"2\"") != std::string::npos ||
                    slice.find("type=\"audio\"") != std::string::npos) {
                    foundAudio = true;
                    break;
                }
                sp = end + 1;
            }
            // Only force false when we successfully parsed Stream tags at all.
            // Empty/malformed metadata keeps hasAudio=true (fail open).
            if (xml.find("<Stream") != std::string::npos)
                r.hasAudio = foundAudio;
        }
    }

    // STREAM product path: prefer direct H.264 Part (elementary after demux) so host
    // CAVLC recon can work on Baseline/Main. PMS Chrome universal often emits High/CABAC.
    // STREAM=0 rate path: direct only when source Media *exactly matches* DECODE bank
    // (true 720 into 720 — skip PMS re-transcode). Do NOT treat FOAR 720×480 as covering
    // 640×480/320×240: that skipped the weak ladder and broke G0b (mode→PMS encode).
    // Never direct FOAR 720x480 into HD either (not an exact match).
    const bool metaOk = metaFound;
    if (metaOk && playWeak.owenb) {   // BUG1 pre-play: a known-CAVLC source above OB_CAVLC_HARD_MAX_KBPS
        auto envKbps = [](const char* a, const char* b, int def) {
            for (const char* k : {a, b})
                if (const char* v = std::getenv(k); v && *v && std::atoi(v) > 0)
                    return std::atoi(v);
            return def;
        };
        const ObSourceEntropy src = parseObSourceEntropy(xml);
        const int hard = envKbps("OB_CAVLC_HARD_MAX_KBPS", "MPX_OB_CAVLC_HARD_MAX_KBPS", 7000);
        const int cap = envKbps("OB_CAVLC_MAX_KBPS", "MPX_OB_CAVLC_MAX_KBPS", 4000);
        const int tsafe = envKbps("OB_TRANSCODE_SAFE_KBPS", "MPX_OB_TRANSCODE_SAFE_KBPS", 3200);
        const int beforeTs = playWeak.maxVideoBitrateKbps;
        if (owenbTranscodeSafeLadder(playWeak, src, tsafe))
            std::fprintf(stderr,
                         "misterplexd: OB pre-play: source %s (not H.264): PMS must transcode, first request "
                         "maxVideoBitrate %d -> %d (OB_TRANSCODE_SAFE_KBPS)\n",
                         src.codec.c_str(), beforeTs, playWeak.maxVideoBitrateKbps);
        const int before = playWeak.maxVideoBitrateKbps;
        if (owenbPrePlayCavlcLadder(playWeak, src, hard, cap))
            std::fprintf(stderr,
                         "misterplexd: OB pre-play: source CAVLC (profile=%s cabac=%d) at %d kbps > "
                         "OB_CAVLC_HARD_MAX_KBPS=%d: first request maxVideoBitrate %d -> %d\n",
                         src.profile.c_str(), src.cabac, src.bitrateKbps, hard, before,
                         playWeak.maxVideoBitrateKbps);
        else
            std::fprintf(stderr, "misterplexd: OB pre-play: source %s profile=%s cabac=%d %d kbps: ladder %d kbps\n",
                         src.codec.empty() ? "-" : src.codec.c_str(), src.profile.empty() ? "-" : src.profile.c_str(),
                         src.cabac, src.bitrateKbps, playWeak.maxVideoBitrateKbps);
    }
    const bool isH264 = metaOk && mediaVideoIsH264(xml);
    const bool mediaMatchesDecode = decodeW > 0 && decodeH > 0 && r.mediaWidth == decodeW &&
                                    r.mediaHeight == decodeH;
    // Optional profile tag for operator logs (High often implies CABAC sticky skip).
    auto videoProfileNote = [&]() -> std::string {
        if (!metaOk)
            return {};
        // Prefer video Stream profile, then Media@videoProfile / videoCodec.
        size_t sp = 0;
        while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
            auto end = xml.find('>', sp);
            if (end == std::string::npos)
                break;
            const std::string slice = xml.substr(sp, end - sp);
            const bool isVideo = slice.find("streamType=\"1\"") != std::string::npos ||
                                 slice.find("type=\"video\"") != std::string::npos;
            if (isVideo) {
                auto p = attrIn(slice, "profile");
                if (p.empty())
                    p = attrIn(slice, "videoProfile");
                if (!p.empty())
                    return p;
                break;
            }
            sp = end + 1;
        }
        auto p = attr(xml, "Media", "videoProfile");
        if (p.empty())
            p = attr(xml, "Media", "videoCodec");
        return p;
    };
    // STREAM=0 rate: direct Part only when Media exactly matches DECODE *and* profile
    // is baseline/main. High@L4 Grid720 measured ~17 pfps direct vs ~18.6 via Main
    // universal + scale bypass — keep weak ladder for High.
    const std::string srcProfEarly = videoProfileNote();
    auto profLower = srcProfEarly;
    for (char& c : profLower) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    const bool lightDirectProfile =
        profLower.find("baseline") != std::string::npos ||
        profLower == "main" || profLower.find("constrained baseline") != std::string::npos ||
        (profLower.find("main") != std::string::npos &&
         profLower.find("high") == std::string::npos);
    const bool wantDirect = !fpgaBackend &&
        key.rfind("/library", 0) == 0 &&
        (preferDirectH264 || (mediaMatchesDecode && lightDirectProfile));
    const bool directH264 = wantDirect && isH264;
    if (directH264) {
        const std::string prof = videoProfileNote();
        const std::string profSuffix = prof.empty() ? "" : (" profile=" + prof);
        auto partKey = attr(xml, "Part", "key");
        if (!partKey.empty()) {
            if (partKey[0] != '/')
                partKey = "/" + partKey;
            r.playable = plexBase + partKey;
            if (!token.empty())
                r.playable += (r.playable.find('?') == std::string::npos ? "?" : "&") +
                              std::string("X-Plex-Token=") + urlEncodeQuery(token);
            r.ok = true;
            r.transcoded = false;
            r.detail = std::string("direct H.264 Part (") +
                       (preferDirectH264 ? "STREAM" : "media_covers_decode") + profSuffix + ")";
            return r;
        }
        auto file = attr(xml, "Part", "file");
        if (!file.empty()) {
            r.playable = urlDecode(file);
            if (libraryKeyMustNotSpawnLocalFile(key, r.playable)) {
                r.ok = false;
                r.playable.clear();
                r.transcoded = false;
                r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
                return r;
            }
            r.ok = true;
            r.transcoded = false;
            r.detail = std::string("direct H.264 Part file (") +
                       (preferDirectH264 ? "STREAM" : "media_covers_decode") + profSuffix + ")";
            return r;
        }
        // Fall through to universal if Part missing
    }

    // Reason the universal ladder was rejected by validateWeakLadder(); surfaces
    // in r.detail instead of the old misleading "remote transcoder" text.
    std::string whyBuild;
    // Prefer weak universal for dual A9 (STREAM=0 cast path / non-H.264 STREAM)
    if ((weakAlways || fpgaBackend) && key.rfind("/library", 0) == 0) {
        WeakLadder requested = weak;
        if (fpgaBackend) {
            fpgaMode = "240p";
            const char* prototype = std::getenv("MPX_H264_PROTOTYPE");
            const char* filtering = std::getenv("MPX_H264_FILTER");
            if (filtering && std::string(filtering) != "on" && std::string(filtering) != "off") {
                r.detail = "FPGA H.264 filtering must be explicitly on or off";
                return r;
            }
            int exactNum = 0, exactDen = 0;
            // Prefer Stream@frameRate; also accept Media@videoFrameRate (PAL/NTSC/24p).
            // No numeric rate is decided after the raster mode: NTSC rasters must not guess.
            const bool fpsMissing =
                !parseExactFps(r.videoFrameRate, r.frameRate, exactNum, exactDen);
            // The ladder's FPGA mode (fpgaModeForLadder); 480p and 480i may use the
            // reserved 480 experiment ladders.
            fpgaMode = fpgaModeForLadder(weak);
            const bool allowReserved = fpgaMode != "240p";
            // True 480i with the lab ladder (MPX_FPGA_MODE=480i, PLEX_DECODE_CODED_640)
            // maps x one-to-one across 640 px, so anything narrower leaves 16 stale
            // columns. Without a helper, PMS FPGA profiles are 624: that is the product
            // noproxy path for every raster (mode table, not a 480i-only refuse).
            if (fpgaMode == "480i" && ladderProxyConfigured()) {
                const int codedW = fpga480pCodedWidthCeiling();
                if (codedW != 640) {
                    r.detail = "FPGA 480i raster: coded width limit is " + std::to_string(codedW) +
                               ", not 640 (set FPGA_480P_CODED_W=640); a " +
                               std::to_string(codedW) +
                               "-wide picture leaves stale columns; refusing (no 624 fallback)";
                    std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                    return r;
                }
            }
            if (fpsMissing) {
                const bool proxyProbe = ladderProxyConfigured();
                if (fpgaRasterNtsc(fpgaMode) && proxyProbe && !attr(xml, "Part", "key").empty()) {
                    // Lab helper probes the Part and keeps its rate (&fps=src).
                    fpgaFpsSrc = true;
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s raster fps not in metadata (vfr=%s "
                                 "frameRate=%s): ladder &fps=src, the proxy probes the source "
                                 "rate\n",
                                 fpgaModeName(fpgaMode),
                                 r.videoFrameRate.c_str(), r.frameRate.c_str());
                } else if (fpgaRasterNtsc(fpgaMode) && proxyProbe) {
                    r.detail = std::string("FPGA ") + fpgaModeName(fpgaMode) +
                               " raster: no numeric frame rate in metadata (vfr=" +
                               r.videoFrameRate + " frameRate=" + r.frameRate +
                               ") and no Part to probe it; refusing (no remap)";
                    std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                    return r;
                } else {
                    // No helper, or 480p: PMS universal at the product clock. Remote
                    // metadata often omits numeric fps; 24000/1001 is the noproxy default
                    // for every raster.
                    exactNum = 24000;
                    exactDen = 1001;
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s fps meta missing; default 24000/1001 "
                                 "for direct PMS (vfr=%s frameRate=%s)\n",
                                 fpgaModeName(fpgaMode),
                                 r.videoFrameRate.c_str(), r.frameRate.c_str());
                }
            }
            // Every FPGA raster: 24-family is 24000/1001, 30-family is 30000/1001.
            // 60-family becomes 30000/1001 (2:2 on 59.94). PAL dumps to film on
            // 480p; 240p/480i refuse anything outside 24/30/60-family.
            const bool fpsProxySet = ladderProxyConfigured();
            const bool ntscRaster = fpgaRasterNtsc(fpgaMode);
            const char* raster = fpgaModeName(fpgaMode);
            // fps=src: the rate stays unknown (0/0) until the proxy probes it.
            if (!fpgaFpsSrc) {
                int outNum = 0, outDen = 0;
                const bool fpsChanged =
                    fpgaContentFps(exactNum, exactDen, ntscRaster, outNum, outDen);
                const bool src24 = fpsIsTrue24(exactNum, exactDen);
                const bool src30 = fpsIsTrue30(exactNum, exactDen);
                const bool src60 = (exactNum == 60 && exactDen == 1) ||
                                   (exactNum == 60000 && exactDen == 1001);
                if (fpgaMode == "480p" && !fpga480pMinFpsOk(exactNum, exactDen)) {
                    r.fpsNum = exactNum;
                    r.fpsDen = exactDen;
                    r.detail = std::string("FPGA 480p raster: source fps ") +
                               std::to_string(exactNum) + "/" +
                               std::to_string(exactDen) +
                               " is below 480p min_fps " +
                               std::to_string(kMinFps480p) +
                               "; refusing (no remap)";
                    std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                    return r;
                }
                if (ntscRaster) {
                    const std::string src =
                        std::to_string(exactNum) + "/" + std::to_string(exactDen);
                    std::string refusal;
                    if (fpsChanged && !src24 && !src30 && !src60)
                        refusal = "source fps " + src + " is not 24/30/60-family";
                    if (refusal.empty() && fpgaMode == "480i" &&
                        !fpga480iMinFpsOk(exactNum, exactDen))
                        refusal = "source fps " + src + " is below 480i min_fps " +
                                  std::to_string(kMinFps480i);
                    if (refusal.empty() && fpgaMode == "240p" &&
                        !fpga240pMinFpsOk(exactNum, exactDen))
                        refusal = "source fps " + src + " is below 240p min_fps " +
                                  std::to_string(kMinFps240p);
                    if (!refusal.empty()) {
                        r.fpsNum = exactNum;
                        r.fpsDen = exactDen;
                        r.detail = std::string("FPGA ") + raster + " raster: " + refusal +
                                   "; refusing (no remap)";
                        std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                        return r;
                    }
                }
                const char* route =
                    fpsProxySet ? "ladder proxy fps=src"
                    : std::strcmp(plexLocationForBase(plexBase), "wan") == 0
                        ? "no ladder proxy; remote PMS universal"
                        : "no ladder proxy; PMS source-rate profile";
                if (fpsChanged && src60)
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s raster fps halve %d/%d → %d/%d (%s)\n",
                                 raster, exactNum, exactDen, outNum, outDen, route);
                else if (fpsChanged)
                    std::fprintf(stderr,
                                 "misterplexd: FPGA remap source fps %d/%d → %d/%d "
                                 "(PMS transcode)\n",
                                 exactNum, exactDen, outNum, outDen);
                else if (fpsIsTrue30(outNum, outDen))
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s raster fps passthrough %d/%d (%s)\n",
                                 raster, outNum, outDen, route);
                exactNum = outNum;
                exactDen = outDen;
            }
            r.fpsNum = exactNum;
            r.fpsDen = exactDen;
            // Film-rate profile still sets the ladder geometry. A 30-family rate then
            // swaps in the raster-specific src30 variant; noprofile Extra pins
            // -r 30000/1001 (videoFrameRate on the universal URL).
            const bool fpsFilm = fpsIsTrue24(exactNum, exactDen);
            const int priorBr = weak.maxVideoBitrateKbps;
            const int priorQ = weak.videoQuality;
            if (!applyFpgaPlexProfile(requested, prototype ? prototype : "ip",
                                      fpsFilm ? exactNum : 24000, fpsFilm ? exactDen : 1001,
                                      filtering && std::string(filtering) == "off", &r.detail,
                                      fpgaMode, allowReserved))
                return r;
            // Overlay codec/profile/channels only. Leave FPGA videoResolution at
            // applyFpgaPlexProfile / fpga480pCodedWidthCeiling (624x480, not host-paint 720).
            if (const ModeResolveRow* row = modeResolveRow(fpgaMode)) {
                requested.videoCodec = row->videoCodec;
                requested.audioCodec = row->audioCodec;
                requested.h264ProfileParam = row->h264ProfileParam;
                requested.audioChannels = row->audioChannels;
            }
            if (requested.highCabac) {
                // Mode row is baseline,main. Plex_high rejects that PPS (error 16).
                requested.h264Profile = "main";
                requested.h264ProfileParam = "main";
                std::fprintf(stderr,
                             "misterplexd: High core CABAC ladder profile=main cabac=1 "
                             "ref=1 no-B no-8x8 (no named profile)\n");
            }
            if (fpsIsTrue30(exactNum, exactDen)) {
                requested.fpgaProfileVariant = fpgaNtscSourceProfileName(
                    prototype ? prototype : "ip", raster,
                    filtering && std::string(filtering) == "off");
                requested.fpgaFpsNum = exactNum;
                requested.fpgaFpsDen = exactDen;
            }
            // applyFpgaPlexProfile resets to profile defaults (4 Mbps / q60). Preserve
            // operator WEAK_* and always keep 240p under the 8KiB AU ring budget.
            if (priorBr > 0 && priorBr < requested.maxVideoBitrateKbps)
                requested.maxVideoBitrateKbps = priorBr;
            if (priorQ > 0 && priorQ < requested.videoQuality)
                requested.videoQuality = priorQ;
            if (fpgaMode == "240p") {
                constexpr int kStartMaxBitrateKbps = 1500;
                constexpr int kStartMaxQuality = 40;
                if (requested.maxVideoBitrateKbps > kStartMaxBitrateKbps)
                    requested.maxVideoBitrateKbps = kStartMaxBitrateKbps;
                if (requested.videoQuality > kStartMaxQuality)
                    requested.videoQuality = kStartMaxQuality;
            }
            // Seek/offset restarts: default FPGA ladder IDRs measured 10489 > 8192 AU.
            // Keep geometry/profile; only clamp bitrate/quality so PMS emits smaller AUs.
            if (offsetMs > 0) {
                // Live seek: 10489@4M, 8592@1M, 8s@750 PASS, cold@30s 8882@750 FAIL.
                // Drop further so complex-scene IDRs fit 8KiB AU ring.
                // Resume complex-scene IDRs still measured >8KiB at 500k; 350 keeps ring.
                const FpgaAuSafeClamp auClamp =
                    fpgaAuSafeClamp(fpgaAuSafeTier480(requested) || fpgaMode == "480p" ||
                                   fpgaMode == "480i",
                                   offsetMs, true);
                if (requested.maxVideoBitrateKbps > auClamp.maxBitrateKbps)
                    requested.maxVideoBitrateKbps = auClamp.maxBitrateKbps;
                if (requested.videoQuality > auClamp.maxQuality)
                    requested.videoQuality = auClamp.maxQuality;
            }
        }
        const WeakLadder transcodeWeak = fitWeakLadderToAspect(requested, r.sourceAspect);
        const std::string session = makeSessionId();
const std::string start = buildUniversalTranscodeUrl(plexBase, key, token, session,
                                                              offsetMs > 0 ? offsetMs : 0,
                                                              transcodeWeak, &whyBuild);
        const bool remoteBase = std::strcmp(plexLocationForBase(plexBase), "wan") == 0;
        // conf MPX_H264_PROTOTYPE=idr: PMS universal is never all-IDR. Prefer the lab
        // FFmpeg ladder for every remote cast when a proxy is configured (fallback
        // block below builds /ladder.ts). Skip dead/working remote universal alike.
        if (fpgaBackend && metaOk) {
            const char* prototypeIdr = std::getenv("MPX_H264_PROTOTYPE");
            const std::string proxyEarly = ladderProxyUrl();
            const bool helperOn = !proxyEarly.empty();
            const bool idrLadder =
                prototypeIdr && std::string(prototypeIdr) == "idr" && helperOn;
            // 480 coded path needs pad+setsar 624x480; PMS universal often ships
            // 624x416 DAR letterbox which never presents on e12 glass.
            const bool ladder480 =
                helperOn &&
                (fpgaMode == "480p" || fpgaMode == "480i" ||
                 transcodeWeak.profileName == "fpga-h264-480p" ||
                 transcodeWeak.profileName == "480p");
            // NTSC raster + 30-family rate: with a proxy the lab ladder carries the source
            // rate (&fps=src). Without one, PMS universal uses the source-rate profile. An
            // unknown rate (fps=src) always takes the ladder: the proxy probes it.
            const bool ntscLadder30 = helperOn &&
                                      fpgaRasterNtsc(fpgaMode) &&
                                      (fpsIsTrue30(r.fpsNum, r.fpsDen) || fpgaFpsSrc);
            // 240p on the 480p ABI (480-tier conf ladder) uses the proxy's 240crf16 rung
            // for every family, as 480p/480i use the pad ladder.
            int ladder240W = 0, ladder240H = 0;
            parseResolution(weak.videoResolution, ladder240W, ladder240H);
            const bool ladder240 = helperOn && fpgaMode == "240p" &&
                                   (weak.profileName == "480p" ||
                                    weak.profileName == "fpga-h264-480p" ||
                                    ladder240W >= 480 || ladder240H >= 480);
            // forceLadder takes the helper only when one is configured. With none,
            // fall through to PMS universal (never a direct Part).
            if ((forceLadder && helperOn) || (remoteBase && idrLadder) || ladder480 ||
                ntscLadder30 || ladder240) {
                // Fall through past universal to the Part/proxy path.
                if (forceLadder)   // SPS-gate L3 (s2): the gate fallback's own first detail, ahead of the chain
                    r.detail = "gate fallback: " + gateDetail + "; skip PMS universal → lab ladder";
                else
                r.detail = ladder480 ? "480p conf; skip PMS universal → lab pad ladder"
                           : (remoteBase && idrLadder)
                               ? "remote all-IDR conf; skip PMS universal → lab ladder"
                           : fpgaFpsSrc
                               ? "NTSC raster fps unknown; skip PMS universal → lab ladder fps=src"
                           : ntscLadder30
                               ? "NTSC raster 30-family fps; skip PMS universal → lab ladder"
                               : "240p raster on the 480p ABI; skip PMS universal → 240crf16 ladder";
            } else if (remoteBase &&
                       ensureUniversalDecision(start, session, token, transcodeWeak)) {
                goto remote_universal_decision_ok;
            } else if (!remoteBase &&
                       ensureUniversalDecision(start, session, token, transcodeWeak)) {
                goto remote_universal_decision_ok;
            } else if (fpgaBackend && !remoteBase && helperOn) {
                if (fpgaRasterNtsc(fpgaMode) && fpsIsTrue30(r.fpsNum, r.fpsDen)) {
                    r.detail = std::string("FPGA ") + fpgaModeName(fpgaMode) + " raster fps " +
                               std::to_string(r.fpsNum) + "/" + std::to_string(r.fpsDen) +
                               ": ladder helper configured and PMS profile " +
                               plexRequestProfileName(transcodeWeak) + " (variant " +
                               transcodeWeak.fpgaProfileVariant + ")" +
                               " decision failed; refusing (no remap)";
                    std::fprintf(stderr, "misterplexd: ERROR %s\n", r.detail.c_str());
                    return r;
                }
                r.detail =
                    "FPGA H.264 universal profile decision failed; direct/software fallback forbidden";
                return r;
            } else {
                r.detail = remoteBase ? "remote universal decision failed; trying Part"
                                      : "universal decision failed; trying direct part";
                if (forceLadder)
                    r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
                if (!whyBuild.empty())
                    r.detail += " (" + whyBuild + ")";
            }
        } else if (ensureUniversalDecision(start, session, token, transcodeWeak)) {
        remote_universal_decision_ok:
            // Shared/remote PMS often advertises Conversion OK then closes start with
            // 0 body (no working transcoder for the share). Probe before claiming ok.
            bool startAlive = true;
            if (remoteBase) {
                const std::vector<std::pair<std::string, std::string>> probeHdr =
                    plexStartProbeHeaders(session, token, transcodeWeak, start);
                if (transcodeWeak.highCabac) {
                    // VideoEncodeFlags is percent-encoded, so the header holds cabac%3D1.
                    bool sawHeader = false;
                    bool sawCabac = false;
                    for (const auto& hv : probeHdr) {
                        if (hv.first == "X-Plex-Client-Profile-Extra")
                            sawHeader = true;
                        if (hv.second.find("cabac=1") != std::string::npos ||
                            hv.second.find("cabac%3D1") != std::string::npos ||
                            hv.second.find("cabac%3d1") != std::string::npos)
                            sawCabac = true;
                    }
                    std::fprintf(stderr, "misterplexd: start probe Profile-Extra %s\n",
                                 sawCabac ? "cabac=1"
                                          : sawHeader ? "header-without-cabac=1"
                                                      : "no-Profile-Extra");
                } else if (isFpgaPlexProfile(transcodeWeak)) {
                    bool sawCabac0 = false;
                    for (const auto& hv : probeHdr) {
                        if (hv.second.find("cabac=0") != std::string::npos ||
                            hv.second.find("cabac%3D0") != std::string::npos ||
                            hv.second.find("cabac%3d0") != std::string::npos)
                            sawCabac0 = true;
                    }
                    std::fprintf(stderr, "misterplexd: start probe Profile-Extra %s\n",
                                 sawCabac0 ? "cabac=0" : "no-cabac=0");
                }
                // Unset: one ranged GET (status + leading bytes). MPX_PREWARM=0
                // keeps the two-GET drain that held 075 for ~20s after the decision.
                const bool prewarmProbe = plexPrewarmProbeEnabled();
                int probeHttp = 0;
                std::string probeBody;
                const auto probeStart = std::chrono::steady_clock::now();
                if (prewarmProbe) {
                    const auto probe = plexHttpGetStatusAndBody(start, probeHdr,
                                                                plexRequestProfileName(transcodeWeak, start),
                                                                12);
                    probeHttp = probe.httpStatus;
                    probeBody = probe.body;
                } else {
                    const auto probe = plexHttpGetNoBodyResult(start, probeHdr, 12);
                    probeHttp = probe.httpStatus;
                }
                // 206/200 with body is ideal; 0-byte 200 or 4xx/5xx ⇒ dead transcoder.
                startAlive = plexHttpStatusOk(probeHttp) && probeHttp != 204;
                // Companion cannot see body size via no-body helper; do a tiny GET.
                if (startAlive) {
                    if (!prewarmProbe)
                        probeBody = httpGet(start, 12, curlHeaderArgs(probeHdr),
                                            plexRequestProfileName(transcodeWeak, start));
                    if (probeBody.size() < 64 || probeBody.find("<html") != std::string::npos ||
                        probeBody.find("Bad Request") != std::string::npos ||
                        probeBody.find("MediaContainer") != std::string::npos) {
                        startAlive = false;
                    }
                }
                const auto probeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - probeStart)
                                         .count();
                std::fprintf(stderr,
                             "misterplexd: remote start probe ms=%lld http=%d bytes=%zu prewarm=%d\n",
                             static_cast<long long>(probeMs), probeHttp, probeBody.size(),
                             prewarmProbe ? 1 : 0);
                if (!startAlive) {
                    std::fprintf(stderr,
                                 "misterplexd: remote universal start dead http=%d — "
                                 "shared PMS transcoder produced no media\n",
                                 probeHttp);
                }
            }
            {
                const DecisionVideo dv = lastDecisionVideo();
                if (dv.width > 0 && dv.height > 0) {
                    r.decisionWidth = dv.width;
                    r.decisionHeight = dv.height;
                }
                const std::string partMm = decisionCoreMismatch(dv, transcodeWeak);
                std::string partKey = dv.partKey;
                if (partKey.empty())
                    partKey = attr(xml, "Part", "key");
                const bool takePart = decisionIsDirectPlay(dv) && partMm.empty() && !partKey.empty();
                if (takePart || startAlive) {
                r.ok = true;
                r.httpHeaders = plexFfmpegHeaders(session, token, transcodeWeak,
                                                  /*forceLadderCaps=*/remoteBase, start);
                if (transcodeWeak.highCabac) {
                    const bool playCabac = r.httpHeaders.find("cabac=1") != std::string::npos ||
                                           r.httpHeaders.find("cabac%3D1") != std::string::npos ||
                                           r.httpHeaders.find("cabac%3d1") != std::string::npos;
                    std::fprintf(stderr, "misterplexd: PLAY Profile-Extra %s\n",
                                 playCabac ? "cabac=1" : "no-cabac=1");
                }
                r.clientProfileName = plexRequestProfileName(transcodeWeak, start);
                if (takePart) {
                    if (partKey[0] != '/')
                        partKey = "/" + partKey;
                    r.transcoded = false;
                    r.playable = plexBase + partKey;
                    if (!token.empty())
                        r.playable += (r.playable.find('?') == std::string::npos ? "?" : "&") +
                                      std::string("X-Plex-Token=") + urlEncodeQuery(token);
                    r.detail = "PMS direct " + transcodeWeak.profileName + " " +
                               transcodeWeak.videoResolution + " " + key +
                               " part=" + partKey + " part_mismatch=";
                } else {
                    r.transcoded = true;
                    r.playable = start;
                    r.detail = "PMS universal " + transcodeWeak.profileName + " " +
                               transcodeWeak.videoResolution + " " + key +
                               " part_mismatch=" + partMm;
                }
                if (forceLadder)
                    r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
                if (playableIsFarpointIdentityCache(r.playable) ||
                    libraryKeyMustNotSpawnLocalFile(key, r.playable)) {
                    r.ok = false;
                    r.playable.clear();
                    r.transcoded = false;
                    r.detail = "refusing farpoint_1280x720.mp4 cache bypass";
                    return r;
                }
                // STREAM preferDirect fallthrough: operator can see why recon may hit CABAC.
                if (preferDirectH264) {
                    if (!metaOk)
                        r.detail += " (STREAM preferDirect: no metadata)";
                    else if (!isH264)
                        r.detail += " (STREAM preferDirect: source not H.264)";
                    else
                        r.detail += " (STREAM preferDirect: H.264 Part missing → universal may be High/CABAC)";
                }
                return r;
                }
            }
            // Remote decision OK but start body empty: try direct Part / lab proxy below.
            r.detail = "remote PMS transcoder returned empty body; trying direct Part";
        } else if (fpgaBackend && !remoteBase && ladderProxyConfigured()) {
            r.detail =
                "FPGA H.264 universal profile decision failed; direct/software fallback forbidden";
            return r;
        } else {
            r.detail = remoteBase ? "remote universal decision failed; trying Part"
                                  : "universal decision failed; trying direct part";
            if (forceLadder)
                r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
        }
    }

    // Fallback: direct Part stream URL from metadata, or lab ladder proxy for remote.
    if (metaFound) {
        auto partKey = attr(xml, "Part", "key");
        if (!partKey.empty()) {
            if (partKey[0] != '/')
                partKey = "/" + partKey;
            std::string partUrl = plexBase + partKey;
            if (!token.empty())
                partUrl += (partUrl.find('?') == std::string::npos ? "?" : "&") +
                           std::string("X-Plex-Token=") + urlEncodeQuery(token);

            // Lab host FFmpeg proxy: re-ladder remote Parts into FPGA all-IDR mpegts.
            // Debug opt-in: MPX_REMOTE_LADDER_PROXY / REMOTE_LADDER_PROXY non-empty.
            const std::string proxy = ladderProxyUrl();
            const char* prototypeNow = std::getenv("MPX_H264_PROTOTYPE");
            const bool idrNeed =
                prototypeNow && std::string(prototypeNow) == "idr";
            int ladderW = 0, ladderH = 0;
            parseResolution(weak.videoResolution, ladderW, ladderH);
            const bool tier480Ladder =
                weak.profileName == "480p" || weak.profileName == "fpga-h264-480p" ||
                ladderW >= 480 || ladderH >= 480;
            // Local 480 FOAR PMS universal often emits 624x416 (DAR letterbox) without
            // pad; that fails first-present on e12. Lab ladder forces pad+setsar.
            // NTSC raster + 30-family rate (or fps=src): the ladder &fps= carries it.
            const bool ntscFps30 = fpgaBackend && fpgaRasterNtsc(fpgaMode) &&
                                   (fpsIsTrue30(r.fpsNum, r.fpsDen) || fpgaFpsSrc);
            // A 240p raster on a 480-tier conf ladder (MPX_FPGA_MODE=240p on the 480p
            // ABI) decodes coded 320x240 into the 640-pitch DPB: every family takes the
            // 320x240 ladder rung there, never the 624/640x480 pad ladder.
            const bool raster240Ladder = fpgaBackend && fpgaMode == "240p" && tier480Ladder;
            const bool pad480 = tier480Ladder && !raster240Ladder;
            const bool wantProxy =
                fpgaBackend && !proxy.empty() &&
                (forceLadder ||   // SPS-gate L3 b1: the gate fallback takes the ladder whatever the conf terms
                 std::strcmp(plexLocationForBase(plexBase), "wan") == 0 ||
                 !mediaVideoIsH264(xml) || idrNeed || tier480Ladder || ntscFps30);
            if (wantProxy) {
                std::string proxyBase = proxy;
                while (!proxyBase.empty() && proxyBase.back() == '/')
                    proxyBase.pop_back();
                int pw = pad480 ? 624 : 320;
                int ph = pad480 ? 480 : 240;
                // AU32 240p: default ~2.5 Mbps all-IDR (was 800k/1Mbps for 8KiB ring).
                int pbr = pad480 ? 1500 : 2500;
                if (ladderW > 0 && !raster240Ladder)
                    pw = ladderW;
                if (ladderH > 0 && !raster240Ladder)
                    ph = ladderH;
                // Coded 480 FPGA rectangle: 624×480 (39 MB) by default; TRUE640
                // 640×480 (40 MB, PLEX_DECODE_CODED_640 cores) only with the
                // MPX_FPGA_480P_CODED_W=640 opt-in. The ladder proxy encodes
                // exactly this w, so the switch holds end to end.
                if (pad480) {
                    const int maxCodedW = fpga480pCodedWidthCeiling();
                    if (pw > maxCodedW || pw == 0)
                        pw = maxCodedW;
                    if (ph == 0 || ph > 480)
                        ph = 480;
                }
                if (weak.maxVideoBitrateKbps > 0)
                    pbr = weak.maxVideoBitrateKbps;
                const int brCap = pad480 ? 2500 : 3000;
                if (pbr > brCap)
                    pbr = brCap;
                if (pw <= 0)
                    pw = pad480 ? 624 : 320;
                if (ph <= 0)
                    ph = pad480 ? 480 : 240;
                // Prefer measured source fps when true24; else product 24000/1001.
                // NTSC rasters (240p, 480i) always ask the proxy for the source rate
                // (&fps=src, DAEMON-FPS-SRC-ALWAYS-1003): it probes the Part and keeps
                // a 24/30-family rate (60-family halved), so no metadata rate can turn
                // into a remap on the ladder. A numeric metadata rate stays the expected
                // rate (pacing, and the demux PTS identity check, which refuses a stream
                // at any other rate); fps-less titles stay 0/0.
                const bool ntscSrc = fpgaBackend && fpgaRasterNtsc(fpgaMode);
                std::string fpsQ = "24000/1001";
                if (fpgaFpsSrc || ntscSrc)
                    fpsQ = "src";
                else if (r.fpsNum > 0 && r.fpsDen > 0)
                    fpsQ = std::to_string(r.fpsNum) + "/" + std::to_string(r.fpsDen);
                if (ntscSrc && !fpgaFpsSrc)
                    std::fprintf(stderr,
                                 "misterplexd: FPGA %s raster fps=src to the ladder proxy "
                                 "(metadata rate %d/%d stays the expected rate)\n",
                                 fpgaModeName(fpgaMode), r.fpsNum, r.fpsDen);
                // MPX_BROWSE_V2 only (weak fields empty for every cast): the ladder input a
                // browse play ticket chose (lead Q4). The helper's own URL wins: for
                // "server-transcode" that is its relay (mpx-menu 59044aa, /menu/tsrc/<slug>/<rk>/
                // o<ms>, contract §9.3), which carries the offset in its path (no &offset_ms) and
                // gets an explicit fps (tsrcRelayLadderFps), else the original Part. With no helper
                // URL, "server-transcode" asks the owning server for its own transcode at the
                // offset, used only when it streams the way the ladder will fetch it (no X-Plex
                // headers); else the metadata Part as before.
                std::string ladderSrc =
                    weak.ladderSrcOverride.empty() ? partUrl : weak.ladderSrcOverride;
                bool ladderSrcAtOffset = false;
                if (weak.ladderServerTranscode && !weak.ladderSrcOverride.empty()) {
                    int tn = 0, td = 0;
                    if (tsrcRelayLadderFps(r.fpsNum, r.fpsDen, tn, td)) {
                        fpsQ = std::to_string(tn) + "/" + std::to_string(td);
                        ladderSrcAtOffset = true;
                        std::fprintf(stderr,
                                     "misterplexd: browse ladder input: the helper's "
                                     "server-transcode relay (offset in its path, fps %s)\n",
                                     fpsQ.c_str());
                    } else {
                        ladderSrc = partUrl;
                        std::fprintf(stderr,
                                     "misterplexd: browse ladder input: the server-transcode "
                                     "relay needs a 24/30/60-family metadata rate (got %d/%d); "
                                     "the original Part instead\n",
                                     r.fpsNum, r.fpsDen);
                    }
                }
                if (weak.ladderServerTranscode && weak.ladderSrcOverride.empty()) {
                    WeakLadder ow = weak;
                    ow.ladderServerTranscode = false;
                    const WeakLadder owFit = fitWeakLadderToAspect(ow, r.sourceAspect);
                    const std::string tsSession = makeSessionId();
                    const bool tsWan = std::strcmp(plexLocationForBase(plexBase), "wan") == 0;
                    std::string tsStart;
                    const std::string tsSrc = ownerTranscodeLadderSrc(
                        plexBase, key, token, tsSession, offsetMs, owFit, tsWan, &tsStart);
                    std::string tsWhy;
                    if (tsStart.empty()) {
                        tsWhy = "no transcode URL for this raster";
                    } else if (!ensureUniversalDecision(tsStart, tsSession, token, owFit,
                                                        /*forceLadderCaps=*/tsWan)) {
                        tsWhy = "its transcode decision failed";
                    } else {
                        const LadderStartProbe pr = probeStartLikeLadder(tsSrc, 12);
                        if (!pr.media)
                            tsWhy = "its transcode does not stream without headers (http=" +
                                    std::to_string(pr.httpStatus) + " bytes=" +
                                    std::to_string(pr.bytes) + ")";
                    }
                    if (tsWhy.empty()) {
                        ladderSrc = tsSrc;
                        ladderSrcAtOffset = universalOffsetSeconds(offsetMs) > 0;
                        std::fprintf(stderr,
                                     "misterplexd: browse ladder input: the owning server's own "
                                     "transcode %s %d kbps (start %s)\n",
                                     owFit.videoResolution.c_str(), owFit.maxVideoBitrateKbps,
                                     ladderSrcAtOffset ? "at the offset" : "at 0");
                    } else {
                        std::fprintf(stderr,
                                     "misterplexd: browse ladder input: owning server: %s; the "
                                     "original Part instead\n",
                                     tsWhy.c_str());
                    }
                }
                r.playable = proxyBase + "/ladder.ts?src=" + urlEncodeQuery(ladderSrc) +
                             "&w=" + std::to_string(pw) + "&h=" + std::to_string(ph) +
                             "&br=" + std::to_string(pbr) + "&fps=" + fpsQ;
                r.viaLadderProxy = true;   // SPS-gate L3: the stream is the ladder helper's (GatePlayContext)
                // 240p: 2x lanczos intermediate then downscale before encode (less blocky).
                if (!pad480)
                    r.playable += "&hq=1";
                // Bake resume/seek into the ladder so FFmpeg -ss runs on the Part;
                // live mpegts is not demux-seekable (EPERM → bounce to title).
                if (offsetMs > 0 && !ladderSrcAtOffset)
                    r.playable += "&offset_ms=" + std::to_string(offsetMs);
                r.ok = true;
                r.transcoded = true;
                r.httpHeaders.clear();
                // fps=src keeps the rate unknown (0/0): pacing follows the stream PTS
                // and the demux skips its rate-identity check, so nothing is guessed.
                // An NTSC raster never takes the 24000/1001 default either.
                if ((r.fpsNum <= 0 || r.fpsDen <= 0) && !fpgaFpsSrc && !ntscSrc) {
                    r.fpsNum = 24000;
                    r.fpsDen = 1001;
                }
                const char* why =
                    pad480 ? "480p pad ladder"
                    : (idrNeed ? "all-IDR conf"
                       : fpgaFpsSrc ? (fpgaMode == "240p" ? "240p raster fps=src"
                                                          : "480i raster fps=src")
                       : ntscFps30 ? (fpgaMode == "240p" ? "240p raster fps passthrough"
                                                         : "480i raster fps passthrough")
                       : raster240Ladder ? "240p raster 240crf16 rung"
                       : forceLadder ? "gate fallback rung"
                                     : "remote PMS transcoder unavailable");
                r.detail = std::string("lab ladder proxy ") + std::to_string(pw) + "x" +
                           std::to_string(ph) + "@" + std::to_string(pbr) + "k (" + why + ")";
                if (offsetMs > 0)
                    r.detail += " offset_ms=" + std::to_string(offsetMs);
                if (forceLadder)   // T1f: the result's detail starts "gate fallback: "
                    r.detail = "gate fallback: " + gateDetail + "; " + r.detail;
                std::fprintf(stderr, "misterplexd: %s base=%s → %s\n", r.detail.c_str(),
                             plexBase.c_str(), proxyBase.c_str());
                return r;
            }

            // FPGA needs annex-B H.264 AUs. Direct Part is only viable when the
            // source is already H.264 (remote shared PMS often has a dead transcoder).
            if (fpgaBackend && !mediaVideoIsH264(xml)) {
                r.ok = false;
                r.playable.clear();
                if (!whyBuild.empty())
                    r.detail = "FPGA ladder rejected (" + whyBuild +
                               "); remote/shared PMS had no stream to fall back to "
                               "(source is not H.264)";
                else
                    r.detail =
                        "remote/shared PMS transcoder produced no stream and source is not "
                        "H.264";
                return r;
            }
            // Gate fallback already tried PMS universal; the Part is the stream the
            // gate refused. Never replay it as a "direct Part".
            if (forceLadder) {
                r.ok = false;
                r.playable.clear();
                r.detail = "gate fallback: " + gateDetail +
                           "; PMS universal failed (never a direct Part)";
                return r;
            }
            r.playable = partUrl;
            r.ok = true;
            r.transcoded = false;
            r.httpHeaders = plexFfmpegHeaders(makeSessionId(), token, weak);
            r.detail = fpgaBackend ? "direct H.264 Part (remote transcoder unavailable)"
                                   : "direct Part stream";
            return r;
        }
        auto file = attr(xml, "Part", "file");
        if (!file.empty()) {
            r.playable = urlDecode(file);
            r.ok = true;
            r.detail = "Part file path";
            return r;
        }
    }

    if (!metaFound)
        r.detail = "no such item on PMS (" + key + " returned no MediaContainer — "
                   "deleted, renumbered by a library re-scan, or bad token)";
    if (r.detail.empty())
        r.detail = "resolve failed for " + key;
    return r;
}

PlayQueue fetchPlayQueue(const std::string& queueIdOrContainerKey, const std::string& plexBase,
                         const std::string& token, const std::string& currentKey,
                         const std::string& playQueueItemId) {
    PlayQueue q;
    q.currentIndex = -1;
    std::string raw = urlDecode(queueIdOrContainerKey);
    std::string extraQuery;
    auto qmark = raw.find('?');
    if (qmark != std::string::npos) {
        extraQuery = raw.substr(qmark + 1);
        raw = raw.substr(0, qmark);
    }
    std::string id = raw;
    auto slash = id.find_last_of('/');
    if (slash != std::string::npos)
        id = id.substr(slash + 1);
    while (!id.empty() && !std::isdigit(static_cast<unsigned char>(id.back())))
        id.pop_back();
    if (id.empty() || !std::isdigit(static_cast<unsigned char>(id.front()))) {
        q.detail = "empty/invalid play queue id from '" + queueIdOrContainerKey + "'";
        return q;
    }

    std::string base = normalizePlexBase(plexBase);
    if (base.empty()) {
        q.detail = "no PMS base for play queue";
        return q;
    }

    std::string url = base + "/playQueues/" + id + "?";
    if (!extraQuery.empty())
        url += extraQuery + "&";
    else
        url += "own=1&";
    if (!token.empty())
        url += "X-Plex-Token=" + urlEncodeQuery(token);

    const std::string xml = httpGet(url, 20);
    if (xml.empty() || xml.find("MediaContainer") == std::string::npos) {
        q.detail = "play queue fetch failed id=" + id + " bytes=" + std::to_string(xml.size());
        return q;
    }
    return parsePlayQueueXml(xml, id, currentKey, playQueueItemId);
}

namespace {
// An attribute of the opening tag only (not "subtype=" nor a child element's attribute).
std::string openTagAttr(const std::string& slice, const char* name) {
    const auto tagEnd = slice.find('>');
    const std::string tag = slice.substr(0, tagEnd == std::string::npos ? slice.size() : tagEnd);
    const std::string key = std::string(" ") + name + "=\"";
    auto p = tag.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    const auto e = tag.find('"', p);
    return e == std::string::npos ? std::string() : tag.substr(p, e - p);
}
} // namespace

PlayQueue parsePlayQueueXml(const std::string& xml, const std::string& id, const std::string& currentKey,
                            const std::string& playQueueItemId) {
    PlayQueue q;
    q.currentIndex = -1;
    if (xml.find("MediaContainer") == std::string::npos) {
        q.detail = "play queue fetch failed id=" + id + " bytes=" + std::to_string(xml.size());
        return q;
    }
    q.containerKey = "/playQueues/" + id;
    q.playQueueId = id;
    {
        auto a = xml.find("playQueueID=\"");
        if (a != std::string::npos) {
            a += 13;
            auto b = xml.find('"', a);
            if (b != std::string::npos)
                q.playQueueId = xml.substr(a, b - a);
        }
        a = xml.find("playQueueVersion=\"");
        if (a != std::string::npos) {
            a += 18;
            auto b = xml.find('"', a);
            if (b != std::string::npos)
                q.playQueueVersion = xml.substr(a, b - a);
        }
    }

    size_t pos = 0;
    while ((pos = xml.find("<Video", pos)) != std::string::npos) {
        auto tagEnd = xml.find('>', pos);
        if (tagEnd == std::string::npos)
            break;
        size_t sliceEnd = tagEnd;
        auto end2 = xml.find("</Video>", pos);
        auto endSlash = xml.find("/>", pos);
        if (endSlash != std::string::npos && endSlash < tagEnd + 8)
            sliceEnd = endSlash;
        if (end2 != std::string::npos)
            sliceEnd = std::max(sliceEnd, end2);
        const std::string slice = xml.substr(pos, std::min<size_t>(4000, sliceEnd - pos + 8));

        QueueItem item;
        item.key = attrIn(slice, "key");
        item.ratingKey = attrIn(slice, "ratingKey");
        item.playQueueItemId = attrIn(slice, "playQueueItemID");
        if (item.playQueueItemId.empty())
            item.playQueueItemId = attrIn(slice, "playQueueItemId");
        item.title = attrIn(slice, "title");
        item.type = openTagAttr(slice, "type");
        auto d = attrIn(slice, "duration");
        if (!d.empty())
            item.durationMs = std::strtoll(d.c_str(), nullptr, 10);
        if (!item.key.empty())
            q.items.push_back(item);
        pos = tagEnd + 1;
    }

    if (q.items.empty()) {
        q.detail = "play queue had no Video items index=-1 none";
        return q;
    }

    const char* by = "none";
    q.currentIndex = selectQueueIndex(q.items, currentKey, playQueueItemId, &by);

    q.ok = true;
    q.detail = "queue size=" + std::to_string(q.items.size()) +
               (q.currentIndex >= 0
                    ? " index=" + std::to_string(q.currentIndex) + " by=" + by
                    : " index=-1 none");
    return q;
}

namespace {

int bucketFps(double fps) {
    if (fps <= 0.0)
        return 0;
    // Nearest of OSD Content FPS options 12 / 24 / 30 / 60
    const int opts[] = {12, 24, 30, 60};
    int best = 24;
    double bestDiff = 1e9;
    for (int o : opts) {
        double d = std::fabs(fps - static_cast<double>(o));
        if (d < bestDiff) {
            bestDiff = d;
            best = o;
        }
    }
    // 23.976 → 24, 29.97 → 30, 59.94 → 60
    return best;
}

} // namespace

int contentFpsHint(const std::string& videoFrameRate, const std::string& frameRate) {
    // Prefer numeric stream frameRate
    if (!frameRate.empty()) {
        char* end = nullptr;
        double v = std::strtod(frameRate.c_str(), &end);
        if (end != frameRate.c_str() && v > 0.0)
            return bucketFps(v);
    }
    if (videoFrameRate.empty())
        return 0;
    std::string s = videoFrameRate;
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Common PMS tokens
    if (s == "ntsc" || s.find("29.97") != std::string::npos || s == "30p" || s == "30")
        return 30;
    if (s == "pal" || s.find("25") == 0 || s == "25p")
        return 24; // nearest OSD bucket (no 25); cadence at 24 is less wrong than 30 for film-ish
    if (s == "24p" || s == "24" || s.find("23.9") != std::string::npos ||
        s.find("film") != std::string::npos)
        return 24;
    if (s == "60p" || s == "60" || s.find("59.9") != std::string::npos || s == "120p")
        return 60;
    if (s == "12p" || s == "12")
        return 12;
    // Strip trailing 'p' and parse number
    if (!s.empty() && (s.back() == 'p' || s.back() == 'i'))
        s.pop_back();
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && v > 0.0)
        return bucketFps(v);
    return 0;
}

int applySourceFpsConf(const std::string& sourceFpsConf, int resolvedHint) {
    std::string s = sourceFpsConf;
    // trim
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.empty() || s == "auto")
        return resolvedHint;
    if (s == "off" || s == "0" || s == "none")
        return 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end != s.c_str() && v > 0)
        return bucketFps(static_cast<double>(v));
    return resolvedHint;
}

namespace {

struct FpsRational {
    int num;
    int den;
};

// Standard broadcast rates. 1001-denominator entries must snap exactly: treating
// 23.976 as 24 costs ~1 ms/s of lipsync drift (~234 ms by 3:54, ~5.5 s over 91 min).
constexpr FpsRational kStdFps[] = {
    {12, 1},        {15, 1},        {24000, 1001}, {24, 1},       {25, 1},
    {30000, 1001},  {30, 1},        {48, 1},       {50, 1},       {60000, 1001},
    {60, 1},
};

// Snap a decimal rate onto the standard family. Tolerance is well under the
// 23.976↔24 gap (0.024) so the NTSC pairs never collapse into each other.
bool snapStdFps(double v, int& num, int& den) {
    if (!(v > 0.0) || v > 1000.0)
        return false;
    for (const auto& f : kStdFps) {
        const double std_v = static_cast<double>(f.num) / static_cast<double>(f.den);
        if (std::fabs(v - std_v) <= 0.010) {
            num = f.num;
            den = f.den;
            return true;
        }
    }
    // Not a standard rate — keep it exact to 3 decimals rather than rounding to int.
    long n = std::lround(v * 1000.0);
    if (n <= 0)
        return false;
    num = static_cast<int>(n);
    den = 1000;
    return true;
}

std::string lowerTrim(const std::string& in) {
    std::string s = in;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool parseExactFps(const std::string& videoFrameRate, const std::string& frameRate,
                   int& num, int& den) {
    num = 0;
    den = 0;
    // Prefer the numeric video Stream@frameRate — it carries the real 23.976/29.97.
    if (!frameRate.empty()) {
        const std::string rate = lowerTrim(frameRate);
        const auto slash = rate.find('/');
        if (slash != std::string::npos) {
            const std::string numerator = rate.substr(0, slash);
            const std::string denominator = rate.substr(slash + 1);
            char *nEnd = nullptr, *dEnd = nullptr;
            errno = 0;
            const long n = std::strtol(numerator.c_str(), &nEnd, 10);
            const bool nOk = errno == 0;
            errno = 0;
            const long d = std::strtol(denominator.c_str(), &dEnd, 10);
            if (nOk && errno == 0 && nEnd != numerator.c_str() && !*nEnd &&
                dEnd != denominator.c_str() && !*dEnd && n > 0 && d > 0 &&
                n <= std::numeric_limits<int>::max() && d <= std::numeric_limits<int>::max() &&
                static_cast<double>(n) / d <= 1000.0) {
                const long divisor = std::gcd(n, d);
                num = static_cast<int>(n / divisor);
                den = static_cast<int>(d / divisor);
                return true;
            }
        } else {
            char* end = nullptr;
            const double v = std::strtod(rate.c_str(), &end);
            if (end != rate.c_str() && !*end && v > 0.0 && snapStdFps(v, num, den)) {
                // PMS 24p/30p with Stream@frameRate "24.000"/"30.000" is NTSC
                // (048 H1 paced 24/1 and glass dupfps fell to 22.81).
                const std::string vfr = lowerTrim(videoFrameRate);
                if ((vfr == "24p" || vfr == "film") && num == 24 && den == 1) {
                    num = 24000;
                    den = 1001;
                } else if (vfr == "30p" && num == 30 && den == 1) {
                    num = 30000;
                    den = 1001;
                }
                return true;
            }
        }
    }
    if (videoFrameRate.empty())
        return false;

    std::string s = lowerTrim(videoFrameRate);
    if (s.empty())
        return false;
    // PMS Media@videoFrameRate tokens. NTSC/PAL are film/broadcast families, not integers.
    if (s == "ntsc") {
        num = 30000;
        den = 1001;
        return true;
    }
    if (s == "pal") {
        num = 25;
        den = 1;
        return true;
    }
    // PMS Media@videoFrameRate="24p" is NTSC film on Trek/Blu-ray (23.976).
    // True 24.000 still wins when Stream@frameRate is "24" / "24.000".
    if (s == "film" || s == "24p") {
        num = 24000;
        den = 1001;
        return true;
    }
    if (s == "30p") {
        num = 30000;
        den = 1001;
        return true;
    }
    // Strip a trailing progressive/interlaced marker ("24p", "60i").
    if (s.back() == 'p' || s.back() == 'i')
        s.pop_back();
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && v > 0.0)
        return snapStdFps(v, num, den);
    return false;
}

bool capExactFpsToMaxHz(int& num, int& den, double maxHz) {
    if (num <= 0 || den <= 0 || !(maxHz > 0.0))
        return false;
    const double v = static_cast<double>(num) / static_cast<double>(den);
    if (v <= maxHz + 0.010)
        return false;
    if (num == 60 && den == 1 && maxHz >= 30.0 - 0.010) {
        num = 30;
        den = 1;
        return true;
    }
    if (num == 60000 && den == 1001 && maxHz >= 30000.0 / 1001.0 - 0.010) {
        num = 30000;
        den = 1001;
        return true;
    }
    if (num == 50 && den == 1 && maxHz >= 25.0 - 0.010) {
        num = 25;
        den = 1;
        return true;
    }
    if (num == 48 && den == 1 && maxHz >= 24.0 - 0.010) {
        num = 24;
        den = 1;
        return true;
    }
    num = static_cast<int>(maxHz);
    den = 1;
    if (num < 1)
        num = 1;
    return true;
}

bool applyContentFpsConf(const std::string& conf, int& num, int& den) {
    const std::string s = lowerTrim(conf);
    if (s.empty() || s == "auto")
        return false;
    if (s == "off" || s == "0" || s == "none") {
        num = 0;
        den = 0;
        return true;
    }
    // "24000/1001" rational form
    const auto slash = s.find('/');
    if (slash != std::string::npos) {
        char* e1 = nullptr;
        char* e2 = nullptr;
        const std::string ns = s.substr(0, slash);
        const std::string ds = s.substr(slash + 1);
        long n = std::strtol(ns.c_str(), &e1, 10);
        long d = std::strtol(ds.c_str(), &e2, 10);
        if (e1 != ns.c_str() && e2 != ds.c_str() && n > 0 && d > 0) {
            num = static_cast<int>(n);
            den = static_cast<int>(d);
            return true;
        }
        return false;
    }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && v > 0.0)
        return snapStdFps(v, num, den);
    return false;
}

} // namespace misterplex
