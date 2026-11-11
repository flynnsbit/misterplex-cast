#pragma once
// #274: ffmpeg has no -K for headers, so a Plex token on `-headers` is visible
// in ps. This loopback relay takes a token-free URL
// `http://127.0.0.1:<port>/<nonce>` and forwards to the allowlisted upstream
// with X-Plex-Token (and the rest of the identity block) injected.
// Bind is 127.0.0.1 only. MUT_274_HEADERS_ARGV restores argv `-headers`.

#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mpx {

inline bool headersCarryPlexToken(const std::string& h) {
    std::string s = h;
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s.find("x-plex-token:") != std::string::npos;
}

inline void ffmpegAsciiLowerInPlace(std::string& s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline std::string ffmpegPctDecodeOnce(const std::string& s) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

inline std::string ffmpegPctDecodeStable(const std::string& s) {
    std::string decoded = s;
    for (int round = 0; round < 8; ++round) {
        std::string next = ffmpegPctDecodeOnce(decoded);
        if (next == decoded)
            break;
        decoded = std::move(next);
    }
    return decoded;
}

// #630 GBE P3-2: match on the raw slot AND on its percent-decoded form,
// decoded until stable (X-Plex%2DToken, %58-Plex-Token, x%252Dplex%252Dtoken,
// X%2DMPX%2DAuth). The bytes passed to exec are never changed.
inline bool textCarriesLaunchSecret(const std::string& raw) {
    const auto matches = [](std::string s) {
        ffmpegAsciiLowerInPlace(s);
        if (s.find("x-plex-token") != std::string::npos)
            return true;
        if (s.find("x-mpx-auth") != std::string::npos)
            return true;
        return false;
    };
    std::string decoded = ffmpegPctDecodeStable(raw);
    return matches(raw) || matches(decoded);
}

inline bool ffmpegArgvCarriesPlexToken(const std::vector<std::string>& a) {
    for (const auto& arg : a) {
        if (textCarriesLaunchSecret(arg))
            return true;
    }
    return false;
}

inline constexpr const char* kFfmpegArgvSecretRefuseMessage =
    "ffmpeg argv carries a secret; refusing to launch";

// Fail closed: write a token-free reason and return true when argv must not be exec'd.
inline bool ffmpegArgvRefuseIfSecret(const std::vector<std::string>& a,
                                     std::string* failLog = nullptr) {
    if (!ffmpegArgvCarriesPlexToken(a))
        return false;
    if (failLog)
        *failLog = kFfmpegArgvSecretRefuseMessage;
    return true;
}

using FfmpegLaunchStub = pid_t (*)(const std::vector<std::string>& argv);

inline FfmpegLaunchStub& ffmpegLaunchStub() {
    static FfmpegLaunchStub stub = nullptr;
    return stub;
}

// #630 GBE P3-3: the ONE ffmpeg launch path. The secret guard and fork are
// adjacent here, so no caller can add -headers between check and exec (G18).
// Tests install ffmpegLaunchStub() to see the exact argv that would be exec'd.
inline pid_t launchFfmpeg(const std::vector<std::string>& args,
                          const std::function<void()>& childSetup,
                          std::string* failLog = nullptr, bool* refused = nullptr) {
    if (refused)
        *refused = false;
    if (args.empty())
        return -1;
    if (ffmpegArgvRefuseIfSecret(args, failLog)) {
        if (refused)
            *refused = true;
        return -1;
    }
    if (ffmpegLaunchStub())
        return ffmpegLaunchStub()(args);
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (childSetup)
            childSetup();
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        ::execv(args[0].c_str(), argv.data());
        ::_exit(127);
    }
    return pid;
}

struct FfmpegHeaderRelay {
    FfmpegHeaderRelay() = default;
    FfmpegHeaderRelay(const FfmpegHeaderRelay&) = delete;
    FfmpegHeaderRelay& operator=(const FfmpegHeaderRelay&) = delete;
    ~FfmpegHeaderRelay() { stop(); }

    bool boundLoopback() const { return boundLoopback_; }
    uint16_t port() const { return port_; }
    std::string nonce() const { return nonce_; }

    // Rewrites url to the loopback nonce URL and clears headers on success.
    bool wrap(std::string& url, std::string& headers) {
#ifdef MUT_274_HEADERS_ARGV
        (void)url;
        (void)headers;
        return false;
#else
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            return false;
        if (!headersCarryPlexToken(headers))
            return false;
        if (!ensureListen())
            return false;
        std::lock_guard<std::mutex> g(mu_);
        upstream_ = url;
        inject_ = headers;
        if (inject_.size() < 2 || inject_.compare(inject_.size() - 2, 2, "\r\n") != 0)
            inject_ += "\r\n";
        url = "http://127.0.0.1:" + std::to_string(port_) + "/" + nonce_;
        headers.clear();
        return true;
#endif
    }

