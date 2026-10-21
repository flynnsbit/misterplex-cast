#pragma once
// GCAST P1: cast_resolve -- turn a sender URL (AirPlay Content-Location, YouTube itag URL) into
// something the static ARM libav + MiSTer decoder can open.
//   Direct : plain http:// file the libav build demuxes itself (mov/mp4/m4v, mkv, ts) when the
//            caller allows it (GCAST_DIRECT=1; the codec is the sender's, so default off).
//   Helper : a gcast-xcode helper (GCAST_XCODE=http://host:port) pulls the URL with host
//            FFmpeg (https, HLS, HEVC, any rate) and streams H.264/AAC MPEG-TS at the play's
//            raster. Its path is /ladder.ts with offset_ms=, the same contract as the lab ladder
//            proxy, so MediaPlayer::seekMs re-requests at the new offset (live TS is not seekable).
//   Refuse : anything else (rtsp:, mlhls: without FCUP, no helper configured for https/HLS).
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace misterplex {
namespace gcast {

enum class Route : uint8_t { Refuse = 0, Direct = 1, Helper = 2 };

struct CastPlan {
    Route route = Route::Refuse;
    std::string playable;
    std::string why;
};

inline std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

inline bool startsWithI(const std::string& s, const char* p) {
    const std::string ls = lowerAscii(s.substr(0, std::char_traits<char>::length(p)));
    return ls == p;
}

// Path extension of a URL, lower case, without query/fragment ("" when none).
inline std::string urlPathExt(const std::string& url) {
    auto end = url.find_first_of("?#");
    std::string path = url.substr(0, end);
    auto scheme = path.find("://");
    if (scheme != std::string::npos) {
        auto slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? std::string() : path.substr(slash);
    }
    auto dot = path.rfind('.');
    auto lastSlash = path.rfind('/');
    if (dot == std::string::npos || (lastSlash != std::string::npos && dot < lastSlash))
        return {};
    return lowerAscii(path.substr(dot + 1));
}

inline bool libavDirectExt(const std::string& ext) {
    return ext == "mp4" || ext == "m4v" || ext == "mov" || ext == "mkv" || ext == "ts";
}

// RFC 3986 unreserved stay; everything else %XX (the helper parses with parse_qs).
inline std::string encodeComponent(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            o.push_back(static_cast<char>(c));
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

inline std::string helperPlayable(std::string base, const std::string& src, int w, int h, int brKbps,
                                  int64_t startMs) {
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    std::string u = base + "/ladder.ts?src=" + encodeComponent(src) + "&w=" + std::to_string(w) +
                    "&h=" + std::to_string(h) + "&br=" + std::to_string(brKbps) + "&gcast=1";
#ifndef MUT_CR_DROP_OFFSET
    if (startMs > 0)
        u += "&offset_ms=" + std::to_string(startMs);
#endif
    return u;
}

// YouTube casts arrive as "yt:<11-char videoId>" (YtFrontend). Resolution (yt-dlp) lives in
// gcast-xcode (remote or local standalone); yt: needs a non-empty helperBase.
inline bool ytCastId(const std::string& url, std::string& id) {
    if (url.size() != 14 || url.compare(0, 3, "yt:") != 0)
        return false;
    for (size_t i = 3; i < url.size(); ++i) {
        const char c = url[i];
#ifndef MUT_CR_YT_ANYID
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
#else
        (void)c;
#endif
    }
    id = url.substr(3);
    return true;
}

// FPGA backend: the ladder proxy needs an http source, so hand it gcast-xcode's yt.ts remux,
// already started at startMs (the ladder then runs with offset 0).
inline std::string ytRemuxSource(std::string xcodeBase, const std::string& id, int64_t startMs) {
    while (!xcodeBase.empty() && xcodeBase.back() == '/')
        xcodeBase.pop_back();
    return xcodeBase + "/yt.ts?v=" + id + "&start_ms=" + std::to_string(startMs > 0 ? startMs : 0);
}

// The AirPlay front end's live RAOP PCM stream (airplay_fe audioUrl): http://host:port/raop.wav
inline bool raopPcmSource(const std::string& url) {
    static const char kTail[] = "/raop.wav";
    const size_t n = sizeof kTail - 1;
#ifndef MUT_CR_RAOP_ANYWAV
    return startsWithI(url, "http://") && url.size() > n && url.compare(url.size() - n, n, kTail) == 0;
#else
    return url.size() > 4 && url.compare(url.size() - 4, 4, ".wav") == 0;
#endif
}

// GYT standalone: where a yt:<id> plays. xcode = effective gcast-xcode (healthy remote or a
// loopback helper that answers /health); its /yt.ts is a yt-dlp pipe copy-remux (avc1 <=720p30 + m4a, CABAC).
//   OB / owenb (!fpga): Direct on the remux -- the OB decoder takes CABAC natively, no re-encode.
//   legacy FPGA + ladder: ladder proxy transcodes the remux (Baseline/CAVLC raster), as before.
//   legacy FPGA, no ladder: refuse (the CAVLC fabric cannot take YouTube's CABAC).
inline CastPlan planYouTube(const std::string& id, bool fpga, const std::string& ladder, const std::string& xcode,
                            int w, int h, int brKbps, int64_t startMs, bool localYtdlp = false) {
    CastPlan p;
    if (xcode.empty()) {
#ifndef MUT_CR_NO_LOCAL_YT
        // OB plays the resolved progressive URL directly. localYtdlp is true only when an
        // executable resolver is on the card (see yt_dlp.hpp). FPGA still needs the ladder.
        if (localYtdlp && !fpga) {
            p.route = Route::Direct;
            p.playable = "yt-local:" + id;
            p.why = "local yt-dlp direct (H.264/AAC <=720p)";
            return p;
        }
#endif
        p.why = "youtube needs gcast-xcode or local /media/fat/misterplex yt-dlp";
        return p;
    }
    const std::string remux = ytRemuxSource(xcode, id, startMs);
#ifndef MUT_CR_YT_OB_LADDER
    if (!fpga) {
        p.route = Route::Direct;
        p.playable = remux;
        p.why = "yt copy-remux direct (OB CABAC)";
        return p;
    }
#endif
    if (!ladder.empty()) {
        p.route = Route::Helper;
        p.playable = helperPlayable(ladder, remux, w, h, brKbps, 0);
        p.why = "yt remux via ladder";
        return p;
    }
#ifdef MUT_CR_YT_OB_LADDER
    if (!fpga) {
        p.route = Route::Helper;
        p.playable = helperPlayable(xcode, "yt:" + id, w, h, brKbps, startMs);
        p.why = "mutant";
        return p;
    }
#endif
    p.why = "youtube on legacy FPGA core needs MPX_LADDER_URL (CABAC source)";
    return p;
}

inline CastPlan planCast(const std::string& url, bool directAllowed, const std::string& helperBase, int w,
                         int h, int brKbps, int64_t startMs) {
    CastPlan p;
    if (url.compare(0, 3, "yt:") == 0) {
        std::string id;
        if (!ytCastId(url, id)) {
            p.why = "bad youtube video id";
            return p;
        }
        if (helperBase.empty()) {
            p.why = "youtube needs gcast-xcode or local /media/fat/misterplex yt-dlp";
            return p;
        }
        p.route = Route::Helper;
        p.playable = helperPlayable(helperBase, url, w, h, brKbps, startMs);
        p.why = "gcast-xcode youtube";
        return p;
    }
    const bool http = startsWithI(url, "http://");
    const bool https = startsWithI(url, "https://");
    if (!http && !https) {
        p.why = "scheme not http(s)";
        return p;
    }
    if (url.find_first_of(" \r\n\t") != std::string::npos) {
        p.why = "url has whitespace/CRLF";
        return p;
    }
#ifdef MUT_CR_DIRECT_HTTPS
    const bool directOk = directAllowed && libavDirectExt(urlPathExt(url));
#else
    const bool directOk = directAllowed && http && libavDirectExt(urlPathExt(url));
#endif
    if (directOk) {
        p.route = Route::Direct;
        p.playable = url;
        p.why = "direct http " + urlPathExt(url);
        return p;
    }
    if (helperBase.empty()) {
        p.why = std::string("needs gcast-xcode (GCAST_XCODE unset) for ") + (https ? "https" : "http");
        return p;
    }
    p.route = Route::Helper;
    p.playable = helperPlayable(helperBase, url, w, h, brKbps, startMs);
    p.why = "gcast-xcode";
    return p;
}

// The castPlay planner. One function so a unit test fails if any of these routes is dropped:
//   AirPlay PCM (/raop.wav) on FPGA: xcode transcode (the ladder needs a picture on the live PCM).
//   YouTube on OB: planYouTube direct copy-remux (libav plays the CABAC yt.ts).
//   YouTube on FPGA: planYouTube via the ladder, including a loopback helper that answers /health.
struct GcastRouteInput {
    std::string url;
    bool fpga = false;
    std::string ladder;
    std::string xcode;
    std::string helper;
    int w = 0;
    int h = 0;
    int brKbps = 0;
    int64_t startMs = 0;
    bool directWanted = false;
    bool localYtdlp = false;   // executable /media/fat/misterplex yt-dlp, xcode unset
};

inline CastPlan planGcastPlay(const GcastRouteInput& in) {
    std::string ytId;
    const bool ytSrc = ytCastId(in.url, ytId);
    std::string planSrc = in.url;
    if (!ytSrc && raopPcmSource(in.url) && in.fpga && !in.xcode.empty())
        planSrc = helperPlayable(in.xcode, in.url, in.w, in.h, in.brKbps, 0);
    if (ytSrc)
        return planYouTube(ytId, in.fpga, in.ladder, in.xcode, in.w, in.h, in.brKbps, in.startMs,
                           in.localYtdlp);
    return planCast(planSrc, in.directWanted && !in.fpga, in.helper, in.w, in.h, in.brKbps,
                    planSrc == in.url ? in.startMs : 0);
}

inline constexpr const char* kLoopbackHelperBase = "http://127.0.0.1:18781";

// True only when the HTTP status line is 200. A 500 body or a header that
// happens to contain "200" is not healthy.
inline bool httpStatusLineIs200(const char* buf) {
    if (!buf || std::strncmp(buf, "HTTP/", 5) != 0)
        return false;
    const char* nl = std::strchr(buf, '\n');
    const char* sp = std::strchr(buf, ' ');
    if (!sp || (nl && sp >= nl))
        return false;
    ++sp;
    while (*sp == ' ' || *sp == '\t')
        ++sp;
    if (nl && sp >= nl)
        return false;
    if (std::strncmp(sp, "200", 3) != 0)
        return false;
    const char after = sp[3];
    if (nl && sp + 3 > nl)
        return false;
    return after == ' ' || after == '\r' || after == '\n' || after == '\0';
}

// GET one path with a short connect+read budget. A missing helper, or any
// status other than 200, is not healthy. bodyNeedle, when set, must appear
// in the first read (the /probe self-test looks for audio=1).
inline bool probeHttpHealth(const std::string& base, int timeoutMs, const char* path = "/health",
                            const char* bodyNeedle = nullptr) {
    if (timeoutMs < 1)
        timeoutMs = 1;
    if (base.compare(0, 7, "http://") != 0)
        return false;
    std::string rest = base.substr(7);
    const auto slash = rest.find('/');
    if (slash != std::string::npos)
        rest.erase(slash);
    std::string host = rest;
    int port = 80;
    const auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = std::atoi(rest.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0 || port > 65535)
        return false;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, timeoutMs) <= 0) {
        ::close(fd);
        return false;
    }
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
        ::close(fd);
        return false;
    }
    const std::string req = std::string("GET ") + (path ? path : "/health") +
                            " HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
        ::close(fd);
        return false;
    }
    // One deadline for the whole reply. The helper can write the headers and
    // the body as two sends; a single recv under load often sees only the
    // status line and misses the needle (audio=1).
    char buf[1024];
    size_t len = 0;
    buf[0] = 0;
    pfd.events = POLLIN;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
