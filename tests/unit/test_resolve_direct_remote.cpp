// Direct remote resolve with MPX_REMOTE_LADDER_PROXY unset (c941). Included from
// test_resolve.cpp so `./build/test_resolve` covers it. Curl shim only: never a
// live PMS or MiSTer.
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

static std::string envOrEmpty(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

static void setOrUnset(const char* k, const std::string& v, bool had) {
    if (had)
        ::setenv(k, v.c_str(), 1);
    else
        ::unsetenv(k);
}

static void testDirectRemoteNoproxy() {
    using namespace misterplex;
    const int failsBefore = fails;

    ::unsetenv("MPX_REMOTE_LADDER_PROXY");
    ::unsetenv("REMOTE_LADDER_PROXY");
    CHECK(!ladderProxyConfigured());
    ::setenv("MPX_REMOTE_LADDER_PROXY", "", 1);
    CHECK(!ladderProxyConfigured());
    ::setenv("REMOTE_LADDER_PROXY", "http://192.0.2.1:18765", 1);
    CHECK(!ladderProxyConfigured()); // empty MPX_ beats conf/legacy env
    ::unsetenv("MPX_REMOTE_LADDER_PROXY");
    CHECK(ladderProxyConfigured());
    ::unsetenv("REMOTE_LADDER_PROXY");
    CHECK(!ladderProxyConfigured());
    ::setenv("MPX_REMOTE_LADDER_PROXY", "http://192.0.2.1:18765", 1);
    CHECK(ladderProxyConfigured());
    ::unsetenv("MPX_REMOTE_LADDER_PROXY");
    CHECK(!ladderProxyConfigured());

    const std::string src = readRepoFile("arm/misterplexd/plex_resolve.cpp");
    const std::string mainCpp = readRepoFile("arm/misterplexd/main.cpp");
    CHECK(mainCpp.find("REMOTE_LADDER_PROXY unset (direct PMS)") != std::string::npos);
    CHECK(mainCpp.find("loadConf(confPath, \"REMOTE_LADDER_PROXY\")") == std::string::npos);
    CHECK(src.find("no ladder helper configured: refusing (never a direct Part)") ==
          std::string::npos);
    CHECK(src.find("needs the ladder proxy to halve it") == std::string::npos);
    CHECK(src.find("coded 640x480 needs the ladder proxy") == std::string::npos);

    // https / plex.direct → plain HTTP the static ARM libav can open (token rides the URL).
    CHECK(companionOpenablePmsBase("https://198-51-100-153.hash.plex.direct:32400", "") ==
          "http://198.51.100.153:32400");
    CHECK(companionOpenablePmsBase("https://pms.example:32400", "") == "http://pms.example:32400");
    CHECK(companionOpenablePmsBase("http://203.0.113.8:32400", "") == "http://203.0.113.8:32400");

    // One WAN transcode URL builder for 240p/480p/480i (mode table, not three copies).
    for (const char* mode : {"240p", "480p", "480i"}) {
        WeakLadder w;
        CHECK(applyPlexTranscodeProfile(mode, w));
        const auto url = buildUniversalTranscodeUrl("http://203.0.113.8:32400",
                                                    "/library/metadata/29729", "tok29729", "sWan",
                                                    0, w);
        CHECK(!url.empty());
        CHECK(url.find("location=wan") != std::string::npos);
        CHECK(url.find("/video/:/transcode/universal/start.mp4") != std::string::npos);
        CHECK(url.find("X-Plex-Token=tok29729") != std::string::npos);
        CHECK(url.find("ladder.ts") == std::string::npos);
        CHECK(url.find("REMOTE_LADDER") == std::string::npos);
    }
    {
        WeakLadder w480;
        CHECK(applyPlexTranscodeProfile("480p", w480));
        const auto kube = buildUniversalTranscodeUrl("http://203.0.113.8:32401",
                                                     "/library/metadata/11681", "tokKube", "sK1",
                                                     0, w480);
        CHECK(kube.find("path=%2Flibrary%2Fmetadata%2F11681") != std::string::npos);
        CHECK(kube.find("X-Plex-Token=tokKube") != std::string::npos);
        CHECK(kube.find("location=wan") != std::string::npos);
    }

    char tmpl[] = "/tmp/mpx-noproxy-XXXXXX";
    const char* dirp = ::mkdtemp(tmpl);
    CHECK(dirp != nullptr);
    if (!dirp)
        return;
    const std::string dir = dirp;
    {
        std::ofstream f(dir + "/meta.xml");
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<MediaContainer size=\"1\" identifier=\"com.plexapp.plugins.library\">\n"
             "<Video ratingKey=\"29729\" key=\"/library/metadata/29729\" type=\"movie\" "
             "title=\"Remote noproxy\" duration=\"3028000\">\n"
             "<Media id=\"1\" duration=\"3028000\" width=\"1920\" height=\"1080\" "
             "aspectRatio=\"1.78\" audioCodec=\"aac\" videoCodec=\"h264\" videoProfile=\"high\" "
             "container=\"mkv\" videoFrameRate=\"24p\">\n"
             "<Part id=\"2\" key=\"/library/parts/9001/file.mkv\" duration=\"3028000\" "
             "container=\"mkv\">\n"
             "<Stream id=\"3\" streamType=\"1\" codec=\"h264\" width=\"1920\" height=\"1080\" "
             "frameRate=\"23.976\" profile=\"high\" />\n"
             "<Stream id=\"4\" streamType=\"2\" codec=\"aac\" channels=\"2\" />\n"
             "</Part></Media></Video></MediaContainer>\n";
    }
    {
        std::ofstream f(dir + "/decision.xml");
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<MediaContainer size=\"1\" generalDecisionCode=\"1001\" "
             "transcodeDecisionCode=\"1001\" transcodeDecisionText=\"Conversion OK.\">\n"
             "<Video ratingKey=\"29729\" type=\"movie\"/>\n</MediaContainer>\n";
    }
    {
        std::ofstream f(dir + "/curl");
        f << "#!/bin/sh\n"
             "echo \"$*\" >> '" << dir << "/calls'\n"
             "url=\"\"\n"
             "wcode=0\n"
             "prev=\"\"\n"
             "for a in \"$@\"; do\n"
             "  [ \"$prev\" = \"-w\" ] && wcode=1\n"
             "  prev=\"$a\"\n"
             "  url=\"$a\"\n"
             "done\n"
             "case \"$url\" in\n"
             "  *library/metadata*) cat '" << dir << "/meta.xml'; exit 0 ;;\n"
             "  *universal/decision*) cat '" << dir << "/decision.xml'; exit 0 ;;\n"
             "esac\n"
             "if [ \"$wcode\" = 1 ]; then\n"
             "  python3 -c 'import sys; sys.stdout.buffer.write(b\"G\"*128)'\n"
             "  printf '200'\n"
             "  exit 0\n"
             "fi\n"
             "python3 -c 'import sys; sys.stdout.buffer.write(b\"G\"*128)'\n";
    }
    ::chmod((dir + "/curl").c_str(), 0755);

    const std::string oldPath = envOrEmpty("PATH");
    const bool hadPath = std::getenv("PATH") != nullptr;
    const std::string oldBe = envOrEmpty("MPX_VIDEO_BACKEND");
    const bool hadBe = std::getenv("MPX_VIDEO_BACKEND") != nullptr;
    const std::string oldMode = envOrEmpty("MPX_FPGA_MODE");
    const bool hadMode = std::getenv("MPX_FPGA_MODE") != nullptr;
    const std::string oldFilt = envOrEmpty("MPX_H264_FILTER");
    const bool hadFilt = std::getenv("MPX_H264_FILTER") != nullptr;
    const std::string oldProto = envOrEmpty("MPX_H264_PROTOTYPE");
    const bool hadProto = std::getenv("MPX_H264_PROTOTYPE") != nullptr;

    ::setenv("PATH", (dir + ":" + oldPath).c_str(), 1);
    ::setenv("MPX_VIDEO_BACKEND", "fpga-h264", 1);
    ::setenv("MPX_FPGA_MODE", "480p", 1);
    ::setenv("MPX_H264_FILTER", "off", 1);
    ::unsetenv("MPX_H264_PROTOTYPE");
    ::unsetenv("MPX_REMOTE_LADDER_PROXY");
    ::unsetenv("REMOTE_LADDER_PROXY");

    WeakLadder weak480;
    CHECK(applyPlexTranscodeProfile("480p", weak480));
    const char* wanBase = "http://203.0.113.8:32400";

    auto checkDirect = [&](const std::string& key, const char* token, bool force,
                           const char* tag) {
        const auto r = resolvePlayTarget(key, wanBase, token, 0, /*weakAlways=*/true, weak480,
                                         /*preferDirectH264=*/false, 0, 0, force,
                                         "max_num_ref_frames 3 (limit 1)");
        CHECK(r.ok);
        CHECK(r.transcoded);
        CHECK(!r.viaLadderProxy);
        CHECK(r.playable.find("/video/:/transcode/universal/start") != std::string::npos);
        CHECK(r.playable.find("X-Plex-Token=") != std::string::npos);
        CHECK(r.playable.find("location=wan") != std::string::npos);
        CHECK(r.playable.find("ladder.ts") == std::string::npos);
        CHECK(r.playable.find("/library/parts/") == std::string::npos);
        CHECK(std::string(r.detail).find("refusing (never a direct Part)") == std::string::npos);
        CHECK(std::string(r.detail).find("PMS universal") != std::string::npos);
        if (force) {
            CHECK(std::string(r.detail).find("gate fallback:") == 0);
        }
        (void)tag;
    };
    checkDirect("/library/metadata/29729", "tok29729", false, "N1");
    checkDirect("/library/metadata/11681", "tokKube", false, "K1");
    checkDirect("/library/metadata/29729", "tok29729", true, "N1-force");

    // Fifth Element home key 488: High metadata still takes the PMS baseline
    // universal ladder. No MiSTerPlex.xml and no named High profile.
    {
        const auto r = resolvePlayTarget("/library/metadata/488", wanBase, "tok488", 0, true, weak480,
                                         false, 0, 0, false, {});
        CHECK(r.ok && r.transcoded && !r.viaLadderProxy);
        CHECK(r.playable.find("488") != std::string::npos);
        CHECK(r.playable.find("/video/:/transcode/universal/start") != std::string::npos);
        CHECK(r.playable.find("/library/parts/") == std::string::npos);
        const std::string blob = r.playable + "\n" + r.httpHeaders + "\n" + std::string(r.detail);
        CHECK(blob.find("MiSTerPlex.xml") == std::string::npos);
        CHECK(blob.find("MiSTerPlex-High") == std::string::npos);
        CHECK(blob.find("X-Plex-Client-Profile-Name: MiSTerPlex") == std::string::npos);
        CHECK(blob.find("baseline") != std::string::npos);
        CHECK(blob.find("cabac=0") != std::string::npos || blob.find("cabac%3D0") != std::string::npos ||
              blob.find("cabac%3d0") != std::string::npos);
    }

    // 240p and 480i share the same noproxy PMS path (mode table).
    for (const char* mode : {"240p", "480i"}) {
        ::setenv("MPX_FPGA_MODE", mode, 1);
        WeakLadder w;
        CHECK(applyPlexTranscodeProfile(mode, w));
        const auto r = resolvePlayTarget("/library/metadata/29729", wanBase, "tokMode", 0, true, w,
                                         false, 0, 0, false, {});
        CHECK(r.ok && r.transcoded && !r.viaLadderProxy);
        CHECK(r.playable.find("/video/:/transcode/universal/start") != std::string::npos);
        CHECK(r.playable.find("ladder.ts") == std::string::npos);
    }

    setOrUnset("PATH", oldPath, hadPath);
    setOrUnset("MPX_VIDEO_BACKEND", oldBe, hadBe);
    setOrUnset("MPX_FPGA_MODE", oldMode, hadMode);
    setOrUnset("MPX_H264_FILTER", oldFilt, hadFilt);
    setOrUnset("MPX_H264_PROTOTYPE", oldProto, hadProto);
    ::unsetenv("MPX_REMOTE_LADDER_PROXY");
    ::unsetenv("REMOTE_LADDER_PROXY");
    std::system(("rm -rf '" + dir + "'").c_str());

    std::printf("test_resolve: direct remote noproxy %s\n",
                fails == failsBefore ? "OK" : "FAIL");
}
