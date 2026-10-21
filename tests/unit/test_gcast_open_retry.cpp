// YouTube open retry: a helper that answers empty (or 5xx) twice, then a TS
// body, must keep the play. Lounge state stays buffering until the body
// arrives. A newer generation cancels the wait. A seek that never produces
// bytes resumes at the previous position.
#include "gcast/open_retry.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

using misterplex::gcast::OpenClass;
using misterplex::gcast::RetryHit;
using misterplex::gcast::gcastOpenRetryable;
using misterplex::gcast::kGcastOpenBackoffMs;
using misterplex::gcast::retryOpens;
using misterplex::gcast::urlWithStartMs;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (c)                                                                                         \
            ++g_pass;                                                                                  \
        else {                                                                                         \
            ++g_fail;                                                                                  \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                    \
        }                                                                                              \
    } while (0)

static std::string httpGet(const std::string& host, int port, const std::string& path, int& status, std::string& err) {
    status = 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = "socket";
        return {};
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        err = "connect";
        ::close(fd);
        return {};
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) < 0) {
        err = "send";
        ::close(fd);
        return {};
    }
    std::string raw;
    char buf[1024];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        raw.append(buf, buf + n);
    }
    ::close(fd);
    const auto hdr = raw.find("\r\n\r\n");
    if (hdr == std::string::npos) {
        err = "no header";
        return {};
    }
    const auto sp = raw.find(' ');
    if (sp != std::string::npos)
        status = std::atoi(raw.c_str() + sp + 1);
    return raw.substr(hdr + 4);
}

// Scripted helper. Responses are (status, body) in order, then the last one repeats.
struct FakeHelper {
    int fd = -1;
    int port = 0;
    std::thread thr;
    std::atomic<bool> stop{false};
    std::vector<std::pair<int, std::string>> script;
    std::atomic<int> hits{0};
    void start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        CHECK(::listen(fd, 8) == 0);
        socklen_t l = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        thr = std::thread([this] { loop(); });
    }
    void loop() {
        while (!stop.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            timeval tv{0, 100000};
            if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
                continue;
            const int c = ::accept(fd, nullptr, nullptr);
            if (c < 0)
                continue;
            char buf[512];
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            const std::string req(buf, buf + (n > 0 ? n : 0));
            const int i = hits.fetch_add(1);
            const auto& step = script[static_cast<size_t>(i < static_cast<int>(script.size()) ? i : script.size() - 1)];
            // start_ms= in the request selects a body when the script key is negative status.
            (void)req;
            const std::string body = step.second;
            const std::string hdr = "HTTP/1.0 " + std::to_string(step.first) + " x\r\nContent-Length: " +
                                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            ::send(c, hdr.data(), hdr.size(), 0);
            if (!body.empty())
                ::send(c, body.data(), body.size(), 0);
            ::close(c);
        }
    }
    void shutdownSrv() {
        stop.store(true);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd = -1;
        }
        if (thr.joinable())
            thr.join();
    }
};

static bool badState(const std::vector<std::string>& st) {
    for (const auto& s : st)
        if (s == "ended" || s == "idle" || s == "stopped")
            return true;
    return false;
}

static RetryHit playUrl(int port, const std::string& path, int64_t startMs, int64_t previousMs,
                        std::atomic<bool>& current, std::vector<std::string>& states, std::vector<int>& sleeps,
                        std::string& body) {
    return retryOpens(
        [&](int64_t ms, OpenClass& c) {
            std::string err;
            const std::string p = path + (path.find('?') == std::string::npos ? "?" : "&") + "start_ms=" +
                                  std::to_string(ms);
            body = httpGet("127.0.0.1", port, p, c.httpStatus, err);
            c.bodyBytes = body.size();
            c.error = err;
            c.ok = c.httpStatus >= 200 && c.httpStatus < 300 && c.bodyBytes > 0;
        },
        startMs, previousMs, [&] { return current.load(); },
        [&](int64_t) { states.push_back("buffering"); },
        [&](int ms) { sleeps.push_back(ms); });
}

