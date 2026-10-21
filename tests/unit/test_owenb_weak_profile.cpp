#include <cstdlib>
#include <iostream>
#include <string>

#include "plex_resolve.hpp"

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    const auto lan = misterplex::owenbWeakLadder(false);
    const auto wan = misterplex::owenbWeakLadder(true);
    require(lan.maxVideoBitrateKbps == 8000, "LAN cap");
    require(wan.maxVideoBitrateKbps == 6000, "WAN cap");
    {   // BUG1 pre-play: a known-CAVLC source above OB_CAVLC_HARD_MAX_KBPS asks 3400k on the FIRST request
        auto xml = [](const std::string& streamAttrs, const std::string& mediaAttrs) {
            return "<MediaContainer><Video><Media " + mediaAttrs + "><Part><Stream streamType=\"1\" codec=\"h264\" " +
                   streamAttrs + " /><Stream streamType=\"2\" codec=\"aac\" bitrate=\"192\" /></Part></Media></Video></MediaContainer>";
        };
        auto run = [&](const std::string& x, misterplex::WeakLadder w) {
            misterplex::owenbPrePlayCavlcLadder(w, misterplex::parseObSourceEntropy(x), 7000, 4000);
            return w.maxVideoBitrateKbps;
        };
        const auto cbl9000 = xml("profile=\"constrained baseline\" bitrate=\"9000\"", "bitrate=\"9200\"");
        const auto e1 = misterplex::parseObSourceEntropy(cbl9000);
        require(e1.cabac == 0 && e1.bitrateKbps == 9000, "Q0 parse: Constrained Baseline 9000 -> cabac 0, video Stream bitrate");
        require(run(cbl9000, lan) == 3400, "Q1 CAVLC (Baseline) 9000 kbps > 7000 -> first request 3400k (got " +
                                               std::to_string(run(cbl9000, lan)) + ")");
        require(run(xml("profile=\"high\" cabac=\"0\" bitrate=\"7500\"", ""), wan) == 3400,
                "Q2 cabac=\"0\" (High CAVLC) 7500 > 7000 -> 3400k");
        require(run(xml("profile=\"baseline\" bitrate=\"6500\"", ""), lan) == 8000,
                "Q3 CAVLC 6500 <= 7000 -> unchanged (the in-session load gate decides)");
        require(run(xml("profile=\"baseline\" bitrate=\"7000\"", ""), lan) == 8000, "Q3b exactly 7000 -> unchanged");
        require(run(xml("profile=\"high\" cabac=\"1\" bitrate=\"20000\"", ""), lan) == 8000, "Q4 CABAC source 20000 -> unchanged");
        require(run(xml("profile=\"high\" bitrate=\"20000\"", ""), lan) == 8000,
                "Q5 unknown entropy (High, no cabac attr) -> unchanged");
        require(run(xml("bitrate=\"20000\"", ""), lan) == 8000, "Q5b no profile, no cabac -> unchanged");
        auto legacy = lan;
        legacy.owenb = false;
        require(run(cbl9000, legacy) == 8000, "Q6 legacy (FPGA) ladder unaffected");
        const auto mediaOnly = xml("profile=\"baseline\"", "bitrate=\"9100\" videoProfile=\"baseline\"");
        require(misterplex::parseObSourceEntropy(mediaOnly).bitrateKbps == 9100 && run(mediaOnly, lan) == 3400,
                "Q7 no Stream bitrate: Media@bitrate");
    }
    {   // BUG1 (lab 7AE78D80): a non-H.264 source (PMS must transcode) asks OB_TRANSCODE_SAFE_KBPS first
        auto vx = [](const std::string& codec) {
            return "<MediaContainer><Video><Media videoCodec=\"" + codec + "\" bitrate=\"2600\"><Part><Stream streamType=\"1\" codec=\"" +
                   codec + "\" profile=\"main 10\" bitrate=\"2554\" /></Part></Media></Video></MediaContainer>";
        };
        auto ts = [&](const std::string& x, misterplex::WeakLadder w) {
            misterplex::owenbTranscodeSafeLadder(w, misterplex::parseObSourceEntropy(x), 3200);
            return w;
        };
        const auto hevc = ts(vx("hevc"), wan);
        require(hevc.maxVideoBitrateKbps == 3200 && hevc.videoResolution == "1280x720",
                "T1 HEVC Main10 (a remote HEVC title) -> 3200k, 720p kept (got " + std::to_string(hevc.maxVideoBitrateKbps) + ")");
        for (const char* c : {"vp9", "av1", "mpeg2video", "vc1"})
            require(ts(vx(c), lan).maxVideoBitrateKbps == 3200, std::string("T2 ") + c + " -> 3200k");
        require(ts(vx("h264"), lan).maxVideoBitrateKbps == 8000, "T3 H.264 source -> unchanged");
        require(ts("<MediaContainer><Video><Media><Part><Stream streamType=\"1\" /></Part></Media></Video></MediaContainer>", lan)
                        .maxVideoBitrateKbps == 8000,
                "T4 unknown codec -> unchanged");
        auto leg = lan;
        leg.owenb = false;
        require(ts(vx("hevc"), leg).maxVideoBitrateKbps == 8000, "T5 legacy (FPGA) ladder unaffected");
        auto low = wan;
        low.maxVideoBitrateKbps = 2000;
        require(ts(vx("hevc"), low).maxVideoBitrateKbps == 2000, "T6 never raises a lower ladder");
        require(misterplex::plexClientProfileExtra(hevc, true).find("X-Plex-Client-Profile-Name") == std::string::npos &&
                    misterplex::plexRequestProfileName(hevc, "http://203.0.113.8:32400").empty(),
                "T7 no Plex client profile name on the transcode-safe ladder");
    }
    {   // BUG1-CAP: a lab MiSTer R6p retry after "CAVLC at 5228 kbps" asked 6000k again and got 5411 kbps of CAVLC
        const std::string why = "OB: CAVLC at 5228 kbps, above OB_CAVLC_MAX_KBPS=4000";
        const auto low = misterplex::owenbCavlcRetryLadder(misterplex::lowerPmsLadder(wan), why, 4000);
        require(low.maxVideoBitrateKbps == 3400, "B1 CAVLC retry asks 85 % of OB_CAVLC_MAX_KBPS (got " +
                                                     std::to_string(low.maxVideoBitrateKbps) + ")");
        require(misterplex::owenbCavlcRetryLadder(wan, "OB: refused: rewrite", 4000).maxVideoBitrateKbps == 6000,
                "B2 other reasons keep the ladder");
        auto fpga = wan;
        fpga.owenb = false;
        const auto fr = misterplex::owenbCavlcRetryLadder(fpga, why, 4000);
        require(fr.maxVideoBitrateKbps == 6000 && fr.obMaxKbps == 3400,
                "B3 a legacy ladder keeps its own rate and carries the OB-only cap 3400");
        // B5 (lab 7AE78D80): main's ladder is the legacy one; the OB ladder ladderForPlaySession builds honours it.
        auto legacyIn = misterplex::owenbWeakLadder(true);
        legacyIn.owenb = false;
        legacyIn.maxVideoBitrateKbps = 8000;
        legacyIn = misterplex::owenbCavlcRetryLadder(legacyIn, why, 4000);
        const auto obl = misterplex::ladderForPlaySession(misterplex::VideoBackend::OwenbRing, legacyIn, true);
        require(obl.owenb && obl.maxVideoBitrateKbps == 3400 && obl.videoResolution == "1280x720",
                "B5 the R6p CAVLC retry reaches the OB ladder: 3400k at 720p (got " +
                    std::to_string(obl.maxVideoBitrateKbps) + ")");
        require(misterplex::ladderForPlaySession(misterplex::VideoBackend::FpgaH264, legacyIn, true).maxVideoBitrateKbps == 8000,
                "B6 the FPGA backend ignores the OB-only cap");
        require(misterplex::owenbCavlcRetryLadder(wan, why, 800).maxVideoBitrateKbps == 1000, "B4 floor 1000 kbps");
    }
    require(misterplex::plexRequestProfileName(lan, "http://192.0.2.24:32400").empty(),
            "LAN sent a profile name");
    require(misterplex::plexRequestProfileName(wan, "http://203.0.113.8:32400").empty(),
            "WAN sent a profile name");

    const std::string extra = misterplex::plexClientProfileExtra(lan, true);
    require(has(extra, "list=baseline,main,high"), "profile list");
    require(has(extra, "video.bitDepth&value=8"), "bit depth");
    require(has(extra, "video.width&value=1280"), "width");
    require(has(extra, "video.height&value=720"), "height");
    require(has(extra, "video.frameRate&value=30"), "frame rate");
    require(!has(extra, "VideoEncodeFlags"), "encode flags");
    require(!has(extra, "refFrames"), "ref cap");
    require(!has(extra, "MiSTerPlex"), "named profile");

    std::string why;
    const std::string lanUrl = misterplex::buildUniversalTranscodeUrl(
        "http://192.0.2.24:32400", "/library/metadata/6", "unit", "sess", 0, lan, &why);
    const std::string wanUrl = misterplex::buildUniversalTranscodeUrl(
        "http://203.0.113.8:32400", "/library/metadata/6", "unit", "sess", 0, wan, &why);
    require(has(lanUrl, "directPlay=1"), "LAN direct play");
    require(has(lanUrl, "directStream=1"), "LAN direct stream");
    require(has(wanUrl, "directPlay=1"), "WAN direct play");
    require(has(wanUrl, "directStream=1"), "WAN direct stream");
    require(has(wanUrl, "maxVideoBitrate=6000"), "WAN bitrate");
    require(has(wanUrl, "videoProfile=baseline%2Cmain%2Chigh") ||
                has(wanUrl, "videoProfile=baseline,main,high"),
            "WAN profile list");

    auto lowered = wan;
    lowered.pmsLowered = true;
    const std::string retry = misterplex::buildUniversalTranscodeUrl(
        "http://203.0.113.8:32400", "/library/metadata/6", "unit", "sess", 0, lowered, &why);
    require(has(retry, "directPlay=0"), "lowered retry directPlay");
    require(has(retry, "directStream=0"), "lowered retry directStream");

    require(misterplex::owenbCavlcRewriteOk(4000), "4000 kb/s rewrite");
    require(!misterplex::owenbCavlcRewriteOk(4001), "over cap rewrite");
    require(!misterplex::owenbCavlcRewriteOk(0), "unknown bitrate rewrite");

    const std::string hdr = misterplex::plexFfmpegHeaders(
        "sess", "unit", lan, false, "http://203.0.113.8:32400");
    require(has(hdr, "X-Plex-Client-Profile-Extra:"), "ffmpeg extra");
    require(!has(hdr, "X-Plex-Client-Profile-Name"), "ffmpeg name");

    bool probeName = false;
    bool probeExtra = false;
    for (const auto& h : misterplex::plexStartProbeHeaders(
             "sess", "unit", wan, "http://203.0.113.8:32400/video/:/transcode/universal/start.mp4")) {
        if (h.first == "X-Plex-Client-Profile-Name")
            probeName = true;
        if (h.first == "X-Plex-Client-Profile-Extra")
            probeExtra = true;
    }
    require(probeExtra, "probe extra");
    require(!probeName, "probe name");

    misterplex::WeakLadder legacy;
    legacy.profileName = "fpga-h264-480p";
    legacy.videoResolution = "624x480";
    legacy.maxVideoBitrateKbps = 2500;
    legacy.h264Profile = "baseline";
    legacy.h264ProfileParam = "baseline";
    const auto ob = misterplex::ladderForPlaySession(misterplex::VideoBackend::OwenbRing, legacy, false);
    require(ob.owenb, "PlexOB session uses the owenb ladder");
    require(ob.videoResolution == "1280x720", "PlexOB cap is 1280x720");
    require(ob.maxVideoBitrateKbps == 8000, "PlexOB LAN cap");
    const std::string obUrl = misterplex::buildUniversalTranscodeUrl(
        "http://192.0.2.24:32400", "/library/metadata/2", "unit", "sess", 0, ob, &why);
    require(has(obUrl, "directPlay=1"), "PlexOB direct play");
    require(has(obUrl, "directStream=1"), "PlexOB direct stream");
    require(has(obUrl, "videoResolution=1280x720"), "PlexOB url is not 624x480");
    require(has(obUrl, "videoProfile=baseline%2Cmain%2Chigh") ||
                has(obUrl, "videoProfile=baseline,main,high"),
            "PlexOB profile list");
    const auto stay = misterplex::ladderForPlaySession(misterplex::VideoBackend::FpgaH264, legacy, false);
    require(!stay.owenb && stay.videoResolution == "624x480", "legacy session keeps the FPGA ladder");
    legacy.pmsLowered = true;
    const auto sessionRetry = misterplex::ladderForPlaySession(misterplex::VideoBackend::OwenbRing, legacy, true);
    require(sessionRetry.owenb && sessionRetry.pmsLowered && sessionRetry.maxVideoBitrateKbps == 6000,
            "lowered PlexOB retry keeps the flag and the WAN cap");
    const std::string retryUrl = misterplex::buildUniversalTranscodeUrl(
        "http://203.0.113.8:32400", "/library/metadata/2", "unit", "sess", 0, sessionRetry, &why);
    require(has(retryUrl, "directPlay=0"), "lowered PlexOB retry transcodes");

    std::cout << "test_owenb_weak_profile: OK\n";
    return 0;
}
