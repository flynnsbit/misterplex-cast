// NO-REQUIRED-PROFILE (owner 1011 20:59 CT, docs/plans/NO-REQUIRED-PROFILE.md): by default NO
// X-Plex-Client-Profile-Name on any request and the Profile-Extra on every server (WAN and LAN):
//   N0 default: Baseline FPGA ladder and (High build) the High ladder: decision, start probe, FFmpeg
//      start/segments, timeline, ping carry no Profile-Name; decision + FFmpeg carry the Extra with the
//      core's limits on WAN and LAN.
//   N1 SEND_PROFILE_NAME=variant: LAN PMS gets the per-variant name, WAN the fixed name.
//   N1f SEND_PROFILE_NAME=fixed: fixed names everywhere.
// Mutants: MUT_PROFILE_NAME_ALWAYS (N0 WAN decision), MUT_NO_EXTRA_LAN (N0 LAN decision Extra),
// MUT_PROFILE_NAME_VARIANT (N1 WAN decision), MUT_PROFILE_NAME_LAN_FIXED (N1 LAN decision).
// (history below)
// The PMS client profile name is FIXED on every request path (2026-10-11).
// A remote PMS logged at 16:41:14 CT "Unable to find client profile
// MiSTerPlex-FPGA-IP-480-23976-filter-off": the 480p Baseline FPGA ladder sent its internal stream-contract
// variant (selectFpgaPmsProfile) as X-Plex-Client-Profile-Name (prodfix 2e40a799 on a lab MiSTer, the noproxy
// run-B attempt, playMedia 16:40:48 CT). Every path must now send exactly one name:
// MiSTerPlex (Baseline cores) / MiSTerPlex-High (High core), or the CLIENT_PROFILE_NAME override
// (MPX_CLIENT_PROFILE_NAME env alias wins).
//   (Update 1011 evening: a LAN PMS (127.0.0.24 stands in for one) resolves the fixed MiSTerPlex to an
//   NVENC MiSTerPlex.xml, ref=3, refused by the prod core; it has the 16 per-variant XMLs. So a LAN/private
//   PMS gets the per-variant name again and only remote/WAN PMS get the fixed names.)
//   N1 Baseline 480p FPGA ladder (the logged variant): WAN (203.0.113.47) decision, start probe, FFmpeg
//      start/segment reads (plain + forced caps), /:/timeline say exactly "MiSTerPlex"; LAN
//      (127.0.0.24) decision, FFmpeg start/segments and timeline say exactly the variant
//      "MiSTerPlex-FPGA-IP-480-23976-filter-off"; the WAN Extra carries the variant's limits.
//   N2 the NTSC src30 variant (240p IDR 30000/1001): same.
//   N3 (High builds) High core: every path says "MiSTerPlex-High"; the Extra still asks for High.
//   N4 override: conf CLIENT_PROFILE_NAME and env MPX_CLIENT_PROFILE_NAME (wins) replace the name.
//   N5 no WAN request carries a MiSTerPlex-FPGA-* name.
// Requests that shell out to curl run against a PATH shim that logs the argv.
// Mutants: MUT_PROFILE_NAME_VARIANT (variant to every server) FAILs at N1 WAN decision;
// MUT_PROFILE_NAME_LAN_FIXED (fixed name to a LAN PMS, 1e20f68f3/02e5e4595) FAILs at N1 LAN decision.
#include "plex_resolve.hpp"
#include "pms_timeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace misterplex;

