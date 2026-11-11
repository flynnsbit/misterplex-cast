#pragma once
#include "airplay/ns.hpp"
// YouTube livestream / in-progress premiere (YT_LIVE).
//
// The VOD selector (yt_dlp::kResolveFormat) requires protocol=https. A livestream
// has no progressive file; yt-dlp's playable is an HLS manifest. ARM libav in
// this daemon demuxes mpegts, not hls, so the player must not open that URL.
// When YT_LIVE is on, the same -g ladder gets m3u8 fallbacks after the https
// selectors. A manifest URL is copy-remuxed by the on-card helper ffmpeg
// (hls demuxer + mpegts muxer) onto 127.0.0.1. Unset / off does not change
// the VOD argv or the play URL.
//
// An upcoming premiere has no manifest: resolve fails and the cast ends.
// A premiere that has started is the same HLS shape as a livestream.
#include "gcast/yt_dlp.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace misterplex {
namespace airplay {
namespace yt_live {

// Live HLS audio (itags 233/234 on ISS/ABC/Sky) reports acodec=unknown.
// acodec^=mp4a makes yt-dlp return no format, so the daemon never remuxes.
// Height, avc, fps, and m3u8 stay. The VOD selector is kResolveFormat and
// still requires acodec^=mp4a. MUT_YT_LIVE_REQUIRE_MP4A puts that filter back.
#ifdef MUT_YT_LIVE_REQUIRE_MP4A
inline constexpr char kLiveHlsFormat[] =
    "b[height<=720][vcodec^=avc][acodec^=mp4a][fps<=30][protocol^=m3u8]"
    "/bv*[height<=720][vcodec^=avc][fps<=30][protocol^=m3u8]+ba[acodec^=mp4a][protocol^=m3u8]";
#else
inline constexpr char kLiveHlsFormat[] =
    "b[height<=720][vcodec^=avc][fps<=30][protocol^=m3u8]"
    "/bv*[height<=720][vcodec^=avc][fps<=30][protocol^=m3u8]+ba[protocol^=m3u8]";
#endif

// Phone lounge duration when the player has none. The player itself stays at
// duration 0 so it does not end the cast at this mark. 48 h covers a live
// MPEG-TS PTS that started away from zero. A real duration is left alone.
inline constexpr int64_t kLiveLoungeDurationMs = 48LL * 3600 * 1000;
inline constexpr int kLiveFirstByteMs = 12000;

inline bool ytLiveEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n) {
        unsigned char c = static_cast<unsigned char>(e[n]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        buf[n] = static_cast<char>(c);
    }
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// YT_LIVE (misterplex.conf) / MPX_YT_LIVE (env wins). Default off: the VOD
// selector and play URL stay on the tip path.
inline bool ytLiveEnabled() {
#ifdef MUT_YT_LIVE_IGNORED
    return false;
#else
    if (const char* e = std::getenv("MPX_YT_LIVE"); e && e[0])
        return ytLiveEnvOn(e);
    return ytLiveEnvOn(std::getenv("YT_LIVE"));
#endif
}

// https selectors first, then HLS. yt-dlp '/' is fallback: a VOD hit still
// matches kResolveFormat and never reaches the m3u8 selectors.
inline std::string liveEnabledFormat() {
    return std::string(yt_dlp::kResolveFormat) + "/" + kLiveHlsFormat;
}

inline bool plainHttpUrl(const std::string& u) {
    if (!(u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) || u.size() <= 8 || u.size() > 4096)
        return false;
    for (unsigned char c : u)
        if (c <= ' ' || c == 0x7f)
            return false;
    return true;
}

inline std::string asciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// YouTube live manifests are hls_variant / hls_playlist URLs, not always .m3u8.
inline bool hlsManifestUrl(const std::string& url) {
    if (!plainHttpUrl(url))
        return false;
    const std::string l = asciiLower(url);
    return l.find(".m3u8") != std::string::npos || l.find("/hls_variant/") != std::string::npos ||
           l.find("/hls_playlist/") != std::string::npos ||
           l.find("mime=application%2fx-mpegurl") != std::string::npos ||
           l.find("mime=application/x-mpegurl") != std::string::npos ||
           l.find("playlist_type=live") != std::string::npos;
}

// Live edge. A lounge currentTime is seconds-into-the-broadcast, not a file offset.
inline int64_t livePlayStartMs(bool live, int64_t loungeMs) {
#ifdef MUT_YT_LIVE_KEEP_OFFSET
    (void)live;
    return loungeMs;
#else
    if (live)
        return 0;
    return loungeMs;
#endif
}

// The player opens the loopback MPEG-TS, never the manifest.
inline std::string livePlayable(bool remux, const std::string& direct, const std::string& relay) {
#ifdef MUT_YT_LIVE_PLAY_DIRECT
    (void)remux;
    (void)relay;
    return direct;
#else
    if (remux && !relay.empty())
        return relay;
    return direct;
#endif
}

inline int64_t liveLoungeDurationMs(bool live, int64_t reported) {
#ifdef MUT_YT_LIVE_ZERO_LOUNGE_DUR
    (void)live;
    return reported;
#else
    if (live && reported <= 0)
        return kLiveLoungeDurationMs;
    return reported;
#endif
}

inline bool fileExecutable(const std::string& path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

// MPX_YT_LIVE_FFMPEG wins. Else the on-card helper binary (hls demuxer + mpegts muxer).
inline std::string findLiveFfmpeg() {
    if (const char* e = std::getenv("MPX_YT_LIVE_FFMPEG"); e && e[0])
        return fileExecutable(e) ? std::string(e) : std::string();
    if (const char* e = std::getenv("YT_LIVE_FFMPEG"); e && e[0])
        return fileExecutable(e) ? std::string(e) : std::string();
    const char* cands[] = {"/media/fat/misterplex/bin/ffmpeg", "/media/fat/linux/misterplex/bin/ffmpeg", nullptr};
    for (int i = 0; cands[i]; ++i)
        if (fileExecutable(cands[i]))
            return cands[i];
    return {};
}

struct LiveSource {
    std::string ffmpeg;
    std::string videoUrl;
    std::string audioUrl;
};

// Same whitelist the on-card helper uses for an http(s) input.
inline constexpr char kFfmpegNetProto[] = "http,https,tcp,tls,crypto";

inline std::vector<std::string> liveFfmpegArgs(const LiveSource& src) {
    std::vector<std::string> a;
    a.push_back(src.ffmpeg);
    a.push_back("-nostdin");
    a.push_back("-hide_banner");
    a.push_back("-loglevel");
    a.push_back("error");
    a.push_back("-protocol_whitelist");
    a.push_back(kFfmpegNetProto);
    a.push_back("-i");
    a.push_back(src.videoUrl);
    const bool split = !src.audioUrl.empty() && src.audioUrl != src.videoUrl;
    if (split) {
        a.push_back("-protocol_whitelist");
        a.push_back(kFfmpegNetProto);
        a.push_back("-i");
        a.push_back(src.audioUrl);
    }
    a.push_back("-map");
    a.push_back("0:v:0");
    a.push_back("-map");
    a.push_back(split ? "1:a:0?" : "0:a:0?");
    a.push_back("-sn");
    a.push_back("-dn");
    a.push_back("-c");
    a.push_back("copy");
    a.push_back("-bsf:v");
    a.push_back("h264_mp4toannexb");
    a.push_back("-f");
    a.push_back("mpegts");
    a.push_back("-muxdelay");
    a.push_back("0");
    a.push_back("pipe:1");
    return a;
}

inline bool writeAll(int fd, const char* p, size_t n) {
    while (n) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// One loopback MPEG-TS. The manifest stays in this process; the player URL
// does not carry it. One ffmpeg at a time. stop() kills the process group.
class LiveRelay {
public:
    std::string start(const LiveSource& src, std::string& err) {
        stop();
        err.clear();
        if (!hlsManifestUrl(src.videoUrl)) {
            err = "not an hls manifest";
            return {};
        }
        if (!src.audioUrl.empty() && !plainHttpUrl(src.audioUrl)) {
            err = "bad audio url";
            return {};
        }
        if (!fileExecutable(src.ffmpeg)) {
            err = "ffmpeg not executable";
            return {};
        }
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            err = "socket failed";
            return {};
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 8) != 0) {
            ::close(fd);
            err = "bind failed";
            return {};
        }
        sockaddr_in bound{};
        socklen_t bl = sizeof bound;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bl) != 0) {
            ::close(fd);
            err = "getsockname failed";
            return {};
        }
        const int port = ntohs(bound.sin_port);
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_.store(false);
            listenFd_ = fd;
            src_ = src;
        }
        thread_ = std::thread([this] { loop(); });
        return "http://127.0.0.1:" + std::to_string(port) + "/yt-live.ts";
    }

    void stop() {
        std::thread joining;
        int listenFd = -1;
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_.store(true);
            listenFd = listenFd_;
            listenFd_ = -1;
            pid = ffmpegPid_.exchange(-1);
            if (thread_.joinable())
                joining = std::move(thread_);
        }
        if (listenFd >= 0) {
            ::shutdown(listenFd, SHUT_RDWR);
            ::close(listenFd);
        }
        if (pid > 0)
            ::killpg(pid, SIGKILL);
        if (joining.joinable())
            joining.join();
        pid = ffmpegPid_.exchange(-1);
        if (pid > 0)
            ::killpg(pid, SIGKILL);
    }

    ~LiveRelay() { stop(); }

