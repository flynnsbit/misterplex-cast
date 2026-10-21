// SPS-gate layer 3, T1f + bar b1 (review 06:17): resolvePlayTarget's forceLadder against the fake PMS
// (tests/fixtures/fake_pms/fake_pms.py), in process. One case per run (the driver counts the fake PMS requests each
// case makes): tests/unit/test_resolve_gate_fallback.sh.
//
//   test_resolve_gate_fallback --base URL --helper URL --case C0|C1|C2|C3|C4
//
// Config (a) (pre-reg T1f): MPX_VIDEO_BACKEND=fpga-h264, MPX_FPGA_MODE=240p, the 240p weak profile, item 101
// (H.264, 24000/1001), MPX_H264_PROTOTYPE unset, the helper set: no conf term (idrLadder, ladder480, ntscLadder30,
// ladder240) selects the ladder.
//   C0 control, no forceLadder: PMS universal, never the ladder.
//   C1 forceLadder: the helper's /ladder.ts (w=320 h=240 fps=src hq=1), detail "gate fallback: <reason>; ...".
//   C2 forceLadder at a resume offset (n1): the ladder URL carries &offset_ms=754000.
//   C3 forceLadder with NO helper configured: PMS universal of the same item (never a
//      direct Part, never a hard refuse).
//   C4 a 480p conf with the helper and no forceLadder: ladder480 already routes the first play through the helper
//      (viaLadderProxy = GatePlayContext.helperStream, the code-packet finding).
#include "plex_resolve.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int fails = 0;

void check(bool cond, const std::string& c, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL resolve gate fallback %s: %s\n", c.c_str(), msg.c_str());
        ++fails;
    }
}

bool starts(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}

bool has(const std::string& s, const std::string& p) {
    return s.find(p) != std::string::npos;
}

const char* const kReason = "max_num_ref_frames 3 (limit 1)";

} // namespace

int main(int argc, char** argv) {
    std::string base, helper, c;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--base"))
            base = argv[i + 1];
        else if (!std::strcmp(argv[i], "--helper"))
            helper = argv[i + 1];
        else if (!std::strcmp(argv[i], "--case"))
            c = argv[i + 1];
    }
    if (base.empty() || helper.empty() || c.empty()) {
        std::fprintf(stderr, "usage: test_resolve_gate_fallback --base URL --helper URL --case C0..C4\n");
        return 2;
    }
    setenv("MPX_VIDEO_BACKEND", "fpga-h264", 1);
    unsetenv("MPX_H264_PROTOTYPE");
    unsetenv("REMOTE_LADDER_PROXY");
    std::string mode = "240p", weakName = "240p";
    bool force = false, proxy = true;
    int64_t off = 0;
    if (c == "C0") {
    } else if (c == "C1") {
        force = true;
    } else if (c == "C2") {
        force = true;
        off = 754000;
    } else if (c == "C3") {
        force = true;
        proxy = false;
    } else if (c == "C4") {
        mode = "480p";
        weakName = "480p";
    } else {
        std::fprintf(stderr, "test_resolve_gate_fallback: unknown case %s\n", c.c_str());
        return 2;
    }
    if (proxy)
        setenv("MPX_REMOTE_LADDER_PROXY", helper.c_str(), 1);
    else
        unsetenv("MPX_REMOTE_LADDER_PROXY");
    setenv("MPX_FPGA_MODE", mode.c_str(), 1);
    misterplex::WeakLadder weak;
    if (!misterplex::applyPlexTranscodeProfile(weakName, weak)) {
        std::fprintf(stderr, "test_resolve_gate_fallback: no weak profile %s\n", weakName.c_str());
        return 2;
    }
    const auto r = misterplex::resolvePlayTarget("/library/metadata/101", base, "tok", off, /*weakAlways=*/true,
                                                 weak, /*preferDirectH264=*/false, 0, 0, force, kReason);
    std::fflush(stderr);
    std::printf("CASE %s ok=%d via_ladder=%d transcoded=%d helper_configured=%d playable=%s detail=%s\n", c.c_str(),
                r.ok ? 1 : 0, r.viaLadderProxy ? 1 : 0, r.transcoded ? 1 : 0,
                misterplex::ladderProxyConfigured() ? 1 : 0, r.playable.c_str(), r.detail.c_str());
    const std::string ladder = helper + "/ladder.ts?src=";
    const std::string gate = std::string("gate fallback: ") + kReason + "; ";
    if (c == "C0") {
        check(r.ok && r.transcoded && !r.viaLadderProxy, c, "control resolves without the ladder");
        check(has(r.playable, "/video/:/transcode/universal/start"), c, "control takes PMS universal: " + r.playable);
        check(!starts(r.detail, "gate fallback"), c, "control detail is not a gate fallback: " + r.detail);
    } else if (c == "C1" || c == "C2") {
        check(r.ok && r.viaLadderProxy, c, "forceLadder resolves to the ladder helper");
        check(starts(r.playable, ladder), c, "forceLadder playable is the helper's /ladder.ts: " + r.playable);
        // 240p is an NTSC raster (fpgaRasterNtsc), so the proxy rung carries fps=src.
        check(has(r.playable, "&w=320&h=240&br=") && has(r.playable, "&fps=src") && has(r.playable, "&hq=1"),
              c, "the 240p rung for config (a): " + r.playable);
        check(starts(r.detail, gate + "lab ladder proxy 320x240@"), c, "detail starts 'gate fallback: ': " + r.detail);
        if (c == "C1")
            check(!has(r.playable, "offset_ms="), c, "no offset at a 0 start: " + r.playable);
        else
            check(has(r.playable, "&offset_ms=754000"), c, "the resume offset rides the ladder URL: " + r.playable);
    } else if (c == "C3") {
        check(r.ok && r.transcoded && !r.viaLadderProxy, c, "no helper: PMS universal, never a direct Part");
        check(has(r.playable, "/video/:/transcode/universal/start"), c,
              "no helper playable is PMS universal: " + r.playable);
        check(!has(r.playable, "/library/parts/"), c, "never a direct Part: " + r.playable);
        check(starts(r.detail, gate), c, "detail starts 'gate fallback: ': " + r.detail);
        check(has(r.detail, "PMS universal"), c, "detail names PMS universal: " + r.detail);
        check(!has(r.detail, "refusing (never a direct Part)"), c, "must not refuse: " + r.detail);
    } else if (c == "C4") {
        check(r.ok && r.viaLadderProxy && starts(r.playable, ladder) && has(r.playable, "&w=624&h=480"), c,
              "480p conf: the first play already rides the helper's pad ladder: " + r.playable);
    }
    if (fails) {
        std::fprintf(stderr, "test_resolve_gate_fallback %s: %d failure(s)\n", c.c_str(), fails);
        return 1;
    }
    std::printf("test_resolve_gate_fallback %s: OK\n", c.c_str());
    return 0;
}
