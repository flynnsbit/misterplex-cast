#include "libmisterplex/core_id_select.hpp"
#include "libmisterplex/daemon_signal.hpp"
#include "libmisterplex/display_raster.hpp"
#include "libmisterplex/fpga_build_ids.hpp"

#include <cstdio>
#include <cstring>

using misterplex::selectCoreIdFromStream;
using misterplex::selectCoreIdForMode;
using misterplex::selectedCoreIdForPlay;
using misterplex::selectedCoreIdForContentLabel;
using misterplex::menuReloadTargetForPlay;
using misterplex::selectedCoreIdForDecisionOutput;
using misterplex::menuTargetAfterDecision;
using misterplex::pairedBuildForEntropy;
using misterplex::kHighCabacCoreId480p;
using misterplex::kSelectedCoreId480i;
using misterplex::kSelectedCoreId240p;
using misterplex::kSelectedCoreId480p;
using misterplex::recognisedFpgaBuild;
using misterplex::FpgaBuildInfo;

static int fails = 0;
#define CHECK(c)                                                                                 \
    do {                                                                                         \
        if (!(c)) {                                                                              \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                     \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    CHECK(kSelectedCoreId480i == 0x48F14A0Bu);
    CHECK(kSelectedCoreId240p == 0x24F1A2C4u);
    CHECK(recognisedFpgaBuild(kSelectedCoreId480i) != nullptr);
    CHECK(recognisedFpgaBuild(kSelectedCoreId240p) != nullptr);
    CHECK(recognisedFpgaBuild(1u) == nullptr);
    CHECK(std::strcmp(recognisedFpgaBuild(kSelectedCoreId480i)->raster, "480i") == 0);
    CHECK(std::strcmp(recognisedFpgaBuild(kSelectedCoreId240p)->raster, "240p") == 0);

    const auto i720 = selectCoreIdFromStream(720, 480, true);
    CHECK(i720.coreId480i == 0x48F14A0Bu && i720.coreId240p == 0u);

    const auto i720pflag = selectCoreIdFromStream(720, 480, false);
    CHECK(i720pflag.coreId480i == 0x48F14A0Bu && i720pflag.coreId240p == 0u);

    const auto i640 = selectCoreIdFromStream(640, 480, true);
    CHECK(i640.coreId480i == 0x48F14A0Bu && i640.coreId240p == 0u);

    const auto field = selectCoreIdFromStream(720, 240, true);
    CHECK(field.coreId480i == 0x48F14A0Bu && field.coreId240p == 0u);

    const auto p240 = selectCoreIdFromStream(320, 240, false);
    CHECK(p240.coreId480i == 0u && p240.coreId240p == 0x24F1A2C4u);

    const auto p624 = selectCoreIdFromStream(624, 240, false);
    CHECK(p624.coreId480i == 0u && p624.coreId240p == 0x24F1A2C4u);

    const auto p480 = selectCoreIdFromStream(640, 480, false);
    CHECK(p480.coreId480i == 0u && p480.coreId240p == 0u);

    const auto unknown = selectCoreIdFromStream(0, 0, false);
    CHECK(unknown.coreId480i == 0u && unknown.coreId240p == 0u);

    const uint32_t p = kSelectedCoreId480p, i = kSelectedCoreId480i, b = kSelectedCoreId240p;
    CHECK(selectCoreIdForMode(624, 240, p, i, b) == b);
    CHECK(selectCoreIdForMode(320, 240, p, i, b) == b);
    CHECK(selectCoreIdForMode(720, 480, p, i, b) == i);
    CHECK(selectCoreIdForMode(624, 480, p, i, b) == p);
    CHECK(selectCoreIdForMode(640, 480, p, i, b) == p);
    CHECK(selectCoreIdForMode(0, 0, p, i, b) == p);
    CHECK(selectCoreIdForMode(720, 0, p, i, b) == p);

    CHECK(selectedCoreIdForPlay(720, 480, false) == i);
    CHECK(selectedCoreIdForPlay(720, 240, true) == i);
    CHECK(selectedCoreIdForPlay(320, 240, false) == b);
    CHECK(selectedCoreIdForPlay(640, 480, false) == p);
    CHECK(selectedCoreIdForPlay(0, 0, false) == p);
    CHECK(recognisedFpgaBuild(kSelectedCoreId480p) != nullptr);
    CHECK(std::strcmp(recognisedFpgaBuild(kSelectedCoreId480p)->raster, "480p") == 0);

    CHECK(kHighCabacCoreId480p == 0x48A64AEDu);
    CHECK(kSelectedCoreId480p != kHighCabacCoreId480p);
    const FpgaBuildInfo* high = recognisedFpgaBuild(kHighCabacCoreId480p);
    CHECK(high != nullptr);
    CHECK(high && std::strcmp(high->raster, "480p") == 0);
    CHECK(high && high->highCabac);
    CHECK(pairedBuildForEntropy(640, 480, false, true) == kHighCabacCoreId480p);
    CHECK(pairedBuildForEntropy(640, 480, false, false) == p);
    CHECK(pairedBuildForEntropy(0, 0, false, true) == kHighCabacCoreId480p);
    CHECK(pairedBuildForEntropy(320, 240, false, true) == b);
    CHECK(pairedBuildForEntropy(720, 480, false, true) == i);
    CHECK(pairedBuildForEntropy(720, 240, true, true) == i);
    CHECK(selectedCoreIdForPlay(640, 480, false) == p);
    CHECK(selectedCoreIdForContentLabel("240p") == b);
    CHECK(selectedCoreIdForContentLabel("480i") == i);
    CHECK(selectedCoreIdForContentLabel("480p") == p);
    CHECK(selectedCoreIdForContentLabel("") == 0u);
    CHECK(menuReloadTargetForPlay("480p", 720, 524, false, "480i", "720x480") == i);
    CHECK(menuReloadTargetForPlay("480p", 1440, 1080, false, "240p", "320x240") == b);
    // rc2.1: known-progressive 480-line media stays 480p (glass 09:22: lab rk1 624x480p24 went
    // Plex_480i and CLOSED-STOPped on the 640-wide decode). Unknown scan keeps 00008470 (480i).
    CHECK(menuReloadTargetForPlay("480p", 624, 480, false, "", "", true) == p);   // MUT_MRT_IGNORE_PROGRESSIVE
    CHECK(menuReloadTargetForPlay("480p", 720, 480, false, "", "", true) == i);   // 720x480 coded 480i frame rule (q380) unchanged
    CHECK(menuReloadTargetForPlay("480p", 640, 480, false, "", "", true) == p);
    CHECK(menuReloadTargetForPlay("480p", 624, 480, false, "", "") == i);
    CHECK(menuReloadTargetForPlay("480p", 720, 480, false, "", "", false) == i);
    CHECK(menuReloadTargetForPlay("480p", 720, 480, true, "", "", true) == i);   // interlaced still 480i
    CHECK(menuReloadTargetForPlay("480p", 720, 524, false, "480i", "720x480", true) == i); // weak 480i wins
    CHECK(menuReloadTargetForPlay("480p", 640, 240, false, "", "", true) == b);   // 240p rule unchanged
    CHECK(selectedCoreIdForDecisionOutput(624, 468) == b);
    CHECK(selectedCoreIdForDecisionOutput(624, 454) == i);
    CHECK(selectedCoreIdForDecisionOutput(624, 264) == 0u);
    CHECK(selectedCoreIdForDecisionOutput(640, 480) == 0u);
    CHECK(menuTargetAfterDecision(i, 320, 232) == i);
    CHECK(menuTargetAfterDecision(p, 624, 264) == p);
    CHECK(menuTargetAfterDecision(p, 624, 468) == b);
    CHECK(menuTargetAfterDecision(b, 320, 232) == b);
    using misterplex::menuReloadUtilityTarget;
    // F531d: 624x454 stays on the 480i Utility. No fallback to 0x48C51FF1.
    CHECK(menuReloadUtilityTarget(menuTargetAfterDecision(p, 624, 454)) == i);
    CHECK(menuReloadUtilityTarget(menuTargetAfterDecision(i, 624, 454)) == i);
    CHECK(misterplex::menuTargetAfterLoweredRetry(
              menuReloadUtilityTarget(menuTargetAfterDecision(p, 320, 240)), b, 320, 240, true) == b);
    CHECK(misterplex::menuTargetAfterLoweredRetry(
              menuReloadUtilityTarget(menuTargetAfterDecision(p, 320, 134)), b, 320, 134, false) ==
          0x48C51FF1u);
    CHECK(menuTargetAfterDecision(i, 720, 480) == i);
    // F531b 480p target ID
    using misterplex::menuTargetSatisfied;
    using misterplex::kUtilityCoreId480p;
    CHECK(kUtilityCoreId480p == 0x48C51FF1u);
    CHECK(menuReloadUtilityTarget(menuTargetAfterDecision(p, 624, 264)) == 0x48C51FF1u);
    CHECK(menuReloadUtilityTarget(i) == i);
    CHECK(menuReloadUtilityTarget(b) == b);
    CHECK(menuTargetSatisfied(0x48C51FF1u, 0x48C51FF1u));
    CHECK(!menuTargetSatisfied(0x48C51FF1u, p));
    CHECK(!menuTargetSatisfied(0x48C51FF1u, i));
    CHECK(!menuTargetSatisfied(0x48C51FF1u, b));
    CHECK(!menuTargetSatisfied(i, p));
    CHECK(!menuTargetSatisfied(b, 0x48C51FF1u));
    CHECK(misterplex::menuReloadAction(i, 0x48C51FF1u, "Plex") == misterplex::MenuReloadAction::BounceMenu);
    CHECK(misterplex::menuReloadAction(b, 0x48C51FF1u, "Menu") == misterplex::MenuReloadAction::LoadUtility);
    // switch 480p->480i->240p->High->480p: every 480p hop targets the Utility build
    {
        const uint32_t seq[] = {0x48C51FF1u, i, b, 0x48A64AEDu, 0x48C51FF1u};
        uint32_t loaded = p;
        for (uint32_t t : seq) {
            if (!menuTargetSatisfied(t, loaded)) loaded = t;
            CHECK(menuTargetSatisfied(t, loaded));
            CHECK(loaded != p || t == 0x48C51FF1u);
        }
        CHECK(loaded == 0x48C51FF1u);
    }

    using misterplex::utilityRbfForBuild;
    using misterplex::menuReloadWanted;
    CHECK(menuReloadWanted(p, b));
    using misterplex::menuReloadAction;
    using misterplex::MenuReloadAction;
    using misterplex::loadCoreLineAllowed;
    CHECK(std::strcmp(utilityRbfForBuild(b), "/media/fat/_Utility/Plex_240p15.rbf") == 0);
    CHECK(std::strcmp(utilityRbfForBuild(i), "/media/fat/_Utility/Plex_480i.rbf") == 0);
    CHECK(std::strcmp(utilityRbfForBuild(p), "/media/fat/_Utility/Plex_480p.rbf") == 0);
    CHECK(utilityRbfForBuild(kHighCabacCoreId480p) == nullptr);
    CHECK(menuReloadWanted(b, i));
    CHECK(!menuReloadWanted(i, i));
    CHECK(!menuReloadWanted(0, i));
    CHECK(!menuReloadWanted(b, kHighCabacCoreId480p));
    CHECK(menuReloadAction(b, i, "Plex") == MenuReloadAction::BounceMenu);
    CHECK(menuReloadAction(b, i, "MENU") == MenuReloadAction::LoadUtility);
    CHECK(menuReloadAction(b, i, "Menu") == MenuReloadAction::LoadUtility);
    CHECK(menuReloadAction(b, i, "menu") == MenuReloadAction::LoadUtility);
    CHECK(menuReloadAction(i, i, "MENU") == MenuReloadAction::Skip);
    CHECK(menuReloadAction(0, i, "MENU") == MenuReloadAction::LoadUtility);
    CHECK(menuReloadAction(0, i, "Plex") == MenuReloadAction::Skip);
    CHECK(menuReloadAction(b, kHighCabacCoreId480p, "MENU") == MenuReloadAction::Skip);
    CHECK(loadCoreLineAllowed("load_core /media/fat/menu.rbf"));
    CHECK(loadCoreLineAllowed("load_core /media/fat/_Utility/Plex_480i.rbf"));
    CHECK(!loadCoreLineAllowed("load_core /media/fat/_Utility/Plex.rbf"));
    CHECK(!loadCoreLineAllowed("load_core /media/fat/Plex.rbf"));

    using misterplex::rasterFamilyChanges;
    using misterplex::kVideoModeCmd240pLcd;
    using misterplex::kVideoModeCmd480p;
    const uint32_t u480 = 0x48C51FF1u;
    CHECK(rasterFamilyChanges(b, i));
    CHECK(rasterFamilyChanges(i, b));
    CHECK(rasterFamilyChanges(b, u480));
    CHECK(rasterFamilyChanges(u480, i));
    CHECK(!rasterFamilyChanges(b, b));
    CHECK(!rasterFamilyChanges(p, u480));
    CHECK(!rasterFamilyChanges(u480, p));
    CHECK(!rasterFamilyChanges(0, b));
    CHECK(std::strcmp(kVideoModeCmd240pLcd,
                      "video_mode 640,16,96,48,480,10,2,33,25175") == 0);
    CHECK(std::strcmp(kVideoModeCmd240pLcd, kVideoModeCmd480p) != 0);
    using misterplex::baseline240pRejectsProfile;
    CHECK(baseline240pRejectsProfile(b, 100));
    CHECK(baseline240pRejectsProfile(b, 77));
    CHECK(!baseline240pRejectsProfile(b, 66));
    CHECK(!baseline240pRejectsProfile(i, 100));
    CHECK(!baseline240pRejectsProfile(u480, 100));
    using misterplex::baselineCorePlayResolution;
    using misterplex::playUrlHasVideoResolution;
    CHECK(std::strcmp(baselineCorePlayResolution(b), "320x240") == 0);
    CHECK(std::strcmp(baselineCorePlayResolution(i), "624x480") == 0);
    CHECK(std::strcmp(baselineCorePlayResolution(u480), "624x480") == 0);
    using misterplex::presentBankWidthForMenuTarget;
    using misterplex::presentBankHeightForMenuTarget;
    using misterplex::menuReloadCoreLine;
    CHECK(presentBankWidthForMenuTarget(b) == 320);
    CHECK(presentBankHeightForMenuTarget(b) == 240);
    CHECK(presentBankWidthForMenuTarget(i) == 624);
    CHECK(presentBankHeightForMenuTarget(i) == 480);
    CHECK(presentBankWidthForMenuTarget(u480) == 640);
    CHECK(presentBankHeightForMenuTarget(u480) == 480);
    CHECK(std::strcmp(menuReloadCoreLine(i),
                      "load_core /media/fat/_Utility/Plex_480i.rbf") == 0);
    CHECK(std::strcmp(menuReloadCoreLine(u480),
                      "load_core /media/fat/_Utility/Plex_480p.rbf") == 0);
    CHECK(std::strcmp(baselineCorePlayResolution(p), "624x480") == 0);
    CHECK(playUrlHasVideoResolution("http://x/start?videoResolution=624x480&videoProfile=baseline",
                                    "624x480"));
    CHECK(!playUrlHasVideoResolution("http://x/start?videoResolution=320x240&videoProfile=baseline",
                                     "624x480"));
    using misterplex::daemonTermShouldExit;
    CHECK(!daemonTermShouldExit(true, false));
    CHECK(!daemonTermShouldExit(true, true));
    CHECK(daemonTermShouldExit(false, false));
    CHECK(daemonTermShouldExit(false, true));
    using misterplex::menuReloadSkipsLiveModelineForce;
    using misterplex::utility480pHdmiReprobeAfterTtfp;
    CHECK(menuReloadSkipsLiveModelineForce(p, u480));
    CHECK(menuReloadSkipsLiveModelineForce(i, u480));
    CHECK(!menuReloadSkipsLiveModelineForce(b, u480));
    CHECK(!menuReloadSkipsLiveModelineForce(i, b));
    CHECK(utility480pHdmiReprobeAfterTtfp(i, u480));
    CHECK(!utility480pHdmiReprobeAfterTtfp(p, u480));
    CHECK(!utility480pHdmiReprobeAfterTtfp(b, u480));
    using misterplex::stopShouldWaitForResolve;
    CHECK(stopShouldWaitForResolve(true, false));
    CHECK(!stopShouldWaitForResolve(true, true));
    CHECK(!stopShouldWaitForResolve(false, false));
    CHECK(!stopShouldWaitForResolve(false, true));

    if (fails) {
        std::fprintf(stderr, "%d FAIL\n", fails);
        return 1;
    }
    std::printf("test_core_id_select: OK\n");
    return 0;
}
