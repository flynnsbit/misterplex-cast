// IDLE-BLACK: g++ -std=c++17 -Ihost tests/unit/test_last_frame_hold_at_end.cpp && ./a.out   (repo root)
// Mutant: -DMUT_LFH_HOLD_ON_R1 (the hold also skips the idle paint on an armed R1 idle FB core) must fail.
// Replay of a lab MiSTer 03:10 CT leg (e9b526b5 on 0x487A1BD1 -> menu reload 0x48C51FF1, R1 idle FB armed, user
// stop): the end-of-session idle paint must run, as prod 4fdf97e0 does (chevron); the hold stays on elsewhere.
#include "libmisterplex/last_frame_hold.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
using misterplex::last_frame_hold::holdAtEnd;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count(const std::string& s, const std::string& n) {
    size_t c = 0;
    for (size_t i = s.find(n); i != std::string::npos; i = s.find(n, i + 1)) ++c;
    return c;
}
int main() {
    // a lab MiSTer replay: hold default-on (lastFrameHoldEnabled_{true}), R1 armed ("r1: idlefb path=devmem")
    const bool holdDefault = true, r1Armed = true;
    CHECK(!holdAtEnd(holdDefault, r1Armed));   // stop() paints idle -> chevron, not black
    CHECK(holdAtEnd(true, false));              // no R1 idle FB: the frame store keeps the last AU (c620 hold)
    CHECK(!holdAtEnd(false, false));
    CHECK(!holdAtEnd(false, true));
    const std::string cpp = slurp("arm/misterplexd/media_player.cpp");
    const std::string hpp = slurp("arm/misterplexd/media_player.hpp");
    CHECK(hpp.find("last_frame_hold::holdAtEnd(lastFrameHoldEnabled_.load(), r1Armed_.load())") != std::string::npos);
    CHECK(count(cpp, "!holdLastFrameAtEnd()") == 3);   // stop(), fpgaThreadMain EOF, threadMain session end
    CHECK(count(cpp, "if (!lastFrameHoldEnabled_.load()") == 0);
    CHECK(cpp.find("!shuttingDown_.load() && !lastFrameHoldEnabled_.load()") == std::string::npos);
    std::printf("test_last_frame_hold_at_end %d/%d\n", pass, fail);
    return fail ? 1 : 0;
}
