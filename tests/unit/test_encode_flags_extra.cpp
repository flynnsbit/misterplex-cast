// NO-REQUIRED-PROFILE: X-Plex-Client-Profile-Extra carries the FPGA Baseline encoder flags as
// add-transcode-target-settings(...VideoEncodeFlags=...), so a PMS with no MiSTerPlex profile (augmented Web
// fallback, which adds no -profile) or one whose XML flags replace=true drops still encodes Baseline/CAVLC/ref 1.
// Live: lab-runs/home-pms-hwenc-1011 S1/S2/V1/V2 (Constrained Baseline, CAVLC, refs 1, no B).
// Mutants: MUT_NO_VEF_TERM FAILs at E1; MUT_VEF_CABAC_ON FAILs at E2.
#include "plex_resolve.hpp"
#include <cstdio>
#include <string>
using namespace misterplex;
static int failures = 0;
static void expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static std::string settingsValue(const std::string& extra) {
    const std::string key = "add-transcode-target-settings(type=videoProfile&context=streaming&protocol=http&VideoEncodeFlags=";
    const size_t a = extra.find(key);
    if (a == std::string::npos) return {};
    const size_t b = extra.find(')', a + key.size());
    return extra.substr(a + key.size(), b - a - key.size());
}
static WeakLadder fpga(const char* proto, int num, int den, bool filterOff) {
    WeakLadder w; std::string why;
    if (!applyFpgaPlexProfile(w, proto, num, den, filterOff, &why, "480p", true))
        std::printf("apply failed: %s\n", why.c_str());
    return w;
}
int main() {
    // E1: the term is present for an FPGA Baseline ladder, its value is percent-encoded (no raw separators).
    const WeakLadder ip = fpga("ip", 24000, 1001, true);
    const std::string extra = plexClientProfileExtra(ip);
    const std::string enc = settingsValue(extra);
    expect(!enc.empty(), "E1 Extra carries add-transcode-target-settings VideoEncodeFlags (" + ip.fpgaProfileVariant + ")");
    expect(enc.find_first_of(" =&:()+") == std::string::npos, "E1 the flags value is percent-encoded");
    // E2: decoded, it is the per-variant XML's flags plus the encoder-generic -refs 1 -bf 0.
    const std::string dec = urlDecode(enc);
    expect(dec == "-c:v libx264 -profile:v baseline -level 3.0 -r 24000/1001 -refs 1 -bf 0 -x264opts cabac=0:bframes=0:"
                  "ref=1:weightp=0:8x8dct=0:partitions=none:keyint=24:min-keyint=24:scenecut=0:open-gop=0:intra-refresh=0:"
                  "threads=1:sliced-threads=0:slices=1:vbv-maxrate=4000:vbv-bufsize=1000:qpmin=10:qpmax=40:no-deblock=1",
           "E2 IP-480-23976-filter-off flags: " + dec);
    expect(dec == plexVideoEncodeFlags(ip), "E2 Extra value == plexVideoEncodeFlags");
    // E3: IDR keyint 1, 24 fps rate, filter-on deblock.
    const WeakLadder idr = fpga("idr", 24, 1, false);
    const std::string d2 = plexVideoEncodeFlags(idr);
    expect(d2.find("keyint=1:min-keyint=1") != std::string::npos && d2.find(" -r 24 ") != std::string::npos &&
           d2.find("no-deblock=0") != std::string::npos, "E3 " + idr.fpgaProfileVariant + ": " + d2);
    // E4: a non-FPGA ladder (High core) sends no encoder flags (its limits are met by the PMS default encode).
    WeakLadder high; high.h264Profile = "high"; high.videoResolution = "640x480"; high.h264Level = 30;
    expect(plexVideoEncodeFlags(high).empty() && settingsValue(plexClientProfileExtra(high)).empty(),
           "E4 High ladder: no VideoEncodeFlags term");
    // E4b: 480p 29.97 src30 Extra pins -r 30000/1001 (Goal B).
    WeakLadder src30 = ip;
    src30.fpgaProfileVariant = fpgaNtscSourceProfileName("ip", "480p", true);
    src30.fpgaFpsNum = 30000;
    src30.fpgaFpsDen = 1001;
    const std::string d30 = plexVideoEncodeFlags(src30);
    expect(d30.find(" -r 30000/1001 ") != std::string::npos &&
               urlDecode(settingsValue(plexClientProfileExtra(src30))).find("-r 30000/1001") !=
                   std::string::npos,
           "E4b 480p src30 Extra pins -r 30000/1001: " + d30);
    // E4c: 240p/480i src30 Extra pins the same 29.97 clock (Goal D cadence).
    for (const char* mode : {"240p", "480i"}) {
        WeakLadder ntsc = ip;
        ntsc.fpgaProfileVariant = fpgaNtscSourceProfileName("ip", mode, true);
        ntsc.fpgaFpsNum = 30000;
        ntsc.fpgaFpsDen = 1001;
        const std::string dNtsc = plexVideoEncodeFlags(ntsc);
        expect(dNtsc.find(" -r 30000/1001 ") != std::string::npos &&
                   urlDecode(settingsValue(plexClientProfileExtra(ntsc))).find("-r 30000/1001") !=
                       std::string::npos,
               std::string("E4c ") + mode + " src30 Extra pins -r 30000/1001: " + dNtsc);
    }
    // E5: the lowered retry keeps the term.
    WeakLadder low = lowerPmsLadder(ip);
    expect(!settingsValue(plexClientProfileExtra(low)).empty(), "E5 lowered retry Extra keeps the flags term");
    // E6: a negotiated High core (MPX_HIGH_NO_CAVLC) asks for Main/CABAC, not baseline.
    WeakLadder cab = ip;
    cab.highCabac = true;
    cab.h264Profile = "main";
    cab.h264ProfileParam = "main";
    std::string why;
    expect(validateWeakLadder(cab, &why), "E6 high core main ladder validates: " + why);
    const std::string dCab = plexVideoEncodeFlags(cab);
    expect(dCab.find("-profile:v main") != std::string::npos && dCab.find("cabac=1") != std::string::npos &&
               dCab.find("cabac=0") == std::string::npos && dCab.find("8x8dct=0") != std::string::npos &&
               dCab.find("weightp=0") != std::string::npos && dCab.find(" -bf 0 ") != std::string::npos &&
               dCab.find("-refs 1") != std::string::npos &&
               dCab.find("-c:v libx264") == std::string::npos,
           "E6 High flags: " + dCab);
    const std::string playHeaders = plexFfmpegHeaders("mplex-1", "tok", cab, true,
                                                      "http://198.51.100.153:32400/x");
    const std::string extraKey = "X-Plex-Client-Profile-Extra: ";
    const auto extraAt = playHeaders.find(extraKey);
    std::string playExtra = extraAt == std::string::npos ? std::string()
                                                         : playHeaders.substr(extraAt + extraKey.size());
    const auto extraNl = playExtra.find('\r');
    if (extraNl != std::string::npos)
        playExtra = playExtra.substr(0, extraNl);
    const std::string playFlags = urlDecode(settingsValue(playExtra));
    expect(extraAt != std::string::npos && playFlags.find("cabac=1") != std::string::npos &&
               playFlags.find("-profile:v main") != std::string::npos &&
               playFlags.find("-c:v libx264") == std::string::npos &&
               playHeaders.find("X-Plex-Client-Profile-Name:") == std::string::npos,
           "E6d High PLAY headers carry Main/CABAC Extra and no profile name: " + playFlags);
    expect(cab.h264ProfileParam == "main", "E6 videoProfile query is main");
    // E6b: the WAN start probe is the GET that starts the transcoder. It must
    // carry the same CABAC Extra as playback, and no named profile.
    const auto probe = plexStartProbeHeaders("mplex-1", "tok", cab, "http://198.51.100.153:32400/x");
    std::string probeExtra, probeName;
    for (const auto& hv : probe) {
        if (hv.first == "X-Plex-Client-Profile-Extra") probeExtra = hv.second;
        if (hv.first == "X-Plex-Client-Profile-Name") probeName = hv.second;
    }
    const std::string probeFlags = urlDecode(settingsValue(probeExtra));
    expect(probeName.empty() && probeFlags.find("cabac=1") != std::string::npos &&
               probeFlags.find("cabac=0") == std::string::npos &&
               probeFlags.find("-profile:v main") != std::string::npos,
           "E6b High probe Extra is Main/CABAC and unnamed: " + probeFlags);
    const auto baseProbe = plexStartProbeHeaders("mplex-1", "tok", ip, "http://198.51.100.153:32400/x");
    std::string baseProbeExtra;
    for (const auto& hv : baseProbe)
        if (hv.first == "X-Plex-Client-Profile-Extra") baseProbeExtra = hv.second;
    const std::string baseProbeFlags = urlDecode(settingsValue(baseProbeExtra));
    expect(baseProbeFlags.find("cabac=0") != std::string::npos &&
               baseProbeFlags.find("cabac=1") == std::string::npos &&
               baseProbeFlags.find("-profile:v baseline") != std::string::npos,
           "E6b baseline probe Extra is Baseline/CAVLC: " + baseProbeFlags);
    expect(plexClientProfileExtra(cab).find("list=main") != std::string::npos &&
               plexClientProfileExtra(cab).find("list=baseline") == std::string::npos,
           "E6 Extra limits list main only");
    // E6c: remote PMS decides the session on the WAN decision. videoProfile=main has to be
    // on that URL. container and videoFrameRate stay off (shared PMS HTTP 400).
    {
        const std::string wan = "http://198.51.100.153:32400";
        const std::string start = buildUniversalTranscodeUrl(wan, "/library/metadata/488", "tok",
                                                             "mplex-1", 0, cab);
        const std::string dec = buildUniversalDecisionUrl(start, "mplex-1", "tok", cab);
        expect(dec.find("videoProfile=main") != std::string::npos &&
                   dec.find("videoCodec=h264") != std::string::npos &&
                   dec.find("container=") == std::string::npos &&
                   dec.find("videoFrameRate") == std::string::npos,
               "E6c High WAN decision carries videoProfile=main and not container: " + dec);
        expect(start.find("directPlay=0") != std::string::npos &&
                   start.find("directStream=0") != std::string::npos,
               "E6c High WAN start refuses direct play: " + start);
        const std::string baseStart =
            buildUniversalTranscodeUrl(wan, "/library/metadata/488", "tok", "mplex-1", 0, ip);
        expect(baseStart.find("directPlay=0") != std::string::npos &&
                   baseStart.find("directStream=0") != std::string::npos,
               "E6c baseline WAN start refuses direct play: " + baseStart);
        const std::string baseDec = buildUniversalDecisionUrl(
            buildUniversalTranscodeUrl(wan, "/library/metadata/488", "tok", "mplex-1", 0, ip),
            "mplex-1", "tok", ip);
        expect(baseDec.find("videoProfile=baseline") != std::string::npos &&
                   baseDec.find("videoProfile=main") == std::string::npos &&
                   baseDec.find("container=") == std::string::npos,
               "E6c baseline WAN decision pins videoProfile=baseline: " + baseDec);
        const std::string lanDec = buildUniversalDecisionUrl(
            buildUniversalTranscodeUrl("http://127.0.0.24:32400", "/library/metadata/488",
                                       "tok", "mplex-1", 0, ip),
            "mplex-1", "tok", ip);
        expect(lanDec.find("location=lan") != std::string::npos &&
                   lanDec.find("videoProfile=baseline") != std::string::npos &&
                   lanDec.find("baseline%2Cmain") == std::string::npos &&
                   lanDec.find("videoProfile=main") == std::string::npos,
               "E6d baseline LAN decision pins videoProfile=baseline: " + lanDec);
        WeakLadder p240;
        std::string why240;
        expect(applyFpgaPlexProfile(p240, "ip", 24000, 1001, true, &why240, "240p", true),
               "E6e 240p profile applies: " + why240);
        p240.videoResolution = "320x240";
        p240.h264Profile = "baseline";
        p240.h264ProfileParam = "baseline";
        p240.highCabac = false;
        const std::string start240 = buildUniversalTranscodeUrl(
            "http://127.0.0.24:32400", "/library/metadata/488", "tok", "mplex-240", 0, p240);
        const std::string dec240 = buildUniversalDecisionUrl(start240, "mplex-240", "tok", p240);
        expect(start240.find("videoResolution=320x240") != std::string::npos &&
                   start240.find("videoProfile=baseline") != std::string::npos &&
                   start240.find("baseline%2Cmain") == std::string::npos &&
                   dec240.find("location=lan") != std::string::npos &&
                   dec240.find("videoResolution=320x240") != std::string::npos &&
                   dec240.find("videoProfile=baseline") != std::string::npos &&
                   dec240.find("baseline%2Cmain") == std::string::npos &&
                   dec240.find("videoProfile=main") == std::string::npos,
               "E6e 240p re-resolve pins baseline at 320x240");
    }
    WeakLadder baselineMain = ip;
    baselineMain.h264Profile = "main";
    expect(!validateWeakLadder(baselineMain, &why), "E6 a baseline core still refuses main at 480p");
    std::printf("test_encode_flags_extra: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
