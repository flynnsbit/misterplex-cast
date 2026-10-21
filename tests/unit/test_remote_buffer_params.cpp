// Remote transcode start/decision shape (fix/remote-buffer-playbar-1011): the wan start and decision carry no
// acceptWav/mediaBufferSize (the 2026-09-26 "&acceptWav=1&mediaBufferSize=102400": 102400 KiB at 2500 kbps ~ 335 s,
// a lab MiSTer remote EOFs at 330.6/335.6 s). MPX_REMOTE_MEDIA_BUFFER_KB re-adds mediaBufferSize only (A/B knob).
// LAN shape unchanged. Mutant MUT_REMOTE_BUFFER_102400 fails "B1".
#include "plex_resolve.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace misterplex;
static int fails = 0;
static void expect(bool c, const std::string& m) { if (!c) { std::fprintf(stderr, "FAIL remote buffer: %s\n", m.c_str()); ++fails; } }
int main() {
    WeakLadder w;
    w.profileName = "480p";
    w.videoResolution = "572x428";
    w.maxVideoBitrateKbps = 2500;
    const std::string wan = "http://203.0.113.47:32401";
    unsetenv("MPX_REMOTE_MEDIA_BUFFER_KB");
    const std::string start = buildUniversalTranscodeUrl(wan, "/library/metadata/11681", "tok", "mplex-1", 0, w);
    const std::string dec = buildUniversalDecisionUrl(start, "mplex-1", "tok", w);
    expect(start.find("mediaBufferSize") == std::string::npos && start.find("acceptWav") == std::string::npos &&
           dec.find("mediaBufferSize") == std::string::npos && dec.find("acceptWav") == std::string::npos,
           "B1 wan start/decision carry no mediaBufferSize/acceptWav");
    expect(start.find("location=wan") != std::string::npos && dec.find("location=wan") != std::string::npos,
           "B2 wan start/decision keep location=wan");
    setenv("MPX_REMOTE_MEDIA_BUFFER_KB", "51200", 1);
    const std::string ab = buildUniversalTranscodeUrl(wan, "/library/metadata/11681", "tok", "mplex-1", 0, w);
    expect(ab.find("&mediaBufferSize=51200") != std::string::npos && ab.find("acceptWav") == std::string::npos,
           "B3 MPX_REMOTE_MEDIA_BUFFER_KB=51200 re-adds only mediaBufferSize");
    setenv("MPX_REMOTE_MEDIA_BUFFER_KB", "junk", 1);
    expect(buildUniversalTranscodeUrl(wan, "/library/metadata/1", "tok", "s", 0, w).find("mediaBufferSize") ==
               std::string::npos, "B3 a bad knob value adds nothing");
    unsetenv("MPX_REMOTE_MEDIA_BUFFER_KB");
    const std::string lan = buildUniversalTranscodeUrl("http://127.0.0.24:32400", "/library/metadata/1", "tok", "s", 0, w);
    expect(lan.find("location=lan") != std::string::npos && lan.find("copyts=1") != std::string::npos &&
           lan.find("mediaBufferSize") == std::string::npos, "B4 LAN start unchanged");
    std::printf("test_remote_buffer_params: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
