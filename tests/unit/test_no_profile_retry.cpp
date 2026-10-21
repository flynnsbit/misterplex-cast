// NO-REQUIRED-PROFILE (docs/plans/NO-REQUIRED-PROFILE.md): decision/SPS mismatch -> one lowered retry ->
// clean fallback.
//   P1 decision check: a Baseline-core decision reporting High / too wide / 10-bit / refs 3 is a mismatch;
//      a baseline 624x480 one fits; the High ladder takes main/high but not refs 5 or level 40.
//   P2 lowered request: bitrate down a step above the tier floor, quality <= 30, pmsLowered, and the Extra
//      gains isRequired transcode-target refFrames/bitDepth/size bounds.
//   P3 ensureUniversalDecision (curl shim, a mismatching PMS): the first decision refuses with the reason
//      (takeDecisionMismatch); the lowered one proceeds; exactly 2 decisions, the 2nd with the strict Extra.
//   R1 NVENC-ignores-refs replay (review C2 20:16 CT, home PMS h264_nvenc: "max_num_ref_frames 3 (limit 1)"
//      on core 0x48FF65F2, no ladder helper): the gate takes the lowered PMS retry (R6p); the lowered play's
//      refusal is the on-screen R7 error (never a silent stop); with a helper configured R6 (ladder) wins.
// Mutants: MUT_DECISION_CHECK_OFF FAILs at P1; MUT_DECISION_PROFILE_FALLBACK (35c510937's Media@videoProfile
// fallback) FAILs at P1r; MUT_NO_LOWERED_RETRY FAILs at R1.
#include "plex_resolve.hpp"
#include "libmisterplex/gate_owner.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace misterplex;

namespace {
int fails = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) { ++fails; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
std::string xmlFor(const std::string& streamAttrs) {
    return "<MediaContainer generalDecisionCode=\"1000\"><Metadata><Media videoCodec=\"h264\"><Part decision=\"transcode\">"
           "<Stream streamType=\"1\" " + streamAttrs + " decision=\"transcode\"/>"
           "<Stream streamType=\"2\" codec=\"aac\" channels=\"2\"/></Part></Media></Metadata></MediaContainer>";
}
std::string realDecision;
WeakLadder baseline480() {
    WeakLadder w;
    std::string why;
    applyFpgaPlexProfile(w, "ip", 24000, 1001, true, &why, "480p", true);
    return w;
}
} // namespace

