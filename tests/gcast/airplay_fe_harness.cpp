// GCAST P2 harness: the real airplay_fe (MPX_HAVE_AIRPLAY) on a host, with a fake player that
// answers progress like MediaPlayer. Drive it with tools/gcast/ios_like_sender.py.
//   airplay_fe_harness NAME PORT   (prints SINK lines; SIGINT to stop)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "gcast/airplay_fe.hpp"
#include "gcast/session_arbiter.hpp"

using namespace misterplex::gcast;
static std::atomic<bool> g_run{true};
int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { g_run = false; });
    std::signal(SIGTERM, [](int) { g_run = false; });
    std::signal(SIGPIPE, SIG_IGN);
    SessionArbiter arb;
    AirplayFrontend fe;
    std::atomic<int64_t> pos{0}, dur{60000};
    std::atomic<bool> playing{false};
    arb.setRetire(Owner::AirPlay, [&](Owner) { fe.snapshot().ended(); });
    arb.setProgress(Owner::AirPlay, [&](const std::string& st, int64_t t, int64_t d) { fe.snapshot().progress(st, t, d); });
    AirplayConfig cfg;
    cfg.name = argc > 1 ? argv[1] : "MiSTer-gcast-harness";
    cfg.port = static_cast<uint16_t>(argc > 2 ? std::atoi(argv[2]) : 7000);
    cfg.audioHost = std::getenv("AP_AUDIO_HOST") ? std::getenv("AP_AUDIO_HOST") : "127.0.0.1";   // RAOP /raop.wav
    if (const char* p = std::getenv("AP_AUDIO_PORT"))
        cfg.audioPort = static_cast<uint16_t>(std::atoi(p));
    if (!fe.start(cfg, [&](const ApEvent& e) {
            std::printf("SINK type=%d url=%s value=%.3f\n", static_cast<int>(e.type), e.url.c_str(), e.value);
            std::fflush(stdout);
            if (e.type == ApEventType::Play) {
                arb.claim(Owner::AirPlay);
                pos = static_cast<int64_t>(e.value * 1000);
                playing = true;
                arb.dispatchProgress("playing", pos, dur);
            } else if (e.type == ApEventType::Scrub) {
                pos = static_cast<int64_t>(e.value * 1000);
            } else if (e.type == ApEventType::Rate) {
                playing = e.value != 0;
                arb.dispatchProgress(playing ? "playing" : "paused", pos, dur);
            } else {
                playing = false;
                arb.dispatchProgress("stopped", pos, dur);
            }
        })) {
        std::printf("start failed\n");
        return 1;
    }
    while (g_run) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (playing) {
            pos += 250;
            if (pos >= dur) {
                playing = false;
                arb.dispatchProgress("ended", dur, dur);
                std::printf("SINK natural-end\n");
                std::fflush(stdout);
            } else {
                arb.dispatchProgress("playing", pos, dur);
            }
        }
    }
    fe.stop();
    return 0;
}
