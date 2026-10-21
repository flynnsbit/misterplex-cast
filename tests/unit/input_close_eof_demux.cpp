// N2 driver: opens URL with the daemon's AvInprocDecoder (compressed video, the shared demux)
// and reads it to the end as misterplexd does: AUs on this thread, and with AUDIO=1 the PCM
// pump (advanceCompressedAudio + drainPcm, media_player.cpp fpgaAudioPump) on another. Prints
// one line: how each reader ended, AUs read, EIO-to-EOF conversions and the short ms.
// usage: input_close_eof_demux URL EXPECTED_DURATION_MS SESSION_START_MS AUDIO(0|1) [DEMUX_START_MS]
// DEMUX_START_MS is opts.startMs: the demux seeks itself (a URL without a baked-in offset).
#include "libmisterplex/av_inproc_decode.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace misterplex;

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: input_close_eof_demux URL EXPECTED_MS START_MS AUDIO [DEMUX_START_MS]\n");
        return 2;
    }
    AvInprocOpenOpts o;
    o.compressedVideo = true;
    o.decodeAudio = std::atoi(argv[4]) != 0;
    o.allowInter = true;
    o.allowDeblock = true;
    o.expectW = 640;
    o.expectH = 480;
    o.expectedDurationMs = std::atoll(argv[2]);
    o.sessionStartMs = std::atoll(argv[3]);
    if (argc == 6)
        o.startMs = std::atoll(argv[5]);
    std::atomic<bool> cancelled{false};
    o.cancelled = &cancelled;
    AvInprocDecoder d;
    std::string err;
    if (!d.open(argv[1], o, err)) {
        std::printf("result=open-failed aus=0 audio=none conversions=0 short_ms=none "
                    "session_ms=none err=[%s] audio_err=[]\n", err.c_str());
        return 0;
    }
    std::atomic<bool> stop{false}, audioDone{!o.decodeAudio};
    std::string audioState = o.decodeAudio ? "running" : "none", audioErr;
    std::thread pump;
    if (o.decodeAudio) {
        pump = std::thread([&] {
            uint8_t buf[16384];
            while (!stop.load()) {
                if (d.drainPcm(buf, sizeof buf, false) > 0)
                    continue;
                std::string e;
                const AvAudioProgress p = d.advanceCompressedAudio(e);
                if (p == AvAudioProgress::Eof) {
                    while (d.drainPcm(buf, sizeof buf, false) > 0) {
                    }
                    audioState = "eof";
                    break;
                }
                if (p == AvAudioProgress::Error) {
                    audioState = "error";
                    audioErr = e;
                    break;
                }
                if (p == AvAudioProgress::Cancelled) {
                    audioState = "cancelled";
                    break;
                }
                if (p != AvAudioProgress::Ready)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            audioDone.store(true);
        });
    }
    AvCompressedAccessUnit au;
    long aus = 0;
    int r;
    while ((r = d.readAccessUnit(au, err)) > 0)
        ++aus;
    // The other reader gets its own end (F4: a sticky EOF, never a fresh EIO); bounded wait.
    for (int i = 0; i < 1000 && !audioDone.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true);
    if (!audioDone.load())
        d.requestStop();
    if (pump.joinable())
        pump.join();
    const int64_t shortMs = d.inputClosedAtEndShortMs();
    const AvCompressedDiagnostics diag = d.compressedDiagnostics();
    std::printf("result=%s aus=%ld audio=%s conversions=%u short_ms=%s session_ms=%s err=[%s] "
                "audio_err=[%s]\n",
                r == 0 ? "eof" : "error", aus, audioState.c_str(), d.inputCloseConversions(),
                shortMs == kNoInputCloseAtEnd ? "none" : std::to_string(shortMs).c_str(),
                diag.detailsAvailable && diag.inputSessionMs >= 0
                    ? std::to_string(diag.inputSessionMs).c_str() : "none",
                r == 0 ? "" : err.c_str(), audioErr.c_str());
    d.close();
    return 0;
}
