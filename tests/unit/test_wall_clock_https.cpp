// HTTPS Date and the fake RTC, with the process clock held at 1970 by
// tests/unit/wall_clock_faketime.c (LD_PRELOAD). Certificate notBefore is
// "now" on the real clock, so a 1970 verifier rejects the chain unless
// X509_V_FLAG_NO_CHECK_TIME is set. Chain and hostname stay enforced.
#include "gcast/https_client.hpp"
#include "wall_clock.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <csignal>

namespace {
int g_checks = 0;
int g_fail = 0;
#define CHECK(cond)                                                               \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_fail;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                         \
    } while (0)

template <typename T>
T* sym(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        std::fprintf(stderr, "faketime shim missing %s (run under LD_PRELOAD=wall_clock_faketime.so)\n", name);
        std::exit(2);
    }
    return static_cast<T*>(p);
}

class LocalTls {
public:
    LocalTls(std::string cert, std::string key, std::string date, std::string body)
        : cert_(std::move(cert)), key_(std::move(key)), date_(std::move(date)), body_(std::move(body)) {
        lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        if (lfd_ < 0 || ::bind(lfd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(lfd_, 8) != 0) {
            std::fprintf(stderr, "local tls bind failed\n");
            std::exit(2);
        }
        socklen_t sl = sizeof sa;
        ::getsockname(lfd_, reinterpret_cast<sockaddr*>(&sa), &sl);
        port_ = ntohs(sa.sin_port);
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_file(ctx_, cert_.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx_, key_.c_str(), SSL_FILETYPE_PEM) != 1) {
            std::fprintf(stderr, "local tls cert load failed\n");
            std::exit(2);
        }
        thr_ = std::thread([this] { loop(); });
    }
    ~LocalTls() {
        stop_.store(true);
        if (lfd_ >= 0)
            ::shutdown(lfd_, SHUT_RDWR);
        if (thr_.joinable())
            thr_.join();
        if (ctx_)
            SSL_CTX_free(ctx_);
        if (lfd_ >= 0)
            ::close(lfd_);
    }
    int port() const { return port_; }

private:
    void loop() {
        while (!stop_.load()) {
            pollfd p{lfd_, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0)
                continue;
            int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0)
                continue;
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) == 1) {
                char buf[2048];
                std::string req;
                while (req.find("\r\n\r\n") == std::string::npos && req.size() < 4000) {
                    int n = SSL_read(ssl, buf, sizeof buf);
                    if (n <= 0)
                        break;
                    req.append(buf, static_cast<size_t>(n));
                }
                const std::string resp = "HTTP/1.1 200 OK\r\nDate: " + date_ + "\r\nContent-Length: " +
                                         std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
                SSL_write(ssl, resp.data(), static_cast<int>(resp.size()));
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(fd);
        }
    }
    std::string cert_, key_, date_, body_;
    SSL_CTX* ctx_ = nullptr;
    int lfd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thr_;
};

misterplex::HttpsDateTarget target(const std::string& ca, int port, bool skip) {
    misterplex::HttpsDateTarget t;
    t.verifyHost = "clock.test";
    t.connectHost = "127.0.0.1";
    t.port = port;
    t.caFile = ca;
    t.skipTimeCheck = skip;
    t.path = "/";
    return t;
}
} // namespace