#ifdef MUT_CR_ONE_RECV
    if (::poll(&pfd, 1, timeoutMs) > 0) {
        const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
        if (n > 0) {
            len = static_cast<size_t>(n);
            buf[len] = 0;
        }
    }
#else
    while (len + 1 < sizeof buf) {
        if (bodyNeedle && std::strstr(buf, bodyNeedle) != nullptr)
            break;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0)
            break;
        const int pr = ::poll(&pfd, 1, static_cast<int>(left));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            break;
        const ssize_t n = ::recv(fd, buf + len, sizeof buf - 1 - len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        len += static_cast<size_t>(n);
        buf[len] = 0;
    }
#endif
    ::close(fd);
    if (len == 0 || !httpStatusLineIs200(buf))
        return false;
    if (bodyNeedle && std::strstr(buf, bodyNeedle) == nullptr)
        return false;
    return true;
}

// Cached for a few seconds so a down helper is not dialed on every cast.
inline bool cachedLoopbackHelperHealth() {
    static std::mutex mu;
    static std::chrono::steady_clock::time_point at{};
    static bool ok = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (at.time_since_epoch().count() != 0 && now - at < std::chrono::seconds(5))
            return ok;
    }
    const bool got = probeHttpHealth(kLoopbackHelperBase, 400);
    std::lock_guard<std::mutex> lock(mu);
    at = now;
    ok = got;
    return ok;
}

