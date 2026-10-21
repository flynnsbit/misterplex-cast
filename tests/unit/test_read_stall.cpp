// WAN read-stall bound (a lab MiSTer 1011-cp-cutoff-playbar run-A, High ed77fa3e, a remote PMS):
// every remote transcode session died with "compressed audio demux: Immediate exit requested"
// (input_read_code AVERROR_EXIT, io_bytes_read 155154 / 110426 / ..., input_session_ms 3878 / 5462) = the
// demux's own 5 s in-session read deadline firing on a PMS pause after its first burst. The same signature
// hit prod-line builds on a remote PMS WITH acceptWav/mediaBufferSize (F2 121618 B, B1 250466 B, J2 36194 B),
// so it is not the buffer-param drop, the bitrate cap or the stream id.
//   R1 LAN / ladder proxy / file URLs keep 5000 ms (unchanged).
//   R2 the run-A WAN start.mp4 URL gets 20000 ms (= the open's rw_timeout).
//   R3 MPX_REMOTE_READ_STALL_MS override 2000..60000 on WAN only; junk keeps 20000.
//   R4 wiring: media_player sets opts.readStallMs from compressedReadStallMs(url); the decoder has no literal
//      5000 ms read deadline left.
// Mutant: MUT_WAN_READ_STALL_5S dies at R2.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "libmisterplex/av_inproc_decode.hpp"

using namespace misterplex;
namespace {
int fails = 0;
void expect(bool c, const std::string& m) {
    if (!c) { std::fprintf(stderr, "FAIL read stall: %s\n", m.c_str()); ++fails; }
}
std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
const char* kRunA =
    "http://pms.example:32401/video/:/transcode/universal/start.mp4?hasMDE=1&path=%2Flibrary%2Fmetadata%2F11681"
    "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0&subtitleSize=100&audioBoost=100"
    "&location=wan&autoAdjustQuality=0&session=mplex-1c5a5c00&videoQuality=40&videoResolution=624x264"
    "&maxVideoBitrate=2500&X-Plex-Token=R";
}  // namespace

int main() {
    unsetenv("MPX_REMOTE_READ_STALL_MS");
    // R1
    for (const char* lan : {"http://192.0.2.24:32400/video/:/transcode/universal/start.mp4?location=lan&copyts=1",
                            "http://192.0.2.21:18767/ladder.ts?src=x&w=624&h=480&br=2500", "/media/fat/x.ts"})
        expect(compressedReadStallMs(lan) == 5000, std::string("R1 LAN/proxy/file keeps 5000 ms: ") + lan);
    // R2
    expect(compressedReadStallMs(kRunA) == 20000, "R2 WAN universal transcode gets 20000 ms (got " +
                                                      std::to_string(compressedReadStallMs(kRunA)) + ")");
    // R3
    setenv("MPX_REMOTE_READ_STALL_MS", "30000", 1);
    expect(compressedReadStallMs(kRunA) == 30000, "R3 env 30000 on WAN");
    expect(compressedReadStallMs("http://192.0.2.21:18767/ladder.ts") == 5000, "R3 env never changes LAN");
    for (const char* bad : {"1000", "90000", "20s", ""}) {
        setenv("MPX_REMOTE_READ_STALL_MS", bad, 1);
        expect(compressedReadStallMs(kRunA) == 20000, std::string("R3 junk env -> 20000: '") + bad + "'");
    }
    unsetenv("MPX_REMOTE_READ_STALL_MS");
    // R4
    const std::string mp = slurp(MPX_ROOT "/arm/misterplexd/media_player.cpp");
    const std::string dec = slurp(MPX_ROOT "/arm/misterplexd/av_inproc_decode.cpp");
    expect(mp.find("opts.readStallMs = compressedReadStallMs(url);") != std::string::npos,
           "R4 media_player wires compressedReadStallMs(url) into the compressed open");
    expect(dec.find("monotonicMs() + 5000") == std::string::npos && dec.find("monotonicMs() + readStallMs_") != std::string::npos,
           "R4 decoder read deadlines use readStallMs_");
    std::printf("test_read_stall: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