int main() {
    std::signal(SIGALRM, [](int) {
        std::fprintf(stderr, "FAIL wall-clock https test exceeded 30s (deadlock or hang)\n");
        std::_Exit(3);
    });
    ::alarm(45);

    auto* fakeNow = sym<volatile time_t>("mpx_fake_now");
    auto* rtcMode = sym<volatile int>("mpx_fake_rtc_mode");
    auto* rtcValid = sym<volatile int>("mpx_fake_rtc_valid");
    auto* rtcUnix = sym<volatile time_t>("mpx_fake_rtc_unix");
    auto* rtcWrites = sym<volatile int>("mpx_fake_rtc_writes");
    auto* rtcWritten = sym<volatile time_t>("mpx_fake_rtc_written");
    const char* rtcPath = sym<char>("mpx_fake_rtc_path");
    *fakeNow = 0;
    CHECK(std::time(nullptr) == 0);

    const char* dir = std::getenv("MPX_CLOCK_CERT_DIR");
    if (!dir || !*dir) {
        std::fprintf(stderr, "MPX_CLOCK_CERT_DIR is not set\n");
        return 2;
    }
    const std::string ca = std::string(dir) + "/ca.pem";
    const std::string cert = std::string(dir) + "/srv.pem";
    const std::string key = std::string(dir) + "/srv.key";
    const std::string badca = std::string(dir) + "/badca.pem";
    const std::time_t good = misterplex::wallClockBuildFloor() + 2 * 86400;
    CHECK(misterplex::wallTimePlausible(good));

    auto imf = [](std::time_t t) {
        std::tm tm{};
        gmtime_r(&t, &tm);
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = {months[tm.tm_mon * 3], months[tm.tm_mon * 3 + 1], months[tm.tm_mon * 3 + 2], 0};
        char buf[64];
        std::snprintf(buf, sizeof buf, "Sun, %02d %s %04d %02d:%02d:%02d GMT", tm.tm_mday, mon, tm.tm_year + 1900,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    };
    const std::string goodDate = imf(good);

    // Absent: the node does not exist, and the shim is not claiming it.
    *rtcMode = 0;
    {
        const auto absent = misterplex::readHardwareRtc("/dev/rtc-misterplex-absent");
        CHECK(!absent.present);
        CHECK(!absent.time);
        const auto also = misterplex::readHardwareRtc(rtcPath);
        CHECK(!also.present);
    }

    // Present and plausible. The decision steps from the chip and does not write it back.
    *rtcMode = 1;
    *rtcValid = 1;
    *rtcUnix = good;
    *rtcWrites = 0;
    {
        const auto sample = misterplex::readHardwareRtc(rtcPath);
        CHECK(sample.present);
        CHECK(sample.time && *sample.time == good);
        CHECK(misterplex::wallTimePlausible(*sample.time));
        int ntp = 0;
        std::time_t clock = 0;
        misterplex::WallClockOps ops;
        ops.now = [&] { return clock; };
        ops.readRtc = [&] { return misterplex::readHardwareRtc(rtcPath); };
        ops.writeRtc = [&](std::time_t t) { return misterplex::writeHardwareRtc(rtcPath, t); };
        ops.queryNtp = [&](int) {
            ++ntp;
            return std::nullopt;
        };
        ops.queryHttps = [&](int) { return std::nullopt; };
        ops.step = [&](std::time_t t) {
            clock = t;
            return true;
        };
        misterplex::WallClockSource src = misterplex::WallClockSource::None;
        std::string err;
        const auto st = misterplex::ensurePlausibleWallClock(ops, &err, &src);
        CHECK(st == misterplex::WallClockStatus::Stepped);
        CHECK(src == misterplex::WallClockSource::Rtc);
        CHECK(clock == good);
        CHECK(ntp == 0);
        CHECK(*rtcWrites == 0);
    }

    // Present but holding 1970. Not a source. SNTP supplies the time and is written back.
    *rtcUnix = 0;
    *rtcWrites = 0;
    *rtcWritten = 0;
    {
        const auto sample = misterplex::readHardwareRtc(rtcPath);
        CHECK(sample.present);
        CHECK(sample.time && *sample.time == 0);
        CHECK(!misterplex::wallTimePlausible(*sample.time));
        int ntp = 0;
        std::time_t clock = 0;
        misterplex::WallClockOps ops;
        ops.now = [&] { return clock; };
        ops.readRtc = [&] { return misterplex::readHardwareRtc(rtcPath); };
        ops.writeRtc = [&](std::time_t t) { return misterplex::writeHardwareRtc(rtcPath, t); };
        ops.queryNtp = [&](int) {
            ++ntp;
            return std::optional<std::time_t>(good);
        };
        ops.step = [&](std::time_t t) {
            clock = t;
            return true;
        };
        misterplex::WallClockSource src = misterplex::WallClockSource::None;
        std::string err;
        const auto st = misterplex::ensurePlausibleWallClock(ops, &err, &src);
        CHECK(st == misterplex::WallClockStatus::Stepped);
        CHECK(src == misterplex::WallClockSource::Sntp);
        CHECK(ntp == 1);
        CHECK(*rtcWrites == 1);
        CHECK(*rtcWritten == good);
        const auto after = misterplex::readHardwareRtc(rtcPath);
        CHECK(after.time && *after.time == good);
    }

    // Opened but never set (ioctl EINVAL): present, no time, then write-back.
    *rtcValid = 0;
    *rtcUnix = 0;
    *rtcWrites = 0;
    {
        const auto sample = misterplex::readHardwareRtc(rtcPath);
        CHECK(sample.present);
        CHECK(!sample.time);
        std::time_t clock = 0;
        misterplex::WallClockOps ops;
        ops.now = [&] { return clock; };
        ops.readRtc = [&] { return misterplex::readHardwareRtc(rtcPath); };
        ops.writeRtc = [&](std::time_t t) { return misterplex::writeHardwareRtc(rtcPath, t); };
        ops.queryNtp = [&](int) { return std::optional<std::time_t>(good); };
        ops.step = [&](std::time_t t) {
            clock = t;
            return true;
        };
        misterplex::WallClockSource src = misterplex::WallClockSource::None;
        const auto st = misterplex::ensurePlausibleWallClock(ops, nullptr, &src);
        CHECK(st == misterplex::WallClockStatus::Stepped);
        CHECK(src == misterplex::WallClockSource::Sntp);
        CHECK(*rtcWrites == 1);
    }
    *rtcMode = 0;

    LocalTls srv(cert, key, goodDate, "abcdefgh");
    const auto t0 = std::chrono::steady_clock::now();
    const auto ok = misterplex::tlsHttpExchange(target(ca, srv.port(), true), 2000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 10000);
    CHECK(ok.ok);
    CHECK(ok.date && *ok.date == good);
    CHECK(ok.err.empty());

    const auto rejected = misterplex::tlsHttpExchange(target(ca, srv.port(), false), 2000);
    CHECK(!rejected.ok);
    CHECK(rejected.err.find("not yet valid") != std::string::npos);

    auto wrong = target(ca, srv.port(), true);
    wrong.verifyHost = "not-clock.test";
    const auto badHost = misterplex::tlsHttpExchange(wrong, 2000);
    CHECK(!badHost.ok);
    CHECK(badHost.err.find("not yet valid") == std::string::npos);

    auto untrusted = target(badca, srv.port(), true);
    const auto badChain = misterplex::tlsHttpExchange(untrusted, 2000);
    CHECK(!badChain.ok);
    CHECK(!badChain.date);

    LocalTls other(cert, key, imf(good + 600), "abcdefgh");
    misterplex::HttpsDateTarget pair[2] = {target(ca, srv.port(), true), target(ca, other.port(), true)};
    CHECK(!misterplex::httpsDateFromTargets(pair, 2, 2000));
    LocalTls closeEnough(cert, key, imf(good + 120), "abcdefgh");
    misterplex::HttpsDateTarget agree[2] = {target(ca, srv.port(), true), target(ca, closeEnough.port(), true)};
    const auto agreed = misterplex::httpsDateFromTargets(agree, 2, 2000);
    CHECK(agreed && *agreed == good);

    LocalTls garbage(cert, key, "not-a-date", "abcdefgh");
    misterplex::HttpsDateTarget badOne = target(ca, garbage.port(), true);
    const auto bad = misterplex::tlsHttpExchange(badOne, 2000);
    CHECK(bad.ok);
    CHECK(!bad.date);
    CHECK(!misterplex::httpsDateFromTargets(&badOne, 1, 2000));

    LocalTls epoch(cert, key, "Thu, 01 Jan 1970 00:00:00 GMT", "abcdefgh");
    misterplex::HttpsDateTarget epochT = target(ca, epoch.port(), true);
    CHECK(!misterplex::httpsDateFromTargets(&epochT, 1, 2000));

    // generate_screen_id at 1970: Skip time validity, keep chain and hostname.
    CHECK(std::time(nullptr) == 0);
    misterplex::gcast::HttpsClient client(ca);
    CHECK(client.ready());
    misterplex::gcast::HttpsRequestFlags flags;
    flags.timeCheck = misterplex::gcast::TlsTimeCheck::Skip;
    flags.port = srv.port();
    flags.connectHost = "127.0.0.1";
    const auto sid = client.request("GET", "clock.test", "/api/lounge/pairing/generate_screen_id", "", "", 5, {},
                                    2u << 20, flags);
    CHECK(sid.ok);
    CHECK(sid.status == 200);
    CHECK(sid.body.size() >= 8 && sid.body.size() <= 64);
    CHECK(sid.body == "abcdefgh");

    misterplex::gcast::HttpsRequestFlags wrongFlags = flags;
    const auto sidWrong = client.request("GET", "not-clock.test", "/api/lounge/pairing/generate_screen_id", "", "", 5,
                                         {}, 2u << 20, wrongFlags);
    CHECK(!sidWrong.ok);

    // 1970: lounge token checks the current time and does not send the POST.
    {
        CHECK(std::time(nullptr) == 0);
        CHECK(!misterplex::wallTimePlausible(std::time(nullptr)));
        std::string err = "sentinel";
        const auto t0 = std::chrono::steady_clock::now();
        const auto st = misterplex::waitForWallClockRescue(0, &err, nullptr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(st == misterplex::WallClockStatus::Unavailable);
        CHECK(err.find("wall clock is implausible") != std::string::npos);
        CHECK(ms < 500);
        misterplex::gcast::HttpsRequestFlags lounge;
        lounge.port = srv.port();
        lounge.connectHost = "127.0.0.1";
        const auto tok = client.request("POST", "clock.test", "/api/lounge/pairing/get_lounge_token_batch", "",
                                        "screen_ids=abcdefgh", 5, {}, 2u << 20, lounge);
        CHECK(!tok.ok);
        CHECK(tok.status == 0);
        CHECK(tok.err.find("wall clock is implausible") != std::string::npos);
    }

    // Already 2026 (past the build). YouTube is allowed on the first check.
    {
        *fakeNow = good;
        CHECK(std::time(nullptr) == good);
        CHECK(misterplex::wallTimePlausible(std::time(nullptr)));
        std::string err = "sentinel";
        const auto t0 = std::chrono::steady_clock::now();
        const auto st = misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &err, nullptr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(st == misterplex::WallClockStatus::AlreadySane);
        CHECK(err.empty());
        CHECK(ms < 500);
        misterplex::gcast::HttpsRequestFlags lounge;
        lounge.port = srv.port();
        lounge.connectHost = "127.0.0.1";
        const auto tok = client.request("POST", "clock.test", "/api/lounge/pairing/get_lounge_token_batch", "",
                                        "screen_ids=abcdefgh", 5, {}, 2u << 20, lounge);
        CHECK(tok.ok);
        CHECK(tok.status == 200);
        CHECK(tok.err.find("wall clock is implausible") == std::string::npos);
    }

    std::printf("test_wall_clock_https: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