namespace {
int fails = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

std::string shimDir, shimLog;
void installCurlShim() {
    char tmpl[] = "/tmp/pnshimXXXXXX";
    shimDir = mkdtemp(tmpl);
    shimLog = shimDir + "/curl.log";
    std::ofstream f(shimDir + "/curl");
    f << "#!/bin/sh\n"
         "for a in \"$@\"; do printf '%s\\037' \"$a\"; done >> \"" << shimLog << "\"\n"
         "printf '\\n' >> \"" << shimLog << "\"\n"
         "case \"$*\" in *decision*) printf '<MediaContainer generalDecisionCode=\"1000\"/>';; *) printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx';; esac\n"
         "case \"$*\" in *http_code*) printf '200';; esac\n"
         "exit 0\n";
    f.close();
    chmod((shimDir + "/curl").c_str(), 0755);
    const std::string path = shimDir + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin");
    setenv("PATH", path.c_str(), 1);
}
std::vector<std::vector<std::string>> takeCalls() {
    std::vector<std::vector<std::string>> calls;
    std::ifstream f(shimLog);
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> args;
        std::string cur;
        for (char c : line) {
            if (c == '\037') { args.push_back(cur); cur.clear(); } else cur += c;
        }
        calls.push_back(args);
    }
    std::ofstream(shimLog, std::ios::trunc).close();
    return calls;
}
std::vector<std::string> curlHeaderValues(const std::vector<std::string>& args, const std::string& name) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "-H" && args[i + 1].rfind(name + ": ", 0) == 0)
            out.push_back(args[i + 1].substr(name.size() + 2));
    return out;
}
std::vector<std::string> blockHeaderValues(const std::string& block, const std::string& name) {
    std::vector<std::string> out;
    std::istringstream in(block);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind(name + ": ", 0) == 0) out.push_back(l.substr(name.size() + 2));
    }
    return out;
}
std::vector<std::string> pairValues(const std::vector<std::pair<std::string, std::string>>& h,
                                    const std::string& name) {
    std::vector<std::string> out;
    for (const auto& p : h)
        if (p.first == name) out.push_back(p.second);
    return out;
}
const char* kPN = "X-Plex-Client-Profile-Name";
const std::string kNone = "<none>";
void expectExactly(const std::vector<std::string>& got, const std::string& want, const std::string& what) {
    std::string g;
    for (const auto& v : got) g += (g.empty() ? "" : ",") + v;
    if (want == kNone)
        expect(got.empty(), what + ": Profile-Name [" + g + "] want none");
    else
        expect(got.size() == 1 && got[0] == want, what + ": Profile-Name [" + g + "] want exactly [" + want + "]");
}
std::string lanExtraSeen;
std::string wanLog;

// Every request path for one ladder; returns the decision call's Extra (WAN).
std::string checkAllPaths(const WeakLadder& w, const std::string& want, const std::string& tag,
                          std::string wantLan = {}) {
    if (wantLan.empty()) wantLan = want;
    std::string wanExtra;
    for (const char* base : {"http://203.0.113.47:32401", "http://127.0.0.24:32400"}) {
        const bool isWan = std::string(base).find("203.0.113.47") != std::string::npos;
        const std::string where = tag + (isWan ? " WAN" : " LAN");
        const std::string& wantHere = isWan ? want : wantLan;
        std::string why;
        const std::string start = buildUniversalTranscodeUrl(base, "/library/metadata/11681", "TOK", "mplex-1",
                                                             0, w, &why);
        expect(!start.empty(), where + ": start URL built (" + why + ")");
        // decision
        takeCalls();
        ensureUniversalDecision(start, "mplex-1", "TOK", w);
        auto calls = takeCalls();
        bool sawDecision = false;
        for (const auto& c : calls) {
            std::string joined;
            for (const auto& a : c) joined += a + " ";
            if (isWan) wanLog += joined + "\n";
            if (joined.find("/universal/decision") == std::string::npos) continue;
            sawDecision = true;
            expectExactly(curlHeaderValues(c, kPN), wantHere, where + " decision");
            const auto ex = curlHeaderValues(c, "X-Plex-Client-Profile-Extra");
            if (wantHere == kNone) {
                expect(!ex.empty(), where + " decision Extra: FPGA unnamed Extra (Goal B)");
            }
            if (!ex.empty() && isWan) wanExtra = ex[0];
            if (!ex.empty() && !isWan) lanExtraSeen = ex[0];
        }
        expect(sawDecision, where + ": decision request issued");
        // start probe (remote only in the resolve; both shapes checked): the resolve's probe headers
        plexHttpGetNoBodyResult(start, plexStartProbeHeaders("mplex-1", "TOK", w, start), 12);
        calls = takeCalls();
        expect(calls.size() == 1, where + ": one start probe");
        for (const auto& c : calls) {
            if (isWan) { for (const auto& a : c) wanLog += a + " "; wanLog += "\n"; }
            expectExactly(curlHeaderValues(c, kPN), wantHere, where + " start probe");
        }
        // FFmpeg start.mp4 open and every range/segment read it makes (also the ladder src query block)
        for (bool force : {false, true}) {
            const std::string h = plexFfmpegHeaders("mplex-1", "TOK", w, force, start);
            if (isWan) wanLog += h;
            expectExactly(blockHeaderValues(h, kPN), wantHere, where + std::string(" ffmpeg start/segments") +
                                                                   (force ? " (forced caps)" : ""));
            if (wantHere == kNone) {
                const bool extra = !blockHeaderValues(h, "X-Plex-Client-Profile-Extra").empty();
                expect(extra, where + " ffmpeg start/segments Extra: FPGA unnamed Extra (Goal B)");
            }
        }
        // /:/timeline (+ the transcode ping) with the name the resolve recorded (ResolveResult::clientProfileName)
        PmsTimelineSession s;
        s.baseUrl = base;
        s.token = "TOK";
        s.key = "/library/metadata/11681";
        s.ratingKey = "11681";
        s.clientIdentifier = "misterplex-192.0.2.12";
        s.clientProfileName = plexRequestProfileName(w, start);
#ifdef MPX_TEST_HIGH
        s.sessionIdentifier = "mplex-1";
        s.transcodeSession = "mplex-1";
#endif
        PmsTimelineHttpRequest tr;
        expect(buildPmsTimelineHttpRequest(s, "playing", 1000, 7553590, tr), where + ": timeline built");
        expectExactly(pairValues(tr.headers, kPN), wantHere, where + " timeline");
#ifdef MPX_TEST_HIGH
        PmsTimelineHttpRequest pr;
        expect(buildPmsTranscodePingRequest(s, pr), where + ": ping built");
        expectExactly(pairValues(pr.headers, kPN), wantHere, where + " transcode ping");
#endif
    }
    // No target URL (callers that do not know the server): the WAN-safe answer (none / fixed).
    expectExactly(blockHeaderValues(plexFfmpegHeaders("mplex-1", "TOK", w, false), kPN), want,
                  tag + " ffmpeg headers without a target");
    // Metadata/probe default identity: none unless a name is opted in.
    expectExactly(defaultRequestProfileName().empty() ? std::vector<std::string>{}
                                                      : std::vector<std::string>{defaultRequestProfileName()},
                  want == kNone ? kNone : defaultRequestProfileName(), tag + " default identity");
    return wanExtra;
}
} // namespace

