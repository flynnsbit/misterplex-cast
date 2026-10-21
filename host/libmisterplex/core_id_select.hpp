#pragma once

#include "ddr_frame_layout.hpp"
#include "fpga_build_ids.hpp"

#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {

// 480i P_PART+COLRAM RBF s5 203158fe (2ad700e7a). Closed 4801F40A stays recognised.
inline constexpr uint32_t kSelectedCoreId480i = 0x48F14A0Bu;
// F1 240p P_PART+COLRAM RBF s5 247b062f (57beec621). Closed 24003997 stays recognised.
inline constexpr uint32_t kSelectedCoreId240p = 0x24F1A2C4u;

struct CoreIdSelect {
    uint32_t coreId480i = 0;
    uint32_t coreId240p = 0;
};

// Stream coded size + interlace. Decode-bank outW_/outH_ are not stream size.
// 480i wins over 240p when interlaced (480i field height is also 240).
// 720x480 with interlaced==false still counts as 480i (Plex 480i coded frame).
// 640x480 progressive and unknown size stay 0/0 so prod 480p 0x487A1BD1 remains.
inline CoreIdSelect selectCoreIdFromStream(int width, int height, bool interlaced) {
    CoreIdSelect s;
    const bool h480 = height == kPlex480iCodedHeight;
    const bool h240 = height == kPlex240pCodedHeight;
    const bool wOk480i = width <= 0 || width <= kPlex480iCodedWidth;
    const bool wOk240p = width <= 0 || width <= kPlex240pPresentedWidth;
    const bool coded480iFrame =
        width == kPlex480iCodedWidth && height == kPlex480iCodedHeight;
    if ((interlaced && (h480 || h240) && wOk480i) || coded480iFrame) {
        s.coreId480i = kSelectedCoreId480i;
        return s;
    }
    if (!interlaced && h240 && wOk240p) {
        s.coreId240p = kSelectedCoreId240p;
        return s;
    }
    return s;
}

// Size-only map (q380). Unknown / 0-height keeps 480p. Prefer
// selectCoreIdFromStream when interlace is known (720x240i is 480i).
inline uint32_t selectCoreIdForMode(int width, int height, uint32_t id480p, uint32_t id480i,
                                    uint32_t id240p) {
    if (height > 0 && height <= 240)
        return id240p;
    if (width == kPlex480iCodedWidth && height == kPlex480iCodedHeight)
        return id480i;
    return id480p;
}

inline constexpr uint32_t kSelectedCoreId480p = 0x487A1BD1u;

inline uint32_t selectedCoreIdForPlay(int width, int height, bool interlaced,
                                     uint32_t id480p = kSelectedCoreId480p) {
    const auto s = selectCoreIdFromStream(width, height, interlaced);
    if (s.coreId480i)
        return s.coreId480i;
    if (s.coreId240p)
        return s.coreId240p;
    return id480p;
}

// PMS content ladder / OSD O[5:4] label → closed Utility BUILD_ID (0 = unknown).
inline uint32_t selectedCoreIdForContentLabel(const char* label) {
    if (!label || !*label)
        return 0;
    if (std::strcmp(label, "240p") == 0)
        return kSelectedCoreId240p;
    if (std::strcmp(label, "480i") == 0)
        return kSelectedCoreId480i;
    if (std::strcmp(label, "480p") == 0)
        return kSelectedCoreId480p;
    return 0;
}

