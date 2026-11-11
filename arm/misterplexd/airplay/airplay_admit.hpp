#pragma once
#include "airplay/ns.hpp"
// AirPlay video admit. /play asks the helper GET /admit before it answers the phone.
// copy and audio start playback. Anything else is an HTTP error and a short on-screen notice.
// The video x264 ladder is not a successful admit.
#include "gcast/airplay_state.hpp"
#include "gcast/cast_resolve.hpp"

#include <string>

namespace misterplex {
namespace airplay {

// One display in GET /info. refreshRate in the plist is the frame period in seconds
// (1/kAirplayInfoFps), which Apple reads as kAirplayInfoFps Hz. maxFPS is the integer.
#ifndef MUT_AP_INFO_1080
inline constexpr int kAirplayInfoWidth = 1280;
inline constexpr int kAirplayInfoHeight = 720;
#else
inline constexpr int kAirplayInfoWidth = 1920;
inline constexpr int kAirplayInfoHeight = 1080;
#endif
inline constexpr int kAirplayInfoFps = 30;
// Stock UxPlay sends 14 (2|4|8) with a 1920x1080 display. 2 is the 720p flag alone.
inline constexpr int kAirplayInfoFeatures = 2;

inline constexpr const char* kNoticeResolution = "Video too high resolution for MiSTer (max 720p)";
// <= 20 chars so the 480i toast does not clip it (GBE #627 P3-2). Same text as the helper NOTICE_FRAMERATE.
inline constexpr const char* kNoticeFramerate = "Max 30 fps on MiSTer";
inline constexpr const char* kNoticeFormat = "format not supported";

struct AirplayAdmit {
    bool play = false;
    int status = 415;
    std::string notice = kNoticeFormat;
    std::string reason = "format";
};

inline int httpStatusCode(const std::string& raw) {
    if (raw.compare(0, 5, "HTTP/") != 0)
        return 0;
    const auto sp = raw.find(' ');
    if (sp == std::string::npos)
        return 0;
    return std::atoi(raw.c_str() + sp + 1);
}

inline std::string headerValue(const std::string& body, const char* key) {
    const std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        auto nl = body.find('\n', pos);
        if (nl == std::string::npos)
            nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0)
            return line.substr(prefix.size());
        pos = nl + 1;
    }
    return {};
}

// Only the three notice strings are shown. A 200 that is not "copy" or "audio"
// (the x264 mutant) does not play.
inline AirplayAdmit parseAdmitHttp(const std::string& raw) {
    AirplayAdmit a;
    const auto sep = raw.find("\r\n\r\n");
    const std::string body = sep == std::string::npos ? std::string() : raw.substr(sep + 4);
    const int status = httpStatusCode(raw);
    const bool copy = body.find("copy") != std::string::npos && body.find("refused") == std::string::npos;
    const bool audio = body.find("audio") != std::string::npos && body.find("refused") == std::string::npos;
    if (status == 200 && (copy || audio)) {
        a.play = true;
        a.status = 200;
        a.notice.clear();
        a.reason.clear();
        return a;
    }
    const std::string reason = headerValue(body, "reason");
    a.play = false;
    if (reason == "resolution") {
        a.reason = "resolution";
        a.notice = kNoticeResolution;
    } else if (reason == "framerate") {
        a.reason = "framerate";
        a.notice = kNoticeFramerate;
    } else {
        a.reason = "format";
        a.notice = kNoticeFormat;
    }
    if (status == 400)
        a.status = 400;
    else
        a.status = 415;
    return a;
}

inline ApEvent noticeEvent(const AirplayAdmit& a) {
    ApEvent e;
    e.type = ApEventType::Notice;
    e.url = a.notice.empty() ? std::string(kNoticeFormat) : a.notice;
    e.value = 0;
    return e;
}

// Bounded GET. False when the helper does not answer. raw is the status line plus body.
inline bool httpGetRaw(const std::string& base, const std::string& path, int timeoutMs, std::string& raw) {
    raw.clear();
    if (timeoutMs < 1)
        timeoutMs = 1;
    if (base.compare(0, 7, "http://") != 0 || path.empty() || path[0] != '/')
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
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n" + misterplex::lan_auth::helperWireLine() +
                            "Connection: close\r\n\r\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
        ::close(fd);
        return false;
    }
    std::string buf;
    buf.resize(8192);
    size_t len = 0;
    pfd.events = POLLIN;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (len + 1 < buf.size()) {
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
        const ssize_t n = ::recv(fd, &buf[len], buf.size() - 1 - len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        len += static_cast<size_t>(n);
    }
    ::close(fd);
    if (len == 0)
        return false;
    raw.assign(buf.data(), len);
    return true;
}

// Blocks the AirPlay httpd thread until the helper answers or the budget expires.
// /raop.wav is audio and is not refused. A missing helper is status 400, not a hang.
inline AirplayAdmit admitAirplayVideo(const std::string& url, int timeoutMs = 12000) {
    AirplayAdmit denied;
    denied.play = false;
    denied.status = 400;
    denied.reason = "format";
    denied.notice = kNoticeFormat;
    if (url.empty())
        return denied;
    if (url.size() >= 9 && url.compare(url.size() - 9, 9, "/raop.wav") == 0) {
        AirplayAdmit ok;
        ok.play = true;
        ok.status = 200;
        ok.notice.clear();
        ok.reason.clear();
        return ok;
    }
    // A location that is not http(s) (a refused twitterav:// host, GBE #627 P3-1) is not
    // sent to the helper: 415 and the format notice, so the TV is not silent.
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        denied.status = 415;
        return denied;
    }
    std::string base = helperBaseFromEnv();
    if (base.empty())
        base = kLoopbackHelperBase;
    std::string raw;
    if (!httpGetRaw(base, "/admit?src=" + encodeComponent(url), timeoutMs, raw))
        return denied;
    return parseAdmitHttp(raw);
}

}  // namespace airplay
}  // namespace misterplex
