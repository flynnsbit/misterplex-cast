// 720p A/V keep-up vs 480p gold class. RED twin is the Trek stutter soak.
#include "libmisterplex/p720_audio_keepup.hpp"
#include "libmisterplex/av_clock.hpp"
#include "libmisterplex/av_inproc_decode.hpp"
#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/fabric_direct.hpp"
#include "libmisterplex/present_bank.hpp"

#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    using namespace misterplex;
    using namespace misterplex::p720_av;

    // RED twin: live 720p24 Trek soak (40868) — unique ~12, audio held to presents.
    CHECK(!uniqueMeets24(11.9));
    CHECK(!audioKeepupMeets480pClass(28.0, 56.94));
    CHECK(!productAvKeepupOk(11.9, 28.0, 56.94, 0));
    CHECK(!productAvKeepupOk(23.7, 25.16, 51.67, 0)); // 24 unique but audio starved
    CHECK(!productAvKeepupOk(23.7, 73.0, 73.88, 1));  // drops shred

    // GREEN twin: 480p gold Trek class.
    CHECK(uniqueMeets24(23.7));
    CHECK(audioKeepupMeets480pClass(73.0, 73.88));
    CHECK(productAvKeepupOk(23.7, 73.0, 73.88, 0));
    CHECK(productAvKeepupOk(24.0, 40.0, 40.0, 0));
    // 23.5 floor is not true 24000/1001. +1.1 s drift is not 480p lipsync.
    CHECK(uniqueMeets24(23.5));
    CHECK(!uniqueMeetsTrue24p(23.5));
    CHECK(uniqueMeetsTrue24p(23.98));
    CHECK(uniqueMeetsTrue24p(24.00));
    CHECK(!uniqueMeetsTrue24p(24.50));
    CHECK(!productTrue24pAvOk(23.5, 89.56, 89.99, 0, 1163));
    CHECK(productTrue24pAvOk(23.98, 40.0, 40.0, 0, 40));

    // RED: ARM unique 23.8 with FPGA swap 22.2 is lost kicks (pre wait_swap).
    CHECK(!uniqueScanoutMeets24(23.8, 22.2));
    CHECK(!productScanoutKeepupOk(23.8, 22.2, 57.09, 57.32, 0));
    // GREEN: both ARM and scanout unique ≥23.5, audio keep-up, hold-only.
    CHECK(uniqueScanoutMeets24(23.8, 23.7));
    CHECK(productScanoutKeepupOk(23.8, 23.7, 57.09, 57.32, 0));
    CHECK(productScanoutKeepupOk(23.8, 23.8, 40.51, 40.70, 0));

    // RED twin: wait-THIS-swap + 2.2 ms avDecide/overlay (live 22.8 soak).
    CHECK(!kickOnSwapMeets24(24.10, 2200.0));
    CHECK(uniqueFpsWaitThisSwap(24.10, 2200.0) < 23.5);
    CHECK(uniqueFpsWaitThisSwap(24.10, 2200.0) > 22.5);
    // GREEN: kick-on-swap (post-swap doorbell only).
    CHECK(kickOnSwapMeets24(24.10, 0.0));
    CHECK(kickOnSwapMeets24(24.10, 400.0));
    CHECK(uniqueMeets24(uniqueFpsWaitThisSwap(24.10, 0.0)));

    // 24 presents @ 24000/1001 → 1.001 s of 48 kHz stereo.
    CHECK(pcmBytesForPresents(24, 24000, 1001) == 24LL * 8 * 1001);
    CHECK(pcmBytesForPresents(0, 24000, 1001) == 0);
    CHECK(trimGatedPcmDrop(192000 * 3, 24, 24000, 1001) == 24LL * 8 * 1001);
    CHECK(trimGatedPcmDrop(100, 24, 24000, 1001) == 100); // gated shorter than want
    CHECK(capGatedPcmRemain(332096, 40) == 48000 * 4 * 40 / 1000);
    CHECK(capGatedPcmRemain(1000, 40) == 1000);
    CHECK(capGatedPcmRemain(7680, 40) == 7680);
    CHECK(capGatedPcmEraseFront(192000 * 2, 40) == 192000 * 2 - 48000 * 4 * 40 / 1000);
    CHECK(capGatedPcmEraseFront(1000, 40) == 0);
    CHECK(!driftTensOfMs(1400));
    CHECK(!driftTensOfMs(-1400));
    CHECK(driftTensOfMs(40));
    CHECK(driftTensOfMs(-62));
    CHECK(driftTensOfMs(80));
    CHECK(!driftTensOfMs(81));

    ::unsetenv("MPX_BEAM_PACE");
    CHECK(beamPaceWanted(1280, 720));
    CHECK(!beamPaceWanted(640, 480));
    CHECK(!beamPaceWanted(320, 240));
    ::setenv("MPX_BEAM_PACE", "0", 1);
    CHECK(!beamPaceWanted(1280, 720));
    ::unsetenv("MPX_BEAM_PACE");
    CHECK(beamPaceWanted(1280, 720));

    CHECK(inprocSkipLoopFilter720p(1280, 720));
    CHECK(!inprocSkipLoopFilter720p(640, 480));
    CHECK(!inprocSkipLoopFilter720p(960, 540));

    CHECK(kDefaultPresentLeadMs == 40);
    CHECK(avResyncDropMsForPresent(80, true) == 0);
    CHECK(avResyncDropMsForPresent(80, false) == 80);

    // RED: 24-present gate / inproc-only stick / 40 ms lead vs 100 ms queue.
    CHECK(combined720pAudioStartsWithVideo(false));
    CHECK(combined720pAudioStartsWithVideo(true));
    CHECK(inprocSameDemuxAudioWanted(true, true));
    CHECK(!inprocSameDemuxAudioWanted(true, false));
    CHECK(!inprocSameDemuxAudioWanted(false, true));
    CHECK(inprocRemuxMustCopyAudio(true, true));
    CHECK(!inprocRemuxMustCopyAudio(true, false));
    CHECK(!inprocRemuxMustCopyAudio(false, true));
    CHECK(urlIsLoopbackHttp("http://127.0.0.1:9324/video/:/transcode/universal/start.mp4"));
    CHECK(!urlIsLoopbackHttp("http://192.0.2.21:9324/x"));
    CHECK(!urlIsLoopbackHttp(nullptr));
    CHECK(!urlIsLoopbackHttp(""));
    CHECK(holdAudioToPicturesWanted(false, false));
    CHECK(!holdAudioToPicturesWanted(true, false));
    CHECK(!holdAudioToPicturesWanted(true, true));
    CHECK(plex720pWcBankIngest(true));
    CHECK(!plex720pWcBankIngest(false));
    CHECK(stickIngestWantedOn720pPipe(true, false));
    CHECK(stickIngestWantedOn720pPipe(true, true));
    CHECK(!stickIngestWantedOn720pPipe(false, true));
    CHECK(combined720pSkipAvHold(true, false));
    CHECK(combined720pSkipAvHold(true, true));
    CHECK(!combined720pSkipAvHold(false, false));
    CHECK(!plex720pSkipBankMemcpy(0, true));
    CHECK(!plex720pSkipBankMemcpy(0x3f000000u, false)); // live 03f1b95a must copy
    CHECK(plex720pSkipBankMemcpy(0x3f000000u, true));
    CHECK(holdLeadMsWithQueued(40, 0) == 40);
    CHECK(holdLeadMsWithQueued(40, kFeedTargetBytes) == 140);
    CHECK(avDecide(-100, 40, 0, 0) == AvAction::Hold);
    CHECK(avDecide(-100, holdLeadMsWithQueued(40, kFeedTargetBytes), 0, 0) ==
          AvAction::Present);
    CHECK(holdLeadMsWithQueued(40, kMrAudioBytesPerSec) == 240); // cap 200

    // Leftover 480p PLXI + static L4 PLXJ (28cb5a75 live) must classify L4.
    CHECK(decideLiveGlassFromProbes(true, false, true, false) == LiveGlass::L4);
    CHECK(decideLiveGlassFromProbes(true, true, true, false) == LiveGlass::L4);
    // Live 480p scanout (PLXD changing) still beats leftover L4 PLXJ.
    CHECK(decideLiveGlassFromProbes(true, false, true, true) == LiveGlass::True480);
    CHECK(decideLiveGlassFromProbes(false, false, true, false) == LiveGlass::True480);
    CHECK(glassMax(1280, 720, LiveGlass::L4).width == 1280);
    CHECK(glassMax(1280, 720, LiveGlass::True480).width == 640);

    CHECK(mplexDdrCovers(kPlex720pPhysBase, 2u * kPlex720pYuv420pBankStride));
    CHECK(kMplexDdrSize > 0x400000u); // 4MiB clipped 720p bank 1
    CHECK(!mplexDdrCovers(kPlex720pPhysBase, 2u * kPlex720pYuv420pBankStride) == false);

    // 4MiB historical window must FAIL 720p dual-bank need (0x480000).
    CHECK((static_cast<uint64_t>(kPlex720pPhysBase - kMplexDdrPhys) +
           2ull * kPlex720pYuv420pBankStride) > 0x400000ull);

    if (fails) {
        std::fprintf(stderr, "test_p720_audio_keepup: %d failures\n", fails);
        return 1;
    }
    std::printf("test_p720_audio_keepup: OK keepup gold=23.7 stutter=11.9 FAIL\n");
    return 0;
}
