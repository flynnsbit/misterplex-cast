// Gate 8: Ready advertises YouTube only after Lounge connects, and a failed
// connect (DNS not up at boot) keeps retrying with backoff instead of stopping.
#include "gcast/lounge_ready.hpp"
#include "libmisterplex/gcast_osd.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using misterplex::gcast::loungeAdvertisesYouTube;
using misterplex::gcast::loungeConnectBackoffMs;
using misterplex::gcast_osd::gcastReadyLine;

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

static std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void readyLine() {
    CHECK(!loungeAdvertisesYouTube(false));
    CHECK(loungeAdvertisesYouTube(true));
    // Boot, Lounge still down: Plex (and AirPlay) only.
    CHECK(gcastReadyLine(true, loungeAdvertisesYouTube(false)) == "Ready: Plex | AirPlay");
    CHECK(gcastReadyLine(false, loungeAdvertisesYouTube(false)).empty());
    // Lounge bind succeeded: YouTube joins the line.
    CHECK(gcastReadyLine(true, loungeAdvertisesYouTube(true)) == "Ready: Plex | AirPlay | YouTube");
    CHECK(gcastReadyLine(false, loungeAdvertisesYouTube(true)) == "Ready: Plex | YouTube");
}

static void backoffKeepsRetrying() {
    CHECK(loungeConnectBackoffMs(0) == 1000);
    CHECK(loungeConnectBackoffMs(1) == 2000);
    CHECK(loungeConnectBackoffMs(2) == 4000);
    CHECK(loungeConnectBackoffMs(3) == 8000);
    CHECK(loungeConnectBackoffMs(4) == 16000);
    CHECK(loungeConnectBackoffMs(5) == 30000);
    CHECK(loungeConnectBackoffMs(6) == 30000);
    CHECK(loungeConnectBackoffMs(40) == 30000);
    int prev = 0;
    for (int failures = 0; failures < 12; ++failures) {
        const int delay = loungeConnectBackoffMs(failures);
        CHECK(delay > 0);
        CHECK(delay <= 30000);
        if (failures > 0 && prev < 30000)
            CHECK(delay > prev);
        prev = delay;
    }
}

static void daemonUsesIt() {
    const std::string fe = slurp("arm/misterplexd/gcast/yt_fe.cpp");
    const std::string main = slurp("arm/misterplexd/main.cpp");
    CHECK(fe.find("loungeConnectBackoffMs(failures)") != std::string::npos);
    CHECK(fe.find("loungeUp_.store(true)") != std::string::npos);
    CHECK(fe.find("yt: SSDP ignored; Lounge not connected") != std::string::npos);
    CHECK(fe.find("yt: DIAL ignored; Lounge not connected") != std::string::npos);
    CHECK(fe.find("ctx.youtubeReady = loungeUp_.load()") != std::string::npos);
    CHECK(fe.find("dial::youtubeAppPath") != std::string::npos);
    CHECK(main.find("ytFe.loungeConnected()") != std::string::npos);
    CHECK(main.find("GYT play dropped; Lounge not connected") != std::string::npos);
    CHECK(main.find("std::atomic<bool> gcastYtUp") != std::string::npos);
    CHECK(main.find("gcastYtUp.load()") != std::string::npos);
    const std::string player = slurp("arm/misterplexd/media_player.cpp");
    CHECK(player.find("setIdleHint") != std::string::npos);
    CHECK(player.find("idleCfg_.hint") != std::string::npos ||
          slurp("host/libmisterplex/owenb_play.cpp").find("idleCfg_.hint = hint") != std::string::npos);
}

int main() {
    readyLine();
    backoffKeepsRetrying();
    daemonUsesIt();
    std::printf("test_lounge_ready: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
