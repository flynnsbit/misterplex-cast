// GCAST P4: g++ -std=c++17 -Ihost tests/unit/test_gcast_osd.cpp && ./a.out   (from the repo root)
// Mutants: -DMUT_GOSD_NO_CAP (no title cap), -DMUT_GOSD_READY_FLAGS_OFF (a line with both flags off) must fail.
#include "libmisterplex/gcast_osd.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
using namespace misterplex::gcast_osd;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static void textChecks() {
    CHECK(gcastReadyLine(false, false).empty());   // flags off: nothing
    CHECK(gcastReadyLine(true, true) == "Ready: Plex | AirPlay | YouTube");
    CHECK(gcastReadyLine(true, false) == "Ready: Plex | AirPlay");
    CHECK(gcastReadyLine(false, true) == "Ready: Plex | YouTube");
    CHECK(gcastNowPlaying("YouTube", "Big Buck Bunny") == "YouTube: Big Buck Bunny");
    CHECK(gcastNowPlaying("AirPlay", "clip.mp4") == "AirPlay: clip.mp4");
    CHECK(gcastNowPlaying("Plex", "Movie") == "Movie");
    CHECK(gcastNowPlaying("", "Movie") == "Movie");
    const std::string longT(300, 'x');
    const std::string np = gcastNowPlaying("YouTube", longT);
    CHECK(np.size() == 60);
    CHECK(np.size() >= 3 && np.substr(np.size() - 3) == "...");
    CHECK(gcastNowPlaying("YouTube", "a\nb\x01" "c") == "YouTube: abc");
    CHECK(gcastNowPlaying("YouTube", "caf\xc3\xa9!") == "YouTube: caf?!");
}
static void stampChecks() {
    const int w = 640, h = 480;
    std::vector<uint8_t> f(w * h * 3, 0);
    CHECK(!stampReadyLineRgb24(f.data(), w, h, ""));
    bool any = false;
    for (auto v : f) any |= v != 0;
    CHECK(!any);
    CHECK(stampReadyLineRgb24(f.data(), w, h, gcastReadyLine(true, true)));
    int lit = 0, outside = 0, minx = w, maxx = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (f[(y * w + x) * 3]) {
                ++lit;
                if (y < h - 48 || y >= h - 32) ++outside;
                minx = std::min(minx, x); maxx = std::max(maxx, x);
            }
    CHECK(lit > 200);
    CHECK(outside == 0);
    CHECK(std::abs((minx + maxx) / 2 - w / 2) <= 8);
    CHECK(!stampReadyLineRgb24(nullptr, w, h, "x"));
    std::vector<uint8_t> tiny(8 * 8 * 3, 0);
    CHECK(!stampReadyLineRgb24(tiny.data(), 8, 8, "x"));   // no room: nothing drawn
    std::vector<uint8_t> narrow(64 * 64 * 3 + 16, 0x5a);
    std::fill(narrow.begin(), narrow.begin() + 64 * 64 * 3, 0);
    CHECK(stampReadyLineRgb24(narrow.data(), 64, 64, gcastReadyLine(true, true)));   // clipped
    bool guard = true;
    for (size_t i = 64 * 64 * 3; i < narrow.size(); ++i) guard &= narrow[i] == 0x5a;
    CHECK(guard);   // no overrun
}
static void wiringChecks() {
    const std::string m = slurp("arm/misterplexd/main.cpp"), mp = slurp("arm/misterplexd/media_player.cpp");
    CHECK(m.find("if (gcastOn) {   // P4") != std::string::npos);
    CHECK(m.find("std::atomic<bool> gcastYtUp") != std::string::npos);
    CHECK(m.find("gcastYtUp.load()") != std::string::npos);
    CHECK(m.find("gcastNowPlaying(ownerName(owner)") != std::string::npos);
    CHECK(mp.find("c.hint = readyLine;") != std::string::npos);
    CHECK(mp.find("stampReadyLineRgb24(frame.data()") != std::string::npos);
    CHECK(mp.find("rm == IdleMode::Logo || rm == IdleMode::Screensaver") != std::string::npos);
}
int main() {
    textChecks();
    stampChecks();
    wiringChecks();
    std::printf("gcast_osd %d/%d\n", pass, fail);
    return fail ? 1 : 0;
}