// /probe?selftest=1 is a local ffprobe of a generated WAV. AirPlay stays off
// the ready card until that returns 200 with audio=1 (ffprobe is present).
inline bool cachedHelperProbeSelfTest(const std::string& base) {
    static std::mutex mu;
    static std::string cachedBase;
    static std::chrono::steady_clock::time_point at{};
    static bool ok = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (cachedBase == base && at.time_since_epoch().count() != 0 && now - at < std::chrono::seconds(5))
            return ok;
    }
    const bool got = probeHttpHealth(base, 1500, "/probe?selftest=1", "audio=1");
    std::lock_guard<std::mutex> lock(mu);
    cachedBase = base;
    at = now;
    ok = got;
    return ok;
}

inline std::string standaloneLocalBase(bool helperHealthOk = false) {
#ifdef MUT_CR_NO_STANDALONE
    (void)helperHealthOk;
    return {};
#else
    if (const char* v = std::getenv("MPX_GCAST_LOCAL"); v && v[0])
        return std::string(v);
    if (helperHealthOk)
        return kLoopbackHelperBase;
    return {};
#endif
}

inline std::string helperBaseFromEnv() {
    const char* v = std::getenv("GCAST_XCODE");
    return (v && v[0]) ? std::string(v) : std::string();
}

// YouTube follows a healthy remote GCAST_XCODE. The on-card loopback helper
// is for AirPlay. When local yt-dlp is present and the remote helper is not,
// YouTube stays on the gated local path (empty base -> yt-local:).
inline std::string youtubeXcodeBase(bool remoteHealthy, bool localYtdlp, const std::string& airplayBase) {
    if (!remoteHealthy && localYtdlp)
        return {};
    return airplayBase;
}

