// C704 / review-296 r0: TTFP complete needs all four stamps; stalls log incomplete.
// On 60bb5b52ab this file is absent and ttfpTryComplete/formatTtfpLog do not exist.
#include "media_player.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace misterplex;

int g_fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++g_fails; std::printf("FAIL %s\n", msg); } } while (0)

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return {};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static std::string repoFile(const char* rel) {
    std::ifstream cwd(rel);
    if (cwd)
        return rel;
    std::string dir = __FILE__;
    for (int i = 0; i < 6; ++i) {
        const auto slash = dir.find_last_of('/');
        if (slash == std::string::npos)
            break;
        dir = dir.substr(0, slash);
        const std::string cand = dir + "/" + rel;
        std::ifstream f(cand);
        if (f)
            return cand;
    }
    return rel;
}

int main() {
    TtfpTimestamps a{};
    a.startUs = 1000; a.firstPacketUs = 4000; a.firstDecodedUs = 5000; a.firstPresentedUs = 8000;
    ttfpTryComplete(a);
    CHECK(a.complete, "all four -> complete");
    CHECK(a.phase1Us == 3000, "phase1");
    CHECK(a.phase2Us == 1000, "phase2");
    CHECK(a.phase3Us == 3000, "phase3");
    const std::string s = formatTtfpLog(a);
    CHECK(s.find("TTFP complete") != std::string::npos, "log complete");
    CHECK(s.find("TTFP incomplete") == std::string::npos, "complete log is not incomplete");
    CHECK(s.find("phase1=3000") != std::string::npos, "log phase1");
    CHECK(s.find("first_picture_ms=7") != std::string::npos, "first_picture_ms is (8000-1000)/1000");
    CHECK(s.find(" us") != std::string::npos, "absolute stamps stay microseconds");

    TtfpTimestamps b{};
    b.startUs = 1000; b.firstPacketUs = 4000; // decoded/presented still 0
    ttfpTryComplete(b);
    CHECK(!b.complete, "missing decoded/presented -> incomplete");
    CHECK(b.phase1Us == 0 && b.phase2Us == 0 && b.phase3Us == 0, "no phases if incomplete");
    const std::string t = formatTtfpLog(b);
    CHECK(t.find("TTFP incomplete") != std::string::npos, "log incomplete");
    CHECK(t.find("first_decoded=0") != std::string::npos, "decoded stays 0");

    // Finding 2: present without packet/decoded must not claim complete or emit 0-startUs phases.
    TtfpTimestamps c{};
    c.startUs = 1000;
    c.firstPresentedUs = 8000;
    ttfpTryComplete(c);
    CHECK(!c.complete, "present without packet/decoded -> incomplete");
    CHECK(c.phase1Us == 0 && c.phase2Us == 0 && c.phase3Us == 0, "no negative phases if packet/decoded unset");

    TtfpTimestamps d{};
    d.startUs = 1000;
    d.firstPacketUs = 4000;
    d.firstPresentedUs = 8000; // decoded still 0
    ttfpTryComplete(d);
    CHECK(!d.complete, "present without decoded -> incomplete");
    CHECK(d.phase2Us == 0 && d.phase3Us == 0, "no phases from a zero decoded stamp");

    const std::string hpp = slurp(repoFile("arm/misterplexd/media_player.hpp"));
    const std::string cpp = slurp(repoFile("arm/misterplexd/media_player.cpp"));
    CHECK(!hpp.empty() && !cpp.empty(), "slurp media_player sources");
    CHECK(hpp.find("play() entry (wall)") == std::string::npos, "startUs comment is not wall/play() entry");
    CHECK(hpp.find("first decoded frame") == std::string::npos, "firstDecodedUs is not 'first decoded frame'");
    {
        const auto dec = hpp.find("firstDecodedUs");
        CHECK(dec != std::string::npos, "firstDecodedUs in hpp");
        if (dec != std::string::npos) {
            const auto lineEnd = hpp.find('\n', dec);
            const std::string line = hpp.substr(dec, lineEnd == std::string::npos ? std::string::npos : lineEnd - dec);
            CHECK(line.find("submitted") != std::string::npos || line.find("FPGA ring") != std::string::npos,
                  "firstDecodedUs comment names submit/FPGA ring");
        }
    }
    CHECK(hpp.find("ttfpTryComplete") != std::string::npos, "ttfpTryComplete in hpp");
    CHECK(hpp.find("firstPacketUs && t.firstDecodedUs && t.firstPresentedUs") != std::string::npos ||
              hpp.find("t.firstPacketUs && t.firstDecodedUs && t.firstPresentedUs") != std::string::npos,
          "complete requires all four stamps");

    CHECK(cpp.find("if (ttfp.complete)") == std::string::npos, "stop() no longer gates the log on complete");
    CHECK(cpp.find("formatTtfpLog") != std::string::npos, "cpp uses formatTtfpLog");
    {
        const auto fail = cpp.find("void MediaPlayer::failFpgaSession");
        CHECK(fail != std::string::npos, "failFpgaSession exists");
        if (fail != std::string::npos) {
            const auto window = cpp.substr(fail, 4000);
            CHECK(window.find("formatTtfpLog") != std::string::npos, "failFpgaSession dumps TTFP");
            CHECK(window.find("!ttfp_.logged") != std::string::npos, "failFpgaSession skips an already-logged session");
        }
    }
    {
        const auto clear = cpp.find("media: gate-error stream clear=");
        CHECK(clear != std::string::npos, "gate-error path clears the stream");
        if (clear != std::string::npos) {
            const auto window = cpp.substr(clear > 600 ? clear - 600 : 0, 800);
            const auto end = window.rfind("plxhPolicy_.endSession()");
            const auto entry = window.find("r1EntryStreamClear(&why)");
            CHECK(end != std::string::npos && entry != std::string::npos && end < entry,
                  "gate-error clear ends the PLXH session before the stream clear");
        }
    }
    {
        const auto fail = cpp.find("void MediaPlayer::failFpgaSession");
        const auto note = cpp.find("noteGateRefuse();", fail == std::string::npos ? 0 : fail);
        CHECK(fail != std::string::npos && note != std::string::npos && note > fail && note - fail < 1200,
              "failFpgaSession latches a gate refuse");
        const auto main = slurp(repoFile("arm/misterplexd/main.cpp"));
        CHECK(main.find("matrix-start reload prod") != std::string::npos,
              "doPlay reloads prod to Plex_480p before PLAY");
        CHECK(main.find("teardownAfterGateRefuse") != std::string::npos,
              "the next play tears down a gate refuse before resolve");
    }
    {
        const auto stop = cpp.find("MediaPlayer::StoppedPosition MediaPlayer::stop()");
        CHECK(stop != std::string::npos, "stop() exists");
        if (stop != std::string::npos) {
            const auto window = cpp.substr(stop, 8000);
            CHECK(window.find("formatTtfpLog") != std::string::npos, "stop() dumps TTFP");
            CHECK(window.find("!ttfp_.logged") != std::string::npos, "stop() skips an already-logged session");
        }
    }
    {
        const auto play = cpp.find("ttfp_ = TtfpTimestamps{}");
        CHECK(play != std::string::npos, "playInternal resets ttfp_");
        if (play != std::string::npos) {
            const auto before = cpp.rfind("!ttfp_.logged", play);
            CHECK(before != std::string::npos && play - before < 400, "playInternal logs unlogged prior session before reset");
        }
    }
    {
        const auto push = cpp.find("pending.push_back");
        const auto dec = (push == std::string::npos) ? std::string::npos : cpp.find("firstDecodedUs", push);
        CHECK(push != std::string::npos && dec != std::string::npos && dec > push && dec - push <= 800,
              "firstDecodedUs stays at FPGA-ring submit (pending.push_back)");
    }

    if (g_fails) { std::printf("%d FAIL\n", g_fails); return 1; }
    std::printf("PASS\n");
    return 0;
}
