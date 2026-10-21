// Host harness: runs YtFrontend (SSDP/DIAL/Lounge) with a fake player that
// reports progress like misterplexd's arbiter would.  Lab use only.
//   yt_fe_harness NAME DIALPORT CAFILE [SCREENIDFILE]
#include "gcast/yt_fe.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace misterplex::gcast;
static std::atomic<bool> g_stop{false};
int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s NAME DIALPORT CAFILE [SCREENIDFILE]\n", argv[0]); return 2; }
    std::signal(SIGPIPE, SIG_IGN);   // as misterplexd main.cpp: SSL_write/SSL_shutdown on a closed peer
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    YtFrontend fe;
    std::mutex m; bool playing = false, active = false; int64_t pos = 0; const int64_t dur = 30000;
    YtConfig c; c.name = argv[1]; c.hwSeed = "harness"; c.dialPort = uint16_t(atoi(argv[2])); c.caFile = argv[3];
    if (argc > 4) c.screenIdFile = argv[4];
    bool ok = fe.start(c, [&](const YtCommand& cmd) {
        std::lock_guard<std::mutex> lk(m);
        switch (cmd.kind) {
        case YtCmd::Play: active = playing = true; pos = cmd.ms; std::printf("PLAYER play %s @%lld\n", cmd.videoId.c_str(), (long long)cmd.ms); break;
        case YtCmd::Seek: pos = cmd.ms; std::printf("PLAYER seek %lld\n", (long long)cmd.ms); break;
        case YtCmd::Pause: playing = false; std::printf("PLAYER pause\n"); break;
        case YtCmd::Resume: playing = true; std::printf("PLAYER resume\n"); break;
        case YtCmd::Stop: active = playing = false; std::printf("PLAYER stop\n"); break;
        }
        std::fflush(stdout);
    });
    if (!ok) { std::fprintf(stderr, "start failed\n"); return 1; }
    const int stopAt = std::getenv("HARNESS_STOP_AT") ? std::atoi(std::getenv("HARNESS_STOP_AT")) : 0;
    int secs = 0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        bool end = false; std::string st; int64_t p;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!active) continue;
            if (playing) pos += 1000;
            if (pos >= dur) { active = playing = false; end = true; }
            st = playing ? "playing" : "paused"; p = pos;
        }
        if (stopAt > 0 && ++secs == stopAt) {   // like a preemption / player error
            { std::lock_guard<std::mutex> lk(m); active = playing = false; }
            std::printf("PLAYER aborted\n"); std::fflush(stdout); fe.ended(false); continue;
        }
        if (end) { std::printf("PLAYER natural end\n"); std::fflush(stdout); fe.ended(true); }
        else fe.report(st, p, dur);
    }
    fe.stop();
    return 0;
}