// Goal F menu reload: prod Plex.rbf may be loaded while the play ladder is 240p/480i.
// rc2.1: mediaProgressive = PMS says the stream is progressive (scanType=progressive or a
// videoFrameRate like 24p/30p). The NTSC 480-line heuristic below (00008470: unknown-scan
// remote PMS SD -> Plex_480i) must not send known-progressive 480-line media (e.g. 624x480p24) to the
// 480i Utility: that core cannot present the 640-wide 480p decode and the play CLOSED-STOPs.
inline uint32_t menuReloadTargetForPlay(const char* contentLabel, int mediaW, int mediaH,
                                        bool mediaInterlaced, const std::string& weakProfile,
                                        const std::string& weakResolution,
                                        bool mediaProgressive = false) {
    uint32_t target = selectedCoreIdForContentLabel(contentLabel);
    if (weakProfile == "240p" || weakResolution.find("x240") != std::string::npos)
        target = kSelectedCoreId240p;
    else if (weakProfile == "480i")
        target = kSelectedCoreId480i;
    const uint32_t stream = selectedCoreIdForPlay(mediaW, mediaH, mediaInterlaced);
    if (stream != kSelectedCoreId480p)
        target = stream;
#ifdef MUT_MRT_IGNORE_PROGRESSIVE
    mediaProgressive = false;
#endif
    if (target == kSelectedCoreId480p && !mediaProgressive && mediaW > 0 &&
        mediaW <= kPlex480iCodedWidth && mediaH >= 480 && mediaH <= 525)
        target = kSelectedCoreId480i;
    if (target == kSelectedCoreId480p && mediaH > 0 && mediaH <= 240 &&
        mediaW <= kPlex240pPresentedWidth)
        target = kSelectedCoreId240p;
    return target;
}

// PMS universal decision output (e.g. remote PMS 624x468 on a 480-tier ladder).
inline uint32_t selectedCoreIdForDecisionOutput(int width, int height) {
    if (width <= 0 || height <= 0)
        return 0;
    if (height <= 240 && width <= kPlex240pPresentedWidth)
        return kSelectedCoreId240p;
    if (width == kPlex480iCodedWidth && height == kPlex480iCodedHeight)
        return kSelectedCoreId480i;
    // remote PMS universal 624-wide ladder: 468 → 240p (29729); ~454 field → 480i (37985).
    if (width == kPlex480pCodedWidth && height == 468)
        return kSelectedCoreId240p;
    if (width == kPlex480pCodedWidth && height >= 450 && height < kPlex480pCodedHeight)
        return kSelectedCoreId480i;
    return 0;
}

// Decision geometry may replace the play target only for a known ladder remap.
// A 240p-sized decision (320x232, or 624x264) must not cancel a 480i or 480p
// Utility load. remote PMS 624x468 still remaps a 480p default to the 240p core.
inline uint32_t menuTargetAfterDecision(uint32_t menuTarget, int decisionW, int decisionH) {
    const uint32_t decisionTarget = selectedCoreIdForDecisionOutput(decisionW, decisionH);
    if (!decisionTarget)
        return menuTarget;
    if (menuTarget == kSelectedCoreId480i && decisionTarget != kSelectedCoreId480i)
        return menuTarget;
    if (menuTarget == kSelectedCoreId480p && decisionTarget == kSelectedCoreId240p &&
        !(decisionW == kPlex480pCodedWidth && decisionH == 468))
        return menuTarget;
    return decisionTarget;
}

// F531b: the 480p Utility file Plex_480p.rbf is BUILD_ID 0x48C51FF1. Old prod
// Plex.rbf 0x487A1BD1 is never a menu reload target and does not satisfy one:
// 172 L1 stayed on it and HDMI was locked black, while L4 on 0x48C51FF1 painted.
inline constexpr uint32_t kUtilityCoreId480p = 0x48C51FF1u;

inline uint32_t menuReloadUtilityTarget(uint32_t target) {
    return target == kSelectedCoreId480p ? kUtilityCoreId480p : target;
}

// A lowered retry whose decision is 240p-sized stays on the 240p core already
// loaded. The content label is still 480p, which would otherwise reload away.
inline uint32_t menuTargetAfterLoweredRetry(uint32_t menuTarget, uint32_t loaded,
                                            int decisionW, int decisionH, bool lowered) {
    if (!lowered || loaded != kSelectedCoreId240p)
        return menuTarget;
    if (selectedCoreIdForDecisionOutput(decisionW, decisionH) != kSelectedCoreId240p)
        return menuTarget;
    return kSelectedCoreId240p;
}

