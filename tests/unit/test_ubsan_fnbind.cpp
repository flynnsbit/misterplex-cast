// Host UBSan coverage for the ARM Thumb "reference binding to misaligned address"
// sites. On Thumb, std::function constructed from a raw function pointer binds a
// reference to an odd code address. These sites must wrap the function in a lambda.
// Constructing the objects under -fsanitize=undefined is the host half; the source
// scan fails if a raw bind is put back (that fault does not reproduce on x86).
#include "libmisterplex/d52_log.hpp"
#include "libmisterplex/f1_beat.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool has(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

int main() {
    const std::string root = MPX_SOURCE_ROOT;
    const std::string d52 = readFile(root + "/host/libmisterplex/d52_log.hpp");
    const std::string pms = readFile(root + "/arm/misterplexd/pms_timeline.cpp");
    const std::string beat = readFile(root + "/host/libmisterplex/f1_beat.hpp");
    const std::string player = readFile(root + "/arm/misterplexd/media_player.cpp");
    const std::string mph = readFile(root + "/arm/misterplexd/media_player.hpp");
    CHECK(!d52.empty() && !pms.empty() && !beat.empty() && !player.empty() && !mph.empty());
    CHECK(!has(d52, "Clock(nowStamp)"));
    CHECK(has(d52, "return nowStamp()"));
    CHECK(!has(pms, "sink_ = defaultSink"));
    CHECK(has(pms, "return defaultSink(req)"));
    CHECK(!has(beat, "f1Open = f1GateOpenNow"));
    CHECK(has(beat, "return f1GateOpenNow(snap, plxlPhys)"));
    CHECK(!has(mph, "BeatGate{f1::f1GateOpenNow"));
    CHECK(!has(player, ", owenb::openAvSource)"));
    CHECK(has(player, "return owenb::openAvSource(req, err)"));

    misterplex::d52::Config cfg;
    cfg.enabled = false;
    misterplex::d52::Logger logger(cfg);
    CHECK(!logger.armed());
    misterplex::f1::BeatGate gate;
    CHECK(static_cast<bool>(gate.f1Open));
    std::function<int()> wrapped = [] { return 1; };
    CHECK(wrapped() == 1);

    std::printf("test_ubsan_fnbind: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
