#include "wall_clock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

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
} // namespace

std::string imfDate(std::time_t t) {
    std::tm tm{};
    if (!gmtime_r(&t, &tm))
        return {};
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {months[tm.tm_mon * 3], months[tm.tm_mon * 3 + 1], months[tm.tm_mon * 3 + 2], 0};
    char buf[64];
    std::snprintf(buf, sizeof buf, "Sun, %02d %s %04d %02d:%02d:%02d GMT", tm.tm_mday, mon, tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

int main() {
#ifdef MPX_REQUIRE_TIME_T_32
    CHECK(sizeof(std::time_t) == 4);
#endif
    const std::time_t good = misterplex::wallClockBuildFloor() + 86400;
    CHECK(good < misterplex::kPlausibleWallCeiling);
    CHECK(!misterplex::wallTimePlausible(0));
    CHECK(!misterplex::wallTimePlausible(1));
    CHECK(!misterplex::wallTimePlausible(misterplex::wallClockBuildFloor() - 1));
    CHECK(!misterplex::wallTimePlausible(1609459200)); // 2021-01-01, before this build
    CHECK(misterplex::wallTimePlausible(good));
    CHECK(!misterplex::wallTimePlausible(misterplex::kPlausibleWallCeiling));
    // Arm GNU time_t is 32-bit. 2100-01-01 does not fit: the ceiling wraps
    // negative, and a comparison in time_t then rejects every 2026 clock.
    {
        const auto wrapped = static_cast<std::int32_t>(static_cast<std::uint32_t>(misterplex::kPlausibleWallCeiling));
        CHECK(wrapped < 0);
        const std::time_t y2026 = 1767225600; // 2026-01-01 UTC, still inside signed 32-bit
        CHECK(y2026 > 0);
        CHECK(static_cast<std::int64_t>(y2026) < misterplex::kPlausibleWallCeiling);
        CHECK(!(static_cast<std::int64_t>(y2026) < static_cast<std::int64_t>(wrapped)));
        CHECK(misterplex::wallTimePlausible(good));
        CHECK(!misterplex::wallTimePlausible(0));
    }

    int queries = 0;
    int steps = 0;
    std::time_t clock = 0;
    misterplex::WallClockOps ops;
    ops.now = [&] { return clock; };
    ops.queryNtp = [&](int) {
        ++queries;
        return std::optional<std::time_t>(good);
    };
    ops.step = [&](std::time_t t) {
        ++steps;
        clock = t;
        return true;
    };
    std::string err;
    const auto stepped = misterplex::ensurePlausibleWallClock(ops, &err);
    CHECK(stepped == misterplex::WallClockStatus::Stepped);
    CHECK(queries == 1);
    CHECK(steps == 1);
    CHECK(misterplex::wallTimePlausible(clock));

    queries = 0;
    steps = 0;
    const auto again = misterplex::ensurePlausibleWallClock(ops, &err);
    CHECK(again == misterplex::WallClockStatus::AlreadySane);
    CHECK(queries == 0);
    CHECK(steps == 0);

    clock = 0;
    ops.queryNtp = [&](int) {
        ++queries;
        return std::nullopt;
    };
    err.clear();
    const auto down = misterplex::ensurePlausibleWallClock(ops, &err);
    CHECK(down == misterplex::WallClockStatus::Unavailable);
    CHECK(steps == 0);
    CHECK(err.find("NTP") != std::string::npos);

    clock = 0;
    ops.queryNtp = [&](int) { return std::optional<std::time_t>(good); };
    ops.step = [&](std::time_t) {
        ++steps;
        return false;
    };
    err.clear();
    const auto noset = misterplex::ensurePlausibleWallClock(ops, &err);
    CHECK(noset == misterplex::WallClockStatus::Unavailable);
    CHECK(steps == 1);
    CHECK(err.find("settimeofday") != std::string::npos);

    auto put32 = [](unsigned char* p, uint32_t v) {
        p[0] = static_cast<unsigned char>(v >> 24);
        p[1] = static_cast<unsigned char>(v >> 16);
        p[2] = static_cast<unsigned char>(v >> 8);
        p[3] = static_cast<unsigned char>(v);
    };
    unsigned char origin[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char pkt[48] = {};
    pkt[0] = static_cast<unsigned char>((0u << 6) | (4u << 3) | 4u); // LI=0 VN=4 mode=4
    pkt[1] = 1;
    std::memcpy(pkt + 24, origin, 8);
    put32(pkt + 40, static_cast<uint32_t>(static_cast<uint64_t>(good) + misterplex::kNtpUnixDelta));
    const auto accepted = misterplex::sntpAcceptReply(pkt, 48, origin);
    CHECK(accepted.has_value());
    CHECK(accepted && *accepted == good);
    put32(pkt + 40, 1700000000u + misterplex::kNtpUnixDelta); // before this binary's build
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));
    put32(pkt + 40, static_cast<uint32_t>(static_cast<uint64_t>(good) + misterplex::kNtpUnixDelta));

    pkt[0] = static_cast<unsigned char>((0u << 6) | (4u << 3) | 3u); // mode=3, not a server
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));
    pkt[0] = static_cast<unsigned char>((0u << 6) | (4u << 3) | 4u);
    pkt[1] = 0; // kiss-o-death
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));
    pkt[1] = 1;
    pkt[0] = static_cast<unsigned char>((3u << 6) | (4u << 3) | 4u); // LI=3
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));
    pkt[0] = static_cast<unsigned char>((0u << 6) | (4u << 3) | 4u);
    pkt[24] ^= 0x01; // origin does not echo
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));
    std::memcpy(pkt + 24, origin, 8);
    put32(pkt + 40, misterplex::kNtpUnixDelta); // unix 0
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));
    put32(pkt + 40, 4102444800u + misterplex::kNtpUnixDelta); // 2100-01-01
    CHECK(!misterplex::sntpAcceptReply(pkt, 47, origin));
    CHECK(!misterplex::sntpAcceptReply(pkt, 48, origin));

    int ntpQ = 0;
    int httpsQ = 0;
    int rtcWrites = 0;
    auto resetOps = [&] {
        clock = 0;
        steps = 0;
        ntpQ = 0;
        httpsQ = 0;
        rtcWrites = 0;
        ops.now = [&] { return clock; };
        ops.readRtc = [] { return misterplex::RtcSample{}; };
        ops.writeRtc = [&](std::time_t) {
            ++rtcWrites;
            return true;
        };
        ops.queryNtp = [&](int) {
            ++ntpQ;
            return std::optional<std::time_t>(good);
        };
        ops.queryHttps = [&](int) {
            ++httpsQ;
            return std::optional<std::time_t>(good);
        };
        ops.step = [&](std::time_t t) {
            ++steps;
            clock = t;
            return true;
        };
    };

    // RTC present and plausible: step from it, do not ask the network, do not write back.
    resetOps();
    ops.readRtc = [&] {
        misterplex::RtcSample s;
        s.present = true;
        s.time = good;
        return s;
    };
    misterplex::WallClockSource src = misterplex::WallClockSource::None;
    err.clear();
    const auto viaRtc = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(viaRtc == misterplex::WallClockStatus::Stepped);
    CHECK(src == misterplex::WallClockSource::Rtc);
    CHECK(clock == good);
    CHECK(steps == 1);
    CHECK(ntpQ == 0);
    CHECK(httpsQ == 0);
    CHECK(rtcWrites == 0);

    // RTC present but still at 1970: fall through to SNTP, then write the good time back.
    resetOps();
    ops.readRtc = [] {
        misterplex::RtcSample s;
        s.present = true;
        s.time = 0;
        return s;
    };
    src = misterplex::WallClockSource::None;
    const auto viaSntp = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(viaSntp == misterplex::WallClockStatus::Stepped);
    CHECK(src == misterplex::WallClockSource::Sntp);
    CHECK(ntpQ == 1);
    CHECK(httpsQ == 0);
    CHECK(rtcWrites == 1);
    CHECK(clock == good);

    // No RTC node: SNTP steps the clock and does not try to write a chip that is not there.
    resetOps();
    src = misterplex::WallClockSource::None;
    const auto noRtc = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(noRtc == misterplex::WallClockStatus::Stepped);
    CHECK(src == misterplex::WallClockSource::Sntp);
    CHECK(rtcWrites == 0);
    CHECK(ntpQ == 1);

    // SNTP blocked, HTTPS Date answers: one query each, step from HTTPS, no loop.
    resetOps();
    ops.queryNtp = [&](int) {
        ++ntpQ;
        return std::nullopt;
    };
    src = misterplex::WallClockSource::None;
    const auto viaHttps = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(viaHttps == misterplex::WallClockStatus::Stepped);
    CHECK(src == misterplex::WallClockSource::HttpsDate);
    CHECK(ntpQ == 1);
    CHECK(httpsQ == 1);
    CHECK(steps == 1);
    CHECK(rtcWrites == 0);

    // Present-but-1970 plus SNTP down: HTTPS steps, and the chip is written.
    resetOps();
    ops.readRtc = [] {
        misterplex::RtcSample s;
        s.present = true;
        s.time = std::nullopt;
        return s;
    };
    ops.queryNtp = [&](int) {
        ++ntpQ;
        return std::nullopt;
    };
    src = misterplex::WallClockSource::None;
    const auto viaHttpsRtc = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(viaHttpsRtc == misterplex::WallClockStatus::Stepped);
    CHECK(src == misterplex::WallClockSource::HttpsDate);
    CHECK(rtcWrites == 1);

    // Both network sources blocked: one try each, the same error, no step.
    resetOps();
    ops.queryNtp = [&](int) {
        ++ntpQ;
        return std::nullopt;
    };
    ops.queryHttps = [&](int) {
        ++httpsQ;
        return std::nullopt;
    };
    err.clear();
    const auto bothDown = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(bothDown == misterplex::WallClockStatus::Unavailable);
    CHECK(ntpQ == 1);
    CHECK(httpsQ == 1);
    CHECK(steps == 0);
    CHECK(err.find("NTP") != std::string::npos);

    // The Date fetch must not wait on a nested rescue.
    resetOps();
    ops.queryNtp = [&](int) {
        ++ntpQ;
        return std::nullopt;
    };
    ops.queryHttps = [&](int) {
        ++httpsQ;
        std::string inner;
        const auto st = misterplex::ensurePlausibleWallClock(ops, &inner);
        CHECK(st == misterplex::WallClockStatus::Unavailable);
        CHECK(inner.find("re-entered") != std::string::npos);
        return std::optional<std::time_t>(good);
    };
    err.clear();
    src = misterplex::WallClockSource::None;
    const auto reentered = misterplex::ensurePlausibleWallClock(ops, &err, &src);
    CHECK(reentered == misterplex::WallClockStatus::Stepped);
    CHECK(src == misterplex::WallClockSource::HttpsDate);
    CHECK(httpsQ == 1);
    CHECK(ntpQ == 1);

    const auto parsed = misterplex::parseImfFixDate(imfDate(good));
    CHECK(parsed && *parsed == good);
    CHECK(!misterplex::parseImfFixDate("not a date"));
    CHECK(!misterplex::parseImfFixDate(""));
    const auto epochDate = misterplex::parseImfFixDate("Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK(epochDate && *epochDate == 0);
    CHECK(!misterplex::wallTimePlausible(*epochDate));
    CHECK(!misterplex::acceptHttpsDatePair(epochDate, std::nullopt));
    CHECK(!misterplex::acceptHttpsDatePair(std::optional<std::time_t>(misterplex::kPlausibleWallCeiling),
                                           std::nullopt));
    CHECK(!misterplex::acceptHttpsDatePair(std::optional<std::time_t>(1609459200), std::nullopt));
    CHECK(misterplex::acceptHttpsDatePair(good, good + 240) &&
          *misterplex::acceptHttpsDatePair(good, good + 240) == good);
    CHECK(!misterplex::acceptHttpsDatePair(good, good + 301));
    CHECK(misterplex::acceptHttpsDatePair(good, std::nullopt) &&
          *misterplex::acceptHttpsDatePair(good, std::nullopt) == good);
    CHECK(misterplex::acceptHttpsDatePair(std::nullopt, epochDate) == std::nullopt);
    CHECK(misterplex::acceptHttpsDatePair(epochDate, std::optional<std::time_t>(good)) &&
          *misterplex::acceptHttpsDatePair(epochDate, std::optional<std::time_t>(good)) == good);

    // Network comes up 20s after start. Early attempts must not touch SNTP or HTTPS.
    {
        std::time_t wall = 1000;
        int mono = 0;
        int ntpCalls = 0;
        int httpsCalls = 0;
        int ntpBeforeReady = 0;
        std::vector<int> sleeps;
        std::vector<std::string> logs;
        const std::time_t good = misterplex::wallClockBuildFloor() + 86400;
        misterplex::WallClockRescueOps ops;
        ops.now = [&] { return wall; };
        ops.readRtc = [] { return misterplex::RtcSample{}; };
        ops.networkReady = [&] { return mono >= 20000; };
        ops.queryNtp = [&](int) -> std::optional<std::time_t> {
            ++ntpCalls;
            if (mono < 20000) {
                ++ntpBeforeReady;
                return std::nullopt;
            }
            return good;
        };
        ops.queryHttps = [&](int) -> std::optional<std::time_t> {
            ++httpsCalls;
            return std::nullopt;
        };
        ops.step = [&](std::time_t t) {
            wall = t;
            return true;
        };
        ops.sleepMs = [&](int ms) {
            sleeps.push_back(ms);
            mono += ms;
        };
        ops.log = [&](const char* line) { logs.push_back(line); };
        ops.stop = [&] { return mono > 120000; };
        const misterplex::WallClockRescueResult r = misterplex::runWallClockRescue(ops);
        CHECK(r.status == misterplex::WallClockStatus::Stepped);
        CHECK(r.source == misterplex::WallClockSource::Sntp);
        CHECK(ntpBeforeReady == 0);
        CHECK(ntpCalls == 1);
        CHECK(httpsCalls == 0);
        CHECK(mono >= 20000);
        CHECK(sleeps.size() == 4);
        if (sleeps.size() == 4) {
            CHECK(sleeps[0] == 2000 && sleeps[1] == 4000 && sleeps[2] == 8000 && sleeps[3] == 16000);
        }
        bool sawWait = false, sawStep = false;
        for (const auto& line : logs) {
            if (line.find("network not ready") != std::string::npos)
                sawWait = true;
            if (line.find("stepped from SNTP") != std::string::npos)
                sawStep = true;
        }
        CHECK(sawWait && sawStep);
        CHECK(wall == good);
    }

    // Stops once the clock is plausible. The mutant that ignores that check stays in the backoff.
    {
        misterplex::WallClockRescueThread thr;
        const std::time_t good = misterplex::wallClockBuildFloor() + 86400;
        misterplex::WallClockRescueOps ops;
        ops.now = [&] { return good; };
        ops.readRtc = [] { return misterplex::RtcSample{}; };
        ops.networkReady = [] { return false; };
        const auto t0 = std::chrono::steady_clock::now();
        thr.start(ops);
        const auto deadline = t0 + std::chrono::milliseconds(500);
        while (!thr.finished() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool stopped = thr.finished();
        thr.stopAndJoin();
        CHECK(stopped);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    }

    // Offline, shutdown during the backoff: join returns in under a second.
    {
        misterplex::WallClockRescueThread thr;
        misterplex::WallClockRescueOps ops;
        ops.now = [] { return static_cast<std::time_t>(1000); };
        ops.readRtc = [] { return misterplex::RtcSample{}; };
        ops.networkReady = [] { return false; };
        thr.start(ops);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto t0 = std::chrono::steady_clock::now();
        thr.stopAndJoin();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms < 1000);
        CHECK(thr.finished());
    }

    {
        long long last = 0;
        CHECK(misterplex::offlineClockLogDue(1000, last, 300000));
        CHECK(!misterplex::offlineClockLogDue(2000, last, 300000));
        CHECK(misterplex::offlineClockLogDue(1000 + 300000, last, 300000));
    }
    CHECK(misterplex::logClockSteppedOnce("sntp"));
    CHECK(!misterplex::logClockSteppedOnce("sntp"));

    {
        std::ifstream cf(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/wall_clock.cpp");
        std::stringstream css;
        css << cf.rdbuf();
        const std::string clockcpp2 = css.str();
        const auto startAt = clockcpp2.find("void startWallClockRescue()");
        CHECK(startAt != std::string::npos);
        if (startAt != std::string::npos) {
            const std::string body = clockcpp2.substr(startAt, 2200);
            CHECK(body.find("detach") == std::string::npos);
            CHECK(body.find("rescueThread().start") != std::string::npos);
        }
        CHECK(clockcpp2.find("stopWallClockRescue") != std::string::npos);
        std::ifstream mf(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/main.cpp");
        std::stringstream mss;
        mss << mf.rdbuf();
        CHECK(mss.str().find("stopWallClockRescue()") != std::string::npos);
    }

    auto slurp = [](const char* rel) {
        std::ifstream f(std::string(MPX_SOURCE_ROOT) + "/" + rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string https = slurp("arm/misterplexd/gcast/https_client.cpp");
    const std::string maincpp = slurp("arm/misterplexd/main.cpp");
    const std::string yt = slurp("arm/misterplexd/gcast/yt_fe.cpp");
    const std::string clockcpp = slurp("arm/misterplexd/wall_clock.cpp");
    CHECK(https.find("SSL_VERIFY_PEER") != std::string::npos);
    CHECK(https.find("SSL_VERIFY_NONE") == std::string::npos);
    CHECK(https.find("X509_V_FLAG_NO_CHECK_TIME") != std::string::npos);
    CHECK(https.find("TlsTimeCheck::Skip") != std::string::npos);
    CHECK(maincpp.find("rescueClockFromEnv") == std::string::npos);
    CHECK(maincpp.find("clock_sync") == std::string::npos);
    CHECK(yt.find("waitForWallClockRescue") != std::string::npos);
    CHECK(maincpp.find("startWallClockRescue") != std::string::npos);
    CHECK(https.find("waitForWallClockRescue") != std::string::npos);
    CHECK(https.find("SSL_set1_host") != std::string::npos);
    CHECK(https.find("SSL_set1_host(ssl, host.c_str()) != 1") != std::string::npos);
    CHECK(clockcpp.find("SSL_set1_host") != std::string::npos);
    CHECK(clockcpp.find("!= 1") != std::string::npos);
    CHECK(yt.find("TlsTimeCheck::Skip") != std::string::npos);
    CHECK(yt.find("generate_screen_id") != std::string::npos);
    CHECK(clockcpp.find("RTC_RD_TIME") != std::string::npos);
    CHECK(clockcpp.find("RTC_SET_TIME") != std::string::npos);
    CHECK(clockcpp.find("X509_V_FLAG_NO_CHECK_TIME") != std::string::npos);
    CHECK(clockcpp.find("SSL_VERIFY_NONE") == std::string::npos);
    CHECK(clockcpp.find("SSL_VERIFY_PEER") != std::string::npos);

    // A clock that is already sane stops the rescue on attempt 1. SNTP and
    // HTTPS Date are not queried, so YouTube is not waiting on a step.
    {
        int ntp = 0;
        int httpsCalls = 0;
        std::vector<std::string> logs;
        misterplex::WallClockRescueOps ops;
        ops.now = [&] { return good; };
        ops.readRtc = [] {
            misterplex::RtcSample s;
            s.present = true;
            s.time = static_cast<std::time_t>(0);
            return s;
        };
        ops.networkReady = [] { return true; };
        ops.queryNtp = [&](int) {
            ++ntp;
            return std::optional<std::time_t>(good);
        };
        ops.queryHttps = [&](int) {
            ++httpsCalls;
            return std::optional<std::time_t>(good);
        };
        ops.log = [&](const char* line) { logs.push_back(line); };
        const misterplex::WallClockRescueResult r = misterplex::runWallClockRescue(ops);
        CHECK(r.status == misterplex::WallClockStatus::AlreadySane);
        CHECK(r.attempts == 1);
        CHECK(ntp == 0);
        CHECK(httpsCalls == 0);
        bool sawSane = false;
        bool sawSntp = false;
        for (const auto& line : logs) {
            if (line.find("already plausible") != std::string::npos)
                sawSane = true;
            if (line.find("SNTP") != std::string::npos)
                sawSntp = true;
        }
        CHECK(sawSane);
        CHECK(!sawSntp);
    }

    // ntpd can answer with a different time. A plausible clock does not step.
    {
        int steps = 0;
        misterplex::WallClockRescueOps ops;
        ops.now = [&] { return good; };
        ops.networkReady = [] { return true; };
        ops.queryNtp = [&](int) { return std::optional<std::time_t>(good + 90); };
        ops.step = [&](std::time_t) {
            ++steps;
            return true;
        };
        const misterplex::WallClockRescueResult r = misterplex::runWallClockRescue(ops);
        CHECK(r.status == misterplex::WallClockStatus::AlreadySane);
        CHECK(steps == 0);
    }

    // 1970 is not plausible. The rescue logs rtc0 / SNTP / HTTPS Date and
    // does not claim the clock was already sane or stepped.
    {
        int ntp = 0;
        int httpsCalls = 0;
        bool stop = false;
        std::vector<std::string> logs;
        misterplex::WallClockRescueOps ops;
        ops.now = [] { return static_cast<std::time_t>(0); };
        ops.readRtc = [] {
            misterplex::RtcSample s;
            s.present = true;
            s.time = static_cast<std::time_t>(0);
            return s;
        };
        ops.networkReady = [] { return true; };
        ops.queryNtp = [&](int) {
            ++ntp;
            return std::nullopt;
        };
        ops.queryHttps = [&](int) {
            ++httpsCalls;
            return std::nullopt;
        };
        ops.sleepMs = [&](int) { stop = true; };
        ops.stop = [&] { return stop; };
        ops.log = [&](const char* line) { logs.push_back(line); };
        const misterplex::WallClockRescueResult r = misterplex::runWallClockRescue(ops);
        CHECK(r.status == misterplex::WallClockStatus::Unavailable);
        CHECK(r.attempts == 1);
        CHECK(ntp == 1);
        CHECK(httpsCalls == 1);
        bool sawRtc = false, sawSntp = false, sawHttps = false, sawSane = false, sawStep = false;
        for (const auto& line : logs) {
            if (line.find("rtc0 present but not a plausible time") != std::string::npos)
                sawRtc = true;
            if (line.find("SNTP") != std::string::npos)
                sawSntp = true;
            if (line.find("HTTPS Date") != std::string::npos)
                sawHttps = true;
            if (line.find("already plausible") != std::string::npos)
                sawSane = true;
            if (line.find("stepped") != std::string::npos)
                sawStep = true;
        }
        CHECK(sawRtc && sawSntp && sawHttps);
        CHECK(!sawSane && !sawStep);
    }

    // Lounge token uses the current time. The wait is only entered when that
    // check fails; a sane clock is not gated on "rescue stepped".
    {
        const auto gate = yt.find("wallTimePlausible(std::time(nullptr))");
        CHECK(gate != std::string::npos);
        CHECK(yt.find("waitForWallClockRescue", gate) != std::string::npos);
        const auto waitFn = clockcpp.find("WallClockStatus waitForWallClockRescue");
        CHECK(waitFn != std::string::npos);
        const std::string body = clockcpp.substr(waitFn, 1600);
        const auto sane = body.find("return WallClockStatus::AlreadySane");
        const auto fallback = body.find("background rescue has not stepped it yet");
        CHECK(sane != std::string::npos);
        CHECK(fallback != std::string::npos);
        CHECK(sane < fallback);
        CHECK(body.find("wallTimePlausible(std::time(nullptr))") != std::string::npos);
    }

    // Exit 134 is the shell's report of SIGABRT. A joinable std::thread
    // destroyed on the way out of main calls std::terminate. main.cpp's
    // StopWallClockOnReturn joins before that destructor runs.
    {
        auto child = [](bool join) -> int {
            const pid_t pid = fork();
            if (pid < 0)
                return -1;
            if (pid == 0) {
                struct rlimit noCore = {0, 0};
                setrlimit(RLIMIT_CORE, &noCore);
                const int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
                {
                    misterplex::WallClockRescueThread thr;
                    misterplex::WallClockRescueOps ops;
                    ops.now = [] { return std::time(nullptr); };
                    ops.networkReady = [] { return false; };
                    thr.start(ops);
                    if (join) {
                        struct Guard {
                            misterplex::WallClockRescueThread* t;
                            ~Guard() { t->stopAndJoin(); }
                        } guard{&thr};
                    }
                }
                std::_Exit(0);
            }
            int status = 0;
            if (waitpid(pid, &status, 0) < 0)
                return -1;
            return status;
        };
        const int aborted = child(false);
        const bool sigAbort = WIFSIGNALED(aborted) && WTERMSIG(aborted) == SIGABRT;
        const bool exit134 = WIFEXITED(aborted) && WEXITSTATUS(aborted) == 134;
        CHECK(sigAbort || exit134);
        const int joined = child(true);
        CHECK(WIFEXITED(joined) && WEXITSTATUS(joined) == 0);

        const auto mainAt = maincpp.find("int main(");
        const auto guardAt = maincpp.find("struct StopWallClockOnReturn");
        const auto dtorAt = maincpp.find("~StopWallClockOnReturn()");
        const auto joinAt = maincpp.find("stopWallClockRescue();", dtorAt);
        const auto objAt = maincpp.find("StopWallClockOnReturn stopWallClockOnReturn;");
        const auto rescueAt = maincpp.find("startWallClockRescue()", objAt);
        CHECK(mainAt != std::string::npos);
        CHECK(guardAt != std::string::npos);
        CHECK(dtorAt != std::string::npos && dtorAt > guardAt);
        CHECK(joinAt != std::string::npos && joinAt < dtorAt + 80);
        CHECK(objAt != std::string::npos && objAt > guardAt);
        CHECK(rescueAt != std::string::npos && objAt < rescueAt);
        std::string prefix = maincpp.substr(mainAt, objAt - mainAt);
        std::string code;
        for (size_t i = 0; i < prefix.size();) {
            if (i + 1 < prefix.size() && prefix[i] == '/' && prefix[i + 1] == '/') {
                const auto nl = prefix.find('\n', i);
                if (nl == std::string::npos)
                    break;
                i = nl + 1;
                continue;
            }
            code.push_back(prefix[i++]);
        }
        CHECK(code.find("return") == std::string::npos);
    }

    std::printf("test_wall_clock: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