// F531d: a 480i decision stays on Plex_480i (0x48F14A0B). 624x454 is that core.
// Do not retarget it to the 480p Utility.
inline bool menuTargetSatisfied(uint32_t target, uint32_t loaded) {
    return target == 0 || loaded == target;
}

// Closed High RBF s5 353e7c6d. Not the prod 480p selection.
inline constexpr uint32_t kHighCabacCoreId480p = 0x48A64AEDu;

// PPS entropy_coding_mode picks the paired BUILD_ID. Log only: callers must not load_core.
// CABAC on a 480p geometry (including unknown 0x0, which stays in the 480p family) pairs
// with the High core. CABAC on 240p or 480i stays on that baseline pair; those rasters
// have no closed High RBF. CAVLC always stays on the baseline pair.
inline uint32_t pairedBuildForEntropy(int width, int height, bool interlaced, bool cabac) {
    const uint32_t baseline = selectedCoreIdForPlay(width, height, interlaced);
    if (cabac && baseline == kSelectedCoreId480p)
        return kHighCabacCoreId480p;
    return baseline;
}

// Named Utility file for a closed pair. High has no resolver name, so it
// returns null and the caller must not load_core. Never the generic Plex.rbf.
inline const char* utilityRbfForBuild(uint32_t id) {
    switch (id) {
    case kSelectedCoreId240p:
        return "/media/fat/_Utility/Plex_240p15.rbf";
    case kSelectedCoreId480i:
        return "/media/fat/_Utility/Plex_480i.rbf";
    case kSelectedCoreId480p:
    case 0x48C51FF1u: // matrix 480p 6365a3d6
    case 0x48CFEE31u: // v12 052990c6
        return "/media/fat/_Utility/Plex_480p.rbf";
    default:
        return nullptr;
    }
}

// One swap when the paired id is a named file and is not the core already loaded.
inline bool menuReloadWanted(uint32_t loaded, uint32_t paired) {
    if (loaded == 0 || paired == 0 || loaded == paired)
        return false;
    return utilityRbfForBuild(paired) != nullptr;
}

// The utility load_core runs only after the live name is Menu. A name that
// is already Menu skips another menu bounce. Anything else bounces first.
enum class MenuReloadAction { Skip, BounceMenu, LoadUtility };

inline bool coreNameIsMenu(const char* name) {
    if (!name)
        return false;
    return std::strcmp(name, "MENU") == 0 || std::strcmp(name, "Menu") == 0 ||
           std::strcmp(name, "menu") == 0;
}

inline MenuReloadAction menuReloadAction(uint32_t loaded, uint32_t paired, const char* coreName) {
    if (paired == 0 || utilityRbfForBuild(paired) == nullptr)
        return MenuReloadAction::Skip;
    if (loaded == paired)
        return MenuReloadAction::Skip;
    // After menu.rbf load_core the gate drops buildId before CORENAME is Menu; finish the swap.
    if (loaded == 0 && coreNameIsMenu(coreName))
        return MenuReloadAction::LoadUtility;
    if (!menuReloadWanted(loaded, paired))
        return MenuReloadAction::Skip;
    if (coreNameIsMenu(coreName))
        return MenuReloadAction::LoadUtility;
    return MenuReloadAction::BounceMenu;
}

// 240p, 480i, and 480p are different present rasters. Prod 0x487A1BD1 and
// utility 0x48C51FF1 are the same 480p family, so a reload between them does
// not clear the present latch. A cross (240p ↔ 480i ↔ 480p) does.
inline int presentRasterFamily(uint32_t build) {
    if (build == kSelectedCoreId240p)
        return 1;
    if (build == kSelectedCoreId480i)
        return 2;
    if (build == kUtilityCoreId480p || build == kSelectedCoreId480p)
        return 3;
    return 0;
}