inline std::string effectiveXcodeBase(bool remoteHealthy, bool localHelperHealthy = false) {
    const std::string remote = helperBaseFromEnv();
    if (!remote.empty() && remoteHealthy)
        return remote;
    const std::string local = standaloneLocalBase(localHelperHealthy);
    if (!local.empty())
        return local;
    return remote;
}

// gcast-xcode GET /probe?src= answers "key=value" lines: w h dur_ms fps_n fps_d audio vcodec.
struct ProbeInfo {
    int w = 0, h = 0, fpsN = 0, fpsD = 0;
    int64_t durMs = 0;
    bool audio = false, ok = false;
    std::string vcodec;
    std::string title;   // gcast-xcode yt: probes only; printable ASCII/UTF-8, <= 96 bytes
};

inline ProbeInfo parseProbe(const std::string& body) {
    ProbeInfo p;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos)
            nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const long long n = std::strtoll(v.c_str(), nullptr, 10);
        if (k == "w") p.w = n > 0 && n < 16384 ? static_cast<int>(n) : 0;
        else if (k == "h") p.h = n > 0 && n < 16384 ? static_cast<int>(n) : 0;
        else if (k == "dur_ms") p.durMs = n > 0 ? n : 0;
        else if (k == "fps_n") p.fpsN = n > 0 && n < 1000000 ? static_cast<int>(n) : 0;
        else if (k == "fps_d") p.fpsD = n > 0 && n < 1000000 ? static_cast<int>(n) : 0;
        else if (k == "audio") p.audio = n != 0;
        else if (k == "vcodec") p.vcodec = v.substr(0, 32);
        else if (k == "title") {
            p.title.clear();
            for (unsigned char c : v)
                if (c >= 0x20 && c != 0x7f && p.title.size() < 96)
                    p.title.push_back(static_cast<char>(c));
        }
    }
    p.ok = p.w > 0 && p.h > 0;
    return p;
}

}  // namespace gcast
}  // namespace misterplex

namespace misterplex {
namespace gcast {
// Same env rule as plex_resolve's (anonymous) ladderProxyUrl(): the FPGA H.264 path's helper.
inline std::string fpgaLadderProxyFromEnv() {
    // MPX_LADDER_URL (preferred), MPX_REMOTE_LADDER_PROXY, REMOTE_LADDER_PROXY.
    // No hardcoded lab IP; empty/unset means no ladder (standalone).
    if (const char* v = std::getenv("MPX_LADDER_URL"))
        return v[0] ? std::string(v) : std::string();
    if (const char* mpx = std::getenv("MPX_REMOTE_LADDER_PROXY"))
        return mpx[0] ? std::string(mpx) : std::string();
    const char* proxy = std::getenv("REMOTE_LADDER_PROXY");
    return (proxy && proxy[0]) ? std::string(proxy) : std::string();
}
// "1280x720" -> 1280, 720 (false on anything else)
inline bool parseWxH(const std::string& s, int& w, int& h) {
    const auto x = s.find('x');
    if (x == std::string::npos)
        return false;
    w = std::atoi(s.substr(0, x).c_str());
    h = std::atoi(s.substr(x + 1).c_str());
    return w > 0 && h > 0 && w <= 4096 && h <= 4096;
}
// A title for the OSD from the URL's last path segment ("AirPlay" when none).
inline std::string titleFromUrl(const std::string& url, const char* fallback) {
    auto end = url.find_first_of("?#");
    std::string path = url.substr(0, end);
    auto slash = path.rfind('/');
    std::string seg = slash == std::string::npos ? std::string() : path.substr(slash + 1);
    if (seg.empty() || seg.size() > 80 || seg == "master.m3u8" || seg == "index.m3u8")
        return fallback;
    return seg;
}
}  // namespace gcast
}  // namespace misterplex
