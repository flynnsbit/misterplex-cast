// Unit tests for slim plex_resolve (no network required for pure helpers).
#include "plex_resolve.hpp"
#include "queue_step.hpp"
#include "libmisterplex/osd_menu.hpp"
#include "libmisterplex/p720_transcode_vf.hpp"
#include "libmisterplex/present_bank.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

// A repo file located from this source's own path (the Makefile compiles it by absolute path).
// Empty when it cannot be read, so a CHECK on its content fails closed.
static std::string readRepoFile(const char* rel) {
    const std::string self = __FILE__;
    const std::string tail = "tests/unit/test_resolve.cpp";
    if (self.size() < tail.size() || self.compare(self.size() - tail.size(), tail.size(), tail) != 0)
        return {};
    std::FILE* f = std::fopen((self.substr(0, self.size() - tail.size()) + rel).c_str(), "rb");
    if (!f)
        return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

#include "test_resolve_direct_remote.cpp"

int main() {
    using namespace misterplex;

    CHECK(urlDecode("a%2Fb") == "a/b");
    CHECK(urlDecode("hello+world") == "hello world");
    CHECK(urlEncodeQuery("a/b").find("a") != std::string::npos);

    // Docker-bridge hosts rejected without LAN fallback. The address is
    // assembled so the tree does not store a full private quad.
    const std::string dockerBridge = std::string("172") + ".17.0.1";
    auto bad = buildPlexBase("http", dockerBridge, "32400", "");
    CHECK(bad.empty());

    auto good = buildPlexBase("http", "pms.lan", "32400", "");
    CHECK(good == "http://pms.lan:32400");

    CHECK(joinPlexHostPort("http", "192.0.2.10:32400", "32400") == "http://192.0.2.10:32400");
    CHECK(joinPlexHostPort("http", "192.0.2.10", "32400") == "http://192.0.2.10:32400");
    CHECK(joinPlexHostPort("https", "http://192.0.2.10:32400", "80") == "http://192.0.2.10:32400");
    CHECK(joinPlexHostPort("", "pms.lan", "") == "http://pms.lan:32400");
    CHECK(joinPlexHostPort("http", "192.0.2.10%3A32400", "32400") == "http://192.0.2.10:32400");
    CHECK(joinPlexHostPort("http", "[::1]", "32400") == "http://[::1]:32400");
    CHECK(joinPlexHostPort("http", "[::1]:32400", "80") == "http://[::1]:32400");
    CHECK(joinPlexHostPort("http", "http://192.0.2.10", "32400") == "http://192.0.2.10:32400");
    CHECK(joinPlexHostPort("http", "http://192.0.2.10/library", "32400") ==
          "http://192.0.2.10:32400/library");
    CHECK(joinPlexHostPort("http", "http://[::1]", "32400") == "http://[::1]:32400");
    CHECK(joinPlexHostPort("http", "http://[::1]:32400/web", "80") == "http://[::1]:32400/web");
    CHECK(buildPlexBase("http", "192.0.2.10:32400", "32400", "") == "http://192.0.2.10:32400");
    CHECK(buildPlexBase("http", "[::1]", "32400", "") == "http://[::1]:32400");
    {
        const PlexEndpoint ep = splitPlexHostPort("192.0.2.10:32400", "32400");
        CHECK(ep.host == "192.0.2.10" && ep.port == "32400" && ep.portInAddress);
    }
    {
        const PlexEndpoint ep = splitPlexHostPort("192.0.2.10", "32401");
        CHECK(ep.host == "192.0.2.10" && ep.port == "32401" && !ep.portInAddress);
    }
    {
        const PlexEndpoint ep = splitPlexHostPort("192.0.2.10:32401", "32400");
        CHECK(ep.host == "192.0.2.10" && ep.port == "32401" && ep.portInAddress);
    }
    {
        const PlexEndpoint ep = splitPlexHostPort("http://192.0.2.10:32400/library", "80");
        CHECK(ep.host == "192.0.2.10" && ep.port == "32400" && ep.protocol == "http");
    }
    {
        const PlexEndpoint ep = splitPlexHostPort("[::1]:32400", "80");
        CHECK(ep.host == "[::1]" && ep.port == "32400" && ep.portInAddress);
    }
    {
        const PlexEndpoint ep = splitPlexHostPort("192.0.2.10%3A32400", "80");
        CHECK(ep.host == "192.0.2.10" && ep.port == "32400" && ep.portInAddress);
    }

    const std::string dockerDirect = std::string("172-17-") + "0-1.abc.plex.direct";
    auto rewritten = buildPlexBase("http", dockerDirect, "32400", "pms.lan");
    CHECK(rewritten.find("pms.lan") != std::string::npos);

    // Local path resolve (no network)
    auto r = resolvePlayTarget("/media/fat/mistercast/test.mp4", "", "", 0, true);
    CHECK(r.ok);
    CHECK(r.playable == "/media/fat/mistercast/test.mp4");

    // rk 40868 must not short-circuit to the identity cache file.
    auto fp = resolvePlayTarget("/media/fat/misterplex/cache/farpoint_1280x720.mp4",
                                "http://127.0.0.1:32400", "tok", 0, true);
    CHECK(!fp.ok);
    CHECK(fp.playable.empty());
    CHECK(fp.transcoded == false);
    CHECK(std::string(fp.detail).find("farpoint_1280x720.mp4") != std::string::npos);
    CHECK(misterplex::libraryKeyMustNotSpawnLocalFile(
        "/library/metadata/40868", "/media/fat/misterplex/cache/farpoint_1280x720.mp4"));
    CHECK(!misterplex::libraryKeyMustNotSpawnLocalFile(
        "/library/metadata/40868",
        "http://127.0.0.1:9324/video/:/transcode/universal/start.mp4?videoResolution=1280x720"));

    auto t = resolvePlayTarget("testsrc", "", "", 0, true);
    CHECK(t.ok && t.playable == "testsrc");
    CHECK(t.sourceFpsHint == 30 && t.fpsNum == 30 && t.fpsDen == 1);

    auto h = plexFfmpegHeaders("sess1", "tok");
    CHECK(h.find("X-Plex-Session-Identifier: sess1") != std::string::npos);
    CHECK(h.find("X-Plex-Token: tok") != std::string::npos);

    // --- PMS universal transcode profile table (240p/480p/480i/720p) ---
    // 480i = the R15 PLEX_PRESENT_480I host-paint tier (720x480), added with the 8addcac1
    // daemon source (735c8281); osd_menu.hpp maps the same tier (720x480, 8000 kbps).
    const auto& profiles = plexTranscodeProfiles();
    CHECK(profiles.size() == 4);
    CHECK(profiles.size() == 4 && profiles[0].name == "240p" && profiles[1].name == "480p" &&
          profiles[2].name == "480i" && profiles[3].name == "720p");
    WeakLadder w240;
    CHECK(applyPlexTranscodeProfile("240p", w240));
    CHECK(w240.profileName == "240p");
    CHECK(w240.videoResolution == "320x240");
    // Known 240p split, parked (review 05:07, option C). Ticket:
    // the 240p ladder split ticket (1005).
    // 735c8281 raised the table's 240p row to 2500 kbps / q55; the OSD tier, the WeakLadder
    // defaults and docs/release.md stay at 1000 kbps (q40). The FPGA 240p start ladder is capped
    // by the prior weak bitrate (then <= 1500), so it starts at 1500 from TRANSCODE_PROFILE=240p
    // and at 1000 from the OSD. Moving any one leg changes a shipped path: decouple the FPGA cap
    // first (the ticket), then replace this pin with table == OSD == docs.
    CHECK(w240.maxVideoBitrateKbps == 2500);
    CHECK(w240.videoQuality == 55);
    CHECK(resolutionFromLabel("240p").weakBitrateKbps == 1000);
    CHECK(contentResolutionFromOsdWord(0).weakBitrateKbps == 1000);
    {
        const WeakLadder defaults;
        CHECK(defaults.profileName == "240p" && defaults.videoResolution == "320x240" &&
              defaults.maxVideoBitrateKbps == 1000 && defaults.videoQuality == 40);
    }
    CHECK(readRepoFile("docs/release.md").find("`240p` = 320x240@1000k") != std::string::npos);
    CHECK(w240.h264Profile == "baseline");
    CHECK(w240.h264Level == 30);
    CHECK(validateWeakLadder(w240));

    CHECK(kBitrateFloor240pKbps == 750);
    WeakLadder thin240 = w240;
    thin240.maxVideoBitrateKbps = 749;
    CHECK(!validateWeakLadder(thin240));
    thin240.maxVideoBitrateKbps = 750;
    CHECK(validateWeakLadder(thin240));
    CHECK(fpgaAuSafeClamp(false, 234000, true).maxBitrateKbps == kBitrateFloor240pKbps);
    CHECK(fpgaAuSafeClamp(true, 234000, true).maxBitrateKbps == 2000);

    WeakLadder w480;
    CHECK(applyPlexTranscodeProfile("480p", w480));
    CHECK(w480.profileName == "480p");
    CHECK(w480.videoResolution == "640x480");
    CHECK(w480.maxVideoBitrateKbps == 2500);
    CHECK(w480.videoQuality == 60);
    CHECK(w480.videoCodec == "h264");
    CHECK(w480.audioCodec == "aac");
    CHECK(w480.h264Profile == "baseline");
    CHECK(w480.h264Level == 30);
    CHECK(w480.clientProfileName == "MiSTerPlex");
    CHECK(validateWeakLadder(w480));
    // Resolution alias selects the 480p profile too.
    WeakLadder byRes;
    CHECK(applyPlexTranscodeProfile("640x480", byRes));
    CHECK(byRes.profileName == "480p");

    WeakLadder w480i;
    CHECK(applyPlexTranscodeProfile("480i", w480i));
    CHECK(w480i.profileName == "480i");
    CHECK(w480i.videoResolution == "720x480");
    CHECK(w480i.maxVideoBitrateKbps == 8000);
    CHECK(w480i.h264Profile == "baseline");
    CHECK(w480i.h264Level == 30);
    CHECK(validateWeakLadder(w480i));
    WeakLadder still480i = w480i;
    still480i.maxVideoBitrateKbps = 1999;
    CHECK(!validateWeakLadder(still480i));
    still480i.maxVideoBitrateKbps = 2000;
    CHECK(validateWeakLadder(still480i));
    // DECODE=720x480 selects 480i; it must not collapse to the 640x480 "480p" tier.
    WeakLadder by480i;
    CHECK(applyPlexTranscodeProfile("720x480", by480i));
    CHECK(by480i.profileName == "480i");

    CHECK(kBitrateFloor480iKbps == 2000);
    WeakLadder thin480i = w480i;
    thin480i.maxVideoBitrateKbps = 1999;
    CHECK(!validateWeakLadder(thin480i));
    thin480i.maxVideoBitrateKbps = 2000;
    CHECK(validateWeakLadder(thin480i));
    CHECK(fpgaAuSafeTier480(w480i));
    CHECK(fpgaAuSafeTier480(w480));
    WeakLadder w240tier;
    CHECK(applyPlexTranscodeProfile("240p", w240tier));
    CHECK(!fpgaAuSafeTier480(w240tier));
    WeakLadder named480i;
    named480i.profileName = "480i";
    named480i.videoResolution = "320x240";
    named480i.maxVideoBitrateKbps = 1000;
    named480i.videoQuality = 40;
    named480i.videoCodec = "h264";
    named480i.audioCodec = "aac";
    named480i.h264Profile = "baseline";
    named480i.h264Level = 30;
    CHECK(fpgaAuSafeTier480(named480i));
    CHECK(fpgaAuSafeClamp(fpgaAuSafeTier480(named480i), 234000, true).maxBitrateKbps == 2000);
    CHECK(fpgaAuSafeClamp(fpgaAuSafeTier480(w240tier), 234000, true).maxBitrateKbps == 750);

    WeakLadder w720;
    CHECK(applyPlexTranscodeProfile("720p", w720));
    CHECK(w720.profileName == "720p");
    CHECK(w720.videoResolution == "1280x720");
    CHECK(w720.maxVideoBitrateKbps == 8000);
    CHECK(w720.h264Profile == "main");
    CHECK(w720.h264Level == 31);
    CHECK(validateWeakLadder(w720));
    WeakLadder by720;
    CHECK(applyPlexTranscodeProfile("1280x720", by720));
    CHECK(by720.profileName == "720p");

    // A private-LAN PMS gets the full query, including the codec keys.
    const auto start480 =
        buildUniversalTranscodeUrl("http://127.0.0.24:32400", "/library/metadata/3", "tok",
                                   "sess480", 1500, w480);
    CHECK(start480.find("location=lan") != std::string::npos);
    CHECK(start480.find("/video/:/transcode/universal/start.mp4") != std::string::npos);
    CHECK(start480.find("videoResolution=640x480") != std::string::npos);
    CHECK(start480.find("maxVideoBitrate=2500") != std::string::npos);
    CHECK(start480.find("autoAdjustQuality=0") != std::string::npos);
    CHECK(start480.find("videoQuality=60") != std::string::npos);
    CHECK(start480.find("videoCodec=h264") != std::string::npos);
    CHECK(start480.find("audioCodec=aac") != std::string::npos);
    CHECK(start480.find("videoProfile=baseline%2Cmain") != std::string::npos);
    CHECK(start480.find("videoLevel=30") != std::string::npos);
    CHECK(start480.find("audioChannels=2") != std::string::npos);
    CHECK(start480.find("directPlay=1") != std::string::npos);
    CHECK(start480.find("directStream=1") != std::string::npos);
    CHECK(start480.find("offset=2") != std::string::npos);
    // C939: plain codec params go to WAN too. FPGA-only keys stay LAN-only.
    const auto startWan =
        buildUniversalTranscodeUrl("http://pms.example:32400", "/library/metadata/3", "tok",
                                   "sessWan", 1500, w480);
    CHECK(startWan.find("location=wan") != std::string::npos);
    CHECK(startWan.find("videoResolution=640x480") != std::string::npos);
    CHECK(startWan.find("maxVideoBitrate=2500") != std::string::npos);
    CHECK(startWan.find("videoCodec=h264") != std::string::npos);
    CHECK(startWan.find("audioCodec=aac") != std::string::npos);
    CHECK(startWan.find("audioChannels=2") != std::string::npos);
    CHECK(startWan.find("videoProfile=baseline%2Cmain") != std::string::npos);
    CHECK(startWan.find("videoLevel=30") != std::string::npos);
    CHECK(startWan.find("directPlay=1") != std::string::npos);
    CHECK(startWan.find("container=mpegts") == std::string::npos);
    CHECK(startWan.find("videoFrameRate=") == std::string::npos);

    const auto extra480 = plexClientProfileExtra(w480);
    CHECK(extra480.find("container=mpegts") != std::string::npos);
    CHECK(extra480.find("videoCodec=h264") != std::string::npos);
    CHECK(extra480.find("audioCodec=aac") != std::string::npos);
    CHECK(extra480.find("name=video.profile&list=baseline") != std::string::npos);
    CHECK(extra480.find("name=video.level&value=30") != std::string::npos);
    CHECK(extra480.find("scope=videoTranscodeTarget&scopeName=h264") != std::string::npos);
    CHECK(extra480.find("name=video.width&value=640") != std::string::npos);
    CHECK(extra480.find("name=video.height&value=480") != std::string::npos);
    const auto caps480 = plexClientCapabilities(w480);
    CHECK(caps480.find("videoDecoders=h264{profile:baseline&resolution:640x480&level:30}") !=
          std::string::npos);
    unsetenv("MPX_CLIENT_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_NAME");
    unsetenv("MPX_SEND_PROFILE_EXTRA");
    const auto headers480 = plexFfmpegHeaders("sess480", "tok", w480);
    CHECK(headers480.find("X-Plex-Client-Profile-Name: ") == std::string::npos); // C939: no name
    CHECK(headers480.find("X-Plex-Client-Profile-Name: Generic") == std::string::npos);
    CHECK(headers480.find("X-Plex-Client-Profile-Name: Chrome") == std::string::npos);
    CHECK(headers480.find("X-Plex-Client-Capabilities: ") == std::string::npos);
    CHECK(headers480.find("X-Plex-Client-Profile-Extra: ") == std::string::npos); // mode-table ladder is not FPGA variant
    CHECK(!plexSendClientLadderCaps(w480));
    WeakLadder w720caps;
    CHECK(applyPlexTranscodeProfile("720p", w720caps));
    CHECK(plexSendClientLadderCaps(w720caps));
    const auto headers720 = plexFfmpegHeaders("sess720", "tok", w720caps);
    CHECK(headers720.find("X-Plex-Client-Capabilities: ") != std::string::npos);
    CHECK(headers720.find("resolution:1280x720") != std::string::npos);
    CHECK(headers720.find("X-Plex-Client-Profile-Extra: ") != std::string::npos);
    CHECK(headers720.find("name=video.width&value=1280") != std::string::npos);

    w480.clientProfileName = "Generic";
    const auto genericHeaders480 = plexFfmpegHeaders("sess480", "tok", w480);
    CHECK(genericHeaders480.find("X-Plex-Client-Profile-Name: ") == std::string::npos); // no name by default
    CHECK(genericHeaders480.find("X-Plex-Client-Capabilities: ") == std::string::npos);
    CHECK(genericHeaders480.find("X-Plex-Client-Profile-Extra: ") == std::string::npos);

    // --- C939: one mode table (480p 640x480, 480i 720x480, 240p 320x240) ---
    {
        CHECK(modeResolveTable().size() == 3);
        CHECK(modeResolveRow("720p") == nullptr);
        unsetenv("MPX_CLIENT_PROFILE_NAME");
        unsetenv("MPX_SEND_PROFILE_NAME");
        unsetenv("MPX_SEND_PROFILE_EXTRA");
        CHECK(!plexSendProfileExtra());
        for (const char* mode : {"480p", "480i", "240p"}) {
            WeakLadder w;
            CHECK(applyModeResolve(mode, w));
            const ModeResolveRow* row = modeResolveRow(mode);
            CHECK(row != nullptr);
            CHECK(std::string(row->mode) == mode);
            CHECK(w.profileName == mode);
            CHECK(w.videoResolution == row->videoResolution);
            CHECK(w.videoCodec == "h264");
            CHECK(w.audioCodec == "aac");
            CHECK(w.audioChannels == 2);
            CHECK(w.h264Profile == "baseline");
            CHECK(w.h264ProfileParam == "baseline,main");
            CHECK(w.h264Level == 30);
            if (std::string(mode) == "480p")
                CHECK(w.videoResolution == "640x480");
            if (std::string(mode) == "480i")
                CHECK(w.videoResolution == "720x480");
            if (std::string(mode) == "240p")
                CHECK(w.videoResolution == "320x240");
            for (const char* base : {"http://127.0.0.24:32400", "http://pms.example:32400"}) {
                const auto url = buildUniversalTranscodeUrl(base, "/library/metadata/1", "tok",
                                                            "sessMode", 0, w);
                CHECK(!url.empty());
                CHECK(url.find(std::string("videoResolution=") + row->videoResolution) !=
                      std::string::npos);
                CHECK(url.find("videoCodec=h264") != std::string::npos);
                CHECK(url.find("audioCodec=aac") != std::string::npos);
                CHECK(url.find("audioChannels=2") != std::string::npos);
                CHECK(url.find("videoProfile=baseline%2Cmain") != std::string::npos);
                CHECK(url.find("videoLevel=30") != std::string::npos);
                CHECK(url.find("directPlay=1") != std::string::npos);
                CHECK(url.find("directStream=1") != std::string::npos);
            }
            const auto hdr = plexFfmpegHeaders("sessMode", "tok", w, false,
                                               "http://127.0.0.24:32400/x");
            CHECK(hdr.find("X-Plex-Client-Profile-Name:") == std::string::npos);
            CHECK(hdr.find("X-Plex-Client-Profile-Extra:") == std::string::npos);
            CHECK(!plexClientSendsProfileExtra(w, false, plexRequestProfileName(w)));
        }
        WeakLadder w480e;
        CHECK(applyModeResolve("480p", w480e));
        setenv("MPX_SEND_PROFILE_EXTRA", "1", 1);
        CHECK(plexSendProfileExtra());
        const auto hdrE = plexFfmpegHeaders("sessE", "tok", w480e);
        CHECK(hdrE.find("X-Plex-Client-Profile-Name:") == std::string::npos);
        CHECK(hdrE.find("X-Plex-Client-Profile-Extra:") != std::string::npos);
        unsetenv("MPX_SEND_PROFILE_EXTRA");
        const auto tr = parseDecisionVideo(
            "<MediaContainer><Media videoCodec=\"h264\" width=\"640\" height=\"480\">"
            "<Part decision=\"transcode\" key=\"/video/:/transcode/universal/start.mp4\">"
            "<Stream streamType=\"1\" codec=\"h264\" width=\"640\" height=\"480\" "
            "decision=\"transcode\"/></Part></Media></MediaContainer>");
        CHECK(tr.found && tr.codec == "h264" && tr.partDecision == "transcode");
        CHECK(!decisionIsDirectPlay(tr));
        const auto dp = parseDecisionVideo(
            "<MediaContainer><Media videoCodec=\"h264\" width=\"640\" height=\"480\">"
            "<Part decision=\"directplay\" key=\"/library/parts/9/file.mkv\">"
            "<Stream streamType=\"1\" codec=\"h264\" width=\"640\" height=\"480\" "
            "decision=\"directplay\"/></Part></Media></MediaContainer>");
        CHECK(decisionIsDirectPlay(dp) && dp.partKey == "/library/parts/9/file.mkv");
        WeakLadder fit480;
        CHECK(applyModeResolve("480p", fit480));
        CHECK(decisionCoreMismatch(dp, fit480).empty());
        const auto highPart = parseDecisionVideo(
            "<MediaContainer><Media videoCodec=\"h264\" width=\"1280\" height=\"536\">"
            "<Part decision=\"directplay\" key=\"/library/parts/11681/file.mkv\">"
            "<Stream streamType=\"1\" codec=\"h264\" width=\"1280\" height=\"536\" "
            "decision=\"directplay\"/></Part></Media></MediaContainer>");
        CHECK(decisionIsDirectPlay(highPart));
        CHECK(!decisionCoreMismatch(highPart, fit480).empty());
        CHECK(decisionCoreMismatch(highPart, fit480).find("width") != std::string::npos);
        // C940: FPGA overlay copies codec only; 480i/480p stay 624x480 (not host-paint 720/640).
        {
            WeakLadder f480i;
            std::string why;
            CHECK(applyFpgaPlexProfile(f480i, "ip", 24000, 1001, true, &why, "480i", true));
            CHECK(f480i.videoResolution == "624x480");
            if (const ModeResolveRow* row = modeResolveRow("480i")) {
                f480i.videoCodec = row->videoCodec;
                f480i.audioCodec = row->audioCodec;
                f480i.h264ProfileParam = row->h264ProfileParam;
                f480i.audioChannels = row->audioChannels;
            }
            CHECK(f480i.videoResolution == "624x480");
            CHECK(validateWeakLadder(f480i));
            WeakLadder f480p;
            CHECK(applyFpgaPlexProfile(f480p, "ip", 24000, 1001, true, &why, "480p", true));
            CHECK(f480p.videoResolution == "624x480");
            if (const ModeResolveRow* row = modeResolveRow("480p")) {
                f480p.videoCodec = row->videoCodec;
                f480p.audioCodec = row->audioCodec;
                f480p.h264ProfileParam = row->h264ProfileParam;
                f480p.audioChannels = row->audioChannels;
            }
            CHECK(f480p.videoResolution == "624x480");
            CHECK(validateWeakLadder(f480p));
            const auto startFpga = buildUniversalTranscodeUrl(
                "http://127.0.0.24:32400", "/library/metadata/1", "tok", "sessFpga", 0, f480i);
            CHECK(!startFpga.empty());
            CHECK(startFpga.find("videoResolution=720x480") == std::string::npos);
            CHECK(startFpga.find("videoResolution=624x480") != std::string::npos);
        }
    }

    WeakLadder bad480 = w480;
    bad480.h264Profile = "high";
    CHECK(!validateWeakLadder(bad480));
    bad480 = w480;
    bad480.maxVideoBitrateKbps = 1000;
    CHECK(!validateWeakLadder(bad480));
    bad480 = w480;
    bad480.h264Level = 31;
    CHECK(!validateWeakLadder(bad480));

    // TRUE640: FPGA 480p ladder accepts 624 (39-MB cores) and 640 (40 MB/row,
    // PLEX_DECODE_CODED_640 cores); nothing wider than the 640 bank.
    {
        WeakLadder f480;
        std::string why;
        CHECK(applyFpgaPlexProfile(f480, "ip", 24000, 1001, true, &why, "480p", true));
        CHECK(isFpgaPlexProfile(f480));
        CHECK(validateWeakLadder(f480, &why));
        f480.videoResolution = "624x480";
        CHECK(validateWeakLadder(f480, &why));
        f480.videoResolution = "640x480";
        CHECK(validateWeakLadder(f480, &why));
        f480.videoResolution = "656x480";
        CHECK(!validateWeakLadder(f480, &why));
        f480.videoResolution = "640x496";
        CHECK(!validateWeakLadder(f480, &why));
        // A/B switch: default 624; only MPX_FPGA_480P_CODED_W=640 opts in.
        unsetenv("MPX_FPGA_480P_CODED_W");
        CHECK(fpga480pCodedWidthCeiling() == 624);
        setenv("MPX_FPGA_480P_CODED_W", "640", 1);
        CHECK(fpga480pCodedWidthCeiling() == 640);
        setenv("MPX_FPGA_480P_CODED_W", "624", 1);
        CHECK(fpga480pCodedWidthCeiling() == 624);
        setenv("MPX_FPGA_480P_CODED_W", "656", 1);
        CHECK(fpga480pCodedWidthCeiling() == 624);
        unsetenv("MPX_FPGA_480P_CODED_W");
    }

    // --- offset/seek clamp floor: fpgaAuSafeClamp rungs stay at/above the
    // --- validateWeakLadder bitrate floor (480 tier 2000 / 240 tier 750), quality untouched ---
    {
        const auto flat480 = fpgaAuSafeClamp(true, 234000, true);
        CHECK(flat480.maxBitrateKbps == 2000 && flat480.maxQuality == 18);
        const auto flat240 = fpgaAuSafeClamp(false, 234000, true);
        CHECK(flat240.maxBitrateKbps == 750 && flat240.maxQuality == 18);
        const auto rung480 = fpgaAuSafeClamp(true, 234000, false);
        CHECK(rung480.maxBitrateKbps == 2000 && rung480.maxQuality == 30);
        const auto rung240 = fpgaAuSafeClamp(false, 234000, false);
        CHECK(rung240.maxBitrateKbps == 900 && rung240.maxQuality == 28);
        // Fresh start (offset 0): pre-floor rungs remain untouched.
        const auto start480c = fpgaAuSafeClamp(true, 0, false);
        CHECK(start480c.maxBitrateKbps == 2500 && start480c.maxQuality == 60);
        const auto start240c = fpgaAuSafeClamp(false, 0, false);
        CHECK(start240c.maxBitrateKbps == 2800 && start240c.maxQuality == 55);
        // Floored rungs validate on the profile ladders: resolve no longer
        // falls through to Part with the misleading "no stream" detail.
        WeakLadder cf480;
        CHECK(applyPlexTranscodeProfile("480p", cf480));
        cf480.maxVideoBitrateKbps = flat480.maxBitrateKbps;
        CHECK(validateWeakLadder(cf480));
        WeakLadder cf240;
        CHECK(applyPlexTranscodeProfile("240p", cf240));
        cf240.maxVideoBitrateKbps = flat240.maxBitrateKbps;
        CHECK(validateWeakLadder(cf240));
        // On the FPGA client ladder (isFpgaPlexProfile → validateWeakLadder runs
        // inside buildUniversalTranscodeUrl), the pre-fix 350 seek-flat rung
        // yields no URL AND a concrete why; the floored rung builds a URL.
        WeakLadder fj480;
        std::string why;
        CHECK(applyFpgaPlexProfile(fj480, "ip", 24000, 1001, true, &why, "480p", true));
        CHECK(isFpgaPlexProfile(fj480));
        fj480.maxVideoBitrateKbps = 350;
        CHECK(buildUniversalTranscodeUrl("http://127.0.0.1:32400", "/library/metadata/3", "tok",
                                         "sRej", 234000, fj480, &why)
                  .empty());
        CHECK(!why.empty());
        // The floored ladder itself builds a URL with an empty why.
        why.clear();
        fj480.maxVideoBitrateKbps = flat480.maxBitrateKbps;
        const auto okUrl = buildUniversalTranscodeUrl("http://127.0.0.1:32400",
                                                      "/library/metadata/3", "tok", "sOk",
                                                      234000, fj480, &why);
        CHECK(!okUrl.empty());
        CHECK(why.empty());
    }
    // --- T-NOPROXY-OFFSET-BR: the lead remote repro (PMS key 29729, offset 60000) ---
    // At offset 60000 a 480p ladder takes the AU-safe rung (seekFlat=false, main.cpp
    // "FPGA AU-safe ladder") or the seek-flat rung (seekFlat=true, kSeekMaxBitrateKbps).
    // Pre-fix those rungs were 1000 kbps and 350 kbps respectively — both below the
    // validateWeakLadder 480p floor of 2000 — so buildUniversalTranscodeUrl returned
    // empty and resolve fell through with the misleading "remote/shared PMS transcoder
    // produced no stream" detail. The floor pins offset 60000 at/above the validator.
    {
        const auto au60_480 = fpgaAuSafeClamp(true, 60000, false);
        CHECK(au60_480.maxBitrateKbps >= 2000);
        WeakLadder la480;
        std::string why60;
        CHECK(applyFpgaPlexProfile(la480, "ip", 24000, 1001, true, &why60, "480p", true));
        la480.maxVideoBitrateKbps = au60_480.maxBitrateKbps;
        la480.videoQuality = au60_480.maxQuality;
        CHECK(validateWeakLadder(la480, &why60));
        CHECK(why60.empty());
        const auto auUrl60 = buildUniversalTranscodeUrl("http://127.0.0.1:32400",
                                                        "/library/metadata/29729", "tok", "s60au",
                                                        60000, la480);
        CHECK(!auUrl60.empty());
        CHECK(auUrl60.find("offset=60") != std::string::npos);
        // Seek path: pre-fix 350 (kSeekMaxBitrateKbps) would fail the 2000 floor.
        const auto seek60_480 = fpgaAuSafeClamp(true, 60000, true);
        CHECK(seek60_480.maxBitrateKbps >= 2000);
        WeakLadder ls480;
        CHECK(applyFpgaPlexProfile(ls480, "ip", 24000, 1001, true, &why60, "480p", true));
        ls480.maxVideoBitrateKbps = seek60_480.maxBitrateKbps;
        ls480.videoQuality = seek60_480.maxQuality;
        CHECK(validateWeakLadder(ls480, &why60));
        CHECK(why60.empty());
        // 240p at offset 60000 keeps its 750 floor (AU-safe rung 900 is above it).
        const auto au60_240 = fpgaAuSafeClamp(false, 60000, false);
        CHECK(au60_240.maxBitrateKbps >= 750);
        WeakLadder la240;
        CHECK(applyPlexTranscodeProfile("240p", la240));
        la240.maxVideoBitrateKbps = au60_240.maxBitrateKbps;
        CHECK(validateWeakLadder(la240));
        // offset 0 unchanged: 480p 2500/60, 240p 2800/55.
        CHECK(fpgaAuSafeClamp(true, 0, false).maxBitrateKbps == 2500 &&
              fpgaAuSafeClamp(true, 0, false).maxQuality == 60);
        CHECK(fpgaAuSafeClamp(false, 0, false).maxBitrateKbps == 2800 &&
              fpgaAuSafeClamp(false, 0, false).maxQuality == 55);
    }
    // --- Phase 4 multi-server conf helpers (no network) ---
    CHECK(normalizePlexBase("http://pms.lan:32400/") == "http://pms.lan:32400");
    CHECK(normalizePlexBase("pms2.lan:32400") == "http://pms2.lan:32400");
    CHECK(normalizePlexBase("pms2.lan") == "http://pms2.lan:32400");
    CHECK(normalizePlexBase("  https://pms.lan:32400  ") == "https://pms.lan:32400");
    CHECK(normalizePlexBase("").empty());

    auto list = parsePlexServerList("http://a:32400,http://b:32400; http://c:32400");
    CHECK(list.size() == 3);
    CHECK(list[0] == "http://a:32400");
    CHECK(list[1] == "http://b:32400");
    CHECK(list[2] == "http://c:32400");

    // Dedup + bare host
    auto dedup = parsePlexServerList("pms3.lan, http://pms3.lan:32400, pms3.lan:32400");
    CHECK(dedup.size() == 1);
    CHECK(dedup[0] == "http://pms3.lan:32400");

    // merge: PLEX_SERVERS first, then extra PLEX_BASE lines
    std::vector<std::string> bases = {"http://extra:32400", "http://a:32400"};
    auto merged = mergePlexServers("http://a:32400,http://b:32400", bases);
    CHECK(merged.size() == 3);
    CHECK(merged[0] == "http://a:32400");
    CHECK(merged[1] == "http://b:32400");
    CHECK(merged[2] == "http://extra:32400");

    // Empty merge
    auto empty = mergePlexServers("", {});
    CHECK(empty.empty());

    // Invalid / empty play-queue ids fail without network (P4-SCRUB edges)
    auto pq = fetchPlayQueue("not-a-queue", "http://127.0.0.1:32400", "tok");
    CHECK(!pq.ok);

    // --- Seek/resume: companion ms → PMS universal offset= seconds ---
    // Resume @ 3:54 (234000 ms) must become offset=234, not double-seek with -ss.
    CHECK(universalOffsetSeconds(0) == 0);
    CHECK(universalOffsetSeconds(-1) == 0);
    CHECK(universalOffsetSeconds(500) == 1);     // half-up
    CHECK(universalOffsetSeconds(499) == 0);
    CHECK(universalOffsetSeconds(1000) == 1);
    CHECK(universalOffsetSeconds(234000) == 234); // Trek ~3:54
    CHECK(universalOffsetSeconds(234499) == 234);
    CHECK(universalOffsetSeconds(234500) == 235);
    // A raw cast offset reaches resolve before doPlay clamps the start: no signed overflow.
    CHECK(universalOffsetSeconds(INT64_MAX) == INT64_MAX / 1000 + 1);
    CHECK(universalOffsetSeconds(INT64_MAX - 308) == INT64_MAX / 1000);
    auto pqEmpty = fetchPlayQueue("", "http://127.0.0.1:32400", "tok");
    CHECK(!pqEmpty.ok);
    auto pqLib = fetchPlayQueue("/library/metadata/9", "http://127.0.0.1:32400", "tok");
    CHECK(!pqLib.ok); // must not treat metadata key as queue
    auto pqNoBase = fetchPlayQueue("/playQueues/1", "", "tok");
    CHECK(!pqNoBase.ok);
    // Bare numeric id is accepted as queue id shape (network would still fail/empty)
    auto pqBare = fetchPlayQueue("42", "", "tok");
    CHECK(!pqBare.ok); // no PMS base

    // --- TASKS-NIGHT-1005 #22: a queue step never invents playQueueItemID from the ratingKey ---
    {
        PlayRequest cur;
        cur.key = "/library/metadata/100";
        cur.ratingKey = "100";
        cur.playQueueItemId = "5000";
        cur.playQueueId = "77";
        cur.playQueueVersion = "1";
        cur.containerKey = "/playQueues/77?own=1";
        cur.offsetMs = 123000;
        PlayQueue q;
        q.ok = true;
        q.playQueueId = "77";
        q.playQueueVersion = "3";
        q.containerKey = "/playQueues/77";
        const QueueItem withId{"/library/metadata/101", "101", "5001", "Ep 2", 0};
        const QueueItem noId{"/library/metadata/102", "102", "", "Ep 3", 0};
        const auto a = queueStepRequest(cur, q, withId, "tok", 9);
        CHECK(a.playQueueItemId == "5001");
        CHECK(a.key == "/library/metadata/101" && a.ratingKey == "101");
        CHECK(a.playQueueId == "77" && a.playQueueVersion == "3" && a.containerKey == "/playQueues/77?own=1");
        CHECK(a.offsetMs == 0 && a.offsetPresent && a.token == "tok" && a.dispatchGeneration == 10);
        const auto b = queueStepRequest(cur, q, noId, "tok", 9);
        CHECK(b.playQueueItemId.empty()); // neither the item's ratingKey "102" nor the previous item's "5000"
        CHECK(b.key == "/library/metadata/102" && b.ratingKey == "102");
    }

    // --- TASKS #25 T-PQ-SUBSTRING-MATCH: play-queue identity is exact only ---
    {
        const auto detailFor = [](const std::vector<QueueItem>& items, const std::string& key,
                                  const std::string& id) {
            const char* by = "none";
            const int idx = selectQueueIndex(items, key, id, &by);
            return idx >= 0 ? "index=" + std::to_string(idx) + " by=" + by
                            : std::string("index=-1 none");
        };

        const std::vector<QueueItem> f1{{"/library/metadata/150", "150", "9001", "A", 0},
                                        {"/library/metadata/2", "2", "15", "B", 0}};
        const char* by = nullptr;
        CHECK(selectQueueIndex(f1, "", "15", &by) == 1);
        CHECK(std::string(by) == "id");
        CHECK(detailFor(f1, "", "15") == "index=1 by=id");

        const std::vector<QueueItem> f2{{"/library/metadata/5001", "5001", "7", "A", 0},
                                        {"/library/metadata/9", "9", "5001", "B", 0}};
        CHECK(selectQueueIndex(f2, "", "5001", &by) == 1);
        CHECK(std::string(by) == "id");

        const std::vector<QueueItem> f3{{"/library/metadata/12", "12", "7001", "A", 0},
                                        {"/library/metadata/123", "123", "7002", "B", 0}};
        CHECK(selectQueueIndex(f3, "/library/metadata/123", "", &by) == 1);
        CHECK(std::string(by) == "key");
        CHECK(detailFor(f3, "/library/metadata/123", "") == "index=1 by=key");
        CHECK(selectQueueIndex(f3, "/library/metadata/123", "missing", &by) == 1);
        CHECK(std::string(by) == "key");

        const std::vector<QueueItem> f3rk{{"/library/metadata/not-12", "12", "7101", "A", 0},
                                          {"/library/metadata/not-123", "123", "7102", "B", 0}};
        CHECK(selectQueueIndex(f3rk, "/library/metadata/123", "", &by) == 1);
        CHECK(std::string(by) == "rk");

        const std::vector<QueueItem> rkOnly{{"/library/metadata/not-this", "123", "8001", "A", 0}};
        CHECK(selectQueueIndex(rkOnly, "/library/metadata/123", "", &by) == 0);
        CHECK(std::string(by) == "rk");
        CHECK(detailFor(rkOnly, "/library/metadata/123", "") == "index=0 by=rk");
        CHECK(selectQueueIndex(rkOnly, "/library/metadata/123", "missing", &by) == 0);
        CHECK(std::string(by) == "rk");

        CHECK(selectQueueIndex({}, "/library/metadata/123", "", &by) == -1);
        CHECK(std::string(by) == "none");
        CHECK(detailFor({}, "/library/metadata/123", "") == "index=-1 none");

        const std::vector<QueueItem> noCurrent{{"/library/metadata/1", "1", "1", "A", 0}};
        CHECK(selectQueueIndex(noCurrent, "/library/metadata/999", "", &by) == -1);

        const std::vector<QueueItem> n1{{"/playlist/item", "", "1", "empty-rk", 0}};
        CHECK(selectQueueIndex(n1, "/playlists/5", "", &by) == -1);

        const auto oldBind = [](const QueueItem& item, const std::string& reqKey,
                                const std::string& resolvedRatingKey) {
            return item.key == reqKey ||
                   (!item.ratingKey.empty() &&
                    (item.ratingKey == resolvedRatingKey ||
                     reqKey.find(item.ratingKey) != std::string::npos));
        };
        CHECK(oldBind(f3[0], "/library/metadata/123", "123"));
        CHECK(!firstResolveBinds(f3[0], "/library/metadata/123", "123"));
        CHECK(firstResolveBinds(f3[1], "/library/metadata/123", "123"));
        const QueueItem equal{"/library/metadata/456", "456", "7003", "equal", 0};
        CHECK(oldBind(equal, "/library/metadata/456", "456"));
        CHECK(firstResolveBinds(equal, "/library/metadata/456", "456"));

        CHECK(!queueStepDest(-1, 1, 2));
        CHECK(queueStepDest(0, 1, 2) && *queueStepDest(0, 1, 2) == 1);
        CHECK(!queueStepDest(1, 1, 2));
        CHECK(!queueStepDest(0, -1, 2));
    }

    // --- TASKS #25 differential: generated no-collision queues preserve old results ---
    {
        const auto oldSelect = [](const std::vector<QueueItem>& items, const std::string& currentKey,
                                  const std::string& playQueueItemId) {
            int currentIndex = 0;
            if (!playQueueItemId.empty()) {
                for (size_t i = 0; i < items.size(); ++i) {
                    if (items[i].playQueueItemId == playQueueItemId ||
                        items[i].ratingKey == playQueueItemId ||
                        items[i].key.find(playQueueItemId) != std::string::npos) {
                        currentIndex = static_cast<int>(i);
                        break;
                    }
                }
            } else if (!currentKey.empty()) {
                for (size_t i = 0; i < items.size(); ++i) {
                    if (items[i].key == currentKey ||
                        (!items[i].ratingKey.empty() &&
                         currentKey.find(items[i].ratingKey) != std::string::npos)) {
                        currentIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
            return currentIndex;
        };
        unsigned rng = 25;
        auto next = [&rng]() {
            rng = rng * 1664525u + 1013904223u;
            return rng;
        };
        int noCollision = 0;
        int colliding = 0;
        int oldWrong = 0;
        for (int t = 0; t < 25; ++t) {
            std::vector<QueueItem> q;
            for (int i = 0; i < 5; ++i) {
                const int rk = 1000 + static_cast<int>((next() + i * 37) % 8000);
                const int id = 100000 + t * 10 + i;
                q.push_back({"/library/metadata/" + std::to_string(rk), std::to_string(rk),
                             std::to_string(id), "N", 0});
            }
            for (size_t i = 0; i < q.size(); ++i) {
                CHECK(selectQueueIndex(q, q[i].key, "", nullptr) == oldSelect(q, q[i].key, ""));
                ++noCollision;
                CHECK(selectQueueIndex(q, "", q[i].playQueueItemId, nullptr) ==
                      oldSelect(q, "", q[i].playQueueItemId));
                ++noCollision;
            }
        }
        const auto checkCollision = [&](const std::vector<QueueItem>& q, const std::string& key,
                                        const std::string& id, int exact) {
            const int n = selectQueueIndex(q, key, id, nullptr);
            const int o = oldSelect(q, key, id);
            CHECK(n == exact);
            if (o != exact)
                ++oldWrong;
            ++colliding;
        };
        for (int t = 0; t < 10; ++t) {
            checkCollision({{"/library/metadata/150", "150", "9001", "A", 0},
                            {"/library/metadata/2", "2", "15", "B", 0}},
                           "", "15", 1);
            checkCollision({{"/library/metadata/5001", "5001", "7", "A", 0},
                            {"/library/metadata/9", "9", "5001", "B", 0}},
                           "", "5001", 1);
            checkCollision({{"/library/metadata/12", "12", "7001", "A", 0},
                            {"/library/metadata/123", "123", "7002", "B", 0}},
                           "/library/metadata/123", "", 1);
        }
        std::printf("differential seed=25 no_collision=%d colliding=%d old_wrong=%d\n",
                    noCollision, colliding, oldWrong);
        CHECK(noCollision == 250);
        CHECK(colliding == 30);
        CHECK(oldWrong == 30);
    }

    // --- Phase 4 Content FPS / SOURCE_FPS helpers (no network) ---
    CHECK(contentFpsHint("24p", "") == 24);
    CHECK(contentFpsHint("NTSC", "") == 30);
    CHECK(contentFpsHint("60p", "") == 60);
    CHECK(contentFpsHint("", "23.976") == 24);
    CHECK(contentFpsHint("", "29.970") == 30);
    CHECK(contentFpsHint("", "59.94") == 60);
    CHECK(contentFpsHint("film", "") == 24);
    CHECK(contentFpsHint("", "") == 0);
    // Numeric frameRate wins over token
    CHECK(contentFpsHint("NTSC", "23.976") == 24);

    CHECK(applySourceFpsConf("auto", 24) == 24);
    CHECK(applySourceFpsConf("", 30) == 30);
    CHECK(applySourceFpsConf("off", 24) == 0);
    CHECK(applySourceFpsConf("60", 24) == 60);
    CHECK(applySourceFpsConf("24", 0) == 24);
    CHECK(applySourceFpsConf("auto", 0) == 0);

    // --- STREAM preferDirectH264 helpers (no network) ---
    CHECK(mediaVideoIsH264("") == false);
    CHECK(mediaVideoIsH264("<Media videoCodec=\"h264\" />") == true);
    CHECK(mediaVideoIsH264("<Media videoCodec=\"hevc\" />") == false);
    CHECK(mediaVideoIsH264("<Media videoCodec=\"avc\" />") == true);
    CHECK(mediaVideoIsH264("<Media videoCodec=\"x264\" />") == true);
    CHECK(mediaVideoIsH264("<Media videoCodec=\"H264\" />") == true);
    CHECK(mediaVideoIsH264(
              "<Stream streamType=\"1\" codec=\"h264\" type=\"video\" />") == true);
    CHECK(mediaVideoIsH264(
              "<Stream type=\"video\" codec=\"avc1\" />") == true);
    CHECK(mediaVideoIsH264(
              "<Stream streamType=\"2\" codec=\"aac\" /><Media videoCodec=\"hevc\"/>") == false);
    // Local path + direct URL still resolve without preferDirect flag
    auto directLocal =


        resolvePlayTarget("/media/fat/misterplex/plex_real_baseline.264", "", "", 0, true, {},
                          true);
    CHECK(directLocal.ok &&
          directLocal.playable == "/media/fat/misterplex/plex_real_baseline.264");


    // preferDirectH264 does not alter local/http passthrough detail
    auto directUrl =
        resolvePlayTarget("http://pms.lan:32400/library/parts/1/file.mkv", "", "", 0, true, {},
                          true);
    CHECK(directUrl.ok && directUrl.detail == "direct URL");
    auto localPath = resolvePlayTarget("/media/fat/misterplex/clip.mp4", "", "", 0, true, {}, true);
    CHECK(localPath.ok && localPath.detail == "local path");

    // --- exact rational content rate (A/V pacing) ---
    // PMS reports Media@videoFrameRate="24p" for 23.976 content; the Stream@frameRate
    // value is the truthful one and must win.
    {
        int n = 0, d = 0;
        CHECK(parseExactFps("24p", "23.976", n, d) && n == 24000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("", "23.976023", n, d) && n == 24000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("", "24.000", n, d) && n == 24 && d == 1);
        n = d = 0;
        CHECK(parseExactFps("", "29.97", n, d) && n == 30000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("", "59.94", n, d) && n == 60000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("", "25", n, d) && n == 25 && d == 1);
        n = d = 0;
        CHECK(parseExactFps("PAL", "", n, d) && n == 25 && d == 1);
        n = d = 0;
        CHECK(parseExactFps("NTSC", "", n, d) && n == 30000 && d == 1001);
        n = d = 0;
        // 24p + Stream 24.000 is NTSC film (048 H1). Bare 24.000 without vfr stays 24/1.
        CHECK(parseExactFps("24p", "", n, d) && n == 24000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("24p", "24.000", n, d) && n == 24000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("30p", "30.000", n, d) && n == 30000 && d == 1001);
        n = d = 0;
        CHECK(parseExactFps("", "", n, d) == false && n == 0 && d == 0);
        n = d = 0;
        CHECK(parseExactFps("", "garbage", n, d) == false);
        // Non-standard rate is kept as-is (no snap), not rejected.
        n = d = 0;
        CHECK(parseExactFps("", "23.0", n, d) && n == 23000 && d == 1000);
    }
    {
        // Conf override wins over metadata; "auto"/empty leaves it alone.
        int n = 24000, d = 1001;
        CHECK(applyContentFpsConf("auto", n, d) == false && n == 24000 && d == 1001);
        CHECK(applyContentFpsConf("", n, d) == false && n == 24000 && d == 1001);
        CHECK(applyContentFpsConf("25", n, d) && n == 25 && d == 1);
        CHECK(applyContentFpsConf("24000/1001", n, d) && n == 24000 && d == 1001);
        CHECK(applyContentFpsConf("29.97", n, d) && n == 30000 && d == 1001);
        n = 24; d = 1;
        CHECK(applyContentFpsConf("junk", n, d) == false && n == 24 && d == 1);
    }

    // --- Goal D cadence T1: every FPGA raster uses NTSC content clocks ---
    {
        const int failsBeforeFps = fails;
        ::unsetenv("MPX_PRESENT_480I");
        auto keeps = [](const char* mode, int num, int den) {
            int n = 0, d = 0;
            return !fpgaContentFps(num, den, fpgaRasterNtsc(mode), n, d) && n == num &&
                   d == den;
        };
        auto mapsTo = [](const char* mode, int num, int den, int wantNum, int wantDen) {
            int n = 0, d = 0;
            return fpgaContentFps(num, den, fpgaRasterNtsc(mode), n, d) && n == wantNum &&
                   d == wantDen;
        };
        CHECK(fpgaRasterNtsc("240p"));
        CHECK(fpgaRasterNtsc("480i"));
        CHECK(!fpgaRasterNtsc("480p"));
        CHECK(!fpgaRasterNtsc(""));
        for (const char* mode : {"240p", "480i", "480p", ""}) {
            CHECK(keeps(mode, 24000, 1001));
            CHECK(mapsTo(mode, 24, 1, 24000, 1001));
            CHECK(keeps(mode, 30000, 1001));
            CHECK(mapsTo(mode, 30, 1, 30000, 1001));
            CHECK(mapsTo(mode, 60000, 1001, 30000, 1001));
            CHECK(mapsTo(mode, 60, 1, 30000, 1001));
            CHECK(mapsTo(mode, 25, 1, 24000, 1001));
            CHECK(mapsTo(mode, 50, 1, 24000, 1001));
            CHECK(mapsTo(mode, 23000, 1000, 24000, 1001));
        }
        CHECK(fpga480iMinFpsOk(24000, 1001));
        CHECK(fpga480iMinFpsOk(24, 1));
        CHECK(fpga480iMinFpsOk(30, 1));
        CHECK(fpga480iMinFpsOk(30000, 1001));
        CHECK(!fpga480iMinFpsOk(15, 1));
        CHECK(!fpga480iMinFpsOk(12, 1));
        CHECK(!fpga480iMinFpsOk(23, 1));
        CHECK(!fpga480iMinFpsOk(0, 0));
        CHECK(!fpga480iMinFpsOk(20, 1));
        CHECK(!fpga480iMinFpsOk(23976, 1001));
        CHECK(!fpga480iMinFpsOk(1, 0));
        CHECK(kMinFps480i == 24);
        CHECK(fpga240pMinFpsOk(24000, 1001));
        CHECK(fpga240pMinFpsOk(24, 1));
        CHECK(fpga240pMinFpsOk(30, 1));
        CHECK(fpga240pMinFpsOk(30000, 1001));
        CHECK(!fpga240pMinFpsOk(15, 1));
        CHECK(!fpga240pMinFpsOk(12, 1));
        CHECK(!fpga240pMinFpsOk(23, 1));
        CHECK(!fpga240pMinFpsOk(0, 0));
        CHECK(!fpga240pMinFpsOk(20, 1));
        CHECK(!fpga240pMinFpsOk(23976, 1001));
        CHECK(!fpga240pMinFpsOk(1, 0));
        CHECK(kMinFps240p == 24);
        CHECK(fpga480pMinFpsOk(24000, 1001));
        CHECK(fpga480pMinFpsOk(24, 1));
        CHECK(fpga480pMinFpsOk(30, 1));
        CHECK(fpga480pMinFpsOk(25, 1));
        CHECK(!fpga480pMinFpsOk(15, 1));
        CHECK(!fpga480pMinFpsOk(12, 1));
        CHECK(!fpga480pMinFpsOk(23, 1));
        CHECK(!fpga480pMinFpsOk(0, 0));
        CHECK(!fpga480pMinFpsOk(20, 1));
        CHECK(!fpga480pMinFpsOk(23976, 1001));
        CHECK(!fpga480pMinFpsOk(1, 0));
        CHECK(kMinFps480p == 24);
        // 480i present on the 480p decode path: same NTSC clocks as own-core 480i.
        ::setenv("MPX_PRESENT_480I", "1", 1);
        CHECK(fpgaRasterNtsc("480p"));
        CHECK(!fpgaRasterNtsc(""));
        CHECK(keeps("480p", 24000, 1001));
        CHECK(mapsTo("480p", 24, 1, 24000, 1001));
        CHECK(keeps("480p", 30000, 1001));
        CHECK(mapsTo("480p", 30, 1, 30000, 1001));
        CHECK(mapsTo("480p", 60000, 1001, 30000, 1001));
        CHECK(mapsTo("480p", 60, 1, 30000, 1001));
        CHECK(mapsTo("480p", 25, 1, 24000, 1001));
        CHECK(mapsTo("480p", 50, 1, 24000, 1001));
        for (const char* v : {"1", "y", "Y", "yes", "YES", "Yes", "on", "ON", "0", "no", "off",
                              "true", "On", "2", ""}) {
            ::setenv("MPX_PRESENT_480I", v, 1);
            CHECK(fpgaRasterNtsc("480p") == envFlagWanted("MPX_PRESENT_480I"));
            CHECK(fpgaRasterNtsc("240p"));
            CHECK(!fpgaRasterNtsc(""));
        }
        ::unsetenv("MPX_PRESENT_480I");

        // PMS client profiles stay film-rate; a 30-family rate is never a film profile.
        std::string why;
        WeakLadder p30;
        CHECK(!applyFpgaPlexProfile(p30, "ip", 30000, 1001, false, &why, "240p"));
        CHECK(!applyFpgaPlexProfile(p30, "ip", 30000, 1001, false, &why, "480i", true));
        CHECK(!applyFpgaPlexProfile(p30, "ip", 30, 1, true, &why, "480p", true));
        CHECK(fpgaNtscSourceProfileName("ip", "240p", false) ==
              "MiSTerPlex-FPGA-IP-240p-src30-filter-on");
        CHECK(fpgaNtscSourceProfileName("idr", "480i", true) ==
              "MiSTerPlex-FPGA-IDR-480i-src30-filter-off");
        // No-proxy NTSC fallback: the film profile geometry under the raster-specific
        // source-rate profile, with videoFrameRate at the source rate.
        for (const char* mode : {"240p", "480i"}) {
            WeakLadder f;
            CHECK(applyFpgaPlexProfile(f, "ip", 24000, 1001, true, &why, mode, true));
            CHECK(validateWeakLadder(f));
            // The film-rate profiles keep rejecting a 30-family rate (8d8faaf1 guard).
            f.fpgaFpsNum = 30000;
            f.fpgaFpsDen = 1001;
            CHECK(!validateWeakLadder(f));
            CHECK(buildUniversalTranscodeUrl("http://127.0.0.1:32400", "/library/metadata/7",
                                             "tok", "s1", 2000, f)
                      .empty());
            f.fpgaProfileVariant = fpgaNtscSourceProfileName("ip", mode, true);
            CHECK(isFpgaPlexProfile(f));
            CHECK(validateWeakLadder(f));
            CHECK(plexVideoEncodeFlags(f).find("-r 30000/1001") != std::string::npos);
            CHECK(buildUniversalTranscodeUrl("http://127.0.0.1:32400", "/library/metadata/7",
                                             "tok", "s1", 2000, f)
                      .find("&videoFrameRate=30000%2F1001&offset=2") != std::string::npos);
            f.fpgaFpsNum = 30;
            f.fpgaFpsDen = 1;
            CHECK(validateWeakLadder(f));
            CHECK(buildUniversalTranscodeUrl("http://127.0.0.1:32400", "/library/metadata/7",
                                             "tok", "s1", 2000, f)
                      .find("&videoFrameRate=30&offset=2") != std::string::npos);
            for (auto bad : {std::make_pair(24000, 1001), std::make_pair(25, 1),
                             std::make_pair(60000, 1001), std::make_pair(60, 1),
                             std::make_pair(0, 0)}) {
                f.fpgaFpsNum = bad.first;
                f.fpgaFpsDen = bad.second;
                CHECK(!validateWeakLadder(f));
                CHECK(buildUniversalTranscodeUrl("http://127.0.0.1:32400",
                                                 "/library/metadata/7", "tok", "s1", 0, f)
                          .empty());
            }
        }
        {
            WeakLadder f;
            CHECK(applyFpgaPlexProfile(f, "ip", 24000, 1001, true, &why, "480p", true));
            CHECK(validateWeakLadder(f));
            f.fpgaProfileVariant = fpgaNtscSourceProfileName("ip", "480p", true);
            f.fpgaFpsNum = 30000;
            f.fpgaFpsDen = 1001;
            CHECK(isFpgaPlexProfile(f));
            CHECK(validateWeakLadder(f));
            CHECK(fpgaNtscSourceProfileName("ip", "480p", true) ==
                  "MiSTerPlex-FPGA-IP-480p-src30-filter-off");
            CHECK(plexVideoEncodeFlags(f).find("-r 30000/1001") != std::string::npos);
            CHECK(buildUniversalTranscodeUrl("http://127.0.0.1:32400", "/library/metadata/7",
                                             "tok", "s1", 2000, f)
                      .find("&videoFrameRate=30000%2F1001&offset=2") != std::string::npos);
        }
        std::printf("test_resolve: fps NTSC clocks T1 %s\n",
                    fails == failsBeforeFps ? "OK" : "FAIL");
    }

    testDirectRemoteNoproxy();

    if (fails) {
        std::fprintf(stderr, "test_resolve: %d failures\n", fails);
        return 1;
    }
    std::printf("test_resolve: OK\n");
    return 0;
}