inline bool rasterFamilyChanges(uint32_t loaded, uint32_t target) {
    const int from = presentRasterFamily(loaded);
    const int to = presentRasterFamily(target);
    return from != 0 && to != 0 && from != to;
}

// 183 L1 and L7: utility 480p after prod or 480i. The gate-open path force-writes
// video_mode even when that modeline is already live. A same-line poke resets
// ddr_frame_store and HDMI stays locked black while the play still ACKs.
// 240p → utility still force-writes: that modeline actually changes (L4 PASS).
inline bool menuReloadSkipsLiveModelineForce(uint32_t prior, uint32_t target) {
    if (target != kUtilityCoreId480p)
        return false;
    return prior == kSelectedCoreId480p || prior == kSelectedCoreId480i;
}

// 480i → utility changes modeline before any picture, so ascal can lock black.
// One video_mode write after the first presented picture renegotiates HDMI
// onto the filled store. Prod is already on that modeline; poking it after
// TTFP is the L1 reset again. 240p → utility already shows programme.
inline bool utility480pHdmiReprobeAfterTtfp(uint32_t prior, uint32_t loaded) {
    return loaded == kUtilityCoreId480p && prior == kSelectedCoreId480i;
}

// Plex_240p15 is Baseline CAVLC. profile_idc 100 is the source High SPS.
inline bool baseline240pRejectsProfile(uint32_t build, unsigned profileIdc) {
    return build == kSelectedCoreId240p && profileIdc > 66;
}

// The play URL has to match the core that will decode it. 174 L6 kept a 480i
// target (0x48F14A0B) while the decision URL was still videoResolution=320x240.
// 182 L7 stayed on the 240p bank after the Utility cross. Each menu target
// has one present size. 0 means this id does not own a bank.
inline int presentBankWidthForMenuTarget(uint32_t target) {
    if (target == kSelectedCoreId240p)
        return 320;
    if (target == kSelectedCoreId480i)
        return 624;
    if (target == kUtilityCoreId480p || target == kSelectedCoreId480p)
        return 640;
    return 0;
}

inline int presentBankHeightForMenuTarget(uint32_t target) {
    if (target == kSelectedCoreId240p)
        return 240;
    if (target == kSelectedCoreId480i || target == kUtilityCoreId480p ||
        target == kSelectedCoreId480p)
        return 480;
    return 0;
}

// Scorer reads "menu reload paired" plus a Utility load_core from this leg's
// tail. An already-loaded core does not issue load_core, so the line has to
// name the file that core would have loaded.
inline const char* menuReloadCoreLine(uint32_t target) {
    if (target == kSelectedCoreId240p)
        return "load_core /media/fat/_Utility/Plex_240p15.rbf";
    if (target == kSelectedCoreId480i)
        return "load_core /media/fat/_Utility/Plex_480i.rbf";
    if (target == kUtilityCoreId480p || target == kSelectedCoreId480p)
        return "load_core /media/fat/_Utility/Plex_480p.rbf";
    return nullptr;
}

inline const char* baselineCorePlayResolution(uint32_t target) {
    if (target == kSelectedCoreId240p)
        return "320x240";
    if (target == kSelectedCoreId480i)
        return "624x480";
    // 181 L4 reloaded Plex_480p and then played the previous leg's 320x240 URL.
    // The Utility id is the menu target, so it has to carry the 480p ladder size.
    if (target == kUtilityCoreId480p || target == kSelectedCoreId480p)
        return "624x480";
    return nullptr;
}

inline bool playUrlHasVideoResolution(const std::string& url, const char* resolution) {
    if (!resolution || !resolution[0])
        return true;
    return url.find(std::string("videoResolution=") + resolution) != std::string::npos;
}

// PlexOB is one core for 480i and 480p. 240p output is the other file.
// A 480i↔480p change is a settings write, not a load_core.
enum class OwenbCore { PlexOB, PlexOB240p };

