// Round E 1011 (LAN PMS E1/E2 + remote PMS 29729):
//  N: with an X-Plex-Client-Profile-Name sent, the Extra keeps the named XML's encoder settings: no
//     add-transcode-target(...replace=true), no VideoEncodeFlags term, limitations without replace=true.
//  L: the startup log states the name actually sent and its source (never send=none beside an override).
//  D: a decision's absent/zero level, refs and profile are unknown, never a mismatch; codec + output size count.
// Mutants: MUT_NAMED_EXTRA_DESTRUCTIVE FAILs at N1; MUT_PROFILE_NAME_LOG_OLD at L1; MUT_DECISION_ZERO_IS_MISMATCH at D1.
#include "plex_resolve.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace misterplex;
static int failures = 0;
static void expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
static std::string headerValue(const std::string& hdrs, const std::string& name) {
    const size_t a = hdrs.find(name + ": ");
    if (a == std::string::npos) return {};
    const size_t b = hdrs.find("\r\n", a);
    return hdrs.substr(a + name.size() + 2, b - a - name.size() - 2);
}
static WeakLadder fpga() {
    WeakLadder w; std::string why;
    if (!applyFpgaPlexProfile(w, "ip", 24000, 1001, true, &why, "480p", true))
        std::printf("apply failed: %s\n", why.c_str());
    return w;
}
int main() {
    unsetenv("MPX_CLIENT_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_EXTRA");
    const WeakLadder b = fpga();
    WeakLadder hi; hi.h264Profile = "high"; hi.videoResolution = "640x480"; hi.h264Level = 30;
    hi.videoCodec = "h264"; hi.audioCodec = "aac";
    const WeakLadder& high = hi;
    // N1: the named Extra is non-destructive, for the Baseline variant and the High ladder.
    for (const WeakLadder* w : {&b, &high}) {
        const std::string named = plexClientProfileExtra(*w, true);
        expect(!has(named, "add-transcode-target") && !has(named, "VideoEncodeFlags") && !has(named, "replace=true") &&
                   has(named, "add-limitation(") && has(named, "name=video.profile"),
               "N1 named Extra (" + w->h264Profile + "): limitations only, no replace=true / target / flags: " + named);
    }
    // N2: no name: unchanged (the target terms, replace=true, the flags term for the FPGA variant).
    const std::string unnamed = plexClientProfileExtra(b);
    expect(has(unnamed, "add-transcode-target(") && has(unnamed, "replace=true") &&
               has(unnamed, "add-transcode-target-settings(") && plexClientProfileExtra(b, false) == unnamed,
           "N2 unnamed Extra keeps add-transcode-target replace=true and VideoEncodeFlags");
    // N3: the request headers with the env override (lead E1: MPX_CLIENT_PROFILE_NAME=MiSTerPlex-High) and the
    // forced ladder caps: the name goes out, and its Extra is the named (non-destructive) one.
    setenv("MPX_CLIENT_PROFILE_NAME", "MiSTerPlex-High", 1);
    for (const WeakLadder* w : {&b, &high}) {
        const std::string h = plexFfmpegHeaders("sess", "tok", *w, true, "http://192.0.2.24:32400/x");
        const std::string ex = headerValue(h, "X-Plex-Client-Profile-Extra");
        expect(headerValue(h, "X-Plex-Client-Profile-Name") == "MiSTerPlex-High" && !ex.empty() &&
                   ex == plexClientProfileExtra(*w, true),
               "N3 override name sent, Extra non-destructive (" + w->h264Profile + ")");
    }
    // L1: the log names what is sent and why (env), never send=none.
    const std::string l1 = profileNameSendDescription();
    expect(has(l1, "send=MiSTerPlex-High") && has(l1, "env MPX_CLIENT_PROFILE_NAME") && !has(l1, "send=none"),
           "L1 env override log: " + l1);
    unsetenv("MPX_CLIENT_PROFILE_NAME");
    setClientProfileNameConf("MiSTerPlex");
    const std::string l2 = profileNameSendDescription();
    expect(has(l2, "send=MiSTerPlex ") && has(l2, "conf CLIENT_PROFILE_NAME") && !has(l2, "send=none"),
           "L2 conf override log: " + l2);
    setClientProfileNameConf("");
    const std::string l3 = profileNameSendDescription();
    expect(l3.rfind("send=none", 0) == 0, "L3 no override, no SEND_PROFILE_NAME: " + l3);
    setenv("MPX_SEND_PROFILE_NAME", "variant", 1);
    const std::string l4 = profileNameSendDescription();
    expect(has(l4, "MiSTerPlex-FPGA-*") && has(l4, "SEND_PROFILE_NAME=variant"), "L4 variant mode log: " + l4);
    unsetenv("MPX_SEND_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_EXTRA");
    const std::string hn = plexFfmpegHeaders("sess", "tok", b, true, "http://192.0.2.24:32400/x");
    expect(headerValue(hn, "X-Plex-Client-Profile-Name").empty() &&
               headerValue(hn, "X-Plex-Client-Profile-Extra") == plexClientProfileExtra(b, false),
           "L5 no name: FPGA Extra still sent (Goal B CAVLC/ref1, no MiSTerPlex.xml)");
    setenv("MPX_SEND_PROFILE_EXTRA", "1", 1);
    const std::string he = plexFfmpegHeaders("sess", "tok", b, true, "http://192.0.2.24:32400/x");
    expect(headerValue(he, "X-Plex-Client-Profile-Name").empty() &&
               headerValue(he, "X-Plex-Client-Profile-Extra") == plexClientProfileExtra(b, false),
           "L5e Extra opted in: the full (unnamed) Extra");
    unsetenv("MPX_SEND_PROFILE_EXTRA");
    // D1: remote PMS 29729 (e3a4eb274 run): Stream profile=main level=0 refFrames=0 at 624x468, transcode output fits.
    const std::string remote_decision =
        "<MediaContainer><Media videoCodec=\"h264\" videoProfile=\"main\" width=\"624\" height=\"468\">"
        "<Part><Stream streamType=\"1\" codec=\"h264\" profile=\"main\" level=\"0\" refFrames=\"0\" width=\"624\" "
        "height=\"468\"/></Part></Media></MediaContainer>";
    const DecisionVideo dj = parseDecisionVideo(remote_decision);
    expect(dj.found && decisionCoreMismatch(dj, b).empty(), "D1 remote PMS decision (level/refs 0): no mismatch");
    // D2: home PMS E2 shape: constrained baseline, level 0, refs 0, 624x480: no mismatch; attributes absent too.
    const std::string e2 =
        "<MediaContainer><Media videoCodec=\"h264\"><Part><Stream streamType=\"1\" codec=\"h264\" "
        "profile=\"constrained baseline\" level=\"0\" refFrames=\"0\" width=\"624\" height=\"480\"/></Part></Media>"
        "</MediaContainer>";
    const std::string bare = "<MediaContainer><Media videoCodec=\"h264\" width=\"624\" height=\"480\"/></MediaContainer>";
    expect(decisionCoreMismatch(parseDecisionVideo(e2), b).empty() &&
               decisionCoreMismatch(parseDecisionVideo(bare), b).empty() &&
               decisionCoreMismatch(parseDecisionVideo(e2), high).empty(),
           "D2 unreported level/refs/profile are unknown (Baseline and High)");
    // D3: control: codec and output size still count.
    const std::string wide = "<MediaContainer><Media><Part><Stream streamType=\"1\" codec=\"h264\" level=\"0\" "
                             "refFrames=\"0\" width=\"720\" height=\"480\"/></Part></Media></MediaContainer>";
    const std::string hevc = "<MediaContainer><Media><Part><Stream streamType=\"1\" codec=\"hevc\" width=\"624\" "
                             "height=\"480\"/></Part></Media></MediaContainer>";
    expect(!decisionCoreMismatch(parseDecisionVideo(wide), b).empty() &&
               !decisionCoreMismatch(parseDecisionVideo(hevc), b).empty(),
           "D3 an oversize or non-h264 decision still mismatches");
    std::printf("test_named_profile_extra: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