int main() {
    OpenClass empty;
    empty.httpStatus = 200;
    empty.bodyBytes = 0;
    CHECK(gcastOpenRetryable(empty));
    OpenClass five;
    five.httpStatus = 503;
    five.bodyBytes = 20;
    CHECK(gcastOpenRetryable(five));
    OpenClass io;
    io.error = "open: Input/output error";
    CHECK(gcastOpenRetryable(io));
    OpenClass missing;
    missing.httpStatus = 404;
    missing.bodyBytes = 0;
    CHECK(!gcastOpenRetryable(missing));
    CHECK(urlWithStartMs("http://127.0.0.1/yt.ts?v=abc&start_ms=240000", 10000) ==
          "http://127.0.0.1/yt.ts?v=abc&start_ms=10000");
    CHECK(kGcastOpenBackoffMs[0] == 300 && kGcastOpenBackoffMs[1] == 800 && kGcastOpenBackoffMs[2] == 1500 &&
          kGcastOpenBackoffMs[3] == 3000);

    const std::string ts(188, '\x47');
    FakeHelper helper;
    helper.script = {{200, ""}, {200, ""}, {200, ts}};
    helper.start();
    CHECK(helper.port > 0);
    std::atomic<bool> current{true};
    std::vector<std::string> states;
    std::vector<int> sleeps;
    std::string body;
    const RetryHit hit = playUrl(helper.port, "/yt.ts?v=abc", 240000, -1, current, states, sleeps, body);
    if (hit.played)
        states.push_back("playing");
    CHECK(hit.played && !hit.superseded && body.size() == 188 && body[0] == '\x47');
    CHECK(!badState(states));
    CHECK(!states.empty() && states.back() == "playing");
    CHECK(sleeps.size() == 2 && sleeps[0] == 300 && sleeps[1] == 800);
    helper.shutdownSrv();

    FakeHelper fiveSrv;
    fiveSrv.script = {{503, "busy"}, {200, ts}};
    fiveSrv.start();
    states.clear();
    sleeps.clear();
    body.clear();
    const RetryHit hit5 = playUrl(fiveSrv.port, "/yt.ts?v=abc", 0, -1, current, states, sleeps, body);
    if (hit5.played)
        states.push_back("playing");
    CHECK(hit5.played && body.size() == 188 && sleeps.size() == 1 && sleeps[0] == 300);
    CHECK(!badState(states) && states.back() == "playing");
    fiveSrv.shutdownSrv();

    FakeHelper gone;
    gone.script = {{200, ""}, {200, ""}, {200, ""}, {200, ""}, {200, ""}};
    gone.start();
    states.clear();
    sleeps.clear();
    std::atomic<bool> live{true};
    const RetryHit sup = retryOpens(
        [&](int64_t, OpenClass& c) {
            c.httpStatus = 200;
            c.bodyBytes = 0;
            c.ok = false;
            c.error = "open: Input/output error";
        },
        180000, -1, [&] { return live.load(); },
        [&](int64_t) { states.push_back("buffering"); },
        [&](int ms) {
            sleeps.push_back(ms);
            live.store(false);   // a newer cast arrives during the first wait
        });
    CHECK(sup.superseded && !sup.played);
    CHECK(!badState(states));
    CHECK(states.size() == 1 && states[0] == "buffering");
    gone.shutdownSrv();

    states.clear();
    sleeps.clear();
    body.clear();
    current.store(true);
    const RetryHit back = retryOpens(
        [&](int64_t ms, OpenClass& c) {
            if (ms == 240000) {
                c.httpStatus = 200;
                c.bodyBytes = 0;
                c.ok = false;
                c.error = "open: Input/output error";
                return;
            }
            body = ts;
            c.httpStatus = 200;
            c.bodyBytes = body.size();
            c.ok = true;
        },
        240000, 10000, [&] { return true; }, [&](int64_t) { states.push_back("buffering"); },
        [&](int ms) { sleeps.push_back(ms); });
    if (back.played)
        states.push_back("playing");
    CHECK(back.played && back.resumedPrevious && back.playedMs == 10000);
    CHECK(body.size() == 188 && !badState(states) && states.back() == "playing");
    CHECK(sleeps.size() == 4 && sleeps[0] == 300 && sleeps[3] == 3000);

    std::printf("test_gcast_open_retry: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