inline bool owenbCoreForOutput(const char* outputMode, OwenbCore& out) {
    if (!outputMode)
        return false;
    if (std::strcmp(outputMode, "240p") == 0) {
        out = OwenbCore::PlexOB240p;
        return true;
    }
    if (std::strcmp(outputMode, "480i") == 0 || std::strcmp(outputMode, "480p") == 0) {
        out = OwenbCore::PlexOB;
        return true;
    }
    return false;
}

inline bool owenbNeedsLoadCore(OwenbCore loaded, OwenbCore target) {
    return loaded != target;
}

inline const char* owenbUtilityRbf(OwenbCore core) {
    if (core == OwenbCore::PlexOB240p)
        return "/media/fat/_Utility/PlexOB_240p.rbf";
    return "/media/fat/_Utility/PlexOB.rbf";
}

inline const char* owenbLoadCoreLine(OwenbCore core) {
    if (core == OwenbCore::PlexOB240p)
        return "load_core /media/fat/_Utility/PlexOB_240p.rbf";
    return "load_core /media/fat/_Utility/PlexOB.rbf";
}

// A PlexOB RBF the Y/C reload may target: the file that is actually loaded, when
// that path is under /media/fat (or /media/fat/_Utility) and the basename is one
// of the PlexOB names. Never a generic Plex.rbf, never a path outside /media/fat.
inline bool plexObReloadPathOk(const std::string& path) {
    if (path.find("..") != std::string::npos)
        return false;
    const std::string prefix = "/media/fat/";
    if (path.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string rest = path.substr(prefix.size());
    if (rest.empty() || rest.front() == '/' || rest.find("//") != std::string::npos)
        return false;
    std::string base = rest;
    const std::string util = "_Utility/";
    if (rest.compare(0, util.size(), util) == 0)
        base = rest.substr(util.size());
    if (base.find('/') != std::string::npos)
        return false;
    if (base.size() < 10 || base.compare(base.size() - 4, 4, ".rbf") != 0)
        return false;
    const std::string name = base.substr(0, base.size() - 4);
    return name == "PlexOB" || name == "PlexOB_240p" || name == "PlexOB_480p";
}

inline bool plexObReloadFileExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Resolve the RBF that is actually loaded. /tmp/RBFNAME is the CONF_STR core
// name ("PlexOB"), not a path. Prod is /media/fat/PlexOB.rbf, preferred over
// the _Utility copy. A full allow-listed path is used only when that file
// exists. An empty result means the bounce must be refused.
template <typename Exists>
inline std::string plexObReloadResolvePath(const std::string& loadedNameOrPath, Exists exists) {
    if (plexObReloadPathOk(loadedNameOrPath))
        return exists(loadedNameOrPath) ? loadedNameOrPath : std::string();
    std::string base = loadedNameOrPath;
    const size_t slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".rbf") == 0)
        base.resize(base.size() - 4);
    const char* first = nullptr;
    const char* second = nullptr;
    if (base == "PlexOB" || base.empty()) {
        first = "/media/fat/PlexOB.rbf";
        second = "/media/fat/_Utility/PlexOB.rbf";
    } else if (base == "PlexOB_240p") {
        first = "/media/fat/PlexOB_240p.rbf";
        second = "/media/fat/_Utility/PlexOB_240p.rbf";
    } else if (base == "PlexOB_480p") {
        first = "/media/fat/PlexOB_480p.rbf";
        second = "/media/fat/_Utility/PlexOB_480p.rbf";
    } else {
        return {};
    }
    if (first && exists(first))
        return first;
    if (second && exists(second))
        return second;
    return {};
}

template <typename Exists>
inline std::string plexObReloadLoadCoreLine(const std::string& loadedNameOrPath, Exists exists) {
    const std::string path = plexObReloadResolvePath(loadedNameOrPath, exists);
    if (path.empty())
        return {};
    return std::string("load_core ") + path;
}