int main() {
    const WeakLadder b = baseline480();
    // P1: only codec and output size are checked (Media@videoProfile is the source's; profile/refs/level/bit depth
    // are the SPS gate's).
    {
        expect(!decisionCoreMismatch(parseDecisionVideo(xmlFor("codec=\"h264\" width=\"1280\" height=\"720\"")), b).empty(),
               "P1 decision 1280x720 is a mismatch");
        expect(!decisionCoreMismatch(parseDecisionVideo(xmlFor("codec=\"hevc\"")), b).empty(), "P1 decision hevc is a mismatch");
        expect(decisionCoreMismatch(parseDecisionVideo("<MediaContainer generalDecisionCode=\"1000\"/>"), b).empty(),
               "P1 no video fields: nothing to check (the SPS gate decides)");
        expect(parseDecisionVideo(xmlFor("codec=\"h264\"") + "<TranscodeSession transcodeHwEncoding=\"nvenc\"/>").hwEncoding == "nvenc",
               "P1 transcodeHwEncoding parsed when present");
    }
    // P1r: the real home-PMS variant-run decision (lab-runs/home-pms-hwenc-1011/variant-key6, 2026-10-11 21:32 CT):
    // Media videoProfile="high" (the source), output 624x468 Constrained Baseline/CAVLC/refs 1. No mismatch.
    {
        std::ifstream f(std::string(MPX_FIXTURES) + "/pms_decisions/home-pms-variant-key6-decision.xml");
        std::stringstream ss; ss << f.rdbuf();
        const std::string real = ss.str();
        expect(real.find("videoProfile=\"high\"") != std::string::npos, "P1r fixture loaded");
        const DecisionVideo dv = parseDecisionVideo(real);
        expect(dv.found && dv.codec == "h264" && dv.width == 624 && dv.height == 468, "P1r parsed h264 624x468");
        expect(decisionCoreMismatch(dv, b).empty(), "P1r real variant decision: no mismatch (got '" +
                                                     decisionCoreMismatch(dv, b) + "')");
        realDecision = real;
    }
    {
        expect(decisionCoreMismatch(parseDecisionVideo(xmlFor("codec=\"h264\" profile=\"high\" level=\"40\" refFrames=\"4\" bitDepth=\"10\" width=\"624\" height=\"468\"")), b).empty(),
               "P1 profile/level/refs/bitDepth in the decision are not checked (SPS gate)");
    }
    // P2
    {
        WeakLadder l = lowerPmsLadder(b);
        expect(l.pmsLowered && l.maxVideoBitrateKbps >= 2000 && l.maxVideoBitrateKbps <= b.maxVideoBitrateKbps &&
                   l.videoQuality <= 30, "P2 lowered Baseline 480 ladder (" + std::to_string(l.maxVideoBitrateKbps) + " kbps)");
        std::string why;
        expect(validateWeakLadder(l, &why), "P2 lowered ladder still valid (" + why + ")");
        const std::string ex = plexClientProfileExtra(l);
        expect(ex.find("scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec&context=streaming&protocol=http&type=upperBound&name=video.refFrames&value=1&isRequired=true") != std::string::npos,
               "P2 lowered Extra: refFrames 1 required on the transcode target");
        expect(ex.find("name=video.bitDepth&value=8&isRequired=true") != std::string::npos &&
                   ex.find("name=video.width&value=") != std::string::npos,
               "P2 lowered Extra: 8-bit and size required");
        expect(plexClientProfileExtra(b).find("isRequired=true)+add-limitation(scope=videoTranscodeTarget&scopeName=h264&scopeType=videoCodec&context=streaming&protocol=http&type=upperBound&name=video.refFrames") == std::string::npos,
               "P2 the first request has no lowered bounds");
        WeakLadder h = b; h.h264Profile = "high"; h.fpgaProfileVariant.clear(); h.maxVideoBitrateKbps = 1500;
        expect(lowerPmsLadder(h).maxVideoBitrateKbps == 1125, "P2 High 1500 -> 1125 kbps");
    }
    // P3
    {
        char tmpl[] = "/tmp/nprshimXXXXXX";
        const std::string dir = mkdtemp(tmpl);
        const std::string calls = dir + "/calls";
        {
            std::ofstream f(dir + "/curl");
            f << "#!/bin/sh\necho \"$*\" >> '" << calls << "'\nprintf '%s' '"
              << xmlFor("codec=\"h264\" profile=\"baseline\" width=\"1280\" height=\"720\"") << "'\n";
        }
        chmod((dir + "/curl").c_str(), 0755);
        setenv("PATH", (dir + ":" + std::getenv("PATH")).c_str(), 1);
        setenv("MPX_SEND_PROFILE_EXTRA", "1", 1);
        const std::string start = buildUniversalTranscodeUrl("http://192.0.2.24:32400", "/library/metadata/1", "TOK", "s1", 0, b);
        (void)takeDecisionMismatch();
        expect(!ensureUniversalDecision(start, "s1", "TOK", b), "P3 mismatching decision refuses the first request");
        const std::string mm = takeDecisionMismatch();
        expect(mm.find("width 1280") == 0, "P3 mismatch reason: " + mm);
        const WeakLadder l = lowerPmsLadder(b);
        const std::string start2 = buildUniversalTranscodeUrl("http://192.0.2.24:32400", "/library/metadata/1", "TOK", "s2", 0, l);
        expect(ensureUniversalDecision(start2, "s2", "TOK", l), "P3 the lowered request proceeds (SPS gate decides)");
        expect(takeDecisionMismatch().empty(), "P3 no second retry is asked for");
        std::ifstream in(calls);
        std::string line, last;
        int n = 0;
        while (std::getline(in, line)) if (line.find("/universal/decision") != std::string::npos) { ++n; last = line; }
        expect(n == 2, "P3 exactly 2 decisions, got " + std::to_string(n));
        expect(last.find("video.refFrames&value=1&isRequired=true") != std::string::npos, "P3 the 2nd decision carries the strict Extra");
        expect(last.find("X-Plex-Client-Profile-Name") == std::string::npos, "P3 no profile name on either decision");
        unsetenv("MPX_SEND_PROFILE_EXTRA");
        // P3r: the real variant-run decision through the same path: accepted at once, no lowered retry asked.
        {
            std::ofstream xf(dir + "/real.xml"); xf << realDecision;
        }
        {
            std::ofstream f(dir + "/curl");
            f << "#!/bin/sh\necho \"$*\" >> '" << calls << "'\ncat '" << dir << "/real.xml'\n";
        }
        std::ofstream(calls, std::ios::trunc).close();
        expect(ensureUniversalDecision(start, "s3", "TOK", b), "P3r real variant decision accepted (no lowered retry)");
        expect(takeDecisionMismatch().empty(), "P3r real variant decision: no lowered retry asked");
        std::system(("rm -rf '" + dir + "'").c_str());
    }
    // R1
    {
        gate::StreamVerdict v;
        v.outcome = gate::StreamOutcome::Out; v.field = "max_num_ref_frames"; v.value = 3; v.limit = 1;
        gate::GatePlayContext first;   // PMS universal transcode, no helper, retry not used
        first.pmsRetryAvailable = true;
        const gate::GateDecision d1 = gate::decideStream(v.outcome, 0, first);
        expect(d1.action == gate::GateAction::Fallback && d1.row == gate::GateRow::R6p,
               std::string("R1 NVENC refs 3: lowered PMS retry (R6p), got ") + gate::rowName(d1.row));
        gate::GatePlayContext lowered;  // the retry itself
        const gate::GateDecision d2 = gate::decideStream(v.outcome, 0, lowered);
        expect(d2.action == gate::GateAction::Error && d2.row == gate::GateRow::R7, "R1 lowered retry refused: on-screen error R7");
        const std::string text = gate::errorText(d2.row, gate::verdictPhrase(v), 0x48FF65F2u);
        expect(text.find("max_num_ref_frames 3 (limit 1)") != std::string::npos && text.find("Can't play here") == 0,
               "R1 error text names the refs: " + text);
        gate::GatePlayContext helper = first;
        helper.helperConfigured = true;
        expect(gate::decideStream(v.outcome, 0, helper).row == gate::GateRow::R6, "R1 ladder helper configured: R6 first");
        expect(gate::decideStream(v.outcome, 5, first).row == gate::GateRow::R9, "R1 after AUs were written: R9 (unchanged)");
    }
    std::printf("test_no_profile_retry: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