private:
    void loop() {
        for (;;) {
            int listenFd = -1;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_.load())
                    return;
                listenFd = listenFd_;
            }
            if (listenFd < 0)
                return;
            pollfd pf{};
            pf.fd = listenFd;
            pf.events = POLLIN;
            const int pr = ::poll(&pf, 1, 200);
            if (pr <= 0)
                continue;
            const int client = ::accept(listenFd, nullptr, nullptr);
            if (client < 0)
                continue;
            serve(client);
            ::close(client);
        }
    }

    void serve(int client) {
        timeval snd{};
        snd.tv_sec = 1;
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192 && !stop_.load()) {
            pollfd pf{};
            pf.fd = client;
            pf.events = POLLIN;
            if (::poll(&pf, 1, 200) <= 0)
                continue;
            const ssize_t n = ::recv(client, buf, sizeof buf, 0);
            if (n <= 0)
                return;
            req.append(buf, buf + n);
        }
        if (req.compare(0, 4, "GET ") != 0) {
            static const char k[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(client, k, sizeof k - 1);
            return;
        }
        const auto sp = req.find(' ', 4);
        std::string path = sp == std::string::npos ? std::string() : req.substr(4, sp - 4);
        const auto q = path.find('?');
        if (q != std::string::npos)
            path.resize(q);
        if (path != "/yt-live.ts") {
            static const char k[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(client, k, sizeof k - 1);
            return;
        }
        LiveSource src;
        {
            std::lock_guard<std::mutex> lk(mu_);
            src = src_;
        }
        int outPipe[2] = {-1, -1};
        if (::pipe(outPipe) != 0)
            return;
        const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        // Build argv before fork. The child of a multithreaded process must not allocate.
        const std::vector<std::string> store = liveFfmpegArgs(src);
        std::vector<char*> argv;
        argv.reserve(store.size() + 1);
        for (const auto& s : store)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            if (devnull >= 0)
                ::close(devnull);
            return;
        }
        if (pid == 0) {
            ::setpgid(0, 0);
            ::dup2(outPipe[1], STDOUT_FILENO);
            if (devnull >= 0)
                ::dup2(devnull, STDERR_FILENO);
            for (int fd = 3; fd < 256; ++fd)
                ::close(fd);
            ::execv(src.ffmpeg.c_str(), argv.data());
            _exit(127);
        }
        ::setpgid(pid, pid);
        ::close(outPipe[1]);
        if (devnull >= 0)
            ::close(devnull);
        ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
        ffmpegPid_.store(pid);
        auto reap = [&](bool kill) {
            if (kill && pid > 0)
                ::killpg(pid, SIGKILL);
            if (pid > 0) {
                int status = 0;
                ::waitpid(pid, &status, 0);
            }
            ::close(outPipe[0]);
            ffmpegPid_.store(-1);
            pid = -1;
        };
        if (stop_.load()) {
            reap(true);
            return;
        }
        std::string first;
        const auto t0 = std::chrono::steady_clock::now();
        bool got = false;
        while (!stop_.load()) {
            pollfd pf{};
            pf.fd = outPipe[0];
            pf.events = POLLIN;
            const int pr = ::poll(&pf, 1, 100);
            if (pr > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR))) {
                char b[4096];
                const ssize_t n = ::read(outPipe[0], b, sizeof b);
                if (n > 0) {
                    first.append(b, b + n);
                    got = true;
                    break;
                }
                if (n == 0)
                    break;
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (ms >= kLiveFirstByteMs)
                break;
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                ffmpegPid_.store(-1);
                break;
            }
        }
        if (!got) {
            static const char k[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(client, k, sizeof k - 1);
            reap(true);
            return;
        }
        static const char kHdr[] =
            "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        if (!writeAll(client, kHdr, sizeof kHdr - 1) || !writeAll(client, first.data(), first.size())) {
            reap(true);
            return;
        }
        while (!stop_.load()) {
            pollfd pf{};
            pf.fd = outPipe[0];
            pf.events = POLLIN;
            const int pr = ::poll(&pf, 1, 200);
            if (pr > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR))) {
                char b[8192];
                const ssize_t n = ::read(outPipe[0], b, sizeof b);
                if (n > 0) {
                    if (!writeAll(client, b, static_cast<size_t>(n)))
                        break;
                    continue;
                }
                if (n == 0)
                    break;
            }
            int status = 0;
            if (pid > 0 && ::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                ffmpegPid_.store(-1);
                break;
            }
        }
        reap(true);
    }

    std::mutex mu_;
    std::atomic<bool> stop_{true};
    std::atomic<pid_t> ffmpegPid_{-1};
    int listenFd_ = -1;
    LiveSource src_;
    std::thread thread_;
};

inline LiveRelay& liveRelay() {
    static LiveRelay r;
    return r;
}

}  // namespace yt_live
}  // namespace airplay
}  // namespace misterplex