inline std::string plexObReloadResolvePath(const std::string& loadedNameOrPath) {
    return plexObReloadResolvePath(loadedNameOrPath, plexObReloadFileExists);
}

inline std::string plexObReloadLoadCoreLine(const std::string& loadedNameOrPath) {
    return plexObReloadLoadCoreLine(loadedNameOrPath, plexObReloadFileExists);
}

// The box is on the menu core. That is the gap between menu.rbf and the
// PlexOB load_core, including after an INT in that gap.
inline bool strandedOnMenuCore(const char* coreName) {
    return coreNameIsMenu(coreName);
}

// Written immediately before load_core menu.rbf, removed once PlexOB's
// load_core has been issued. A user who opened the menu themselves has no
// marker, so a daemon restart must not pull them back onto PlexOB.
inline constexpr const char* kMenuBounceMarkerPath = "/media/fat/linux/misterplex/yc-menu-bounce";

inline bool menuBounceRecoveryDue(const char* coreName, bool markerPresent) {
    return markerPresent && strandedOnMenuCore(coreName);
}

// Process start on the menu core, with or without the bounce marker. A
// mid-session "user opened menu" still uses menuBounceRecoveryDue, which
// stays false when the marker is absent.
inline bool bareDaemonMenuReload(const char* coreName) {
    return strandedOnMenuCore(coreName);
}

// Marker still on disk, core already left Menu. Leaving the file makes the
// next real visit to the menu look like the bounce gap.
inline bool staleMenuBounceMarker(const char* coreName, bool markerPresent) {
    return markerPresent && !strandedOnMenuCore(coreName);
}

inline bool menuBounceMarkerPresent(const char* path) {
    if (!path || !path[0])
        return false;
    struct stat st {};
    return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

inline bool writeMenuBounceMarker(const char* path) {
    if (!path || !path[0])
        return false;
    const std::string tmp = std::string(path) + ".mpx-tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    const char mark = '1';
    const bool wrote = ::write(fd, &mark, 1) == 1;
    if (wrote)
        ::fsync(fd);
    ::close(fd);
    if (!wrote || ::rename(tmp.c_str(), path) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    const std::string parent(path);
    const auto slash = parent.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string dir = slash == 0 ? std::string("/") : parent.substr(0, slash);
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
    return true;
}

inline void clearMenuBounceMarker(const char* path) {
    if (path && path[0])
        ::unlink(path);
}

// Do not start a menu bounce while shutting down. A bounce already in
// progress (menu loaded) must still be finished with the PlexOB load_core.
inline bool plexObReloadWriteMenu(bool shuttingDown, bool menuLoaded) {
    return !menuLoaded && !shuttingDown;
}

inline bool plexObReloadWriteCore(bool shuttingDown, bool menuLoaded) {
    (void)shuttingDown;
    return menuLoaded;
}

// Allowlist for /dev/MiSTer_cmd. Generic _Utility/Plex.rbf is rejected.
inline bool loadCoreLineAllowed(const char* line) {
    if (!line)
        return false;
    if (std::strcmp(line, "load_core /media/fat/menu.rbf") == 0 ||
        std::strcmp(line, "load_core /media/fat/_Utility/Plex_240p15.rbf") == 0 ||
        std::strcmp(line, "load_core /media/fat/_Utility/Plex_480i.rbf") == 0 ||
        std::strcmp(line, "load_core /media/fat/_Utility/Plex_480p.rbf") == 0 ||
        std::strcmp(line, "load_core /media/fat/_Utility/PlexOB.rbf") == 0 ||
        std::strcmp(line, "load_core /media/fat/_Utility/PlexOB_240p.rbf") == 0)
        return true;
    constexpr const char* kPrefix = "load_core ";
    if (std::strncmp(line, kPrefix, std::strlen(kPrefix)) != 0)
        return false;
    return plexObReloadPathOk(line + std::strlen(kPrefix));
}

}  // namespace misterplex