int main() {
    unsetenv("MPX_CLIENT_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_EXTRA");
    installCurlShim();
#ifdef MPX_TEST_HIGH
    setResolverCoreBuildId(0x48FF65F2u); // the prod 480p Baseline core
#endif
    // N0: the default, no profile installed anywhere.
    {
        expect(sendProfileNameMode() == SendProfileNameMode::None, "N0 default send mode is none");
        WeakLadder w;
        std::string why;
        applyFpgaPlexProfile(w, "ip", 24000, 1001, true, &why, "480p", true);
        const std::string extra = checkAllPaths(w, kNone, "N0");
        expect(extra.find("name=video.refFrames&value=1") != std::string::npos &&
                   lanExtraSeen.find("name=video.refFrames&value=1") != std::string::npos,
               "N0 FPGA Extra (no Profile-Name) on WAN and LAN: wan=[" + extra + "] lan=[" + lanExtraSeen + "]");
        setenv("MPX_SEND_PROFILE_EXTRA", "1", 1);
        lanExtraSeen.clear();
        const std::string extraOn = checkAllPaths(w, kNone, "N0e");
        for (const std::string& ex : {extraOn, lanExtraSeen})
            expect(ex.find("name=video.refFrames&value=1") != std::string::npos &&
                       ex.find("list=baseline") != std::string::npos &&
                       ex.find("name=video.level&value=30") != std::string::npos &&
                       ex.find("name=video.width&value=") != std::string::npos,
                   "N0e Extra opted in carries the Baseline core limits on WAN and LAN: " + ex);
        unsetenv("MPX_SEND_PROFILE_EXTRA");
#ifdef MPX_TEST_HIGH
        setResolverCoreBuildId(0x48FE178Fu);
        WeakLadder h;
        applyFpgaPlexProfile(h, "ip", 24000, 1001, true, &why, "480p", true);
        const std::string hx = checkAllPaths(h, kNone, "N0 High");
        expect(hx.find("list=high") != std::string::npos && lanExtraSeen.find("list=high") != std::string::npos,
               "N0 High Extra asks for High on WAN and LAN");
        setResolverCoreBuildId(0x48FF65F2u);
#endif
    }
    setSendProfileNameConf("variant");
    // N1 (SEND_PROFILE_NAME=variant): the exact variant a remote PMS logged.
    {
        WeakLadder w;
        std::string why;
        expect(applyFpgaPlexProfile(w, "ip", 24000, 1001, /*filteringOff=*/true, &why, "480p", true),
               "N1 480p FPGA ladder applies (" + why + ")");
        expect(w.fpgaProfileVariant == "MiSTerPlex-FPGA-IP-480-23976-filter-off",
               "N1 internal variant is the logged one, got " + w.fpgaProfileVariant);
        expect(isFpgaPlexProfile(w), "N1 FPGA ladder");
        const std::string extra = checkAllPaths(w, "MiSTerPlex", "N1", "MiSTerPlex-FPGA-IP-480-23976-filter-off");
        expect(extra.find("name=video.refFrames&value=1") != std::string::npos &&
                   extra.find("name=video.frameRate&value=30") != std::string::npos &&
                   extra.find("name=video.bitDepth&value=8") != std::string::npos &&
                   extra.find("name=audio.channels&value=2") != std::string::npos &&
                   extra.find("list=baseline") != std::string::npos,
               "N1 WAN Extra carries the variant limits (refFrames 1, <=30 fps, 8-bit, stereo, baseline): " + extra);
    }
    // N2: the NTSC source-rate variant.
    {
        WeakLadder w;
        std::string why;
        expect(applyFpgaPlexProfile(w, "idr", 24000, 1001, false, &why, "240p", false), "N2 240p applies");
        w.fpgaProfileVariant = fpgaNtscSourceProfileName("idr", "240p", false);
        w.fpgaFpsNum = 30000;
        w.fpgaFpsDen = 1001;
        expect(validateWeakLadder(w, &why), "N2 src30 ladder valid (" + why + ")");
        checkAllPaths(w, "MiSTerPlex", "N2", "MiSTerPlex-FPGA-IDR-240p-src30-filter-on");
    }
#ifdef MPX_TEST_HIGH
    // N3: High core (SEND_PROFILE_NAME=fixed).
    setSendProfileNameConf("fixed");
    {
        setResolverCoreBuildId(0x48FE178Fu);
        WeakLadder w;
        std::string why;
        expect(applyFpgaPlexProfile(w, "ip", 24000, 1001, true, &why, "480p", true), "N3 High ladder applies");
        expect(w.h264Profile == "high" && !isFpgaPlexProfile(w), "N3 High ladder (not a Baseline variant)");
        const std::string extra = checkAllPaths(w, "MiSTerPlex-High", "N3");
        expect(extra.find("list=high") != std::string::npos, "N3 High Extra still asks for High: " + extra);
        setResolverCoreBuildId(0x48FF65F2u);
    }
#endif
    setSendProfileNameConf("fixed");   // (also outside the High-only N3 block)
    {
        WeakLadder w;
        std::string why;
        applyFpgaPlexProfile(w, "ip", 24000, 1001, true, &why, "480p", true);
        checkAllPaths(w, "MiSTerPlex", "N1f");
    }
    setenv("MPX_SEND_PROFILE_NAME", "0", 1);   // env wins over the conf: back to none
    expect(sendProfileNameMode() == SendProfileNameMode::None, "N1f MPX_SEND_PROFILE_NAME=0 beats the conf");
    unsetenv("MPX_SEND_PROFILE_NAME");
    setSendProfileNameConf("");
    // N4: override (conf, then the env alias wins).
    {
        WeakLadder w;
        std::string why;
        applyFpgaPlexProfile(w, "ip", 24000, 1001, true, &why, "480p", true);
        setClientProfileNameConf("Custom Profile");
        checkAllPaths(w, "Custom Profile", "N4 conf");
        setenv("MPX_CLIENT_PROFILE_NAME", "EnvAlias", 1);
        checkAllPaths(w, "EnvAlias", "N4 env");
#ifdef MPX_TEST_HIGH
        setResolverCoreBuildId(0x48FE178Fu);
        WeakLadder h;
        applyFpgaPlexProfile(h, "ip", 24000, 1001, true, &why, "480p", true);
        checkAllPaths(h, "EnvAlias", "N4 env High");
        setResolverCoreBuildId(0x48FF65F2u);
#endif
        unsetenv("MPX_CLIENT_PROFILE_NAME");
        setClientProfileNameConf("");
        expect(baselineClientProfileName() == "MiSTerPlex", "N4 cleared override restores MiSTerPlex");
        expect(plexRequestProfileName(w, "http://203.0.113.47:32401/x").empty(), "N4 cleared override: no name again");
    }
    // N5: no variant name ever went out.
    expect(wanLog.find("MiSTerPlex-FPGA") == std::string::npos, "N5 a MiSTerPlex-FPGA-* name was sent to a WAN PMS");
    std::printf("test_profile_name_fixed: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    std::string rm = "rm -rf '" + shimDir + "'";
    (void)std::system(rm.c_str());
    return fails ? 1 : 0;
}
