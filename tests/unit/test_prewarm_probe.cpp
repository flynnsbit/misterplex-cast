// MPX_PREWARM (lead a lab MiSTer C1 1011: 2 of 2 "remote universal start dead http=0" with MPX_PREWARM=1,
// fine without). Cause: plexHttpGetStatusAndBody read curl's output with fgets + operator+=(char*),
// which stops at the first NUL of each chunk; the MPEG-TS body is full of NULs, so the trailing
// -w '%{http_code}' digits were lost (status 0) and the body was truncated.
//   W1 on by default: unset and "1" are the single GET; "" and "0" keep the two-GET drain.
//   W2 a 4096-byte TS-like body (NULs, 0x47 sync) followed by "206": status 206, body 4096 bytes exact.
//   W3 a short "<html>" 200 body still reads as 200 with that body (dead-transcoder detection unchanged).
// Mutant MUT_PREWARM_FGETS (the old read) FAILs at W2.
#include "plex_resolve.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace misterplex;

namespace {
int fails = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) { ++fails; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
std::string dir;
void shim(const std::string& bodyFile, const char* code) {
    std::ofstream f(dir + "/curl");
    f << "#!/bin/sh\ncat '" << bodyFile << "'\nprintf '" << code << "'\n";
    f.close();
    chmod((dir + "/curl").c_str(), 0755);
}
} // namespace

int main() {
    unsetenv("MPX_PREWARM");
    expect(plexPrewarmProbeEnabled(), "W1 MPX_PREWARM unset is the single GET");
    setenv("MPX_PREWARM", "", 1);
    expect(!plexPrewarmProbeEnabled(), "W1 MPX_PREWARM= is off");
    setenv("MPX_PREWARM", "0", 1);
    expect(!plexPrewarmProbeEnabled(), "W1 MPX_PREWARM=0 is off");
    setenv("MPX_PREWARM", "1", 1);
    expect(plexPrewarmProbeEnabled(), "W1 MPX_PREWARM=1 is on");
    unsetenv("MPX_PREWARM");

    char tmpl[] = "/tmp/pwshimXXXXXX";
    dir = mkdtemp(tmpl);
    setenv("PATH", (dir + ":" + std::getenv("PATH")).c_str(), 1);
    {
        std::string ts(4096, '\0');
        for (size_t i = 0; i < ts.size(); i += 188) ts[i] = 0x47;
        for (size_t i = 5; i < ts.size(); i += 7) ts[i] = static_cast<char>(i & 0xff);
        std::ofstream(dir + "/ts.bin", std::ios::binary).write(ts.data(), ts.size());
        shim(dir + "/ts.bin", "206");
        const PlexStartProbe p = plexHttpGetStatusAndBody(
            "http://203.0.113.47:32401/video/:/transcode/universal/start.mp4?session=s", {}, "MiSTerPlex-High", 12);
        expect(p.httpStatus == 206, "W2 TS body + 206: status " + std::to_string(p.httpStatus));
        expect(p.body.size() == 4096 && p.body == ts, "W2 TS body exact (" + std::to_string(p.body.size()) + " bytes)");
    }
    {
        std::ofstream(dir + "/h.html") << "<html>Bad Request</html>";
        shim(dir + "/h.html", "200");
        const PlexStartProbe p = plexHttpGetStatusAndBody("http://203.0.113.47:32401/x", {}, "MiSTerPlex", 12);
        expect(p.httpStatus == 200 && p.body == "<html>Bad Request</html>", "W3 html 200 body");
    }
    std::system(("rm -rf '" + dir + "'").c_str());
    std::printf("test_prewarm_probe: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
