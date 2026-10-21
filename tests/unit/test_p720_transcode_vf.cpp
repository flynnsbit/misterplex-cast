// Shipped 720p vf skip: transcode→1280x720 must not re-scale. 480p must still scale.
#include "libmisterplex/p720_transcode_vf.hpp"
#include "libmisterplex/fabric_direct.hpp"
#include "libmisterplex/p720_e2e_budget.hpp"
#include "libmisterplex/present_bank.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    using misterplex::skipRedundant720pTranscodeVf;
    const std::string trek =
        "https://example.plex.direct:32400/video/:/transcode/universal/start.mp4"
        "?videoResolution=1280x720&maxVideoBitrate=1500&path=%2Flibrary%2Fmetadata%2F40868";
    const std::string local = "/media/fat/movies/native720.mp4";
    const std::string p480 =
        "http://127.0.0.1:32400/video/:/transcode/universal/start.mp4?videoResolution=640x480";

    // URL claiming 1280x720 is NOT a skip (PMS 40868 HDMI green when 640x480).
    CHECK(!skipRedundant720pTranscodeVf(trek, 1280, 720));
    CHECK(skipRedundant720pTranscodeVf(trek, 1280, 720, 1280, 720));
    CHECK(!skipRedundant720pTranscodeVf(trek, 1280, 720, 640, 480));
    // Library source 1440x1080 is NOT transcode coded size (14 unique lock).
    CHECK(!skipRedundant720pTranscodeVf(trek, 1280, 720, 1440, 1080));
    CHECK(misterplex::skipVfProbedCodedDim(1280, 1440) == 1280);
    CHECK(misterplex::skipVfProbedCodedDim(0, 1440) == 0);
    CHECK(misterplex::skipVfProbedCodedDim(640, 1440) == 640);
    CHECK(misterplex::transcodePadOnly720p(960, 720, 1280, 720));
    CHECK(!misterplex::transcodePadOnly720p(1280, 720, 1280, 720));
    CHECK(!misterplex::transcodePadOnly720p(640, 480, 1280, 720));
    // RED twins: wrong bank or not measured 720p must not skip.
    CHECK(!skipRedundant720pTranscodeVf(trek, 640, 480));
    CHECK(!skipRedundant720pTranscodeVf(trek, 960, 540));
    CHECK(!skipRedundant720pTranscodeVf(local, 1280, 720));
    CHECK(skipRedundant720pTranscodeVf(local, 1280, 720, 1280, 720));
    CHECK(!skipRedundant720pTranscodeVf(p480, 640, 480));
    CHECK(!skipRedundant720pTranscodeVf("", 1280, 720));
    CHECK(misterplex::metadataKeyIsFarpoint40868("/library/metadata/40868"));
    CHECK(misterplex::metadataKeyIsFarpoint40868("40868"));
    CHECK(!misterplex::metadataKeyIsFarpoint40868("/library/metadata/143"));
    CHECK(misterplex::libraryMetadataMustUsePmsUniversal("/library/metadata/40868"));
    CHECK(misterplex::playableIsPmsUniversal720p(trek));
    CHECK(!misterplex::playableIsPmsUniversal720p("/media/fat/misterplex/cache/farpoint_1280x720.mp4"));
    CHECK(misterplex::libraryKeyMustNotSpawnLocalFile(
        "/library/metadata/40868", "/media/fat/misterplex/cache/farpoint_1280x720.mp4"));
    CHECK(misterplex::libraryKeyMustNotSpawnLocalFile(
        "40868", "/media/fat/misterplex/cache/farpoint_1280x720.mp4"));
    CHECK(!misterplex::libraryKeyMustNotSpawnLocalFile(
        "/library/metadata/40868",
        "http://127.0.0.1:9324/video/:/transcode/universal/start.mp4?videoResolution=1280x720"));
    CHECK(misterplex::playableIsFarpointIdentityCache(
        "/media/fat/misterplex/cache/farpoint_1280x720.mp4"));
    CHECK(!misterplex::playableIsFarpointIdentityCache(trek));
    CHECK(misterplex::liveUniversalMustStreamHttp(trek));
    CHECK(!misterplex::inprocPrefetchHttpIdentity720p(trek, 1280, 720, 1280, 720));
    CHECK(!misterplex::inprocPrefetchHttpIdentity720p(trek, 1280, 720, 0, 0));
    CHECK(!misterplex::inprocPrefetchHttpIdentity720p(trek, 1280, 720, 640, 480));
    CHECK(!misterplex::inprocPrefetchHttpIdentity720p(
        "/media/fat/misterplex/cache/farpoint_1280x720.mp4", 1280, 720, 1280, 720));
    // Shipped 40868 playable must be universal 720p, not the identity cache.
    CHECK(misterplex::libraryMetadataMustUsePmsUniversal("/library/metadata/40868") &&
          misterplex::playableIsPmsUniversal720p(trek) &&
          !misterplex::playableIsFarpointIdentityCache(trek));

    int w = 0, h = 0, n = 0, d = 0, ax = 0, ay = 0;
    const char* farpoint720 =
        "Stream #0:0: Video: h264 (Constrained Baseline), yuv420p(progressive), "
        "1280x720 [SAR 1:1 DAR 16:9], 23.98 fps, 23.98 tbr, 24k tbn\n";
    const char* wrong480 =
        "Stream #0:0: Video: h264, yuv420p, 640x480, 29.97 fps\n";
    CHECK(misterplex::parseFfmpegIdentify(farpoint720, w, h, n, d, ax, ay));
    CHECK(misterplex::localFileIsTrue720p24(w, h));
    CHECK(w == 1280 && h == 720);
    CHECK(n == 24000 && d == 1001);
    CHECK(ax == 16 && ay == 9);
    CHECK(misterplex::parseFfmpegIdentify(wrong480, w, h, n, d, ax, ay));
    CHECK(!misterplex::localFileIsTrue720p24(w, h));
    CHECK(w == 640 && h == 480);
    CHECK(!misterplex::parseFfmpegIdentify("", w, h, n, d, ax, ay));
    CHECK(!misterplex::parseFfmpegIdentify("Audio: aac", w, h, n, d, ax, ay));

    CHECK(misterplex::plex720pStickIngestOverridesFabric(true, true));
    CHECK(!misterplex::plex720pStickIngestOverridesFabric(false, true));
    // Shipped contract (prod snapshot 735c8281): default OFF, opt in with MPX_STICK_I420=1.
    ::unsetenv("MPX_STICK_I420");
    CHECK(!misterplex::stickI420Wanted());
    ::setenv("MPX_STICK_I420", "1", 1);
    CHECK(misterplex::stickI420Wanted());
    ::setenv("MPX_STICK_I420", "0", 1);
    CHECK(!misterplex::stickI420Wanted());
    ::setenv("MPX_STICK_I420", "off", 1);
    CHECK(!misterplex::stickI420Wanted());
    ::unsetenv("MPX_STICK_I420");
    CHECK(!misterplex::plex720pClassSplitAv(1280, 720));
    CHECK(!misterplex::plex720pClassSplitAv(960, 540));

    CHECK(misterplex::preferPl330720pPublish(1280, 720, 0x30180000u));
    CHECK(!misterplex::preferPl330720pPublish(640, 480, 0x30180000u));
    CHECK(!misterplex::preferPl330720pPublish(1280, 720, 0));
    CHECK(!misterplex::p720_budget::kSerialSweep116Meets24);
    CHECK(misterplex::p720_budget::kWcSweep116Meets24);
    CHECK(misterplex::avResyncDropMsForPresent(80, true) == 0);

    if (fails) {
        std::fprintf(stderr, "test_p720_transcode_vf: %d failures\n", fails);
        return 1;
    }
    std::printf("test_p720_transcode_vf: OK skip transcode 1280x720 vf\n");
    return 0;
}