    void stop() {
        run_.store(false);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        if (thr_.joinable())
            thr_.join();
        port_ = 0;
        boundLoopback_ = false;
    }

private:
    bool ensureListen() {
        if (fd_ >= 0 && run_.load())
            return true;
        stop();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        sockaddr_in got{};
        socklen_t gl = sizeof(got);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&got), &gl) != 0 ||
            got.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        boundLoopback_ = true;
        port_ = ntohs(got.sin_port);
        if (::listen(fd_, 4) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        nonce_.clear();
        FILE* r = std::fopen("/dev/urandom", "rb");
        unsigned char b[8]{};
        if (r) {
            (void)std::fread(b, 1, sizeof(b), r);
            std::fclose(r);
        }
        char hex[17];
        for (int i = 0; i < 8; ++i)
            std::snprintf(hex + 2 * i, 3, "%02x", b[i]);
        nonce_.assign(hex, 16);
        run_.store(true);
        thr_ = std::thread([this] { loop(); });
        return true;
    }

    static int connectNumeric(const std::string& host, int port) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1)
            return -1;
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0)
            return -1;
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(s);
            return -1;
        }
        return s;
    }

    static bool parseHttpUrl(const std::string& url, std::string& host, int& port, std::string& path) {
        if (url.rfind("http://", 0) != 0)
            return false;
        std::string rest = url.substr(7);
        auto sl = rest.find('/');
        std::string hp = sl == std::string::npos ? rest : rest.substr(0, sl);
        path = sl == std::string::npos ? "/" : rest.substr(sl);
        auto c = hp.find(':');
        host = c == std::string::npos ? hp : hp.substr(0, c);
        port = 80;
        if (c != std::string::npos)
            port = std::atoi(hp.substr(c + 1).c_str());
        return !host.empty() && port > 0;
    }

    void loop() {
        while (run_.load()) {
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0)
                break;
            std::thread(&FfmpegHeaderRelay::serve, this, c).detach();
        }
    }

    void serve(int c) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            req.append(buf, static_cast<size_t>(n));
        }
        const auto sp1 = req.find(' ');
        const auto sp2 = req.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                               ? req.substr(sp1 + 1, sp2 - sp1 - 1)
                               : "";
        std::string want, inject, up;
        {
            std::lock_guard<std::mutex> g(mu_);
            want = "/" + nonce_;
            inject = inject_;
            up = upstream_;
        }
        if (path != want && path.find(want + "?") != 0) {
            const char* n404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(c, n404, std::strlen(n404), 0);
            ::close(c);
            return;
        }
        std::string host;
        int port = 80;
        std::string upath;
        if (!parseHttpUrl(up, host, port, upath)) {
            ::close(c);
            return;
        }
        std::string range;
        auto rpos = req.find("\r\nRange:");
        if (rpos == std::string::npos)
            rpos = req.find("\r\nrange:");
        if (rpos != std::string::npos) {
            auto e = req.find("\r\n", rpos + 2);
            if (e != std::string::npos)
                range = req.substr(rpos + 2, e - (rpos + 2));
        }
        int upfd = connectNumeric(host, port);
        if (upfd < 0) {
            const char* n502 = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(c, n502, std::strlen(n502), 0);
            ::close(c);
            return;
        }
        std::string fwd = "GET " + upath + " HTTP/1.1\r\nHost: " + host +
                          (port == 80 ? "" : ":" + std::to_string(port)) + "\r\n" + inject;
        if (!range.empty())
            fwd += range + "\r\n";
        fwd += "Connection: close\r\n\r\n";
        (void)::send(upfd, fwd.data(), fwd.size(), 0);
        char pipe[4096];
        for (;;) {
            const ssize_t n = ::recv(upfd, pipe, sizeof(pipe), 0);
            if (n <= 0)
                break;
            if (::send(c, pipe, static_cast<size_t>(n), 0) <= 0)
                break;
        }
        ::close(upfd);
        ::close(c);
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    bool boundLoopback_ = false;
    std::string nonce_;
    std::string upstream_;
    std::string inject_;
    std::mutex mu_;
    std::atomic<bool> run_{false};
    std::thread thr_;
};

inline FfmpegHeaderRelay& ffmpegHeaderRelay() {
    static FfmpegHeaderRelay r;
    return r;
}

inline bool ffmpegRelayWrap(std::string& url, std::string& headers) {
    return ffmpegHeaderRelay().wrap(url, headers);
}

inline bool ffmpegRelayPrepare(std::string& url, std::string& headers,
                               std::string* failLog = nullptr) {
    const bool wrapped = ffmpegRelayWrap(url, headers);
    if (!wrapped && headersCarryPlexToken(headers)) {
        if (failLog)
            *failLog = "ffmpeg relay bind failed; refusing to put a Plex token on argv";
        return false;
    }
    return true;
}

}  // namespace mpx
