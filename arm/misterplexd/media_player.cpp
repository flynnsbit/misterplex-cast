#include <pthread.h>
#include "media_player.hpp"
#include "companion.hpp"
#include "log_redact.hpp"
#include <sstream>

#include "libmisterplex/gcast_osd.hpp"
#include "libmisterplex/cached_src_phys.hpp"
#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/fps_gate.hpp"
#include "libmisterplex/core_id_select.hpp"
#include "libmisterplex/stalled_pic_watchdog.hpp"
#include "libmisterplex/gate_error_render.hpp"
#include "libmisterplex/idle_pairing_error.hpp"
#include "libmisterplex/input_mailbox.hpp"
#include "libmisterplex/av_clock.hpp"
#include "libmisterplex/pts_jump_clamp.hpp"
#include "libmisterplex/audio_lead_clamp.hpp"
#include "libmisterplex/bitrate_floor_480i.hpp"
#include "libmisterplex/h264_sps.hpp"
#include "libmisterplex/sps_reject.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/idle_screen.hpp"
#include "libmisterplex/osd_menu.hpp"
#include "libmisterplex/plexob_ini.hpp"
#include "libmisterplex/plexob_yc.hpp"
#include "libmisterplex/display_raster.hpp"
#include "libmisterplex/hdmi_auto_delay.hpp"
#include "libmisterplex/stick_bank_score.hpp"
#include "libmisterplex/h264_nal_dispatch.hpp"
#include "libmisterplex/h264_recon.hpp"
#include "libmisterplex/h264_sps.hpp"
#include "libmisterplex/av_inproc_decode.hpp"
#include "libmisterplex/p720_transcode_vf.hpp"
#include "transcode_offset.hpp"
#include "gcast/dash_seek.hpp"
#include "gcast/open_retry.hpp"
#include "libmisterplex/p720_audio_keepup.hpp"
#include "libmisterplex/present_bank.hpp"
#include "libmisterplex/source_aspect.hpp"
#include "libmisterplex/sps_crop_letterbox.hpp"
#include "libmisterplex/d52_log.hpp"
#include "libmisterplex/browse_r1_adapter.hpp"
#include "libmisterplex/plexui/plexui_idle_paint.hpp"
#include "libmisterplex/fpga_playback_overlay.hpp"
#include "libmisterplex/overlay_publish.hpp"
#include "plex_resolve.hpp"
#if defined(MPX_HAVE_OWENB_RING)
#include "libmisterplex/owenb_play.hpp"   // OWENB-P1B T5: the OB play worker
#include "libmisterplex/owenb_fbcon_guard.hpp"   // OWENB-P1B 0009: fbcon off fb0 (G15)
#include "libmisterplex/owenb_input.hpp"   // OWENB-P1B 0010: the core's input region
#include "libmisterplex/owenb_menu.hpp"    // OWENB-P1B 0010: the menu panel, the routing
#include "libmisterplex/owenb_library.hpp" // OWENB-P1B 0010: the library page
#if defined(MPX_HAVE_LIBAV)
#include "owenb_av_source.hpp"
#endif
#endif
#include "plexui_perf.hpp"
#if defined(MPX_HAVE_OWENB_RING)
#include "libmisterplex/owenb_plexui_nav.hpp"   // OB port: ring_in nav -> PlexUI ops (PLXI 5-11)
#if defined(__linux__)
#include <linux/fb.h>
#include <sys/ioctl.h>
#endif
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <optional>
#include <signal.h>
#include <thread>
#include <time.h>
#include <vector>

#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// OWENB-P1B T6: the OB overlay UI bus clock (steady, ms).
#if defined(MPX_HAVE_OWENB_RING)
static int64_t obUiNowMs() {
    return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif

#ifdef __linux__
#include <sys/syscall.h>
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MPX_FABRIC_DIRECT
#define MPX_FABRIC_DIRECT 0
#endif
#ifndef MPX_STICK_I420
#define MPX_STICK_I420 0
#endif

namespace misterplex {

namespace {
// TRUE640: 480p FPGA coded-width ceiling. Default 624 (kPlex480pCodedWidth).
// 640 (40 MB/row) only with the MPX_FPGA_480P_CODED_W=640 opt-in AND a core
// whose CAPS advertise max_width>=640 (PLEX_DECODE_CODED_640); 39-MB or
// stale/unknown CAPS keep 624.
constexpr int kPlex480pTrue640CodedWidth = 640; // 40 MB/row
int plex480pCodedCeilingW(uint32_t capsMaxW) {
    if (fpga480pCodedWidthCeiling() == kPlex480pTrue640CodedWidth &&
        capsMaxW >= static_cast<uint32_t>(kPlex480pTrue640CodedWidth))
        return kPlex480pTrue640CodedWidth;
    return kPlex480pCodedWidth;
}

namespace {
// MiSTer_cmd can block while Main reloads; never stall the play thread unbounded.
int writeMisterCmdLineWithTimeout(const char* path, const char* line, int timeoutSec) {
    const pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        const int rc = writeMisterCmdLine(path, line);
        _exit(rc > 0 ? 0 : (rc == 0 ? 2 : 1));
    }
    int status = 0;
    for (int i = 0; i < timeoutSec * 10; ++i) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return 1;
            return 0;
        }
        if (w < 0 && errno == ECHILD)
            return 0;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 0;
}

bool liveCoreNameIsMenu() {
    if (coreNameIsMenu(readCoreNameFile().c_str()))
        return true;
    if (coreNameIsMenu(CoreGate::process().name().c_str()))
        return true;
    const std::string why = CoreGate::process().closedWhy();
    return why.find("loaded core 'MENU'") != std::string::npos ||
           why.find("loaded core 'Menu'") != std::string::npos ||
           why.find("loaded core 'menu'") != std::string::npos;
}
} // namespace

// Issue menu bounce + named Utility load_core. Logs coreNameMenu=1 when Menu was seen.
bool issueMenuReloadForPair(uint32_t loaded, uint32_t paired,
                            const std::function<void(const std::string&)>& log,
                            const std::function<void()>& tick = {}) {
    // The configured backend stays FpgaH264 on a PlexOB box. Return before the
    // build id is latched, while the fabric already holds the Owenb core.
    if (CoreGate::process().ownsIdleInFabric())
        return false;
    char pairedHex0[16];
    char loadedHex0[16];
    std::snprintf(pairedHex0, sizeof(pairedHex0), "0x%08X", paired);
    std::snprintf(loadedHex0, sizeof(loadedHex0), "0x%08X", loaded);
    std::fprintf(stderr, "misterplexd: menu reload begin loaded=%s paired=%s\n", loadedHex0,
                 pairedHex0);
    const char* file = utilityRbfForBuild(paired);
    std::string liveName = readCoreNameFile();
    if (!coreNameIsMenu(liveName.c_str())) {
        const std::string gateName = CoreGate::process().name();
        if (coreNameIsMenu(gateName.c_str()))
            liveName = gateName;
    }
    // menuReloadWanted rejects loaded==0. After menu.rbf the gate drops the
    // build id while CORENAME is already Menu; that is still a Utility load.
    const MenuReloadAction action = menuReloadAction(loaded, paired, liveName.c_str());
    if (!file || action == MenuReloadAction::Skip) {
        std::fprintf(stderr,
                     "misterplexd: menu reload skip loaded=%s paired=%s name=%s\n",
                     loadedHex0, pairedHex0, liveName.c_str());
        return false;
    }
    char pairedHex[16];
    char loadedHex[16];
    std::snprintf(pairedHex, sizeof(pairedHex), "0x%08X", paired);
    std::snprintf(loadedHex, sizeof(loadedHex), "0x%08X", loaded);
    char menuLine[80];
    char coreLine[80];
    std::snprintf(menuLine, sizeof(menuLine), "load_core /media/fat/menu.rbf");
    std::snprintf(coreLine, sizeof(coreLine), "load_core %s", file);
    int menuRc = 0;
    bool sawMenu = action == MenuReloadAction::LoadUtility;
    if (!sawMenu) {
        menuRc = writeMisterCmdLine(kMisterCmdPath, menuLine);
        const auto menuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (std::chrono::steady_clock::now() < menuDeadline) {
            if (liveCoreNameIsMenu()) {
                sawMenu = true;
                break;
            }
            if (tick)
                tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    int coreRc = 0;
    if (sawMenu) {
        if (const FpgaBuildInfo* row = recognisedFpgaBuild(paired)) {
            CoreGate::process().setPairedRaster(
                row->raster,
                std::string("Goal F menu reload: paired Utility ") + pairedHex);
        }
        std::fprintf(stderr, "misterplexd: menu reload issuing %s\n", coreLine);
        std::fflush(stderr);
        const auto utilityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < utilityDeadline && coreRc <= 0) {
            if (tick)
                tick();
            coreRc = writeMisterCmdLineWithTimeout(kMisterCmdPath, coreLine, 5);
            if (coreRc > 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    const std::string reloadLine =
        std::string("media: menu reload paired ") + pairedHex + " loaded " + loadedHex +
        (action == MenuReloadAction::LoadUtility ? " menu already"
                                                 : std::string(" ") + menuLine +
                                                       " rc=" + std::to_string(menuRc)) +
        " coreNameMenu=" + (sawMenu ? "1" : "0") +
        (sawMenu ? std::string(" ") + coreLine + " rc=" + std::to_string(coreRc)
                 : " utility withheld");
    log(reloadLine);
    std::fprintf(stderr, "%s\n", reloadLine.c_str());
    std::fflush(stderr);
    if (!sawMenu)
        std::fprintf(stderr, "misterplexd: menu reload utility withheld menuRc=%d coreRc=%d\n",
                     menuRc, coreRc);
    if (sawMenu)
        CoreGate::process().closeNow("menu reload: named Utility load_core");
    return sawMenu && coreRc > 0;
}
} // namespace

std::string MediaPlayer::selectCoreRbfForMode(int width, int height) const {
    const uint32_t id = selectedCoreIdForPlay(width, height, sourceInterlaced_);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", id);
    return buf;
}

bool MediaPlayer::tryMenuReloadForTargetBuild(uint32_t targetBuild) {
    if (CoreGate::process().ownsIdleInFabric())
        return false;
    if (!targetBuild || !fpgaH264Backend())
        return false;
    const auto logLine = [this](const std::string& line) {
        log(line);
        std::fprintf(stderr, "%s\n", line.c_str());
    };
    // 0x24F1A2C4 plex_fb_en_hold blanks HDMI 1s after PLXL stops changing.
    const auto armHoldBeat = [this] {
        const FpgaBuildInfo* row = recognisedFpgaBuild(CoreGate::process().buildId());
        if (!row || !row->idleBeatPlxl || shuttingDown_.load() || f1Beat_.running())
            return;
        f1Beat_.start();
        log("f1: PLXL beat started for plex_fb_en_hold");
        std::fprintf(stderr, "misterplexd: f1 PLXL beat started for plex_fb_en_hold\n");
    };
    const auto onMenuReloadGateOpen = [this, &armHoldBeat, targetBuild](uint32_t prior) {
        armHoldBeat();
        hdmiReprobeAfterTtfp_ = misterplex::utility480pHdmiReprobeAfterTtfp(prior, targetBuild);
        if (const FpgaBuildInfo* row = recognisedFpgaBuild(targetBuild)) {
            if (row->raster) {
                ContentResolution dr = resolutionFromLabel(row->raster);
                // 15 kHz 240p is not HDMI-safe; lab MJPEG capture needs 31 kHz LCD timing.
                if (dr.label && std::strcmp(dr.label, "240p") == 0)
                    dr = resolutionFromLabel("240p_lcd");
                const bool skipForce =
                    misterplex::menuReloadSkipsLiveModelineForce(prior, targetBuild);
                latchAndApplyDisplayRaster(dr);
                if (!skipForce)
                    applyDisplayRaster(dr, true);
                else
                    std::fprintf(stderr,
                                 "misterplexd: menu reload display raster %s kept "
                                 "(bookend modeline already live)\n",
                                 dr.label ? dr.label : "?");
                std::fprintf(stderr,
                             "misterplexd: menu reload display raster %s after Utility load\n",
                             dr.label ? dr.label : "?");
            }
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08X", targetBuild);
            const std::string adopted = std::string("media: core load adopted ") + hex +
                                        " raster " + (row->raster ? row->raster : "?") +
                                        " status " + fpgaStatusLayoutName(*row) +
                                        " overlay " + (overlayPlaneAbsent(targetBuild) ? "off" : "on");
            log(adopted);
            std::fprintf(stderr, "misterplexd: %s\n", adopted.c_str());
        }
    };
    const auto noteFamilyReset = [this](uint32_t fromBuild, uint32_t toBuild) {
        if (!misterplex::rasterFamilyChanges(fromBuild, toBuild))
            return;
        idleFbArmTried_ = false;
        playEpoch_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lk(r1PaintMu_);
            f1Beat_.stop();
        }
        std::string why;
        const bool cleared = r1EntryStreamClear(&why);
        char fromHex[16];
        char toHex[16];
        std::snprintf(fromHex, sizeof fromHex, "0x%08X", fromBuild);
        std::snprintf(toHex, sizeof toHex, "0x%08X", toBuild);
        const std::string line = std::string("media: raster family reset loaded=") + fromHex +
                                  " paired=" + toHex +
                                  " streamClear=" + (cleared ? "1" : "0");
        log(line);
        std::fprintf(stderr, "misterplexd: %s\n", line.c_str());
        if (!cleared && !why.empty())
            std::fprintf(stderr, "misterplexd: raster family stream clear: %s\n", why.c_str());
    };
    uint32_t priorLoaded = 0;
    for (int attempt = 0; attempt < 15; ++attempt) {
        const uint32_t loaded = CoreGate::process().buildId();
        priorLoaded = loaded;
        if (issueMenuReloadForPair(loaded, targetBuild, logLine, [this] { coreGateTick(); })) {
            for (int i = 0; i < 120; ++i) {
                coreGateTick();
                if (CoreGate::process().open() &&
                    CoreGate::process().buildId() == targetBuild) {
                    noteFamilyReset(loaded, targetBuild);
                    onMenuReloadGateOpen(loaded);
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const bool opened = CoreGate::process().open() &&
                                CoreGate::process().buildId() == targetBuild;
            if (opened) {
                noteFamilyReset(loaded, targetBuild);
                onMenuReloadGateOpen(loaded);
            }
            return opened;
        }
        coreGateTick();
        std::string name = readCoreNameFile();
        if (!coreNameIsMenu(name.c_str())) {
            const std::string gateName = CoreGate::process().name();
            if (coreNameIsMenu(gateName.c_str()))
                name = gateName;
        }
        if (!coreNameIsMenu(name.c_str()) && loaded != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const bool ok = CoreGate::process().open() &&
                    CoreGate::process().buildId() == targetBuild;
    if (ok) {
        noteFamilyReset(priorLoaded, targetBuild);
        onMenuReloadGateOpen(priorLoaded);
    }
    if (!ok)
        std::fprintf(stderr,
                     "misterplexd: menu reload gave up target=0x%08X gate build=0x%08X "
                     "open=%d name=%s\n",
                     targetBuild, CoreGate::process().buildId(),
                     CoreGate::process().open() ? 1 : 0,
                     CoreGate::process().name().c_str());
    return ok;
}

namespace {

// HTTP byte-range retry cap: maximum number of demux reopen attempts after a short-read/stall
// before the session is aborted.  Keeps the player from hanging on a permanently broken remote
// range.
constexpr int kRangeRetryMax = 3;

// Live MiSTer core name from Main (/tmp/CORENAME). Empty = unreadable/transient.
std::string readLiveCoreName() { return readCoreNameFile(); }

// True when the loaded core is still Plex (or name unavailable). MENU/other
// means the user left the product core — stop feeding MrAudio immediately so
// stream PCM cannot keep playing on the Menu ALSA path. The core gate
// (core_gate.hpp) applies the same name test to every FPGA access.
bool liveCoreStillPlex() {
    return coreGateAllowsAudioWrite();
}

// Drain remaining audio in MrAudio ring at session end so 480p does not hang.
static void eosAudioDrain_() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    char buf[160];
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = ::open("/dev/MrAudio", O_RDONLY);
        if (fd < 0)
            return;
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0)
            return;
        const int64_t queued = misterplex::parseMrAudioQueuedBytes(buf, n);
        if (!misterplex::eosAudioDrainShouldWait(queued, /*userStop=*/false))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

#if MPX_FPGA_AV_TRACE && defined(MPX_HAVE_LIBAV)
using AvTraceSpan = FpgaAvTrace::Span;
struct AvTraceRelease {
    AvTraceSpan& span;
    ~AvTraceRelease() { span.released(); }
};

void tracePressure(AvTraceSpan& span, const AvCompressedPressure& before,
                   const AvCompressedPressure& after) {
    span.flags(FpgaAvRecord::ApproximatePressure);
    if (before.available) span.uvalue(2, before.pcmBytes);
    if (!after.available) return;
    span.uvalue(3, after.pcmBytes);
    span.uvalue(4, after.queuedPackets);
    span.uvalue(5, after.queuedBytes);
    span.value(6, static_cast<int>(after.blocked));
    span.value(7, after.inputEof);
    span.value(8, after.audioEof);
    span.value(9, after.reservedVideo);
}

void traceMast(AvTraceSpan& span, bool ok, const FpgaSpi::AudioSessionStatus& status,
               int64_t written, int64_t held, bool paused, bool approximateWritten) {
    span.value(0, ok);
    span.value(7, written);
    span.value(9, held);
    span.value(10, paused);
    if (approximateWritten) span.flags(FpgaAvRecord::ApproximateWritten);
    if (!ok) { span.force(); return; }
    span.uvalue(1, status.session_id);
    span.uvalue(2, status.nonce);
    span.uvalue(3, status.publication);
    span.uvalue(4, status.samples_consumed);
    span.value(5, unsigned(status.active) | (unsigned(status.paused) << 1) |
        (unsigned(status.read_pending) << 2) | (unsigned(status.prefetched) << 3));
    span.value(6, status.error);
    if (written >= 0 &&
        status.samples_consumed <= uint64_t(std::numeric_limits<int64_t>::max() / 4))
        span.value(8, written - static_cast<int64_t>(status.samples_consumed) * 4);
    if (!status.active || status.error) span.force();
}
#endif

// PMS universal already bakes offset= (seconds). Applying FFmpeg -ss again double-seeks
// and breaks resume / mid-play scrub on STREAM=0 cast. Timeline still uses startMs.
inline bool isUniversalTranscodeUrl(const std::string& url) {
    return url.find("transcode/universal") != std::string::npos ||
           url.find("/video/:/transcode/") != std::string::npos;
}

inline bool waitPidMs(pid_t pid, int timeoutMs, int* statusOut) {
    if (pid <= 0)
        return false;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        int st = 0;
        const pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            if (statusOut)
                *statusOut = st;
            return true;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms >= timeoutMs)
            return false;
        ::usleep(20000);
    }
}

// Never mix pthread_timedjoin_np with std::thread: a successful native join
// leaves the C++ object joinable, and detach() then throws system_error
// "No such process" — terminate, Play spinner, last frame stuck. Interrupt
// + close PCM fds + WNOHANG reap make a normal join return.

// Branch B: compile-time MPX_FABRIC_DIRECT=1 or env MPX_FABRIC_DIRECT=1.
// Default OFF — 480p / 07f54d9f still use sendDdrFrame memcpy.
inline bool fabricDirectWanted() {
    const char* e = std::getenv("MPX_FABRIC_DIRECT");
    if (e && *e) {
        const char c = e[0];
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F' || c == 'o' ||
            c == 'O')
            return false;
        return true;
    }
#if MPX_FABRIC_DIRECT
    return true;
#else
    // Default OFF. Opt in via env/conf MPX_FABRIC_DIRECT=1; 720p still
    // geometry-gated at call sites.
    return false;
#endif
}

#if defined(__linux__)
pid_t findCommPid(const char* name) {
    DIR* dir = ::opendir("/proc");
    if (!dir)
        return -1;
    pid_t found = -1;
    while (dirent* ent = ::readdir(dir)) {
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        FILE* f = std::fopen(path, "r");
        if (!f)
            continue;
        char comm[32] = {};
        const bool ok = std::fgets(comm, sizeof(comm), f) != nullptr;
        std::fclose(f);
        if (!ok)
            continue;
        char* nl = std::strchr(comm, '\n');
        if (nl)
            *nl = 0;
        if (std::strcmp(comm, name) == 0) {
            found = static_cast<pid_t>(std::atoi(ent->d_name));
            break;
        }
    }
    ::closedir(dir);
    return found;
}

bool g_misterNiced = false;
int g_misterNiceSaved = 0;

void setMisterNice(int niceVal, const MediaPlayer::LogFn& log) {
    const pid_t pid = findCommPid("MiSTer");
    if (pid <= 0)
        return;
    errno = 0;
    const int cur = ::getpriority(PRIO_PROCESS, pid);
    if (errno != 0)
        return;
    if (!g_misterNiced) {
        g_misterNiceSaved = cur;
        g_misterNiced = true;
    }
    if (::setpriority(PRIO_PROCESS, pid, niceVal) == 0 && log) {
        log("media: MiSTer pid=" + std::to_string(pid) + " nice=" +
            std::to_string(niceVal) + " (was " + std::to_string(cur) + ")");
    }
}

void restoreMisterNice(const MediaPlayer::LogFn& log) {
    if (!g_misterNiced)
        return;
    const pid_t pid = findCommPid("MiSTer");
    if (pid > 0)
        (void)::setpriority(PRIO_PROCESS, pid, g_misterNiceSaved);
    if (log)
        log("media: MiSTer nice restored");
    g_misterNiced = false;
}
#endif

// URL start-offset helpers (shared with doPlay): arm/misterplexd/transcode_offset.hpp.
using transcode_offset::findQueryParam;
using transcode_offset::urlHasUniversalOffset;
using transcode_offset::ladderYtRemuxSrc;
using transcode_offset::withQueryParam;
using transcode_offset::withSeekSafeTranscodeParams;
using transcode_offset::withUniversalOffset;

static std::string urlWithBitrateFloor480i(std::string url) {
    if (!isUniversalTranscodeUrl(url))
        return url;
    const std::string liveRaster = CoreGate::process().pairedRaster();
    const char* mode = !liveRaster.empty() ? liveRaster.c_str() : std::getenv("MPX_FPGA_MODE");
    const char* present = std::getenv("MPX_PRESENT_480I");
    if (!bitrateFloor480iApplies(mode, present, nullptr))
        return url;
    std::string cur;
    int kbps = 0;
    const bool have = findQueryParam(url, "maxVideoBitrate", &cur);
    if (have) {
        try {
            kbps = std::stoi(cur);
        } catch (...) {
            kbps = 0;
        }
    }
    const int raised = have ? bitrate_floor_480i(mode, present, nullptr, kbps)
                            : bitrateFloor480i;
    if (!have || raised != kbps)
        return withQueryParam(url, "maxVideoBitrate", std::to_string(raised));
    return url;
}

// Annex-B start-code length at `i`, or 0 if none.
inline size_t annexBStartLen(const uint8_t* p, size_t n, size_t i) {
    if (i + 3 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
        return 4;
    if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        return 3;
    return 0;
}

// Local annex-B elementary H.264 (skip remux BSF when possible).
inline bool looksElementaryH264(const std::string& url) {
    if (url.empty() || url.rfind("http", 0) == 0 || url.rfind("lavfi", 0) == 0)
        return false;
    auto lower = url;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto q = lower.find('?');
    if (q != std::string::npos)
        lower = lower.substr(0, q);
    return lower.size() >= 5 &&
           (lower.compare(lower.size() - 5, 5, ".h264") == 0 ||
            lower.compare(lower.size() - 4, 4, ".264") == 0 ||
            lower.compare(lower.size() - 4, 4, ".avc") == 0);
}

inline bool confTruthyMode(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// In-place UV plane bias for planar YUV420p (Y then U then V). Used to counter
// measured fluorescent-green U-low on HDMI vs source (see UV_U_BIAS conf).
inline void applyYuv420pUvBias(uint8_t* yuv, int width, int height, int uBias, int vBias) {
    if (!yuv || width <= 0 || height <= 0 || (uBias == 0 && vBias == 0))
        return;
    const size_t yBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t cBytes = yBytes / 4u;
    uint8_t* u = yuv + yBytes;
    uint8_t* v = u + cBytes;
    auto add = [](uint8_t* p, size_t n, int bias) {
        if (bias == 0)
            return;
        for (size_t i = 0; i < n; ++i) {
            int x = static_cast<int>(p[i]) + bias;
            if (x < 0)
                x = 0;
            else if (x > 255)
                x = 255;
            p[i] = static_cast<uint8_t>(x);
        }
    };
    add(u, cBytes, uBias);
    add(v, cBytes, vBias);
}

inline int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

enum class RawVideoFormat {
    Rgb24,
    Rgb565Le,
    Bgra32,
    Yuv420p,
};

inline const char* ffmpegPixFmt(RawVideoFormat f) {
    switch (f) {
    case RawVideoFormat::Rgb565Le:
        return "rgb565le";
    case RawVideoFormat::Bgra32:
        return "bgra";
    case RawVideoFormat::Yuv420p:
        return "yuv420p";
    case RawVideoFormat::Rgb24:
    default:
        return "rgb24";
    }
}

inline size_t rawVideoFrameBytes(RawVideoFormat f, int width, int height) {
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    switch (f) {
    case RawVideoFormat::Rgb565Le:
        return pixels * 2;
    case RawVideoFormat::Bgra32:
        return pixels * 4;
    case RawVideoFormat::Yuv420p:
        return yuv420pFrameBytes(width, height);
    case RawVideoFormat::Rgb24:
    default:
        return pixels * 3;
    }
}

inline size_t rawVideoPackedBytesPerPixel(RawVideoFormat f) {
    switch (f) {
    case RawVideoFormat::Rgb565Le:
        return 2;
    case RawVideoFormat::Bgra32:
        return 4;
    case RawVideoFormat::Rgb24:
        return 3;
    case RawVideoFormat::Yuv420p:
    default:
        return 0;
    }
}

inline void clearYuv420pCropPadding(uint8_t* yuv, const DdrFrameGeometry& g) {
    if (!yuv || (g.crop_left == 0 && g.crop_right == 0 && g.crop_top == 0 && g.crop_bottom == 0))
        return;
    const int w = g.coded_width;
    const int h = g.coded_height;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return;

    auto clearPlane = [](uint8_t* plane, int stride, int width, int height, int cropLeft,
                         int cropRight, int cropTop, int cropBottom, uint8_t value) {
        const int topRows = std::max(0, std::min(cropTop, height));
        const int bottomRows = std::max(0, std::min(cropBottom, height - topRows));
        for (int y = 0; y < topRows; ++y)
            std::memset(plane + static_cast<size_t>(y) * stride, value, width);
        for (int y = height - bottomRows; y < height; ++y)
            std::memset(plane + static_cast<size_t>(y) * stride, value, width);
        const int first = topRows;
        const int last = height - bottomRows;
        const int left = std::max(0, std::min(cropLeft, width));
        const int right = std::max(0, std::min(cropRight, width - left));
        for (int y = first; y < last; ++y) {
            uint8_t* row = plane + static_cast<size_t>(y) * stride;
            if (left)
                std::memset(row, value, left);
            if (right)
                std::memset(row + width - right, value, right);
        }
    };

    const int yBytes = w * h;
    const int cW = w / 2;
    const int cH = h / 2;
    clearPlane(yuv, w, w, h, g.crop_left, g.crop_right, g.crop_top, g.crop_bottom,
               kYuv420BlackY);
    clearPlane(yuv + yBytes, cW, cW, cH, g.crop_left / 2, g.crop_right / 2, g.crop_top / 2,
               g.crop_bottom / 2, kYuv420BlackU);
    clearPlane(yuv + yBytes + cW * cH, cW, cW, cH, g.crop_left / 2, g.crop_right / 2,
               g.crop_top / 2, g.crop_bottom / 2, kYuv420BlackV);
}

inline int64_t microsBetween(std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

inline int64_t threadCpuMicros() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec) / 1000;
}

bool ffmpegHasAudioStream(const std::string& ffmpeg, const std::string& url,
                          const std::string& headers, int64_t startMs) {
    std::vector<std::string> args;
    args.push_back(ffmpeg);
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostdin");
    if (transcode_offset::demuxSeekMs(url, startMs) > 0) {
        char ss[32];
        std::snprintf(ss, sizeof(ss), "%.3f", startMs / 1000.0);
        args.push_back("-ss");
        args.push_back(ss);
    }
    if (!headers.empty()) {
        std::string h = headers;
        if (h.size() < 2 || h[h.size() - 1] != '\n')
            h += "\r\n";
        args.push_back("-headers");
        args.push_back(h);
        args.push_back("-reconnect");
        args.push_back("1");
        args.push_back("-reconnect_streamed");
        args.push_back("1");
        args.push_back("-reconnect_delay_max");
        args.push_back("5");
    }
    args.push_back("-i");
    args.push_back(url);
    args.push_back("-map");
    args.push_back("0:a:0");
    args.push_back("-frames:a");
    args.push_back("1");
    args.push_back("-f");
    args.push_back("null");
    args.push_back("-");

    pid_t pid = fork();
    if (pid < 0)
        return true; // fail open: do not suppress product audio just because probe fork failed
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull != STDOUT_FILENO && devnull != STDERR_FILENO)
                ::close(devnull);
        }
        for (int fd = 3; fd < 256; ++fd)
            ::close(fd);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execv(args[0].c_str(), argv.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR)
            continue;
        return true;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

void setSourceAspectProbeFail(std::string* dest, const char* why,
                              size_t stderrBytes = 0) {
    if (!dest)
        return;
    if (std::strcmp(why, "timeout") == 0 || std::strcmp(why, "empty_dar") == 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s stderr_b=%zu", why, stderrBytes);
        *dest = buf;
        return;
    }
    *dest = why;
}

SourceAspect ffmpegSourceAspect(const std::string& ffmpeg, const std::string& url,
                                const std::string& headers,
                                std::string* failDetail = nullptr,
                                int* codedW = nullptr,
                                int* codedH = nullptr,
                                int* fpsNum = nullptr,
                                int* fpsDen = nullptr) {
    int stderrPipe[2]{-1, -1};
#if defined(__linux__)
    if (pipe2(stderrPipe, O_CLOEXEC) != 0) {
        setSourceAspectProbeFail(failDetail, "pipe");
        return {};
    }
#else
    if (pipe(stderrPipe) != 0) {
        setSourceAspectProbeFail(failDetail, "pipe");
        return {};
    }
#endif

    std::vector<std::string> args = {
        ffmpeg, "-hide_banner", "-loglevel", "info", "-nostdin",
    };
    if (!headers.empty()) {
        std::string h = headers;
        if (h.size() < 2 || h[h.size() - 1] != '\n')
            h += "\r\n";
        args.push_back("-headers");
        args.push_back(h);
    }
    if (url.rfind("http", 0) == 0) {
        args.push_back("-rw_timeout");
        args.push_back("4000000");
    }
    args.insert(args.end(), {
        "-i", url, "-map", "0:v:0", "-frames:v", "0",
        "-an", "-sn", "-dn", "-f", "null", "-",
    });

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        setSourceAspectProbeFail(failDetail, "fork");
        return {};
    }
    if (pid == 0) {
        ::close(stderrPipe[0]);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            ::close(devnull);
        if (stderrPipe[1] > STDERR_FILENO)
            ::close(stderrPipe[1]);
        for (int fd = 3; fd < 256; ++fd)
            ::close(fd);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execv(args[0].c_str(), argv.data());
        _exit(127);
    }

    ::close(stderrPipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool timedOut = false;
    bool pipeClosed = false;
    while (!pipeClosed) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timedOut = true;
            break;
        }
        const int timeoutMs = std::max(
            1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - now)
                                    .count()));
        pollfd pfd{stderrPipe[0], POLLIN | POLLHUP, 0};
        const int ready = ::poll(&pfd, 1, timeoutMs);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            pipeClosed = true;
            break;
        }
        if (ready == 0) {
            timedOut = true;
            break;
        }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            const ssize_t n = ::read(stderrPipe[0], buffer.data(), buffer.size());
            if (n > 0) {
                if (output.size() < 65536) {
                    const size_t keep =
                        std::min(static_cast<size_t>(n), 65536u - output.size());
                    output.append(buffer.data(), keep);
                }
            } else if (n == 0) {
                pipeClosed = true;
            } else if (errno != EINTR) {
                pipeClosed = true;
            }
        } else if (pfd.revents & (POLLERR | POLLNVAL)) {
            pipeClosed = true;
        }
    }
    ::close(stderrPipe[0]);
    int st = 0;
    bool reaped = false;
    while (!timedOut && std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = waitpid(pid, &st, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            reaped = true;
            break;
        }
        usleep(10000);
    }
    if (!reaped) {
        timedOut = true;
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (waitpid(pid, &st, WNOHANG) == pid) {
                reaped = true;
                break;
            }
            usleep(10000);
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
        }
    }
    if (timedOut) {
        setSourceAspectProbeFail(failDetail, "timeout", output.size());
        return {};
    }
    if (reaped && WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        setSourceAspectProbeFail(failDetail, "exec");
        return {};
    }
    // A damaged stream can still have an authoritative DAR in its parsed
    // header. Preserve that fact so playback reaches the independent
    // zero-frame/short-read guard instead of misclassifying it as unknown DAR.
    const auto parsed = sourceAspectFromFfmpegProbeText(output);
    if (!parsed.valid)
        setSourceAspectProbeFail(failDetail, "empty_dar", output.size());
    if (codedW || codedH) {
        int pw = 0, ph = 0;
        if (codedSizeFromFfmpegProbeText(output, pw, ph)) {
            if (codedW)
                *codedW = pw;
            if (codedH)
                *codedH = ph;
        }
    }
    if (fpsNum || fpsDen) {
        std::string tok;
        int pn = 0, pd = 0;
        if (fpsTokenFromFfmpegProbeText(output, tok) &&
            parseExactFps("", tok, pn, pd) && pn > 0 && pd > 0) {
            if (fpsNum)
                *fpsNum = pn;
            if (fpsDen)
                *fpsDen = pd;
        }
    }
    return parsed;
}

class FpgaBitstreamProducer final : public h264stream::IBitstreamProducer {
public:
    explicit FpgaBitstreamProducer(FpgaSpi& fpga) : fpga_(fpga) {}

    h264stream::ControlResult begin(uint64_t session_id) override {
        if (active_)
            return h264stream::ControlResult::ActiveSession;
        if (!fpga_.ok() || !fpga_.beginBitstreamSession(session_id, 250))
            return h264stream::ControlResult::Fatal;
        session_id_ = session_id;
        producer_seq_ = 0;
        consumer_seq_ = 0;
        bytes_accepted_ = 0;
        nal_accepted_ = 0;
        desync_count_ = 0;
        last_bad_seq_ = 0;
        active_ = true;
        paused_ = false;
        return h264stream::ControlResult::Ok;
    }

    h264stream::PushResult pushNal(const h264stream::NalView& nal) override {
        if (!active_ || nal.session_id != session_id_ || !nal.annexb || nal.len == 0)
            return h264stream::PushResult::Fatal;
        if (nal.seq != producer_seq_) {
            ++desync_count_;
            last_bad_seq_ = nal.seq;
            return h264stream::PushResult::Desync;
        }
        // Contract: copy-on-push. The caller may reuse the demux accumulator as
        // soon as this function returns, even if a future transport is DMA-backed.
        std::vector<uint8_t> copy(nal.annexb, nal.annexb + nal.len);
        FpgaSpi::BitstreamNal fpgaNal;
        fpgaNal.session_id = nal.session_id;
        fpgaNal.seq = nal.seq;
        fpgaNal.nal_type = nal.nal_type;
        fpgaNal.annexb = copy.data();
        fpgaNal.len = copy.size();
        const auto r = fpga_.pushBitstreamNal(fpgaNal, 0);
        if (r == FpgaSpi::BitstreamPushResult::Full)
            return h264stream::PushResult::Full;
        if (r == FpgaSpi::BitstreamPushResult::Desync) {
            syncStatus();
            return h264stream::PushResult::Desync;
        }
        if (r != FpgaSpi::BitstreamPushResult::Ok)
            return h264stream::PushResult::Fatal;
        ++producer_seq_;
        bytes_accepted_ += copy.size();
        ++nal_accepted_;
        return h264stream::PushResult::Ok;
    }

    h264stream::ControlResult flush(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        if (!fpga_.flushBitstreamSession(session_id, 250))
            return h264stream::ControlResult::Fatal;
        consumer_seq_ = producer_seq_;
        return h264stream::ControlResult::Ok;
    }

    h264stream::ControlResult end(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        if (!fpga_.endBitstreamSession(session_id, 250))
            return h264stream::ControlResult::Fatal;
        active_ = false;
        paused_ = false;
        return h264stream::ControlResult::Ok;
    }

    h264stream::ControlResult pause(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        if (!fpga_.pauseBitstreamSession(session_id, 250))
            return h264stream::ControlResult::Fatal;
        paused_ = true;
        return h264stream::ControlResult::Ok;
    }

    h264stream::ControlResult resume(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        if (!fpga_.resumeBitstreamSession(session_id, 250))
            return h264stream::ControlResult::Fatal;
        paused_ = false;
        return h264stream::ControlResult::Ok;
    }

    h264stream::Telemetry status() const override {
        h264stream::Telemetry t;
        t.session_id = session_id_;
        t.bytes_accepted = bytes_accepted_;
        t.nal_accepted = nal_accepted_;
        t.producer_seq = producer_seq_;
        t.consumer_seq = consumer_seq_;
        t.desync_count = desync_count_;
        t.last_bad_seq = last_bad_seq_;
        t.active = active_;
        t.paused = paused_;
        FpgaSpi::BitstreamStatus s;
        if (fpga_.readBitstreamStatus(s)) {
            t.session_id = s.session_id ? s.session_id : t.session_id;
            t.ring_level_bytes = s.ring_level;
            t.ring_capacity_bytes = s.ring_capacity;
            t.consumer_seq = s.consumer_seq;
            t.underrun_count = s.underrun_count;
            t.overrun_count = s.overrun_count;
            t.desync_count = s.desync_count;
            t.last_bad_seq = s.last_bad_seq;
            t.active = s.active;
            t.paused = s.paused;
        }
        return t;
    }

private:
    void syncStatus() {
        FpgaSpi::BitstreamStatus s;
        if (!fpga_.readBitstreamStatus(s))
            return;
        consumer_seq_ = s.consumer_seq;
        desync_count_ = s.desync_count;
        last_bad_seq_ = s.last_bad_seq;
        paused_ = s.paused;
        active_ = s.active;
    }

    FpgaSpi& fpga_;
    uint64_t session_id_ = 0;
    uint32_t producer_seq_ = 0;
    uint32_t consumer_seq_ = 0;
    uint64_t bytes_accepted_ = 0;
    uint64_t nal_accepted_ = 0;
    uint64_t desync_count_ = 0;
    uint32_t last_bad_seq_ = 0;
    bool active_ = false;
    bool paused_ = false;
};

} // namespace

inline bool letterboxCrop480i(int& outLeft, int& outRight, int& outTop, int& outBottom,
                              int codedW, int codedH, int displayW, int displayH,
                              bool isLetterbox, bool isCropTo43) {
    int left = 0, right = 0, top = 0, bottom = 0;
    bool changed = false;
    if (isCropTo43) {
        int targetW = displayH * 4 / 3;
        if (targetW < displayW) {
            int extra = displayW - targetW;
            left = extra / 2;
            right = extra - left;
            changed = true;
        }
    } else if (isLetterbox) {
        int targetH = codedW * 9 / 16;
        if (targetH < codedH) {
            int extra = codedH - targetH;
            top = extra / 2;
            bottom = extra - top;
            changed = true;
        }
    }
    outLeft = left;
    outRight = right;
    outTop = top;
    outBottom = bottom;
    return changed;
}

void MediaPlayer::log(const std::string& s) const {
    // Sink: SyncedLogAssembler::push redacts via redactUrlTokens. Playback argv is not this path.
    const std::vector<std::string> lines = logAsm_.push(s);
    for (const std::string& line : lines) {
        if (log_)
            log_(line);
        else
            std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void MediaPlayer::notePicDelivery() {
    lastPicTime_ = std::chrono::steady_clock::now();
    picStallLogged_ = false;
    stalledPicWatchdogHits_ = 0;
}

bool MediaPlayer::isPicStalled() const {
    if (!fpgaVideoStarted_.load())
        return false;
    // PAUSE-WATCHDOG: count from the later of the last picture and the last resume(); none while paused.
    using Clock = std::chrono::steady_clock;
    const int64_t perMs = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(1)).count();
    return stalledPicGapExceeded(true, paused_.load(), Clock::now().time_since_epoch().count(),
                                 lastPicTime_.time_since_epoch().count(),
                                 resumeArmTicks_.load(std::memory_order_acquire), perMs, picWatchdogTimeoutMs_);
}

bool MediaPlayer::directProxyPolicy(int profile_idc, bool has_cavlc, int max_ref_frames,
                                    bool has_bframe, int level) {
    if (profile_idc != 66)
        return false;
    if (!has_cavlc)
        return false;
    if (max_ref_frames != 1)
        return false;
    if (has_bframe)
        return false;
    if (level <= 0 || level > 30)
        return false;
    return true;
}

PlaybackSummary MediaPlayer::lastPlaybackSummary() const {
    std::lock_guard<std::mutex> lock(summaryMu_);
    return lastSummary_;
}

TtfpTimestamps MediaPlayer::ttfpTimestamps() const {
    std::lock_guard<std::mutex> lock(ttfpMu_);
    return ttfp_;
}

void MediaPlayer::setContentFpsRational(int num, int den) {
    if (num <= 0 || den <= 0) {
        fpsNum_ = 0;
        fpsDen_ = 0;
        return;
    }
    // Sanity clamp: 1..240 fps. Keeps a bogus PMS value from wedging the schedule.
    const double v = static_cast<double>(num) / static_cast<double>(den);
    if (v < 1.0 || v > 240.0) {
        fpsNum_ = 0;
        fpsDen_ = 0;
        return;
    }
    if (fpsGate_ &&
        fpsGateMode(outW_, outH_, std::getenv("MPX_FPGA_MODE"),
                    std::getenv("MPX_PRESENT_480I")) != FpsGateMode::None) {
        if (snapFpsToStandard(num, den, fpsGate_)) {
            log("media: fps gate snapped " + std::to_string(num) + "/" +
                std::to_string(den) + " (w=" + std::to_string(outW_) +
                " h=" + std::to_string(outH_) + ")");
        }
    }
    fpsNum_ = num;
    fpsDen_ = den;
}

std::string MediaPlayer::hex16(uint16_t v) {
    static const char* d = "0123456789abcdef";
    std::string out(4, '0');
    for (int i = 3; i >= 0; --i) {
        out[static_cast<size_t>(i)] = d[v & 0xF];
        v >>= 4;
    }
    return out;
}

void MediaPlayer::startOsdPoll() {
    if (browseV2_) {
        startOsdPollV2();
        return;
    }
    std::lock_guard<std::mutex> lk(osdMu_);
    if (shuttingDown_.load() || !osdControl_ || osdRun_.exchange(true))
        return;
    if (osdThr_.joinable())
        osdThr_.join();
    osdThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-osd");
#endif
        bool mailboxLogged = false;
        while (osdRun_.load()) {
            uint16_t word = 0;
            bool got = false;
            bool viaMailbox = false;
            if (CoreGate::process().open()) {
                std::lock_guard<std::mutex> lk(presentMu_);
                // Preferred path: the core publishes the OSD word into HPS DDR,
                // so reading it is a plain memory load that Main never sees.
                if (fpga_.readOsdMailbox(word)) {
                    got = true;
                    viaMailbox = true;
                } else if (fpga_.ok()) {
                    // Pre-mailbox RBF: fall back to UIO_GET_STATUS over SPI.
                    // This reads the status[15:0] menu word only (v3 cores also
                    // echo bytes 12/15, decoded by parseCoreStatus per build row).
                    uint8_t raw[16]{};
                    if (fpga_.getCoreStatus(raw)) {
                        word = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
                        got = true;
                    }
                }
            }
            if (got && viaMailbox && !mailboxLogged) {
                mailboxLogged = true;
                log("media: OSD via DDR mailbox (no SPI)");
            }
            if (got) {
                const uint16_t prev = lastOsd_.load();
                const bool first = !osdSeen_.exchange(true);
                lastOsd_.store(word);
                // Log-only leftover: F12 T[10] must not open browse (PLXI cmd=5).
                if (!first && (word & (1u << 10)) && !(prev & (1u << 10)))
                    log("media: OSD T[10] leftover ignored (browse is PLXI cmd=5)");
                if (first || osdChanged(prev, word))
                    applyOsd(word);
            }
            // The mailbox is free to poll. The SPI fallback is not: it has to
            // park Main for the critical section, so keep that path slow.
            const int quietMs = viaMailbox ? 100 : (playing_.load() ? 250 : 1000);
            for (int slept = 0; slept < quietMs && osdRun_.load(); slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void MediaPlayer::startOsdPollV2() {
    std::lock_guard<std::mutex> lk(osdMu_);
    if (shuttingDown_.load() || !osdControl_ || osdRun_.exchange(true))
        return;
    if (osdThr_.joinable())
        osdThr_.join();
    osdThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-osd");
#endif
        bool mailboxLogged = false;
        while (osdRun_.load()) {
            uint16_t word = 0;
            bool got = false;
            bool viaMailbox = false;
            if (CoreGate::process().open()) {
                // The mailbox read takes no presentMu_ (readOsdMailboxDirect: only the doorbell-
                // page lock, for the read); only the SPI fallback, which parks Main, still runs
                // under presentMu_.
                if (fpga_.readOsdMailboxDirect(word)) {
                    got = true;
                    viaMailbox = true;
                } else if (fpga_.ok()) {
                    std::lock_guard<std::mutex> lk(presentMu_);
                    uint8_t raw[16]{};
                    if (fpga_.getCoreStatus(raw)) {
                        word = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
                        got = true;
                    }
                }
            }
            if (got && viaMailbox && !mailboxLogged) {
                mailboxLogged = true;
                log("media: OSD via DDR mailbox (no SPI, no lock)");
            }
            if (got) {
                const uint16_t prev = lastOsd_.load();
                const bool first = !osdSeen_.exchange(true);
                lastOsd_.store(word);
                if (!first && (word & (1u << 10)) && !(prev & (1u << 10)))
                    log("media: OSD T[10] leftover ignored (browse is PLXI cmd=5)");
                if (first || osdChanged(prev, word))
                    applyOsd(word);
            }
            const int quietMs = viaMailbox ? 100 : (playing_.load() ? 250 : 1000);
            for (int slept = 0; slept < quietMs && osdRun_.load(); slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void MediaPlayer::stopOsdPoll() {
    std::lock_guard<std::mutex> lk(osdMu_);
    osdRun_.store(false);
    if (osdThr_.joinable())
        osdThr_.join();
}

void MediaPlayer::setSkipDeltasMs(int64_t forwardMs, int64_t backMs) {
    if (forwardMs < 0)
        forwardMs = 0;
    if (backMs < 0)
        backMs = 0;
    skipForwardMs_ = forwardMs;
    skipBackMs_ = backMs;
}

void MediaPlayer::startInputPoll() {
    if (browseV2_) {
        startInputPollV2();
        return;
    }
    std::lock_guard<std::mutex> lk(inputMu_);
    if (shuttingDown_.load() || inputRun_.exchange(true))
        return;
    if (inputThr_.joinable())
        inputThr_.join();
    inputThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-input");
#endif
        bool logged = false;
        while (inputRun_.load()) {
            PlaybackCommand command = PlaybackCommand::None;
            bool got = false;
            if (CoreGate::process().open()) {
                std::lock_guard<std::mutex> lk(presentMu_);
                got = fpga_.readInputMailbox(command);
            }
            if (got) {
                if (!logged) {
                    logged = true;
                    log("media: playback input via DDR mailbox (no SPI)");
                }
                dispatchPlaybackInput(command);
            }
            for (int slept = 0; slept < 50 && inputRun_.load(); slept += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void MediaPlayer::startInputPollV2() {
    std::lock_guard<std::mutex> lk(inputMu_);
    if (shuttingDown_.load() || inputRun_.exchange(true))
        return;
    if (inputThr_.joinable())
        inputThr_.join();
    inputThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-input");
#endif
        bool logged = false;
        while (inputRun_.load()) {
            PlaybackCommand command = PlaybackCommand::None;
            bool got = false;
            // No presentMu_: a paint or an SPI window holding it cannot delay a press.
            if (CoreGate::process().open())
                got = fpga_.readInputMailboxDirect(command, browseInputMax_);
            if (got) {
                if (!logged) {
                    logged = true;
                    log("media: playback input via DDR mailbox (no SPI, no lock; commands 1-" +
                        std::to_string(browseInputMax_) + ")");
                }
                dispatchPlaybackInput(command);
            }
            for (int slept = 0; slept < 50 && inputRun_.load(); slept += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void MediaPlayer::stopInputPoll() {
    std::lock_guard<std::mutex> lk(inputMu_);
    inputRun_.store(false);
    if (inputThr_.joinable())
        inputThr_.join();
}

void MediaPlayer::suspendFpgaWorkers() {
    stopInputPoll();
    stopOsdPoll();
    stopIdle();
}

void MediaPlayer::resumeFpgaWorkers(bool restoreIdle) {
    if (restoreIdle) {
        paintIdle();
        startIdle();
    }
    startInputPoll();
    startOsdPoll();
}

void MediaPlayer::openLibraryBrowser() {
    if (browseV2_) {
        if (plexUi_)
            plexUi_->post(BrowseOp::Open);
        else
            browse_->post(BrowseOp::Open);
        return;
    }
    if (playing_.load() || shuttingDown_.load()) {
        log("media: library browse aborted (playing/shutdown)");
        return;
    }
    if (!libraryFetch_) {
        log("media: library browse needs PLEX_TOKEN / fetch");
        {
            std::lock_guard<std::mutex> lk(libraryMu_);
            library_.show({}, "no plex token");
        }
        if (!playing_.load() && !shuttingDown_.load())
            paintIdle();
        return;
    }
    log("media: library browse open");
    const std::string xml = libraryFetch_("/library/sections");
    auto rows = parseLibraryXml(xml);
    if (playing_.load() || shuttingDown_.load()) {
        log("media: library browse aborted after fetch (play won)");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        library_.show(std::move(rows), xml.empty() ? "fetch failed" : "");
    }
    if (playing_.load() || shuttingDown_.load()) {
        std::lock_guard<std::mutex> lk(libraryMu_);
        library_.hide();
        log("media: library browse aborted after show (play won)");
        return;
    }
    paintIdle();
}

void MediaPlayer::openLibraryList() {
    if (!libraryFetch_) {
        log("media: library list needs PLEX_TOKEN / fetch");
        {
            std::lock_guard<std::mutex> lk(libraryMu_);
            libraryList_ = librarylist::LibraryList{};
            libraryList_.status = "NO PLEX SERVER";
            libraryListVisible_ = true;
        }
        if (!playing_.load() && !shuttingDown_.load())
            paintIdle();
        return;
    }
    log("media: library list fetch /library/sections");
    const librarylist::HomeLibraryFetch fetched = librarylist::fetchHomeLibrary(
        [this](const std::string& path) {
            log("media: library list GET " + path);
            return libraryFetch_(path);
        });
    if (playing_.load() || shuttingDown_.load()) {
        log("media: library list aborted after fetch (play won)");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        libraryList_ = fetched.list;
        libraryListVisible_ = true;
    }
    log("media: library list " + std::to_string(fetched.list.rows.size()) + " rows" +
        (fetched.list.status.empty() ? "" : " status=" + fetched.list.status));
    if (!playing_.load() && !shuttingDown_.load())
        paintIdle();
}

void MediaPlayer::requestOpenLibraryList() {
    log("media: library list request");
    if (shuttingDown_.load())
        return;
    if (playing_.load()) {
        if (browseOpenBusy_.exchange(true)) {
            log("media: library list already in flight");
            return;
        }
        std::lock_guard<std::mutex> lk(browseThrMu_);
        if (browseThr_.joinable())
            browseThr_.join();
        browseThr_ = std::thread([this] {
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "mpx-liblist");
#endif
            stop();
            if (!shuttingDown_.load() && !playing_.load())
                openLibraryList();
            browseOpenBusy_.store(false);
        });
        return;
    }
    if (browseOpenBusy_.exchange(true)) {
        log("media: library list already in flight");
        return;
    }
    std::lock_guard<std::mutex> lk(browseThrMu_);
    if (browseThr_.joinable())
        browseThr_.join();
    browseThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-liblist");
#endif
        openLibraryList();
        browseOpenBusy_.store(false);
    });
}

bool MediaPlayer::libraryHandleInputList(PlaybackCommand command) {
    // Mailbox press (dispatchPlaybackInput already logged "input cmd="). The
    // state machine decides; play runs on the same hook as companion playMedia.
    if (command == PlaybackCommand::Browse) {
        requestOpenLibraryList();
        return true;
    }
    librarylist::BrowseListStep step;
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        // A live session keeps Skip/PlayPause/Stop. Browse above still stops
        // and opens the list. consumeBrowseListInput refuses before step.
        if (!librarylist::consumeBrowseListInput(libraryList_, libraryListVisible_,
                                                 playing_.load(), command, step))
            return false;
    }
    switch (step.effect) {
    case librarylist::BrowseListEffect::Ignore:
        return false;
    case librarylist::BrowseListEffect::Open:
        requestOpenLibraryList();
        return true;
    case librarylist::BrowseListEffect::Play:
        log("media: library list play ratingKey=" + step.playKey + " key=" + step.playMediaKey);
        if (libraryPlay_)
            libraryPlay_(step.playKey);
        else {
            log("media: library list play has no hook; idle chevron");
            paintIdle();
        }
        return true;
    case librarylist::BrowseListEffect::IdleChevron:
        log("media: library list B -> idle chevron");
        paintIdle();
        return true;
    case librarylist::BrowseListEffect::Move:
    case librarylist::BrowseListEffect::Note:
        log(std::string("media: library list input cmd=") +
            std::to_string(static_cast<int>(command)));
        paintIdle();
        return true;
    case librarylist::BrowseListEffect::Hold:
        return true;
    }
    return false;
}

void MediaPlayer::requestOpenLibraryBrowser() {
    log("media: library browse request (always-open)");
    if (shuttingDown_.load())
        return;
    if (playing_.load()) {
        if (browseOpenBusy_.exchange(true)) {
            log("media: library browse already in flight");
            return;
        }
        // stop() joins play/OSD/idle — never run on those threads.
        std::lock_guard<std::mutex> lk(browseThrMu_);
        if (browseThr_.joinable())
            browseThr_.join();
        browseThr_ = std::thread([this] {
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "mpx-browse");
#endif
            stop();
            if (!shuttingDown_.load() && !playing_.load())
                openLibraryBrowser();
            browseOpenBusy_.store(false);
        });
        return;
    }
    openLibraryBrowser();
}

void MediaPlayer::libraryActivate() {
    LibraryRow row;
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        const LibraryRow* cur = library_.current();
        if (!cur)
            return;
        row = *cur;
    }
    if (row.directory) {
        if (!libraryFetch_)
            return;
        const std::string path = libraryListPath(row);
        log("media: library enter " + (path.empty() ? row.key : path));
        const std::string xml = libraryFetch_(path.empty() ? row.key : path);
        if (playing_.load() || shuttingDown_.load()) {
            log("media: library enter aborted (play won)");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(libraryMu_);
            library_.push(parseLibraryXml(xml));
        }
        paintIdle();
        return;
    }
    std::string playId = row.ratingKey;
    if (playId.empty() && !row.key.empty() &&
        librarylist::browseListPlayMediaKey(row.key) == row.key)
        playId = row.key;
    if (playId.empty() || !libraryPlay_)
        return;
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        library_.hide();
    }
    log("media: library play ratingKey=" + playId);
    libraryPlay_(playId);
}

bool MediaPlayer::libraryHandleInput(PlaybackCommand command) {
    if (libraryListOn_)
        return libraryHandleInputList(command);
    if (browseV2_)
        return libraryHandleInputV2(command);
    if (command == PlaybackCommand::Browse) {
        requestOpenLibraryBrowser();
        return true;
    }
    bool vis = false;
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        vis = library_.visible;
    }
    if (!vis) {
        if (!playing_.load() && command == PlaybackCommand::Stop) {
            openLibraryBrowser();
            return true;
        }
        return false;
    }
    log(std::string("media: library input cmd=") + std::to_string(static_cast<int>(command)));
    switch (command) {
    case PlaybackCommand::PlayPause:
        libraryActivate();
        return true;
    case PlaybackCommand::Stop: {
        std::lock_guard<std::mutex> lk(libraryMu_);
        if (!library_.pop())
            library_.hide();
        break;
    }
    case PlaybackCommand::SkipForward: {
        std::lock_guard<std::mutex> lk(libraryMu_);
        library_.move(1);
        break;
    }
    case PlaybackCommand::SkipBack: {
        std::lock_guard<std::mutex> lk(libraryMu_);
        library_.move(-1);
        break;
    }
    default:
        return false;
    }
    paintIdle();
    return true;
}

namespace {

void maybeStartPlexUiNavSelftest(misterplex::plexui::PlexUiController* ui) {
    const char* raw = std::getenv("MPX_PLEXUI_SELFTEST_NAV");
    if (!raw || !*raw || !ui)
        return;
    const int n = std::atoi(raw);
    if (n <= 0)
        return;
    std::thread([ui, n] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        for (int wait = 0; wait < 120; ++wait) {
            if (ui->frameSummary().view == "grid")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        for (int i = 0; i < n; ++i) {
            ui->postIngress(BrowseOp::Right, "selftest", misterplex::plexui::perfSteadyMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        for (int i = 0; i < n; ++i) {
            ui->postIngress(BrowseOp::Down, "selftest", misterplex::plexui::perfSteadyMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

} // namespace

void MediaPlayer::enablePlexUi(plexui::PlexUiController::Config cfg,
                               std::function<uint64_t(const BrowsePlayTicket&)> play) {
    if (plexUi_ || browse_)
        return;
    plexui::PlexUiController::Hooks h;
    h.repaint = [this] { paintIdle(); };
    h.playing = [this] { return playing_.load(); };
    h.stopPlayback = [this] { (void)stop(); };
    h.play = std::move(play);
    h.log = [this](const std::string& line) { log(redactSensitive(line)); };
    plexUi_ = std::make_unique<plexui::PlexUiController>(std::move(cfg), std::move(h));
    browseV2_ = true;
    plexUi_->start([] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-plexui");
#endif
    });
    maybeStartPlexUiNavSelftest(plexUi_.get());
}

void MediaPlayer::enableBrowseV2(BrowseController::Config cfg,
                                 std::function<uint64_t(const BrowsePlayTicket&)> play,
                                 bool navCommands) {
    if (browse_ || plexUi_)
        return;
    browseInputMax_ = navCommands ? kBrowseMaxInputCommand : kLegacyMaxInputCommand;
    BrowseController::Hooks h;
    h.repaint = [this] { paintIdle(); };
    h.playing = [this] { return playing_.load(); };
    h.stopPlayback = [this] { (void)stop(); }; // on mpx-browse, never on input/OSD/idle
    h.play = std::move(play);
    h.log = [this](const std::string& line) { log(line); };
    browse_ = std::make_unique<BrowseController>(std::move(cfg), std::move(h));
    browseV2_ = true;
    browse_->start([] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-browse");
#endif
    });
}

// MPX_BROWSE_V2: the input thread only posts; the mpx-browse worker fetches, paints, plays
// and stops. Browse opens (stopping a play first); a visible browser takes every browse
// command; idle with it hidden, Stop opens it; Stop during a play this browser started stops
// it and returns to that item (build kit A8). Everything else is playback, as before.
void MediaPlayer::plexUiCompanionNav(const std::string& op) {
    if (!plexUi_)
        return;
    const int64_t recvMs = misterplex::plexui::perfSteadyMs();
    if (op == "up")
        plexUi_->postIngress(BrowseOp::Up, "http", recvMs);
    else if (op == "down")
        plexUi_->postIngress(BrowseOp::Down, "http", recvMs);
    else if (op == "left")
        plexUi_->postHorizontal(-1, "http", recvMs);
    else if (op == "right")
        plexUi_->postHorizontal(1, "http", recvMs);
    else if (op == "select")
        plexUi_->postIngress(BrowseOp::Select, "http", recvMs);
    else if (op == "back")
        plexUi_->postIngress(BrowseOp::Back, "http", recvMs);
    else if (op == "home")
        plexUi_->postIngress(BrowseOp::Open, "http", recvMs);
}

bool MediaPlayer::libraryHandleInputV2(PlaybackCommand command) {
    if (plexUi_) {
        const int64_t recvMs = misterplex::plexui::perfSteadyMs();
        if (command == PlaybackCommand::Browse) {
            plexUi_->postIngress(BrowseOp::Open, "plxi", recvMs);
            return true;
        }
        if (plexUi_->visible()) {
            if (command == PlaybackCommand::SkipForward) {
                plexUi_->postHorizontal(1, "plxi", recvMs);
                return true;
            }
            if (command == PlaybackCommand::SkipBack) {
                plexUi_->postHorizontal(-1, "plxi", recvMs);
                return true;
            }
            BrowseOp op = BrowseOp::Open;
            if (!browseOpForVisibleCommand(command, op))
                return false;
            plexUi_->postIngress(op, "plxi", recvMs);
            return true;
        }
        if (!playing_.load()) {
            if (command != PlaybackCommand::Stop)
                return false;
            plexUi_->post(BrowseOp::Open);
            return true;
        }
        if (command == PlaybackCommand::Stop && plexUi_->ownsSession()) {
            plexUi_->post(BrowseOp::StopAndReopen);
            return true;
        }
        return false;
    }
    if (command == PlaybackCommand::Browse) {
        browse_->post(BrowseOp::Open);
        return true;
    }
    if (browse_->visible()) {
        BrowseOp op = BrowseOp::Open;
        if (!browseOpForVisibleCommand(command, op))
            return false;
        browse_->post(op);
        return true;
    }
    if (!playing_.load()) {
        if (command != PlaybackCommand::Stop)
            return false;
        browse_->post(BrowseOp::Open);
        return true;
    }
    if (command == PlaybackCommand::Stop && browse_->ownsSession()) {
        browse_->post(BrowseOp::StopAndReopen);
        return true;
    }
    return false;
}

void MediaPlayer::dispatchPlaybackInput(PlaybackCommand command) {
    log(std::string("media: input cmd=") + std::to_string(static_cast<int>(command)));
    if (libraryHandleInput(command))
        return;
    if (command == PlaybackCommand::Stop)
        userStop_ = true;
    const PlaybackTransportState state{playing_.load(), paused_.load(), positionMs_.load(),
                                       durationMs()};
    (void)dispatchPlaybackCommand(command, state, skipForwardMs_, skipBackMs_, steadyMs(),
                                  ignoreInputUntilMs_.load(), *this);
}

void MediaPlayer::ycOwenbTick() {
    CoreGate& gate = CoreGate::process();
    OwenbCtrlReader reader;
    if (!gate.owenbOpen()) {
        if (ycMap_.mapped())
            ycMap_.close();
    } else {
        if (!ycMap_.mapped())
            (void)ycMap_.open(MPX_DEVMEM_PATH);
        if (ycMap_.mapped())
            reader = ycMap_.reader();
    }
    const plexob::YcPollFiles files{plexob::kMisterIniPath, plexob::kYcReloadMarkerPath,
                                    plexob::plexObCfgPath()};
    plexob::YcPollResult result;
    {
        std::lock_guard<std::mutex> lk(ycMu_);
        result = plexob::pollYcOnOwenbGate(gate, reader, ycState_, files, playing_.load(),
                                           coreGateNowUs());
    }
    if (result.gaveUp)
        log("media: YC reload gave up after the per-boot attempt cap");
    if (result.removedKeys)
        log("media: YC RGB removed [PlexOB] vga_mode/ntsc_mode/vga_scaler");
    if (result.wantReload)
        enqueueYcReload(result);
}

void MediaPlayer::enqueueYcReload(const plexob::YcPollResult& job) {
    {
        std::lock_guard<std::mutex> lk(ycJobMu_);
        ycJob_ = job;
        ycJobPending_ = true;
        if (!ycThr_.joinable()) {
            ycRun_.store(true);
            ycThr_ = std::thread([this] { ycReloadWorker(); });
        }
    }
    ycJobCv_.notify_one();
}

void MediaPlayer::ycReloadWorker() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "mpx-yc");
#endif
    while (ycRun_.load()) {
        plexob::YcPollResult job;
        {
            std::unique_lock<std::mutex> lk(ycJobMu_);
            ycJobCv_.wait(lk, [this] { return ycJobPending_ || !ycRun_.load(); });
            if (!ycRun_.load())
                return;
            if (!ycJobPending_)
                continue;
            job = ycJob_;
            ycJobPending_ = false;
        }
        const char* vgaMode = plexob::ycVgaModeFromAnalogOutput(job.analog);
        const char* ntscMode = plexob::ycNtscModeFromColor(job.ntsc);
        const bool ok = reloadCoreForYcMode(vgaMode, ntscMode);
        bool marked = false;
        if (ok)
            marked = plexob::writeYcReloadMarker(plexob::kYcReloadMarkerPath, job.buildId, job.value);
        if (!ok || !marked)
            log(std::string("media: YC reload FAILED; marker not written vga_mode=") + vgaMode);
        std::lock_guard<std::mutex> lk(ycMu_);
        plexob::ycNoteReloadFinished(ycState_, ok && marked, coreGateNowUs());
    }
}

void MediaPlayer::stopYcReloadWorker() {
    ycRun_.store(false);
    ycJobCv_.notify_all();
    if (ycThr_.joinable())
        ycThr_.join();
}

bool MediaPlayer::recoverStrandedMenuCore(bool atStart) {
    const std::string name = readCoreNameFile();
    const bool marker = menuBounceMarkerPresent(kMenuBounceMarkerPath);
    // Startup runs this before anything else. A leftover marker on Plex or
    // PlexOB is not the gap between menu.rbf and the PlexOB load. Clear it
    // so a later user visit to Menu is not treated as stranded.
    if (staleMenuBounceMarker(name.c_str(), marker)) {
        clearMenuBounceMarker(kMenuBounceMarkerPath);
        return false;
    }
    // Process start on Menu loads PlexOB even with no bounce marker. A
    // mid-session exit still requires the marker: a person who opened the
    // menu has none, so menuBounceRecoveryDue stays false.
    const bool due = atStart ? bareDaemonMenuReload(name.c_str())
                             : menuBounceRecoveryDue(name.c_str(), marker);
    if (!due)
        return false;
    const std::string path = plexObReloadResolvePath("PlexOB");
    if (path.empty() || !plexObReloadWriteCore(false, true))
        return false;
    const std::string coreLine = std::string("load_core ") + path;
    if (!loadCoreLineAllowed(coreLine.c_str()))
        return false;
    const int rc = writeMisterCmdLine(kMisterCmdPath, coreLine.c_str());
    log("media: PlexOB recovery " + coreLine + " (core was " + name + ") rc=" + std::to_string(rc));
    std::fprintf(stderr, "misterplexd: PlexOB recovery %s (core was %s) rc=%d\n", coreLine.c_str(),
                 name.c_str(), rc);
    if (rc > 0)
        clearMenuBounceMarker(kMenuBounceMarkerPath);
    return rc > 0;
}

bool MediaPlayer::reloadCoreForYcMode(const char* vgaMode, const char* ntscMode) {
    if (playing_.load() || shuttingDown_.load())
        return false;
    const std::string loaded = readCoreNameFile(kRbfNamePath);
    const std::string path = plexObReloadResolvePath(loaded);
    if (path.empty()) {
        log("media: YC reload refused (RBF missing) name='" + loaded + "'");
        std::fprintf(stderr, "misterplexd: YC reload refused, RBF missing for '%s'\n", loaded.c_str());
        return false;
    }
    const std::string coreLine = std::string("load_core ") + path;
    if (!loadCoreLineAllowed(coreLine.c_str())) {
        log("media: YC reload refused (load_core not allow-listed): " + coreLine);
        return false;
    }
    log("media: YC reload resolved path=" + path);
    // Play can start after the entry check. Re-check immediately before each
    // MiSTer_cmd write. lifeMu_ is not held across the bounce: stop() takes it
    // and then joins this worker.
    if (plexObReloadWriteMenu(shuttingDown_.load(), liveCoreNameIsMenu())) {
        if (!writeMenuBounceMarker(kMenuBounceMarkerPath))
            return false;
        if (playing_.load()) {
            clearMenuBounceMarker(kMenuBounceMarkerPath);
            log("media: YC reload aborted, play started before menu.rbf");
            return false;
        }
        if (writeMisterCmdLine(kMisterCmdPath, "load_core /media/fat/menu.rbf") <= 0) {
            clearMenuBounceMarker(kMenuBounceMarkerPath);
            return false;
        }
        const auto menuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (std::chrono::steady_clock::now() < menuDeadline && !shuttingDown_.load()) {
            if (liveCoreNameIsMenu())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!liveCoreNameIsMenu()) {
        clearMenuBounceMarker(kMenuBounceMarkerPath);
        return false;
    }
    if (playing_.load()) {
        clearMenuBounceMarker(kMenuBounceMarkerPath);
        log("media: YC reload aborted, play started before load_core");
        return false;
    }
    // INT between the two load_core lines must still emit PlexOB. The marker
    // stays until that load_core is issued, so a restart still on Menu recovers.
    if (shuttingDown_.load() && plexObReloadWriteCore(true, true)) {
        const int rc = writeMisterCmdLine(kMisterCmdPath, coreLine.c_str());
        log("media: YC reload INT mid-load issued " + coreLine + " rc=" + std::to_string(rc));
        std::fprintf(stderr, "misterplexd: YC reload INT mid-load %s rc=%d\n", coreLine.c_str(), rc);
        if (rc > 0)
            clearMenuBounceMarker(kMenuBounceMarkerPath);
        return false;
    }
    int coreRc = 0;
    const auto utilityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < utilityDeadline && coreRc <= 0 && !shuttingDown_.load()) {
        coreRc = writeMisterCmdLineWithTimeout(kMisterCmdPath, coreLine.c_str(), 5);
        if (coreRc > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (coreRc <= 0) {
        log(std::string("media: YC reload vga_mode=") + (vgaMode ? vgaMode : "?") +
            " ntsc_mode=" + (ntscMode ? ntscMode : "?") + " " + coreLine + " reload=0 path=" + path);
        std::fprintf(stderr, "misterplexd: YC reload %s reload=0\n", coreLine.c_str());
        return false;
    }
    clearMenuBounceMarker(kMenuBounceMarkerPath);
    // Word 88 must come back as MPXB / 0x0B59C11A. A prod RBF with a different
    // stamp is refused: the marker stays unwritten so the next idle tick can retry.
    uint64_t word88 = 0;
    std::string stampWhy = "word 88 unreadable";
    const auto stampDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool stamped = false;
    while (std::chrono::steady_clock::now() < stampDeadline && !shuttingDown_.load()) {
        if (coreNameIsPlexOwenb(readCoreNameFile())) {
            OwenbCtrlPageMap map;
            if (map.open(MPX_DEVMEM_PATH) == 0) {
                const OwenbCtrlReader rd = map.reader();
                if (rd.read) {
                    word88 = rd.read(owenb_id::kMpxBuildIdWord);
                    if (plexob::ycReloadStampOk(word88)) {
                        stamped = true;
                        break;
                    }
                    const uint32_t tag = static_cast<uint32_t>(word88 >> 32);
                    const uint32_t id = static_cast<uint32_t>(word88);
                    if (tag == owenb_id::kMpxBuildIdTag && id != 0) {
                        stampWhy = "word 88 BUILD_ID is not 0x0B59C11A";
                        break;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (shuttingDown_.load())
        stampWhy = "shutdown";
    char idHex[16];
    char tagHex[16];
    std::snprintf(idHex, sizeof idHex, "%08X", static_cast<unsigned>(static_cast<uint32_t>(word88)));
    std::snprintf(tagHex, sizeof tagHex, "%08X",
                  static_cast<unsigned>(static_cast<uint32_t>(word88 >> 32)));
    log(std::string("media: YC reload vga_mode=") + (vgaMode ? vgaMode : "?") +
        " ntsc_mode=" + (ntscMode ? ntscMode : "?") + " path=" + path + " " + coreLine +
        " word88_tag=" + tagHex + " word88_id=" + idHex + " reload=" + (stamped ? "1" : "0") +
        (stamped ? "" : " (" + stampWhy + ")"));
    std::fprintf(stderr, "misterplexd: YC reload %s path=%s word88=%s/%s reload=%d\n", coreLine.c_str(),
                 path.c_str(), tagHex, idHex, stamped ? 1 : 0);
    return stamped;
}

void MediaPlayer::applyOsd(uint16_t word) {
    ignoreInputUntilMs_.store(steadyMs() + 300);
    const OsdSettings s = decodeOsdWord(word);
    setAvOffsetMs(s.avOffsetMs);
    // Takes effect on the next session: the feed rate is captured when audioPump
    // opens MrAudio, and re-timing it mid-stream would step the audio clock.
    setAudioClockTrimEnabled(s.audioClockTrimEnabled);
    setResyncDropMs(s.resyncEnabled ? kDefaultResyncDropMs : 0);
    const IdleMode im = idleModeFromBits(static_cast<unsigned>(s.idleMode));
    const bool idleChanged = im != idleMode();
    setIdleMode(im);
    if (idleChanged)
        idleLogged_.store(false);

    // Content (O[5:4]) owns DECODE/PMS. Display (O[15:14]) owns video_mode.
    // First sample seeds last* (default 240p must not look like 240→720) but
    // still applies the live Display row to /dev/MiSTer_cmd (daemon start).
    const ContentResolution& cr = s.contentResolution;
    const ContentResolution& dr = s.displayResolution;
    int nextW = outW_;
    int nextH = outH_;
    const bool retargeted =
        osdRetargetDecodeSizeFromPresented(nextW, nextH, cr, liveGlass_, rbfPrefix8_);
    if (retargeted)
        setDecodeSize(nextW, nextH);
    if ((idleChanged || retargeted) && !playing_.load())
        paintIdle();
    // Display O[15:14]: persist only. video_mode is latched at daemon start /
    // core reset — do not poke /dev/MiSTer_cmd while the user flips F12.
    const bool contentChanged =
        osdResSeeded_ &&
        (lastContentRes_.width != cr.width || lastContentRes_.height != cr.height);
    const bool displayChanged =
        osdResSeeded_ &&
        (lastDisplayRes_.width != dr.width || lastDisplayRes_.height != dr.height);
    lastContentRes_ = cr;
    lastDisplayRes_ = dr;
    osdResSeeded_ = true;
    if (contentChanged && onContentRes_) {
        const bool nowPlaying = playing_.load();
        log(std::string("media: res change content→") + cr.label + " display→" + dr.label +
            " " + std::to_string(dr.width) + "x" + std::to_string(dr.height) +
            (nowPlaying ? " (live restart)" : " (next play)"));
        onContentRes_(cr, nowPlaying);
    }
    if (displayChanged && onDisplayRes_)
        onDisplayRes_(dr);

    log("media: OSD word=0x" + hex16(word) + " av_offset_ms=" + std::to_string(s.avOffsetMs) +
        " clock_ppm=" + std::to_string(audioClockPpm_) +
        " resync=" + (s.resyncEnabled ? "on" : "off") +
        " content_res=" + cr.label + " display_res=" + dr.label +
        " decode=" + std::to_string(outW_) + "x" + std::to_string(outH_));
}

void MediaPlayer::latchAndApplyDisplayRaster(const ContentResolution& display) {
    latchedDisplayRes_ = display;
    displayRasterLatched_ = true;
    lastDisplayRes_ = display;
    // Seed lastVideoModeCmd_ so IfChanged does not rewrite a live CEA 720p60
    // raster on every daemon start. A same-modeline video_mode poke resets
    // ddr_frame_store (primed=0, have_seq=0) and freezes glass on the chevron.
    if (const char* cmd = videoModeCmdForDisplayLabel(display.label))
        lastVideoModeCmd_ = cmd;
    applyDisplayRaster(display, /*force=*/false);
    if (!playing_.load())
        paintIdle();
}

void MediaPlayer::applyLiveDisplayRaster(const ContentResolution& osdDisplay, bool force) {
    // Re-arm the *latched* row only (Main vid_changed). Ignore a live OSD
    // Display that has not been reset into the latch.
    if (displayRasterLatched_)
        applyDisplayRaster(latchedDisplayRes_, force);
    else
        applyDisplayRaster(osdDisplay, force);
}

void MediaPlayer::applyDisplayRaster(const ContentResolution& display, bool force) {
    const char* cmd = videoModeCmdForDisplayLabel(display.label);
    // 112bb + CEA 720p24 HDMI (30000 kHz) wrote and HURT: 14.5/12.31 vs
    // 15.4/12.87 at 720p60. ascal 60→24 is not the unique24 closer. L4 stays
    // on 720p60. kVideoModeCmd720p24 remains a legal cmd for a later design.
    const char* next = force ? (videoModeCmdIsSafe(cmd) ? cmd : nullptr)
                             : videoModeCmdIfChanged(cmd, lastVideoModeCmd_.c_str());
    if (!next) {
        log(std::string("media: display raster ") + (display.label ? display.label : "?") +
            " unchanged (skip video_mode force=" + (force ? "1" : "0") + ")");
        return;
    }
    // Core gate: no video_mode switch while the loaded core is not a verified Plex build.
    if (!CoreGate::process().open()) {
        log(std::string("media: display raster ") + (display.label ? display.label : "?") +
            " not written: core gate closed");
        return;
    }
    const int rc = writeMisterCmdLine(kMisterCmdPath, next);
    if (rc > 0) {
        lastVideoModeCmd_ = next;
        log(std::string("media: display raster ") + (display.label ? display.label : "?") +
            " → " + next);
        return;
    }
    // Fail closed: missing cmd node is not a crash and is not HDMI PASS.
    log(std::string("media: /dev/MiSTer_cmd missing or unwritable; raster unchanged (") + next +
        ")");
}

void MediaPlayer::coreGateTick() {
    // The sequence is coreGateTickStep (core_gate.hpp, unit-tested); these are the daemon's
    // actions, each under the locks its FPGA path needs.
    CoreGateTickHooks h;
    h.observe = [this] {
        CoreObservation o;
        {
            std::lock_guard<std::mutex> present(presentMu_);
            if (!fpga_.ok())
                (void)fpga_.open(); // register map only; no FPGA traffic
            o.fabric = fpga_.fabricState();
        }
        o.main = FpgaSpi::mainIdentity();
        o.mainStartWallNs = mainStartWallNs(o.main);
        o.bootWallNs = bootWallNs();
        o.coreName = stampCoreFile(kCoreNamePath);
        o.rbfName = stampCoreFile(kRbfNamePath);
        o.nowUs = coreGateNowUs();
        return o;
    };
    h.playing = [this] { return playing_.load(); };
    h.probe = [this] {
        // Who is loaded: one CAPS probe, AU ring only (the gate stays closed).
        std::lock_guard<std::mutex> control(pauseControlMu_);
        std::lock_guard<std::mutex> present(presentMu_);
        return fpga_.runCoreGateProbe(500);
    };
    h.coreReturned = [this] {
        // SPS-gate L3 (r2): a verified core (a core change, or a CAPS re-read after a reload) clears the gate error,
        // so a stale "core 0x..." line cannot stay up. h.repaint follows in this tick sequence.
        clearGateError("core change / CAPS re-read", false);
        if (CoreGate::process().takeRePair()) {
            // The idle FB arm captured the previous core's beat and map.
            idleFbArmTried_ = false;
            const uint32_t id = CoreGate::process().buildId();
            const FpgaBuildInfo* row = recognisedFpgaBuild(id);
            if (row && row->raster && row->raster[0]) {
                ContentResolution dr = resolutionFromLabel(row->raster);
                if (dr.label && std::strcmp(dr.label, "240p") == 0)
                    dr = resolutionFromLabel("240p_lcd");
                if (dr.label) {
                    latchedDisplayRes_ = dr;
                    displayRasterLatched_ = true;
                    lastDisplayRes_ = dr;
                    applyDisplayRaster(dr, false);
                }
                char hex[16];
                std::snprintf(hex, sizeof hex, "0x%08X", id);
                const std::string line = std::string("media: core load adopted ") + hex +
                                         " raster " + row->raster +
                                         " status " + fpgaStatusLayoutName(*row) +
                                         " overlay " + (overlayPlaneAbsent(id) ? "off" : "on");
                log(line);
                std::fprintf(stderr, "misterplexd: %s\n", line.c_str());
            }
            if (row && row->idleBeatPlxl && !shuttingDown_.load() && !f1Beat_.running()) {
                f1Beat_.start();
                log("f1: PLXL beat started for plex_fb_en_hold");
                std::fprintf(stderr, "misterplexd: f1 PLXL beat started for plex_fb_en_hold\n");
            }
        }
        std::lock_guard<std::mutex> lk(presentMu_);
        fpga_.onCoreReturned();
    };
    h.finishReturn = [this] {
        // An epoch that died with the swapped core: drop it, nothing can ACK its end.
        std::lock_guard<std::mutex> control(pauseControlMu_);
        std::lock_guard<std::mutex> present(presentMu_);
        const uint64_t retained = fpgaSession_.load();
        if (coreGateEpochIsStale(retained, fpgaSessionCoreChanges_)) {
            log("media: core gate: epoch " + std::to_string(retained) +
                " retained from before the core swap; ownership dropped");
            fpga_.dropStaleSession();
            fpgaSession_.store(0);
            if (fpgaAudioOutput_ >= 0) {
                ::close(fpgaAudioOutput_);
                fpgaAudioOutput_ = -1;
            }
        }
        // MAST-WEDGE (review 21:14): once per open, before anything can play.
        probeMastAtGateOpen();
    };
    h.repaint = [this] { paintIdle(); };
    h.log = [this](const std::string& line) { log(line); };
    // OWENB-P1B T3: a PlexOB core is identified by its ring (magic, version, geometry, an advancing heartbeat,
    // the MPX BUILD_ID word), not by CAPS. The ctrl page is read-only; no FPGA write, no idle FB.
    h.ringProbe = [] {
        return owenbGateRingProbe(CoreGate::process(), MPX_DEVMEM_PATH, 100, coreGateNowUs(),
                                  owenbAllowUnstampedFromEnv());
    };
    h.owenbOpened = [this] {
        log("media: core gate: PlexOB core verified; OwenbRing backend, no daemon DDR paint");
    };
    (void)coreGateTickStep(CoreGate::process(), h, coreReturnPending_);
    // H-c (and any trigger latched while the gate was closed). No-op when
    // nothing is pending. The row comes from this open's CAPS build.
    fpga_.serviceWedgeRead("H-c", true);
    checkMastSilentGate();
    owenbIdleTick();   // OWENB-P1B T7: the idle session follows the gate (a no-op off OB)
    ycOwenbTick();     // Y/C word 87: owenbOpen + OwenbCtrlReader, not the legacy OSD poll
}

void MediaPlayer::checkMastSilentGate() {
    if (mastSilentDone_)
        return;
    CoreGate& gate = CoreGate::process();
    if (!gate.managed() || gate.open()) {
        mastSilentDone_ = true; // an open gate: the gate-open probe owns the marker
        return;
    }
    const int64_t sinceS = (coreGateNowUs() - mastStartUs_) / 1000000;
    if (sinceS < mast::kGateDeadlineS)
        return; // no marker read before the bound
    const mast::Marker marker = mast::readMarker();
    if (!marker.present || marker.state != "attempted") {
        mastSilentDone_ = true; // nothing of a reload to judge
        return;
    }
    if (!mast::silentGateCureFailed(marker, gate.open(), gate.probeSequenceActive(),
                                    coreNameIsPlex(gate.name()), sinceS))
        return; // still probing, or the core is not Plex: wait
    mastSilentDone_ = true;
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const std::string gateWhy = gate.closedWhy();
    const bool ok = mast::writeMarkerWhy("cure-failed", now, marker.reason, "gate-silent:" + gateWhy,
                                        mast::kMarkerPath, marker.r6);
    const std::string line = mast::silentGateLine(marker, now, sinceS, gateWhy);
    log("media: !!! " + line + (ok ? std::string() : std::string(" [marker write FAILED]")));
    // The gate is closed here: SpiExclusive would refuse the pair. Latch;
    // the next open tick services it after coreGateTickStep returns.
    fpga_.noteWedgeTrigger("H-c");
    mast::appendBootLoud(line);
}

void MediaPlayer::probeMastAtGateOpen() {
#ifdef MUT_MAST_NO_GATE_PROBE   // test-only mutant: no gate-open probe
    return;
#endif
    mast::Probe probe;
    const bool live = fpga_.probeAudioLane(probe);
    const char* force = std::getenv("MPX_MAST_FORCE_WEDGE");
    const bool forced = probe.lane && mast::forceWedge(force, mast::readMarker().present);
    log("media: " + mast::probeLine(probe, "gate open") +
        (forced ? std::string(" [FORCED WEDGE: lab MPX_MAST_FORCE_WEDGE=") + force + "]"
                : std::string()));
    if (!live || forced) {
        onMastWedge(forced ? "gate-open probe, forced" : "gate-open probe");
        return;
    }
    const mast::Marker marker = mast::readMarker();
    if (probe.lane && marker.present && marker.state != "recovered") {
        // review 23:53 R2: recovered only from attempted (a reload that ran).
        const int64_t now = static_cast<int64_t>(::time(nullptr));
        const mast::LiveAction act = mast::onLive(marker);
        const bool ok = act != mast::LiveAction::MarkRecovered ||
                        mast::writeMarkerWhy("recovered", now, marker.reason, std::string(),
                                              mast::kMarkerPath, marker.r6);
        log("media: " + mast::liveLine(act, marker, now) +
            (ok ? std::string() : std::string(" [marker update FAILED]")));
    }
}

void MediaPlayer::onMastWedge(const std::string& where) {
    if (mastReloadRequested_.load())
        return; // one hand-off per process
    fpga_.serviceWedgeRead("H-b");
    const char* cmd = std::getenv("MPX_MAST_RELOAD_CMD");
    const bool haveHelper = cmd && *cmd && mastReloadHandler_;
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const mast::Marker marker = mast::readMarker();
    const mast::Action action = mast::decide(haveHelper, marker, now);
    if (action == mast::Action::AfterReload && mast::cureFailed(marker)) {
        const bool ok = mast::writeMarkerWhy("cure-failed", now, marker.reason, std::string(),
                                             mast::kMarkerPath, marker.r6);
        log("media: " + mast::cureFailedLine(marker, now, where) +
            (ok ? "" : " [marker write FAILED]"));
        return;
    }
    log("media: " + mast::actionLine(action, marker, now, where));
    if (action != mast::Action::RequestReload)
        return;
    if (!mast::writeMarker("requested", now, where)) {
        log(std::string("media: MAST reload marker ") + mast::kMarkerPath +
            " not written (" + std::strerror(errno) + "): no reload, clean fail");
        return;
    }
    std::string err;
    if (!mast::spawnHelper(cmd, ::getpid(), where, mast::kMarkerPath, &err)) {
        std::remove(mast::kMarkerPath);
        log("media: MAST reload launcher did not start (" + err + "): no reload, clean fail");
        return;
    }
    log(std::string("media: MAST reload launcher started: ") + cmd + " mast-reload " +
        std::to_string(::getpid()) + " (" + where + "); misterplexd exits with code " +
        std::to_string(mast::kReloadExitCode) + " so the guarded reload finds no daemon");
    mastReloadRequested_.store(true);
    mastReloadHandler_(where);
}

bool MediaPlayer::waitCoreGateOpen(int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    for (;;) {
        coreGateTick();
        if (CoreGate::process().open())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

static std::string idlePaintText(const std::string& spsErr) {
    if (CoreGate::process().idleErrorPaintAllowed()) {
        const std::string p = CoreGate::process().pairingErrorText();
        if (!p.empty())
            return p;
    }
    return spsErr;
}

void MediaPlayer::paintIdle() {
    // OWENB-P1B T3: a PlexOB core owns black and idle in fabric. There is no daemon DDR paint, and the idle FB
    // at 0x31600000 is never armed. This returns before gateRepaintOnce_ is read, so a gate-error clear's one
    // repaint stays latched for the next legacy core.
    obLibraryPublish();   // OWENB-P1B 0010: on an OB core the browser's repaint is the overlay's library page
    if (CoreGate::process().ownsIdleInFabric()) {
        obPlexUiPublish();   // OB port (MPX_PLEXUI=1): PlexUI is a full-screen overlay surface
        return;
    }
    if (plexUi_)   // OB port: back on R1 (or any legacy core) PlexUI renders the 640x480 RGB24 idle FB again
        (void)plexUi_->setRenderTarget(640, 480, plexui::PixelFormat::RGB24);
    // SPS-gate L3: a gate-error clear asked for ONE repaint, even in IDLE_SCREEN=last-frame (painted black there).
    // Read before any painter reads the latch: a clear clears the latch before it sets this flag. Only a paint that
    // can proceed takes the flag; a paint skipped for a closed gate or a live session leaves it for the next one.
    const bool pairingPaint = CoreGate::process().idleErrorPaintAllowed();
    const bool canPaint = (CoreGate::process().open() || pairingPaint) && !playing_.load() &&
                          !(fpgaH264Backend() && fpgaSession_.load() != 0);
    const bool forceOnce = canPaint && gateRepaintOnce_.exchange(false);
    IdleErrorPaintGuard idleErrGuard(pairingPaint);
    // R1: HDMI shows the idle FB. The present-bank paint below still runs, so 480p analog VGA is
    // unchanged.
    if (r1IdleFbReady())
        paintIdleR1(forceOnce);
    // F1 (idleBeatPlxl): the publish-only 240p core never presents a host-painted bank, so the
    // present-bank paint below (blitDisplayedBank, sendYuv420pFrameDdr and their PLXK kicks) is dead
    // work there and never runs (review 01:05: the blit excluded on F1 builds).
    const FpgaBuildInfo* idleRow = recognisedFpgaBuild(CoreGate::process().buildId());
    if (pairingPaint) {
        const FpgaBuildInfo* capsRow = recognisedFpgaBuild(CoreGate::process().lastCapsId());
        if (capsRow)
            idleRow = capsRow;
    }
    if (!f1::idlePaintsPresentBank(idleRow)) {
        logF1Beat();
        return;
    }
    if (browseV2_) {
        paintIdleV2(forceOnce);
        return;
    }
    // Core gate: never paint or ring the doorbell while another core is loaded.
    if (!CoreGate::process().open() && !pairingPaint)
        return;
    // Host idle paint targets the present DDR bank / fb0, never the H.264 DPB.
    // Skip only while a live compressed session still owns the picture path —
    // after release (stop/EOF) the frame store still latches the last AU, so the
    // chevron must be written or glass sticks on content forever.
    if (playing_.load())
        return;
    if (fpgaH264Backend() && fpgaSession_.load() != 0)
        return;
    const IdleMode m = idleMode();
    bool libVis = false;
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        libVis = library_.visible;
    }
    // SPS-gate L3: a latched gate error paints in place of the chevron, also in LastFrame mode.
    // Pairing mismatch / unrecognised CAPS uses the same renderer with both BUILD_IDs.
    const std::string gateErr = idlePaintText(gateErrorText());
    if (m == IdleMode::LastFrame && !libVis && gateErr.empty() && !forceOnce)
        return;
    // The one forced repaint after a gate-error clear paints LastFrame as black; every other paint is unchanged.
    const IdleMode rm = (forceOnce && m == IdleMode::LastFrame) ? IdleMode::Black : m;
    // Bank is DECODE (L4 1280 store). Chevron *design* follows latched Display
    // so 240p/480p reset makes a chunkier mark; nearest-scale into the bank
    // (do not paint a 624x480 payload into a 1280 store — yellow/static).
    const int w = outW_ > 0 ? outW_ : 320;
    const int h = outH_ > 0 ? outH_ : 240;
    const int designW = (displayRasterLatched_ && latchedDisplayRes_.width > 0)
                            ? latchedDisplayRes_.width
                            : w;
    const int designH = (displayRasterLatched_ && latchedDisplayRes_.height > 0)
                            ? latchedDisplayRes_.height
                            : h;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3);
    if (!gateErr.empty())
        gate::renderGateErrorRgb24(buf.data(), w, h, 0, w, gateErr);
    else
        renderIdleRgb24(buf.data(), w, h, rm, idlePhase_.load());

    std::lock_guard<std::mutex> lk(presentMu_);
    // SPS-gate L3 (review fix): the latch moved after this frame was rendered (a clear or a set). The mover's own
    // repaint follows, so never commit this stale picture over it.
    if (idlePaintText(gateErrorText()) != gateErr)
        return;
    if (fb_.ok() && !fb_.blitRgb24(buf.data(), w, h))
        log("media: idle fb0 blit failed");
    // F1 latches the last frame written, so the frame store must be repainted too.
    // C3 frame-store DDR is YUV-only, so encode the same idle renderer as I420
    // instead of ringing the doorbell with an RGB payload.
    if (fpga_.ok()) {
        bool ok = false;
        if (useDdrF1_) {
            const DdrFrameGeometry g = ddrFrameGeometryForPresentedSize(w, h);
            const DdrFrameLayout layout =
                makeDdrFrameLayout(g, kDdrFramePhysBase, kDdrFrameStrideAlign,
                                   DdrFrameFormat::Yuv420p);
            std::vector<uint8_t> yuv(layout.frame_bytes);
            bool painted = false;
            if (!gateErr.empty() && layout.frame_bytes > 0) {
                painted = gate::renderGateErrorI420(yuv.data(), g.coded_width, g.coded_height, g.crop_left,
                                                    g.display_width, gateErr);
                log("media: idle gate error painted bank=" + std::to_string(g.coded_width) + "x" +
                    std::to_string(g.coded_height) + " active_x=" + std::to_string(g.crop_left) +
                    " active_w=" + std::to_string(g.display_width));
            }
            if (gateErr.empty() && layout.frame_bytes > 0 && designW > 0 && designH > 0 &&
                (designW & 1) == 0 && (designH & 1) == 0 &&
                (designW != g.coded_width || designH != g.coded_height)) {
                const size_t ysz =
                    static_cast<size_t>(designW) * static_cast<size_t>(designH);
                const size_t csz = ysz / 4;
                std::vector<uint8_t> src(ysz + 2 * csz);
                if (renderIdleYuv420p(src.data(), designW, designH, rm, idlePhase_.load(),
                                      0, designW) &&
                    scaleI420NearestPlanes(src.data(), designW, src.data() + ysz,
                                           designW / 2, src.data() + ysz + csz,
                                           designW / 2, designW, designH, yuv.data(),
                                           g.coded_width, g.coded_height,
                                           layout.frame_bytes)) {
                    painted = true;
                    log(std::string("media: idle chevron design=") +
                        std::to_string(designW) + "x" + std::to_string(designH) +
                        " bank=" + std::to_string(g.coded_width) + "x" +
                        std::to_string(g.coded_height));
                }
            }
            if (!painted && layout.frame_bytes > 0 && gateErr.empty())
                painted = renderIdleYuv420p(yuv.data(), g.coded_width, g.coded_height, rm,
                                            idlePhase_.load(), g.crop_left, g.display_width);
            {
                std::lock_guard<std::mutex> lk(libraryMu_);
                if (library_.visible && !yuv.empty()) {
                    renderLibraryI420(yuv.data(), g.coded_width, g.coded_height, library_);
                    painted = true;
                }
            }
            if (painted) {
                // 480p: paint both banks so a later swap cannot flash a stale
                // silhouette. 720p24 L4 (5a7c5085): the second idle doorbell
                // leaves PLXD free=0 pending=1 frames_done=2; RequireReleased
                // then cannot retire the next swap (150 ms wait confirmed).
                const bool wantFabric =
                    fabricDirectWanted() && isPlex720pDdrFrameGeometry(g);
                fpga_.setFabricDirectPresent(wantFabric);
                const bool wantStick =
                    stickI420Wanted() && isPlex720pDdrFrameGeometry(g);
                fpga_.setStickI420Present(wantStick);
                uint32_t idleSrcPhys = 0;
                const uint8_t* idlePayload = yuv.data();
                if (wantFabric) {
                    // Heap std::vector is never 338-page contig → always STUB.
                    // Reuse process-lifetime idleFabric_ (same allocator as play).
                    if (idleFabric_.slot[0].phys == 0) {
                        (void)allocateFabricDirectSlots(idleFabric_, yuv.size(),
                                                        /*tryCompact=*/true,
                                                        /*minSlots=*/1);
                    }
                    if (idleFabric_.slot[0].phys != 0 && idleFabric_.slot[0].virt) {
                        std::memcpy(idleFabric_.slot[0].virt, yuv.data(), yuv.size());
                        idlePayload = idleFabric_.slot[0].virt;
                        // Keep process-lifetime virt; phys tracks live PFN.
                        idleSrcPhys = refreshFabricSlotPhys(idleFabric_.slot[0],
                                                            yuv.size());
                    } else {
                        idleSrcPhys = 0;
                    }
                    char pbuf[20];
                    std::snprintf(pbuf, sizeof(pbuf), "0x%08x",
                                  static_cast<unsigned>(idleSrcPhys));
                    log(std::string("media: fabric_direct idle src_phys=") +
                        (idleSrcPhys != 0 ? "REAL" : "STUB") + " slot=" + pbuf +
                        " how=" + fabricDirectHow(idleFabric_));
                }
                {
                    const auto sc = scoreI420Y(yuv.data(), g.coded_width, g.coded_height, 4);
                    char sbuf[192];
                    formatStickBankScore(sbuf, sizeof(sbuf), sc);
                    log(std::string("media: idle I420 ") + sbuf);
                }
                const bool ok0 = fpga_.sendYuv420pFrameDdr(
                    idlePayload, yuv.size(), g, 0, DdrBankWritePolicy::BestEffort,
                    idleSrcPhys);
                const bool ok1 =
                    (g.presented_height >= kPlex720pPresentedHeight)
                        ? false
                        : fpga_.sendYuv420pFrameDdr(
                              idlePayload, yuv.size(), g, 1,
                              DdrBankWritePolicy::BestEffort, idleSrcPhys);
                // 720p paints the free bank then doorbells; if the swap is
                // stuck, HDMI keeps the old chevron. Overwrite the scanned bank.
                const bool okDisp = fpga_.blitDisplayedBank(idlePayload, yuv.size());
                ok = ok0 || ok1 || okDisp;
                ddrBank_ = 0;
            }
        }
        if (!ok) {
            if (!idleWarned_.exchange(true))
                log("media: idle paint DDR failed (will retry on re-probe): " +
                    fpga_.lastError());
        } else {
            // Arm the warning again so a later failure is not swallowed — the core
            // is briefly out of user mode right after a heal/reload and the first
            // paint legitimately fails.
            idleWarned_.store(false);
            if (!idleLogged_.exchange(true))
                log("media: idle screen painted (mode=" + std::to_string(static_cast<int>(m)) + ")");
        }
    }
}

namespace {

bool paintRgbBufFromPlex(std::vector<uint8_t>& buf, int w, int h,
                         const std::vector<uint8_t>& plexRgb) {
    constexpr int kPw = 640;
    constexpr int kPh = 480;
    if (plexRgb.size() != static_cast<size_t>(kPw) * kPh * 3)
        return false;
    if (w == kPw && h == kPh) {
        std::memcpy(buf.data(), plexRgb.data(), plexRgb.size());
        return true;
    }
    for (int y = 0; y < h; ++y) {
        const int sy = y * kPh / h;
        for (int x = 0; x < w; ++x) {
            const int sx = x * kPw / w;
            const uint8_t* s = plexRgb.data() + (static_cast<size_t>(sy) * kPw + sx) * 3;
            uint8_t* d = buf.data() + (static_cast<size_t>(y) * w + x) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return true;
}

bool rgb24ToYuv420pBank(uint8_t* yuv, int w, int h, const uint8_t* rgb, int rgbW, int rgbH) {
    if (!yuv || !rgb || w < 2 || h < 2 || (w & 1) || (h & 1))
        return false;
    uint8_t* yPlane = yuv;
    uint8_t* uPlane = yPlane + static_cast<size_t>(w) * h;
    uint8_t* vPlane = uPlane + static_cast<size_t>(w / 2) * (h / 2);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int sx = x * rgbW / w;
            const int sy = y * rgbH / h;
            const uint8_t* p = rgb + (static_cast<size_t>(sy) * rgbW + sx) * 3;
            yPlane[static_cast<size_t>(y) * w + x] = idleRgbToY(p[0], p[1], p[2]);
        }
    }
    for (int cy = 0; cy < h / 2; ++cy) {
        for (int cx = 0; cx < w / 2; ++cx) {
            int uSum = 0;
            int vSum = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = cx * 2 + dx;
                    const int y = cy * 2 + dy;
                    const int sx = x * rgbW / w;
                    const int sy = y * rgbH / h;
                    const uint8_t* p = rgb + (static_cast<size_t>(sy) * rgbW + sx) * 3;
                    uSum += idleRgbToU(p[0], p[1], p[2]);
                    vSum += idleRgbToV(p[0], p[1], p[2]);
                }
            }
            uPlane[static_cast<size_t>(cy) * (w / 2) + cx] =
                static_cast<uint8_t>((uSum + 2) / 4);
            vPlane[static_cast<size_t>(cy) * (w / 2) + cx] =
                static_cast<uint8_t>((vSum + 2) / 4);
        }
    }
    return true;
}

} // namespace

// MPX_BROWSE_V2: paintIdle with every CPU step outside presentMu_ (the browse snapshot is
// taken under the browse lock, the chevron and list are rendered with no lock), so the lock
// covers only the fb0 blit, the bank writes and the doorbell. A paint whose snapshot is
// older than one already committed is dropped, so concurrent paints never go backwards.
void MediaPlayer::paintIdleV2(bool forceOnce) {
    if (!CoreGate::process().open() && !CoreGate::process().idleErrorPaintAllowed())
        return;
    if (playing_.load())
        return;
    if (fpgaH264Backend() && fpgaSession_.load() != 0)
        return;
    const IdleMode m = idleMode();
    const int w = outW_ > 0 ? outW_ : 320;
    const int h = outH_ > 0 ? outH_ : 240;
    const DdrFrameGeometry g = ddrFrameGeometryForPresentedSize(w, h);
    const bool plexVis = plexUi_ && plexUi_->visible();
    if (plexUi_ && plexui::deferIdlePaintOverPlexUi(plexVis, gateErrorText().empty(), plexUi_->paintReady()))
        return;
    uint64_t seq = 0;
    BrowseView view{};
    if (browse_) {
        std::lock_guard<std::mutex> lk(paintSnapMu_);
        seq = ++paintSeq_;
        view = browse_->view(g.coded_width, g.coded_height);
    } else {
        std::lock_guard<std::mutex> lk(paintSnapMu_);
        seq = ++paintSeq_;
    }
    const bool libVis = plexVis || view.window.visible;
    // SPS-gate L3: a latched gate error paints in place of the chevron, also in LastFrame mode.
    const std::string gateErr = idlePaintText(gateErrorText());
    if (m == IdleMode::LastFrame && !libVis && gateErr.empty() && !forceOnce)
        return;
    // The one forced repaint after a gate-error clear paints LastFrame as black; every other paint is unchanged.
    const IdleMode rm = (forceOnce && m == IdleMode::LastFrame) ? IdleMode::Black : m;
    const int designW = (displayRasterLatched_ && latchedDisplayRes_.width > 0)
                            ? latchedDisplayRes_.width
                            : w;
    const int designH = (displayRasterLatched_ && latchedDisplayRes_.height > 0)
                            ? latchedDisplayRes_.height
                            : h;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> plexRgb;
    plexuiPaintPlxhMs_ = plexVis ? misterplex::plexui::perfSteadyMs() : 0;
    bool paintPlex = plexVis && gateErr.empty() && plexUi_->copyFrame(plexRgb);
    if (paintPlex && !paintRgbBufFromPlex(buf, w, h, plexRgb))
        paintPlex = false;
    if (!paintPlex) {
        if (!gateErr.empty())
            gate::renderGateErrorRgb24(buf.data(), w, h, 0, w, gateErr);
        else
            renderIdleRgb24(buf.data(), w, h, rm, idlePhase_.load());
    }
    std::vector<uint8_t> yuv;
    bool painted = false;
    if (useDdrF1_) {
        const DdrFrameLayout layout =
            makeDdrFrameLayout(g, kDdrFramePhysBase, kDdrFrameStrideAlign, DdrFrameFormat::Yuv420p);
        yuv.resize(layout.frame_bytes);
        if (paintPlex && layout.frame_bytes > 0) {
            painted = rgb24ToYuv420pBank(yuv.data(), g.coded_width, g.coded_height, buf.data(), w,
                                         h);
        }
        if (!painted && !gateErr.empty() && layout.frame_bytes > 0)
            painted = gate::renderGateErrorI420(yuv.data(), g.coded_width, g.coded_height, g.crop_left,
                                                g.display_width, gateErr);
        if (!painted && gateErr.empty() && layout.frame_bytes > 0 && designW > 0 && designH > 0 &&
            (designW & 1) == 0 && (designH & 1) == 0 &&
            (designW != g.coded_width || designH != g.coded_height)) {
            const size_t ysz = static_cast<size_t>(designW) * static_cast<size_t>(designH);
            const size_t csz = ysz / 4;
            std::vector<uint8_t> src(ysz + 2 * csz);
            if (renderIdleYuv420p(src.data(), designW, designH, rm, idlePhase_.load(), 0,
                                  designW) &&
                scaleI420NearestPlanes(src.data(), designW, src.data() + ysz, designW / 2,
                                       src.data() + ysz + csz, designW / 2, designW, designH,
                                       yuv.data(), g.coded_width, g.coded_height,
                                       layout.frame_bytes)) {
                painted = true;
                log(std::string("media: idle chevron design=") + std::to_string(designW) + "x" +
                    std::to_string(designH) + " bank=" + std::to_string(g.coded_width) + "x" +
                    std::to_string(g.coded_height));
            }
        }
        if (!painted && layout.frame_bytes > 0 && gateErr.empty())
            painted = renderIdleYuv420p(yuv.data(), g.coded_width, g.coded_height, rm,
                                        idlePhase_.load(), g.crop_left, g.display_width);
        if (!paintPlex && libVis && browse_ && !yuv.empty()) {
            renderLibraryI420Titled(yuv.data(), g.coded_width, g.coded_height, view.window,
                                    view.header.empty() ? "PLEX LIBRARY" : view.header.c_str(),
                                    true);
            painted = true;
        }
        if (painted) {
            const auto sc = scoreI420Y(yuv.data(), g.coded_width, g.coded_height, 4);
            char sbuf[192];
            formatStickBankScore(sbuf, sizeof(sbuf), sc);
            log(std::string("media: idle I420 ") + sbuf);
        }
    }

    std::lock_guard<std::mutex> lk(presentMu_);
    // A session may have begun while this frame was rendered: never paint over it.
    if (playing_.load() || (fpgaH264Backend() && fpgaSession_.load() != 0))
        return;
    if (seq < paintCommitted_)
        return; // a later snapshot is already on screen
    paintCommitted_ = seq;
    if (fb_.ok() && !fb_.blitRgb24(buf.data(), w, h))
        log("media: idle fb0 blit failed");
    if (fpga_.ok()) {
        bool ok = false;
        if (useDdrF1_ && painted) {
            const bool wantFabric = fabricDirectWanted() && isPlex720pDdrFrameGeometry(g);
            fpga_.setFabricDirectPresent(wantFabric);
            const bool wantStick = stickI420Wanted() && isPlex720pDdrFrameGeometry(g);
            fpga_.setStickI420Present(wantStick);
            uint32_t idleSrcPhys = 0;
            const uint8_t* idlePayload = yuv.data();
            if (wantFabric) {
                if (idleFabric_.slot[0].phys == 0)
                    (void)allocateFabricDirectSlots(idleFabric_, yuv.size(), /*tryCompact=*/true,
                                                    /*minSlots=*/1);
                if (idleFabric_.slot[0].phys != 0 && idleFabric_.slot[0].virt) {
                    std::memcpy(idleFabric_.slot[0].virt, yuv.data(), yuv.size());
                    idlePayload = idleFabric_.slot[0].virt;
                    idleSrcPhys = refreshFabricSlotPhys(idleFabric_.slot[0], yuv.size());
                }
                char pbuf[20];
                std::snprintf(pbuf, sizeof(pbuf), "0x%08x", static_cast<unsigned>(idleSrcPhys));
                log(std::string("media: fabric_direct idle src_phys=") +
                    (idleSrcPhys != 0 ? "REAL" : "STUB") + " slot=" + pbuf +
                    " how=" + fabricDirectHow(idleFabric_));
            }
            const bool ok0 = fpga_.sendYuv420pFrameDdr(idlePayload, yuv.size(), g, 0,
                                                       DdrBankWritePolicy::BestEffort, idleSrcPhys);
            const bool ok1 = (g.presented_height >= kPlex720pPresentedHeight)
                                 ? false
                                 : fpga_.sendYuv420pFrameDdr(idlePayload, yuv.size(), g, 1,
                                                             DdrBankWritePolicy::BestEffort,
                                                             idleSrcPhys);
            const bool okDisp = fpga_.blitDisplayedBank(idlePayload, yuv.size());
            ok = ok0 || ok1 || okDisp;
            ddrBank_ = 0;
        }
        if (!ok) {
            if (!idleWarned_.exchange(true))
                log("media: idle paint DDR failed (will retry on re-probe): " + fpga_.lastError());
        } else {
            idleWarned_.store(false);
            if (!idleLogged_.exchange(true))
                log("media: idle screen painted (mode=" + std::to_string(static_cast<int>(m)) + ")");
        }
    }
    // R1 idle FB path acks flip (and navtrace) from paintIdleR1; avoid double onR1FlipAck here.
    if (paintPlex && plexUi_ && !r1IdleFbReady()) {
        const int64_t plxo = misterplex::plexui::perfSteadyMs();
        plexUi_->onR1FlipAck(plexuiPaintPlxhMs_, plxo);
    }
}

// R1: true when the core gate's build has hostIdleFb and the idle FB is mapped. The map and the
// target are armed once, on the first call that sees an R1 build (one "r1: idlefb path=" line).
bool MediaPlayer::r1IdleFbReady() {
    CoreGate& gate = CoreGate::process();
    if (!gate.open() && !gate.idleErrorPaintAllowed())
        return false;
    const FpgaBuildInfo* build = recognisedFpgaBuild(gate.buildId());
    if (!build)
        build = recognisedFpgaBuild(gate.lastCapsId());
    if (!build)
        build = fpga_.negotiatedBuild();
    if (!build || !build->hostIdleFb)
        return false;
    std::lock_guard<std::mutex> lk(r1PaintMu_);
    if (!idleFbArmTried_) {
        idleFbArmTried_ = true;
        const IdleFbMapPath path = idleFbMap_.arm(/*devmemSync=*/true);
        log(idleFbMap_.logLine());
        if (path != IdleFbMapPath::Off && idleFbMap_.paintAllowed()) {
            r1Target_ = std::make_unique<r1::R1IdleFbTarget>(
                path == IdleFbMapPath::Wc ? browse::IdleFbPath::WriteCombining : browse::IdleFbPath::DevMem,
                [this](int buf) { return idleFbMap_.buffer(buf); }, plxhLink_, plxhPolicy_, plxhPacer_,
                r1::IdleEntryClearHooks{
                    [this](std::string* why) { return r1EntryStreamClear(why); },
                    [this] { return r1SessionEnds_.load(); },
                    [this] { return playing_.load() || fpgaSession_.load() != 0; },
                    build->idleBeatPlxl},
                [this](const std::string& line) { log(line); });
            r1Planner_ = std::make_unique<browse::BrowsePaintPlanner>();
            r1Planner_->arm();
            r1CoreChanges_ = gate.changes();
            r1Armed_.store(true);
            // F1: the core needs the PLXL beat to show these buffers. Under r1PaintMu_, like
            // shutdown()'s stop: an arm that races shutdown either starts before that stop or never.
            if (build->idleBeatPlxl && !shuttingDown_.load()) {
                f1Beat_.start();
                log("f1: PLXL beat started (every " + std::to_string(f1::kBeatPeriodUs / 1000) +
                    " ms at idle, PLX page + 0x148)");
            }
        }
    }
    return r1Target_ != nullptr;
}

// F1 beat condition, lock-free (review 01:39 k1). plex_fb_en_hold (0x24F1A2C4) blanks HDMI
// one second after PLXL stops changing, including during play, so that row keeps the beat
// while the gate is open. Other rows stay idle-only.
bool MediaPlayer::f1IdleOk() const {
    const CoreGate& g = CoreGate::process();
    if (g.openAndLiveNoLock()) {
        const FpgaBuildInfo* b = recognisedFpgaBuild(CoreGate::snapshotBuildId(g.openSnapshot()));
        if (b && b->idleBeatPlxl)
            return true;
    }
    return r1Armed_.load() && !playing_.load() && fpgaSession_.load() == 0 && !plxhPolicy_.inSession();
}

// The beat's counters, logged from the idle paint (the beat thread itself never logs): the first
// write, then at most one line per 30 s while any skip counter moved.
void MediaPlayer::logF1Beat() {
    if (!f1Beat_.running())
        return;
    const uint64_t now[6] = {f1Beat_.writes(),    f1Beat_.notF1(),    f1Beat_.notIdle(),
                             f1Beat_.contended(), f1Beat_.unmapped(), f1Beat_.gateMoved()};
    const auto t = std::chrono::steady_clock::now();
    const bool first = f1BeatLogged_[0] == 0 && now[0] != 0;
    bool skipsMoved = false;
    for (int i = 3; i < 6; ++i)
        skipsMoved |= now[i] != f1BeatLogged_[i];
    if (!first && !(skipsMoved && t - f1BeatLogAt_ >= std::chrono::seconds(30)))
        return;
    f1BeatLogAt_ = t;
    std::copy(std::begin(now), std::end(now), std::begin(f1BeatLogged_));
    log(std::string(first ? "f1: PLXL beat live" : "f1: PLXL beat") + " writes=" + std::to_string(now[0]) +
        " hb=" + std::to_string(f1Beat_.lastHb()) + " skipped: not_f1=" + std::to_string(now[1]) +
        " not_idle=" + std::to_string(now[2]) + " page_held=" + std::to_string(now[3]) +
        " unmapped=" + std::to_string(now[4]) + " gate_moved=" + std::to_string(now[5]) +
        " max_gap_ms=" + std::to_string(f1Beat_.maxGapUs() / 1000) +
        " max_stall_us=" + std::to_string(f1Beat_.maxTickUs()));
}

bool MediaPlayer::r1EntryStreamClear(std::string* why) {
    std::lock_guard<std::mutex> g(r1ClearMu_);
    if (playing_.load() || fpgaSession_.load() != 0 || plxhPolicy_.inSession()) {
        if (why)
            *why = "session or BEGIN pending";
        return false;
    }
    if (!fpga_.idleEntryStreamClear(250)) {
        if (why)
            *why = fpga_.lastError();
        return false;
    }
    return true;
}

// R1 idle/browse paint: the browse level (r1RenderModel) or the S0 idle screen (renderIdleRgb24)
// through the planner. Flips go through the PLXH policy (refused from the PLAY request to the END,
// review 06:57 D3), the F5 pacer and, at most once per entry, the entry stream clear. Bounded at
// 400 ms; a flip still owed makes the idle thread retry within 250 ms (D1). The first paint after
// each session END repaints in full and flips even an unchanged frame, and logs that it re-showed
// the idle FB or why not (D2).
void MediaPlayer::paintIdleR1(bool forceOnce) {
    std::lock_guard<std::mutex> lk(r1PaintMu_);
    if (!r1Target_ || !r1Planner_) {
        r1FlipOwed_.store(false);
        return;
    }
    browse::BrowsePaintPlanner& planner = *r1Planner_;
    if (r1SessionRearm_.takeSessionEnd(r1SessionEnds_.load())) {
        planner.arm();
        r1AfterSession_ = true;
        r1AfterSessionWhy_.clear();
    }
    const auto owe = [this](const std::string& why) {   // once per reason per session END
        if (r1AfterSession_ && why != r1AfterSessionWhy_) {
            r1AfterSessionWhy_ = why;
            log("r1: idle request after session end deferred: " + why);
        }
    };
    if (playing_.load() || (fpgaH264Backend() && fpgaSession_.load() != 0)) {
        owe("a session is playing");
        r1FlipOwed_.store(r1AfterSession_ || planner.busy());
        return;
    }
    const IdleMode m = idleMode();
    bool listVis = false;
    librarylist::LibraryList listSnap;
    if (libraryListOn_) {
        std::lock_guard<std::mutex> lk(libraryMu_);
        listVis = libraryListVisible_;
        if (listVis)
            listSnap = libraryList_;
    }
    // PlexUI (MPX_PLEXUI=1) owns the idle FB while it is visible; off by default (plexUi_ null).
    const bool plexUiVis = browseV2_ && plexUi_ && plexUi_->visible();
    const bool libVis = plexUiVis || (browseV2_ && browse_ && browse_->visible());
    // SPS-gate L3: a latched gate error paints in place of the chevron, also in LastFrame mode.
    const std::string gateErr = idlePaintText(gateErrorText());
    // The one forced repaint after a gate-error clear paints LastFrame as black; every other paint is unchanged.
    const IdleMode rm = (forceOnce && m == IdleMode::LastFrame) ? IdleMode::Black : m;
    if (m == IdleMode::LastFrame && !libVis && !listVis && gateErr.empty() && !forceOnce) {
        if (r1AfterSession_)
            log("r1: no idle request after session end: IDLE_SCREEN=last-frame keeps the last picture");
        r1AfterSession_ = false;
        r1FlipOwed_.store(false);
        return;
    }
    const uint64_t coreChanges = CoreGate::process().changes();
    if (coreChanges != r1CoreChanges_) {   // a reloaded core shows neither idle buffer
        r1CoreChanges_ = coreChanges;
        planner.arm();
    }
    if (plexUiVis) {
        std::vector<uint8_t> frame;
        if (plexUi_->copyFrame(frame)) {
            plexuiR1SubmitAt_ = std::chrono::steady_clock::now();
            plexuiPaintPlxhMs_ = misterplex::plexui::perfSteadyMs();
            planner.submitFrame(std::move(frame));
        }
    } else if (listVis) {
        std::vector<uint8_t> frame(r1::kIdleBytes);
        librarylist::blitLibraryListIdle(listSnap, frame.data(), static_cast<int>(r1::kIdleW),
                                         static_cast<int>(r1::kIdleH), static_cast<int>(r1::kIdleStride));
        planner.submitFrame(std::move(frame));
    } else if (libVis) {
        const browse::RenderModel model = browse::r1RenderModel(browse_->r1View(), r1ViewportTop_);
        r1ViewportTop_ = model.viewportTop;
        planner.submit(model);
    } else {
        std::vector<uint8_t> frame(r1::kIdleBytes);
        if (!gateErr.empty())
            gate::renderGateErrorRgb24(frame.data(), static_cast<int>(r1::kIdleW), static_cast<int>(r1::kIdleH), 0,
                                       static_cast<int>(r1::kIdleW), gateErr);
        else
            renderIdleRgb24(frame.data(), static_cast<int>(r1::kIdleW), static_cast<int>(r1::kIdleH), rm,
                            idlePhase_.load());
        if (gateErr.empty() && (rm == IdleMode::Logo || rm == IdleMode::Screensaver)) {   // GCAST P4
            std::string ready;
            {
                std::lock_guard<std::mutex> lock(obMu_);
                ready = idleReadyLine_;
            }
            gcast_osd::stampReadyLineRgb24(frame.data(), static_cast<int>(r1::kIdleW), static_cast<int>(r1::kIdleH),
                                           ready);
        }
        planner.submitFrame(std::move(frame));
    }
    constexpr auto kBudget = std::chrono::milliseconds(400);
    const auto deadline = std::chrono::steady_clock::now() + kBudget;
    bool flipAcked = false;
    while (planner.busy() && std::chrono::steady_clock::now() < deadline) {
        if (playing_.load() || fpgaSession_.load() != 0 || plxhPolicy_.inSession()) {
            owe("the session has not ended yet");
            break;
        }
        if (plxhPolicy_.playRequestPending()) {
            owe("a PLAY request is pending (review 06:57 D3)");
            break;
        }
        const browse::PaintResult r = planner.pump(*r1Target_);
        if (r.step == browse::PaintStep::Acked) {
            flipAcked = true;
            const browse::BrowsePaintAck shown = r1Target_->pollAck(0, 0);
            if (plexuiR1SubmitAt_.has_value()) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - *plexuiR1SubmitAt_)
                                    .count();
                plexuiR1SubmitAt_.reset();
                log("plexui: perf flip_ack t_ms=" + std::to_string(ms));
                if (plexUi_) {
                    const int64_t plxo = misterplex::plexui::perfSteadyMs();
                    plexUi_->onR1FlipAck(plexuiPaintPlxhMs_, plxo);
                }
            }
            if (!r1PaintLogged_) {
                r1PaintLogged_ = true;
                log("r1: idle FB painted (first ACK) token=" + std::to_string(r.token) +
                    " idle_buf_shown=" + std::to_string(shown.idleBufShown) +
                    (listVis ? " library-list" : (libVis ? " browse" : " idle")));
            }
            if (r1AfterSession_) {
                r1AfterSession_ = false;
                log("r1: idle FB re-shown after session end token=" + std::to_string(r.token) +
                    " idle_buf_shown=" + std::to_string(shown.idleBufShown));
            }
        }
        if (r.step == browse::PaintStep::Refused || r.step == browse::PaintStep::Idle)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (r1AfterSession_ && planner.busy())
        owe("no ACK within the 400 ms paint bound, retried by the idle thread within 250 ms");
    r1FlipOwed_.store((planner.busy() && !flipAcked) || r1AfterSession_);
}

void MediaPlayer::r1NoteNoIdleRequest(const char* why) {
    std::lock_guard<std::mutex> lk(r1PaintMu_);
    if (!r1Target_ || !r1Planner_)
        return;
    if (r1SessionRearm_.takeSessionEnd(r1SessionEnds_.load())) {
        r1Planner_->arm();
        r1AfterSession_ = true;
    }
    if (r1AfterSession_)
        log(std::string("r1: no idle request after session end: ") + why);
    r1AfterSession_ = false;
    r1FlipOwed_.store(false);
}

// startIdle() is called from the play thread at session end while stopIdle() is
// called from the companion thread at the next play(); without this mutex the two
// can move-assign and join the same std::thread object and std::terminate.
void MediaPlayer::startIdle() {
    std::lock_guard<std::mutex> lk(idleMu_);
    if (shuttingDown_.load() || idleRun_.exchange(true))
        return;
    if (idleThr_.joinable())
        idleThr_.join();
    idleThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-idle");
#endif
        while (idleRun_.load()) {
            bool libVis = false;
            if (browseV2_) {
                libVis = (plexUi_ && plexUi_->visible()) || (browse_ && browse_->visible());
            } else {
                std::lock_guard<std::mutex> lk(libraryMu_);
                libVis = library_.visible;
            }
            if (libraryListOn_) {
                std::lock_guard<std::mutex> lk(libraryMu_);
                if (libraryListVisible_)
                    libVis = true;
            }
            // D2 (review 06:57): paint after the PLXH session END, not between the play thread's
            // session end and it (r1SessionEnds_ moves first); the R1 flip then re-shows the idle FB.
            const bool lastFrame =
                idleMode() == IdleMode::LastFrame && !libVis && gateErrorText().empty() &&
                !CoreGate::process().idleErrorPaintAllowed();
            const bool sessionLive = playing_.load() ||
                                     (fpgaH264Backend() && fpgaSession_.load() != 0) ||
                                     plxhPolicy_.inSession();
            if (sessionLive || lastFrame) {
                if (lastFrame && !sessionLive)
                    r1NoteNoIdleRequest("IDLE_SCREEN=last-frame keeps the last picture");
                std::this_thread::sleep_for(std::chrono::milliseconds(lastFrame ? 200 : 50));
                continue;
            }
            paintIdle();
            // A latched gate error is a static picture: no screensaver motion over it.
            const bool moving = idleMode() == IdleMode::Screensaver && gateErrorText().empty() &&
                                !CoreGate::process().idleErrorPaintAllowed();
            if (moving)
                idlePhase_.fetch_add(1);
            // A static idle screen is already latched in the frame store, so
            // repainting it buys nothing except another SIGSTOP of Main every
            // couple of seconds — forever, with no heal to follow. applyOsd()
            // and the session-end path repaint on the transitions that matter;
            // this slow sweep is only a safety net for a core reload underneath
            // us. The screensaver still moves at ~10 fps because the user asked
            // for motion.
            const int stepMs = r1::idleRepaintStepMs(moving, r1FlipOwed_.load());   // D1
            for (int slept = 0; slept < stepMs && idleRun_.load(); slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void MediaPlayer::stopIdle() {
    std::lock_guard<std::mutex> lk(idleMu_);
    idleRun_.store(false);
    if (idleThr_.joinable())
        idleThr_.join();
}

void MediaPlayer::setDecodeSize(int w, int h) {
    if (w < 160)
        w = 160;
    if (h < 120)
        h = 120;
    // Even dimensions for YUV-friendly sources
    w &= ~1;
    h &= ~1;
    if (fpgaH264Backend()) {
        // 240p product floor, or true480 present bank (demux clamps coded 624×480).
        if (w >= 480 || h >= 400) {
            outW_ = 640;
            outH_ = 480;
            fpsGate_ = true;
            const int active_height = plexSharedActiveHeight(outH_);
            if (active_height == outH_) {
                log("media: 480 active_height=" + std::to_string(active_height) +
                    " coded=" + std::to_string(outH_) + " crop_top=0 crop_bottom=0");
            }
        } else {
            outW_ = 320;
            outH_ = 240;
            fpsGate_ = true;
            const int active_height = plexSharedActiveHeight(outH_);
            if (active_height == outH_) {
                log("media: 240p active_height=" + std::to_string(active_height) +
                    " coded=" + std::to_string(outH_) + " crop_top=0 crop_bottom=0");
            }
        }
        return;
    }
    if (w > 1280)
        w = 1280;
    if (h > 720)
        h = 720;
    outW_ = w;
    outH_ = h;
}

SourceAspect MediaPlayer::probeSourceAspect(const std::string& urlOrPath,
                                            const std::string& httpHeaders,
                                            std::string* failDetail,
                                            int* codedW,
                                            int* codedH,
                                            int* fpsNum,
                                            int* fpsDen) const {
    if (fpgaH264Backend()) {
        if (failDetail)
            *failDetail = "source aspect deferred to the sole compressed demux";
        return {};
    }
    return ffmpegSourceAspect(ffmpeg_, urlOrPath, httpHeaders, failDetail, codedW,
                              codedH, fpsNum, fpsDen);
}

bool MediaPlayer::setSourceAspect(const SourceAspect& aspect) {
    if (fpgaH264Backend()) {
        std::lock_guard<std::mutex> present(presentMu_);
        sourceAspect_ = aspect;
        return true; // Published with a matching core session, before its first AU.
    }
    if (!aspect.valid) {
        log("ERROR media: refusing playback with unknown source display aspect");
        return false;
    }
    std::lock_guard<std::mutex> present(presentMu_);
    sourceAspect_ = aspect;
    if (presentMode_ != "fpga" && presentMode_ != "both") {
        log("media: source aspect=" + std::to_string(aspect.x) + ":" +
            std::to_string(aspect.y) + " owner=host_present no_fpga_transport");
        return true;
    }
    if (!fpga_.setDdrFrameLayout(ddrFrameGeometryForPresentedSize(outW_, outH_),
                                 DdrFrameFormat::Yuv420p)) {
        log("ERROR media: source aspect DDR layout failed: " + fpga_.lastError());
        return false;
    }
    SourceAspect sent;
    std::string clampNote;
    const bool published = fpga_.sendSourceAspect(aspect, &sent, &clampNote);
    if (!clampNote.empty())
        log("media: source aspect " + clampNote);
    if (!published) {
        // PLXJ WE is blank-only. A missing ACK must not CLOSED-STOP a live
        // cast — Plex Web then shows playing while glass stays on the chevron.
        log("media: source aspect publish optional-continue: " + fpga_.lastError());
        return true;
    }
    log("media: source aspect=" + std::to_string(sent.x) + ":" +
        std::to_string(sent.y) + " owner=MiSTer_native_scaler ack=matched");
    return true;
}

std::string MediaPlayer::lastError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lastError_;
}

void MediaPlayer::setSpsRejectCode480p(std::string code) {
    std::lock_guard<std::mutex> lock(mu_);
    spsRejectCode480p_ = std::move(code);
}

std::string MediaPlayer::spsRejectCode480p() const {
    std::lock_guard<std::mutex> lock(mu_);
    return spsRejectCode480p_;
}

std::string MediaPlayer::currentUrl() const {
    std::lock_guard<std::mutex> lock(mu_);
    return currentUrl_;
}

bool MediaPlayer::wantSkipRgbVideo() const {
    if (!streamEnabled_)
        return false;
    // Continuous fb0 needs RGB; skip only frees dual-A9 when FPGA alone owns present.
    if (presentMode_ == "both" || presentMode_ == "fb0" || presentMode_.empty())
        return false;
    // presentMode_ == "fpga": auto/on skip RGB from start (host recon owns F1).
    if (streamSkipRgb_ == "0" || streamSkipRgb_ == "off" || streamSkipRgb_ == "false" ||
        streamSkipRgb_ == "no")
        return false;
    // auto | on | 1 | true | yes | empty(default auto)
    return streamSkipRgb_ == "auto" || confTruthyMode(streamSkipRgb_) || streamSkipRgb_.empty();
}

bool MediaPlayer::initPresent() {
    if (backend_ == VideoBackend::OwenbRing) {
        // OWENB-P1B T3: MPX_VIDEO_BACKEND=owenb-ring. The PlexOB core owns the display: no fb0, no present banks.
        log("media: MPX_VIDEO_BACKEND=owenb-ring: no legacy present path (the PlexOB core owns the display)");
        return true;
    }
    if (backend_ == VideoBackend::Unselected) {
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = "MPX_VIDEO_BACKEND must explicitly select fpga-h264 or legacy-software";
        return false;
    }
    if (fpgaH264Backend())
        return presentMode_ == "fpga" && (fpga_.ok() || fpga_.open());
    if (presentMode_ != "none" &&
        !matchedLegacyVideoCore(rbfPrefix8_, std::getenv("MPX_LEGACY_CORE_PREFIX"))) {
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = "legacy presentation requires an explicitly matched OLD baseline core";
        return false;
    }
    if (presentMode_ == "none") {
        log("media: PRESENT=none decode-only path (test/lab; no fb0 or FPGA writes)");
        return true;
    }

    bool wantFb = (presentMode_ == "fb0" || presentMode_ == "both" || presentMode_.empty());
    bool wantFpga = (presentMode_ == "fpga" || presentMode_ == "both");

    bool any = false;
    if (wantFb) {
        if (fb_.open("/dev/fb0")) {
            if (CoreGate::process().open())
                fb_.clear();
            log("media: fb " + fb_.info() + " decode=" + std::to_string(outW_) + "x" +
                std::to_string(outH_));
            any = true;
        } else {
            log("media: /dev/fb0 unavailable");
        }
    }
    if (wantFpga) {
        if (fpga_.open()) {
            useDdrF1_ = true;
            ddrBank_ = 0;
            log("media: FPGA frame path OK (PRESENT=fpga → DDR YUV420p only)");
            // Env/compile MPX_FABRIC_DIRECT: idle + present share sendDdrFrame.
            // 720p layout still required inside sendDdrFrame; 480p memcpy unchanged.
            const bool wantFabric = fabricDirectWanted();
            fpga_.setFabricDirectPresent(wantFabric);
            log(std::string("media: fabric_direct=") + (wantFabric ? "1" : "0") +
                " (MPX_FABRIC_DIRECT env/compile; 720p may poke PLXP)");
            // PATH_SDRAM stick present. Default OFF — live 480p daemon unchanged.
            // Cannot detect SDRAM_I420_STORE vs ddr_frame_store safely at runtime.
            const bool wantStick = stickI420Wanted();
            fpga_.setStickI420Present(wantStick);
            log(std::string("media: stick_i420=") + (wantStick ? "1" : "0") +
                " (MPX_STICK_I420; idle+play memcpy PHYS_BASE 0x30180000)");
            // Legacy (pre-v3) core only: park the debug bits so a stale saved OSD
            // cannot steal cast frames. On a v3 core those same bits ARE the A/V
            // offset menu item, so zeroing them would silently reset the user's
            // setting on every startup.
            if (!osdControl_) {
                const int park[] = {6, 0, 7, 0, 8, 0, 9, 0};
                if (!fpga_.setStatusBits(park, 4))
                    log("media: park OSD (None/tone-off): " + fpga_.lastError());
                else
                    log("media: park OSD — Pattern=None, audio tone Off, force bars No");
            }
            // 720p HDMI product: Original AR is 16:9 even before a play-file.
            {
                const SourceAspect idleAr = defaultSourceAspectForBank(outW_, outH_);
                if (idleAr.valid && idleAr.x == 16 && idleAr.y == 9) {
                    const char* prev = std::getenv("MPX_ASPECT_ACK_OPTIONAL");
                    if (!prev)
                        ::setenv("MPX_ASPECT_ACK_OPTIONAL", "1", 0);
                    if (setSourceAspect(idleAr))
                        log("media: HDMI 720p default aspect=16:9");
                    else
                        log("media: HDMI 720p default aspect=16:9 publish skipped: " +
                            fpga_.lastError());
                }
            }
            any = true;
        } else {
            log("media: FPGA SPI unavailable: " + fpga_.lastError());
        }
    }
    if (audioEnabled_) {
        int fd = ::open(audioDev_.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            ::close(fd);
            log("media: audio device " + audioDev_ + " OK (s16le stereo @ 48k → FPGA)");
            // Do NOT call assertMisterFullVolume() here: MiSTer_cmd volume SPI
            // during open/play races Main and truncates free-run sessions
            // (~74 frames). start_misterplexd.sh sets volume before exec.
        } else {
            log("media: audio device " + audioDev_ + " unavailable (video-only)");
        }
    }
    if (!any) {
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = "no present path (fb0/fpga)";
        return false;
    }
    return true;
}

void MediaPlayer::signalChildren(int sig) {
    // Pause/resume RGB/audio FFmpeg. STREAM demux stays alive on pause.
    pid_t p = childPid_.load();
    if (p > 0)
        kill(-p, sig);
    pid_t ap = audioPid_.load();
    // Split 720p audio-only ffmpeg must pause with video (unlike streamPid_).
    if (ap > 0)
        kill(-ap, sig);
    pid_t sp = streamPid_.load();
    // Do not SIGSTOP the H.264 source demux on pause: PMS can tear down an HTTP
    // transcode session that stops being consumed. streamPump keeps reading and
    // drops NALs while paused; the FPGA freezes on the last decoded frame.
    if (sp > 0 && sig != SIGSTOP && sig != SIGCONT)
        kill(-sp, sig);
}

void MediaPlayer::killChildren() {
    if (fpgaH264Backend())
        return;
    signalChildren(SIGTERM);
    // Close PCM fds before any wait: O_RDWR self-writer / blocking waitpid
    // used to leave audioPump in pipe_read and freeze Play.
    {
        int hold = remuxPcmHoldFd_.exchange(-1);
        if (hold >= 0)
            ::close(hold);
        int rd = remuxPcmReadFd_.exchange(-1);
        if (rd >= 0)
            ::close(rd);
    }
    auto reap = [](std::atomic<pid_t>& slot) {
        pid_t x = slot.load();
        if (x <= 0)
            return;
        int st = 0;
        if (::waitpid(x, &st, WNOHANG) == x)
            slot.store(-1);
    };
    for (int i = 0; i < 20; ++i) {
        reap(childPid_);
        reap(audioPid_);
        reap(streamPid_);
        if (childPid_.load() <= 0 && audioPid_.load() <= 0 && streamPid_.load() <= 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    signalChildren(SIGKILL);
    for (int i = 0; i < 8; ++i) {
        reap(childPid_);
        reap(audioPid_);
        reap(streamPid_);
        if (childPid_.load() <= 0 && audioPid_.load() <= 0 && streamPid_.load() <= 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    audioActive_.store(false);
    streamActive_.store(false);
    ::unlink("/tmp/mplex-inproc.ts");
    ::unlink("/tmp/mplex-inproc.h264");
    ::unlink("/tmp/mplex-inproc.pcm");
}

void MediaPlayer::shutdown() {
    // Order matters: retire the play thread FIRST. threadMain calls startIdle()
    // at session end, so stopping the idle painter before joining thr_ leaves a
    // window where a brand new idle thread is spawned after we joined the old
    // one — it is then still joinable in ~MediaPlayer and aborts the process.
    shuttingDown_.store(true);
    {
        std::lock_guard<std::mutex> life(lifeMu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            playEpoch_.fetch_add(1);
            currentUrl_.clear();
            currentHeaders_.clear();
        }
        stop_.store(true);
        interruptInproc();
        killChildren();
        stopOwenbPlay();   // OWENB-P1B T5
        if (thr_.joinable())
            thr_.join();
        // threadMain normally joins these at session end, but it may never have
        // run (or may have been torn down mid-session), so sweep them here too.
        if (audioThr_.joinable())
            audioThr_.join();
        if (streamThr_.joinable())
            streamThr_.join();
        playing_.store(false);
        fpsGate_ = false;
        resetPlaybackPauseClock();
    }
    // Join outside lifeMu_: browse worker calls stop() which takes lifeMu_.
    {
        std::lock_guard<std::mutex> lk(browseThrMu_);
        if (browseThr_.joinable())
            browseThr_.join();
    }
    if (plexUi_)
        plexUi_->stop();
    else if (browse_)
        browse_->stop(); // the V2 worker: cancels its fetch, joins (outside lifeMu_ too)
    stopInputPoll();
    stopObInput();   // OWENB-P1B 0010
    stopOsdPoll();
    stopYcReloadWorker();
    stopIdle();
    {
        // Before the FpgaSpi closes and before members the beat reads go away. r1PaintMu_ orders it
        // against r1IdleFbReady's start (shuttingDown_ is already latched); the beat never takes it.
        std::lock_guard<std::mutex> lk(r1PaintMu_);
        f1Beat_.stop();
    }
    if (fpgaAudioOutput_ >= 0) {
        ::close(fpgaAudioOutput_);
        fpgaAudioOutput_ = -1;
    }
    releaseFabricDirectAlloc(idleFabric_);
    // R5f (review 01:19/01:25): every exit, 86 included, withdraws any outstanding audio command,
    // so the DDR control page never carries a valid command into a reload. It runs after every
    // thread above has stopped, so nothing can publish another.
    {
        std::lock_guard<std::mutex> present(presentMu_);
        std::string why;
        if (fpga_.withdrawAudioCommand(why))
            log("media: exit: audio command withdrawn (control commit +0x128 := 0)");
        else
            log("media: exit: audio command not withdrawn (" + why + ")");
    }
}

MediaPlayer::StoppedPosition MediaPlayer::stop() {
    // Only join thr_ here. threadMain owns audioThr_/streamThr_ joins at session end.
    // Joining helpers from both thr_ and stop() races and can hang the companion HTTP thread.
    std::lock_guard<std::mutex> life(lifeMu_);
    const bool userInitiated = userStop_;
    userStop_ = false;
    if (userInitiated)
        gateRepaintOnce_.store(true);
    log(std::string("media: stop userStop=") + (userInitiated ? "1" : "0"));
    {
        std::lock_guard<std::mutex> lock(mu_);
        playEpoch_.fetch_add(1);
        currentUrl_.clear();
        currentHeaders_.clear();
        gateFallbackPending_.reset();   // SPS-gate L3 b2: a stop cancels a pending gate fallback
    }
    stop_.store(true);
    interruptInproc();
    killChildren();
    stopOwenbPlay();   // OWENB-P1B T5: IDLE_SCREEN=last holds the OB picture; black flushes the ring
    if (thr_.joinable())
        thr_.join();
    const int64_t finalPos = positionMs_.load();
    int64_t finalDur = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        finalDur = durationMs_;
    }
    playing_.store(false);
#if defined(__linux__)
    restoreMisterNice(log_);
#endif
    resetPlaybackPauseClock();
    if (onProgress_)
        onProgress_("stopped", finalPos, finalDur);
    {
        // Drop session URL so post-stop seekMs cannot restart without a new playMedia.
        std::lock_guard<std::mutex> lock(mu_);
        currentUrl_.clear();
        currentHeaders_.clear();
        durationMs_ = 0;
    }
    seekReqMs_.store(-1);
    if (finalPos > 0)
        lastKnownPositionMs_.store(finalPos);
    positionMs_.store(0);
    lastPosMs_.store(0);
    showPlaybackOverlay(PlaybackOverlayState::Stopped, 0, 0);
    // Retire the background FPGA users BEFORE tearing the SPI/mmap state down.
    // stop() closes FpgaSpi and reloads the core; an OSD poll or idle paint in
    // flight would then ioctl through an unmapped handle and take the daemon down.
    stopOsdPoll();
    stopIdle();
    releaseFabricDirectAlloc(idleFabric_);
    if (!holdLastFrameAtEnd()) {
        if (fb_.ok() && CoreGate::process().open())
            fb_.clear();
        // Nothing to heal: SPI transactions hand GPO back to Main exactly as they
        // found it, and the frame path never touches SPI at all, so Main is still
        // servicing F12/OSD/MiSTer_cmd. Do NOT unlink /tmp/misterplex_spi.lock here —
        // recreating that inode would put concurrent tools on a different lock.
        paintIdle();
        startIdle();
    }
    startOsdPoll();
    TtfpTimestamps ttfp{};
    {
        std::lock_guard<std::mutex> lock(ttfpMu_);
        if (ttfp_.startUs && !ttfp_.logged) {
            ttfpTryComplete(ttfp_);
            ttfp_.logged = true;
            ttfp = ttfp_;
        }
    }
    if (ttfp.startUs)
        log(formatTtfpLog(ttfp));
    return {finalPos, finalDur};
}

void MediaPlayer::resetPlaybackPauseClock() {
    std::lock_guard<std::mutex> lk(pauseClockMu_);
    MPX_AV_TRACE(tracePauseClock_.beginUpdate();)
    paused_.store(false);
    pauseClockAccumulatedUs_ = 0;
    pauseClockHeld_ = false;
    pauseClockStarted_ = {};
    MPX_AV_TRACE(tracePauseClock_.endUpdate(0, 0, false);)
}

void MediaPlayer::transitionPlaybackPause(
    bool paused, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lk(pauseClockMu_);
    if (paused_.load() == paused)
        return;
    MPX_AV_TRACE(tracePauseClock_.beginUpdate();)
    paused_.store(paused);
    if (paused && !pauseClockHeld_) {
        pauseClockHeld_ = true;
        pauseClockStarted_ = now;
    } else if (!paused && pauseClockHeld_) {
        pauseClockAccumulatedUs_ +=
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - pauseClockStarted_)
                .count();
        pauseClockHeld_ = false;
    }
    MPX_AV_TRACE(tracePauseClock_.endUpdate(pauseClockAccumulatedUs_,
        std::chrono::duration_cast<std::chrono::microseconds>(
            pauseClockStarted_.time_since_epoch()).count(), pauseClockHeld_);)
}

int64_t MediaPlayer::playbackPausedUs(
    std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lk(pauseClockMu_);
    int64_t pausedUs = pauseClockAccumulatedUs_;
    if (pauseClockHeld_ && now > pauseClockStarted_) {
        pausedUs += std::chrono::duration_cast<std::chrono::microseconds>(
                        now - pauseClockStarted_)
                        .count();
    }
    return pausedUs;
}

int64_t MediaPlayer::playbackActiveUs() const {
    const auto now = std::chrono::steady_clock::now();
    return activePlaybackClockUs(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count(),
        playbackPausedUs(now));
}

void MediaPlayer::pause() {
    std::lock_guard<std::mutex> control(pauseControlMu_);
    if (owenbPlayActive()) {
        owenbPauseResume(true);   // OWENB-P1B T5: the core's pause word; the input is not read while paused
        return;
    }
    if (fpgaH264Backend() && (!playing_.load() || stop_.load() || paused_.load()))
        return;
    // Cast clients often emit pause during buffering before first glass. Honoring
    // that freezes the bitstream and a following resume ACK miss kills the title.
    if (fpgaH264Backend() && !fpgaVideoStarted_.load()) {
        log("media: ignore pause before first FPGA presentation");
        return;
    }
    // Close producer admission before publishing Pause. Any in-flight AU
    // finishes under presentMu_ ahead of Pause; none can occupy Resume's room.
    transitionPlaybackPause(true, std::chrono::steady_clock::now());
    if (fpgaSession_.load() && (fpgaH264Backend() || backend_ == VideoBackend::LegacySoftware)) {
        std::lock_guard<std::mutex> present(presentMu_);
        if (fpgaH264Backend()) {
            if (!fpga_.pauseBitstreamSession(fpgaSession_.load(), 500)) {
                // Host pause still applies. Missing ACK used to CLOSED-STOP mid-cast
                // (black glass) on 882b when UI/timeline raced first presentation.
                log("media: pause ACK optional-continue: " + fpga_.lastError());
            } else if (audioEnabled_ && sourceHasAudio_ &&
                       !controlFpgaAudio(fpgaSession_.load(), AudioSessionControl::Pause)) {
                log("media: audio pause ACK optional-continue: " + fpga_.lastError());
            }
        } else if (audioEnabled_ && sourceHasAudio_ && fpgaAudioStarted_.load() &&
                   !controlFpgaAudio(fpgaSession_.load(), AudioSessionControl::Pause)) {
            log("media: legacy audio pause ACK optional-continue: " + fpga_.lastError());
        }
    }
    signalChildren(SIGSTOP);
    showPlaybackOverlay(PlaybackOverlayState::Paused, positionMs_.load(), durationMs());
    if (onProgress_)
        onProgress_("paused", positionMs_.load(), durationMs_);
}

void MediaPlayer::resume() {
    std::lock_guard<std::mutex> control(pauseControlMu_);
    if (owenbPlayActive()) {
        owenbPauseResume(false);
        return;
    }
    if (fpgaH264Backend() && (!playing_.load() || stop_.load() || !paused_.load()))
        return;
    if (fpgaSession_.load() && (fpgaH264Backend() || backend_ == VideoBackend::LegacySoftware)) {
        std::lock_guard<std::mutex> present(presentMu_);
        if (fpgaH264Backend()) {
            // Match soft-pause: host resume must not CLOSED-STOP live glass when the
            // core misses resume ACK (seen after ~18 presented AUs on 882b on a lab MiSTer).
            if (!fpga_.resumeBitstreamSession(fpgaSession_.load(), 1500)) {
                log("media: resume ACK optional-continue: " + fpga_.lastError());
            } else if (audioEnabled_ && sourceHasAudio_ && fpgaAudioStarted_.load() &&
                       !controlFpgaAudio(fpgaSession_.load(), AudioSessionControl::Resume)) {
                log("media: audio resume ACK optional-continue: " + fpga_.lastError());
            }
        } else if (audioEnabled_ && sourceHasAudio_ && fpgaAudioStarted_.load() &&
                   !controlFpgaAudio(fpgaSession_.load(), AudioSessionControl::Resume)) {
            log("media: legacy audio resume ACK optional-continue: " + fpga_.lastError());
        }
    }
    // PAUSE-WATCHDOG: the stall clock restarts at resume (the paused time was not a stall).
    resumeArmTicks_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    transitionPlaybackPause(false, std::chrono::steady_clock::now());
    signalChildren(SIGCONT);
    showPlaybackOverlay(PlaybackOverlayState::Playing, positionMs_.load(), durationMs());
    if (onProgress_)
        onProgress_(fpgaH264Backend() && !fpgaVideoStarted_.load() ? "buffering" : "playing",
                    positionMs_.load(), durationMs_);
}

void MediaPlayer::showPlaybackOverlay(PlaybackOverlayState state, int64_t positionMs,
                                      int64_t durationMs) {
    overlay_.show(state, positionMs, durationMs);
    overlayForceGen_.fetch_add(1); // MPX_OVERLAY_ONCHANGE: a show always sends
#if defined(MPX_HAVE_OWENB_RING)
    if (owenbPlayActive())   // T6: the OB overlay's bar (the renderer, not ioctl 6)
        withObUi([](owenb_osd::UiBus& ui) { ui.showBar(obUiNowMs()); });
#endif
}

void MediaPlayer::onCoreReturned() {
    overlayForceGen_.fetch_add(1);
}

void MediaPlayer::flashPlaybackSkip(int64_t deltaMs) {
    overlay_.flashSkip(deltaMs, positionMs_.load(), durationMs());
#if defined(MPX_HAVE_OWENB_RING)
    if (owenbPlayActive()) {   // T6: transport feedback, "+30s" (§3)
        const int64_t s = deltaMs / 1000;
        const std::string t = (deltaMs >= 0 ? "+" : "-") + std::to_string(s >= 0 ? s : -s) + "s";
        withObUi([&t](owenb_osd::UiBus& ui) { ui.flashTransport(obUiNowMs(), t); });
    }
#endif
    if (fpgaH264Backend()) {
        std::lock_guard<std::mutex> lock(mu_);
        const int64_t seconds = deltaMs / 1000;
        fpgaOverlayTransport_ = deltaMs >= 0 ? ">> " : "<< ";
        fpgaOverlayTransport_ += std::to_string(seconds >= 0 ? seconds : -seconds) + "S";
        fpgaOverlayTransportUntil_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(PlaybackOverlay::kSkipVisibleMs);
    }
    overlayForceGen_.fetch_add(1);
}

void MediaPlayer::setPlaybackTitle(std::string title) {
    std::unique_lock<std::mutex> lock(mu_);
    playbackTitle_ = title.substr(0, 512);
    fpgaOverlayTransport_.clear();
    fpgaOverlayTransportUntil_ = {};
    overlay_.setTitle(playbackTitle_.c_str());
#if defined(MPX_HAVE_OWENB_RING)
    const std::string t = playbackTitle_;
    lock.unlock();   // T6: obMu_ is never taken under mu_
    withObUi([&t](owenb_osd::UiBus& ui) { ui.setTitle(t); });
#endif
}

void MediaPlayer::companionTimelineTick() {
    if (!onProgress_)
        return;
    const int64_t dur = durationMs();
    const char* state = paused_.load() ? "paused" : "playing";
    const int64_t pos = positionMs_.load();
    if (dur <= 0) {
        if (!timelineUnknownDurLogged_) {
            timelineUnknownDurLogged_ = true;
            log("media: timeline heartbeat duration unknown; reporting time with duration=0");
        }
        onProgress_(state, pos, 0);
        return;
    }
    timelineUnknownDurLogged_ = false;
    onProgress_(state, pos, dur);
}

void MediaPlayer::seekMs(int64_t ms, const StartedFn& started, uint64_t generation) {
    if (ms < 0)
        ms = 0;
    const int64_t fromMs = positionMs_.load();
    std::string url, headers;
    int64_t dur = 0;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        url = currentUrl_;
        headers = currentHeaders_;
        dur = durationMs_;
        epoch = playEpoch_.load();
    }
    // Clamp into known duration so scrubber/step edges cannot overshoot EOF.
    if (dur > 0 && ms > dur)
        ms = dur;
    if (url.empty()) {
        // No active session — drop seek (do not leave a phantom seekReq for next play).
        return;
    }
    flashPlaybackSkip(ms - fromMs);
    // Same scrubber position while session is live: skip demux restart thrash
    // (companion already ACK-only gates; belt-and-suspenders for step/skip paths).
    if (playing_.load() && !stop_.load() && positionMs_.load() == ms) {
        log("media: seek same-pos " + std::to_string(ms) + " (no-op)");
        {
            std::lock_guard<std::mutex> lock(mu_);
            startedPlayback_ = {generation, epoch};
        }
        if (started)
            started();
        return;
    }
    if (sessionBackend() == VideoBackend::OwenbRing && owenbSeek(ms, started, generation))
        return;   // OWENB-P1B T5: a held seek inside the running OB play
    if (onProgress_)
        onProgress_("buffering", ms, dur);
    // Full restart: both RGB/audio and STREAM demux re-spawn at new offset (multi-IDR clean).
    // Seek-safe clamp keeps mid-title IDRs under the FPGA 8KiB AU budget.
    playInternal(urlWithBitrateFloor480i(withSeekSafeTranscodeParams(url, ms)), ms, headers, dur,
                 epoch, started, generation);
}

bool MediaPlayer::play(const std::string& urlOrPath, int64_t startOffsetMs,
                       const std::string& httpHeaders, int64_t durationMs, uint64_t generation,
                       const gate::GatePlayContext& gateCtx) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        gateCtx_ = gateCtx;
        gateFallbackPending_.reset();
        boundedReopenRetryCount_ = 0;
        range_retry_ = 0;
        last_good_offset_ = 0;
    }
    return playInternal(urlWithBitrateFloor480i(urlOrPath), startOffsetMs, httpHeaders, durationMs, 0,
                        {}, generation);
}

bool MediaPlayer::resumeAfterRespawn(const std::string& urlOrPath, int64_t lastStoppedMs,
                                     const std::string& httpHeaders, int64_t durationMs,
                                     uint64_t generation) {
    if (lastStoppedMs <= 0)
        lastStoppedMs = lastPosMs_.load();
    if (lastStoppedMs <= 0)
        return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        gateCtx_ = gate::GatePlayContext{};
        gateFallbackPending_.reset();
    }
    return playInternal(urlWithBitrateFloor480i(withSeekSafeTranscodeParams(urlOrPath, lastStoppedMs)),
                        lastStoppedMs, httpHeaders, durationMs, 0, {}, generation);
}

// OWENB-P1B T6: the OB overlay (owenb_osd_renderer.hpp, ARM-RENDERER-SPEC-1017). Pages in fb0 through
// h264ring_fb_map (inside [0x22000000, +8 MiB): never the idle framebuffer at 0x31600000, never FbPresent);
// MPX_OB_OSD=0 turns it off, MPX_OB_OSD_SINGLE=1 forces the single-page path (Decision 3's fallback).
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
// OWENB-P1B 0009 (G15): one fbcon guard for the process -- fbcon off fb0 while our pages are there
// (owenb_fbcon_guard.hpp). MPX_OB_FBCON=keep: only look and log.
static std::shared_ptr<owenb::FbconGuard> owenbFbconGuard() {
    static std::shared_ptr<owenb::FbconGuard> g = [] {
        owenb::FbconGuard::Config c;
        const char* p = std::getenv("MPX_OB_FBCON");
        c.policy = p && std::strcmp(p, "keep") == 0 ? owenb::FbconGuard::Policy::Keep : owenb::FbconGuard::Policy::Unbind;
        c.log = [](const std::string& line) { std::fprintf(stderr, "misterplexd: %s\n", line.c_str()); };
        return std::make_shared<owenb::FbconGuard>(c);
    }();
    return g;
}

static owenb::OsdConfig owenbOsdConfigFromEnv() {
    owenb::OsdConfig c;
    const char* on = std::getenv("MPX_OB_OSD");
    c.enabled = !(on && std::strcmp(on, "0") == 0);
    const char* single = std::getenv("MPX_OB_OSD_SINGLE");
    c.allowDouble = !(single && std::strcmp(single, "1") == 0);
    auto fb = std::make_shared<struct ::h264ring_fb>();
    auto fbcon = owenbFbconGuard();
    c.mapFb = [fb, fbcon](owenb_osd::FbMap& m, std::string& err) {
        *fb = {};
        fb->fd = -1;
        fbcon->acquire();   // 0009: no console text over our pages
        const int rc = h264ring_fb_map(fb.get());
        if (rc != 0) {
            fbcon->release();
            err = std::string("h264ring_fb_map: ") + std::strerror(rc < 0 ? -rc : rc);
            return false;
        }
        m.virt = static_cast<uint8_t*>(fb->virt);
        m.phys = fb->phys;
        m.len = fb->len;
        return true;
    };
    c.unmapFb = [fb, fbcon] {
        h264ring_fb_unmap(fb.get());
        fbcon->release();   // 0009: the console back once our pages are gone
    };
    return c;
}

// OWENB-P1B 0006: the Analog Aspect default (Decision 1) and the restore of the user's value at every
// end; MPX_OB_LETTERBOX=0 turns the default off. The user's value waits in MPX_OB_ASPECT_STATE
// (default /media/fat/misterplex/ob_aspect_restore) while ours is on the menu, so a crash or a reboot
// still restores it at the next session.
static owenb::AspectConfig owenbAspectConfigFromEnv() {
    owenb::AspectConfig c;
    const char* on = std::getenv("MPX_OB_LETTERBOX");
    c.enabled = !(on && std::strcmp(on, "0") == 0);
    const char* path = std::getenv("MPX_OB_ASPECT_STATE");
    c.store = std::make_shared<owenb_osd::FileAspectStore>(path && *path ? path : "/media/fat/misterplex/ob_aspect_restore");
    return c;
}
#endif

// OWENB-P1B T5/T7: the OB player (one for the daemon's life: the idle session and every play share it).
// 0009: a daemon that died holding fb0 left fbcon detached: attach it at the next start.
void MediaPlayer::owenbFbconAtStart() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    owenbFbconGuard()->restoreLeftover();
#endif
}

void MediaPlayer::owenbQueueDepths(unsigned long long& out, unsigned long long& ready,
                                  unsigned long long& probe) const {
    out = ready = probe = 0;
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    std::lock_guard<std::mutex> g(obMu_);
    if (ob_)
        ob_->queueDepths(out, ready, probe);
#else
    (void)out;
    (void)ready;
    (void)probe;
#endif
}

void MediaPlayer::ensureOwenbPlayerLocked() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    if (ob_)
        return;
    ob_ = std::make_shared<owenb::OwenbPlayer>(
        [](std::string& err) -> std::unique_ptr<owenb::OwenbDdrWindow> {
            auto w = std::make_unique<owenb::DevMemDdrWindow>();
            const int rc = w->open(MPX_DEVMEM_PATH);
            if (rc != 0) {
                err = rc == -EBUSY ? "another program holds the owenb ring lock"
                                   : std::string("owenb ring map: ") + std::strerror(-rc);
                return nullptr;
            }
            return w;
        },
        [](const owenb::OpenRequest& req, std::string& err) { return owenb::openAvSource(req, err); });
    ob_->setOsd(owenbOsdConfigFromEnv());
    if (plexUi_) {   // OB port: a PlexUI frame on screen closes its nav trace, steps the focus animation
        owenb::OsdConfig oc = owenbOsdConfigFromEnv();
        oc.onSurfaceShown = [this](uint64_t gen) { obPlexUiShownAck(gen); };
        ob_->setOsd(std::move(oc));
    }
    ob_->setAspect(owenbAspectConfigFromEnv());
#endif
}

// OWENB-P1B T7: the OB idle session (design §c.4 rule 2). On a verified PlexOB core (CoreGate::owenbOpen) the
// OB player starts a session with no stream, so the overlay -- the idle card, the error screens -- shows without
// a play (h264ring.h:286); a hop off OB closes it (h264ring_close). MPX_OB_IDLE=0 turns it off. A failed start
// (the ring lock held, a core not alive yet) is tried again 5 s later.
void MediaPlayer::owenbIdleTick() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    const char* env = std::getenv("MPX_OB_IDLE");
    const bool enabled = !(env && std::strcmp(env, "0") == 0);
    const bool want = enabled && CoreGate::process().owenbOpen();
    std::shared_ptr<owenb::OwenbPlayer> ob;
    std::string name, pmsLabel, readyLine;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        if (want)
            ensureOwenbPlayerLocked();
        ob = ob_;
        name = obIdleName_;
        pmsLabel = obPmsLabel_;
        readyLine = idleReadyLine_;
    }
    if (!ob)
        return;
    const bool wanted = ob->idleWanted();
    if (want && !wanted) {
        const int64_t now = coreGateNowUs();
        if (now < obIdleRetryAtUs_)
            return;
        owenb::IdleConfig c;
        c.name = name;
        c.hint = readyLine;   // GCAST P4: "" -> "Ready to cast"
        if (plexUi_)   // OB port (MPX_PLEXUI=1): the entry point is pad A / the OSD's "Send Home" (Browse)
            c.hint = (readyLine.empty() ? std::string("Ready to cast") : readyLine) + " - Press A to browse Plex";
        c.log = [this](const std::string& line) { log("media: " + line); };
        std::string err;
        if (ob->startIdle(c, err)) {
            obIdleRetryAtUs_ = 0;
            obPmsShown_ = false;
            log("media: OB idle session armed: the overlay shows without a play");
            startObInput();   // 0010: once; it outlives the idle session
        } else {
            obIdleRetryAtUs_ = now + 5000000;
            log("media: OB idle session not started: " + err + " (again in 5 s)");
        }
        return;
    }
    if (!want && wanted) {
        log("media: OB idle session closed: the PlexOB core is no longer open");
        ob->stopIdle();
        return;
    }
    if (!want)
        return;
    // The PMS screen on the idle card: shown while the main loop's probe says unreachable, never over a play.
    if (ob->active()) {
        obPmsShown_ = false;
        return;
    }
    const int st = obPmsState_.load();
    if (st == 0 && !obPmsShown_) {
        ob->ui().setErrorFor(obUiNowMs(), owenb_osd::ErrorScreen::PmsUnreachable, pmsLabel, 0);
        obPmsShown_ = true;
    } else if (st != 0 && obPmsShown_) {
        ob->ui().update([](owenb_osd::UiModel& m) {
            if (m.error == owenb_osd::ErrorScreen::PmsUnreachable)
                m.error = owenb_osd::ErrorScreen::None;
        });
        obPmsShown_ = false;
    }
#endif
}

void MediaPlayer::setObIdleName(const std::string& name) {
    std::lock_guard<std::mutex> lock(obMu_);
    obIdleName_ = name;
}

void MediaPlayer::setIdleReadyLine(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(obMu_);
        if (idleReadyLine_ == line)
            return;
        idleReadyLine_ = line;
    }
    // FIELD-1018 F4: on a PlexOB core the card's hint was only read at startIdle, so the "Protected content -
    // can't play" line never reached the screen (glass ff3). Push it into the running idle card now.
    std::shared_ptr<owenb::OwenbPlayer> ob;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
        name = obIdleName_;
    }
    if (ob && ob->idleWanted() && !ob->active()) {  // a running play keeps its own UI; its end re-reads the hint
        std::string shown = line;
        if (plexUi_)
            shown = (line.empty() ? std::string("Ready to cast") : line) + " - Press A to browse Plex";
        ob->setIdleHint(shown);
    }
    if (!playing_.load() && !shuttingDown_.load())
        paintIdle();   // the idle screen already up shows the line now
}

void MediaPlayer::setObPmsState(int state, const std::string& label) {
    {
        std::lock_guard<std::mutex> lock(obMu_);
        obPmsLabel_ = label;
    }
    obPmsState_.store(state);
}

bool MediaPlayer::owenbIdleWanted() const {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    std::lock_guard<std::mutex> lock(obMu_);
    return ob_ && ob_->idleWanted();
#else
    return false;
#endif
}

void MediaPlayer::withObUi(const std::function<void(owenb_osd::UiBus&)>& f) {
#if defined(MPX_HAVE_OWENB_RING)
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (ob && f)
        f(ob->ui());
#else
    (void)f;
#endif
}

// OWENB-P1B 0010: the OB input thread. The core mirrors Main's pads, keys and OSD actions into the input
// region (h264ring.h:382-401; Main holds the devices with EVIOCGRAB, so nothing else reaches us on an OB
// core). Every 15 ms: a copy (OwenbPlayer::readInput, the handle's mutex for microseconds), the decoder
// (owenb_input.hpp), the panel / routing (owenb_menu.hpp), then the daemon's own dispatchPlaybackInput --
// the same transport and library code the DDR mailbox poll drives (startInputPoll). The thread lives from
// the first idle session to shutdown(); with no handle (a hop off OB) it waits and starts over.
void MediaPlayer::startObInput() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    const char* env = std::getenv("MPX_OB_INPUT");
    if (env && std::strcmp(env, "0") == 0)
        return;
    std::lock_guard<std::mutex> lk(obInputMu_);
    if (shuttingDown_.load() || obInputRun_.exchange(true))
        return;
    if (obInputThr_.joinable())
        obInputThr_.join();
    obInputThr_ = std::thread([this] { obInputLoop(); });
    log("media: OB input on: the core's pads, keys and OSD actions drive the menu panel and the library");
#endif
}

void MediaPlayer::stopObInput() {
    std::lock_guard<std::mutex> lk(obInputMu_);
    obInputRun_.store(false);
    if (obInputThr_.joinable())
        obInputThr_.join();
}

void MediaPlayer::obMenuState(const std::shared_ptr<owenb::OwenbPlayer>& ob, owenb::ObMenuState& s) {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    s.playing = ob && ob->active();
    s.paused = s.playing && ob->paused();
    s.positionMs = s.playing ? positionMs_.load() : -1;
    s.durationMs = s.playing ? durationMs() : 0;
    if (ob && s.playing)
        s.title = ob->ui().snapshot().title;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        s.castName = obIdleName_;
        s.pmsLabel = obPmsLabel_;
    }
    s.pmsState = obPmsState_.load();
    s.skipBackS = int(skipBackMs_ / 1000);
    s.skipForwardS = int(skipForwardMs_ / 1000);
    // the browser: V2 (browse_controller.hpp) or the legacy PMS list (library_browser.hpp); the liblist lab
    // mode draws only on the idle FB, so it is not offered here
    if (browseV2_ && browse_) {
        s.libraryAvailable = true;
        s.libraryVisible = browse_->visible();
    } else if (plexUi_) {   // OB port (MPX_PLEXUI=1): idle Select/Down/Home -> Browse opens PlexUI
        s.libraryAvailable = true;
        s.libraryVisible = plexUi_->visible();
    } else if (!libraryListOn_ && libraryFetch_) {
        s.libraryAvailable = s.libraryLegacy = true;
        std::lock_guard<std::mutex> lk(libraryMu_);
        s.libraryVisible = library_.visible;
    }
#else
    (void)ob;
    (void)s;
#endif
}

void MediaPlayer::obInputLoop() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "mpx-ob-input");
#endif
    owenb::ObInputDecoder dec;
    owenb::ObInputRouter router;
    bool synced = false;
    int64_t refreshAt = 0, libraryAt = 0;
    // BUG3 diagnostic (log only): a core with ring_in rewrites the input region at least once a
    // raster frame (IN_HEAD +1, the seqlock then consistent). A head that never moves, or a copy
    // that is never consistent (-EAGAIN), means the RBF has no input region (ring_in tied off,
    // e.g. the q2t-c / MPX_TRIM_RING_IN trim): no pad, key or OSD action can reach us.
    uint32_t inHeadLast = 0;
    int64_t inHeadSince = -1;
    bool inDeadLogged = false;
    while (obInputRun_.load()) {
        std::shared_ptr<owenb::OwenbPlayer> ob;
        {
            std::lock_guard<std::mutex> lock(obMu_);
            ob = ob_;
        }
        struct ::h264ring_input in {};
        const int rc = ob ? ob->readInput(in) : -ENODEV;
        if (rc == 0 || rc == -EAGAIN) {
            const int64_t t = obUiNowMs();
            const uint32_t head = static_cast<uint32_t>(H264RING_IN(&in, IN_HEAD));
            if (inHeadSince < 0 || (rc == 0 && head != inHeadLast)) {
                if (inDeadLogged)
                    log("media: OB input region alive again (in_head=" + std::to_string(head) + ")");
                inHeadLast = head;
                inHeadSince = t;
                inDeadLogged = false;
            } else if (!inDeadLogged && t - inHeadSince >= 2000) {
                inDeadLogged = true;
                log(std::string("media: OB input region DEAD for 2 s (") +
                    (rc == 0 ? "in_head=" + std::to_string(head) + " unchanged" : std::string("never consistent, -EAGAIN")) +
                    "): this RBF does not write words 48-71 (no ring_in), so its pads, keys and OSD actions"
                    " cannot reach the daemon");
            }
        }
        if (rc != 0) {
            if (rc == -ENODEV && synced) {   // the handle closed (a hop off OB): start over at the next one
                dec = owenb::ObInputDecoder{};
                synced = false;
                if (router.menu().isOpen()) {
                    router.close();
                    ob->ui().setMenu(owenb_osd::Menu{});
                }
            }
            for (int slept = 0; slept < 100 && obInputRun_.load(); slept += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        synced = true;
        const int64_t now = obUiNowMs();
        const owenb::ObInputStats before = dec.stats();
        const std::vector<owenb::ObNav> navs = dec.feed(in, now);
        // v2-bugfix (a lab MiSTer bug 3): say what the RBF sent that no mapping took, so glass shows the real ids
        if (dec.stats().unknownActions != before.unknownActions)
            log("media: OB input: unknown OSD action " + std::to_string(dec.stats().lastUnknownAction) +
                " (h264ring.h knows 0 Send Home, 1 Program menu)");
        if (dec.stats().unmappedKeys != before.unmappedKeys)
            log("media: OB input: unmapped key 0x" + [](int k) { char b[8]; std::snprintf(b, sizeof b, "%03X", k); return std::string(b); }(dec.stats().lastUnmappedKey));
        for (const owenb::ObNav nav : navs) {
            // TODO(F6, dev): binding point. PlexUI nav hooks the daemon's one ring_in decode loop
            // here (no second reader). If F6 shows input is not polled/bound on some play path
            // (gcast/companion), the fix belongs where this loop is started/bound; this hook follows it.
            if (plexUi_) {   // OB port: an open PlexUI takes the nav keys, as PLXI 5-11 on R1
                const owenb::PlexUiObOp op =
                    owenb::plexUiObNav(nav, plexUi_->visible() && !ob->active(), router.menu().isOpen());
                if (op.take) {
                    log(std::string("media: OB input ") + owenb::obNavName(nav) + " -> plexui");
                    if (op.horizontal)
                        plexUi_->postHorizontal(op.delta, "ring_in", now);
                    else
                        plexUi_->postIngress(op.op, "ring_in", now);
                    continue;
                }
            }
            owenb::ObMenuState st;
            obMenuState(ob, st);
            bool menuChanged = false;
            const PlaybackCommand cmd = router.handle(nav, st, menuChanged);
            log(std::string("media: OB input ") + owenb::obNavName(nav) +
                (cmd != PlaybackCommand::None ? " -> cmd=" + std::to_string(static_cast<int>(cmd)) : std::string()) +
                (menuChanged ? (router.menu().isOpen() ? " (menu)" : " (menu closed)") : ""));
            if (menuChanged)
                ob->ui().setMenu(router.menu().view());
            if (cmd != PlaybackCommand::None)
                dispatchPlaybackInput(cmd);
            libraryAt = 0;   // a command may have moved the browser
        }
        if (router.menu().isOpen() && now >= refreshAt) {   // the panel follows the state (position, pause)
            refreshAt = now + 500;
            owenb::ObMenuState st;
            obMenuState(ob, st);
            if (router.refresh(st))
                ob->ui().setMenu(router.menu().view());
        }
        if (now >= libraryAt) {
            libraryAt = now + 250;
            obLibraryPublish();
            obPlexUiTick();   // OB port: hand the screen back on close / play start, follow the canvas
        }
        obPlexUiAckDrain();   // OB port: PlexUI frame-on-screen acks (focus ease-out, nav trace)
        for (int slept = 0; slept < 15 && obInputRun_.load(); slept += 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
}

void MediaPlayer::obLibraryPublish() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (!ob || !CoreGate::process().ownsIdleInFabric())   // only on a PlexOB core (the legacy cores paint the list)
        return;
    owenb_osd::LibraryPage page;
    if (!ob->active()) {   // a play hides the page (the browser hides itself on its play too)
        if (browseV2_ && browse_) {
            if (browse_->visible()) {
                const BrowseR1View v = browse_->r1View();
                page = owenb::obLibraryPage(v.visible, v.rows, v.cursor, v.status, v.header);
            }
        } else if (!libraryListOn_ && libraryFetch_) {
            std::lock_guard<std::mutex> lk(libraryMu_);
            page = owenb::obLibraryPage(library_, "Library");
        }
    }
    std::lock_guard<std::mutex> lk(obLibMu_);
    if (obLibLast_ && *obLibLast_ == page)
        return;
    if (!obLibLast_ && !page.open)
        return;
    if ((obLibLast_ && obLibLast_->open) != page.open)
        log("media: OB library page " + std::string(page.open ? "shown, rows=" + std::to_string(page.total) : "hidden"));
    ob->ui().setLibrary(page);
    obLibLast_ = std::make_shared<owenb_osd::LibraryPage>(std::move(page));
#endif
}

#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
namespace {
// OB port: fb0's visible geometry (FBIOGET_VSCREENINFO) and length (FBIOGET_FSCREENINFO), read-only.
// On RBF 51cbb425 with video_mode 1280x720: 1280 x 720, 32 bpp, line 5120, smem 3,686,400.
struct ObFbGeom {
    unsigned xres = 0, yres = 0, bpp = 0, line = 0, smem = 0;
};
ObFbGeom obReadFb0Geom() {
    ObFbGeom g;
#if defined(__linux__)
    const char* dev = std::getenv("MPX_OB_FB_DEV");
    const int fd = ::open(dev && *dev ? dev : "/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return g;
    struct fb_var_screeninfo v {};
    struct fb_fix_screeninfo f {};
    if (::ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && ::ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0) {
        g.xres = v.xres;
        g.yres = v.yres;
        g.bpp = v.bits_per_pixel;
        g.line = f.line_length;
        g.smem = f.smem_len;
    }
    ::close(fd);
#endif
    return g;
}
} // namespace
#endif

// OB port: set while the input thread runs PlexUiController::onR1FlipAck (see obPlexUiAckDrain).
static thread_local bool tlObPlexUiInAck = false;

// The PlexUI target on OB: the core's canvas (the raster the overlay lays out on), never larger than
// fb0's visible size, and one w x h x 4 page must fit fb0's smem (the renderer double-buffers when
// two fit, else updates one page in place). Shrinks keeping the aspect when a page would not fit.
static void obPlexUiTargetSize(unsigned canvasW, unsigned canvasH, unsigned fbW, unsigned fbH, unsigned smem,
                               unsigned& w, unsigned& h) {
    w = canvasW ? canvasW : fbW;
    h = canvasH ? canvasH : fbH;
    if (fbW && fbH) {
        w = std::min(w, fbW);
        h = std::min(h, fbH);
    }
    w &= ~1u;
    h &= ~1u;
    while (smem && w >= 320 && h >= 240 && uint64_t(w) * h * 4u > smem) {
        w = (w * 7 / 8) & ~1u;
        h = (h * 7 / 8) & ~1u;
    }
}

void MediaPlayer::obPlexUiPublish() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    if (!plexUi_)
        return;
    if (tlObPlexUiInAck) {   // inside onR1FlipAck (controller mu_ held): render right after it
        obPlexUiRepaintPending_.store(true);
        return;
    }
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (!ob || !CoreGate::process().ownsIdleInFabric())
        return;
    std::lock_guard<std::mutex> lk(obPlexUiMu_);
    if (!plexUi_->visible() || ob->active()) {   // closed, or a play owns the screen: OSD / idle card back
        if (obPlexUiShown_) {
            ob->ui().setSurface(nullptr);
            obPlexUiShown_ = false;
            log("media: OB plexui closed: the overlay's idle card / OSD are back");
        }
        return;
    }
    const unsigned cw = ob->ui().canvasW(), ch = ob->ui().canvasH();
    const ObFbGeom fb = obReadFb0Geom();
    unsigned w = 0, h = 0;
    obPlexUiTargetSize(cw, ch, fb.xres, fb.yres, fb.smem, w, h);
    if (w < 320 || h < 240)
        return;
    if (plexUi_->setRenderTarget(int(w), int(h), plexui::PixelFormat::BGRA8888) || cw != obPlexUiCanvasW_ ||
        ch != obPlexUiCanvasH_)
        log("media: OB plexui target " + std::to_string(w) + "x" + std::to_string(h) + " BGRA8888 (canvas " +
            std::to_string(cw) + "x" + std::to_string(ch) + ", fb0 " + std::to_string(fb.xres) + "x" +
            std::to_string(fb.yres) + " " + std::to_string(fb.bpp) + "bpp line " + std::to_string(fb.line) +
            " smem " + std::to_string(fb.smem) + ")");
    obPlexUiCanvasW_ = cw;
    obPlexUiCanvasH_ = ch;
    auto s = std::make_shared<owenb_osd::FullSurface>();
    if (!plexUi_->copyFrame(s->bgra) || s->bgra.size() != size_t(w) * h * 4u)
        return;
    s->w = w;
    s->h = h;
    s->gen = ++obPlexUiGen_;
    obPlexUiSubmitGen_.store(s->gen);
    obPlexUiSubmitMs_.store(misterplex::plexui::perfSteadyMs());
    if (!obPlexUiShown_)
        log("media: OB plexui shown over the idle card");
    obPlexUiShown_ = true;
    ob->ui().setSurface(std::move(s));
#endif
}

void MediaPlayer::obPlexUiTick() {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    if (!plexUi_)
        return;
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (!ob)
        return;
    const bool want = plexUi_->visible() && !ob->active() && CoreGate::process().ownsIdleInFabric();
    bool stale;
    {
        std::lock_guard<std::mutex> lk(obPlexUiMu_);
        stale = want != obPlexUiShown_ ||
                (want && (ob->ui().canvasW() != obPlexUiCanvasW_ || ob->ui().canvasH() != obPlexUiCanvasH_));
    }
    if (stale)
        obPlexUiPublish();
#endif
}

// OSD thread: the surface gen is on screen. Stored only: onR1FlipAck repaints under the controller's mu_,
// and a repaint renders (copyFrame takes mu_), so it runs on the input thread (obPlexUiAckDrain).
void MediaPlayer::obPlexUiShownAck(uint64_t gen) {
    obPlexUiAckMs_.store(misterplex::plexui::perfSteadyMs());
    obPlexUiAckGen_.store(gen);
}


// Only the newest submitted frame closes the nav trace (an older gen was superseded before the glass).
// A repaint asked from inside onR1FlipAck is deferred to right after it (mu_ released).
void MediaPlayer::obPlexUiAckDrain() {
    if (!plexUi_)
        return;
    const uint64_t gen = obPlexUiAckGen_.exchange(0);
    if (gen && gen == obPlexUiSubmitGen_.load()) {
        const int64_t sub = obPlexUiSubmitMs_.load();
        const int64_t shown = obPlexUiAckMs_.load();
        tlObPlexUiInAck = true;
        plexUi_->onR1FlipAck(sub ? shown - sub : 0, 0);
        tlObPlexUiInAck = false;
    }
    if (obPlexUiRepaintPending_.exchange(false))
        obPlexUiPublish();
}

// OWENB-P1B T5: OB_CAVLC_MAX_KBPS (MPX_OB_CAVLC_MAX_KBPS also read): the highest CAVLC rate the OB play takes
// before it asks for a lowered (CABAC) request. Default 4,000 kbps; 0 turns the cap off.
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
static unsigned owenbCavlcMaxKbpsFromEnv() {
    for (const char* k : {"OB_CAVLC_MAX_KBPS", "MPX_OB_CAVLC_MAX_KBPS"})
        if (const char* v = std::getenv(k); v && *v) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(v, &end, 10);
            if (end && *end == 0 && n <= 1000000ul)
                return static_cast<unsigned>(n);
        }
    return 4000;
}
// v2-bugfix (a lab MiSTer bug 1): OB_CAVLC_MAX_LOAD_PCT (MPX_OB_CAVLC_MAX_LOAD_PCT also read): the share of one A9 the
// CAVLC rewrite may take before a stream above OB_CAVLC_MAX_KBPS takes the lowered retry. Default 70; 0 = the
// rate alone decides (the T5 rule).
static unsigned owenbCavlcMaxLoadPctFromEnv() {
    for (const char* k : {"OB_CAVLC_MAX_LOAD_PCT", "MPX_OB_CAVLC_MAX_LOAD_PCT"})
        if (const char* v = std::getenv(k); v && *v) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(v, &end, 10);
            if (end && *end == 0 && n <= 1000ul)
                return static_cast<unsigned>(n);
        }
    return 70;
}
// BUG1-CAP: OB_CAVLC_HARD_MAX_KBPS (default 7000): above it a CAVLC play ends whatever its measured load; 0 = off.
static unsigned owenbCavlcHardMaxKbpsFromEnv() {
    for (const char* k : {"OB_CAVLC_HARD_MAX_KBPS", "MPX_OB_CAVLC_HARD_MAX_KBPS"})
        if (const char* v = std::getenv(k); v && *v) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(v, &end, 10);
            if (end && *end == 0 && n <= 100000ul)
                return static_cast<unsigned>(n);
        }
    return 7000;
}
#endif

void MediaPlayer::armCastOpenRetry(int64_t previousMs) {
    std::lock_guard<std::mutex> lock(mu_);
    castOpenRetry_ = true;
    castOpenIdleOnFail_ = true;
    castOpenRetryPrevMs_ = previousMs;
}

void MediaPlayer::armYt403Reresolve(std::function<bool(std::string& url, std::string& audio)> fn) {
    std::lock_guard<std::mutex> lock(mu_);
    yt403Reresolve_ = std::move(fn);
}

// OWENB-P1B T3/T5: play on the OwenbRing backend. It refuses unless the gate verified a PlexOB core by its ring.
// T5: the OB play (owenb_play.hpp): libav demux -> Annex-B AUs with PTS (the library's CAVLC->CABAC rewrite; a
// CAVLC stream above OB_CAVLC_MAX_KBPS or one the rewrite refuses ends NeedsTranscode -> one lowered PMS request,
// row R6p) and 48 kHz s16le stereo PCM records with PTS -> OwenbRing. The route writes the owenb ctrl page and
// ring only (DevMemDdrWindow: no SPI, no idle FB at 0x31600000, no DDR paint).
bool MediaPlayer::playOwenbRoute(const std::string& urlOrPath, int64_t startOffsetMs, const std::string& httpHeaders,
                                 int64_t durationMs, uint64_t expectedEpoch, const StartedFn& started,
                                 uint64_t generation) {
    const CoreGate& gate = CoreGate::process();
    std::string why;
    if (!gate.owenbOpen())
        why = "OwenbRing backend: the loaded core is not a verified PlexOB core (" + coreGateClosedError("play") + ")";
#if !defined(MPX_HAVE_OWENB_RING) || !defined(MPX_HAVE_LIBAV)
    else
        why = "OwenbRing play is not built into this daemon (it needs MPX_HAVE_OWENB_RING and MPX_HAVE_LIBAV)";
    (void)urlOrPath; (void)startOffsetMs; (void)httpHeaders; (void)durationMs; (void)started; (void)generation;
#endif
    if (why.empty() && expectedEpoch && expectedEpoch != playEpoch_.load())
        return false;   // a stop or a newer play replaced this request
    if (!why.empty()) {
        log("media: play refused: " + why);
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = std::move(why);
        castOpenRetry_ = false;
        castOpenRetryPrevMs_ = -1;
        castOpenIdleOnFail_ = false;
        yt403Reresolve_ = nullptr;
        return false;
    }
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    stopOwenbPlay();
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        epoch = playEpoch_.fetch_add(1) + 1;
        currentUrl_ = urlOrPath;
        currentHeaders_ = httpHeaders;
        durationMs_ = durationMs;
        lastError_.clear();
    }
    owenb::OpenRequest req;
    req.headers = httpHeaders;
    req.startMs = startOffsetMs > 0 ? startOffsetMs : 0;
    std::string dashVideo, dashAudio;
    if (gcast::splitYtAv(urlOrPath, dashVideo, dashAudio)) {
        // Two opened inputs, time-seeked. The audio URL is not rewritten.
        req.url = dashVideo;
        req.audioUrl = dashAudio;
        req.containerSeek = req.startMs > 0;
    } else {
        // Bake 2217 ms as offset=2 (universal) or the ladder carrier, then do not
        // seek that live TS again. A file/Part still container-seeks.
        req.url = transcode_offset::retargetStartUrl(urlOrPath, req.startMs);
        req.containerSeek = transcode_offset::demuxSeekMs(req.url, req.startMs) > 0;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (castOpenRetry_) {
            req.retryTransientOpen = true;
            req.previousMs = castOpenRetryPrevMs_;
            castOpenRetry_ = false;
            castOpenRetryPrevMs_ = -1;
        }
        if (yt403Reresolve_)
            req.reresolve403 = std::move(yt403Reresolve_);
    }
    owenb::SessionConfig cfg;
    cfg.holdLastPicture = lastFrameHoldEnabled_.load();
    cfg.cavlcMaxKbps = owenbCavlcMaxKbpsFromEnv();
    cfg.cavlcMaxLoadPct = owenbCavlcMaxLoadPctFromEnv();
    cfg.cavlcHardMaxKbps = owenbCavlcHardMaxKbpsFromEnv();
    cfg.onEntropy = [this, epoch](bool cavlc, unsigned kbps, unsigned loadPct) {   // BUG1: per-PMS memory
        if (playEpoch_.load() == epoch && obEntropyHook_)
            obEntropyHook_(cavlc, kbps, loadPct);
    };
    cfg.onBuffering = [this, epoch, durationMs](int64_t ms) {
        if (playEpoch_.load() != epoch)
            return;
        positionMs_.store(ms);
        withObUi([ms](owenb_osd::UiBus& ui) {
            ui.update([ms](owenb_osd::UiModel& m) {
                m.buffering = true;
                m.idle = false;
                m.positionMs = ms;
            });
        });
        if (onProgress_)
            onProgress_("buffering", ms, durationMs);
    };
    owenb::OwenbPlayCallbacks cb;
    cb.log = [this](const std::string& s) { log("media: " + s); };
    cb.position = [this](int64_t ms) { positionMs_.store(ms); };
    // BUG2-1017: OB timeline heartbeat (PMS /:/timeline + companion poll time)
    cb.progress = [this, epoch](int64_t ms, bool) {
        if (playEpoch_.load() != epoch)
            return;
        positionMs_.store(ms);
        companionTimelineTick();
    };
    const StartedFn startedCopy = started;
    cb.started = [this, epoch, generation, startedCopy] {
        if (playEpoch_.load() != epoch)
            return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            startedPlayback_ = {generation, epoch};
        }
        if (startedCopy)
            startedCopy();
        withObUi([](owenb_osd::UiBus& ui) {   // T6: the bar for 4 s once the picture is up
            ui.setBuffering(false);
            ui.showBar(obUiNowMs());
        });
        if (onProgress_)
            onProgress_("playing", positionMs_.load(), this->durationMs());
    };
    cb.ended = [this, epoch, generation](owenb::EndReason r, const std::string& err, int64_t pos) {
        owenbPlayEnded(epoch, generation, r, err, pos);
    };
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ensureOwenbPlayerLocked();   // T7: the same player as the idle session's
        ob = ob_;
        obEpoch_ = epoch;
    }
    ob->setVolumePercent(volumePercent_.load());
    {
        std::string title;
        {
            std::lock_guard<std::mutex> lock(mu_);
            title = playbackTitle_;
        }
        const int64_t startMs = req.startMs;
        ob->ui().update([&](owenb_osd::UiModel& m) {   // a new play: Buffering...; a queued toast stays (R6p)
            auto toasts = std::move(m.toasts);
            m = owenb_osd::UiModel{};
            m.toasts = std::move(toasts);
            m.title = title;
            m.durationMs = durationMs;
            m.positionMs = startMs;
            m.buffering = true;
        });
    }
    positionMs_.store(req.startMs);
    paused_.store(false);
    playing_.store(true);
    if (onProgress_)
        onProgress_("buffering", req.startMs, durationMs);
    std::string err;
    if (!ob->start(req, epoch, cfg, cb, err)) {
        playing_.store(false);
        log("media: OB play refused: " + err);
        ob->ui().setErrorFor(obUiNowMs(), owenb_osd::ErrorScreen::StreamError, err);   // T7: on the idle card
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = "OwenbRing: " + err;
        castOpenIdleOnFail_ = false;
        return false;
    }
    log("media: OB play started (OwenbRing, epoch " + std::to_string(epoch) + ")");
    // FIELD-1018 F6: the input thread was started only by an armed idle session (the idle arm). A play reached
    // with no idle session armed first (gcast / companion before the idle arm, an idle start that failed)
    // ran with no pad/key/OSD input at all. Idempotent: a running thread is kept.
    startObInput();
    return true;
#else
    return false;
#endif
}

#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
// The OB play ended (on its own thread). A stale epoch (a stop or a newer play) reports nothing. A stream the core
// cannot take (NeedsTranscode) hands off to main's gate fallback as one lowered PMS request (R6p) at the position
// reached; main drops it if this item already had its lowered retry.
void MediaPlayer::owenbPlayEnded(uint64_t epoch, uint64_t generation, owenb::EndReason r, const std::string& err,
                                 int64_t pos) {
    log(std::string("media: OB play ended: ") + owenb::toString(r) + (err.empty() ? "" : ": " + err));
    if (playEpoch_.load() != epoch || r == owenb::EndReason::Stopped)
        return;   // stop() reports its own state
    const int64_t dur = durationMs();
    if (r == owenb::EndReason::NeedsTranscode && gateFallbackHandler_) {
        GateFallbackRequest f{generation, pos, "OB: " + err, gate::rowName(gate::GateRow::R6p)};
        log("media: GATE fallback R6p (OB): the same item restarts as one lowered PMS request at offset_ms=" +
            std::to_string(pos));
        playing_.store(false);
        withObUi([](owenb_osd::UiBus& ui) { ui.toast(obUiNowMs(), "Transcoding instead"); });
        gateFallbackHandler_(f);
        return;
    }
    // A cast open that was armed for the chevron (helper skip / 403 after the
    // one re-resolve) must not paint PLAYBACK ERROR. Other SourceErrors still do.
    bool idleOnOpenFail = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (gcast::castOpenFailEndsIdle(castOpenIdleOnFail_, r == owenb::EndReason::SourceError)) {
            idleOnOpenFail = true;
            castOpenIdleOnFail_ = false;
        }
    }
    if (idleOnOpenFail) {
        std::string name, hint;
        {
            std::lock_guard<std::mutex> lock(obMu_);
            name = obIdleName_;
            hint = idleReadyLine_;
        }
        withObUi([&](owenb_osd::UiBus& ui) {
            ui.clearError();
            ui.setIdle(true, name, hint);
        });
    } else if (r == owenb::EndReason::NeedsTranscode)
        withObUi([&](owenb_osd::UiBus& ui) { ui.setErrorFor(obUiNowMs(), owenb_osd::ErrorScreen::Refused, err); });
    else if (r != owenb::EndReason::Eof)
        withObUi([&](owenb_osd::UiBus& ui) { ui.setErrorFor(obUiNowMs(), owenb_osd::ErrorScreen::StreamError, err); });
    if (r != owenb::EndReason::Eof) {
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = "OwenbRing: " + err;
    }
    playing_.store(false);
    paused_.store(false);
    if (onProgress_)
        onProgress_(r == owenb::EndReason::Eof ? "ended" : "stopped", pos, dur);
}
#endif

// OWENB-P1B T5: a seek during an OB play is held inside the running play: the picture stays while the input
// reopens at the target (the PMS universal offset, or a container seek), then the ring is flushed onto the new IDR.
bool MediaPlayer::owenbSeek(int64_t ms, const StartedFn& started, uint64_t generation) {
#if defined(MPX_HAVE_OWENB_RING) && defined(MPX_HAVE_LIBAV)
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (!ob || !ob->active())
        return false;
    owenb::OpenRequest q;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        q.url = transcode_offset::retargetStartUrl(currentUrl_, ms);
        q.headers = currentHeaders_;
        epoch = playEpoch_.load();
        startedPlayback_ = {generation, epoch};
        q.previousMs = positionMs_.load();
    }
    q.startMs = ms;
    {
        std::string video, audio;
        if (gcast::splitYtAv(q.url, video, audio)) {
            q.url = video;
            q.audioUrl = audio;
            q.containerSeek = true;
        } else {
            q.containerSeek = transcode_offset::demuxSeekMs(q.url, q.startMs) > 0;
        }
    }
    q.retryTransientOpen = q.url.find("/yt.ts") != std::string::npos || q.url.find("start_ms=") != std::string::npos ||
                           !q.audioUrl.empty();
    ob->seek(q);
    ob->ui().seek(obUiNowMs(), ms);
    positionMs_.store(ms);
    log("media: OB seek (held) to " + std::to_string(ms) + " ms");
    if (started)
        started();
    if (onProgress_)
        onProgress_(ob->paused() ? "paused" : "playing", ms, durationMs());
    return true;
#else
    (void)ms; (void)started; (void)generation;
    return false;
#endif
}

// OWENB-P1B T5: pause / resume on the OB play. A pause before the first picture is ignored (cast clients pause
// while buffering), as on the legacy path.
void MediaPlayer::owenbPauseResume(bool pause) {
#if defined(MPX_HAVE_OWENB_RING)
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (!ob)
        return;
    if (pause) {
        if (!ob->started()) {
            log("media: OB: ignore pause before the first picture");
            return;
        }
        if (paused_.load())
            return;
        ob->pause();
        paused_.store(true);
    } else {
        if (!paused_.load())
            return;
        ob->resume();
        paused_.store(false);
        ob->ui().showBar(obUiNowMs());
    }
    if (onProgress_)
        onProgress_(pause ? "paused" : "playing", positionMs_.load(), durationMs());
#else
    (void)pause;
#endif
}

void MediaPlayer::stopOwenbPlay() {
#if defined(MPX_HAVE_OWENB_RING)
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (ob)
        ob->stop();   // joins the OB thread; IDLE_SCREEN=last holds the picture, black flushes
#endif
}

bool MediaPlayer::owenbPlayActive() const {
#if defined(MPX_HAVE_OWENB_RING)
    std::lock_guard<std::mutex> lock(obMu_);
    return ob_ && ob_->active();
#else
    return false;
#endif
}

void MediaPlayer::setVolumePercent(int volume) {
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    volumePercent_.store(volume);
#if defined(MPX_HAVE_OWENB_RING)
    std::shared_ptr<owenb::OwenbPlayer> ob;
    {
        std::lock_guard<std::mutex> lock(obMu_);
        ob = ob_;
    }
    if (ob)
        ob->setVolumePercent(volume);
#endif
    log("media: volume " + std::to_string(volume));
}

bool MediaPlayer::playInternal(const std::string& urlOrPath, int64_t startOffsetMs,
                               const std::string& httpHeaders, int64_t durationMs,
                               uint64_t expectedEpoch, const StartedFn& started,
                               uint64_t generation) {
    // OWENB-P1B T3: the core gate picks the backend per core. The OB route never reaches the legacy path.
    if (sessionBackend() == VideoBackend::OwenbRing)
        return playOwenbRoute(urlOrPath, startOffsetMs, httpHeaders, durationMs, expectedEpoch, started,
                              generation);
    {
        std::lock_guard<std::mutex> lock(mu_);
        castOpenRetry_ = false;
        castOpenRetryPrevMs_ = -1;
        castOpenIdleOnFail_ = false;
        yt403Reresolve_ = nullptr;
    }
    if (!CoreGate::process().open()) {
        // No SPI/DDR for another core; the idle state is left as it is.
        std::string why = coreGateClosedError("play");
        log("media: play refused: " + why);
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = std::move(why);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(libraryMu_);
        library_.hide();
        libraryListVisible_ = false;
    }
    if (browseV2_) {
        if (plexUi_)
            plexUi_->onPlayStarted(generation);
        else if (browse_)
            browse_->onPlayStarted(generation);
    }
    // Idle painter owns fb0/F1 between sessions — retire it before we present.
    stopIdle();
    {
        std::lock_guard<std::mutex> life(lifeMu_);
        if (expectedEpoch && expectedEpoch != playEpoch_.load())
            return false;
        uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            epoch = playEpoch_.fetch_add(1) + 1;
            currentUrl_.clear();
            currentHeaders_.clear();
        }
        stop_.store(true);
        interruptInproc();
        killChildren();
        if (thr_.joinable())
            thr_.join();

        if (backend_ == VideoBackend::Unselected) {
            std::lock_guard<std::mutex> lock(mu_);
            lastError_ = "select MPX_VIDEO_BACKEND=fpga-h264 or explicit legacy-software";
            return false;
        }
        if (backend_ == VideoBackend::LegacySoftware && presentMode_ != "none") {
            const char* matched = std::getenv("MPX_LEGACY_CORE_PREFIX");
            if (!matchedLegacyVideoCore(rbfPrefix8_, matched) || streamEnabled_) {
                std::lock_guard<std::mutex> lock(mu_);
                lastError_ = "legacy-software requires an explicitly matched OLD baseline core";
                return false;
            }
        }
        if (!fb_.ok() && !initPresent())
            return false;

        {
            std::lock_guard<std::mutex> lock(mu_);
            currentUrl_ = urlOrPath;
            currentHeaders_ = httpHeaders;
            durationMs_ = durationMs;
            lastError_.clear();
            spsRejectCode480p_.clear();
        }
        // Local / lab play-file: drop inherited PMS size unless this call already
        // probed the file (main --play-file sets sourceMedia via ffmpeg WxH).
        // Companion HTTP path calls setSourceMediaSize() after resolve, then
        // play(http) — not this branch.
        if (!fpgaH264Backend() && !urlOrPath.empty() && urlOrPath[0] == '/' &&
            urlOrPath.rfind("http", 0) != 0) {
            setSourceHasAudio(true);
            if (sourceMediaW_ <= 0 || sourceMediaH_ <= 0) {
                int sw = 0, sh = 0;
                std::string probeFail;
                (void)probeSourceAspect(urlOrPath, httpHeaders, &probeFail, &sw, &sh);
                if (sw > 0 && sh > 0)
                    setSourceMediaSize(sw, sh);
            }
        }

        coreId480i_ = 0;
        coreId240p_ = 0;
        if (fpgaH264Backend()) {
            const auto sel = selectCoreIdFromStream(
                sourceMediaW_, sourceMediaH_, sourceInterlaced_);
            coreId480i_ = sel.coreId480i;
            coreId240p_ = sel.coreId240p;
            const std::string selectedRbf =
                selectCoreRbfForMode(sourceMediaW_, sourceMediaH_);
            log("media: selectCoreRbfForMode " + selectedRbf +
                " w=" + std::to_string(sourceMediaW_) +
                " h=" + std::to_string(sourceMediaH_) +
                " interlaced=" + (sourceInterlaced_ ? "1" : "0"));
        }

        stop_.store(false);
        resetPlaybackPauseClock();
        ttfpOpenTimeUs_.store(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        ttfpFirstPacketTimeUs_.store(0);
        ttfpFirstDecodeTimeUs_.store(0);
        ttfpFirstPicTimeUs_.store(0);
        ttfpFirstPicMonotonicUs_.store(0);
        seekReqMs_.store(-1);
        reconFrames_.store(0);
        reconPresentOk_.store(false);
        cabacSkip_.store(false);
        last_ok_pts_ = INT64_MIN;
        {
            std::lock_guard<std::mutex> lock(summaryMu_);
            lastSummary_ = PlaybackSummary{};
        }
        {
            TtfpTimestamps prev{};
            {
                std::lock_guard<std::mutex> ttfpLock(ttfpMu_);
                if (ttfp_.startUs && !ttfp_.logged) {
                    ttfpTryComplete(ttfp_);
                    ttfp_.logged = true;
                    prev = ttfp_;
                }
                ttfp_ = TtfpTimestamps{};
                ttfp_.startUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            if (prev.startUs)
                log(formatTtfpLog(prev));
        }
        // Mark playing before thr_ starts so callers (e.g. lab --play-file) that
        // poll playing() cannot race stop() before threadMain runs and wipe the
        // session at frames=0 / audio_s=0.
        if (started)
            started();
        playing_.store(true);
        // Re-arm the latched Display row only if HDMI is not already on it.
        // force=true rewrites video_mode every cast and renegotiates HDMI
        // (user-visible res blink) while vsync is down — ddr_frame_store
        // cannot retire swap_pending, so glass stays on the chevron.
        fpga_.reprobeDdrKick();
        if (displayRasterLatched_)
            applyDisplayRaster(latchedDisplayRes_, /*force=*/kForceDisplayRasterOnPlay);
        showPlaybackOverlay(PlaybackOverlayState::Playing, startOffsetMs, durationMs);
        gate::GatePlayContext gateCtx;
        {
            std::lock_guard<std::mutex> lock(mu_);
            gateCtx = gateCtx_;
        }
        thr_ = std::thread([this, urlOrPath, startOffsetMs, httpHeaders, durationMs, epoch,
                            generation, gateCtx] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-play");
        // Dual-A9: pin the play/reader thread to CPU0. Isolated ffmpeg needs
        // ~1.8 cores for 32 fps; under play, read_us=53 ms is the limiter
        // (present waits ~38 ms). Do not nice the reader above ffmpeg.
        // In-process libav needs both cores (~1.78 / -threads 2) — do not pin.
#ifdef MPX_HAVE_LIBAV
        const bool skipPlayCpu0 =
            inprocDecodeWanted() && inprocDecodeSizeOk(outW_, outH_) &&
            presentMode_ == "fpga" &&
            ddrFrameFormat_ == DdrFrameFormat::Yuv420p;
        if (!skipPlayCpu0)
#endif
        {
            cpu_set_t cpus;
            CPU_ZERO(&cpus);
            CPU_SET(0, &cpus);
            (void)pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
            (void)::setpriority(PRIO_PROCESS, 0, 0);
        }
        if (outW_ == kPlex720pPresentedWidth && outH_ == kPlex720pPresentedHeight)
            setMisterNice(19, log_);
#endif
            try {
                int64_t offset = startOffsetMs;
                for (;;) {
                    boundedReopenPts_.store(-1);
                    const std::string playUrl = offset > 0
                        ? urlWithBitrateFloor480i(withSeekSafeTranscodeParams(urlOrPath, offset))
                        : urlOrPath;
                    if (fpgaH264Backend()) {
                        fpgaThreadMain(playUrl, offset, httpHeaders, durationMs, epoch,
                                       generation, gateCtx);
                    } else {
                        plxhPolicy_.playRequestEnded(generation);   // R1 C1: no PLXH session here
                        threadMain(playUrl, offset, httpHeaders, durationMs);
                    }
                    const int64_t next = boundedReopenPts_.exchange(-1);
                    if (next <= 0 || stop_.load())
                        break;
                    offset = next;
                    playing_.store(true);
                }
            } catch (const std::exception& ex) {
                log(std::string("media: threadMain exception: ") + ex.what());
                playing_.store(false);
            } catch (...) {
                log("media: threadMain unknown exception");
                playing_.store(false);
            }
        });
        {
            std::lock_guard<std::mutex> lock(mu_);
            startedPlayback_ = {generation, epoch};
        }
    }
    return true;
}

bool MediaPlayer::boundedReopen(uint64_t session) {
    (void)session;
    const int64_t ptsMs = positionMs_.load();
    if (ptsMs <= 0)
        return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (currentUrl_.empty())
        return false;
    if (boundedReopenRetryCount_ >= 3)
        return false;
    if (!stalledPicShouldReopen(true, boundedReopenRetryCount_, kRangeRetryMax))
        return false;
    ++boundedReopenRetryCount_;
    boundedReopenPts_.store(ptsMs);
    log("media: boundedReopen at " + std::to_string(ptsMs) +
        "ms (retry " + std::to_string(boundedReopenRetryCount_) + ")");
    return true;
}

void MediaPlayer::interruptInproc() {
#ifdef MPX_HAVE_LIBAV
    if (!fpgaH264Backend())
        return;
    std::lock_guard<std::mutex> lock(inprocMu_);
    if (inprocPcm_)
        inprocPcm_->requestStop();
#endif
}

void MediaPlayer::avSyncMeasure(int64_t videoContentUs, uint64_t consumedAtDisplay) {
    std::vector<av_sync::AvSyncEvent> events;
    av_sync::AvSyncConfig cfg;
    {
        std::lock_guard<std::mutex> lk(avSyncMu_);
        int64_t audioContentUs = 0;
        if (!avMap_.contentUs(consumedAtDisplay, audioContentUs))
            return;
        avSync_.addMeasurement(videoContentUs - audioContentUs, playbackActiveUs());
        events = avSync_.takeEvents();
        cfg = avSync_.config();
    }
    const unsigned row = avSyncRowId_.load();
    char buf[256];
    for (const av_sync::AvSyncEvent& e : events) {
        switch (e.kind) {
        case av_sync::AvSyncEvent::Baseline:
            std::snprintf(buf, sizeof(buf),
                          "media: av sync baseline %+.1f ms at the display boundary (row 0x%08X); "
                          "resync over %.0f ms held %.0f s",
                          e.offsetUs / 1000.0, row, cfg.thresholdUs / 1000.0, cfg.persistUs / 1e6);
            break;
        case av_sync::AvSyncEvent::ResyncStart:
            std::snprintf(buf, sizeof(buf),
                          "media: av resync start: offset %+.1f ms vs the reference (over %.0f ms "
                          "for %.0f s), audio pulled %s at %.1f%%",
                          e.offsetUs / 1000.0, cfg.thresholdUs / 1000.0, cfg.persistUs / 1e6,
                          e.offsetUs > 0 ? "earlier" : "later", cfg.resyncRate * 100.0);
            break;
        case av_sync::AvSyncEvent::ResyncDone:
            std::snprintf(buf, sizeof(buf),
                          "media: av resync offset before=%+.1f ms after=%+.1f ms (audio moved "
                          "%+.1f ms in %.1f s; row 0x%08X)",
                          e.beforeUs / 1000.0, e.afterUs / 1000.0, e.movedUs / 1000.0,
                          e.durationUs / 1e6, row);
            break;
        case av_sync::AvSyncEvent::UndoStart:
            std::snprintf(buf, sizeof(buf),
                          "media: av resync did not move the measured offset (now %+.1f ms): "
                          "putting the %+.1f ms back, then no more resyncs this play",
                          e.offsetUs / 1000.0, e.movedUs / 1000.0);
            break;
        case av_sync::AvSyncEvent::Disabled:
            std::snprintf(buf, sizeof(buf),
                          "media: av sync resync OFF for this play (offset %+.1f ms); the drift "
                          "compensation stays",
                          e.offsetUs / 1000.0);
            break;
        case av_sync::AvSyncEvent::Status:
            std::snprintf(buf, sizeof(buf),
                          "media: av sync offset=%+.1f ms vs reference, reference %+.1f ms from "
                          "the baseline, drift compensation %+.1f ppm",
                          e.offsetUs / 1000.0, e.referenceUs / 1000.0, e.ppm);
            break;
        }
        log(buf);
    }
}

void MediaPlayer::failFpgaSession(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        lastError_ = "FPGA H.264: " + error;
    }
    // A choke point may have just closed the core gate: say why before the failure (the
    // idle tick would log it within 200 ms; the lab --play-file path has no tick).
    for (const auto& line : CoreGate::process().takeLogs())
        log(line);
    fpga_.serviceWedgeRead("H-a-service", true);
    log("ERROR FPGA H.264: " + error);
    TtfpTimestamps ttfpSnap{};
    {
        std::lock_guard<std::mutex> ttfpLock(ttfpMu_);
        if (ttfp_.startUs && !ttfp_.logged) {
            ttfpTryComplete(ttfp_);
            ttfp_.logged = true;
            ttfpSnap = ttfp_;
        }
    }
    if (ttfpSnap.startUs)
        log(formatTtfpLog(ttfpSnap));
    noteGateRefuse();
    stop_.store(true);
}

void MediaPlayer::noteGateRefuse() {
    gateRefuse_.store(true, std::memory_order_release);
}

bool MediaPlayer::takeGateRefuse() {
    return gateRefuse_.exchange(false, std::memory_order_acq_rel);
}

void MediaPlayer::teardownAfterGateRefuse() {
    plxhPolicy_.endSession();
    playEpoch_.fetch_add(1, std::memory_order_acq_rel);
    std::string why;
    const bool cleared = r1EntryStreamClear(&why);
    const std::string line = std::string("media: gate-refuse teardown streamClear=") +
                             (cleared ? "1" : "0") + (why.empty() ? "" : " " + why);
    log(line);
    std::fprintf(stderr, "misterplexd: %s\n", line.c_str());
}

bool MediaPlayer::tryVideoDrain(uint64_t session) {
    // Conf VIDEO_DRAIN_ON=1: extra SPI drain at EOF. Default off keeps the
    // overnight presentation-drain loop (glass 045). PendingPicture is local
    // to fpgaThreadMain, so the wait lives at the call site.
    if (!videoDrainOn_)
        return true;
    std::lock_guard<std::mutex> control(pauseControlMu_);
    std::lock_guard<std::mutex> present(presentMu_);
    if (!fpga_.drainBitstreamSession(session, 2000)) {
        log("media: video drain NACK at EOF — pictures may cut off");
        return false;
    }
    return true;
}

bool MediaPlayer::controlFpgaAudio(uint64_t session, AudioSessionControl command) {
    return fpga_.controlAudioSession(session, command, 1000);
}

void MediaPlayer::requestGateFallback(uint64_t epoch, uint64_t generation, int64_t offsetMs, std::string reason,
                                      gate::GateRow row) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (playEpoch_.load() != epoch)
            return;   // a stop or a new play already took over this epoch
        gateFallbackPending_ = GateFallbackRequest{generation, offsetMs, std::move(reason), gate::rowName(row)};
        gateFallbackEpoch_ = epoch;
    }
    log(std::string("media: GATE fallback ") + gate::rowName(row) + " requested: 0 AUs written; the same item "
        "restarts " + (row == gate::GateRow::R6p ? "as one lowered PMS request" : "on the ladder helper") +
        " at offset_ms=" + std::to_string(offsetMs));
    stop_.store(true);
}

void MediaPlayer::setGateError(const std::string& text, uint64_t generation, uint64_t epoch) {
    // review 08:18: the on-screen gate error ships ON. MPX_GATE_ERROR_SCREEN=off (conf or env) is its kill switch: the
    // play still stops with the same text (lastError_, the ERROR line, the companion); nothing is latched or painted.
    if (const char* screen = std::getenv("MPX_GATE_ERROR_SCREEN"); screen && std::string(screen) == "off") {
        log("media: GATE error screen off (MPX_GATE_ERROR_SCREEN=off), not painted: " + text);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gateErrMu_);
        // A play a stop or a newer PLAY request has replaced must not latch (review fix: a cancelled open, or an
        // R7/R8 of the old worker after the new request's clear, left a stale error).
        if (playEpoch_.load() != epoch || (generation != 0 && gateLatestRequestGen_ > generation)) {
            log("media: GATE error NOT latched (this play was replaced: epoch " + std::to_string(epoch) + " now " +
                std::to_string(playEpoch_.load()) + ", gen " + std::to_string(generation) + " latest " +
                std::to_string(gateLatestRequestGen_) + "): " + text);
            return;
        }
        gateErrorText_ = text;
    }
    log("media: GATE error latched (on screen until a user stop, a new play, a core change or a CAPS re-read): " +
        text);
}

std::string MediaPlayer::gateErrorText() const {
    std::lock_guard<std::mutex> lock(gateErrMu_);
    return gateErrorText_;
}

void MediaPlayer::noteGatePlayRequest(uint64_t gen) {
    bool cleared = false;
    {
        std::lock_guard<std::mutex> lock(gateErrMu_);
        if (gen > gateLatestRequestGen_)
            gateLatestRequestGen_ = gen;
        cleared = !gateErrorText_.empty();
        gateErrorText_.clear();
    }
    if (cleared) {
        // The play's first presented frame drops this; a play that ends before one gets the forced repaint.
        gateRepaintOnce_.store(true);
        log("media: GATE error latch cleared: play request gen " + std::to_string(gen));
    }
}

void MediaPlayer::clearGateError(const char* why, bool repaint) {
    {
        std::lock_guard<std::mutex> lock(gateErrMu_);
        if (gateErrorText_.empty())
            return;
        gateErrorText_.clear();
    }
    log(std::string("media: GATE error latch cleared: ") + why);
    // The next idle paint replaces the error even in IDLE_SCREEN=last-frame (painted black there, review fix): the
    // core-change clear's own tick repaint, or this call's repaint.
    gateRepaintOnce_.store(true);
    if (repaint && !playing_.load())
        paintIdle();
}

void MediaPlayer::fpgaAudioPump(MPX_AV_TRACE(std::shared_ptr<FpgaAvTrace> avTrace)) {
#ifdef MPX_HAVE_LIBAV
    MPX_AV_TRACE(FpgaAvTrace::Writer traceWriter(avTrace.get(), FpgaAvLane::Audio);)
    FpgaAudioExit exit = FpgaAudioExit::Running;
    fpgaAudioExit_.store(exit);
    auto audioFailure = [&](const std::string& error) {
        exit = FpgaAudioExit::Error;
        fpgaAudioExit_.store(exit);
        failFpgaSession(error);
    };
    AvInprocDecoder* demux = nullptr;
    {
        std::lock_guard<std::mutex> lock(inprocMu_);
        demux = inprocPcm_;
    }
    int output = fpgaAudioOutput_;
    // avsync45: the negotiated row's audio trim, and on avResync rows the PCM goes through the
    // servo's resampler (av_sync.hpp). Every other row writes the decoded PCM as before.
    const FpgaBuildInfo* avRow = fpga_.negotiatedBuild();
    const bool avNudge = avRow && avRow->avResync;
    av_sync::PcmRateNudger nudger;
    std::vector<uint8_t> nudged;
    int64_t written = 0;
    bool aligned = false;
    int64_t trimBytes = 0;
    int64_t audioAnchorPausedUs = 0;
    auto due = std::chrono::steady_clock::now();
    MPX_AV_TRACE(auto traceWait = [&](FpgaAvWait reason, int64_t queued = -1,
                                     int64_t dueUs = -1, int64_t lateUs = 0) {
        AvTraceSpan span(traceWriter, FpgaAvEvent::AudioWait, tracePauseClock_);
        span.value(0, static_cast<int>(reason));
        if (queued >= 0) span.value(1, queued);
        if (dueUs >= 0) { span.value(2, dueUs); span.value(3, lateUs); }
        span.uvalue(4, fpgaAudioHeldBytes_.load());
        span.value(5, written);
        span.value(6, paused_.load());
        span.value(7, stop_.load());
        span.value(8, fpgaVideoStarted_.load());
    };)
    auto readClock = [&](int64_t& queued) -> bool {
        FpgaSpi::AudioSessionStatus status;
        MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::AudioClock, tracePauseClock_);
                     AvTraceRelease release{span};)
        // An older DMA read must not overwrite a newer combined presentation clock.
        std::lock_guard<std::mutex> present(presentMu_);
        MPX_AV_TRACE(span.acquired(); span.serviceBegin();)
        const bool clockRead = fpga_.readAudioSessionStatus(fpgaSession_.load(), status);
        MPX_AV_TRACE(span.serviceEnd();
            traceMast(span, clockRead, status, written, fpgaAudioHeldBytes_.load(),
                      paused_.load(), false);)
        if (!clockRead)
            return false;
        // Hard-fail only on FPGA error flag or impossible consumed>submitted.
        // Transient mast_active=0 (start/pause/edge) previously CLOSED-STOP after
        // had_presentation=1 on 882b castfix; keep picture alive and skip clock.
        if (status.error ||
            status.samples_consumed > static_cast<uint64_t>(written / 4)) {
            audioFailure("audio DMA epoch/error/consumed counter does not match submitted PCM");
            return false;
        }
        if (!status.active)
            return false;
        const int64_t consumed = static_cast<int64_t>(status.samples_consumed);
        queued = written - consumed * 4;
        fpgaHasConsumedClock_.store(true);
        if (fpgaAudioConsumedSamples_.exchange(consumed) != consumed)
            fpgaAudioClockChangedUs_.store(playbackActiveUs());
        audioQueuedBytes_.store(queued);
        return true;
    };
    uint8_t pcm[3840];
    // Hold decoded PCM in userspace until first picture so the aligned
    // write/Resume path bursts kFeedTargetBytes into MrAudio (no underrun
    // at start, no write before the A/V anchor).
    std::vector<uint8_t> prefillBuf;
    const int64_t prefillBytes = misterplex::audioPrefillBudgetBytes(audioPrefillMs_);
    while (demux && !stop_.load() && !fpgaAudioJoin_.load()) {
        if (paused_.load() || !fpgaVideoStarted_.load()) {
            if (!paused_.load() && prefillBytes > 0 &&
                static_cast<int64_t>(prefillBuf.size()) < prefillBytes) {
                const int n = demux->drainPcm(pcm, sizeof(pcm), false);
                if (n > 0) {
                    prefillBuf.insert(prefillBuf.end(), pcm, pcm + n);
                    if (fpgaAvStartGateBank(outW_, outH_))
                        audioReadyForStart_.store(true);
                } else {
                    std::string error;
                    AvAudioProgress progress;
                    try {
                        fpgaAudioProgress_.store(-2);
                        progress = demux->advanceCompressedAudio(error);
                    } catch (...) {
                        audioFailure("compressed audio progress exception");
                        break;
                    }
                    fpgaAudioProgress_.store(static_cast<int>(progress));
                    if (progress == AvAudioProgress::Error) {
                        audioFailure(error);
                        break;
                    }
                    if (progress == AvAudioProgress::Cancelled) {
                        exit = FpgaAudioExit::Cancelled;
                        break;
                    }
                }
            } else {
                MPX_AV_TRACE(traceWait(paused_.load() ? FpgaAvWait::Paused : FpgaAvWait::VideoNotStarted);)
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        int count;
        if (!prefillBuf.empty()) {
            count = static_cast<int>(std::min(sizeof(pcm), prefillBuf.size()));
            std::memcpy(pcm, prefillBuf.data(), static_cast<size_t>(count));
            prefillBuf.erase(prefillBuf.begin(),
                             prefillBuf.begin() + static_cast<std::ptrdiff_t>(count));
        } else {
            MPX_AV_TRACE({
                AvTraceSpan span(traceWriter, FpgaAvEvent::AudioDrain, tracePauseClock_);
                const auto pressure = demux->compressedPressure();
                span.serviceBegin();)
            count = demux->drainPcm(pcm, sizeof(pcm), false);
            MPX_AV_TRACE(span.serviceEnd(); span.value(0, count); span.value(1, sizeof(pcm));
                tracePressure(span, pressure, demux->compressedPressure());
                span.value(10, written); span.value(11, paused_.load());
            })
        }
        fpgaAudioHeldBytes_.store(count);
        if (!count) {
            bool atEof;
            MPX_AV_TRACE({
                AvTraceSpan span(traceWriter, FpgaAvEvent::AudioEof, tracePauseClock_);
                span.serviceBegin();)
            atEof = demux->audioEof();
            MPX_AV_TRACE(span.serviceEnd(); span.value(0, atEof); })
            if (atEof) { exit = FpgaAudioExit::Eof; break; }
            MPX_AV_TRACE(traceWait(FpgaAvWait::PcmEmpty);)
            std::string error;
            AvAudioProgress progress;
            try {
                MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::AudioAdvance, tracePauseClock_);
                    const auto pressure = demux->compressedPressure();)
                fpgaAudioProgress_.store(-2);
                MPX_AV_TRACE(span.serviceBegin();)
                progress = demux->advanceCompressedAudio(error);
                MPX_AV_TRACE(span.serviceEnd(); span.value(0, static_cast<int>(progress));
                    tracePressure(span, pressure, demux->compressedPressure());
                    if (progress == AvAudioProgress::Error) span.force();)
            } catch (...) {
                audioFailure("compressed audio progress exception");
                break;
            }
            fpgaAudioProgress_.store(static_cast<int>(progress));
            if (progress == AvAudioProgress::Error) {
                audioFailure(error);
                break;
            }
            if (progress == AvAudioProgress::Cancelled) {
                exit = FpgaAudioExit::Cancelled;
                break;
            }
            if (progress != AvAudioProgress::Ready)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (!aligned) {
            int64_t audioPts = demux->firstAudioPtsUs();
            if (audioPts == ddr_bitstream_ring::kNoTimestamp) {
                audioFailure("decoded audio has no original timestamp");
                break;
            }
            const int64_t videoPts = fpgaFirstVideoPtsUs_.load();
            if (audioPtsBindEnabled_ && fpgaAudioPtsBindBank(outW_, outH_) &&
                videoPts != ddr_bitstream_ring::kNoTimestamp) {
                log("media: fpga audio PTS bind: audio_pts=" + std::to_string(audioPts) +
                    " -> video_pts=" + std::to_string(videoPts) + " bank=" +
                    std::to_string(outW_) + "x" + std::to_string(outH_));
                audioPts = videoPts;
            }
            const long double timestampDelta =
                static_cast<long double>(audioPts) - videoPts;
            if (timestampDelta < -2000000 || timestampDelta > 2000000) {
                audioFailure("initial audio/video timestamp gap exceeds two seconds");
                break;
            }
            const int64_t delta = static_cast<int64_t>(timestampDelta);
            // AUDIO_DELAY_MS is ignored by the ffmpeg adelay path on FPGA
            // publish. Hold the first MrAudio write so the whole paced stream
            // starts later. 0 keeps the previous alignment.
            const int delayMs = fpgaAudioDelayMs(audioDelayMs_, avRow);
            const int64_t delayUs = static_cast<int64_t>(delayMs) * 1000;
            fpgaAudioStartRelativeUs_.store(std::max<int64_t>(0, delta) + delayUs);
            trimBytes = delta < 0 ? ((-delta * 48000) / 1000000) * 4 : 0;
            const int64_t anchorVideoUs = fpgaVideoStartMonotonicUs_.load();
            due = std::chrono::steady_clock::time_point(
                std::chrono::microseconds(anchorVideoUs + std::max<int64_t>(0, delta) + delayUs));
            {
                // N1: the first MrAudio write's anchor, once per play, next to the video one.
                const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const int64_t dueUs = anchorVideoUs + std::max<int64_t>(0, delta) + delayUs;
                log("media: av anchor audio: first write due mono_us=" + std::to_string(dueUs) +
                    " = video anchor " + std::to_string(anchorVideoUs) + " + delta " +
                    std::to_string(std::max<int64_t>(0, delta)) + " us (audio pts " +
                    std::to_string(audioPts) + " - video pts " +
                    std::to_string(fpgaFirstVideoPtsUs_.load()) + ") + delay " +
                    std::to_string(delayUs) + " us; trim " + std::to_string(trimBytes) +
                    " bytes; computed " + std::to_string(nowUs - anchorVideoUs) +
                    " us after the video anchor (" +
                    (nowUs > dueUs ? "late by " + std::to_string(nowUs - dueUs) + " us"
                                   : "on time") + ")");
            }
            audioAnchorPausedUs = fpgaVideoStartPauseUs_.load();
            aligned = true;
            if (avRow && avRow->avTrimMs != 0) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "media: fpga_audio_delay_ms=%d row 0x%08X trim %+d ms: effective %d ms",
                              audioDelayMs_, avRow->id, avRow->avTrimMs, delayMs);
                log(buf);
            } else {
                log("media: fpga_audio_delay_ms=" + std::to_string(audioDelayMs_));
            }
            if (avNudge) {
                av_sync::AvSyncConfig cfg;
                {
                    std::lock_guard<std::mutex> lk(avSyncMu_);
                    avSync_.reset();
                    avSync_.setBaseRatio(1.0 + avRow->avDriftPpm * 1e-6);
                    avMap_.reset(std::max<int64_t>(0, delta));
                    cfg = avSync_.config();
                }
                avSyncRowId_.store(avRow->id);
                avSyncOn_.store(true);
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "media: av sync on (row 0x%08X): drift compensation %+.1f ppm, "
                              "resync over %.0f ms held %.0f s at %.1f%%",
                              avRow->id, avRow->avDriftPpm, cfg.thresholdUs / 1000.0,
                              cfg.persistUs / 1e6, cfg.resyncRate * 100.0);
                log(buf);
            }
        }
        const size_t skipped = static_cast<size_t>(std::min<int64_t>(trimBytes, count));
        trimBytes -= skipped;
        if (skipped == static_cast<size_t>(count)) continue;
        if (output < 0) {
            // open() leaves the kernel cursor unchanged; only write() publishes PCM/metadata.
            MPX_AV_TRACE({
                AvTraceSpan span(traceWriter, FpgaAvEvent::AudioOpen, tracePauseClock_);
                span.serviceBegin();)
            output = ::open(audioDev_.c_str(), O_WRONLY | O_NONBLOCK);
            MPX_AV_TRACE(span.serviceEnd(); span.value(0, output);
                span.value(1, output < 0 ? errno : 0); span.force(); })
            if (output < 0) {
                audioFailure("MrAudio open failed (no silent audio fallback)");
                break;
            }
            fpgaAudioOutput_ = output;
        }
        const uint8_t* src = pcm;
        size_t srcCount = static_cast<size_t>(count);
        size_t offset = skipped;
        if (avNudge) {
            double ratio = 1.0;
            {
                std::lock_guard<std::mutex> lk(avSyncMu_);
                ratio = avSync_.targetRatio();
            }
            nudged.clear();
            nudger.process(pcm + skipped, static_cast<size_t>(count) - skipped, ratio, nudged);
            {
                std::lock_guard<std::mutex> lk(avSyncMu_);
                avMap_.add(nudger.outputFrames(), nudger.inputPosition());
                avSync_.noteApplied(nudger.appliedUs(), playbackActiveUs());
            }
            if (nudged.empty())
                continue;  // fewer than three frames so far: the resampler holds them
            src = nudged.data();
            srcCount = nudged.size();
            offset = 0;
        }
        fpgaAudioHeldBytes_.store(srcCount - offset);
        const int64_t deadline = playbackActiveUs() + 2000000;
        while (offset < srcCount && !stop_.load()) {
            if (paused_.load()) {
                MPX_AV_TRACE(traceWait(FpgaAvWait::Paused);)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (!coreGateAllowsAudioWrite()) {
                audioFailure(coreGateClosedError("MrAudio write"));
                break;
            }
            if (playbackActiveUs() >= deadline) {
                audioFailure("PCM DMA clock/backpressure timeout");
                break;
            }
            int64_t queued = 0;
            if (!readClock(queued)) {
                MPX_AV_TRACE(traceWait(FpgaAvWait::ClockUnavailable);)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto audibleDue = due + std::chrono::microseconds(
                playbackPausedUs(now) - audioAnchorPausedUs);
            const int64_t audioCeil = audioLeadClamp240p(queued, outW_, outH_);
            if (queued > audioCeil || now < audibleDue) {
                MPX_AV_TRACE(const auto dueUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(audibleDue.time_since_epoch()).count();
                    const auto nowUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
                    traceWait(queued > kMrAudioBytesPerSec / 5 ? FpgaAvWait::QueueHigh :
                              FpgaAvWait::AudibleDue, queued, dueUs, nowUs - dueUs);)
                // 240p-only: drop this PCM chunk when DMA queued bytes exceed 120 ms.
                if (audioQueueBound240p(outW_, outH_, queued)) {
                    offset = srcCount;
                    continue;
                }
                // 240p/480p/480i: drop this PCM chunk instead of sleeping until the
                // 2 s backpressure timeout kills the session.
                if (queued > kMrAudioBytesPerSec / 5 && fpgaAvStartGateBank(outW_, outH_)) {
                    offset = srcCount;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            MPX_AV_TRACE(AvTraceSpan writeSpan(traceWriter, FpgaAvEvent::AudioWrite, tracePauseClock_);
                AvTraceRelease writeRelease{writeSpan};
                writeSpan.value(1, srcCount - offset); writeSpan.value(2, written);
                writeSpan.value(3, queued);
                const auto dueUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(audibleDue.time_since_epoch()).count();
                writeSpan.value(4, dueUs);
                writeSpan.value(5, std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch()).count() - dueUs);)
            std::lock_guard<std::mutex> control(pauseControlMu_);
            MPX_AV_TRACE(writeSpan.acquired(); writeSpan.value(9, paused_.load());
                writeSpan.value(10, stop_.load());)
            if (paused_.load() || stop_.load()) continue;
            MPX_AV_TRACE(writeSpan.serviceBegin();)
            const ssize_t accepted = ::write(output, src + offset, srcCount - offset);
            MPX_AV_TRACE(writeSpan.serviceEnd(); writeSpan.value(0, accepted);
                writeSpan.value(6, accepted < 0 ? errno : 0);
                if (accepted <= 0) writeSpan.force();)
            if (accepted < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            if (accepted <= 0) {
                audioFailure("MrAudio write failed");
                break;
            }
            offset += static_cast<size_t>(accepted);
            fpgaAudioHeldBytes_.store(srcCount - offset);
            written += accepted;
            audioBytes_.store(written);
            MPX_AV_TRACE(writeSpan.value(7, written);
                writeSpan.value(8, srcCount - offset);)
            if (!fpgaAudioStarted_.load()) {
                std::lock_guard<std::mutex> present(presentMu_);
                if (!controlFpgaAudio(fpgaSession_.load(), AudioSessionControl::Resume)) {
                    audioFailure("primed audio DMA Resume ACK failed");
                    break;
                }
                fpgaAudioStarted_.store(true);
            }
            audioActive_.store(true);
            due += std::chrono::microseconds(accepted * 1000000LL / kMrAudioBytesPerSec);
        }
    }
    if (output >= 0 && !stop_.load()) {
        if (exit == FpgaAudioExit::Eof) fpgaAudioExit_.store(FpgaAudioExit::EofDraining);
        const int64_t deadline = playbackActiveUs() + 2000000;
        while (!stop_.load()) {
            int64_t queued = 0;
            if (readClock(queued) && queued == 0) break;
            if (!paused_.load() && playbackActiveUs() >= deadline) {
                audioFailure("audio EOF drain timeout");
                break;
            }
            MPX_AV_TRACE(traceWait(FpgaAvWait::EofDrain);)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (exit != FpgaAudioExit::Error && stop_.load()) exit = FpgaAudioExit::Cancelled;
    if (!demux && exit == FpgaAudioExit::Running) exit = FpgaAudioExit::NoSource;
    MPX_AV_TRACE({
        AvTraceSpan span(traceWriter, FpgaAvEvent::AudioExit, tracePauseClock_);
        span.value(0, static_cast<int>(exit)); span.value(1, written);
        span.uvalue(2, fpgaAudioHeldBytes_.load()); span.value(3, stop_.load());
        span.force();
    })
    fpgaAudioExit_.store(exit);
#endif
    audioActive_.store(false);
    fpgaAudioDone_.store(true);
    fpgaAudioStarted_.store(false);
}

void MediaPlayer::fpgaThreadMain(std::string url, int64_t startMs, std::string headers,
                                 int64_t durationMs, uint64_t epoch, uint64_t generation,
                                 gate::GatePlayContext gateCtx) {
    // R1 (review 19:16 (1)(c)): no PLXH en=1 from here (before BEGIN) to this session's END.
    // The guard ends the session after the END/abort below has run, when the function returns;
    // endSession logs the review 20:08 (c) line when the session ran without the en=0 ACK. The enter
    // takes over the PLAY-request hold of this play generation (review 07:30 C1), not a newer one.
    plxhPolicy_.setLog([this](const std::string& s) { log(s); });
    plxhPolicy_.enterSession(generation);
    struct PlxhSessionEnd {
        r1::PlxhSessionPolicy& policy;
        ~PlxhSessionEnd() { policy.endSession(); }
    } plxhSessionEnd{plxhPolicy_};
    // R1 entry clear (review 20:18 (3)): an idle-entry stream clear that passed its session
    // check before enterSession above finishes before this session touches the ring. The entry
    // ends with this session (one clear per entry).
    { std::lock_guard<std::mutex> clearBarrier(r1ClearMu_); }
    struct R1EntryEnd {
        std::atomic<uint64_t>& ends;
        ~R1EntryEnd() { ends.fetch_add(1); }
    } r1EntryEnd{r1SessionEnds_};
    PlaybackTerminalState terminal = PlaybackTerminalState::Stopped;
    positionMs_.store(startMs);
    lastPosMs_.store(startMs);
    fpgaVideoStarted_.store(false);
    fpgaAudioDone_.store(true);
    fpgaAudioStarted_.store(false);
    audioReadyForStart_.store(false);
    fpgaAudioExit_.store(FpgaAudioExit::NotStarted);
    fpgaAudioProgress_.store(-1);
    fpgaAudioHeldBytes_.store(0);
    fpgaAudioConsumedSamples_.store(-1);
    fpgaAudioClockChangedUs_.store(playbackActiveUs());
    fpgaHasConsumedClock_.store(false);
    avSyncOn_.store(false);
    fpgaFirstVideoPtsUs_.store(ddr_bitstream_ring::kNoTimestamp);
    ptsWrap_.reset();
    picStallLogged_ = false;
    picStallCount_ = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        range_retry_ = 0;
        last_good_offset_ = 0;
    }
    audioBytes_.store(0);
    presentCount_.store(0);
#ifndef MPX_HAVE_LIBAV
    (void)url; (void)headers; (void)epoch; (void)gateCtx;
    failFpgaSession("binary lacks libav demux/audio; software video fallback forbidden");
    terminal = finishFpgaPlayback({}, [] { return std::string("demux=unavailable audio=unavailable mast=unavailable"); },
        [] {}, [] { return true; }, [&] { return playEpoch_.load() == epoch; },
        [&] { return stop_.load(); },
        [&](const FpgaTerminalReceipt& receipt) { log(formatFpgaTerminal(receipt)); });
#else
    const uint64_t session =
        (static_cast<uint64_t>(::getpid()) << 48) ^
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ epoch;
    MPX_AV_TRACE(std::shared_ptr<FpgaAvTrace> avTrace;
        const int traceSavedErrno = errno;
        try { avTrace = std::make_shared<FpgaAvTrace>(epoch, session); } catch (...) {}
        errno = traceSavedErrno;
        FpgaAvTrace::Writer traceWriter(avTrace.get(), FpgaAvLane::Video);
        uint64_t traceReadCalls = 0;)
    bool begun = false, audioBegun = false, eof = false, fullyDrained = false;
    HighCabacHostTranscode hostCabac;
    bool hostCabacOn = false;
    bool hostBaselineOn = false;
    std::chrono::steady_clock::time_point hostCabacSince{};
    bool demuxOpened = false, drainAttempted = false;
    std::optional<bool> drainAck;
    std::optional<bool> releaseAudioReset, releaseFlush, releaseEnd, releaseAbort;
    bool resetAttempted = false, flushAttempted = false;
    bool endAttempted = false, abortAttempted = false;
    std::optional<int> demuxReturn;
    const char* demuxReturnKind = "not-called";
    std::string demuxError;
    uint32_t sequence = 0;
    uint64_t bytes = 0;
    AvInprocDecoder demux;
    auto current = [&] { return !stop_.load() && playEpoch_.load() == epoch; };
    struct PendingPicture {
        uint32_t sequence;
        int64_t pts;
        uint32_t num, den;
    };
    std::deque<PendingPicture> pending;
    std::thread overlayThread;
    std::atomic<bool> stopOverlay{false};
    uint64_t overlayNonce = 0;
    uint32_t overlaySequence = 0;
    auto startOverlay = [&](uint64_t nonce, uint32_t buildId) {
        overlayNonce = nonce;
        overlayThread = std::thread([&, nonce, buildId MPX_AV_TRACE(, avTrace)] {
            MPX_AV_TRACE(FpgaAvTrace::Writer overlayWriter(avTrace.get(), FpgaAvLane::Overlay);)
            unsigned failedTransfers = 0;
            try {
                while (current() && !stopOverlay.load()) {
                    // AutoFit measures native DE in FPGA; never substitute decoded/HDMI geometry.
                    fpga_overlay::Model model;
                    const bool presented = fpgaVideoStarted_.load();
                    model.state = paused_.load() ? fpga_overlay::State::Paused :
                        presented ? fpga_overlay::State::Playing : fpga_overlay::State::Buffering;
                    model.position_ms = presented ? positionMs_.load() : 0;
                    model.duration_ms = durationMs;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        model.title = playbackTitle_.empty() ? "MiSTerPlex" : playbackTitle_;
                        if (std::chrono::steady_clock::now() < fpgaOverlayTransportUntil_)
                            model.transport = fpgaOverlayTransport_;
                    }
                    if (presented && !paused_.load() && !overlay_.visible()) {
                        model.state = fpga_overlay::State::Hidden;
                        model.flags = 0;
                    }
                    bool sent = false;
                    {
                        MPX_AV_TRACE(AvTraceSpan span(overlayWriter, FpgaAvEvent::OverlaySend, tracePauseClock_);
                            AvTraceRelease release{span};
                            span.value(1, static_cast<int>(model.state));
                            span.uvalue(2, overlaySequence);
                            span.value(3, model.position_ms);
                            span.value(4, paused_.load());)
                        std::lock_guard<std::mutex> present(presentMu_);
                        MPX_AV_TRACE(span.acquired();)
                        if (!current() || stopOverlay.load()) break;
                        MPX_AV_TRACE(span.serviceBegin();)
                        sent = fpga_overlay::send(fpga_, session, nonce, overlaySequence, model);
                        MPX_AV_TRACE(span.serviceEnd(); span.value(0, sent);
                            if (!sent) span.force();)
                    }
                    // Lean 480p has no overlay plane. Composite the same renderer into
                    // the host present banks (not the H.264 DPB). A missing SPI ACK
                    // on that core is not a session failure.
                    int overlaySleepMs = 200;
                    if (overlayPlaneAbsent(buildId)) {
                        failedTransfers = 0;
                        if (overlay_.visible()) {
                            overlay_.setProgress(presented ? positionMs_.load() : 0, durationMs);
                            const DdrFrameLayout layout = fpga_.ddrFrameLayout();
                            if (layout.format == DdrFrameFormat::Yuv420p &&
                                layout.coded_width > 0 && layout.coded_height > 0 &&
                                layout.line_bytes == layout.coded_width &&
                                (layout.coded_width & 1) == 0 && (layout.coded_height & 1) == 0) {
                                for (int bank = 0; bank < 2; ++bank) {
                                    if (uint8_t* pixels = fpga_.ddrBankVirt(bank))
                                        overlay_.renderI420(pixels, layout.coded_width,
                                                            layout.coded_height);
                                }
                            }
                            overlaySleepMs = 40;
                        }
                    } else if (sent) {
                        failedTransfers = 0;
                    } else if (++failedTransfers >= 5) {
                        failFpgaSession("native overlay SPI transport failed repeatedly");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(overlaySleepMs));
                }
            } catch (...) {
                failFpgaSession("native overlay publisher failed");
            }
        });
    };
    // Lab overlay flags (overlay_publish.hpp). None set to exactly 1: the publisher above,
    // the prod code byte-for-byte, and nothing below runs; an invalid value fails closed
    // to it (overlayLabPublisherSelected). Any set to 1: the shared publisher
    // (runOverlayPublisher / OverlayPublisherThread) instead.
    // MPX_OVERLAY_DECOUPLE=1 (overlay_publish.hpp, rel-480i-costall-1004): the
    // publisher's SPI send runs outside presentMu_, and the DDR hot paths stop waiting
    // on the SPI lock for their error text. Unset: unchanged.
    const OverlayDecoupleSetting overlayDecoupleSetting = overlayDecoupleFromEnvironment();
    const bool overlayDecoupled = overlayDecoupleSetting == OverlayDecoupleSetting::On;
    if (const std::string notice = formatOverlayDecoupleNotice(
            overlayDecoupleSetting, std::getenv(kOverlayDecoupleEnv));
        !notice.empty())
        log(notice);
    FpgaSpi::setHotPathErrorsNonBlocking(overlayDecoupled);
    // MPX_OVERLAY_ONCHANGE=1 (overlay_publish.hpp): send only when the drawn content
    // changes (+1 confirm), a keepalive, forced and retry sends. Unset: every tick.
    const OverlayOnChangeSettings overlayOnChange = overlayOnChangeFromEnvironment();
    if (const std::string notice = formatOverlayOnChangeNotice(
            overlayOnChange, std::getenv(kOverlayOnChangeEnv), std::getenv(kOverlayKeepaliveEnv));
        !notice.empty())
        log(notice);
    // MPX_OVERLAY_SKIP_NO_PLANE=1 (overlay_publish.hpp): no publisher and no hide on a
    // core without the overlay plane (overlayPlaneAbsent), decided at the first AU.
    const OverlaySkipNoPlaneSetting overlaySkipNoPlane = overlaySkipNoPlaneFromEnvironment();
    // A lab overlay flag set to exactly 1: the publisher summary and session-end index-6
    // count lines, and the overlay trace's values 5/6. None (or only invalid values):
    // logging and trace unchanged.
    const bool overlayLabFlags =
        overlayLabPublisherSelected(overlayDecoupleSetting, overlayOnChange, overlaySkipNoPlane);
    std::atomic<uint64_t> overlaySessionSends{0};
    uint32_t overlayBuildId = 0;
    // The overlay lane's writer lives on the publisher thread, as before: started
    // emplaces it, done and quiesce finish it, since freeze() needs every producer gone.
    MPX_AV_TRACE(std::optional<FpgaAvTrace::Writer> overlayWriter;)
    // The publisher (overlay_publish.hpp runOverlayPublisher): finishFpgaPlayback's
    // quiesce stops and joins it before release hides the plane.
    OverlayPublisherThread overlayPublisher;
    auto startLabOverlay = [&](uint64_t nonce) {
        overlayNonce = nonce;
        OverlayPublisherHooks h;
        h.live = [&] { return current(); };
        h.inputs = [&] {
            OverlayInputs in;
            in.presented = fpgaVideoStarted_.load();
            in.paused = paused_.load();
            in.visible = overlay_.visible();
            in.positionMs = positionMs_.load();
            in.durationMs = durationMs;
            std::lock_guard<std::mutex> lock(mu_);
            in.title = playbackTitle_;
            if (std::chrono::steady_clock::now() < fpgaOverlayTransportUntil_)
                in.transport = fpgaOverlayTransport_;
            return in;
        };
        h.contentKey = [&, nonce](const fpga_overlay::Model& model) {
            return overlayContentKey(
                fpga_overlay::encode(session, nonce, overlaySequence + 1, model));
        };
        h.forceGen = [this] { return overlayForceGen_.load(); };
        h.nowUs = [] {
            return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
        h.publish = [&, nonce](OverlayPublishReason why, const fpga_overlay::Model& model) {
            (void)why;
            OverlayPublish published;
            MPX_AV_TRACE(AvTraceSpan span(*overlayWriter, FpgaAvEvent::OverlaySend, tracePauseClock_);
                AvTraceRelease release{span};
                span.value(1, static_cast<int>(model.state));
                span.uvalue(2, overlaySequence);
                span.value(3, model.position_ms);
                span.value(4, paused_.load());
                if (overlayLabFlags) {
                    span.value(5, overlayDecoupled);
                    span.value(6, static_cast<int>(why));
                })
            published = publishOverlay(
                overlayDecoupled, [&] { return std::unique_lock<std::mutex>(presentMu_); },
                [&] {
                    MPX_AV_TRACE(span.acquired();)
                    return current() && !overlayPublisher.stopping();
                },
                [&] {
                    MPX_AV_TRACE(span.serviceBegin();)
                    const bool ok =
                        fpga_overlay::send(fpga_, session, nonce, overlaySequence, model);
                    MPX_AV_TRACE(span.serviceEnd(); span.value(0, ok);
                        if (!ok) span.force();)
                    return ok;
                });
            return published;
        };
        h.tick = [] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); };
        h.counted = [&overlaySessionSends](bool) {
            overlaySessionSends.fetch_add(1, std::memory_order_relaxed);
        };
        h.skipped = [] {};
        h.failSession = [this](const char* why) { failFpgaSession(why); };
        overlayPublisher.start(
            std::move(h), OverlayPublishGate(overlayOnChange.on, overlayOnChange.keepaliveMs * 1000),
            [&] { MPX_AV_TRACE(overlayWriter.emplace(avTrace.get(), FpgaAvLane::Overlay);) },
            [&, overlayLabFlags](const OverlayPublishGate& gate, const OverlayPublishCounts& counts) {
                MPX_AV_TRACE(overlayWriter.reset();)
                if (overlayLabFlags)
                    log(formatOverlayPublishSummary(gate, counts));
            });
    };
    int64_t firstPtsUs = ddr_bitstream_ring::kNoTimestamp;
    int64_t firstSubmittedPtsUs = ddr_bitstream_ring::kNoTimestamp;
    int64_t anchorPausedUs = 0;
    int64_t lastCommitActiveUs = playbackActiveUs();
    int64_t heldPts = ddr_bitstream_ring::kNoTimestamp;
    uint32_t heldTimebaseNum = 0, heldTimebaseDen = 0;
    const char* waitReason = "opening";
    MPX_AV_TRACE(auto traceVideoWait = [&](FpgaAvWait reason) {
        AvTraceSpan span(traceWriter, FpgaAvEvent::VideoWait, tracePauseClock_);
        span.value(0, static_cast<int>(reason)); span.uvalue(1, pending.size());
        span.uvalue(2, sequence); span.uvalue(3, presentCount_.load());
        if (heldPts != ddr_bitstream_ring::kNoTimestamp) {
            span.value(4, heldPts); span.uvalue(5, heldTimebaseNum); span.uvalue(6, heldTimebaseDen);
        }
        span.value(7, paused_.load()); span.value(8, stop_.load());
    };)
    auto collectPresentation = [&]() -> bool {
        FpgaSpi::VideoPresentation picture;
        MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::VideoPresentation, tracePauseClock_);)
        {
            MPX_AV_TRACE(AvTraceRelease release{span};)
            std::lock_guard<std::mutex> present(presentMu_);
            MPX_AV_TRACE(span.acquired(); span.serviceBegin();)
            const bool pictureRead = fpga_.readVideoPresentation(session, picture);
            MPX_AV_TRACE(span.serviceEnd(); span.value(0, pictureRead);
                if (pictureRead) {
                    span.value(1, picture.active); span.value(2, picture.error);
                    span.value(3, picture.error_code); span.value(4, picture.has_frame);
                    span.uvalue(5, picture.presentation_count);
                    if (picture.has_frame) {
                        span.uvalue(6, picture.seq); span.value(7, picture.pts);
                        span.uvalue(8, picture.timebase_num); span.uvalue(9, picture.timebase_den);
                    }
                    if (picture.has_audio_clock) span.uvalue(12, picture.audio_samples_consumed);
                } else span.force();
                span.uvalue(10, pending.size()); span.uvalue(11, sequence);)
            MPX_AV_TRACE(span.value(13, paused_.load());)
            if (!pictureRead)
                return true;
            // D52-LOG (lab, MPX_D52_LOG=1): MVPS flag/error_code changes from this existing read; no-op when off.
            if (d52::Logger* d = d52::active())
                d->mvps(picture.session_id, picture.active, picture.has_frame, picture.error, picture.error_code,
                        picture.presentation_count, picture.seq);
            if (picture.error || !picture.active) {
                failFpgaSession("core presentation/epoch error " + std::to_string(picture.error_code));
                return false;
            }
            if (picture.has_audio_clock) {
                if (picture.audio_samples_consumed >
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 1000000)) {
                    failFpgaSession("consumed audio clock overflow");
                    return false;
                }
            }
            // MVPS freezes audio at the display boundary; pacing needs current MAST.
            if (audioBegun) {
                FpgaSpi::AudioSessionStatus audio;
                MPX_AV_TRACE(AvTraceSpan audioSpan(traceWriter, FpgaAvEvent::VideoClock, tracePauseClock_);
                    audioSpan.serviceBegin();)
                const bool audioRead = fpga_.readAudioSessionStatus(session, audio);
                MPX_AV_TRACE(audioSpan.serviceEnd();
                    traceMast(audioSpan, audioRead, audio, audioBytes_.load(),
                              fpgaAudioHeldBytes_.load(), paused_.load(), true);)
                if (audioRead) {
                    if (!audio.active || audio.error ||
                        audio.samples_consumed >
                            static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 1000000)) {
                        failFpgaSession("current DMA audio clock lost its active epoch");
                        return false;
                    }
                    fpgaHasConsumedClock_.store(true);
                    const auto consumed = static_cast<int64_t>(audio.samples_consumed);
                    if (fpgaAudioConsumedSamples_.exchange(consumed) != consumed)
                        fpgaAudioClockChangedUs_.store(playbackActiveUs());
                }
            }
            if (!picture.has_frame) return true;
        }
        if (picture.session_id != session)
            return true; // A stale mailbox publication never starts this epoch.
        // R17e31 lab: always surface PLXD identity so multi-present stalls can
        // distinguish HW count stuck vs ARM seq/pts reject.
        {
            static uint32_t lastLoggedCount = 0;
            static uint64_t lastLogUs = 0;
            const uint64_t nowUs = playbackActiveUs();
            const bool countChanged = picture.presentation_count != lastLoggedCount;
            if (countChanged || (nowUs - lastLogUs) > 250000) {
                lastLoggedCount = picture.presentation_count;
                lastLogUs = nowUs;
                log("media: PLXD has_frame=1 count=" + std::to_string(picture.presentation_count) +
                    " seq=" + std::to_string(picture.seq) +
                    " pts=" + std::to_string(picture.pts) +
                    " tb=" + std::to_string(picture.timebase_num) + "/" +
                    std::to_string(picture.timebase_den) +
                    " presentCount=" + std::to_string(presentCount_.load()) +
                    " pending=" + std::to_string(pending.size()));
            }
        }
        if (picture.presentation_count <= presentCount_.load())
            return true;
        auto it = std::find_if(pending.begin(), pending.end(), [&](const PendingPicture& p) {
            return p.sequence == picture.seq;
        });
        if (it == pending.end()) {
            failFpgaSession("new presentation count references an unsubmitted AU count=" +
                std::to_string(picture.presentation_count) + " seq=" + std::to_string(picture.seq) +
                " pending=" + std::to_string(pending.size()));
            return false;
        }
        if (picture.pts != it->pts || picture.timebase_num != it->num ||
            picture.timebase_den != it->den || !picture.timebase_den ||
            picture.presentation_count !=
                presentCount_.load() + std::distance(pending.begin(), it) + 1) {
            failFpgaSession("presentation ACK does not match queued AU timestamp count=" +
                std::to_string(picture.presentation_count) + " seq=" + std::to_string(picture.seq) +
                " pts=" + std::to_string(picture.pts) + " want_pts=" + std::to_string(it->pts) +
                " presentCount=" + std::to_string(presentCount_.load()) +
                " dist=" + std::to_string(std::distance(pending.begin(), it)));
            return false;
        }
        const int64_t clockPts = ptsWrap_.apply(picture.pts);
        const int64_t us = static_cast<int64_t>(
            static_cast<long double>(clockPts) * picture.timebase_num * 1000000 /
            picture.timebase_den);
        const bool firstPresentation = firstPtsUs == ddr_bitstream_ring::kNoTimestamp;
        if (firstPresentation) {
            firstPtsUs = us;
            fpgaFirstVideoPtsUs_.store(us);
            const int64_t anchorMonoUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            ttfpFirstPicTimeUs_.store(anchorMonoUs);
            fpgaVideoStartMonotonicUs_.store(anchorMonoUs);
            ttfpFirstPicMonotonicUs_.store(anchorMonoUs);
            anchorPausedUs = playbackPausedUs(std::chrono::steady_clock::now());
            fpgaVideoStartPauseUs_.store(anchorPausedUs);
            // N1 (RESOAK2, review 14:34): the ACK the A/V anchor took, once per play. An
            // ACK covering more than one picture, or a late one, moves the anchor a picture.
            log("media: av anchor video: first ACK count=" +
                std::to_string(picture.presentation_count) + " seq=" + std::to_string(picture.seq) +
                " pts=" + std::to_string(picture.pts) + " tb=" +
                std::to_string(picture.timebase_num) + "/" + std::to_string(picture.timebase_den) +
                " content_us=" + std::to_string(us) + " pictures_acked=" +
                std::to_string(std::distance(pending.begin(), it) + 1) + " pending=" +
                std::to_string(pending.size()) + " mono_us=" + std::to_string(anchorMonoUs));
            const int64_t openUs = ttfpOpenTimeUs_.load();
            const int64_t pktUs = ttfpFirstPacketTimeUs_.load();
            const int64_t decUs = ttfpFirstDecodeTimeUs_.load();
            const int64_t picUs = ttfpFirstPicTimeUs_.load();
            const int64_t picMonoUs = ttfpFirstPicMonotonicUs_.load();
            if (openUs != 0 && pktUs != 0 && decUs != 0 && picUs != 0) {
                log("media: ttfp-breakdown open_us=" + std::to_string(openUs) +
                    " pkt_us=" + std::to_string(pktUs) + " dec_us=" + std::to_string(decUs) +
                    " pic_us=" + std::to_string(picUs) + " pic_mono_us=" +
                    std::to_string(picMonoUs) +
                    " t_open_to_pkt_us=" + std::to_string(pktUs - openUs) +
                    " t_pkt_to_dec_us=" + std::to_string(decUs - pktUs) +
                    " t_dec_to_pic_us=" + std::to_string(picUs - decUs) +
                    " t_open_to_pic_us=" + std::to_string(picUs - openUs));
            }
        }
        presentCount_.fetch_add(std::distance(pending.begin(), it) + 1);
        {
            std::lock_guard<std::mutex> lock(summaryMu_);
            lastSummary_.lastPresentedAudioSamples = picture.has_audio_clock
                ? static_cast<int64_t>(picture.audio_samples_consumed) : -1;
        }
        pending.erase(pending.begin(), std::next(it));
        lastCommitActiveUs = playbackActiveUs();
        notePicDelivery();
        fpgaVideoStarted_.store(true);
        TtfpTimestamps ttfpSnap{};
        bool ttfpLogNow = false;
        {
            std::lock_guard<std::mutex> ttfpLock(ttfpMu_);
            if (ttfp_.firstPresentedUs == 0) {
                ttfp_.firstPresentedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                ttfpTryComplete(ttfp_);
                if (ttfp_.complete && !ttfp_.logged) {
                    ttfp_.logged = true;
                    ttfpSnap = ttfp_;
                    ttfpLogNow = true;
                }
            }
        }
        if (ttfpLogNow)
            log(formatTtfpLog(ttfpSnap));
        if (ttfpLogNow && hdmiReprobeAfterTtfp_ && displayRasterLatched_) {
            hdmiReprobeAfterTtfp_ = false;
            applyDisplayRaster(latchedDisplayRes_, /*force=*/true);
            std::fprintf(stderr, "misterplexd: utility 480p HDMI reprobe after TTFP\n");
        }
        gateRepaintOnce_.store(false);   // SPS-gate L3: video replaced a cleared gate error; LastFrame keeps the video
        positionMs_.store(startMs + std::max<int64_t>(0, (us - firstSubmittedPtsUs) / 1000));
        lastPosMs_.store(startMs + std::max<int64_t>(0, (us - firstSubmittedPtsUs) / 1000));
        if (const int64_t pos = positionMs_.load(); pos > 0)
            lastKnownPositionMs_.store(pos);
        // avsync45: this frame's A/V offset at its display pickup (the core froze the audio
        // DMA's consumed count there), for the servo on avResync rows.
        if (avSyncOn_.load() && picture.has_audio_clock && fpgaAudioStarted_.load() &&
            !paused_.load())
            avSyncMeasure(us - firstPtsUs, picture.audio_samples_consumed);
        MPX_AV_TRACE({
            AvTraceSpan commit(traceWriter, FpgaAvEvent::VideoCommit, tracePauseClock_);
            commit.uvalue(0, picture.presentation_count); commit.uvalue(1, picture.seq);
            commit.value(2, picture.pts); commit.uvalue(3, picture.timebase_num);
            commit.uvalue(4, picture.timebase_den); commit.uvalue(5, pending.size());
            commit.value(6, positionMs_.load()); commit.value(7, paused_.load());
        })
        if (firstPresentation)
            showPlaybackOverlay(paused_.load() ? PlaybackOverlayState::Paused :
                                PlaybackOverlayState::Playing, positionMs_.load(), durationMs);
        if (current())
            companionTimelineTick();
        return true;
    };
    try {
        do {
            if (onProgress_) onProgress_("buffering", startMs, durationMs);
            FpgaSpi::VideoCapabilities caps;
            const char* prototype = std::getenv("MPX_H264_PROTOTYPE");
            const char* filtering = std::getenv("MPX_H264_FILTER");
            const std::string selectedPrototype = prototype ? prototype : "ip";
            const std::string selectedFilter = filtering ? filtering : "on";
            // One more whole-session begin when the MrAudio consumer provably did not adopt
            // this epoch's audio Begin (FpgaSpi::controlAudioSession read the lane itself)
            // and the epoch was released: a refused first Begin must not cost the play.
            for (int sessionAttempt = 1;; ++sessionAttempt) {
                std::lock_guard<std::mutex> control(pauseControlMu_);
                std::lock_guard<std::mutex> present(presentMu_);
                const uint64_t retained = fpgaSession_.load();
                if (coreGateEpochIsStale(retained, fpgaSessionCoreChanges_)) {
                    // Core gate: the core was swapped after that epoch began, so its
                    // reset fence died with the old core instance. Nothing can ACK an
                    // abort; drop the stale ownership instead of failing every play.
                    log("media: core gate: dropping epoch " + std::to_string(retained) +
                        " retained from before the core swap");
                    fpga_.dropStaleSession();
                    fpgaSession_.store(0);
                    if (fpgaAudioOutput_ >= 0) {
                        ::close(fpgaAudioOutput_);
                        fpgaAudioOutput_ = -1;
                    }
                } else if (retained != 0) {
                    if (!fpga_.abortFpgaVideoSession(retained, 2000)) {
                        failFpgaSession("prior epoch still owns an unacknowledged reset fence: " +
                                        fpga_.lastError());
                        break;
                    }
                    log("media: FPGA epoch " + std::to_string(retained) +
                        " retained by the last play is released (fenced abort ACK)");
                    fpgaSession_.store(0);
                    if (fpgaAudioOutput_ >= 0) {
                        ::close(fpgaAudioOutput_);
                        fpgaAudioOutput_ = -1;
                    }
                }
                {
                    bool beganOk = false;
                    bool beginWedged = false;
                    std::string lastBeginErr = "begin not attempted";
                    for (int attempt = 1; attempt <= 5 && current() && !beganOk; ++attempt) {
                        if (fpga_.beginFpgaVideoSession(session, caps, 3000)) {
                            beganOk = true;
                            break;
                        }
                        lastBeginErr = fpga_.lastError();
                        log("media: FPGA begin attempt " + std::to_string(attempt) +
                            "/5 failed: " + lastBeginErr);
#ifndef MUT_MAST_BLIND_RETRY   // test-only mutant: keep retrying a stopped publisher
                        // review 21:14: only a core reload restarts a stopped MAST
                        // publisher; a begin retry re-runs the same check on the same FSM.
                        if (fpga_.lastBeginWedged()) {
                            beginWedged = true;
                            break;
                        }
#endif
                        std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
                    }
                    if (!beganOk) {
                        if (beginWedged)
                            onMastWedge("begin");
                        if (fpga_.videoCapabilities().nonce != 0) {
                            begun = true;
                            fpgaSession_.store(session);
                            fpgaSessionCoreChanges_ = CoreGate::process().changes();
                            MPX_AV_TRACE(if (avTrace) avTrace->bindNonce(caps.nonce);)
                        }
                        // SPS-gate L3 (R1, T11f): a FRESH CAPS reply (decodeVideoCapabilities sets caps only on a
                        // nonce match) that the begin refused for supportsVideo() is the gate's NoPlay row: the
                        // on-screen error, no play, no fallback. Every other begin failure is today's.
                        if (caps.nonce != 0 && !caps.supportsVideo()) {
                            const gate::CapsVerdict v = gate::evaluateCaps(caps);
                            const gate::GateDecision d = gate::decideCaps(v.outcome, gateCtx);
                            if (d.action == gate::GateAction::Error) {
                                log(gate::capsLogLine(v, d, caps.features, caps.build_id));
                                const std::string text =
                                    gate::errorText(d.row, gate::fallbackReason(v), caps.build_id);
                                setGateError(text, generation, epoch);
                                failFpgaSession(text);
                                break;
                            }
                        }
                        failFpgaSession("matching core capability/session ACK failed: " +
                                        lastBeginErr);
                        break;
                    }
                }
                begun = true;
                fpgaSession_.store(session);
                fpgaSessionCoreChanges_ = CoreGate::process().changes();
                MPX_AV_TRACE(if (avTrace) avTrace->bindNonce(caps.nonce);)
                {
                    const std::string reclaimed = fpga_.takeAudioRecoveryNote();
                    if (!reclaimed.empty())
                        log("media: " + reclaimed);
                }
                if (!current()) break;
                // Only recognised FPGA builds play (fpga_build_ids.hpp): an unknown
                // build ID is refused here, before the first AU. failFpgaSession
                // stops the session; fpga-h264 never falls back to ARM present.
                // The recognised row then drives the status decode (bytes 12/15).
                {
                    const std::string refusal = fpgaBuildRefusal(caps.build_id);
                    if (!refusal.empty()) {
                        fpga_.setNegotiatedBuild(nullptr);
                        failFpgaSession(refusal);
                        break;
                    }
                    const FpgaBuildInfo& build = *recognisedFpgaBuild(caps.build_id);
                    log("media: " + fpgaBuildLabel(build));
                    // OSD-menu rows (CONF_STR v,10) are lab RBFs: their CONF_STR
                    // must say so (an RBF from the right qsf on v,9 RTL would put
                    // telemetry in bytes 12/15). v,9 rows make no extra SPI read.
                    if (build.confVersion != 9) {
                        std::string conf;
                        if (fpga_.getConfigString(conf)) {
                            const std::string bad = fpgaConfVersionMismatch(build, conf);
                            if (!bad.empty()) {
                                fpga_.setNegotiatedBuild(nullptr);
                                failFpgaSession(bad);
                                break;
                            }
                            log("media: CONF_STR v," + std::to_string(build.confVersion) +
                                " matches the build row");
                        } else {
                            log("media: warning: CONF_STR unreadable (" + fpga_.lastError() +
                                "); trusting the build row");
                        }
                    }
                    fpga_.setNegotiatedBuild(&build);
                    // R1 (review 19:16 (1)(c)): PLXH en=0 written and ACKed by PLXO before the
                    // first AU, so a mid-session generation_clear never selects the idle FB.
                    // review 20:08: with no FB_VBL (PLXO bit-identical over the wait and at the
                    // re-read) the session proceeds with en=0 armed; en=1 stays blocked to END.
                    if (build.hostIdleFb) {
                        constexpr int kPlxhEn0AckMs = 200;      // about 12 FB_VBLs at 60 Hz
                        constexpr int kPlxhNoVblRecheckMs = 50;
                        std::string why;
                        if (!plxhPolicy_.beginSession(kPlxhEn0AckMs, &why, kPlxhNoVblRecheckMs)) {
                            failFpgaSession("R1: PLXH en=0 not ACKed before the first AU: " + why);
                            break;
                        }
                        const std::string token = std::to_string(plxhPolicy_.sessionToken());
                        if (plxhPolicy_.en0Pending())
                            log("R1: PLXH en=0 token " + token + " not ACKed before the first AU: " +
                                why + "; en=0 stays armed and latches at the first FB_VBL, en=1 "
                                "blocked to END; continuing");
                        else
                            log("r1: PLXH en=0 token " + token + " ACKed before the first AU" +
                                (why.empty() ? std::string() : " (" + why + ")"));
                    }
                }
                if (caps.max_width < 320 || caps.max_height < 240) {
                    failFpgaSession("core does not support initial 320x240 tier");
                    break;
                }
                // R17 624×480 cores should advertise the coded rectangle. Older
                // 5bb builds may still report 320×240 CAPS while FRAME_W=624;
                // allow DECODE≥480 with that known-stale CAPS binding.
                {
                    int wantW = outW_ > 0 ? outW_ : 320;
                    int wantH = outH_ > 0 ? outH_ : 240;
                    if (wantW >= 640 && wantH >= 480) {
                        // TRUE640: the coded 480p ceiling is the core's CAPS
                        // (640 on PLEX_DECODE_CODED_640, 624 on 39-MB cores).
                        wantW = plex480pCodedCeilingW(caps.max_width);
                        wantH = kPlex480pCodedHeight;
                    }
                    if ((wantW > 320 || wantH > 240) &&
                        caps.max_width <= 320 && caps.max_height <= 240) {
                        log("media: FPGA CAPS max=" + std::to_string(caps.max_width) + "x" +
                            std::to_string(caps.max_height) +
                            " below DECODE " + std::to_string(wantW) + "x" +
                            std::to_string(wantH) +
                            " — proceeding with DECODE geometry (pre-CAPS-fix core)");
                    } else if (wantW > static_cast<int>(caps.max_width) ||
                               wantH > static_cast<int>(caps.max_height)) {
                        failFpgaSession("core max geometry below requested FPGA ladder");
                        break;
                    }
                }
                if (!(caps.features & ddr_bitstream_ring::FencedReset)) {
                    failFpgaSession("core lacks acknowledged ingress/DPB/display reset fencing");
                    break;
                }
                if ((selectedPrototype != "ip" && selectedPrototype != "idr") ||
                    (selectedFilter != "on" && selectedFilter != "off") ||
                    (selectedPrototype == "ip" && !(caps.features & ddr_bitstream_ring::Inter)) ||
                    (selectedFilter == "on" && !(caps.features & ddr_bitstream_ring::Deblock))) {
                    failFpgaSession("selected IP/IDR/filter profile exceeds matching core capabilities");
                    break;
                }
                if (audioEnabled_ && sourceHasAudio_) {
                    constexpr uint32_t requiredAudio =
                        ddr_bitstream_ring::ConsumedAudioClock | ddr_bitstream_ring::AudioSessionControl;
                    if ((caps.features & requiredAudio) != requiredAudio) {
                        failFpgaSession("core lacks real audio DMA lifecycle/consumed-clock capability");
                        break;
                    }
                    audioBegun = true; // A timed-out Begin may still own an in-flight command.
                    if (!controlFpgaAudio(session, AudioSessionControl::Begin)) {
                        const std::string why = fpga_.lastError();
                        // Owned unless the lane proved the consumer runs another identity.
                        audioBegun = fpga_.audioSessionOwned();
                        if (sessionAttempt == 1 && !audioBegun && current() &&
                            fpga_.abortFpgaVideoSession(session, 2000)) {
                            log("media: FPGA audio Begin not adopted by the MrAudio consumer ("
                                + why + "); epoch released by fenced abort, beginning the "
                                "session again (2/2)");
                            begun = false;
                            fpgaSession_.store(0);
                            continue;
                        }
                        failFpgaSession("audio session reset ACK failed: " + why);
                        break;
                    }
                }
                if (paused_.load() &&
                    (!fpga_.pauseBitstreamSession(session, 500) ||
                     (audioBegun &&
                      !controlFpgaAudio(session, AudioSessionControl::Pause)))) {
                    failFpgaSession("initial paused-session ACK failed");
                    break;
                }
                break; // begun: video and audio own this epoch
            }
            if (!current()) break;
            // SPS-gate L3: the CAPS decision (gate::evaluateCaps -> decideCaps), once per play, after the build
            // refusal and before the demux opens. R4 (Ok) gates on the granted caps and logs nothing new; every
            // core of record publishes MPC1[31:16] = 0 (a1), so R4's limits equal today's.
            const gate::CapsVerdict capsVerdict = gate::evaluateCaps(caps);
            {
                const gate::GateDecision capsDecision = gate::decideCaps(capsVerdict.outcome, gateCtx);
                if (capsDecision.row != gate::GateRow::R4)
                    log(gate::capsLogLine(capsVerdict, capsDecision, caps.features, caps.build_id));
                if (capsDecision.action == gate::GateAction::Fallback) {
                    requestGateFallback(epoch, generation, startMs, gate::fallbackReason(capsVerdict),
                                        capsDecision.row);
                    break;
                }
                if (capsDecision.action == gate::GateAction::Error) {
                    const std::string text =
                        gate::errorText(capsDecision.row, gate::fallbackReason(capsVerdict), caps.build_id);
                    setGateError(text, generation, epoch);
                    failFpgaSession(text);
                    break;
                }
            }
            AvInprocOpenOpts opts;
            opts.compressedVideo = true;
            {
                // Demux ceiling is coded geometry. true480 bank is 640×480; FPGA
                // decode is 624×480 on 39-MB cores and 640×480 (40 MB/row) on
                // PLEX_DECODE_CODED_640 cores, which advertise CAPS max_width=640.
                // A 624 stream stays legal on a 640 core (ceiling, not equality).
                int expW = outW_ > 0 ? outW_ : 320;
                int expH = outH_ > 0 ? outH_ : 240;
                if (expW >= 640 && expH >= 480) {
                    expW = plex480pCodedCeilingW(caps.max_width);
                    expH = kPlex480pCodedHeight;
                }
                if (caps.max_width >= 480 && expW > static_cast<int>(caps.max_width))
                    expW = caps.max_width;
                if (caps.max_height >= 480 && expH > static_cast<int>(caps.max_height))
                    expH = caps.max_height;
                opts.expectW = expW;
                opts.expectH = expH;
            }
            opts.maxAccessUnitBytes = caps.max_au_bytes;
            // VCL RBSP budget tracks the core-advertised AU limit (R16: 16KiB).
            // Do not invent headroom beyond caps.max_au_bytes.
            opts.maxVclRbspBytes = caps.max_au_bytes ? caps.max_au_bytes : 8192;
            opts.expectedFpsNum = fpsNum_;
            opts.expectedFpsDen = fpsDen_;
            // SPS-gate L3: the stream gate keys on the CAPS verdict's caps: the granted caps on R4, the floor on R3
            // (never the raw word, s7a/s7b). On R4 inter/deblock equal today's raw-bit reads.
            opts.gateCaps = capsVerdict.caps;
            // SPS relax (0x48C5385C / 0x48C51FF1 / 0x48CFEE31): keyed on the recognised build row, never on a CAPS bit.
            if (const FpgaBuildInfo* row = recognisedFpgaBuild(caps.build_id)) {
                opts.spsRelaxNvenc = row->spsRelaxNvenc;
                opts.allowHighCabac = row->highCabac;
                opts.acceptHighForHostRewrite =
                    baselineHostTranscodeEnabled() && !row->highCabac;
            }
            opts.allowInter = selectedPrototype == "ip" && capsVerdict.caps.inter;
            opts.allowDeblock = selectedFilter == "on" && capsVerdict.caps.deblock;
            opts.requireAllIdr = selectedPrototype == "idr";
            opts.decodeAudio = audioEnabled_ && sourceHasAudio_;
            opts.cancelled = &stop_;
            opts.paused = &paused_;
            opts.headers = headers;
            opts.readStallMs = compressedReadStallMs(url);
            if (opts.readStallMs != kLanReadStallMs)
                log("media: remote PMS transcode: in-session read stall bound " +
                    std::to_string(opts.readStallMs) + " ms (LAN 5000)");
retry_demux:
            // GCAST: a ladder over gcast-xcode /yt.ts carries its start in start_ms= (live remux).
            opts.startMs = transcode_offset::demuxSeekMs(url, startMs);
            // File/Part retries may seek. Live universal already has offset=;
            // av_seek_frame on that session is 049 N2 EPERM.
            if (range_retry_ > 0 && !urlHasUniversalOffset(url) && !ladderYtRemuxSrc(url) &&
                !transcode_offset::liveStartBaked(url)) {
                opts.startMs = positionMs_.load();
            }
            // N2: the server's close of a length-less response (FFmpeg http EIO) ends the
            // stream only within 2 s of the PMS duration, measured from this play's offset.
            opts.expectedDurationMs = durationMs > 0 ? durationMs : 0;
            opts.sessionStartMs = std::max<int64_t>(0, startMs);
            std::string error;
            demuxReturnKind = "opening";
            bool opened;
            MPX_AV_TRACE({
                AvTraceSpan span(traceWriter, FpgaAvEvent::VideoOpen, tracePauseClock_);
                span.serviceBegin();)
            opened = demux.open(url, opts, error);
            MPX_AV_TRACE(span.serviceEnd(); span.value(0, opened); span.force(); })
            if (!opened) {
                demuxReturnKind = "open-error";
                demuxError = error;
                // Same-URL retry after a remote transcode stall reopens a dead
                // session (049 N1: Immediate exit requested). New PMS session
                // at the current offset. Do not failFpgaSession (that sets stop_).
                const int64_t openPos = positionMs_.load();
                if (range_retry_ > 0 && current() &&
                    shouldBoundedReopen(stop_.load(), outH_ <= 480, openPos, startMs) &&
                    boundedReopen(session)) {
                    log("media: http_range open-error — boundedReopen at " +
                        std::to_string(openPos) + " ms err=" + error);
                    break;
                }
                // SPS-gate L3 R10h: on the item's fallback play the stream is the helper's, so a failed open is
                // the helper's failure: the on-screen error, never a direct retry (T7a). Else R10, unchanged. An open
                // cancelled by a stop, a seek or a new play (stop_ / the epoch, !current()) is never R10h.
                const gate::GateDecision openDecision = gate::decideFailure(gateCtx);
                if (current() && openDecision.action == gate::GateAction::Error) {
                    const std::string text = gate::errorText(openDecision.row, error, caps.build_id);
                    log(std::string("GATE FAIL row=") + gate::rowName(openDecision.row) +
                        " action=error stage=open err=" + error);
                    setGateError(text, generation, epoch);
                    failFpgaSession(text);
                } else {
                    failFpgaSession(error);
                }
                break;
            }
            demuxOpened = true;
            demuxReturnKind = "not-called";
            {
                std::lock_guard<std::mutex> lock(inprocMu_);
                inprocPcm_ = &demux;
            }
            if (opts.decodeAudio) {
                fpgaAudioDone_.store(false);
                audioThr_ = std::thread([this MPX_AV_TRACE(, avTrace)] {
                    try {
                        fpgaAudioPump(MPX_AV_TRACE(avTrace));
                    } catch (...) {
                        fpgaAudioExit_.store(FpgaAudioExit::Error);
                        audioActive_.store(false);
                        fpgaAudioDone_.store(true);
                        fpgaAudioStarted_.store(false);
                        failFpgaSession("compressed audio worker exception");
                    }
                });
                // G1 av_start: hold 240p/480p/480i video until audio prefill is ready.
                if (fpgaAvStartGateBank(outW_, outH_)) {
                    const int64_t gateDeadline = playbackActiveUs() + 2000000;
                    while (!audioReadyForStart_.load() && current() && !stop_.load() &&
                           playbackActiveUs() < gateDeadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } else {
                    audioReadyForStart_.store(true);
                }
            } else {
                audioReadyForStart_.store(true);
            }
            bool aspectSent = false;
            bool firstAu = true;
            auto nextStatus = std::chrono::steady_clock::now();
            while (current()) {
                if (!coreGateAllowsSession()) {
                    failFpgaSession(coreGateClosedError("FPGA session"));
                    break;
                }
                if (std::chrono::steady_clock::now() >= nextStatus) {
                    FpgaSpi::BitstreamStatus status;
                    bool healthy = false;
                    {
                        MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::VideoStatus, tracePauseClock_);
                            AvTraceRelease release{span};)
                        std::lock_guard<std::mutex> present(presentMu_);
                        MPX_AV_TRACE(span.acquired(); span.serviceBegin();)
                        const bool statusRead = fpga_.readBitstreamStatus(status);
                        MPX_AV_TRACE(span.serviceEnd(); span.value(0, statusRead);
                            if (statusRead) {
                                span.value(1, status.active); span.value(2, status.fatal);
                                span.value(3, status.desync); span.uvalue(4, status.session_id);
                            } else span.force();)
                        healthy = statusRead && status.active &&
                                  status.session_id == session && !status.fatal && !status.desync;
                    }
                    if (!healthy) {
                        failFpgaSession("core changed or ingress epoch lost");
                        break;
                    }
                    nextStatus = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                }
                if (paused_.load()) {
                    MPX_AV_TRACE(traceVideoWait(FpgaAvWait::Paused);)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (!collectPresentation()) break;
                if (pending.size() >= 4) {
                    waitReason = "presentation-queue";
                    MPX_AV_TRACE(traceVideoWait(FpgaAvWait::PresentationQueue);)
                    if (playbackActiveUs() - lastCommitActiveUs > 2000000) {
                        failFpgaSession("video presentation stalled (bounded buffering)");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                if (fpgaVideoStarted_.load() && lastPicTime_.time_since_epoch().count() != 0) {
                    const int64_t gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lastPicTime_).count();
                    if (gapMs > 500 && !picStallLogged_) {
                        picStallLogged_ = true;
                        ++picStallCount_;
                        log("media: stalled picture detected (no new picture for " +
                            std::to_string(gapMs) + " ms) count=" +
                            std::to_string(picStallCount_));
                    }
                }
                if (isPicStalled()) {
                    const int64_t pos = positionMs_.load();
                    const bool gold = outH_ <= 480;
                    int hits = 0;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        hits = static_cast<int>(++stalledPicWatchdogHits_);
                    }
                    if (stalledPicShouldReopen(true, hits, kRangeRetryMax) &&
                        shouldBoundedReopen(stop_.load(), gold, pos, startMs) &&
                        boundedReopen(session)) {
                        log("media: picture stall — boundedReopen at " +
                            std::to_string(pos) + " ms stalledPicWatchdogHits=" +
                            std::to_string(hits));
                        break;
                    }
                    failFpgaSession("picture delivery stalled (watchdog)");
                    break;
                }
                AvCompressedAccessUnit packet;
                waitReason = "demux";
                demuxReturn.reset();
                demuxReturnKind = "in-flight";
                demuxError.clear();
                int read;
                MPX_AV_TRACE({
                    AvTraceSpan span(traceWriter, FpgaAvEvent::VideoRead, tracePauseClock_);
                    const auto pressure = demux.compressedPressure();
                    span.uvalue(10, ++traceReadCalls); span.uvalue(11, sequence);
                    span.uvalue(12, pending.size()); span.serviceBegin();)
                read = demux.readAccessUnit(packet, error);
                MPX_AV_TRACE(span.serviceEnd(); span.value(0, read);
                    tracePressure(span, pressure, demux.compressedPressure());
                    if (read > 0) {
                        span.value(1, packet.pts); span.value(13, packet.duration);
                        span.uvalue(14, packet.timebaseNum); span.uvalue(15, packet.timebaseDen);
                    } else span.force();
                })
                if (read > 0) {
                    const int fn = fpsNum_ > 0 ? fpsNum_ : kDefaultFpsNum;
                    const int fd = (fpsNum_ > 0 && fpsDen_ > 0) ? fpsDen_ : kDefaultFpsDen;
                    const int64_t frameTicks =
                        frameDurationTicks(packet.timebaseNum, packet.timebaseDen, fn, fd);
                    if (ptsJumpIsAcceptedNFrames(packet.pts, last_ok_pts_,
                                                 packet.timebaseNum, packet.timebaseDen))
                        last_ok_pts_ = packet.pts;
                    else
                        packet.pts = clampPtsJump(packet.pts, last_ok_pts_, frameTicks);
                    MPX_AV_TRACE({
                        AvTraceSpan span(traceWriter, FpgaAvEvent::VideoRead, tracePauseClock_);
                        span.value(2, packet.pts);
                    })
                }
                demuxReturn = read;
                demuxError = error;
                demuxReturnKind = read > 0 ? "au" : read == 0 ? "eof" :
                    current() ? "error" : "cancelled";
                if (read < 0 && current() && !demux.lastGateRefusal().valid) {
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        last_good_offset_ = bytes;
                        ++range_retry_;
                    }
                    log("media: http_range demux error bytes=" + std::to_string(bytes) +
                        " range_retry=" + std::to_string(range_retry_) + " err=" + error);
                    const int64_t pos = positionMs_.load();
                    if (shouldBoundedReopen(stop_.load(), outH_ <= 480, pos, startMs) &&
                        boundedReopen(session)) {
                        log("media: http_range stall — boundedReopen at " +
                            std::to_string(pos) + " ms");
                        break;
                    }
                    // Same-URL retry is only for a first-open glitch (no pictures yet).
                    // After content it is 049 N1 (dead start.mp4 session).
                    if (range_retry_ < kRangeRetryMax && pos <= startMs) {
                        log("media: http_range retrying demux from offset " +
                            std::to_string(last_good_offset_));
                        // 14496a64 L6 logged this retry and the next line was
                        // "terminate called without an active exception": the
                        // reopen assigned a new audio thread over the live one.
                        hostCabac.stop();
                        hostCabacOn = false;
                        hostBaselineOn = false;
                        fpgaAudioJoin_.store(true);
                        interruptInproc();
                        if (audioThr_.joinable())
                            audioThr_.join();
                        fpgaAudioJoin_.store(false);
                        {
                            std::lock_guard<std::mutex> lock(inprocMu_);
                            inprocPcm_ = nullptr;
                        }
                        goto retry_demux;
                    }
                }
                if (read < 0) {
                    if (current()) {
                        const AvGateRefusal& refusal = demux.lastGateRefusal();
                        if (refusal.valid) {
                            if (isPlex480pPresentBank(outW_, outH_)) {
                                const char* code = spsRejectCode480pFromField(refusal.verdict.field);
                                if (code && code[0]) {
                                    setSpsRejectCode480p(code);
                                    log(std::string("media: spsRejectCode480p=") + code);
                                }
                            }
                            // SPS-gate L3: the decision owner routes the refusal (paper section 2; Q1-Q3).
                            const gate::GateDecision d =
                                gate::decideStream(refusal.verdict.outcome, sequence, gateCtx);
                            log(gate::streamLogLine(refusal.verdict, d, caps.features, caps.build_id, sequence,
                                                    bytes));
                            log("media: GATE refusal err: " + refusal.err);
                            if (d.action == gate::GateAction::Fallback) {
                                requestGateFallback(epoch, generation, startMs,
                                                    gate::fallbackReason(refusal.verdict), d.row);
                            } else if (d.action == gate::GateAction::Error) {
                                const std::string text =
                                    gate::errorText(d.row, gate::verdictPhrase(refusal.verdict), caps.build_id);
                                setGateError(text, generation, epoch);
                                failFpgaSession(text);
                            } else {
                                failFpgaSession(error);   // R9: today's stop, its err unchanged (L4 owns the flip)
                            }
                        } else if (sequence == 0 &&
                                   gate::decideFailure(gateCtx).action == gate::GateAction::Error) {
                            // R10h before the first AU: the helper's stream failed (T7a).
                            log("GATE FAIL row=R10h action=error stage=read err=" + error);
                            const std::string text = gate::errorText(gate::GateRow::R10h, error, caps.build_id);
                            setGateError(text, generation, epoch);
                            failFpgaSession(text);
                        } else {
                            failFpgaSession(error);
                        }
                    }
                    break;
                }
                if (!read) {
                    if (opts.decodeAudio && !demux.hasAudio()) {
                        failFpgaSession("expected audio stream was not delivered by the sole demux");
                    } else {
                        eof = true;
                        const int64_t shortMs = demux.inputClosedAtEndShortMs();
                        if (shortMs != kNoInputCloseAtEnd)
                            log("media: input ended by the server's close " +
                                std::to_string(shortMs) + " ms short of the PMS duration " +
                                std::to_string(opts.expectedDurationMs) +
                                " ms (FFmpeg http EIO on a length-less response): end of stream");
                    }
                    break;
                }
                {
                    std::lock_guard<std::mutex> ttfpLock(ttfpMu_);
                    if (ttfp_.firstPacketUs == 0) {
                        ttfp_.firstPacketUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                    }
                }
                {
                    const FpgaBuildInfo* negotiatedNow = recognisedFpgaBuild(caps.build_id);
                    const bool highCoreNow = negotiatedNow && negotiatedNow->highCabac;
                    const bool cavlcAu = packet.annexb.size() >= 4 &&
                        !annexbNeedsBaselineTranscode(packet.annexb.data(), packet.annexb.size());
                    // 104: decision extra=cabac=1 and directPlay=0 still returned CAVLC.
                    // Re-encode on the box ffmpeg (libx264) before the High core.
                    std::string hostCabacErr;
                    bool hostCabacHold = false;
                    if (highCoreNow && highHostCabacTranscodeEnabled() &&
                        (hostCabacOn || (firstAu && cavlcAu))) {
                        if (!hostCabacOn) {
                            if (!hostCabac.start(ffmpeg_)) {
                                log("media: High core first AU is CAVLC; host ffmpeg missing");
                            } else {
                                hostCabacOn = true;
                                hostCabacSince = std::chrono::steady_clock::now();
                                log("media: High core first AU is CAVLC; host libx264 Main transcode");
                            }
                        }
                        if (hostCabacOn) {
                            if (!hostCabac.push(packet.annexb.data(), packet.annexb.size())) {
                                hostCabacErr = "host libx264 Main transcode write failed";
                            } else {
                                std::vector<uint8_t> cabac;
                                const auto popped = hostCabac.pop(cabac, 30);
                                if (popped == HighCabacHostTranscode::Pop::NeedMore) {
                                    // 107: 48 source AUs (~1.4s) expired while x264's first
                                    // frame was still inside the encoder. Wait on the clock.
                                    const auto waited = std::chrono::steady_clock::now() - hostCabacSince;
                                    if (waited > std::chrono::seconds(8))
                                        hostCabacErr =
                                            "host libx264 Main transcode produced no CABAC AU out=" +
                                            std::to_string(hostCabac.bytesOut()) +
                                            " err=" + hostCabac.stderrText();
                                    else
                                        hostCabacHold = true;
                                } else {
                                    if (popped != HighCabacHostTranscode::Pop::Got ||
                                        cabac.size() < 4 ||
                                        !annexbNeedsBaselineTranscode(cabac.data(), cabac.size()))
                                        hostCabacErr =
                                            "host libx264 Main transcode did not emit CABAC";
                                    else {
                                        packet.annexb = std::move(cabac);
                                        packet.keyframe = annexbContainsIdr(packet.annexb.data(),
                                                                            packet.annexb.size());
                                    }
                                }
                            }
                        }
                    } else if (!highCoreNow && baselineHostTranscodeEnabled() &&
                               (hostBaselineOn ||
                                (firstAu && packet.annexb.size() >= 4 && !cavlcAu))) {
                        // 179 L6 reached R7 before this AU could be rewritten.
                        if (!hostBaselineOn) {
                            if (!hostCabac.start(ffmpeg_, true)) {
                                log("media: baseline core first AU is High; host ffmpeg missing");
                            } else {
                                hostBaselineOn = true;
                                hostCabacSince = std::chrono::steady_clock::now();
                                log("media: baseline core first AU is High; host libx264 Baseline transcode");
                            }
                        }
                        if (hostBaselineOn) {
                            if (!hostCabac.push(packet.annexb.data(), packet.annexb.size())) {
                                hostCabacErr = "host libx264 Baseline transcode write failed";
                            } else {
                                std::vector<uint8_t> cavlc;
                                const auto popped = hostCabac.pop(cavlc, 30);
                                if (popped == HighCabacHostTranscode::Pop::NeedMore) {
                                    const auto waited =
                                        std::chrono::steady_clock::now() - hostCabacSince;
                                    if (waited > std::chrono::seconds(8))
                                        hostCabacErr =
                                            "host libx264 Baseline transcode produced no CAVLC AU out=" +
                                            std::to_string(hostCabac.bytesOut()) +
                                            " err=" + hostCabac.stderrText();
                                    else
                                        hostCabacHold = true;
                                } else if (popped != HighCabacHostTranscode::Pop::Got ||
                                           cavlc.size() < 4 ||
                                           annexbNeedsBaselineTranscode(cavlc.data(), cavlc.size())) {
                                    hostCabacErr =
                                        "host libx264 Baseline transcode did not emit CAVLC";
                                } else {
                                    packet.annexb = std::move(cavlc);
                                    packet.keyframe = annexbContainsIdr(packet.annexb.data(),
                                                                        packet.annexb.size());
                                }
                            }
                        }
                    }
                    if (!hostCabacErr.empty()) {
                        failFpgaSession(hostCabacErr);
                        break;
                    }
                    if (hostCabacHold)
                        continue;
                }
                if (firstAu && !packet.keyframe) {
                    failFpgaSession("session/seek must begin at an IDR");
                    break;
                }
                if (firstAu) {
                    const FpgaBuildInfo* negotiated = recognisedFpgaBuild(caps.build_id);
                    const bool highCore = negotiated && negotiated->highCabac;
                    // Plex_high fails a CAVLC PPS closed (error 16). Do not submit it.
                    if (highCore && packet.annexb.size() >= 4 &&
                        !annexbNeedsBaselineTranscode(packet.annexb.data(), packet.annexb.size())) {
                        log("media: High core first AU is CAVLC; not submitting");
                        failFpgaSession("High core requires CABAC; PMS sent CAVLC");
                        break;
                    }
                    if (!highCore && highLibavTranscodeEnabled() && packet.annexb.size() >= 4 &&
                        annexbNeedsBaselineTranscode(packet.annexb.data(), packet.annexb.size())) {
                        if (gateCtx.fallbackUsed || gateCtx.helperStream) {
                            const std::string text =
                                "High/CABAC after the baseline ladder; core is Baseline CAVLC";
                            setGateError(text, generation, epoch);
                            failFpgaSession(text);
                            break;
                        }
                        log("media: High/CABAC SPS probe — PMS baseline ladder "
                            "(libx264 cabac=0, no MiSTerPlex.xml)");
                        requestGateFallback(epoch, generation, startMs,
                                            "high-cabac SPS probe; baseline ladder",
                                            gate::GateRow::R6);
                        break;
                    }
                    const std::string liveRaster = CoreGate::process().pairedRaster();
                    const char* mode =
                        !liveRaster.empty() ? liveRaster.c_str() : std::getenv("MPX_FPGA_MODE");
                    const bool mode480i = mode && std::strcmp(mode, "480i") == 0;
                    const bool coded480i =
                        outW_ == kPlex480iCodedWidth && outH_ == kPlex480iCodedHeight;
                    if (packet.annexb.size() >= 5) {
                        const auto sps = parseFirstSpsAnnexB(packet.annexb.data(),
                                                             packet.annexb.size());
                        if (sps.valid &&
                            misterplex::baseline240pRejectsProfile(caps.build_id, sps.profile_idc)) {
                            failFpgaSession("240p core requires profile_idc<=66");
                            break;
                        }
                        if (spsReject480i(mode480i, coded480i, sps.valid, sps.width, sps.height)) {
                            failFpgaSession("480i SPS coded size is not 720x480");
                            break;
                        }
                    }
                    ttfpFirstPacketTimeUs_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                }
                firstAu = false;
                heldPts = packet.pts;
                heldTimebaseNum = packet.timebaseNum;
                heldTimebaseDen = packet.timebaseDen;
                if (!aspectSent) {
                    std::lock_guard<std::mutex> present(presentMu_);
                    const FirstAuSourceAspect first = firstAuSourceAspect(
                        sourceTranscoded_.load(), sourceAspect_, demux.sourceAspect());
                    SourceAspect aspect = first.aspect;
                    const auto& geometry = packet.geometry;
                    int lbTop = 0, lbBot = 0;
                    if (outW_ == 320 && outH_ == 240)
                        letterboxBars240(geometry.codedHeight, geometry.visibleHeight, &lbTop, &lbBot);
                    else if (outH_ == 480)
                        letterboxBars480(geometry.codedHeight, geometry.visibleHeight, &lbTop, &lbBot);
                    {
                        std::lock_guard<std::mutex> lock(summaryMu_);
                        lastSummary_.codedWidth = geometry.codedWidth;
                        lastSummary_.codedHeight = geometry.codedHeight;
                        lastSummary_.visibleWidth = geometry.visibleWidth;
                        lastSummary_.visibleHeight = geometry.visibleHeight;
                        lastSummary_.macroblockColumns = geometry.macroblockColumns;
                        lastSummary_.macroblockRows = geometry.macroblockRows;
                        lastSummary_.cropLeft = geometry.cropLeft;
                        lastSummary_.cropRight = geometry.cropRight;
                        lastSummary_.cropTop = geometry.cropTop;
                        lastSummary_.cropBottom = geometry.cropBottom;
                        lastSummary_.cropTop240p = (outW_ == 320 && outH_ == 240) ? lbTop : 0;
                        lastSummary_.cropBottom240p = (outW_ == 320 && outH_ == 240) ? lbBot : 0;
                        lastSummary_.cropTopLb = lbTop;
                        lastSummary_.cropBottomLb = lbBot;
                        lastSummary_.sourceAspect = aspect;
                    }
                    log("media: FPGA stream geometry coded=" +
                        std::to_string(geometry.codedWidth) + "x" + std::to_string(geometry.codedHeight) +
                        " visible=" + std::to_string(geometry.visibleWidth) + "x" +
                        std::to_string(geometry.visibleHeight) + " mb=" +
                        std::to_string(geometry.macroblockColumns) + "x" +
                        std::to_string(geometry.macroblockRows) + " count=" +
                        std::to_string(geometry.macroblockColumns * geometry.macroblockRows) +
                        " crop_px=" + std::to_string(geometry.cropLeft) + "," +
                        std::to_string(geometry.cropRight) + "," + std::to_string(geometry.cropTop) +
                        "," + std::to_string(geometry.cropBottom) +
                        " lb=" + std::to_string(lbTop) + "," +
                        std::to_string(lbBot) + " source_dar=" +
                        std::to_string(aspect.x) + ":" + std::to_string(aspect.y) +
                        " dar_source=" + first.source +
                        " full_range=" + std::to_string(geometry.fullRange) +
                        " matrix_coefficients=" + std::to_string(geometry.matrixCoefficients) +
                        " limited_bt601_signaling=" +
                        std::to_string(geometry.limitedBt601Signaled()) +
                        (geometry.matrixCoefficients == 2 ?
                            " color_policy=ASSUMED_LIMITED_BT601_UNQUALIFIED" :
                            " color_policy=SIGNALED_LIMITED_BT601") +
                        " storage_stride=FPGA-owned/unreported (no ARM padding)");
                    if (first.metadataOverridden)
                        log("media: transcode: stream DAR " + std::to_string(aspect.x) + ":" +
                            std::to_string(aspect.y) + " sent, metadata " +
                            std::to_string(sourceAspect_.x) + ":" +
                            std::to_string(sourceAspect_.y) +
                            " not used (the transcoder's raster; review 19:24 L2)");
                    if (!aspect.valid) {
                        // Never CLOSED-STOP a live cast for missing DAR/ACK.
                        // 720x480 square is 3:2; 480i/480p banks want 4:3.
                        if (geometry.codedWidth > 0 && geometry.codedHeight > 0) {
                            const SourceAspect fb = defaultSourceAspectForBank(
                                geometry.codedWidth, geometry.codedHeight);
                            aspect = fb;
                            log("media: source aspect fallback bank " +
                                std::to_string(fb.x) + ":" + std::to_string(fb.y) +
                                " coded=" + std::to_string(geometry.codedWidth) + "x" +
                                std::to_string(geometry.codedHeight));
                        } else {
                            log("media: source aspect missing; continuing without scaler DAR");
                        }
                    }
                    // PLXJ ACK is blank-only on some cores; do not CLOSED-STOP the
                    // cast when the scaler handshake is missing (same policy as
                    // setSourceAspect). Still publish the packet best-effort.
                    {
                        const FpgaBuildInfo* letterboxRow = recognisedFpgaBuild(caps.build_id);
                        if (skipStreamRasterPlacement480(letterboxRow)) {
                            char bbuf[192];
                            std::snprintf(bbuf, sizeof bbuf,
                                          "media: letterbox skip streamRasterPlacement480 on "
                                          "240-line raster=%s build=0x%08X",
                                          letterboxRow && letterboxRow->raster
                                              ? letterboxRow->raster
                                              : "?",
                                          caps.build_id);
                            log(bbuf);
                        } else {
                            const StreamRasterPlacement placement = streamRasterPlacement480(
                                geometry.codedWidth, geometry.codedHeight, geometry.cropLeft,
                                geometry.cropRight, geometry.cropTop, geometry.cropBottom);
                            char bbuf[256];
                            std::snprintf(
                                bbuf, sizeof bbuf,
                                "media: letterbox placement valid=%d letterbox=%d display=%dx%d "
                                "lb_top=%d lb_bottom=%d crop_px=%d,%d,%d,%d build=0x%08X",
                                placement.valid ? 1 : 0, placement.letterbox ? 1 : 0,
                                placement.display_width, placement.display_height, placement.lb_top,
                                placement.lb_bottom, geometry.cropLeft, geometry.cropRight,
                                geometry.cropTop, geometry.cropBottom, caps.build_id);
                            log(bbuf);
                        }
                    }
                    if (aspect.valid) {
                        std::string clampNote;
                        const bool published = fpga_.sendSourceAspect(aspect, nullptr, &clampNote);
                        if (!clampNote.empty())
                            log("media: source aspect " + clampNote);
                        if (!published)
                            log("media: source aspect publish optional-continue: " +
                                fpga_.lastError());
                    }
                    aspectSent = true;
                    if (!overlayLabFlags) {
                        startOverlay(caps.nonce, caps.build_id);
                    } else {
                        overlayBuildId = caps.build_id;
                        if (const std::string notice = formatOverlaySkipNoPlaneNotice(
                                overlaySkipNoPlane, std::getenv(kOverlaySkipNoPlaneEnv),
                                caps.build_id);
                            !notice.empty())
                            log(notice);
                        if (overlayPublisherWanted(overlaySkipNoPlane, caps.build_id))
                            startLabOverlay(caps.nonce);
                    }
                }
                FpgaSpi::BitstreamAccessUnit au;
                au.session_id = session; au.seq = sequence;
                au.annexb = packet.annexb.data(); au.len = packet.annexb.size();
                au.pts = packet.pts; au.duration = packet.duration;
                au.timebase_num = packet.timebaseNum; au.timebase_den = packet.timebaseDen;
                au.flags = packet.keyframe ? ddr_bitstream_ring::kAccessUnitKeyframe : 0;
                // Timestamp pacing is distinct from decoding. No fps filter,
                // frame duplication, or compressed P-picture discard is allowed.
                if (fpgaVideoStarted_.load()) {
                    waitReason = "video-pts";
                    const int64_t pacePts = ptsWrap_.apply(packet.pts);
                    const long double targetUs =
                        static_cast<long double>(pacePts) * packet.timebaseNum *
                        1000000 / packet.timebaseDen - firstPtsUs;
                    MPX_AV_TRACE(AvTraceSpan pace(traceWriter, FpgaAvEvent::VideoPace, tracePauseClock_);
                        pace.value(1, packet.pts); pace.value(2, packet.duration);
                        pace.uvalue(3, packet.timebaseNum); pace.uvalue(4, packet.timebaseDen);
                        pace.uvalue(5, pending.size()); pace.serviceBegin();)
                    const auto paced = waitForCompressedAccessUnit(targetUs, current,
                        collectPresentation, [&] {
                            const auto now = std::chrono::steady_clock::now();
                            const int64_t pausedUs = playbackPausedUs(now);
                            const int64_t activeUs = activePlaybackClockUs(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    now.time_since_epoch()).count(), pausedUs);
                            const int64_t wallUs = activeUs -
                                fpgaVideoStartMonotonicUs_.load() + anchorPausedUs;
                            const int64_t consumed = fpgaAudioConsumedSamples_.load();
                            // Prefer audio mast when it is actually advancing. 882b cast
                            // often reports mast_active=0/paused=1 while picture is live;
                            // locking pace to a frozen consumed counter stalls AUs and
                            // CLOSED-STOPs after a few presented frames.
                            const bool audioClockLive = audioActive_.load() && consumed >= 0 &&
                                fpgaHasConsumedClock_.load() &&
                                (fpgaAudioClockChangedUs_.load() + 500000LL >=
                                 playbackActiveUs());
                            const int64_t clockUs = audioClockLive
                                ? fpgaAudioStartRelativeUs_.load() +
                                    consumed * 1000000LL / 48000 : wallUs;
                            MPX_AV_TRACE({
                                AvTraceSpan wait(traceWriter, FpgaAvEvent::VideoWait, tracePauseClock_);
                                wait.value(0, static_cast<int>(FpgaAvWait::Pts));
                                wait.uvalue(1, pending.size()); wait.uvalue(2, sequence);
                                wait.uvalue(3, presentCount_.load()); wait.value(4, packet.pts);
                                wait.uvalue(5, packet.timebaseNum); wait.uvalue(6, packet.timebaseDen);
                                wait.value(7, paused_.load()); wait.value(8, stop_.load());
                                wait.value(9, activeUs); wait.value(10, clockUs);
                                wait.value(11, wallUs);
                                if (targetUs >= std::numeric_limits<int64_t>::min() &&
                                    targetUs <= std::numeric_limits<int64_t>::max())
                                    wait.value(12, static_cast<int64_t>(targetUs));
                                wait.value(13, audioActive_.load() && consumed >= 0);
                            })
                            return CompressedPacingSnapshot{
                                paused_.load(), activeUs, lastCommitActiveUs, clockUs};
                        }, [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
                    MPX_AV_TRACE(pace.serviceEnd(); pace.value(0, static_cast<int>(paced));)
                    if (paced == CompressedPacingResult::Stalled)
                        failFpgaSession("presentation/audio clock stopped advancing");
                    if (paced != CompressedPacingResult::Due) break;
                }
                const int64_t deadline = playbackActiveUs() + 2000000;
                waitReason = "ring-capacity";
                bool accepted = false;
                while (current()) {
                    if (paused_.load()) {
                        MPX_AV_TRACE(traceVideoWait(FpgaAvWait::Paused);)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    FpgaSpi::BitstreamPushResult result;
                    {
                        MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::VideoSubmit, tracePauseClock_);
                            AvTraceRelease release{span}; span.uvalue(1, sequence);
                            span.value(2, au.pts); span.value(3, au.duration);
                            span.uvalue(4, au.timebase_num); span.uvalue(5, au.timebase_den);
                            span.uvalue(6, au.len); span.uvalue(7, pending.size());)
                        std::lock_guard<std::mutex> present(presentMu_);
                        MPX_AV_TRACE(span.acquired();)
                        if (!current() || paused_.load()) continue;
                        MPX_AV_TRACE(span.serviceBegin();)
                        result = fpga_.pushBitstreamAccessUnit(au, 0);
                        MPX_AV_TRACE(span.serviceEnd(); span.value(0, static_cast<int>(result));)
                    }
                    if (result == FpgaSpi::BitstreamPushResult::Ok) {
                        if (sequence == 0)
                            ttfpFirstDecodeTimeUs_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                        accepted = true; break;
                    }
                    if (result != FpgaSpi::BitstreamPushResult::Full) {
                        // T-DAEMON-MVPS (review 18:34): name the core's decode_error when MVPS already reports it.
                        FpgaSpi::VideoPresentation fault{};
                        const std::string note = fpga_.readVideoPresentation(session, fault)
                            ? ddr_bitstream_ring::decodeErrorNote(fault) : std::string();
                        failFpgaSession("AU transport rejected sequence " + std::to_string(sequence) + note);
                        break;
                    }
                    MPX_AV_TRACE(traceVideoWait(FpgaAvWait::RingFull);)
                    if (!collectPresentation()) break;
                    if (!paused_.load() && playbackActiveUs() >= deadline) {
                        failFpgaSession("compressed ring Full timeout; AU retained, never dropped");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (!accepted) break;
                if (sequence == 0)
                    firstSubmittedPtsUs = static_cast<int64_t>(
                        static_cast<long double>(ptsWrap_.apply(packet.pts)) * packet.timebaseNum *
                        1000000 / packet.timebaseDen);
                pending.push_back({sequence++, packet.pts, packet.timebaseNum, packet.timebaseDen});
                {
                    std::lock_guard<std::mutex> ttfpLock(ttfpMu_);
                    if (ttfp_.firstDecodedUs == 0) {
                        ttfp_.firstDecodedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                    }
                }
                heldPts = ddr_bitstream_ring::kNoTimestamp;
                bytes += packet.annexb.size();
                if (sequence == 1) {
                    waitReason = "first-presentation";
                    // 240p product stays at 2s. 480p FOAR (~1k MBs) can need longer
                    // first-frame wall time before presentation_count rises.
                    const int64_t firstBudgetUs =
                        (outW_ >= 640 || outH_ >= 480) ? 15000000LL : 2000000LL;
                    const int64_t firstFrameDeadline = playbackActiveUs() + firstBudgetUs;
                    while (current() && !fpgaVideoStarted_.load()) {
                        MPX_AV_TRACE(traceVideoWait(FpgaAvWait::FirstPresentation);)
                        if (!collectPresentation()) break;
                        {
                            static int64_t lastDiag = 0;
                            const int64_t now = playbackActiveUs();
                            if (now - lastDiag >= 500000) {
                                lastDiag = now;
                                FpgaSpi::VideoPresentation snap{};
                                if (fpga_.readVideoPresentation(session, snap)) {
                                    log("media: first-present wait has_frame=" +
                                        std::to_string((int)snap.has_frame) +
                                        " present_count=" +
                                        std::to_string(snap.presentation_count) +
                                        " err=" + std::to_string((int)snap.error) +
                                        " code=" + std::to_string(snap.error_code) +
                                        " active=" + std::to_string((int)snap.active));
                                } else {
                                    log(std::string("media: first-present wait PLXD unread: ") +
                                        fpga_.lastError());
                                }
                            }
                        }
                        if (!paused_.load() && playbackActiveUs() >= firstFrameDeadline) {
                            failFpgaSession("first decoded picture was not actually presented");
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
            }
            if (eof && current() && seekReqMs_.load() < 0) {
                waitReason = "eof-drain";
                if (videoDrainOn_)
                    tryVideoDrain(session);
                while (current()) {
                    if (paused_.load()) {
                        MPX_AV_TRACE(traceVideoWait(FpgaAvWait::Paused);)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::VideoDrain, tracePauseClock_);
                        AvTraceRelease release{span};)
                    std::lock_guard<std::mutex> control(pauseControlMu_);
                    std::lock_guard<std::mutex> present(presentMu_);
                    MPX_AV_TRACE(span.acquired();)
                    if (paused_.load()) continue;
                    drainAttempted = true;
                    MPX_AV_TRACE(span.serviceBegin();)
                    drainAck = fpga_.drainBitstreamSession(session, 2000);
                    MPX_AV_TRACE(span.serviceEnd(); span.value(0, *drainAck); span.force();)
                    if (!*drainAck)
                        failFpgaSession("final access-unit drain ACK failed");
                    break;
                }
                const int64_t deadline = playbackActiveUs() + (videoDrainOn_ ? 5000000 : 3000000);
                int max_frames = videoDrainOn_ ? 50 : 1000000;
                while (current() && (!pending.empty() || !fpgaAudioDone_.load()) && max_frames > 0) {
                    MPX_AV_TRACE(traceVideoWait(FpgaAvWait::EofDrain);)
                    if (!collectPresentation()) break;
                    if (!paused_.load() && playbackActiveUs() >= deadline) {
                        if (videoDrainOn_) {
                            log("media: video drain timeout — pending " +
                                std::to_string(pending.size()));
                            break;
                        }
                        failFpgaSession("final video/audio presentation drain timeout");
                        break;
                    }
                    --max_frames;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                fullyDrained = current() && pending.empty() && fpgaAudioDone_.load() &&
                    sequence != 0 && presentCount_.load() == sequence;
            }
        } while (false);
    } catch (const std::exception& ex) {
        if (std::string(demuxReturnKind) == "in-flight" ||
            std::string(demuxReturnKind) == "opening") {
            demuxReturnKind = "exception";
            demuxError = ex.what();
        }
        failFpgaSession(ex.what());
    } catch (...) {
        if (std::string(demuxReturnKind) == "in-flight" ||
            std::string(demuxReturnKind) == "opening") {
            demuxReturnKind = "exception";
            demuxError = "non-standard demux exception";
        }
        failFpgaSession("unexpected compressed session exception");
    }
    auto captureTerminal = [&] {
        const auto queued = demux.compressedDiagnostics();
        FpgaSpi::AudioSessionStatus live{};
        bool liveValid = false;
        std::optional<int64_t> liveReadAtUs;
        {
            std::lock_guard<std::mutex> present(presentMu_);
            if (audioBegun) {
                liveValid = fpga_.readAudioSessionStatus(session, live);
                if (liveValid) liveReadAtUs = playbackActiveUs();
            }
        }
        int64_t frozenSamples = -1;
        {
            std::lock_guard<std::mutex> summary(summaryMu_);
            frozenSamples = lastSummary_.lastPresentedAudioSamples;
        }
        std::ostringstream diagnostic;
        auto field = [&](const char* name, auto value, bool known = true) {
            diagnostic << ' ' << name << '=';
            if (known) diagnostic << value;
            else diagnostic << "unavailable";
        };
        diagnostic << "session=" << session << " wait=" << waitReason
            << " retained_session=" << fpgaSession_.load()
            << " snapshot_active_us=" << playbackActiveUs()
            << " current=" << (playEpoch_.load() == epoch) << " stop=" << stop_.load()
            << " source_opened=" << demuxOpened << " demux_return_kind=" << demuxReturnKind
            << " demux_error=" << fpgaTerminalText(redactSensitive(demuxError))
            << " player_error=" << fpgaTerminalText(redactSensitive(lastError()))
            << ' ' << formatCompressedDiagnostics(queued)
            << " pending_au=" << pending.size() << " submitted_au=" << sequence
            << " presented_au=" << presentCount_.load()
            << " drain_attempted=" << drainAttempted
            << " audio_exit=" << fpgaAudioExitName(fpgaAudioExit_.load())
            << " audio_done=" << fpgaAudioDone_.load();
        field("demux_return", demuxReturn.value_or(0), demuxReturn.has_value());
        field("held_pts", heldPts, heldPts != ddr_bitstream_ring::kNoTimestamp);
        field("held_tb_num", heldTimebaseNum, heldPts != ddr_bitstream_ring::kNoTimestamp);
        field("held_tb_den", heldTimebaseDen, heldPts != ddr_bitstream_ring::kNoTimestamp);
        field("pending_first_pts", pending.empty() ? 0 : pending.front().pts, !pending.empty());
        field("pending_last_pts", pending.empty() ? 0 : pending.back().pts, !pending.empty());
        field("pending_tb_num", pending.empty() ? 0 : pending.front().num, !pending.empty());
        field("pending_tb_den", pending.empty() ? 0 : pending.front().den, !pending.empty());
        field("drain_ack", drainAck.value_or(false), drainAck.has_value());
        const int audioProgress = fpgaAudioProgress_.load();
        field("audio_progress", audioProgress, audioProgress >= 0);
        diagnostic << " audio_progress_call=" <<
            (audioProgress >= 0 ? "returned" : audioProgress == -2 ? "in-flight" : "not-called");
        field("audio_unsubmitted_pcm", fpgaAudioHeldBytes_.load(), audioBegun);
        field("submitted_pcm", audioBytes_.load(), audioBegun);
        const auto dmaQueued = audioQueuedBytes_.load();
        const auto cachedConsumed = fpgaAudioConsumedSamples_.load();
        field("queued_dma_bytes", dmaQueued,
              audioBegun && fpgaHasConsumedClock_.load() && dmaQueued >= 0);
        field("cached_live_samples", cachedConsumed,
              fpgaHasConsumedClock_.load() && cachedConsumed >= 0);
        field("cached_clock_change_age_us", playbackActiveUs() - fpgaAudioClockChangedUs_.load(),
              fpgaHasConsumedClock_.load());
        field("presentation_age_us", playbackActiveUs() - lastCommitActiveUs,
              fpgaVideoStarted_.load());
        diagnostic << " mast_read=" << (liveValid ? "ok" : audioBegun ? "failed" : "not-started");
        field("mast_read_active_us", liveReadAtUs.value_or(0), liveReadAtUs.has_value());
        field("mast_age_us", playbackActiveUs() - liveReadAtUs.value_or(0),
              liveReadAtUs.has_value());
        field("mast_session", live.session_id, liveValid);
        field("mast_nonce", live.nonce, liveValid);
        field("mast_publication", live.publication, liveValid);
        field("mast_consumed", live.samples_consumed, liveValid);
        field("mast_active", live.active, liveValid);
        field("mast_paused", live.paused, liveValid);
        field("mast_read_pending", live.read_pending, liveValid);
        field("mast_prefetched", live.prefetched, liveValid);
        field("mast_error", unsigned(live.error), liveValid);
        field("mvps_frozen_samples", frozenSamples, frozenSamples >= 0);
        field("submitted_bytes", bytes);   // SPS-gate L3: AccessUnit bytes written (the GATE line's au_bytes)
        return diagnostic.str();
    };
    terminal = finishFpgaPlayback({begun, eof, fullyDrained, presentCount_.load() > 0},
        captureTerminal, [&] {
        MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::Quiesce, tracePauseClock_);
            span.serviceBegin();)
        demux.requestStop();
        stopOverlay.store(true);
        if (overlayThread.joinable()) overlayThread.join();
        overlayPublisher.quiesce(); // the lab publisher; a no-op when it never started
        MPX_AV_TRACE(overlayWriter.reset();)
        if (audioThr_.joinable()) audioThr_.join();
        std::lock_guard<std::mutex> lock(inprocMu_);
        inprocPcm_ = nullptr;
        MPX_AV_TRACE(span.serviceEnd(); span.value(0, 1); span.force();)
    }, [&] {
        MPX_AV_TRACE(AvTraceSpan span(traceWriter, FpgaAvEvent::Release, tracePauseClock_);
            AvTraceRelease release{span};)
        std::lock_guard<std::mutex> control(pauseControlMu_);
        std::lock_guard<std::mutex> present(presentMu_);
        MPX_AV_TRACE(span.acquired(); span.serviceBegin();)
        if (overlayNonce) {
            fpga_overlay::Model hidden;
            hidden.state = fpga_overlay::State::Hidden;
            hidden.flags = 0;
            bool hiddenSent;
            MPX_AV_TRACE({
                AvTraceSpan hiddenSpan(traceWriter, FpgaAvEvent::OverlaySend, tracePauseClock_);
                hiddenSpan.value(1, static_cast<int>(hidden.state));
                hiddenSpan.uvalue(2, overlaySequence); hiddenSpan.serviceBegin();)
            hiddenSent = fpga_overlay::send(fpga_, session, overlayNonce, overlaySequence, hidden);
            MPX_AV_TRACE(hiddenSpan.serviceEnd(); hiddenSpan.value(0, hiddenSent);
                hiddenSpan.force(); })
            if (!hiddenSent)
                log("ERROR native overlay hide transfer failed; lifecycle fence must clear the plane");
        }
        if (overlayLabFlags)
            log(formatOverlaySessionSends(overlayNonce != 0, overlaySessionSends.load(),
                                          overlayNonce ? 1 : 0, overlayBuildId));
        bool released = !begun;
        if (begun) {
            bool audioReset = true;
            if (audioBegun) {
                resetAttempted = true;
                audioReset = controlFpgaAudio(session, AudioSessionControl::Reset);
                releaseAudioReset = audioReset;
            }
            flushAttempted = audioReset;
            const bool flushed = audioReset && fpga_.flushBitstreamSession(session, 500);
            if (audioReset) releaseFlush = flushed;
            endAttempted = flushed;
            const bool ended = flushed && fpga_.endBitstreamSession(session, 500);
            if (flushed) releaseEnd = ended;
            released = ended;
            if (!ended) {
                failFpgaSession("audio/DPB/display/ingress reset ACK failed; attempting fenced abort: " +
                                fpga_.lastError());
                abortAttempted = true;
                released = fpga_.abortFpgaVideoSession(session, 2000);
                releaseAbort = released;
                if (!released)
                    log("ERROR FPGA H.264: reset ownership retained; new play must recover this epoch: " +
                        fpga_.lastError());
            }
        }
        // Core gate: a core swap since this epoch began took its reset fence with the
        // old core instance, so nothing can ACK it. Drop the ownership now, or the idle
        // repaint and the next play stay blocked behind a dead epoch. `released` (the
        // receipt) still says no ACK came.
        if (begun && !released && coreGateEpochIsStale(session, fpgaSessionCoreChanges_)) {
            log("media: core gate: epoch " + std::to_string(session) +
                " died with the swapped core; ownership dropped");
            fpga_.dropStaleSession();
            fpgaSession_.store(0);
            if (fpgaAudioOutput_ >= 0) {
                ::close(fpgaAudioOutput_);
                fpgaAudioOutput_ = -1;
            }
        }
        if (begun && released) {
            fpgaSession_.store(0);
            if (fpgaAudioOutput_ >= 0) {
                ::close(fpgaAudioOutput_);
                fpgaAudioOutput_ = -1;
            }
        }
        MPX_AV_TRACE(span.serviceEnd(); span.value(0, released); span.force();)
        return released;
    }, [&] { return playEpoch_.load() == epoch; }, [&] { return stop_.load(); },
        [&](const FpgaTerminalReceipt& receipt) {
            auto ack = [](std::optional<bool> value, bool attempted) {
                return !attempted ? "not-attempted" : !value ? "unavailable" :
                       *value ? "ok" : "failed";
            };
            log(formatFpgaTerminal(receipt) + " reset_ack=" + ack(releaseAudioReset, resetAttempted) +
                " flush_ack=" + ack(releaseFlush, flushAttempted) +
                " end_ack=" + ack(releaseEnd, endAttempted) +
                " abort_ack=" + ack(releaseAbort, abortAttempted) +
                " audio_exit_final=" + fpgaAudioExitName(fpgaAudioExit_.load()));
        });
    {
        std::lock_guard<std::mutex> lock(summaryMu_);
        lastSummary_.streamEnabled = true;
        lastSummary_.totalBytes = bytes;
        lastSummary_.presentedFrames = presentCount_.load();
        lastSummary_.videoEof = terminal == PlaybackTerminalState::Ended;
    }
    if (eof) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            range_retry_ = 0;
        }
        log("media: http_range session ended cleanly bytes=" + std::to_string(bytes) +
            " range_retry_reset");
    }
#endif
    // SPS-gate L3: a pending gate fallback for THIS epoch replaces the terminal report. No 'stopped' (comp.closedStop
    // would close Plex Web's player) and never 'ended' (auto-next): the companion stays buffering until the fallback
    // play reports. A user stop or a new play bumped the epoch and cancels it.
    std::optional<GateFallbackRequest> gateFallback;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (gateFallbackPending_ && gateFallbackEpoch_ == epoch && playEpoch_.load() == epoch)
            gateFallback = std::move(gateFallbackPending_);
        gateFallbackPending_.reset();
    }
    if (gateFallback && gateFallbackHandler_) {
        log("media: GATE fallback hand-off " + gateFallback->row + " offset_ms=" +
            std::to_string(gateFallback->offsetMs) + ": no terminal report for this play; the fallback play reports");
        gateFallbackHandler_(*gateFallback);
    } else if (onProgress_) {
        reportFpgaPlaybackTerminal(terminal, [&] { return playEpoch_.load() == epoch; },
            [&] { return stop_.load(); }, onProgress_, positionMs_.load(), durationMs);
    }
    if (boundedReopenPts_.load() <= 0)
        playing_.store(false);
    // 174 L4/L7: a latched profile_idc refuse left the next Utility leg with a
    // black HDMI picture. Drop the epoch and the present latch once this play
    // has released the session. A fallback play still owns the epoch.
    if (!gateFallback && !gateErrorText().empty() && fpgaSession_.load() == 0 &&
        boundedReopenPts_.load() <= 0) {
        // r1EntryStreamClear refuses while PLXH is still in session. The
        // destructor ends it a few lines later, which is too late for the clear.
        plxhPolicy_.endSession();
        playEpoch_.fetch_add(1, std::memory_order_acq_rel);
        std::string why;
        const bool cleared = r1EntryStreamClear(&why);
        const std::string line = std::string("media: gate-error stream clear=") +
                                 (cleared ? "1" : "0") + (why.empty() ? "" : " " + why);
        log(line);
        std::fprintf(stderr, "misterplexd: %s\n", line.c_str());
    }
#if MPX_FPGA_AV_TRACE && defined(MPX_HAVE_LIBAV)
    // Natural EOF is reported first. Explicit Stop still joins this worker;
    // the bounded dump's measured cost is therefore visible in Stop latency.
    traceWriter.finish();
    try {
        if (!avTrace)
            log("FPGA_AV_TRACE_UNAVAILABLE allocation_failed=1");
        else if (!avTrace->freeze())
            log("FPGA_AV_TRACE_UNAVAILABLE producers_not_quiescent=1");
        else
            avTrace->dump([&](const std::string& chunk) { log(chunk); });
    } catch (...) {
        try { log("FPGA_AV_TRACE_UNAVAILABLE dump_failed=1"); } catch (...) {}
    }
#endif
    // Compressed path previously skipped host idle restore on natural EOF, so
    // glass kept the last decoded AU. stop() also paints after join; this covers
    // unattended end-of-title without a companion Stop.
    if (!shuttingDown_.load() && !holdLastFrameAtEnd() &&
        boundedReopenPts_.load() <= 0) {
        paintIdle();
        startIdle();
    }
}

pid_t MediaPlayer::spawnFfmpeg(const std::vector<std::string>& args, int vWriteFd, int aWriteFd) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setpgid(0, 0);
        // Do NOT pin ffmpeg to a single core (CPU1-only pin regressed 720p
        // to ~0.3 pfps). Isolated -threads 2 is 32 fps / 1.78 cores. Play
        // produce is the limiter (read_us≈53 ms; present idle ~38 ms) — do
        // not nice decode below present. memcpy 12 ms is not the 16 fps hole
        // (ac18 copy_us=0 still 15.6; 6ffa quiet-FPGA still 15.9).
        ::setpriority(PRIO_PROCESS, 0, -5);
        // Video → stdout (pipe:1)
        if (vWriteFd >= 0) {
            dup2(vWriteFd, STDOUT_FILENO);
        }
        // Audio → fd 3 (pipe:3) when enabled. Keep write end open across the
        // mass close below (fd 3 must survive).
        if (aWriteFd >= 0) {
            if (aWriteFd != 3) {
                dup2(aWriteFd, 3);
                if (aWriteFd != STDOUT_FILENO && aWriteFd != 3)
                    ::close(aWriteFd);
            }
        } else if (vWriteFd != 3) {
            // Video-only (720p split / demux): drop inherited fd 3 so this
            // process cannot hold the sibling audio write end or vpipe read end.
            ::close(3);
        }
        if (vWriteFd >= 0 && vWriteFd != STDOUT_FILENO && vWriteFd != 3)
            ::close(vWriteFd);

        // Lab: capture FFmpeg errors on USB (tmpfs /tmp is tiny). Product: /dev/null.
        int errfd = ::open("/media/usb0/misterplex-lab/logs/ffmpeg.err",
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errfd < 0)
            errfd = ::open("/dev/null", O_WRONLY);
        if (errfd >= 0) {
            dup2(errfd, STDERR_FILENO);
            if (errfd != STDERR_FILENO && errfd != 3 && errfd != STDOUT_FILENO)
                ::close(errfd);
        }
        // Close inherited fds but KEEP 0,1,2,3 (stdin/out/err + audio pipe:3).
        for (int fd = 4; fd < 256; ++fd)
            ::close(fd);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execv(args[0].c_str(), argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}

pid_t MediaPlayer::spawnStreamDemux(const std::string& url, const std::string& headers,
                                    int64_t startMs, int writeFd) {
    // Lightweight copy-demux: annex-B elementary for host recon + F3 (no re-encode).
    // Prefer direct elementary when already annex-B (.264); otherwise remux via BSF.
    const bool elementary = looksElementaryH264(url);
    std::vector<std::string> args;
    args.push_back(ffmpeg_);
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostdin");
    // Skip -ss when PMS universal already baked offset= (seconds) into the URL.
    if (transcode_offset::demuxSeekMs(url, startMs) > 0) {
        char ss[32];
        std::snprintf(ss, sizeof(ss), "%.3f", startMs / 1000.0);
        args.push_back("-ss");
        args.push_back(ss);
    }
    if (!headers.empty()) {
        std::string h = headers;
        if (h.size() < 2 || h[h.size() - 1] != '\n')
            h += "\r\n";
        args.push_back("-headers");
        args.push_back(h);
        args.push_back("-reconnect");
        args.push_back("1");
        args.push_back("-reconnect_streamed");
        args.push_back("1");
    }
    if (elementary) {
        // Raw annex-B: declare format so FFmpeg does not probe as MP4.
        args.push_back("-f");
        args.push_back("h264");
    }
    args.push_back("-i");
    args.push_back(url);
    args.push_back("-map");
    args.push_back("0:v:0");
    args.push_back("-an");
    args.push_back("-c:v");
    args.push_back("copy");
    if (!elementary) {
        args.push_back("-bsf:v");
        args.push_back("h264_mp4toannexb");
    }
    args.push_back("-f");
    args.push_back("h264");
    args.push_back("pipe:1");
    return spawnFfmpeg(args, writeFd, -1);
}

pid_t MediaPlayer::spawnHttpRemuxMpegts(const std::string& url, const std::string& headers,
                                        int64_t startMs, const std::string& fifoPath,
                                        bool keepAudio, const std::string& audioFifo) {
    // Box ffmpeg has HTTP. ARM inproc libav is file+h264+mpegts, no HTTP, no AAC
    // (--disable-network --disable-everything --enable-decoder=h264).
    // keepAudio: ONE remux writes annex-B + 48k s16le PCM (no spawnAudioOnly
    // second HTTP; that locked av_drift_ms≈−5000). mpegts+inproc AAC SIGABRT'd.
    const bool pcmOut = keepAudio && !audioFifo.empty();
    std::vector<std::string> args;
    args.push_back(ffmpeg_);
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostdin");
    args.push_back("-fflags");
    args.push_back("+nobuffer");
    args.push_back("-probesize");
    args.push_back("32768");
    args.push_back("-analyzeduration");
    args.push_back("0");
    if (transcode_offset::demuxSeekMs(url, startMs) > 0) {
        char ss[32];
        std::snprintf(ss, sizeof(ss), "%.3f", startMs / 1000.0);
        args.push_back("-ss");
        args.push_back(ss);
    }
    if (!headers.empty()) {
        std::string h = headers;
        if (h.size() < 2 || h[h.size() - 1] != '\n')
            h += "\r\n";
        args.push_back("-headers");
        args.push_back(h);
        args.push_back("-reconnect");
        args.push_back("1");
        args.push_back("-reconnect_streamed");
        args.push_back("1");
        args.push_back("-reconnect_delay_max");
        args.push_back("5");
    }
    args.push_back("-i");
    args.push_back(url);
    args.push_back("-map");
    args.push_back("0:v:0");
    if (!pcmOut)
        args.push_back("-an");
    args.push_back("-c:v");
    args.push_back("copy");
    args.push_back("-bsf:v");
    args.push_back("h264_mp4toannexb");
    args.push_back("-f");
    args.push_back("h264");
    args.push_back("-y");
    args.push_back(fifoPath);
    if (pcmOut) {
        args.push_back("-map");
        args.push_back("0:a:0?");
        args.push_back("-vn");
        args.push_back("-af");
        if (audioDelayMs_ > 0)
            args.push_back("aresample=48000,adelay=" + std::to_string(audioDelayMs_) +
                           ":all=1");
        else
            args.push_back("aresample=48000");
        args.push_back("-f");
        args.push_back("s16le");
        args.push_back("-ac");
        args.push_back("2");
        args.push_back("-ar");
        args.push_back("48000");
        args.push_back("-y");
        args.push_back(audioFifo);
    }
    return spawnFfmpeg(args, -1, -1);
}

pid_t MediaPlayer::spawnHttpPrefetchTs(const std::string& url, const std::string& headers,
                                       int64_t startMs, const std::string& destPath) {
    // Copy A+V to a seekable tmpfs mpegts. Inproc of a complete file is the
    // local-identity 24 unique path; fifo remux of the same HTTP is 23.5.
    std::vector<std::string> args;
    args.push_back(ffmpeg_);
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostdin");
    if (transcode_offset::demuxSeekMs(url, startMs) > 0) {
        char ss[32];
        std::snprintf(ss, sizeof(ss), "%.3f", startMs / 1000.0);
        args.push_back("-ss");
        args.push_back(ss);
    }
    if (!headers.empty()) {
        std::string h = headers;
        if (h.size() < 2 || h[h.size() - 1] != '\n')
            h += "\r\n";
        args.push_back("-headers");
        args.push_back(h);
        args.push_back("-reconnect");
        args.push_back("1");
        args.push_back("-reconnect_streamed");
        args.push_back("1");
        args.push_back("-reconnect_delay_max");
        args.push_back("5");
    }
    args.push_back("-i");
    args.push_back(url);
    args.push_back("-c");
    args.push_back("copy");
    args.push_back("-muxpreload");
    args.push_back("0");
    args.push_back("-muxdelay");
    args.push_back("0");
    args.push_back("-f");
    args.push_back("mp4");
    args.push_back("-movflags");
    args.push_back("+faststart");
    args.push_back("-y");
    args.push_back(destPath);
    return spawnFfmpeg(args, -1, -1);
}

pid_t MediaPlayer::spawnAudioOnly(const std::string& url, const std::string& headers, int64_t startMs,
                                  int aWriteFd) {
    // Audio-only FFmpeg — frees dual-A9 when host recon owns F1 (no RGB scale/decode).
    std::vector<std::string> args;
    args.push_back(ffmpeg_);
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostdin");
    // Second HTTP transcode for inproc audio. Default probe waits seconds
    // (Star Trek av_drift_ms≈−5200). Nobuffer so first PCM matches pictures.
    args.push_back("-fflags");
    args.push_back("+nobuffer+genpts");
    args.push_back("-flags");
    args.push_back("low_delay");
    args.push_back("-probesize");
    args.push_back("32768");
    args.push_back("-analyzeduration");
    args.push_back("0");
    if (transcode_offset::demuxSeekMs(url, startMs) > 0) {
        char ss[32];
        std::snprintf(ss, sizeof(ss), "%.3f", startMs / 1000.0);
        args.push_back("-ss");
        args.push_back(ss);
    }
    if (!headers.empty()) {
        std::string h = headers;
        if (h.size() < 2 || h[h.size() - 1] != '\n')
            h += "\r\n";
        args.push_back("-headers");
        args.push_back(h);
        args.push_back("-reconnect");
        args.push_back("1");
        args.push_back("-reconnect_streamed");
        args.push_back("1");
        args.push_back("-reconnect_delay_max");
        args.push_back("5");
    }
    args.push_back("-i");
    args.push_back(url);
    args.push_back("-vn");
    args.push_back("-map");
    args.push_back("0:a:0?");
    args.push_back("-af");
    if (audioDelayMs_ > 0)
        args.push_back("aresample=48000,adelay=" + std::to_string(audioDelayMs_) + ":all=1");
    else
        args.push_back("aresample=48000");
    args.push_back("-f");
    args.push_back("s16le");
    args.push_back("-ac");
    args.push_back("2");
    args.push_back("-ar");
    args.push_back("48000");
    args.push_back("pipe:3");
    return spawnFfmpeg(args, /*vWriteFd*/ -1, aWriteFd);
}

void MediaPlayer::streamPump(int sfd, bool allowF1Present) {
    // Phase 3.3i/product: demux annex-B → host I-slice recon → YUV420 F1 (+ optional fb0).
    // Also feed the FPGA decoder through the continuous HPS-DDR bitstream ring.
    // Robust multi-IDR: retain last SPS/PPS, recon every I/IDR, sticky CABAC skip.
    const bool wantF3 = fpga_.ok();
    const bool wantF1 =
        allowF1Present && fpga_.ok() &&
        (presentMode_ == "fpga" || presentMode_ == "both");
    // PRESENT=both: FFmpeg owns continuous fb0; recon owns F1 only.
    // PRESENT=fb0 + STREAM: recon I-frames may blit fb0 (sparse keyframe present).
    const bool reconToFb =
        fb_.ok() && (presentMode_ == "fb0" || presentMode_.empty());

    auto formatDdrBitstreamStatus = [](const FpgaSpi::BitstreamStatus& st) {
        return std::string("ddr_status session=") + std::to_string(st.session_id) +
               " active=" + (st.active ? "1" : "0") +
               " paused=" + (st.paused ? "1" : "0") +
               " ring=" + std::to_string(st.ring_level) + "/" +
               std::to_string(st.ring_capacity) +
               " producer_bytes=" + std::to_string(st.producer_count) +
               " consumer_bytes=" + std::to_string(st.consumer_count) +
               " consumer_seq=" + std::to_string(st.consumer_seq) +
               " underrun=" + std::to_string(st.underrun_count) +
               " overrun=" + std::to_string(st.overrun_count) +
               " desync=" + std::to_string(st.desync_count) +
               " last_bad_seq=" + std::to_string(st.last_bad_seq) +
               " flags=u" + (st.underrun ? "1" : "0") +
               "o" + (st.overrun ? "1" : "0") +
               "d" + (st.desync ? "1" : "0") +
               "f" + (st.fatal ? "1" : "0");
    };
    auto readDdrBitstreamStatusString = [&]() {
        FpgaSpi::BitstreamStatus st;
        if (!fpga_.readBitstreamStatus(st))
            return std::string("ddr_status=unreadable err=") + fpga_.lastError();
        return formatDdrBitstreamStatus(st);
    };

    streamActive_.store(true);
    reconFrames_.store(0);
    reconPresentOk_.store(false);
    // cabacSkip_ is session-level (cleared in play()); do not clear here on mid-session re-entry.
    log(std::string("media: STREAM=1 host I-slice recon") +
        (wantF1 ? " →F1" : "") + (wantF3 ? " +DDR-bitstream" : "") +
        (reconToFb ? " +fb0" : ""));

    // Bound NAL scan buffer (SPS+PPS+IDR can be large at 720p; cap for dual-A9)
    constexpr size_t kMaxAcc = 2 * 1024 * 1024;
    std::vector<uint8_t> acc;
    acc.reserve(64 * 1024);
    // Last complete NAL start (start-code index) still in acc; incomplete NAL retained
    size_t parseFrom = 0;

    FpgaBitstreamProducer f3Producer(fpga_);
    h264stream::DispatchConfig f3Cfg;
    f3Cfg.max_full_retries = 50;    // Full is transient: retry for ~100 ms.
    f3Cfg.full_retry_sleep_ms = 2;
    h264stream::NalDispatcher f3Dispatch(f3Producer, f3Cfg);
    bool f3Active = false;
    bool f3Fatal = false;
    bool f3Paused = false;
    static std::atomic<uint64_t> nextStreamSession{1};
    const uint64_t streamSession = nextStreamSession.fetch_add(1);
    if (wantF3) {
        const auto br = f3Dispatch.begin(streamSession);
        if (br == h264stream::ControlResult::Ok) {
            f3Active = true;
            log("media: F3 NAL producer begin session=" + std::to_string(streamSession) +
                " " + readDdrBitstreamStatusString());
        } else {
            f3Fatal = true;
            log("media: F3 NAL producer begin failed " +
                std::string(h264stream::toString(br)) + " — F3 disabled");
        }
    }
    const auto streamWall0 = std::chrono::steady_clock::now();
    const int64_t streamCpu0 = threadCpuMicros();

    std::vector<uint8_t> spsNal; // includes start code
    std::vector<uint8_t> ppsNal;
    std::vector<uint8_t> yuv420p;
    char buf[4096];
    size_t f3Total = 0;
    size_t f3Pushes = 0;
    size_t reconOk = 0;
    size_t reconFail = 0;
    size_t idrSeen = 0;
    size_t iSliceSeen = 0;
    bool cabacLogged = cabacSkip_.load();
    bool menuReloadIssued = false;
    // Throttle sparse host recon F1 publishing; product rawvideo owns continuous playback.
    constexpr size_t kReconPresentEvery = 1;
    bool reconDdrMismatchLogged = false;

    auto syncF3Pause = [&]() {
        if (!f3Active || f3Fatal)
            return;
        const bool paused = paused_.load();
        if (paused && !f3Paused) {
            const auto r = f3Dispatch.pause();
            if (r == h264stream::ControlResult::Ok) {
                f3Paused = true;
                log("media: F3 NAL producer pause session=" + std::to_string(streamSession) +
                    " (HTTP demux kept alive; FPGA holds last frame)");
            } else {
                f3Fatal = true;
                log("media: F3 NAL producer pause failed " +
                    std::string(h264stream::toString(r)));
            }
        } else if (!paused && f3Paused) {
            const auto r = f3Dispatch.resume();
            if (r == h264stream::ControlResult::Ok) {
                f3Paused = false;
                log("media: F3 NAL producer resume session=" + std::to_string(streamSession) +
                    " (no SPS/PPS replay; STREAM=1 pause is unsupported)");
            } else {
                f3Fatal = true;
                log("media: F3 NAL producer resume failed " +
                    std::string(h264stream::toString(r)));
            }
        }
    };

    auto pushF3Nal = [&](const uint8_t* nalSc, size_t nalLen) {
        if (!f3Active || f3Fatal || !wantF3)
            return;
        const uint64_t beforeNals = f3Dispatch.stats().nal_pushed;
        const auto r = f3Dispatch.handleNal(nalSc, nalLen);
        const auto& after = f3Dispatch.stats();
        f3Total = static_cast<size_t>(after.bytes_pushed);
        f3Pushes = static_cast<size_t>(after.nal_pushed);
        if (r == h264stream::PushResult::Ok) {
            if (after.nal_pushed != beforeNals && (after.nal_pushed % 64) == 0)
                log("media: F3 NAL stream nals=" + std::to_string(after.nal_pushed) +
                    " bytes=" + std::to_string(after.bytes_pushed));
            return;
        }
        if (r == h264stream::PushResult::Full) {
            f3Fatal = true;
            log("ERROR media: F3 NAL producer Full persisted after bounded retry; resetting session " +
                readDdrBitstreamStatusString());
        } else {
            f3Fatal = true;
            log("ERROR media: F3 NAL producer " + std::string(h264stream::toString(r)) +
                " — resetting session " + readDdrBitstreamStatusString());
        }
        if (f3Active)
            f3Dispatch.end();
    };

    auto presentRecon = [&](const recon::ReconResult& rec) {
        if (rec.y.empty() || rec.u.empty() || rec.v.empty() || rec.width <= 0 ||
            rec.height <= 0 || (rec.width & 1) || (rec.height & 1))
            return false;
        const size_t yBytes = static_cast<size_t>(rec.width) * static_cast<size_t>(rec.height);
        const size_t cBytes = yBytes / 4u;
        if (rec.y.size() < yBytes || rec.u.size() < cBytes || rec.v.size() < cBytes)
            return false;
        auto ensureYuv420p = [&]() -> const uint8_t* {
            if (yuv420p.empty()) {
                yuv420p.resize(yBytes + 2u * cBytes);
                std::memcpy(yuv420p.data(), rec.y.data(), yBytes);
                std::memcpy(yuv420p.data() + yBytes, rec.u.data(), cBytes);
                std::memcpy(yuv420p.data() + yBytes + cBytes, rec.v.data(), cBytes);
            }
            return yuv420p.data();
        };
        yuv420p.clear();
        bool any = false;
        if (wantF1) {
            // C3 frame-store RTL is YUV-only. Never send RGB565 to the DDR doorbell.
            bool ok = false;
            if (useDdrF1_) {
                // Match play path: geometry from current decode bank, not 480-only.
                const DdrFrameGeometry g = ddrFrameGeometryForPresentedSize(outW_, outH_);
                if (rec.width == g.coded_width && rec.height == g.coded_height) {
                    ensureYuv420p();
                    clearYuv420pCropPadding(yuv420p.data(), g);
                    if (uvUBias_ != 0 || uvVBias_ != 0) {
                        applyYuv420pUvBias(yuv420p.data(), rec.width, rec.height, uvUBias_,
                                           uvVBias_);
                    }
                    ok = fpga_.sendYuv420pFrameDdr(yuv420p.data(), yuv420p.size(), g, ddrBank_);
                    if (ok)
                        ddrBank_ ^= 1;
                    if (!ok) {
                        log("media: recon YUV420 DDR F1 unavailable: " + fpga_.lastError());
                    } else if ((reconOk % 30) == 0) {
                        log("media: recon F1 via YUV420 DDR " +
                            std::to_string(static_cast<int>(fpga_.lastPushMs())) + "ms");
                    }
                } else if (!reconDdrMismatchLogged) {
                    reconDdrMismatchLogged = true;
                    log("media: recon F1 skipped: YUV DDR frame-store expects coded " +
                        std::to_string(g.coded_width) + "x" + std::to_string(g.coded_height) +
                        ", got " + std::to_string(rec.width) + "x" + std::to_string(rec.height));
                }
            }
            if (ok)
                any = true;
        }
        if (reconToFb && fb_.ok() && CoreGate::process().open()) {
            ensureYuv420p();
            if (fb_.blitYuv420p(yuv420p.data(), rec.width, rec.height))
                any = true;
        }
        if (any) {
            reconPresentOk_.store(true);
            reconFrames_.fetch_add(1);
        }
        return any;
    };

    // Lightweight slice_type probe (first_mb ue + slice_type ue) — skip P/B walks.
    auto isISliceNal = [](const uint8_t* nalSc, size_t nalLen) -> bool {
        size_t sc = annexBStartLen(nalSc, nalLen, 0);
        if (!sc || sc >= nalLen)
            return false;
        const uint8_t ntype = nalSc[sc] & 0x1f;
        if (ntype == 5)
            return true; // IDR is always I
        if (ntype != 1)
            return false;
        const uint8_t* pay = nalSc + sc + 1;
        const size_t plen = nalLen - sc - 1;
        if (plen < 1)
            return false;
        auto rbsp = misterplex::detail::removeEpb(pay, plen);
        misterplex::detail::BitReader br(rbsp.data(), rbsp.size());
        br.ue(); // first_mb_in_slice
        uint32_t st = br.ue(); // slice_type
        if (!br.ok)
            return false;
        // 2 or 7 = I (spec allows slice_type % 5)
        return (st % 5) == 2;
    };

    // PPS entropy_coding_mode_flag drives sticky CABAC. In-band SPS/PPS before every
    // IDR used to clear skip → dual-A9 residual walk failed every keyframe on High.
    // Policy: CABAC PPS sets sticky immediately; CAVLC PPS clears for re-probe; SPS no-op.
    auto applyPpsEntropy = [&](const uint8_t* nalSc, size_t nalLen) {
        size_t sc = annexBStartLen(nalSc, nalLen, 0);
        if (!sc || sc + 1 >= nalLen)
            return;
        const uint8_t* pay = nalSc + sc + 1;
        const size_t plen = nalLen - sc - 1;
        if (plen < 1)
            return;
        auto rbsp = misterplex::detail::removeEpb(pay, plen);
        misterplex::detail::BitReader br(rbsp.data(), rbsp.size());
        br.ue(); // pic_parameter_set_id
        br.ue(); // seq_parameter_set_id
        const bool cabac = br.u(1) != 0;
        if (!br.ok)
            return;
        int codedW = sourceMediaW_;
        int codedH = sourceMediaH_;
        if (!spsNal.empty()) {
            const size_t sc = annexBStartLen(spsNal.data(), spsNal.size(), 0);
            if (sc + 1 < spsNal.size()) {
                const SpsInfo sps =
                    parseSpsRbsp(spsNal.data() + sc + 1, spsNal.size() - sc - 1);
                if (sps.valid) {
                    codedW = static_cast<int>(sps.width);
                    codedH = static_cast<int>(sps.height);
                }
            }
        }
        const uint32_t paired =
            pairedBuildForEntropy(codedW, codedH, sourceInterlaced_, cabac);
        char pairedHex[16];
        std::snprintf(pairedHex, sizeof(pairedHex), "0x%08X", paired);
        const uint32_t loaded = CoreGate::process().buildId();
        // loaded 0 means the gate has not published a build yet. Do not consume
        // the one-shot; the next PPS retries.
        if (!menuReloadIssued && loaded != 0) {
            if (issueMenuReloadForPair(loaded, paired,
                                       [this](const std::string& line) { log(line); }))
                menuReloadIssued = true;
            else if (paired != loaded) {
                menuReloadIssued = true;
                char loadedHex[16];
                std::snprintf(loadedHex, sizeof(loadedHex), "0x%08X", loaded);
                log(std::string("media: menu reload skipped paired ") + pairedHex +
                    " loaded " + loadedHex + "; no named Utility file");
            }
        }
        if (cabac) {
            cabacSkip_.store(true);
            if (!cabacLogged) {
                cabacLogged = true;
                log(std::string("media: recon CABAC/High — PPS entropy_coding_mode=1; host CAVLC skip "
                    "(sticky). Stream is High/CABAC; MiSTerPlex.xml profile may be missing "
                    "or inactive on PMS. Use STREAM_SKIP_RGB=0/PRESENT=both for fb0 fallback. "
                    "paired ") + pairedHex + " log-only");
            }
        } else {
            // CAVLC PPS: allow I-slice recon (seek/segment may flip profile).
            if (cabacSkip_.load() && cabacLogged) {
                log(std::string("media: recon CAVLC PPS — sticky CABAC cleared; host I-slice recon "
                    "re-enabled. paired ") + pairedHex + " log-only");
            }
            cabacSkip_.store(false);
            cabacLogged = false;
        }
    };

    auto tryReconNal = [&](const uint8_t* nalSc, size_t nalLen, uint8_t ntype) {
        if (spsNal.empty() || ppsNal.empty())
            return;
        if (ntype != 5 && ntype != 1)
            return;
        if (!isISliceNal(nalSc, nalLen))
            return;
        ++iSliceSeen;
        if (ntype == 5)
            ++idrSeen;

        // Sticky CABAC: do not burn dual-A9 walking High-profile every keyframe.
        if (cabacSkip_.load()) {
            if (ntype == 5 && (idrSeen % 16) == 1)
                log("media: recon skip CABAC/High (sticky) idr=" + std::to_string(idrSeen) +
                    " — legacy RGB F1 path is disabled; use PRESENT=both for fb0 fallback");
            return;
        }

        std::vector<uint8_t> au;
        au.reserve(spsNal.size() + ppsNal.size() + nalLen);
        au.insert(au.end(), spsNal.begin(), spsNal.end());
        au.insert(au.end(), ppsNal.begin(), ppsNal.end());
        au.insert(au.end(), nalSc, nalSc + nalLen);

        auto rec = recon::reconISlice(au.data(), au.size());
        if (rec.mb_decoded <= 0 || rec.mb_decoded != rec.mb_total || rec.y.empty()) {
            ++reconFail;
            // Backup path: CABAC detected late in recon chain (PPS probe missed).
            if (rec.fail_reason && std::strcmp(rec.fail_reason, "cabac") == 0) {
                cabacSkip_.store(true);
                if (!cabacLogged) {
                    cabacLogged = true;
                    log("media: recon CABAC/High — host CAVLC cannot decode this stream; "
                        "stream is High/CABAC; MiSTerPlex.xml profile may be missing or "
                        "inactive on PMS. Legacy RGB F1 path is disabled; STREAM still feeds F3.");
                }
                return;
            }
            if (ntype == 5 || (reconFail % 8) == 1) {
                log("media: recon fail ntype=" + std::to_string(ntype) +
                    " mb=" + std::to_string(rec.mb_decoded) + "/" +
                    std::to_string(rec.mb_total) +
                    " reason=" + (rec.fail_reason ? rec.fail_reason : "?") +
                    " idr=" + std::to_string(idrSeen));
            }
            return;
        }
        ++reconOk;
        // Present every kReconPresentEvery successful I-slice
        if ((reconOk % kReconPresentEvery) != 0)
            return;
        if (presentRecon(rec)) {
            if (reconOk == 1 || ntype == 5 || (reconOk % 8) == 0) {
                log("media: recon frame ok #" + std::to_string(reconOk) + " " +
                    std::to_string(rec.width) + "x" + std::to_string(rec.height) +
                    " mb=" + std::to_string(rec.mb_decoded) + " idr=" + std::to_string(idrSeen) +
                    " i=" + std::to_string(iSliceSeen) +
                    " f1ms=" + std::to_string(static_cast<int>(fpga_.lastPushMs())));
            }
        }
    };

    auto consumeCompleteNals = [&]() {
        // Parse complete NALs from parseFrom; leave trailing incomplete NAL in acc.
        size_t i = parseFrom;
        // Ensure we start at a start code if possible
        while (i + 3 < acc.size()) {
            size_t sc = annexBStartLen(acc.data(), acc.size(), i);
            if (sc)
                break;
            ++i;
        }
        parseFrom = i;

        while (i + 3 < acc.size() && !stop_.load()) {
            size_t sc = annexBStartLen(acc.data(), acc.size(), i);
            if (!sc) {
                ++i;
                parseFrom = i;
                continue;
            }
            // Find next start code (end of this NAL)
            size_t j = i + sc;
            bool foundNext = false;
            while (j + 2 < acc.size()) {
                size_t nsc = annexBStartLen(acc.data(), acc.size(), j);
                if (nsc) {
                    foundNext = true;
                    break;
                }
                ++j;
            }
            if (!foundNext) {
                // Incomplete NAL — wait for more bytes
                parseFrom = i;
                return;
            }
            // Complete NAL: [i, j)
            const size_t nalLen = j - i;
            if (i + sc < j) {
                const uint8_t ntype = acc[i + sc] & 0x1f;
                pushF3Nal(acc.data() + i, nalLen);
                if (ntype == 7) {
                    // SPS alone does not change entropy mode — keep sticky CABAC.
                    spsNal.assign(acc.begin() + static_cast<std::ptrdiff_t>(i),
                                  acc.begin() + static_cast<std::ptrdiff_t>(j));
                } else if (ntype == 8) {
                    ppsNal.assign(acc.begin() + static_cast<std::ptrdiff_t>(i),
                                  acc.begin() + static_cast<std::ptrdiff_t>(j));
                    applyPpsEntropy(acc.data() + i, nalLen);
                } else if ((ntype == 5 || ntype == 1) && !paused_.load()) {
                    tryReconNal(acc.data() + i, nalLen, ntype);
                }
            }
            i = j;
            parseFrom = i;
        }
    };

    auto compactAcc = [&]() {
        // Drop fully parsed bytes; keep the trailing incomplete NAL.
        size_t drop = parseFrom;
        if (drop == 0)
            return;
        // Never drop past incomplete NAL start
        drop = std::min(drop, parseFrom);
        if (drop > 0 && drop <= acc.size()) {
            acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(drop));
            parseFrom -= drop;
        }
        // Hard cap
        if (acc.size() > kMaxAcc) {
            log("media: STREAM acc overflow — reset NAL state");
            acc.clear();
            parseFrom = 0;
            // Keep last SPS/PPS so multi-IDR can recover after overflow gap
        }
    };

    while (!stop_.load()) {
        syncF3Pause();
        ssize_t n = ::read(sfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; // demux EOF or killed (seek/stop closes pipe)
        acc.insert(acc.end(), buf, buf + n);
        consumeCompleteNals();
        compactAcc();
    }

    // EOF: process trailing NAL that has no following start code (short files / last IDR).
    // Without this, single-AU Baseline vectors never recon (IDR is last NAL).
    if (!stop_.load() && parseFrom + 3 < acc.size()) {
        size_t sc = annexBStartLen(acc.data(), acc.size(), parseFrom);
        if (sc && parseFrom + sc < acc.size()) {
            const size_t nalLen = acc.size() - parseFrom;
            const uint8_t ntype = acc[parseFrom + sc] & 0x1f;
            pushF3Nal(acc.data() + parseFrom, nalLen);
            if (ntype == 7) {
                spsNal.assign(acc.begin() + static_cast<std::ptrdiff_t>(parseFrom),
                              acc.end());
            } else if (ntype == 8) {
                ppsNal.assign(acc.begin() + static_cast<std::ptrdiff_t>(parseFrom),
                              acc.end());
                applyPpsEntropy(acc.data() + parseFrom, nalLen);
            } else if ((ntype == 5 || ntype == 1) && !paused_.load()) {
                tryReconNal(acc.data() + parseFrom, nalLen, ntype);
            }
            parseFrom = acc.size();
        }
    }

    // Flush remaining complete NALs and F3 tail (only if not mid-stop)
    if (!stop_.load()) {
        consumeCompleteNals();
    }

    FpgaSpi::BitstreamStatus ddrBeforeEnd{};
    const bool haveDdrBeforeEnd = fpga_.readBitstreamStatus(ddrBeforeEnd);
    const std::string ddrStatusBeforeEnd = haveDdrBeforeEnd
                                               ? formatDdrBitstreamStatus(ddrBeforeEnd)
                                               : (std::string("ddr_status=unreadable err=") +
                                                  fpga_.lastError());
    h264stream::Telemetry f3StatusBeforeEnd = f3Producer.status();
    if (f3Active) {
        const auto endResult = f3Dispatch.end();
        if (endResult != h264stream::ControlResult::Ok)
            log("ERROR media: F3 NAL producer end failed " +
                std::string(h264stream::toString(endResult)) + " " + ddrStatusBeforeEnd);
    }
    ::close(sfd);
    streamActive_.store(false);
    const auto streamWall1 = std::chrono::steady_clock::now();
    const int64_t streamCpu1 = threadCpuMicros();
    const int64_t streamWallMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(streamWall1 - streamWall0).count();
    const int64_t streamCpuUs = std::max<int64_t>(0, streamCpu1 - streamCpu0);
    const auto f3Stats = f3Dispatch.stats();
    const auto f3Status = f3StatusBeforeEnd;
    const bool effectivelyEmptyDelivery =
        wantF3 && f3Status.bytes_accepted > 4 && haveDdrBeforeEnd &&
        ddrBeforeEnd.consumer_count <= 4;
    if (wantF3 && (f3Status.nal_accepted == 0 || f3Status.bytes_accepted <= 4 ||
                   effectivelyEmptyDelivery || f3Fatal || f3Stats.full_escalations != 0 ||
                   f3Stats.desync_or_fatal != 0)) {
        log("ERROR media: DDR bitstream zero/effectively-empty delivery "
            "accepted_nals=" + std::to_string(f3Status.nal_accepted) +
            " accepted_bytes=" + std::to_string(f3Status.bytes_accepted) +
            " dispatcher_seen=" + std::to_string(f3Stats.nal_seen) +
            " full_retries=" + std::to_string(f3Stats.full_retries) +
            " full_escalations=" + std::to_string(f3Stats.full_escalations) +
            " desync_or_fatal=" + std::to_string(f3Stats.desync_or_fatal) +
            " effectively_empty=" + (effectivelyEmptyDelivery ? "1" : "0") +
            " " + ddrStatusBeforeEnd);
    }
    log("media: STREAM end f3_bytes=" + std::to_string(f3Total) +
        " f3_nals=" + std::to_string(f3Status.nal_accepted) +
        " f3_full_retries=" + std::to_string(f3Stats.full_retries) +
        " f3_full_escalations=" + std::to_string(f3Stats.full_escalations) +
        " f3_dropped_paused=" + std::to_string(f3Stats.nal_dropped_paused) +
        " f3_desync=" + std::to_string(f3Status.desync_count) +
        " f3_last_bad_seq=" + std::to_string(f3Status.last_bad_seq) +
        " " + ddrStatusBeforeEnd +
        " stream_wall_ms=" + std::to_string(streamWallMs) +
        " stream_cpu_us=" + std::to_string(streamCpuUs) +
        " recon_ok=" + std::to_string(reconOk) + " recon_fail=" + std::to_string(reconFail) +
        " idr=" + std::to_string(idrSeen) + " i_slices=" + std::to_string(iSliceSeen) +
        " cabac=" + (cabacSkip_.load() ? "1" : "0") +
        " present=" + std::to_string(reconFrames_.load()));
}

MrAudioStatus MediaPlayer::readMrAudioStatus() {
    const int fd = ::open(audioDev_.c_str(), O_RDONLY);
    if (fd < 0)
        return {};
    char buf[128];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    ::close(fd);
    if (n <= 0)
        return {};
    return misterplex::parseMrAudioStatus(buf, n);
}

void MediaPlayer::audioPump(int afd) {
    // Drain PCM to MrAudio. Lab evidence: MrAudio write() does NOT pace realtime
    // (audio_s grew ~3× wall → jumpy audio). Pace ourselves to exact 48 kHz wall
    // clock; that back-pressures FFmpeg and thus video.
    // F2 SPI skipped when MrAudio works (SPI thrash + no heard benefit).
    // AUDIO_DELAY_MS is applied in FFmpeg (adelay) on the product RGB path so A+V
    // stay on one clock. Pump is pure wall-48k MrAudio (no second delay line).
    // afd<0: same-demux inproc PCM (no second HTTP audio transcode).
    {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(0, &cpus);
        (void)::pthread_setaffinity_np(::pthread_self(), sizeof(cpus), &cpus);
    }
#ifdef MPX_HAVE_LIBAV
    AvInprocDecoder* same = (afd < 0) ? inprocPcm_ : nullptr;
    auto pcmRead = [&](char* buf, size_t n) -> ssize_t {
        if (same) {
            const int g = same->drainPcm(reinterpret_cast<uint8_t*>(buf), n);
            if (g > 0)
                return g;
            if (same->audioEof() || !same->isOpen())
                return 0;
            errno = EAGAIN;
            return -1;
        }
        if (afd < 0)
            return 0;
        return ::read(afd, buf, n);
    };
#else
    auto pcmRead = [&](char* buf, size_t n) -> ssize_t {
        if (afd < 0)
            return 0;
        return ::read(afd, buf, n);
    };
#endif
    auto pcmClose = [&]() {
        if (afd < 0)
            return;
        int cur = afd;
        if (remuxPcmReadFd_.compare_exchange_strong(cur, -1))
            ::close(afd);
    };
    const bool wantMr = audioEnabled_ && (::access(audioDev_.c_str(), W_OK) == 0);
    bool wantF2 = fpga_.ok() && presentMode_ == "fpga" && !wantMr;

    int out = -1;
    if (wantMr) {
        out = ::open(audioDev_.c_str(), O_WRONLY);
        if (out < 0)
            log("media: open " + audioDev_ + " failed errno=" + std::to_string(errno));
        else {
            int fl = ::fcntl(out, F_GETFL, 0);
            if (fl >= 0)
                (void)::fcntl(out, F_SETFL, fl | O_NONBLOCK);
            log("media: MrAudio open — software-paced 48kHz delay_ms=" +
                std::to_string(audioDelayMs_) +
                " clock_ppm=" + std::to_string(audioClockPpm_) +
                (afd < 0 ? " src=same_demux" : " (adelay in ffmpeg if >0)"));
            // Do not poke /dev/MiSTer_cmd here: volume SPI during play races Main
            // and was observed to truncate free-run lab sessions. Start-path
            // assertMisterFullVolume() is enough.
        }
    }
    // 480i host-paint product cores (e36+) free-run MrAudio (SESSION_CONTROL=0).
    // Do NOT block the pump on beginFpgaVideoSession: three 2s timeouts delayed
    // first PCM by ~2–6s while video already presented (lab av_drift_ms≈−2700).
    // H.264 path still arms audio in fpgaThreadMain/fpgaAudioPump.
    bool legacyAudioSession = false;
    uint64_t legacyAudioEpoch = 0;
    auto releaseLegacyAudio = [&]() {
        if (!legacyAudioSession)
            return;
        const uint64_t sid = legacyAudioEpoch ? legacyAudioEpoch : fpgaSession_.load();
        if (sid) {
            (void)controlFpgaAudio(sid, AudioSessionControl::Reset);
            if (!fpga_.endBitstreamSession(sid, 500))
                (void)fpga_.abortFpgaVideoSession(sid, 1500);
            if (fpgaSession_.load() == sid)
                fpgaSession_.store(0);
        }
        legacyAudioSession = false;
        fpgaAudioStarted_.store(false);
    };
    if (out >= 0 && !fpgaH264Backend() && backend_ == VideoBackend::LegacySoftware &&
        (presentMode_ == "fpga" || presentMode_ == "both")) {
        log("media: legacy MrAudio free-run (no session arm; host-paint ALSA)");
    }
    if (out < 0 && !wantF2) {
        char buf[4096];
        while (!stop_.load()) {
            ssize_t n = pcmRead(buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
        }
        pcmClose();
        return;
    }

    if (wantF2) {
        fpga_.flushAudioFifo();
        log("media: F2 audio_fifo streaming enabled");
    }

    audioBytes_.store(0);
    audioQueuedBytes_.store(-1);
    // Do not set audioActive until MrAudio is actually fed. audioActive=1
    // with audioBytes=0 made avDecide Hold forever (720p trim stall).
    audioActive_.store(false);
    // 20ms chunks @ 48k stereo s16le
    char buf[3840];
    std::vector<uint8_t> gatedAudio;
    gatedAudio.reserve(48000 * 4 * 4);
    if (!audioFeedRelease_.load())
        log("media: MrAudio feed gated; draining pipe until release");
    while (!stop_.load() && !audioFeedRelease_.load()) {
        ssize_t n = 0;
        if (afd >= 0) {
            struct pollfd pfd {};
            pfd.fd = afd;
            pfd.events = POLLIN;
            const int pr = ::poll(&pfd, 1, 20);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (pr == 0)
                continue;
        }
        n = pcmRead(buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            break;
        }
        if (n == 0)
            break;
        gatedAudio.insert(gatedAudio.end(), buf, buf + n);
        constexpr size_t kMaxGated = 48000u * 4u * 6u;
        if (gatedAudio.size() > kMaxGated)
            gatedAudio.erase(gatedAudio.begin(),
                             gatedAudio.begin() +
                                 static_cast<std::ptrdiff_t>(gatedAudio.size() - kMaxGated));
    }
    if (stop_.load()) {
        releaseLegacyAudio();
        if (out >= 0)
            ::close(out);
        pcmClose();
        audioActive_.store(false);
        return;
    }
    {
        const int afterMs = audioAfterVideoMs_.load();
        if (afterMs > 0) {
            log("media: MrAudio start after_video_ms=" + std::to_string(afterMs));
            const auto due = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(afterMs);
            while (!stop_.load() && std::chrono::steady_clock::now() < due)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (stop_.load()) {
        releaseLegacyAudio();
        if (out >= 0)
            ::close(out);
        pcmClose();
        audioActive_.store(false);
        return;
    }
    if (!gatedAudio.empty()) {
        const int64_t pres = presentCount_.load(std::memory_order_relaxed);
        const size_t drop = misterplex::p720_av::trimGatedPcmDrop(
            gatedAudio.size(), pres, fpsNum_, fpsDen_);
        if (drop > 0)
            gatedAudio.erase(gatedAudio.begin(),
                             gatedAudio.begin() + static_cast<std::ptrdiff_t>(drop));
        // 720p prefetch combined: 40 ms remain left pictures ~90–110 ms
        // ahead of heard audio (soak J/K). 120 ms matches 480p tens-of-ms.
        // Keep the START of the gated PCM (same epoch as the first I420).
        // 120 ms cap discarded ~1.3 s of t=0 audio on live HEVC open and
        // locked av_drift_ms≈−1100. 2.5 s covers the remux-open gate.
        const int capMs = 2500;
        const size_t cap =
            misterplex::p720_av::capGatedPcmRemain(gatedAudio.size(), capMs);
        if (cap < gatedAudio.size())
            gatedAudio.resize(cap);
        audioBytes_.store(static_cast<int64_t>(drop));
        log("media: 720p audio_trim presents=" + std::to_string(pres) +
            " drop_bytes=" + std::to_string(drop) +
            " remain_bytes=" + std::to_string(gatedAudio.size()) +
            " cap_ms=" + std::to_string(capMs));
    }
    audioActive_.store(true);
    std::vector<uint8_t> f2acc;
    f2acc.reserve(32768);
    size_t total = 0;
    size_t f2total = 0;
    int f2Fail = 0;
    constexpr size_t kF2Chunk = 8192;
    // Nominal 48 kHz stereo s16le, seeded by AUDIO_CLOCK_PPM. This used to be
    // the whole story: an open-loop trim for the FPGA's not-quite-48 kHz audio
    // clock. It cannot be, because the ring has no backpressure, so any residual
    // error integrates into ring depth forever (measured: +255 B/s at the old
    // +685 ppm, ~80 ms/min, overrunning the ring mid-episode). The servo in
    // feedRateBytesPerSec() now closes the loop on the measured depth and this
    // value is only a starting point — and the fallback if the depth is
    // unreadable.
    const double kBytesPerSec = 48000.0 * 4.0 * (1.0 + audioClockPpm_ / 1000000.0);
    // Deadline for the next chunk. Started on the FIRST chunk actually read, not
    // here: FFmpeg needs a variable, sometimes multi-hundred-ms warm-up before it
    // emits anything, and anchoring the clock before that made the pump write
    // flat out to "catch up", dumping the entire warm-up into the ring where it
    // stayed for the session (feed and drain are both ~48 kHz, so nothing ever
    // drained it). That is what made ring depth — and therefore lipsync —
    // session-dependent.
    std::chrono::steady_clock::time_point audioDue{};
    bool audioClockStarted = false;
    int64_t chunkIndex = 0;
    int64_t queuedEma = -1;
    int64_t lastLatLog = -1;
    bool latencyLogged = false;
    bool overrunLogged = false;

    while (!stop_.load()) {
        if (!liveCoreStillPlex()) {
            log("media: CORE left Plex (" + readLiveCoreName() +
                ") — closing MrAudio to stop post-Menu stream drain");
            stop_.store(true);
            break;
        }
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        ssize_t n = 0;
        if (!gatedAudio.empty()) {
            n = static_cast<ssize_t>(std::min(sizeof(buf), gatedAudio.size()));
            std::memcpy(buf, gatedAudio.data(), static_cast<size_t>(n));
            gatedAudio.erase(gatedAudio.begin(),
                             gatedAudio.begin() + static_cast<std::ptrdiff_t>(n));
        } else {
            if (afd >= 0) {
                struct pollfd pfd {};
                pfd.fd = afd;
                pfd.events = POLLIN;
                const int pr = ::poll(&pfd, 1, 20);
                if (pr < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (pr == 0)
                    continue;
            }
            n = pcmRead(buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                break;
            }
            if (n == 0)
                break;
        }

        if (out >= 0) {
            // Hard backpressure: ring has no kernel block. If depth is already
            // well above the servo target, wait for drain before stuffing more
            // PCM (lab 480i host-paint grew to 600–900 ms; true480 still climbed
            // to ~274 ms with a 200 ms ceiling because the resume floor was also
            // 200 ms — that left lipsync run-to-run scatter on e37).
            {
                const int64_t q = audioQueuedBytes_.load(std::memory_order_relaxed);
                // Cap ~120 ms; resume near servo target (~100 ms).
                constexpr int64_t kMaxQ = (misterplex::kMrAudioBytesPerSec * 12) / 100;
                constexpr int64_t kResumeQ = misterplex::kFeedTargetBytes;
                if (q > kMaxQ) {
                    while (!stop_.load() && !paused_.load()) {
                        const MrAudioStatus st = readMrAudioStatus();
                        if (st.valid()) {
                            audioQueuedBytes_.store(st.queuedBytes);
                            if (st.queuedBytes <= kResumeQ)
                                break;
                        } else {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
            }
            size_t off = 0;
            while (off < static_cast<size_t>(n) && !stop_.load()) {
                ssize_t w = ::write(out, buf + off, static_cast<size_t>(n) - off);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Ring is software-deep; EAGAIN is rare. Yield and
                        // retry so we never drop PCM mid-chunk.
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    log("media: MrAudio write err errno=" + std::to_string(errno));
                    break;
                }
                if (w == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                off += static_cast<size_t>(w);
            }
            // Count only bytes actually accepted by the driver.
            if (off > 0)
                audioBytes_.fetch_add(off);
            if (off == 0)
                continue;
            // Pace/hold against accepted bytes only (partial last attempt).
            n = static_cast<ssize_t>(off);
            if (legacyAudioSession && !fpgaAudioStarted_.load() && n > 0) {
                if (!controlFpgaAudio(legacyAudioEpoch, AudioSessionControl::Resume)) {
                    log("media: legacy audio Resume failed: " + fpga_.lastError());
                } else {
                    fpgaAudioStarted_.store(true);
                    log("media: legacy MrAudio audio session Resume ok");
                }
            }

            // Hold heard audio to presented pictures — 480p gold (unique ~24).
            // 720p inproc + 2-slot prefill deadlocks if this is on: avDecide
            // Hold waits for audio while this loop waits for presentCount
            // (Star Trek stuck at time=5000). Combined 720p matches 480p.
            const bool holdAudioToPictures = holdAudioToPictures_.load();
            if (holdAudioToPictures && fpsNum_ > 0 && fpsDen_ > 0) {
                const double audioSec =
                    static_cast<double>(audioBytes_.load()) / (48000.0 * 4.0);
                for (;;) {
                    const int64_t pres =
                        presentCount_.load(std::memory_order_relaxed);
                    if (pres <= 0 || stop_.load())
                        break;
                    const double videoSec = static_cast<double>(pres) *
                                            static_cast<double>(fpsDen_) /
                                            static_cast<double>(fpsNum_);
                    const double slackSec =
                        static_cast<double>(audioHoldSlackMs_.load()) / 1000.0;
                    if (audioSec <= videoSec + slackSec)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
            if (outW_ == 640 && outH_ == 480 && fpsNum_ > 0 && fpsDen_ > 0) {
                for (;;) {
                    if (stop_.load())
                        break;
                    const int64_t pres = presentCount_.load(std::memory_order_relaxed);
                    if (pres <= 0)
                        break;
                    const int64_t videoPtsUs = pres * 1000000LL * fpsDen_ / fpsNum_;
                    const int64_t audioPtsUs =
                        audioBytes_.load() * 1000000LL / (48000LL * 4);
                    const int delayMs =
                        audioLeadClamp480p(audioPtsUs, videoPtsUs, kAudioLeadClamp480pMs);
                    if (delayMs <= 0)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        delayMs < 4 ? delayMs : 4));
                }
            }

            // Clamp audio-lead vs video on 480p so A/V stay in sync and audio
            // cannot run ahead into cut-off. 720p L4 is unchanged.
            if (audioLeadClamp480pMs_ > 0 && outW_ <= 640 && outH_ <= 480 &&
                fpsNum_ > 0 && fpsDen_ > 0) {
                const int64_t pres = presentCount_.load(std::memory_order_relaxed);
                if (pres > 0) {
                    const double videoSec =
                        static_cast<double>(pres) *
                        static_cast<double>(fpsDen_) /
                        static_cast<double>(fpsNum_);
                    const double audioSec =
                        static_cast<double>(audioBytes_.load()) / (48000.0 * 4.0);
                    const double clampSec =
                        static_cast<double>(audioLeadClamp480pMs_) / 1000.0;
                    if (audioSec > videoSec + clampSec) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
            }

            // Anchor on the first chunk, biased one target-depth into the past so
            // the pump runs flat out just long enough to prefill the ring to the
            // servo's set point, then falls into paced mode. This is the ordinary
            // audio prefill, and it is bounded — unlike the old warm-up burst.
            if (!audioClockStarted) {
                audioClockStarted = true;
                audioDue = std::chrono::steady_clock::now() -
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               std::chrono::duration<double>(
                                   static_cast<double>(misterplex::kFeedTargetBytes) /
                                   kBytesPerSec));
            }

            // Advance the deadline by this chunk's duration at the servo-corrected
            // rate. Accumulating the deadline (rather than recomputing it from a
            // fixed origin) is what lets the rate change mid-stream without the
            // schedule jumping.
            const double rate = misterplex::feedRateBytesPerSec(
                kBytesPerSec, audioQueuedBytes_.load(std::memory_order_relaxed));
            audioDue += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(n / rate));
            const auto now = std::chrono::steady_clock::now();
            if (audioDue > now)
                std::this_thread::sleep_until(audioDue);
            else if (now - audioDue > std::chrono::seconds(1)) {
                // We fell more than a second behind (decoder stall, CPU spike).
                // Do not try to make it up in one burst — that is precisely the
                // ring-stuffing behaviour we just removed. Re-anchor and let the
                // servo refill the target depth at its own pace.
                audioDue = now;
            }

            // Turn the submitted-byte counter into a real playback position by
            // subtracting what is still sitting in the driver's DMA ring. This
            // reading is also the servo's error signal, so it feeds both the
            // video clock and the feed rate.
            // Sampled every 4th chunk (~80 ms), which is far faster than the
            // servo's 8 s time constant; polling harder buys nothing but
            // syscalls.
            if ((chunkIndex++ % 4) == 0) {
                const MrAudioStatus status = readMrAudioStatus();
                const int64_t q = status.queuedBytes;
                if (!status.valid()) {
                    audioQueuedBytes_.store(-1);
                } else {
                    // Low-pass the depth. The servo holds the true depth
                    // constant, so sample-to-sample movement is mostly noise;
                    // feeding it raw into the video clock would jitter every
                    // frame's release time, and into the servo would make it
                    // chase that jitter. Seed on the first sample so startup is
                    // not slewed in from zero.
                    queuedEma = (queuedEma < 0) ? q : (queuedEma * 3 + q) / 4;
                    audioQueuedBytes_.store(queuedEma);
                    const int64_t latMs =
                        (queuedEma * 1000LL) / misterplex::kMrAudioBytesPerSec;
                    if (!latencyLogged) {
                        latencyLogged = true;
                        log("media: MrAudio playback position available — video now paces "
                            "off what is HEARD, not what is sent");
                    }
                    const int64_t nowMs = audioClockMs(audioBytes_.load());
                    if (lastLatLog < 0 || nowMs - lastLatLog >= 5000) {
                        lastLatLog = nowMs;
                        log("media: audio latency " + std::to_string(latMs) + "ms queued=" +
                            std::to_string(queuedEma) + "B rptr=" +
                            std::to_string(status.readPointer) + " wptr=" +
                            std::to_string(status.writePointer));
                    }
                    // The ring has no backpressure: writing past the read pointer
                    // silently destroys unplayed audio. Nothing else reports this.
                    if (!overrunLogged && queuedEma > (misterplex::kMrAudioRingBytes * 3) / 4) {
                        overrunLogged = true;
                        log("media: WARNING MrAudio ring " + std::to_string(latMs) +
                            "ms deep — approaching overwrite of unplayed audio");
                    }
                }
            }
        }

        if (wantF2) {
            f2acc.insert(f2acc.end(), buf, buf + n);
            while (f2acc.size() >= kF2Chunk && !stop_.load()) {
                if (fpga_.sendPcmChunk(f2acc.data(), kF2Chunk, /*F2*/ 2)) {
                    f2total += kF2Chunk;
                    f2Fail = 0;
                } else {
                    ++f2Fail;
                    // Rate-limit: was logging every chunk when f2total==0 (0 % N == 0).
                    if (f2Fail == 1 || f2Fail == 8 || (f2Fail % 64) == 0)
                        log("media: F2 pcm: " + fpga_.lastError() +
                            " (fail#" + std::to_string(f2Fail) + ")");
                    // Core reconfig / menu: stop hammering SPI; MrAudio still plays.
                    if (fpga_.lastError().find("user mode") != std::string::npos && f2Fail >= 4) {
                        log("media: F2 disabled for session (FPGA left user mode)");
                        wantF2 = false;
                        f2acc.clear();
                        break;
                    }
                    if (f2Fail >= 32) {
                        log("media: F2 disabled for session (too many SPI errors)");
                        wantF2 = false;
                        f2acc.clear();
                        break;
                    }
                }
                if (wantF2)
                    f2acc.erase(f2acc.begin(),
                                f2acc.begin() + static_cast<std::ptrdiff_t>(kF2Chunk));
            }
        }
        total += static_cast<size_t>(n);
    }

    // Flush remainder to F2
    if (wantF2 && f2acc.size() >= 4) {
        size_t n = f2acc.size() & ~size_t(3);
        if (n && fpga_.sendPcmChunk(f2acc.data(), n, 2))
            f2total += n;
    }

    releaseLegacyAudio();
    if (out >= 0)
        ::close(out);
    pcmClose();
    audioActive_.store(false);
    log("media: audio pump end bytes=" + std::to_string(total) +
        " f2=" + std::to_string(f2total));
}

void MediaPlayer::threadMain(std::string url, int64_t startMs, std::string headers,
                             int64_t durationMs) {
    playing_.store(true);
    positionMs_.store(startMs);
    lastPosMs_.store(startMs);
    // Core-leave MrAudio abort lives in audioPump (liveCoreStillPlex). A
    // separate watch thread is not started here — keep play path close to the
    // known-good 6e27 lipsync binary.
    if (onProgress_)
        onProgress_("buffering", startMs, durationMs);

    const bool nativeScalerPresent =
        presentMode_ == "fpga" || presentMode_ == "both";
    DdrFrameGeometry g =
        nativeScalerPresent ? ddrFrameGeometryForPresentedSize(outW_, outH_)
                            : makeDdrFrameGeometry(outW_, outH_);
    const bool sharedLbBank =
        (outW_ == kPlex480iCodedWidth && outH_ == kPlex480iCodedHeight) ||
        (outW_ == kPlex480pPresentedWidth && outH_ == kPlex480pPresentedHeight) ||
        (outW_ == kPlex240pPresentedWidth && outH_ == kPlex240pPresentedHeight) ||
        (outW_ == 320 && outH_ == 240) ||
        (outW_ == 640 && outH_ == 480);
    if (nativeScalerPresent && sharedLbBank && sourceMediaH_ > 0 &&
        sourceMediaH_ < g.coded_height) {
        int top = 0, bot = 0;
        if (plexSharedLetterboxCrop(g.coded_height, sourceMediaH_, top, bot)) {
            g.crop_top = top;
            g.crop_bottom = bot;
            g.display_height = g.coded_height - top - bot;
            log("media: letterboxCrop top=" + std::to_string(top) + " bottom=" + std::to_string(bot) +
                " bank=" + std::to_string(g.coded_width) + "x" + std::to_string(g.coded_height) +
                " display=" + std::to_string(g.display_width) + "x" + std::to_string(g.display_height));
        }
    }
    // 480i letterbox/crop from the resolved source DAR. Apply on g before it
    // is frozen: ddrGeometry stays const so the 480p geometry check holds.
    // Leave a crop the shared letterbox path already wrote.
    if (g.presented_width == 720 && g.presented_height == 480 && sourceAspect_.valid &&
        g.crop_top == 0 && g.crop_bottom == 0) {
        int cl = 0, cr = 0, ct = 0, cb = 0;
        const bool isLetterbox =
            (sourceAspect_.x * static_cast<long long>(3) >
             static_cast<long long>(4) * sourceAspect_.y);
        if (letterboxCrop480i(cl, cr, ct, cb, g.coded_width, g.coded_height,
                              g.display_width, g.display_height, isLetterbox, false)) {
            g.crop_left = cl;
            g.crop_right = cr;
            g.crop_top = ct;
            g.crop_bottom = cb;
        }
    }
    const DdrFrameGeometry ddrGeometry = g;
    // 480p active_height is the stored display height. Letterbox/pillar already
    // match stored crop. Do not mutate ddrGeometry: isPlex480pDdrFrameGeometry
    // must stay true.
    if (nativeScalerPresent && misterplex::isPlex480pDdrFrameGeometry(ddrGeometry)) {
        const int kActiveHeight480p = ddrGeometry.display_height;
        log("media: 480p active_height=" + std::to_string(kActiveHeight480p) +
            " coded=" + std::to_string(ddrGeometry.coded_height) +
            " crop_top=" + std::to_string(ddrGeometry.crop_top) +
            " crop_bottom=" + std::to_string(ddrGeometry.crop_bottom) +
            " crop_right=" + std::to_string(ddrGeometry.crop_right));
    }
    const int rawW = ddrGeometry.coded_width;
    const int rawH = ddrGeometry.coded_height;
    const int rawDisplayW = ddrGeometry.display_width;
    const int rawDisplayH = ddrGeometry.display_height;
    if (budget960Wanted() && !isPlex960BankSize(rawW, rawH)) {
        log("ERROR media: P5_960 contract fail bank=" + std::to_string(rawW) + "x" +
            std::to_string(rawH) + " (refusing 1280 ingest / snap-up)");
        return;
    }

    char scale[64];
    std::snprintf(scale, sizeof(scale), "%d:%d", rawW, rawH);
    std::string vf;
    // Force CFR at the exact content rate FIRST in the chain: frameIndex ↔ content
    // time then holds by construction (even if PMS emits a different rate than its
    // metadata claims), and frames dropped by the fps filter are never scaled.
    // Lab: FFMPEG_FPS_FILTER=off skips this for present-rate ceiling tests.
    if (fpsFilter_ && fpsNum_ > 0 && fpsDen_ > 0) {
        vf = "fps=" + std::to_string(fpsNum_) + "/" + std::to_string(fpsDen_) + ",";
    }
    // Host bob (MiSTer_DVD ascal bob analogue for progressive HDMI present):
    // yadif/bwdif before scale so interlaced TV sources do not comb on glass.
    // Film progressive is a no-op cost only when DEINTERLACE is explicitly set.
    if (!deinterlaceMode_.empty() && deinterlaceMode_ != "off" &&
        deinterlaceMode_ != "0" && deinterlaceMode_ != "none") {
        std::string di = deinterlaceMode_;
        if (di == "bob" || di == "yadif")
            di = "yadif=mode=send_field:parity=auto:deint=interlaced";
        else if (di == "bwdif")
            di = "bwdif=mode=send_field:parity=auto:deint=interlaced";
        // else: pass through raw filter name (advanced lab)
        vf += di + ",";
        log("media: DEINTERLACE=" + deinterlaceMode_ + " filter=" + di);
    }
    // Scale filter: default bicubic avoids fast_bilinear vertical banding on
    // 480p-anamorphic→720 skies (BBB). MiSTer's native scaler owns display
    // aspect, so product frames fill the complete visible raster without
    // host-side letterbox bars.
    // Light present-rate ladder:
    //   fast_bilinear|neighbor — full-raster anamorphic scale
    //   exact / exact_fast_bilinear / exact_neighbor — force coded WxH, no foar/pad
    //   skip|none|off|identity — omit scale (size trust; may desync rawvideo)
    std::string swsFlags = swsFlags_;
    bool forceExact = false;
    if (swsFlags == "exact") {
        forceExact = true;
        swsFlags = "neighbor";
    } else if (swsFlags.rfind("exact_", 0) == 0 && swsFlags.size() > 6) {
        forceExact = true;
        swsFlags = swsFlags.substr(6);
    }
    if (budget960Wanted()) {
        forceExact = true;
        if (swsFlags == "bicubic" && swsFlags_ == "bicubic")
            swsFlags = "fast_bilinear";
        log("media: MPX_BUDGET_960 force exact scale to DECODE bank flags=" +
            swsFlags);
    }
    const bool skipScaleFlag =
        (swsFlags_ == "skip" || swsFlags_ == "none" || swsFlags_ == "off" ||
         swsFlags_ == "identity");
    // NEVER auto-skip scale because weak ladder videoResolution == DECODE alone.
    // PMS often returns a smaller coded size (lab FOAR @720p request → 720x480
    // H.264). Bypassing then packs wrong stride into 1280x720 banks → rainbow (L38).
    // Safe skip sources:
    //   - lab FFMPEG_SWS_FLAGS=skip|none|off|identity
    //   - local play-file already at coded WxH
    //   - PMS Media/Stream size *exactly matches* DECODE (identity). Source larger
    //     than bank (FOAR 720×480 into 640×480/320×240) must still scale down — and
    //     preferably come from PMS weak ladder at bank size (G0b).
    const bool urlIsLocalFile =
        !url.empty() && url[0] == '/' && url.rfind("http", 0) != 0 && url != "testsrc" &&
        url.rfind("lavfi", 0) != 0;
    // Universal 720p: probe start.mp4 coded WxH. Library 1440x1080 must not
    // skip (crop+scale of an already-1280x720 transcode is the 14 unique lock).
    // Probe fail keeps crop/scale so a 640x480 delivery cannot pack the bank.
    int vfCodedW = sourceMediaW_;
    int vfCodedH = sourceMediaH_;
    if (misterplex::isPlex720pBankSize(outW_, outH_) &&
        misterplex::isUniversalTranscodeUrl(url)) {
        int probedW = 0, probedH = 0;
        std::string probeFail;
        (void)probeSourceAspect(url, headers, &probeFail, &probedW, &probedH);
        vfCodedW = misterplex::skipVfProbedCodedDim(probedW, sourceMediaW_);
        vfCodedH = misterplex::skipVfProbedCodedDim(probedH, sourceMediaH_);
        char pbuf[160];
        std::snprintf(pbuf, sizeof(pbuf),
                      "media: transcode coded probe=%dx%d skip_coded=%dx%d library=%dx%d %s",
                      probedW, probedH, vfCodedW, vfCodedH, sourceMediaW_, sourceMediaH_,
                      probeFail.empty() ? "ok" : probeFail.c_str());
        log(pbuf);
    }
    const bool skip720pTranscodeVf =
        misterplex::skipRedundant720pTranscodeVf(url, outW_, outH_, vfCodedW, vfCodedH);
    const bool padOnly720p =
        !skip720pTranscodeVf &&
        misterplex::transcodePadOnly720p(vfCodedW, vfCodedH, outW_, outH_);
    const bool cropPmsBars =
        !skip720pTranscodeVf && !padOnly720p && nativeScalerPresent && sourceAspect_.valid &&
        url.find("/transcode/universal/") != std::string::npos;
    if (cropPmsBars) {
        const std::string dar = std::to_string(sourceAspect_.x) + "/" +
                                std::to_string(sourceAspect_.y);
        vf += "crop=trunc(min(iw\\,ih*" + dar +
              ")/2)*2:trunc(min(ih\\,iw/(" + dar + "))/2)*2,";
        log("media: PMS canvas crop to source DAR=" +
            std::to_string(sourceAspect_.x) + ":" +
            std::to_string(sourceAspect_.y));
    }
    if (padOnly720p) {
        vf += "pad=1280:720:(1280-iw)/2:(720-ih)/2:black";
        log("media: 720p pad-only coded=" + std::to_string(vfCodedW) + "x" +
            std::to_string(vfCodedH) + " → 1280x720 (no scale)");
    }
    // Bank==coded is not file==coded. --decode 960x540 on a 1280x720 local clip
    // used to skip vf (31b6d70f: scale/pad skipped local_identity_file) and
    // desync the 777600-byte raw pipe. Require probed source == bank, like PMS.
    const bool identityLocalFile =
        nativeScalerPresent && urlIsLocalFile && !forceExact &&
        rawDisplayW == rawW && rawDisplayH == rawH && outW_ == rawW && outH_ == rawH &&
        sourceMediaW_ > 0 && sourceMediaH_ > 0 &&
        sourceMediaW_ == outW_ && sourceMediaH_ == outH_;
    const bool pmsSourceMatchesBank =
        nativeScalerPresent && !forceExact && !urlIsLocalFile &&
        sourceMediaW_ > 0 && sourceMediaH_ > 0 &&
        sourceMediaW_ == outW_ && sourceMediaH_ == outH_ &&
        rawDisplayW == rawW && rawDisplayH == rawH && outW_ == rawW && outH_ == rawH;
    const bool skipScale =
        skip720pTranscodeVf || padOnly720p ||
        (!cropPmsBars && (skipScaleFlag || identityLocalFile || pmsSourceMatchesBank));
    if (skip720pTranscodeVf)
        log("media: 720p transcode already bank-sized — skip crop/scale vf");
    // skip|none|off|identity are NOT valid libswscale flag names — if 480p (or any
    // non-identity geom) still needs scaling, fall back to bicubic.
    if (skipScaleFlag && !(skipScale && rawDisplayW == rawW && rawDisplayH == rawH)) {
        swsFlags = "bicubic";
        log("media: FFMPEG_SWS_FLAGS=" + swsFlags_ +
            " ignored for non-identity geom; using bicubic native-aspect scale");
    }
    if (skipScale && rawDisplayW == rawW && rawDisplayH == rawH) {
        // Explicit lab skip, local identity, or PMS source exact DECODE match.
        if (!vf.empty() && vf.back() == ',')
            vf.pop_back();
        const char* why = "FFMPEG_SWS_FLAGS";
        if (skip720pTranscodeVf)
            why = "transcode_coded_1280x720";
        else if (padOnly720p)
            why = "pad_only_720tall";
        else if (identityLocalFile && !skipScaleFlag)
            why = "local_identity_file";
        else if (pmsSourceMatchesBank && !skipScaleFlag)
            why = "pms_source_matches_bank";
        log(std::string("media: scale/pad skipped (") + why +
            (pmsSourceMatchesBank
                 ? (" src=" + std::to_string(sourceMediaW_) + "x" +
                    std::to_string(sourceMediaH_))
                 : "") +
            " coded=" + std::to_string(rawW) + "x" + std::to_string(rawH) +
            " scale=bypass)");
    } else if (forceExact) {
        // Hard size contract: stretch/squash to coded WxH (one scale, no pad).
        vf += std::string("scale=") + scale + ":flags=" + swsFlags;
        log("media: force exact scale=" + std::string(scale) + " flags=" + swsFlags +
            " (no foar/pad)");
    } else if (rawDisplayW != rawW || rawDisplayH != rawH) {
        char displayScale[64];
        std::snprintf(displayScale, sizeof(displayScale), "%d:%d", rawDisplayW, rawDisplayH);
        vf += std::string("scale=") + displayScale + ":flags=" + swsFlags + ",pad=" + scale +
              ":" + std::to_string(ddrGeometry.crop_left) + ":" +
              std::to_string(ddrGeometry.crop_top) + ":color=black";
        log("media: native-aspect scale+crop-pad coded=" + std::to_string(rawW) + "x" +
            std::to_string(rawH) +
            " display=" + std::to_string(rawDisplayW) + "x" + std::to_string(rawDisplayH) +
            " present=" + std::to_string(outW_) + "x" + std::to_string(outH_) +
            " flags=" + swsFlags);
    } else if (nativeScalerPresent) {
        vf += std::string("scale=") + scale + ":flags=" + swsFlags;
    } else {
        vf += std::string("scale=") + scale +
              ":force_original_aspect_ratio=decrease:flags=" + swsFlags +
              ",pad=" + scale + ":(ow-iw)/2:(oh-ih)/2:color=black";
        log("media: host aspect preservation enabled (PRESENT=" + presentMode_ + ")");
    }

    const bool testPattern = (url == "testsrc" || url.rfind("lavfi", 0) == 0);
    // STREAM=0 + local file: optional FFmpeg subtitles filter (burn-in). Network/PMS
    // prefer WeakLadder::burnSubtitles so dual-A9 avoids libass on HTTP streams.
    const bool localFile =
        !testPattern && !url.empty() && url[0] == '/' && url.rfind("http", 0) != 0;
    if (!streamEnabled_ && subtitleMode_ == "ffmpeg" && localFile) {
        // Escape special chars for filtergraph path arg.
        std::string esc;
        for (char c : url) {
            if (c == '\\' || c == ':' || c == '\'' || c == '[' || c == ']')
                esc.push_back('\\');
            esc.push_back(c);
        }
        vf += ",subtitles=" + esc + ":si=" + std::to_string(std::max(0, subtitleStreamIndex_));
        log("media: FFmpeg subtitles burn-in si=" + std::to_string(subtitleStreamIndex_));
    }
    const bool wantMr = audioEnabled_ && (::access(audioDev_.c_str(), W_OK) == 0);
    // Match audioPump: F2 only when PRESENT=fpga and MrAudio unavailable.
    const bool wantF2 = fpga_.ok() && presentMode_ == "fpga" && !wantMr;
    bool wantAudio = audioEnabled_ && (wantMr || wantF2);
    // Dual-output (pipe:1 + pipe:3) aborts the whole ffmpeg process when the
    // container has no audio — Grid720 freckle map is video-only. Prefer resolve
    // metadata (hasAudio=false); fall back to a local-file probe.
    if (wantAudio && !sourceHasAudio_) {
        wantAudio = false;
        log("media: audio disabled for session: source metadata has no audio stream "
            "(avoid empty pipe:3 abort)");
    } else if (wantAudio && localFile &&
               !ffmpegHasAudioStream(ffmpeg_, url, headers, startMs)) {
        wantAudio = false;
        log("media: audio disabled for session: no audio stream detected; avoiding empty "
            "audio output abort");
    }

    // Product path: STREAM + PRESENT=fpga may skip heavy RGB (keep audio + demux).
    // STREAM=0 and PRESENT=both/fb0 always keep the proven FFmpeg RGB path.
    const bool skipRgb = !testPattern && wantSkipRgbVideo();
    if (streamEnabled_ && !testPattern) {
        if (skipRgb) {
            log("media: STREAM skip RGB decode (audio + host recon F1; PRESENT=fpga "
                "STREAM_SKIP_RGB=" +
                streamSkipRgb_ + ")");
        } else {
            // Make preferDirect / skip-RGB product path inspectable in logs.
            log("media: STREAM keep FFmpeg RGB (PRESENT=" + presentMode_ +
                " STREAM_SKIP_RGB=" + streamSkipRgb_ +
                (presentMode_ == "fpga" ? " — RGB retained only for decode/audio fallback)"
                                        : " — continuous fb0 needs RGB)"));
        }
    }

    // Optional continuous annex-B → host recon F1 + F3
    const bool wantStream = streamEnabled_ && fpga_.ok() && !testPattern;
    if (wantStream) {
        int spipe[2] = {-1, -1};
        if (pipe(spipe) == 0) {
            pid_t spid = spawnStreamDemux(url, headers, startMs, spipe[1]);
            ::close(spipe[1]);
            if (spid > 0) {
                streamPid_.store(spid);
                // Only the no-RGB path lets sparse reconstruction own F1.
                // Otherwise continuous rawvideo is the sole DDR frame writer.
                streamThr_ =
                    std::thread([this, sfd = spipe[0], allowF1Present = skipRgb] {
                        streamPump(sfd, allowF1Present);
                    });
                if (looksElementaryH264(url))
                    log("media: STREAM demux elementary H.264 (no mp4toannexb)");
                else
                    log("media: STREAM demux via h264_mp4toannexb (Part/container → annex-B)");
            } else {
                ::close(spipe[0]);
                log("media: STREAM demux fork failed");
            }
        } else {
            log("media: STREAM demux pipe failed errno=" + std::to_string(errno));
        }
    } else if (streamEnabled_ && !testPattern && !fpga_.ok()) {
        log("media: STREAM=1 but FPGA SPI unavailable — host recon F1/F3 disabled");
    }

    int rfd = -1;
    int64_t frameIndex = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto lastLog = t0;
    size_t totalBytes = 0;

    bool usedRawVideo = false;
    bool videoEof = false;
    bool shortRead = false;
    bool true480PipelineAborted = false;
    size_t shortReadGot = 0;
    size_t shortReadWant = 0;

    // A/V pacing state. The exact rational rate is load-bearing: pacing 23.976 fps
    // content at a hardcoded 24 leaks ~1 ms/s of video lead.
    const int fpsNum = fpsNum_ > 0 ? fpsNum_ : kDefaultFpsNum;
    const int fpsDen = fpsNum_ > 0 && fpsDen_ > 0 ? fpsDen_ : kDefaultFpsDen;
    // NTSC 480i frame family (two fields). 60000/1001 is already field rate.
    // Do not treat 60/1 as 480i (480p60 / 720p60). Do not use rawH==480
    // (product FPGA coded height is 720; 480-line 60/1 is 480p60).
    const bool is480iFrameRate =
        (fpsNum == 30000 && fpsDen == 1001) || (fpsNum == 30 && fpsDen == 1);
    const bool is480iFieldRate =
        (fpsNum == 60000 && fpsDen == 1001);
    const bool is480iPtsBind = is480iFrameRate || is480iFieldRate;
    constexpr int64_t drift_threshold = 80;
    bool ptsDriftGuardFired = false;
    const int64_t leadMs = presentLeadMs_;
    // L4 / 720p bank: never 2:1-drop for audio drift. Hold (video ahead) stays.
    // FPGA NACK is sendYuv420pFrameDdr / commitDdrBankIngest fail, not Drop.
    // 720p L4 and true480 640×480: never 2:1-drop for wall-clock drift.
    // Silent Grid720 on the 480 store was presenting 13 pfps / dropping half
    // (dropMs>0) after glass pinned true480. FPGA already swaps every kick.
    const bool l4PresentEveryDecoded =
        liveGlass_ == LiveGlass::L4 || isPlex720pDdrFrameGeometry(ddrGeometry) ||
        isPlex480pDdrFrameGeometry(ddrGeometry) ||
        (ddrGeometry.presented_width == 720 && ddrGeometry.presented_height == 480) ||
        (ddrGeometry.presented_width == 320 && ddrGeometry.presented_height == 240);
    const int64_t dropMs =
        avResyncDropMsForPresent(resyncDropMs_, l4PresentEveryDecoded);
    int dropRun = 0;
    avDriftMs_.store(0);
    droppedFrames_.store(0);
    pts_drift_ms_ = 0;
    videoPts_ = 0;
    audioPts_ = 0;
    if (fpsNum_ <= 0)
        log("media: content fps UNKNOWN — pacing at " + std::to_string(kDefaultFpsNum) + "/" +
            std::to_string(kDefaultFpsDen) + " and relying on drift correction");
    else
        log("media: content fps=" + std::to_string(fpsNum) + "/" + std::to_string(fpsDen) +
            " lead_ms=" + std::to_string(leadMs) + " resync_drop_ms=" + std::to_string(dropMs) +
            (l4PresentEveryDecoded ? " (L4 present-every-decoded; FPGA NACK only)" : ""));

    if (skipRgb) {
        // Audio-only FFmpeg + wall-clock position. Host recon owns F1.
        int apipe[2] = {-1, -1};
        if (wantAudio && pipe(apipe) == 0) {
            pid_t pid = spawnAudioOnly(url, headers, startMs, apipe[1]);
            ::close(apipe[1]);
            if (pid > 0) {
                childPid_.store(pid);
                audioThr_ = std::thread([this, afd = apipe[0]] { audioPump(afd); });
            } else {
                ::close(apipe[0]);
                log("media: audio-only fork failed");
            }
        } else if (wantAudio) {
            log("media: audio pipe failed — STREAM recon video only");
        }

        if (onProgress_)
            onProgress_("playing", startMs, durationMs);
        auto lastProgress = t0;

        // Wait for session end: stop/seek, or both pumps exit.
        while (!stop_.load()) {
            int64_t seekTo = seekReqMs_.exchange(-1);
            if (seekTo >= 0) {
                log("media: seek requested " + std::to_string(seekTo));
                break;
            }
            if (paused_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            // Wall-clock position (no RGB frame cadence)
            auto now = std::chrono::steady_clock::now();
            int64_t elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
            int64_t tms = startMs + elapsed;
            positionMs_.store(tms);
            lastPosMs_.store(tms);
            if (now - lastProgress >= std::chrono::seconds(1)) {
                lastProgress = now;
                companionTimelineTick();
            }
            if (durationMs > 0 && tms >= durationMs) {
                log("media: STREAM audio-only reached duration");
                break;
            }
            // Exit when demux and audio both finished (EOF)
            if (!streamActive_.load() && streamThr_.joinable() && !audioActive_.load() &&
                childPid_.load() > 0) {
                // Give stream thr a moment; if stream ended and no audio, done
                pid_t cp = childPid_.load();
                if (cp > 0) {
                    int st = 0;
                    if (waitpid(cp, &st, WNOHANG) == cp)
                        childPid_.store(-1);
                }
                if (childPid_.load() <= 0 && !streamActive_.load())
                    break;
            }
            if (now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                log("media: STREAM no-RGB t_ms=" + std::to_string(tms) +
                    " recon=" + std::to_string(reconFrames_.load()) +
                    " cabac=" + (cabacSkip_.load() ? "1" : "0") +
                    " audio=" + (audioActive_.load() ? "on" : "off") +
                    " stream=" + (streamActive_.load() ? "on" : "off"));
            }
            // CABAC with no recon: optional soft note (RGB was skipped — nothing to fall back)
            if (cabacSkip_.load() && reconFrames_.load() == 0 && elapsed > 3000 &&
                (elapsed / 1000) % 10 == 3) {
                static thread_local int64_t lastCabacWarn = -1;
                if (elapsed - lastCabacWarn > 9000) {
                    lastCabacWarn = elapsed;
                    log("media: STREAM no-RGB + CABAC — stream is High/CABAC; MiSTerPlex.xml "
                        "profile may be missing or inactive on PMS. Set STREAM_SKIP_RGB=0 or "
                        "PRESENT=both for fb0 fallback.");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } else {
        // Full FFmpeg rawvideo path (STREAM=0 default; STREAM=1 with PRESENT=both/fb0; skip off).
        // STREAM=0 + PRESENT=fpga: every decoded frame → F1 (DDR preferred) — not IDR recon.
        // STREAM=1 recon is ~1 fps (keyframe only) and is the wrong interactive cast path.
        const bool wantFpgaFrameStore =
            fpga_.ok() && (presentMode_ == "fpga" || presentMode_ == "both");
        const bool wantYuvDdr = wantFpgaFrameStore && ddrFrameFormat_ == DdrFrameFormat::Yuv420p;
        RawVideoFormat videoFmt = RawVideoFormat::Rgb24;
        if (wantYuvDdr) {
            videoFmt = RawVideoFormat::Yuv420p;
        } else if (wantFpgaFrameStore || (fb_.ok() && fb_.bpp() == 16)) {
            videoFmt = RawVideoFormat::Rgb565Le;
        } else if (fb_.ok() && (fb_.bpp() == 32 || fb_.bpp() == 24)) {
            videoFmt = RawVideoFormat::Bgra32;
        }
        if (!streamEnabled_ && (presentMode_ == "fpga" || presentMode_ == "both"))
            log("media: STREAM=0 rawvideo(" + std::string(ffmpegPixFmt(videoFmt)) +
                ")→F1 PRESENT=" + presentMode_ +
                " decode=" + std::to_string(outW_) + "x" + std::to_string(outH_) +
                " coded=" + std::to_string(rawW) + "x" + std::to_string(rawH) +
                " display=" + std::to_string(rawDisplayW) + "x" +
                std::to_string(rawDisplayH) +
                " clock=wall-48k-audio+every-frame-present");
        if (wantYuvDdr && presentMode_ == "both" && fb_.ok())
            log("media: PRESENT=both uses yuv420p DDR frame-store path; fb0 blit converts the "
                "same frame");
        usedRawVideo = true;
        presentCount_ = 0;
        audioBytes_.store(0);
        audioFeedRelease_.store(true);
        audioReleaseAfterPresents_.store(0);
        audioAfterVideoMs_.store(0);
        BankReleaseStatus hwPresentBaseline;
        bool hwPresentBaselineValid = fpga_.readBankRelease(hwPresentBaseline);
        int64_t hwPresentArmBaseline = 0;
        uint16_t hwPresentLastFrames = hwPresentBaseline.frames_done;
        if (hwPresentBaselineValid) {
            char pbuf[128];
            std::snprintf(pbuf, sizeof(pbuf),
                          "media: plxd_play_start frames_done=%u pending=%d disp=%u free=0x%x",
                          static_cast<unsigned>(hwPresentBaseline.frames_done),
                          hwPresentBaseline.swap_pending ? 1 : 0,
                          static_cast<unsigned>(hwPresentBaseline.disp_bank),
                          static_cast<unsigned>(hwPresentBaseline.free_bank_mask));
            log(pbuf);
        } else {
            log("media: plxd_play_start unavailable: " + fpga_.lastError());
        }
        uint64_t hwPresentTotal = 0;
        auto hwPresentTimeBaseline = std::chrono::steady_clock::now();
        auto hardwarePresentTelemetry = [&]() {
            BankReleaseStatus current;
            if (!fpga_.readBankRelease(current))
                return std::string(" hw_presents=unavailable hw_presents_src=plxd_swap_count");
            const auto now = std::chrono::steady_clock::now();
            if (!hwPresentBaselineValid) {
                hwPresentBaseline = current;
                hwPresentArmBaseline = presentCount_;
                hwPresentLastFrames = current.frames_done;
                hwPresentTotal = 0;
                hwPresentTimeBaseline = now;
                hwPresentBaselineValid = true;
            } else {
                hwPresentTotal += frameCounterDelta(current.frames_done, hwPresentLastFrames);
                hwPresentLastFrames = current.frames_done;
            }
            const int64_t armDelta = presentCount_ - hwPresentArmBaseline;
            const int64_t windowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - hwPresentTimeBaseline)
                    .count();
            const double hardwareFps =
                windowMs > 0 ? 1000.0 * static_cast<double>(hwPresentTotal) /
                                   static_cast<double>(windowMs)
                             : 0.0;
            return std::string(" hw_presents=") + std::to_string(hwPresentTotal) +
                   " hw_arm_delta=" + std::to_string(armDelta) +
                   " hw_fps=" + std::to_string(hardwareFps).substr(0, 5) +
                   " hw_window_ms=" + std::to_string(windowMs) +
                   " hw_match=" +
                   (hardwarePresentTotalsMatch(hwPresentTotal, armDelta)
                        ? "1"
                        : "0") +
                   " hw_presents_src=plxd_swap_count";
        };
        // Live universal never prefetches to tmpfs (waitPid 20s killed the
        // 91 min Trek transcode; identity /tmp body is not product).
        std::vector<std::string> args;
        args.push_back(ffmpeg_);
        args.push_back("-hide_banner");
        args.push_back("-loglevel");
        args.push_back("error");
        args.push_back("-nostdin");
        // Dual-A9: isolated 720p Farpoint is ~32 fps at -threads 2. Product
        // wait_swap sleeps ~41 ms per unique, so both cores can decode during
        // the blank. -threads 1 + wait_swap was unique 22.9; memcpy-era
        // thread=1 without wait was 0.3 pfps (15 ms copy).
        args.push_back("-threads");
        args.push_back("2");
        // Identity 1280×720 baseline: deblock is ~2–4 ms/f on dual-A9 and was
        // the 21.8 unique lock (host x264 is 9.5× realtime). skip-vf path only.
        if (skip720pTranscodeVf) {
            args.push_back("-skip_loop_filter");
            args.push_back("all");
            log("media: 720p identity skip_loop_filter=all");
        }
        // Match scale=...:flags=; omit for skip/none/off/identity (no swscale).
        // exact_* → pass the algorithm only (exact_fast_bilinear → fast_bilinear).
        if (!(swsFlags_ == "skip" || swsFlags_ == "none" || swsFlags_ == "off" ||
              swsFlags_ == "identity")) {
            std::string alg = swsFlags_;
            if (alg == "exact")
                alg = "neighbor";
            else if (alg.rfind("exact_", 0) == 0 && alg.size() > 6)
                alg = alg.substr(6);
            args.push_back("-sws_flags");
            args.push_back(alg);
        }
        // D3 decode cost: skip in-loop deblock on dual-A9 (lab). Softens slightly;
        // measure pfps only — not a quality/product PASS claim.
        // Do not skip loop filter: all-skip left greenish H.264 mosquito / chroma
        // bleed on soft edges (user BBB HDMI). Default deblock is worth the dual-A9
        // cost once present is pipelined.

        // 720p product + audio: video ffmpeg is -an; audio is a second process
        // so MrAudio 48 kHz wall-pace cannot stall I420 produce. 480p and
        // testPattern stay single-process (pipe:1 + pipe:3).
        const bool splitAv720 =
            wantAudio && !testPattern && plex720pClassSplitAv(outW_, outH_);
        if (testPattern) {
            std::string lavfi;
            if (url.rfind("lavfi:", 0) == 0 && url.size() > 6) {
                lavfi = url.substr(6);
            } else {
                const std::string rate =
                    fpsNum_ > 0 ? (std::to_string(fpsNum_) +
                                   (fpsDen_ > 1 ? ("/" + std::to_string(fpsDen_)) : ""))
                                : "30";
                lavfi = "testsrc2=size=" + std::to_string(outW_) + "x" +
                        std::to_string(outH_) + ":rate=" + rate;
            }
            args.push_back("-f");
            args.push_back("lavfi");
            args.push_back("-i");
            args.push_back(lavfi);
            if (wantAudio) {
                args.push_back("-f");
                args.push_back("lavfi");
                args.push_back("-i");
                args.push_back("sine=f=440:r=48000:d=120");
            }
            args.push_back("-t");
            args.push_back("120");
            args.push_back("-map");
            args.push_back("0:v:0");
            // Same rule as universal path: -an only when no audio output follows.
            if (!wantAudio)
                args.push_back("-an");
            args.push_back("-f");
            args.push_back("rawvideo");
            args.push_back("-pix_fmt");
            args.push_back(ffmpegPixFmt(videoFmt));
            // Empty vf (scale+fps both skipped) must not pass bare "-vf" — ffmpeg exits.
            if (!vf.empty()) {
                args.push_back("-vf");
                args.push_back(vf);
            }
            args.push_back("pipe:1");
            if (wantAudio) {
                args.push_back("-map");
                args.push_back("1:a:0");
                args.push_back("-vn");
                args.push_back("-f");
                args.push_back("s16le");
                args.push_back("-ac");
                args.push_back("2");
                args.push_back("-ar");
                args.push_back("48000");
                args.push_back("pipe:3");
            }
        } else {
            // Local/direct Part: FFmpeg -ss. Universal: offset already in URL (no double-seek).
            if (transcode_offset::demuxSeekMs(url, startMs) > 0) {
                char ss[32];
                std::snprintf(ss, sizeof(ss), "%.3f", startMs / 1000.0);
                args.push_back("-ss");
                args.push_back(ss);
            } else if (startMs > 0 && urlHasUniversalOffset(url)) {
                log("media: skip -ss (universal offset baked) startMs=" +
                    std::to_string(startMs));
            }
            // Prefer native HTTP with headers (single demux for A+V — dual-A9 critical)
            if (!headers.empty()) {
                // FFmpeg requires trailing CRLF on -headers block
                std::string h = headers;
                if (h.size() < 2 || h[h.size() - 1] != '\n')
                    h += "\r\n";
                args.push_back("-headers");
                args.push_back(h);
                args.push_back("-reconnect");
                args.push_back("1");
                args.push_back("-reconnect_streamed");
                args.push_back("1");
                args.push_back("-reconnect_delay_max");
                args.push_back("5");
            }
            args.push_back("-i");
            args.push_back(url);

            // Video output first. Combined A+V must not pass global -an when a
            // second audio output follows — FFmpeg 6/7 treats -an as
            // disable-all-audio and then fails pipe:3. Video-only and 720p
            // split sessions use -an so this process does not pull unused audio.
            args.push_back("-map");
            args.push_back("0:v:0");
            if (!wantAudio || splitAv720)
                args.push_back("-an");
            args.push_back("-f");
            args.push_back("rawvideo");
            args.push_back("-pix_fmt");
            args.push_back(ffmpegPixFmt(videoFmt));
            // Empty vf (identity skip + FPS filter off) must not pass bare "-vf".
            if (!vf.empty()) {
                args.push_back("-vf");
                args.push_back(vf);
            }
            args.push_back("pipe:1");

            if (wantAudio && !splitAv720) {
                // 480p combined spawn: MrAudio wall-pace back-pressures A+V
                // together. 720p uses spawnAudioOnly() instead (below).
                // AUDIO_DELAY_MS>0: adelay shifts audio content later (ms).
                args.push_back("-map");
                args.push_back("0:a:0?");
                args.push_back("-vn");
                args.push_back("-af");
                if (audioDelayMs_ > 0) {
                    // adelay unit is ms per channel; all=1 applies to every channel.
                    args.push_back("aresample=48000,adelay=" + std::to_string(audioDelayMs_) +
                                   ":all=1");
                    log("media: ffmpeg adelay_ms=" + std::to_string(audioDelayMs_));
                } else {
                    args.push_back("aresample=48000");
                }
                args.push_back("-f");
                args.push_back("s16le");
                args.push_back("-ac");
                args.push_back("2");
                args.push_back("-ar");
                args.push_back("48000");
                args.push_back("pipe:3");
            }
        }

#ifdef MPX_HAVE_LIBAV
        AvInprocDecoder inprocDec;
#endif
        bool useInproc = false;
        int remuxPcmFd = -1;
        int remuxHoldV = -1;
        int remuxHoldA = -1;
        bool remuxPcmPumpEarly = false;
        std::string audioUrl = url;
        std::string audioHeaders = headers;
        int64_t audioStartMs = startMs;
        const bool wantInproc =
            inprocDecodeWanted() && !testPattern &&
            inprocDecodeSizeOk(outW_, outH_);
        log(std::string("media: want_inproc=") + (wantInproc ? "1" : "0") +
            " prefetched=0 url_local=" +
            (url.rfind("/tmp/", 0) == 0 ? "1" : "0"));
        auto abortInprocSession = [&](const std::string& why) {
            log(why);
            playing_.store(false);
            stop_.store(true);
#ifdef MPX_HAVE_LIBAV
            inprocDec.requestStop();
#endif
            killChildren();
            if (audioThr_.joinable())
                audioThr_.join();
            if (streamThr_.joinable())
                streamThr_.join();
#ifdef MPX_HAVE_LIBAV
            inprocPcm_ = nullptr;
#endif
        };
        if (wantInproc) {
#ifndef MPX_HAVE_LIBAV
            abortInprocSession(
                "media: inproc_decode=1 but binary has no libav — abort (no pipe:1)");
            return;
#else
            if (!(wantYuvDdr && presentMode_ == "fpga" &&
                  videoFmt == RawVideoFormat::Yuv420p)) {
                abortInprocSession(
                    "media: inproc_decode=1 needs fpga yuv420p 2-slot — abort (no pipe:1)");
                return;
            }
            AvInprocOpenOpts iopts;
            iopts.cancelled = &stop_;
            iopts.threads = 2;
            iopts.startMs = startMs;
            iopts.headers = headers;
            if (isPlex960BankSize(outW_, outH_) || isPlex960BankSize(rawW, rawH)) {
                if (sourceMediaW_ == kInproc960W && sourceMediaH_ == kInproc960H) {
                    iopts.expectW = kInproc960W;
                    iopts.expectH = kInproc960H;
                } else {
                    // Leftover / 1280 clip: decode coded 1280×720, pack 960×540.
                    iopts.expectW = kInproc720W;
                    iopts.expectH = kInproc720H;
                    iopts.outW = kInproc960W;
                    iopts.outH = kInproc960H;
                }
            } else {
                iopts.expectW = rawW;
                iopts.expectH = rawH;
            }
            std::string ierr;
            std::string inprocUrl = url;
            const bool httpSrc = (url.compare(0, 7, "http://") == 0 ||
                                  url.compare(0, 8, "https://") == 0);
            const bool remuxCopyAudio = misterplex::p720_av::inprocRemuxMustCopyAudio(
                isPlex720pDdrFrameGeometry(ddrGeometry) && !isPlex960BankSize(rawW, rawH),
                true);
            if (httpSrc && inprocUrl == url) {
                // ARM libav: file+h264 only (no HTTP, no AAC). Box ffmpeg remuxes
                // one HTTP into annex-B ± PCM fifos. Direct HTTP open → Protocol
                // not found; mpegts+AAC inproc SIGABRT'd (dl-call-libc-early-init).
                // Sole reader on each fifo: a leftover O_RDWR hold stole annex-B
                // bytes from libav (avcodec_send_packet Invalid data).
                const char* vfifo = "/tmp/mplex-inproc.h264";
                const char* afifo = "/tmp/mplex-inproc.pcm";
                ::unlink(vfifo);
                if (remuxCopyAudio)
                    ::unlink(afifo);
                auto mkSizedFifo = [](const char* path, int bytes) -> bool {
                    if (mkfifo(path, 0644) != 0)
                        return false;
                    const int fd = ::open(path, O_RDWR | O_NONBLOCK);
                    if (fd < 0)
                        return false;
#ifdef F_SETPIPE_SZ
                    (void)::fcntl(fd, F_SETPIPE_SZ, bytes);
#else
                    (void)bytes;
#endif
                    ::close(fd);
                    return true;
                };
                const bool vok =
                    mkSizedFifo(vfifo, remuxCopyAudio ? 4 * 1024 * 1024 : 1048576);
                const bool aok =
                    !remuxCopyAudio || mkSizedFifo(afifo, 4 * 1024 * 1024);
                if (vok && aok) {
                    if (remuxCopyAudio) {
                        // Hold is a dummy writer so ffmpeg's PCM open is not
                        // ENXIO during video rendezvous. Pump reads O_RDONLY:
                        // sharing O_RDWR with the pump hides EOF when remux
                        // exits (pipe_read forever → stop() joins forever →
                        // playMedia HTTP blocks on playHandoffMu → spinner).
                        int hold = remuxPcmHoldFd_.exchange(-1);
                        if (hold >= 0)
                            ::close(hold);
                        hold = ::open(afifo, O_RDWR | O_NONBLOCK);
                        remuxPcmHoldFd_.store(hold);
                        remuxPcmFd = ::open(afifo, O_RDONLY | O_NONBLOCK);
                    }
                    const pid_t rpid = spawnHttpRemuxMpegts(
                        url, headers, startMs, vfifo, remuxCopyAudio,
                        remuxCopyAudio ? afifo : "");
                    if (rpid > 0) {
                        streamPid_.store(rpid);
                        inprocUrl = vfifo;
                        iopts.startMs = 0;
                        iopts.liveFifo = true;
                        log(std::string("media: inproc_decode remux fifo http→") +
                            (remuxCopyAudio ? "annexb+pcm" : "annexb"));
                    } else {
                        log("media: inproc_decode remux spawn failed — pipe");
                        if (remuxPcmFd >= 0) {
                            ::close(remuxPcmFd);
                            remuxPcmFd = -1;
                        }
                        int hold = remuxPcmHoldFd_.exchange(-1);
                        if (hold >= 0)
                            ::close(hold);
                    }
                } else {
                    log(std::string("media: inproc_decode mkfifo failed errno=") +
                        std::to_string(errno) + " — pipe");
                }
            }
            if (remuxPcmFd >= 0) {
                // Gate MrAudio until the first picture, but drain PCM during
                // avformat_open so remux cannot fill the fifo and deadlock.
                audioFeedRelease_.store(false);
                audioReleaseAfterPresents_.store(0);
                holdAudioToPictures_.store(false);
                const int pfd = remuxPcmFd;
                remuxPcmFd = -1;
                remuxPcmReadFd_.store(pfd);
                audioThr_ = std::thread([this, pfd] { audioPump(pfd); });
                remuxPcmPumpEarly = true;
            }
#ifdef MPX_HAVE_LIBAV
            inprocPcm_ = &inprocDec;
#endif
            const bool inprocOpened = inprocDec.open(inprocUrl, iopts, ierr);
            if (inprocOpened && remuxPcmPumpEarly)
                audioFeedRelease_.store(true);
            if (!inprocOpened) {
                const pid_t rp = streamPid_.exchange(-1);
                if (rp > 0) {
                    ::kill(rp, SIGTERM);
                    ::kill(-rp, SIGTERM);
                }
                if (remuxPcmFd >= 0) {
                    ::close(remuxPcmFd);
                    remuxPcmFd = -1;
                }
                if (isPlex720pDdrFrameGeometry(ddrGeometry) &&
                    !isPlex960BankSize(rawW, rawH)) {
                    abortInprocSession("media: inproc_decode open failed: " + ierr +
                                       " — abort (no spawnAudioOnly pipe)");
                    return;
                }
                log("media: inproc_decode open failed: " + ierr +
                    " — pipe fallback");
            } else {
            useInproc = true;
            std::string extra;
            if (inprocDec.width() != iopts.expectW ||
                inprocDec.height() != iopts.expectH) {
                extra = " src=" + std::to_string(iopts.expectW) + "x" +
                        std::to_string(iopts.expectH) + " scale=4/3";
            } else if (sourceMediaW_ > 0 && sourceMediaH_ > 0 &&
                       (sourceMediaW_ != inprocDec.width() ||
                        sourceMediaH_ != inprocDec.height())) {
                extra = " src=" + std::to_string(sourceMediaW_) + "x" +
                        std::to_string(sourceMediaH_) + " scale=nearest";
            }
            log(std::string("media: inproc_decode=1 libav=") +
                AvInprocDecoder::libavIdent() + " wxh=" +
                std::to_string(inprocDec.width()) + "x" +
                std::to_string(inprocDec.height()) + extra + " pix=yuv420p");
            }
#endif
        }

        const bool sameDemuxAudio =
            misterplex::p720_av::inprocSameDemuxAudioWanted(
                useInproc && isPlex720pDdrFrameGeometry(ddrGeometry) &&
                    !isPlex960BankSize(rawW, rawH),
                useInproc)
#ifdef MPX_HAVE_LIBAV
            && inprocDec.hasAudio()
#endif
            ;
        const bool remuxPcmAudio =
            remuxPcmPumpEarly || (remuxPcmFd >= 0 && !sameDemuxAudio);

        int vpipe[2] = {-1, -1};
        int apipe[2] = {-1, -1};
        if (!useInproc && pipe(vpipe) != 0) {
            log("media: video pipe failed");
            playing_.store(false);
            killChildren();
            if (streamThr_.joinable())
                streamThr_.join();
            return;
        }
        // Enlarge video pipe so decode can run ahead of uncached DDR present.
        // Default pipe (~64KiB) and even pipe-max 1MiB are < one 720p I420 frame
        // (1.38MiB) — producer blocks mid-frame. Prefer multi-frame capacity when
        // the kernel allows (sysctl fs.pipe-max-size ≥ 4–8MiB on lab).
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
#ifndef F_GETPIPE_SZ
#define F_GETPIPE_SZ 1032
#endif
        if (!useInproc) {
            const int wantPipe = 4 * 1024 * 1024; // 4 MiB ≈ 2.9× 720p I420
            int got0 = static_cast<int>(::fcntl(vpipe[0], F_SETPIPE_SZ, wantPipe));
            int got1 = static_cast<int>(::fcntl(vpipe[1], F_SETPIPE_SZ, wantPipe));
            int sz = static_cast<int>(::fcntl(vpipe[0], F_GETPIPE_SZ));
            log("media: video_pipe_sz want=" + std::to_string(wantPipe) +
                " set0=" + std::to_string(got0) + " set1=" + std::to_string(got1) +
                " get=" + std::to_string(sz));
        }
        if (wantAudio && !sameDemuxAudio && !remuxPcmAudio && pipe(apipe) != 0) {
            log("media: audio pipe failed — video only");
            apipe[0] = apipe[1] = -1;
        } else if (wantAudio && apipe[0] >= 0) {
            // Gated trim may pause MrAudio writes ~1–3 s; 1 MiB ≈ 5 s of
            // 48 kHz s16le stereo so ffmpeg does not stall I420.
            const int wantA = 1024 * 1024;
            (void)::fcntl(apipe[0], F_SETPIPE_SZ, wantA);
            (void)::fcntl(apipe[1], F_SETPIPE_SZ, wantA);
        }

        if (!useInproc) {
            // Never log raw PMS tokens (X-Plex-Token / query) into misterplexd.log.
            auto redactArg = [](std::string a) {
                const char* keys[] = {"X-Plex-Token=", "X-Plex-Token%3D", "token="};
                for (const char* k : keys) {
                    std::size_t p = 0;
                    while ((p = a.find(k, p)) != std::string::npos) {
                        const std::size_t v = p + std::strlen(k);
                        std::size_t e = a.find_first_of("& \t\"'", v);
                        if (e == std::string::npos)
                            e = a.size();
                        a.replace(v, e - v, "REDACTED");
                        p = v + 8;
                    }
                }
                return a;
            };
            if (splitAv720)
                log("media: spawn 720p split A/V (video -an + audio-only) — hypothesized audio backpressure relief");
            std::string joined =
                splitAv720 ? "media: spawn 720p video" : "media: spawn single-process";
            for (const auto& a : args) {
                joined += ' ';
                if (a.find(' ') != std::string::npos || a.find('\r') != std::string::npos)
                    joined += "[...]";
                else
                    joined += redactArg(a);
            }
            log(joined);

            // Combined 480p: one ffmpeg owns pipe:1 + pipe:3. Split 720p: video
            // ffmpeg is video-only (no aWriteFd); audio-only is a second process.
            pid_t pid = spawnFfmpeg(args, vpipe[1],
                                    (splitAv720 || apipe[1] < 0) ? -1 : apipe[1]);
            if (pid < 0) {
                ::close(vpipe[1]);
                if (apipe[1] >= 0)
                    ::close(apipe[1]);
                ::close(vpipe[0]);
                if (apipe[0] >= 0)
                    ::close(apipe[0]);
                log("media: fork failed");
                playing_.store(false);
                killChildren();
                if (streamThr_.joinable())
                    streamThr_.join();
                return;
            }
            childPid_.store(pid);
        }

        if (sameDemuxAudio) {
#ifdef MPX_HAVE_LIBAV
            inprocPcm_ = &inprocDec;
            log(std::string("media: inproc_audio=same_demux has_audio=") +
                (inprocDec.hasAudio() ? "1" : "0"));
#else
            log("media: inproc_audio=same_demux has_audio=0");
#endif
        } else if (remuxPcmPumpEarly) {
            log("media: inproc_audio=remux_pcm (one HTTP, no spawnAudioOnly)");
        } else if (remuxPcmAudio) {
            apipe[0] = remuxPcmFd;
            remuxPcmFd = -1;
            apipe[1] = -1;
            log("media: inproc_audio=remux_pcm (one HTTP, no spawnAudioOnly)");
        } else if (useInproc && isPlex720pDdrFrameGeometry(ddrGeometry) &&
                   !isPlex960BankSize(rawW, rawH)) {
            log("media: inproc_audio=abort remux_pcm fd missing — no spawnAudioOnly");
            playing_.store(false);
            killChildren();
            if (streamThr_.joinable())
                streamThr_.join();
            return;
        } else if (splitAv720 && apipe[1] >= 0) {
            pid_t apid = spawnAudioOnly(audioUrl, audioHeaders, audioStartMs, apipe[1]);
            if (apid > 0) {
                audioPid_.store(apid);
            } else {
                log("media: 720p audio-only fork failed — continuing video-only");
                ::close(apipe[0]);
                apipe[0] = -1;
            }
        }

        if (vpipe[1] >= 0)
            ::close(vpipe[1]);
        if (apipe[1] >= 0)
            ::close(apipe[1]);
        rfd = vpipe[0];
        // Blocking read: present thread sleeps in kernel until ffmpeg produces
        // bytes — frees A9 for decode. Nonblock+EAGAIN sleep fought the producer
        // (200us spin → 19 pfps; 2ms sleep → 20.7). Blocking is the clean yield.
        if (rfd >= 0) {
            const int rflags = fcntl(rfd, F_GETFL, 0);
            if (rflags >= 0)
                fcntl(rfd, F_SETFL, rflags & ~O_NONBLOCK);
        }

        if (!remuxPcmPumpEarly &&
            (apipe[0] >= 0 || sameDemuxAudio || remuxPcmAudio)) {
            // Gate MrAudio for every 720p inproc session (60-frame warmup).
            // Extra after_video_ms=100 is only native 1280 (lands ~+4 ms).
            // Scaled 240/480: gate until first kick, then 0 ms.
            const bool warmup720Inproc =
                useInproc && isPlex720pDdrFrameGeometry(ddrGeometry) &&
                !isPlex960BankSize(rawW, rawH) &&
                !isPlex960DdrFrameGeometry(ddrGeometry);
            const bool native1280Inproc =
                warmup720Inproc && sourceMediaW_ == kInproc720W &&
                sourceMediaH_ == kInproc720H;
            audioFeedRelease_.store(true);
            // Per-raster / per-source-class bake. AUDIO_DELAY_MS stays the
            // user adelay knob. Conf AV_HDMI_AUDIO_LAG_MS>0 overrides after_ms.
            const char* dlab = (displayRasterLatched_ && latchedDisplayRes_.label)
                                   ? latchedDisplayRes_.label
                                   : "720p";
            const HdmiAutoDelay baked =
                hdmiAutoDelayForPlay(dlab, sourceMediaW_, sourceMediaH_);
            if (native1280Inproc || warmup720Inproc) {
                // 480p-class: start MrAudio with video. Gating until first
                // kick left audio_s/wall≈0.91 at 57 s while unique was 23.7.
                audioAfterVideoMs_.store(0);
                audioHoldSlackMs_.store(baked.holdSlackMs);
                audioReleaseAfterPresents_.store(0);
                log("media: hdmi_audio_lag path=inproc after_video_ms=0 src=" +
                    std::to_string(sourceMediaW_) + "x" +
                    std::to_string(sourceMediaH_));
            } else {
                audioAfterVideoMs_.store(0);
                // 480p gold + combined 720p prefetch: start MrAudio with video.
                // The 24-present gate left 1 s of silence and audio_s/wall≈0.98
                // at 40 s (Star Trek stutter). Inproc warmup still gates above.
                const bool startWithVideo =
                    misterplex::p720_av::combined720pAudioStartsWithVideo(useInproc);
                if (startWithVideo) {
                    audioFeedRelease_.store(true);
                    audioReleaseAfterPresents_.store(0);
                    log("media: hdmi_audio_lag path=pipe after_video_ms=0");
                } else {
                    audioFeedRelease_.store(false);
                    audioReleaseAfterPresents_.store(24);
                    log("media: hdmi_audio_lag path=pipe 720p_trim_after_presents=24");
                }
            }
            holdAudioToPictures_.store(misterplex::p720_av::holdAudioToPicturesWanted(
                isPlex720pDdrFrameGeometry(ddrGeometry) && !isPlex960BankSize(rawW, rawH),
                useInproc));
            log("media: hold_audio_to_pictures=" +
                std::string(holdAudioToPictures_.load() ? "1" : "0") +
                " skip_av_hold=" +
                (misterplex::p720_av::combined720pSkipAvHold(
                     isPlex720pDdrFrameGeometry(ddrGeometry), useInproc)
                     ? "1"
                     : "0"));
            const int pumpFd = sameDemuxAudio ? -1 : apipe[0];
            audioThr_ = std::thread([this, pumpFd] { audioPump(pumpFd); });
        }

        auto closeRemuxHolds = [&]() {
            if (remuxHoldV >= 0) {
                ::close(remuxHoldV);
                remuxHoldV = -1;
            }
            if (remuxHoldA >= 0) {
                ::close(remuxHoldA);
                remuxHoldA = -1;
            }
            if (remuxPcmFd >= 0) {
                ::close(remuxPcmFd);
                remuxPcmFd = -1;
            }
            int hold = remuxPcmHoldFd_.exchange(-1);
            if (hold >= 0)
                ::close(hold);
        };

        const size_t frameBytes = rawVideoFrameBytes(videoFmt, rawW, rawH);
        if (budget960Wanted() || isPlex960BankSize(rawW, rawH)) {
            log("media: P5_960 produce contract bank=" + std::to_string(rawW) + "x" +
                std::to_string(rawH) + " frame_bytes=" + std::to_string(frameBytes) +
                " want=" + std::to_string(static_cast<size_t>(kPlex960Yuv420pBytes)) +
                " src=" + std::to_string(sourceMediaW_) + "x" +
                std::to_string(sourceMediaH_) +
                " skip_last_presented=" +
                (skipLastPresentedWanted() ? "1" : "0") +
                " split_av=" + (splitAv720 ? "1" : "0"));
        }
        std::vector<uint8_t> frame(frameBytes);
        std::vector<uint8_t> fbOverlayBackup;
        I420DirtyBackup i420OverlayBackup;
        // Program DDR geometry before direct bank ingest (pipe→bank).
        if (useDdrF1_ && wantFpgaFrameStore && videoFmt == RawVideoFormat::Yuv420p) {
            if (!fpga_.setDdrFrameLayout(ddrGeometry, DdrFrameFormat::Yuv420p))
                log("media: setDdrFrameLayout failed: " + fpga_.lastError());
            else
                log("media: DDR layout coded=" + std::to_string(rawW) + "x" +
                    std::to_string(rawH) + " frame_bytes=" + std::to_string(frameBytes) +
                    " direct_ingest=" +
                    (presentMode_ == "fpga" ? "on" : "off"));
        }

        struct PresentProfileAccum {
            int64_t frames = 0;
            int64_t presented = 0;
            int64_t readCalls = 0;
            int64_t readOkCalls = 0;
            int64_t readEagain = 0;
            int64_t readEintr = 0;
            int64_t readZero = 0;
            int64_t readBytes = 0;
            int64_t readMaxBytes = 0;
            int64_t readWallUs = 0;
            int64_t readCpuUs = 0;
            int64_t readSyscallUs = 0;
            int64_t readSleepUs = 0;
            int64_t pacingWaitUs = 0;
            int64_t pacingWaitCpuUs = 0;
            int64_t overlayUs = 0;
            int64_t overlayCpuUs = 0;
            int64_t fbUs = 0;
            int64_t fbCpuUs = 0;
            int64_t pixelUs = 0;
            int64_t pixelCpuUs = 0;
            int64_t ddrPrepWaitUs = 0;
            int64_t ddrCopyUs = 0;
            int64_t ddrFlushUs = 0;
            int64_t ddrDoorbellUs = 0;
            int64_t ddrPostWaitUs = 0;
            int64_t ddrBankReuseWaitUs = 0;
            int64_t ddrTotalUs = 0;
            int64_t ddrCpuUs = 0;
            int64_t ddrUnaccountedUs = 0;
            int64_t drops = 0;
        } prof;
        const bool profilePresent = presentProfile_;
        auto logProfile = [&]() {
            if (!profilePresent || prof.frames <= 0)
                return;
            const int64_t presented = prof.presented > 0 ? prof.presented : 1;
            const int64_t readOk = prof.readOkCalls > 0 ? prof.readOkCalls : 1;
            auto avgFrame = [&](int64_t us) { return us / prof.frames; };
            auto avgFrameX100 = [&](int64_t v) { return (v * 100) / prof.frames; };
            auto avgPresented = [&](int64_t us) { return us / presented; };
            auto avgRead = [&](int64_t v) { return v / readOk; };
            const int64_t readLoopUs =
                std::max<int64_t>(0, prof.readWallUs - prof.readSyscallUs - prof.readSleepUs);
            const int64_t ddrWaitUs = prof.ddrPrepWaitUs + prof.ddrPostWaitUs;
            const int64_t ddrAccountedUs =
                ddrWaitUs + prof.ddrCopyUs + prof.ddrFlushUs + prof.ddrDoorbellUs;
            log("media: present_profile frames=" + std::to_string(prof.frames) +
                " presented=" + std::to_string(prof.presented) +
                " drops=" + std::to_string(prof.drops) +
                " read_us_f=" + std::to_string(avgFrame(prof.readWallUs)) +
                " read_cpu_us_f=" + std::to_string(avgFrame(prof.readCpuUs)) +
                " read_syscall_us_f=" + std::to_string(avgFrame(prof.readSyscallUs)) +
                " read_eagain_sleep_us_f=" + std::to_string(avgFrame(prof.readSleepUs)) +
                " read_loop_overhead_us_f=" + std::to_string(avgFrame(readLoopUs)) +
                " read_calls_f=" + std::to_string(prof.readCalls / prof.frames) +
                " read_calls_x100_f=" + std::to_string(avgFrameX100(prof.readCalls)) +
                " read_ok_calls_f=" + std::to_string(prof.readOkCalls / prof.frames) +
                " read_ok_calls_x100_f=" + std::to_string(avgFrameX100(prof.readOkCalls)) +
                " read_eagain_f=" + std::to_string(prof.readEagain / prof.frames) +
                " read_eagain_x100_f=" + std::to_string(avgFrameX100(prof.readEagain)) +
                " read_eintr_f=" + std::to_string(prof.readEintr / prof.frames) +
                " read_zero=" + std::to_string(prof.readZero) +
                " read_bytes_f=" + std::to_string(prof.readBytes / prof.frames) +
                " read_avg_bytes_call=" + std::to_string(avgRead(prof.readBytes)) +
                " read_max_bytes_call=" + std::to_string(prof.readMaxBytes) +
                " pacing_wait_us_f=" + std::to_string(avgFrame(prof.pacingWaitUs)) +
                " pacing_wait_cpu_us_f=" + std::to_string(avgFrame(prof.pacingWaitCpuUs)) +
                " overlay_us_p=" + std::to_string(avgPresented(prof.overlayUs)) +
                " overlay_cpu_us_p=" + std::to_string(avgPresented(prof.overlayCpuUs)) +
                " fb_us_p=" + std::to_string(avgPresented(prof.fbUs)) +
                " fb_cpu_us_p=" + std::to_string(avgPresented(prof.fbCpuUs)) +
                " pixel_us_p=" + std::to_string(avgPresented(prof.pixelUs)) +
                " pixel_cpu_us_p=" + std::to_string(avgPresented(prof.pixelCpuUs)) +
                " ddr_wait_us_p=" + std::to_string(avgPresented(ddrWaitUs)) +
                " ddr_prep_wait_us_p=" + std::to_string(avgPresented(prof.ddrPrepWaitUs)) +
                " ddr_copy_us_p=" + std::to_string(avgPresented(prof.ddrCopyUs)) +
                " ddr_flush_us_p=" + std::to_string(avgPresented(prof.ddrFlushUs)) +
                " ddr_doorbell_us_p=" + std::to_string(avgPresented(prof.ddrDoorbellUs)) +
                " ddr_post_wait_us_p=" + std::to_string(avgPresented(prof.ddrPostWaitUs)) +
                " ddr_bank_reuse_wait_us_p=" +
                    std::to_string(avgPresented(prof.ddrBankReuseWaitUs)) +
                " ddr_accounted_us_p=" + std::to_string(avgPresented(ddrAccountedUs)) +
                " ddr_unaccounted_us_p=" +
                    std::to_string(avgPresented(prof.ddrUnaccountedUs)) +
                " ddr_total_us_p=" + std::to_string(avgPresented(prof.ddrTotalUs)) +
                " ddr_cpu_us_p=" + std::to_string(avgPresented(prof.ddrCpuUs)) +
                " frame_bytes=" + std::to_string(frameBytes) +
                " fmt=" + ffmpegPixFmt(videoFmt));
            prof = PresentProfileAccum{};
        };

        auto blitFrame = [&](const uint8_t* data) {
            if (!fb_.ok() || !CoreGate::process().open())
                return;
            bool fbOk = false;
            switch (videoFmt) {
            case RawVideoFormat::Rgb565Le:
                fbOk = fb_.blitRgb565Le(data, rawW, rawH);
                break;
            case RawVideoFormat::Bgra32:
                fbOk = fb_.blitBgra32(data, rawW, rawH);
                break;
            case RawVideoFormat::Yuv420p:
                fbOk = fb_.blitYuv420p(data, rawW, rawH);
                break;
            case RawVideoFormat::Rgb24:
            default:
                fbOk = fb_.blitRgb24(data, rawW, rawH);
                break;
            }
            if (!fbOk)
                log("media: blit failed fmt=" + std::string(ffmpegPixFmt(videoFmt)));
        };

        auto renderPackedOverlay = [&](uint8_t* data) {
            switch (videoFmt) {
            case RawVideoFormat::Rgb565Le:
                overlay_.renderRgb565Le(data, rawW, rawH);
                break;
            case RawVideoFormat::Bgra32:
                overlay_.renderBgra32(data, rawW, rawH);
                break;
            case RawVideoFormat::Yuv420p:
                break;
            case RawVideoFormat::Rgb24:
            default:
                overlay_.renderRgb24(data, rawW, rawH);
                break;
            }
        };

        auto backupOverlayDirty = [&](uint8_t* cleanFrame, const OverlayRect& dirty) {
            fbOverlayBackup.clear();
            if (dirty.empty())
                return true;
            const size_t bpp = rawVideoPackedBytesPerPixel(videoFmt);
            if (bpp == 0)
                return false;
            const size_t rowBytes = static_cast<size_t>(dirty.w) * bpp;
            fbOverlayBackup.resize(rowBytes * static_cast<size_t>(dirty.h));
            for (int yy = 0; yy < dirty.h; ++yy) {
                const size_t src =
                    (static_cast<size_t>(dirty.y + yy) * rawW + dirty.x) * bpp;
                std::memcpy(fbOverlayBackup.data() + rowBytes * static_cast<size_t>(yy),
                            cleanFrame + src, rowBytes);
            }
            return true;
        };

        auto restoreOverlayDirty = [&](uint8_t* cleanFrame, const OverlayRect& dirty) {
            if (dirty.empty())
                return;
            if (videoFmt == RawVideoFormat::Yuv420p) {
                (void)i420OverlayBackup.restore(cleanFrame, rawW, rawH);
                return;
            }
            if (fbOverlayBackup.empty())
                return;
            const size_t bpp = rawVideoPackedBytesPerPixel(videoFmt);
            if (bpp == 0)
                return;
            const size_t rowBytes = static_cast<size_t>(dirty.w) * bpp;
            for (int yy = 0; yy < dirty.h; ++yy) {
                const size_t dst =
                    (static_cast<size_t>(dirty.y + yy) * rawW + dirty.x) * bpp;
                std::memcpy(cleanFrame + dst,
                            fbOverlayBackup.data() + rowBytes * static_cast<size_t>(yy),
                            rowBytes);
            }
        };

        auto presentCleanFrame = [&](uint8_t* cleanFrame, bool countPresent) {
            OverlayRect dirty{};
            bool overlayBackedUp = false;
            if (videoFmt == RawVideoFormat::Yuv420p) {
                if (profilePresent) {
                    const auto overlay0 = std::chrono::steady_clock::now();
                    const int64_t overlayCpu0 = threadCpuMicros();
                    overlayBackedUp = overlay_.renderI420WithBackup(
                        cleanFrame, rawW, rawH, i420OverlayBackup);
                    const int64_t overlayCpu1 = threadCpuMicros();
                    const auto overlay1 = std::chrono::steady_clock::now();
                    if (overlayBackedUp) {
                        prof.overlayUs += microsBetween(overlay0, overlay1);
                        prof.overlayCpuUs += overlayCpu1 - overlayCpu0;
                    }
                } else {
                    overlayBackedUp = overlay_.renderI420WithBackup(
                        cleanFrame, rawW, rawH, i420OverlayBackup);
                }
                dirty = i420OverlayBackup.rect;
            } else {
                dirty = overlay_.dirtyBounds(rawW, rawH);
                overlayBackedUp = backupOverlayDirty(cleanFrame, dirty);
                if (!dirty.empty() && overlayBackedUp) {
                    if (profilePresent) {
                        const auto overlay0 = std::chrono::steady_clock::now();
                        const int64_t overlayCpu0 = threadCpuMicros();
                        renderPackedOverlay(cleanFrame);
                        const int64_t overlayCpu1 = threadCpuMicros();
                        const auto overlay1 = std::chrono::steady_clock::now();
                        prof.overlayUs += microsBetween(overlay0, overlay1);
                        prof.overlayCpuUs += overlayCpu1 - overlayCpu0;
                    } else {
                        renderPackedOverlay(cleanFrame);
                    }
                }
            }

            if (fb_.ok()) {
                if (profilePresent) {
                    const auto fb0 = std::chrono::steady_clock::now();
                    const int64_t fbCpu0 = threadCpuMicros();
                    blitFrame(cleanFrame);
                    const int64_t fbCpu1 = threadCpuMicros();
                    const auto fb1 = std::chrono::steady_clock::now();
                    prof.fbUs += microsBetween(fb0, fb1);
                    prof.fbCpuUs += fbCpu1 - fbCpu0;
                } else {
                    blitFrame(cleanFrame);
                }
            }

            const bool reconOwnsF1 = streamEnabled_ && reconPresentOk_.load();
            if (!reconOwnsF1 && wantFpgaFrameStore) {
                const uint8_t* txFrame = cleanFrame;
                size_t txBytes = frameBytes;

                // Serialise with the OSD poller / idle painter: FpgaSpi keeps
                // transaction state, so overlapping ioctls corrupt each other.
                std::lock_guard<std::mutex> lk(presentMu_);
                bool ok = false;
                if (useDdrF1_) {
                    const int64_t ddrCpu0 = profilePresent ? threadCpuMicros() : 0;
                    if (videoFmt == RawVideoFormat::Yuv420p) {
                        ok = fpga_.sendYuv420pFrameDdr(txFrame, txBytes, ddrGeometry, ddrBank_);
                    } else {
                        ok = false;
                    }
                    if (profilePresent && ok) {
                        const int64_t ddrCpu1 = threadCpuMicros();
                        const auto dt = fpga_.lastDdrTiming();
                        const int64_t accounted = dt.prep_wait_us + dt.copy_us + dt.flush_us +
                                                  dt.doorbell_us + dt.post_wait_us;
                        prof.ddrPrepWaitUs += dt.prep_wait_us;
                        prof.ddrCopyUs += dt.copy_us;
                        prof.ddrFlushUs += dt.flush_us;
                        prof.ddrDoorbellUs += dt.doorbell_us;
                        prof.ddrPostWaitUs += dt.post_wait_us;
                        prof.ddrBankReuseWaitUs += dt.bank_reuse_wait_us;
                        prof.ddrTotalUs += dt.total_us;
                        prof.ddrCpuUs += ddrCpu1 - ddrCpu0;
                        if (dt.total_us > accounted)
                            prof.ddrUnaccountedUs += dt.total_us - accounted;
                    }
                    if (ok)
                        ddrBank_ ^= 1;
                    if (!ok)
                        log("media: DDR YUV420p F1 unavailable: " + fpga_.lastError());
                }
                if (!ok && videoFmt != RawVideoFormat::Yuv420p) {
                    log("media: non-YUV F1 frame refused before send; frame store requires DDR "
                        "YUV420p");
                }
                if (!ok) {
                    if (countPresent && (frameIndex % 30) == 0)
                        log("media: fpga frame_tx: " + fpga_.lastError());
                } else if (countPresent) {
                    ++presentCount_;
                    if (profilePresent)
                        ++prof.presented;
                    if ((presentCount_ % 48) == 0) {
                        const auto dt = fpga_.lastDdrTiming();
                        log(std::string("media: fpga frame_tx ok via ") +
                            "DDR" +
                            " presents=" + std::to_string(presentCount_) +
                            " frames=" + std::to_string(frameIndex) +
                            " ms=" + std::to_string(static_cast<int>(fpga_.lastPushMs())) +
                            " prep_us=" + std::to_string(dt.prep_wait_us) +
                            " copy_us=" + std::to_string(dt.copy_us) +
                            " flush_us=" + std::to_string(dt.flush_us) +
                            " doorbell_us=" + std::to_string(dt.doorbell_us) +
                            " post_us=" + std::to_string(dt.post_wait_us) +
                            " bank_reuse_us=" + std::to_string(dt.bank_reuse_wait_us) +
                            " plxd_us=" + std::to_string(dt.plxa_poll_us) +
                            " plxd_iters=" + std::to_string(dt.plxa_poll_iters) +
                            " plxd_used=" + (dt.plxa_used ? "1" : "0") +
                            " total_us=" + std::to_string(dt.total_us));
                    }
                }
            }

            if (overlayBackedUp)
                restoreOverlayDirty(cleanFrame, dirty);
        };

        if (onProgress_)
            onProgress_("playing", startMs, durationMs);

        // Deterministic A/V origin: never start the schedule on the wall clock and then
        // switch to the audio clock mid-stream — that step discontinuity randomises the
        // lipsync offset by tens of ms on every play (measured spread ~67 ms across
        // identical runs). Wait for the audio master clock to exist first.
        if (wantAudio && apipe[0] >= 0 && audioFeedRelease_.load()) {
            const auto waitStart = std::chrono::steady_clock::now();
            while (!stop_.load() && !audioActive_.load() &&
                   std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const int64_t waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - waitStart)
                                       .count();
            log("media: A/V origin armed audio_active=" +
                std::string(audioActive_.load() ? "1" : "0") +
                " waited_ms=" + std::to_string(waited));
        }
        t0 = std::chrono::steady_clock::now();
        hwPresentBaselineValid = fpga_.readBankRelease(hwPresentBaseline);
        hwPresentLastFrames = hwPresentBaseline.frames_done;
        hwPresentArmBaseline = presentCount_;
        hwPresentTotal = 0;
        hwPresentTimeBaseline = t0;
        log("media: hw_present_t0_rearm after_prefetch=1");
        // Architecture: 2-slot cached ring — reader fills host RAM while present
        // thread does uncached bank memcpy+kick. Overlaps the ~14ms copy with
        // the next pipe read (direct uncached read was ~30ms serial).
        // Branch B (opt-in MPX_FABRIC_DIRECT): present cache-cleans the slot,
        // pokes PLXP (src_phys), DSB, existing doorbell — skips uncached memcpy.
        // PATH_SDRAM: stick DMA is PHYS_BASE. sendDdrFrame memcpy idle+play
        // into 0x30180000 then doorbell. Do not skip (empty bank = no chevron).
        const bool pipelineDdr = useDdrF1_ && wantFpgaFrameStore &&
                                 videoFmt == RawVideoFormat::Yuv420p &&
                                 presentMode_ == "fpga";
        const bool true480Pipeline =
            pipelineDdr && isPlex480pDdrFrameGeometry(ddrGeometry);
        // Host-paint 480i product uses DECODE/present 720x480 on the e36 core.
        // That is NOT the true480 640x480 bank geometry — without an explicit
        // match here av_lock stayed 0 and DDR_PIPE free-ran ~30–40 pfps while
        // MrAudio wall-paced (HDMI beep/flash skew + ring depth 600–900 ms).
        const bool hostPaint720x480 =
            pipelineDdr && ddrGeometry.presented_width == 720 &&
            ddrGeometry.presented_height == 480;
        // 720p L4: RequireReleased waits on PLXD when free=0 and produces ~15
        // while ffmpeg→/dev/null on the same live core holds 24 fps (480/20s).
        // Triple-buffer BestEffort writes a free bank (or non-display) without
        // a 150ms frames_done poll. true480 2-bank stays strict.
        const bool strictDdrPipeline = true480Pipeline;
        // 720p matches 480p A/V: audio master (heard/wall 48 kHz), video hold-only.
        // RequireReleased stays 480p-only (L4 poll produced ~15 unique).
        // host-paint 720x480 must lock too — otherwise lipsync is open-loop.
        const bool avLockPresent =
            pipelineDdr && (true480Pipeline || hostPaint720x480 ||
                            isPlex720pDdrFrameGeometry(ddrGeometry));
        if (pipelineDdr) {
            const bool wantStick =
                stickI420Wanted() &&
                misterplex::p720_av::plex720pWcBankIngest(
                    isPlex720pDdrFrameGeometry(ddrGeometry));
            fpga_.setStickI420Present(wantStick);
            // Publication offload: pipe/inproc writes WC present bank, present
            // doorbells. PLXD 150 ms spin on the reader is banned (pipe:3 starve).
            const bool fabricWant =
                fabricDirectWanted() && isPlex720pDdrFrameGeometry(ddrGeometry);
            bool wantStickIngest = misterplex::p720_av::stickIngestWantedOn720pPipe(
                plex720pStickIngestOverridesFabric(wantStick, fabricWant), useInproc);
            const bool wantFabric = fabricWant && !wantStickIngest;
            fpga_.setFabricDirectPresent(wantFabric);
            log("media: present_pipeline=" +
                std::string(wantStickIngest ? "2slot_stick_ingest"
                                            : "2slot_cached_ring") +
                " frame_bytes=" + std::to_string(frameBytes) +
                " true480_exact_gate=" + (true480Pipeline ? "1" : "0") +
                " strict_ddr_gate=" + (strictDdrPipeline ? "1" : "0") +
                " fabric_direct=" + std::string(wantFabric ? "1" : "0") +
                " stick_i420=" + std::string(wantStick ? "1" : "0") +
                " stick_ingest=" + std::string(wantStickIngest ? "1" : "0") +
                " av_lock=" + (avLockPresent ? "1" : "0"));
            std::array<std::vector<uint8_t>, 2> ring;
            std::array<uint8_t*, 2> ringPtr{{nullptr, nullptr}};
            std::array<uint32_t, 2> ringPhys{{0, 0}};
            FabricDirectAlloc fabricAlloc{};
            auto releaseFabricSlots = [&]() { releaseFabricDirectAlloc(fabricAlloc); };
            bool fabricSlotsReal = false;
            if (wantFabric) {
                // R1: hugepage → compact + 2 MiB memalign retries → 16–32 MiB arena.
                // Full-span pagemap is the only REAL gate. Flag stays default OFF.
                fabricSlotsReal =
                    allocateFabricDirectSlots(fabricAlloc, frameBytes, /*tryCompact=*/true) &&
                    fabricAlloc.real();
                if (fabricSlotsReal) {
                    ringPtr[0] = fabricAlloc.slot[0].virt;
                    ringPtr[1] = fabricAlloc.slot[1].virt;
                    ringPhys[0] = fabricAlloc.slot[0].phys;
                    ringPhys[1] = fabricAlloc.slot[1].phys;
                }
            }
            if (!fabricSlotsReal) {
                ring[0].assign(frameBytes, 0);
                ring[1].assign(frameBytes, 0);
                ringPtr[0] = ring[0].data();
                ringPtr[1] = ring[1].data();
                if (wantFabric) {
                    // Heap is almost never 338-page contiguous; both-or-none.
                    ringPhys[0] = FpgaSpi::resolveCachedSrcPhys(ringPtr[0], frameBytes);
                    ringPhys[1] = FpgaSpi::resolveCachedSrcPhys(ringPtr[1], frameBytes);
                    failClosedFabricDirectPhysPair(ringPhys[0], ringPhys[1]);
                }
            }
            {
                char pbuf[80];
                std::snprintf(pbuf, sizeof(pbuf), "0x%08x,0x%08x",
                              static_cast<unsigned>(ringPhys[0]),
                              static_cast<unsigned>(ringPhys[1]));
                const bool srcReal = fabricDirectPhysPairReal(ringPhys[0], ringPhys[1]);
                const char* how = "stub";
                if (srcReal)
                    how = fabricSlotsReal ? fabricAlloc.how : "heap";
                std::string extra;
                if (wantFabric && !srcReal) {
                    extra = std::string(" pagemap=") +
                            pagemapPfnVisName(probePagemapPfnVisibility());
                    extra += " (need CAP_SYS_ADMIN PFN + contiguous cached System-RAM)";
                }
                log(std::string("media: fabric_direct src_phys=") +
                    (srcReal ? "REAL" : "STUB") + " slots=" + pbuf +
                    " alloc=" + how + extra);
            }
            std::array<int64_t, 2> ringFrameIndex{0, 0};
            std::array<int, 2> ringIngestBank{{-1, -1}};
            std::array<bool, 3> bankHeld{{false, false, false}};
            std::mutex ringMu;
            std::condition_variable ringCv;
            int fullCount = 0;
            auto releaseIngestBank = [&](int slot) {
                const int b = ringIngestBank[static_cast<size_t>(slot)];
                if (b >= 0 && b <= 2)
                    bankHeld[static_cast<size_t>(b)] = false;
                ringIngestBank[static_cast<size_t>(slot)] = -1;
            };
            auto takeIngestBank = [&]() -> int {
                // One PLXD sample. 150×1 ms spin starved combined pipe:3.
                std::lock_guard<std::mutex> lk(ringMu);
                BankReleaseStatus brs{};
                const bool have = fpga_.readBankRelease(brs);
                int pick = -1;
                if (have && brs.anyFree()) {
                    if ((brs.free_bank_mask & 1u) && !bankHeld[0])
                        pick = 0;
                    else if ((brs.free_bank_mask & 2u) && !bankHeld[1])
                        pick = 1;
                }
                if (pick < 0) {
                    const int fallback = have && brs.disp_bank <= 1
                                             ? (brs.disp_bank ^ 1)
                                             : 0;
                    if (fallback >= 0 && fallback <= 1 &&
                        !bankHeld[static_cast<size_t>(fallback)])
                        pick = fallback;
                    else if (!bankHeld[0])
                        pick = 0;
                    else if (!bankHeld[1])
                        pick = 1;
                }
                if (pick >= 0)
                    bankHeld[static_cast<size_t>(pick)] = true;
                return pick;
            };
            int readSlot = 0;
            int presentSlot = 0;
            const bool skipLastPresented = skipLastPresentedWanted();
            std::vector<uint8_t> lastPresentedFrame;
            if (!skipLastPresented)
                lastPresentedFrame.assign(frameBytes, 0);
            bool lastPresentedFrameValid = false;
            bool lastPresentedHadOverlay = false;
            if (skipLastPresented)
                log("media: skip lastPresentedFrame snapshot (P5 produce)");
            std::atomic<bool> readerEof{false};
            std::atomic<bool> pipelineFatal{false};
            std::atomic<int64_t> pipelinePresentCount{0};
            std::atomic<int64_t> pipeReadUs{0};
            std::atomic<int64_t> pipeSlotWaitUs{0};
            std::atomic<int64_t> pipePresentWaitUs{0};
            std::atomic<int64_t> pipeSendUs{0};
            std::atomic<int64_t> pipeCopyUs{0};
            const bool pipeProfile = presentProfile_;
            auto pipeUs = [](std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b) {
                return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
            };
            bool hdmiLagArmed = false;
            auto holdHdmiAudioLag = [&]() {
                if (hdmiLagArmed)
                    return;
                hdmiLagArmed = true;
                const int need = audioReleaseAfterPresents_.load();
                if (need > 0) {
                    log("media: hdmi_audio_lag video_first trim_after_presents=" +
                        std::to_string(need));
                    return;
                }
                // First video kick now. Audio pump (if gated) starts after
                // audioAfterVideoMs_ so the HDMI click meets the flash.
                audioFeedRelease_.store(true);
                log("media: hdmi_audio_lag video_first after_video_ms=" +
                    std::to_string(audioAfterVideoMs_.load()));
            };
            int localBank = ddrBank_;
            // This tree's DDR map is 2 banks + doorbell. Bank 2 is rejected
            // by sendDdrFrame/kickDdrDoorbell (map_bytes = 2*stride).
            const int nBanks = 2;
            auto nextBank = [nBanks](int b) {
                if (nBanks <= 1)
                    return 0;
                if (b < 0)
                    return 0;
                return (b + 1) % nBanks;
            };
#ifdef MPX_HAVE_LIBAV
            // First 3s on leftover 2bbe: hw_match=1 and pfps≈hw (~10). FPGA
            // already swaps every kick; last-line 19.86 is host warmup inside
            // the 15s window. Prime both ring slots then re-arm t0/hw baseline.
            if (useInproc && !strictDdrPipeline &&
                isPlex720pDdrFrameGeometry(ddrGeometry) &&
                !isPlex960DdrFrameGeometry(ddrGeometry) &&
                !isPlex960BankSize(rawW, rawH)) {
                int primed = 0;
                for (int i = 0; i < 2 && !stop_.load(); ++i) {
                    std::string ierr;
                    uint8_t* dst = ringPtr[static_cast<size_t>(readSlot)];
                    if (!dst)
                        break;
                    const int rc = inprocDec.readI420(dst, frameBytes, ierr);
                    if (rc != 1) {
                        log("media: warmup_prefill abort rc=" +
                            std::to_string(rc) + " " + ierr);
                        break;
                    }
                    ++frameIndex;
                    ringFrameIndex[static_cast<size_t>(readSlot)] = frameIndex;
                    readSlot = (readSlot + 1) % 2;
                    ++fullCount;
                    ++primed;
                }
                t0 = std::chrono::steady_clock::now();
                hwPresentBaselineValid = fpga_.readBankRelease(hwPresentBaseline);
                hwPresentLastFrames = hwPresentBaseline.frames_done;
                hwPresentArmBaseline = 0;
                hwPresentTotal = 0;
                hwPresentTimeBaseline = t0;
                log("media: warmup_prefill n=" + std::to_string(primed) +
                    " t0_rearm=1");
            }
            // 960 leftover: n=2 prefill never ran (1280-only geom). First 4 s
            // of 620c fair were 6–8 pfps (24 ms uncached memcpy, 2-slot full).
            // Late is already ~24. Decode 36 frames into cached heap first,
            // then BestEffort-present them so CATCH has kicks from t0.
            // Not P1_PREFILL n=2. Not DDR ingest (HURT 12.6/black).
            if (useInproc && !strictDdrPipeline &&
                (budget960Wanted() || isPlex960BankSize(rawW, rawH) ||
                 isPlex960DdrFrameGeometry(ddrGeometry))) {
                constexpr int kAhead = 48; // 64 WASH 23.42 < 48's 23.68
                std::vector<std::vector<uint8_t>> ahead;
                ahead.reserve(static_cast<size_t>(kAhead));
                for (int i = 0; i < kAhead && !stop_.load(); ++i) {
                    std::vector<uint8_t> buf(frameBytes);
                    std::string ierr;
                    const int rc = inprocDec.readI420(buf.data(), frameBytes, ierr);
                    if (rc != 1) {
                        log("media: warmup_ahead abort rc=" + std::to_string(rc) +
                            " " + ierr);
                        break;
                    }
                    ++frameIndex;
                    ahead.push_back(std::move(buf));
                }
                t0 = std::chrono::steady_clock::now();
                hwPresentBaselineValid = fpga_.readBankRelease(hwPresentBaseline);
                hwPresentLastFrames = hwPresentBaseline.frames_done;
                hwPresentArmBaseline = 0;
                hwPresentTotal = 0;
                hwPresentTimeBaseline = t0;
                int sent = 0;
                for (auto& buf : ahead) {
                    if (stop_.load())
                        break;
                    BankReleaseStatus pre{};
                    const bool havePre = fpga_.readBankRelease(pre);
                    const bool ok = fpga_.sendYuv420pFrameDdr(
                        buf.data(), frameBytes, ddrGeometry, localBank,
                        DdrBankWritePolicy::BestEffort, 0);
                    if (!ok) {
                        log("media: warmup_ahead send fail: " + fpga_.lastError());
                        break;
                    }
                    localBank = nextBank(localBank);
                    ++sent;
                    pipelinePresentCount.fetch_add(1);
                    // One kick per blank so CATCH keeps the frame (blast
                    // at memcpy rate overwrote ~half of the 36).
                    const auto wt0 = std::chrono::steady_clock::now();
                    while (havePre && !stop_.load()) {
                        const int64_t waited = pipeUs(
                            wt0, std::chrono::steady_clock::now());
                        BankReleaseStatus cur{};
                        if (fpga_.readBankRelease(cur) &&
                            frameCounterDelta(cur.frames_done, pre.frames_done) > 0)
                            break;
                        if (waited >= 50000)
                            break;
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                }
                log("media: warmup_ahead n=" + std::to_string(ahead.size()) +
                    " sent=" + std::to_string(sent) +
                    " frame_bytes=" + std::to_string(frameBytes));
            }
            // True 1280×720 identity only. Scaled 240/480 must not eat 60
            // source frames or the integer upsample cannot hold 24 fps.
            if (useInproc && !strictDdrPipeline &&
                isPlex720pDdrFrameGeometry(ddrGeometry) &&
                !isPlex960BankSize(rawW, rawH) &&
                !isPlex960DdrFrameGeometry(ddrGeometry) &&
                sourceMediaW_ == kInproc720W && sourceMediaH_ == kInproc720H) {
                constexpr int kAhead720 = 60;
                std::vector<std::vector<uint8_t>> ahead720;
                ahead720.reserve(static_cast<size_t>(kAhead720));
                for (int i = 0; i < kAhead720 && !stop_.load(); ++i) {
                    std::vector<uint8_t> buf(frameBytes);
                    std::string ierr;
                    const int rc = inprocDec.readI420(buf.data(), frameBytes, ierr);
                    if (rc != 1) {
                        log("media: warmup_ahead720 abort rc=" + std::to_string(rc) +
                            " " + ierr);
                        break;
                    }
                    ++frameIndex;
                    ahead720.push_back(std::move(buf));
                }
                t0 = std::chrono::steady_clock::now();
                hwPresentBaselineValid = fpga_.readBankRelease(hwPresentBaseline);
                hwPresentLastFrames = hwPresentBaseline.frames_done;
                hwPresentArmBaseline = 0;
                hwPresentTotal = 0;
                hwPresentTimeBaseline = t0;
                int sent720 = 0;
                for (auto& buf : ahead720) {
                    if (stop_.load())
                        break;
                    holdHdmiAudioLag();
                    BankReleaseStatus pre{};
                    const bool havePre = fpga_.readBankRelease(pre);
                    const bool ok = fpga_.sendYuv420pFrameDdr(
                        buf.data(), frameBytes, ddrGeometry, localBank,
                        DdrBankWritePolicy::BestEffort, 0);
                    if (!ok) {
                        log("media: warmup_ahead720 send fail: " + fpga_.lastError());
                        break;
                    }
                    localBank = nextBank(localBank);
                    ++sent720;
                    pipelinePresentCount.fetch_add(1);
                    const auto wt0 = std::chrono::steady_clock::now();
                    while (havePre && !stop_.load()) {
                        const int64_t waited = pipeUs(
                            wt0, std::chrono::steady_clock::now());
                        BankReleaseStatus cur{};
                        if (fpga_.readBankRelease(cur) &&
                            frameCounterDelta(cur.frames_done, pre.frames_done) > 0)
                            break;
                        if (waited >= 50000)
                            break;
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                }
                log("media: warmup_ahead720 n=" + std::to_string(ahead720.size()) +
                    " sent=" + std::to_string(sent720) +
                    " frame_bytes=" + std::to_string(frameBytes));
            }
#endif
            auto lastPipeLog = t0;
            const int64_t pipelinePauseBaselineUs = playbackPausedUs(t0);
            auto pipelineElapsedUs = [&](std::chrono::steady_clock::time_point now) {
                const int64_t pausedUs = std::max<int64_t>(
                    0, playbackPausedUs(now) - pipelinePauseBaselineUs);
                const int64_t wallUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count();
                return activePlaybackClockUs(wallUs, pausedUs);
            };
            auto servicePipelinePause = [&]() {
                const bool pipelinePaused = paused_.load();
                if (!pipelinePaused)
                    return false;

                const bool overlayNow = overlay_.visible();
                {
                    std::lock_guard<std::mutex> plk(presentMu_);
                    if (lastPresentedFrameValid &&
                        (overlayNow || lastPresentedHadOverlay)) {
                        uint8_t* slotFrame = lastPresentedFrame.data();
                        const bool overlayDrawn = overlay_.renderI420WithBackup(
                            slotFrame, rawW, rawH, i420OverlayBackup);
                        const OverlayRect dirty = i420OverlayBackup.rect;
                        const bool ok = fpga_.sendYuv420pFrameDdr(
                            slotFrame, frameBytes, ddrGeometry, localBank,
                            strictDdrPipeline ? DdrBankWritePolicy::RequireReleased
                                            : DdrBankWritePolicy::BestEffort);
                        if (overlayDrawn)
                            restoreOverlayDirty(slotFrame, dirty);
                        if (ok) {
                            localBank = nextBank(localBank);
                            lastPresentedHadOverlay = overlayDrawn;
                        } else if (strictDdrPipeline) {
                            log("media: DDR_PIPE paused present fail: " +
                                fpga_.lastError());
                            pipelineFatal.store(true);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return true;
            };

            std::thread presentThr([&] {
                // 720p: pin present to CPU0 with audio so ffmpeg -threads 2
                // keeps CPU1 (1.8 ms WC copy on CPU1 was the 23.7 unique hole).
                // 480p stays CPU1 (copy 4.3 ms, unique already ~24).
                {
                    cpu_set_t cpus;
                    CPU_ZERO(&cpus);
                    CPU_SET(isPlex720pDdrFrameGeometry(ddrGeometry) ? 0 : 1, &cpus);
                    (void)::pthread_setaffinity_np(::pthread_self(), sizeof(cpus), &cpus);
                    // Per-thread only. PRIO_PROCESS+pid 0 niced the whole
                    // daemon (decode+present) and slowed 1280 memcpy/decode.
#ifdef __linux__
                    (void)::setpriority(PRIO_PROCESS,
                                        static_cast<int>(::syscall(SYS_gettid)), 5);
#else
                    (void)::setpriority(PRIO_PROCESS, 0, 5);
#endif
                }
                int pipelineFailCount = 0;
                bool overlapReady = false;
                int overlapBank = -1;
                int overlapHits = 0;
                bool paceHaveLast = false;
                BankReleaseStatus paceLast{};
                int64_t lastKickLagUs = 0;
                int64_t lastPaceWaitUs = 0;
                auto prefetchOverlap = [&](int curSlot) {
                    if (overlapReady || wantStickIngest || overlay_.visible())
                        return;
                    if (!isPlex720pDdrFrameGeometry(ddrGeometry) ||
                        isPlex960BankSize(rawW, rawH))
                        return;
                    int nextSlot = -1;
                    {
                        std::lock_guard<std::mutex> lk(ringMu);
                        if (fullCount >= 2)
                            nextSlot = (curSlot + 1) % 2;
                    }
                    if (nextSlot < 0)
                        return;
                    // Never memcpy the displayed bank. overlap_hide used to
                    // write localBank (xor ping-pong) during wait_swap — that
                    // is the scanout bank for ~12 ms of active video and it
                    // tears VGA and HDMI (vga_scaler copies HDMI).
                    BankReleaseStatus brs{};
                    if (!fpga_.readBankRelease(brs) || !brs.anyFree())
                        return;
                    const int destBank = brs.freeBank();
                    if (destBank < 0 || destBank == static_cast<int>(brs.disp_bank))
                        return;
                    uint8_t* dst = fpga_.ddrBankVirt(destBank);
                    if (!dst)
                        return;
                    const uint8_t* nf = ringPtr[static_cast<size_t>(nextSlot)];
                    const auto tc0 = std::chrono::steady_clock::now();
                    std::memcpy(dst, nf, frameBytes);
                    clearYuv420pCropPadding(dst, ddrGeometry);
                    if (uvUBias_ != 0 || uvVBias_ != 0)
                        applyYuv420pUvBias(dst, rawW, rawH, uvUBias_, uvVBias_);
                    __sync_synchronize();
                    if (pipeProfile)
                        pipeCopyUs.fetch_add(
                            pipeUs(tc0, std::chrono::steady_clock::now()));
                    overlapReady = true;
                    overlapBank = destBank;
                    localBank = destBank;
                    static std::atomic<bool> loggedPref{false};
                    if (!loggedPref.exchange(true)) {
                        log("media: overlap_free prefetch bank=" +
                            std::to_string(destBank) +
                            " disp=" + std::to_string(brs.disp_bank) +
                            " free=0x" +
                            std::to_string(brs.free_bank_mask) +
                            " next_slot=" + std::to_string(nextSlot) +
                            " copy_us=" +
                            std::to_string(pipeUs(
                                tc0, std::chrono::steady_clock::now())));
                    }
                };
                auto stageCurrentToFree = [&](uint8_t* src) {
                    if (!src || overlapReady || wantStickIngest)
                        return;
                    if (!isPlex720pDdrFrameGeometry(ddrGeometry) ||
                        isPlex960BankSize(rawW, rawH))
                        return;
                    BankReleaseStatus brs{};
                    if (!fpga_.readBankRelease(brs))
                        return;
                    int dest = -1;
                    if (brs.anyFree())
                        dest = brs.freeBank();
                    else if (brs.disp_bank <= 1)
                        dest = static_cast<int>(brs.disp_bank) ^ 1;
                    if (dest < 0 || dest == static_cast<int>(brs.disp_bank))
                        return;
                    uint8_t* dst = fpga_.ddrBankVirt(dest);
                    if (!dst)
                        return;
                    const auto tc0 = std::chrono::steady_clock::now();
                    std::memcpy(dst, src, frameBytes);
                    clearYuv420pCropPadding(dst, ddrGeometry);
                    if (uvUBias_ != 0 || uvVBias_ != 0)
                        applyYuv420pUvBias(dst, rawW, rawH, uvUBias_, uvVBias_);
                    __sync_synchronize();
                    if (pipeProfile)
                        pipeCopyUs.fetch_add(
                            pipeUs(tc0, std::chrono::steady_clock::now()));
                    overlapReady = true;
                    overlapBank = dest;
                    localBank = dest;
                };
                while (true) {
                    int slot = -1;
                    int64_t slotFrameIndex = 0;
                    {
                        std::unique_lock<std::mutex> lk(ringMu);
                        const auto tw0 = std::chrono::steady_clock::now();
                        ringCv.wait(lk, [&] {
                            return fullCount > 0 || readerEof.load() ||
                                   stop_.load() || pipelineFatal.load();
                        });
                        if (pipeProfile)
                            pipePresentWaitUs.fetch_add(pipeUs(tw0, std::chrono::steady_clock::now()));
                        if (fullCount == 0) {
                            if (readerEof.load() || stop_.load() ||
                                pipelineFatal.load())
                                break;
                            continue;
                        }
                        slot = presentSlot;
                        slotFrameIndex = ringFrameIndex[static_cast<size_t>(slot)];
                    }
                    bool overlayDrawn = false;
                    bool presentFrame = true;
                    if (avLockPresent) {
                        const int64_t frameUs =
                            frameContentUs(slotFrameIndex, fpsNum, fpsDen) +
                            avOffsetMs_.load() * 1000LL;
                        for (;;) {
                            if (stop_.load() || pipelineFatal.load()) {
                                presentFrame = false;
                                break;
                            }
                            if (paused_.load()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                continue;
                            }
                            const int64_t audioB = audioBytes_.load();
                            const int64_t clockUs =
                                (wantAudio && audioActive_.load() && audioB > 0)
                                    ? audibleClockUs(audioB,
                                                     audioQueuedBytes_.load())
                                    : pipelineElapsedUs(std::chrono::steady_clock::now());
                            const int64_t driftUs = clockUs - frameUs;
                            avDriftMs_.store(driftUs / 1000);
                            const int leadHoldMs = misterplex::p720_av::holdLeadMsWithQueued(
                                static_cast<int>(leadMs), audioQueuedBytes_.load());
                            const AvAction action =
                                avDecide(driftUs, static_cast<int64_t>(leadHoldMs) * 1000LL,
                                         dropMs * 1000LL, dropRun);
                            if (action == AvAction::Hold) {
                                // Combined 720p: kick-on-swap is the 24.10 Hz
                                // pace (copy 1.8 ms, kick_lag ~20 us). Hold
                                // slaved unique to heard clock (pfps 23.5,
                                // hw_fps 22.8). 480p still Holds.
                                if (misterplex::p720_av::combined720pSkipAvHold(
                                        isPlex720pDdrFrameGeometry(ddrGeometry),
                                        useInproc))
                                    break;
                                // 720p dual-pipe / true480 gold: skip Hold when the
                                // ring has no spare so combined pipe:1+pipe:3
                                // cannot deadlock (video ahead → Hold →
                                // ffmpeg stalls audio too). Host-paint 720x480
                                // must KEEP Hold — spare-skip made pfps race
                                // ~40 with pace_wait_us=0 and broke lipsync.
                                const bool dualPipe720 =
                                    wantAudio && splitAv720 &&
                                    isPlex720pDdrFrameGeometry(ddrGeometry);
                                const bool spareHoldSkipOk =
                                    dualPipe720 ||
                                    (true480Pipeline && !hostPaint720x480);
                                if (!spareHoldSkipOk) {
                                    // fall through to Hold sleep
                                } else {
                                    int spare = 0;
                                    {
                                        std::lock_guard<std::mutex> lk(ringMu);
                                        spare = fullCount;
                                    }
                                    if (spare < 2)
                                        break;
                                }
                                // Combined A+V: ffmpeg stalls when the video
                                // pipe fills. Refresh MrAudio queued bytes so
                                // the heard clock still advances and Hold
                                // can finish (otherwise soak G hung at 12s).
                                {
                                    const MrAudioStatus st = readMrAudioStatus();
                                    if (st.valid())
                                        audioQueuedBytes_.store(st.queuedBytes);
                                }
                                const int64_t remainUs = -(driftUs + leadMs * 1000LL);
                                const int64_t sleepUs =
                                    std::max<int64_t>(100, std::min<int64_t>(remainUs, 2000));
                                std::this_thread::sleep_for(
                                    std::chrono::microseconds(sleepUs));
                                continue;
                            }
                            if (action == AvAction::Drop) {
                                presentFrame = false;
                                ++dropRun;
                                droppedFrames_.fetch_add(1);
                                if ((droppedFrames_.load() % 24) == 1) {
                                    log("media: DDR_PIPE A/V resync drop frame=" +
                                        std::to_string(slotFrameIndex) +
                                        " drift_ms=" +
                                        std::to_string(avDriftMs_.load()) +
                                        " drops=" +
                                        std::to_string(droppedFrames_.load()));
                                }
                            } else {
                                dropRun = 0;
                            }
                            break;
                        }
                    }
                    if (presentFrame) {
                        holdHdmiAudioLag();
                        bool fatalPresent = false;
                        std::lock_guard<std::mutex> plk(presentMu_);
                        uint8_t* slotFrame = ringPtr[static_cast<size_t>(slot)];
                        clearYuv420pCropPadding(slotFrame, ddrGeometry);
                        if (uvUBias_ != 0 || uvVBias_ != 0) {
                            applyYuv420pUvBias(slotFrame, rawW, rawH, uvUBias_, uvVBias_);
                        }
                        overlayDrawn = overlay_.renderI420WithBackup(
                            slotFrame, rawW, rawH, i420OverlayBackup);
                        const OverlayRect dirty = i420OverlayBackup.rect;
                        const uint32_t slotPhys =
                            wantFabric ? ringPhys[static_cast<size_t>(slot)] : 0u;
                        const int ingestBank = ringIngestBank[static_cast<size_t>(slot)];
                        bool ok = false;
                        const bool doBeamPace =
                            !strictDdrPipeline && pipelineDdr &&
                            beamPaceWanted(outW_, outH_) &&
                            isPlex720pDdrFrameGeometry(ddrGeometry);
                        // Kick-on-swap: wait the PREVIOUS doorbell's blank,
                        // then copy+kick. Wait-THIS-swap put avDecide/overlay
                        // after the blank (~2 ms → unique 22.8 vs 24.1).
                        bool paceSawThis = false;
                        std::chrono::steady_clock::time_point paceSawAt{};
                        if (doBeamPace && paceHaveLast) {
                            constexpr int64_t kSwapWaitUs = 50000;
                            bool sawSwap = false;
                            const auto paceT0 = std::chrono::steady_clock::now();
                            while (!stop_.load() && !pipelineFatal.load()) {
                                const auto noww = std::chrono::steady_clock::now();
                                const int64_t waited = pipeUs(paceT0, noww);
                                BankReleaseStatus cur{};
                                if (fpga_.readBankRelease(cur) &&
                                    frameCounterDelta(cur.frames_done,
                                                      paceLast.frames_done) > 0) {
                                    sawSwap = true;
                                    paceSawThis = true;
                                    paceSawAt = noww;
                                }
                                if (sawSwap || waited >= kSwapWaitUs)
                                    break;
                                if (!overlapReady)
                                    stageCurrentToFree(slotFrame);
                                std::this_thread::sleep_for(
                                    std::chrono::microseconds(overlapReady ? 10 : 50));
                            }
                            if (!overlapReady)
                                stageCurrentToFree(slotFrame);
                            lastPaceWaitUs = pipeUs(paceT0, std::chrono::steady_clock::now());
                            static std::atomic<bool> loggedPace{false};
                            if (!loggedPace.exchange(true)) {
                                log("media: beam_pace=kick_on_swap timeout_us=" +
                                    std::to_string(kSwapWaitUs) +
                                    " first_wait_us=" +
                                    std::to_string(lastPaceWaitUs) +
                                    " saw_swap=" + (sawSwap ? "1" : "0") +
                                    " overlap=" + (overlapReady ? "1" : "0"));
                            }
                        } else if (doBeamPace && !overlapReady) {
                            stageCurrentToFree(slotFrame);
                        }
                        BankReleaseStatus livePre{};
                        const bool haveLivePre =
                            !avLockPresent && !doBeamPace && !wantStickIngest &&
                            isPlex720pDdrFrameGeometry(ddrGeometry) &&
                            !isPlex960BankSize(rawW, rawH) &&
                            fpga_.readBankRelease(livePre);
                        bool usedOverlap = false;
                        BankReleaseStatus pacePre{};
                        const bool havePacePre =
                            doBeamPace && fpga_.readBankRelease(pacePre);
                        if (wantStickIngest && (ingestBank == 0 || ingestBank == 1)) {
                            ok = fpga_.commitDdrBankIngest(ingestBank, frameBytes);
                            if (ok)
                                localBank = ingestBank ^ 1;
                            overlapReady = false;
                            overlapBank = -1;
                        } else if (overlapReady && overlapBank == localBank) {
                            ok = fpga_.commitDdrBankIngest(localBank, frameBytes);
                            usedOverlap = ok;
                            overlapReady = false;
                            overlapBank = -1;
                            if (ok) {
                                localBank = nextBank(localBank);
                                ++overlapHits;
                                static std::atomic<bool> loggedCommit{false};
                                if (!loggedCommit.exchange(true)) {
                                    log("media: overlap_hide commit bank=" +
                                        std::to_string(localBank ^ 1));
                                }
                            }
                        } else {
                            overlapReady = false;
                            overlapBank = -1;
                            ok = fpga_.sendYuv420pFrameDdr(
                                slotFrame, frameBytes, ddrGeometry, localBank,
                                strictDdrPipeline ? DdrBankWritePolicy::RequireReleased
                                                : DdrBankWritePolicy::BestEffort,
                                slotPhys);
                            if (ok)
                                localBank = nextBank(localBank);
                        }
                        if (ok && doBeamPace && havePacePre) {
                            paceLast = pacePre;
                            paceHaveLast = true;
                        } else if (ok && doBeamPace) {
                            paceHaveLast = fpga_.readBankRelease(paceLast);
                        }
                        if (ok && doBeamPace && paceSawThis)
                            lastKickLagUs =
                                pipeUs(paceSawAt, std::chrono::steady_clock::now());
                        else if (ok && doBeamPace)
                            lastKickLagUs = 0;
                        // 1280 live: wait this kick to swap so CATCH keeps it
                        // (BestEffort blast overwrote blanks; 960 ahead pace worked).
                        // Hide the next 1.38 MiB memcpy inside that wait so the
                        // following kick is not 14 ms late of the blank (23.66).
                        if (ok && haveLivePre) {
                            const auto wt0 = std::chrono::steady_clock::now();
                            prefetchOverlap(slot);
                            while (!stop_.load() && !pipelineFatal.load()) {
                                const int64_t waited = pipeUs(
                                    wt0, std::chrono::steady_clock::now());
                                BankReleaseStatus cur{};
                                if (fpga_.readBankRelease(cur) &&
                                    frameCounterDelta(cur.frames_done,
                                                      livePre.frames_done) > 0)
                                    break;
                                if (waited >= 50000)
                                    break;
                                if (!overlapReady)
                                    prefetchOverlap(slot);
                                std::this_thread::sleep_for(
                                    std::chrono::microseconds(overlapReady ? 50 : 200));
                            }
                            if (!overlapReady)
                                prefetchOverlap(slot);
                        }
                        if (pipeProfile && ok) {
                            const auto dt = fpga_.lastDdrTiming();
                            pipeSendUs.fetch_add(dt.total_us);
                            pipeCopyUs.fetch_add(dt.copy_us);
                        }
                        if (overlayDrawn)
                            restoreOverlayDirty(slotFrame, dirty);
                        if (ok) {
                            // Pause-OSD snapshot only. Movie frames do not need a
                            // third 777600/1.38 MiB copy on the same DDR3 as decode.
                            // P5: MPX_SKIP_LAST_PRESENTED (default on 960) skips it.
                            if (!skipLastPresented &&
                                (overlayDrawn || overlay_.visible())) {
                                std::memcpy(lastPresentedFrame.data(), slotFrame,
                                            frameBytes);
                                lastPresentedFrameValid = true;
                            } else {
                                lastPresentedFrameValid = false;
                            }
                            lastPresentedHadOverlay = overlayDrawn;
                            pipelineFailCount = 0;
                            const int64_t presented = pipelinePresentCount.fetch_add(1) + 1;
                            presentCount_ = presented;
                            {
                                const int need = audioReleaseAfterPresents_.load();
                                if (need > 0 && presented >= need &&
                                    !audioFeedRelease_.load()) {
                                    audioFeedRelease_.store(true);
                                    log("media: 720p audio_release presents=" +
                                        std::to_string(presented));
                                }
                            }
                            if ((presented % 48) == 0) {
                                const auto dt = fpga_.lastDdrTiming();
                                log(std::string("media: fpga frame_tx ok via DDR_PIPE") +
                                    " presents=" + std::to_string(presented) +
                                    " frames=" + std::to_string(slotFrameIndex) +
                                    " ms=" +
                                    std::to_string(static_cast<int>(fpga_.lastPushMs())) +
                                    " prep_us=" + std::to_string(dt.prep_wait_us) +
                                    " copy_us=" + std::to_string(dt.copy_us) +
                                    " plxd_us=" + std::to_string(dt.plxa_poll_us) +
                                    " total_us=" + std::to_string(dt.total_us) +
                                    " fabric=" +
                                    std::string(usedOverlap
                                                    ? "overlap"
                                                    : fpga_.lastDdrPublishHow()) +
                                    " overlap_hits=" +
                                    std::to_string(overlapHits) +
                                    " pace_wait_us=" +
                                    std::to_string(lastPaceWaitUs) +
                                    " kick_lag_us=" +
                                    std::to_string(lastKickLagUs));
                            }
                        } else {
                            ++pipelineFailCount;
                            if (pipelineFailCount == 1 || (pipelineFailCount % 60) == 0) {
                                log("media: DDR_PIPE present fail frame=" +
                                    std::to_string(slotFrameIndex) + ": " +
                                    fpga_.lastError());
                            }
                            fatalPresent = strictDdrPipeline;
                        }
                        if (fatalPresent) {
                            pipelineFatal.store(true);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(ringMu);
                        releaseIngestBank(slot);
                        presentSlot = (presentSlot + 1) % 2;
                        --fullCount;
                    }
                    ringCv.notify_all();
                    if (pipelineFatal.load())
                        break;
                }
            });

            while (!stop_.load() && !pipelineFatal.load()) {
                int64_t seekTo = seekReqMs_.exchange(-1);
                if (seekTo >= 0) {
                    log("media: seek requested " + std::to_string(seekTo));
#ifdef MPX_HAVE_LIBAV
                    if (useInproc)
                        inprocDec.requestStop();
#endif
                    break;
                }
                if (servicePipelinePause())
                    continue;
                {
                    std::unique_lock<std::mutex> lk(ringMu);
                    const auto tw0 = std::chrono::steady_clock::now();
                    ringCv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                        return fullCount < 2 || paused_.load() ||
                               stop_.load() || pipelineFatal.load();
                    });
                    if (pipeProfile)
                        pipeSlotWaitUs.fetch_add(pipeUs(tw0, std::chrono::steady_clock::now()));
                    if (stop_.load() || pipelineFatal.load())
                        break;
                    if (paused_.load() || fullCount >= 2)
                        continue;
                }
                if (wantStickIngest) {
                    const int ibank = takeIngestBank();
                    if (ibank < 0) {
                        // Both banks held — wait for present to release. Do not
                        // memcpy-fallback (second copy + 150 ms spin was the
                        // combined-pipe audio starve).
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        continue;
                    } else {
                        uint8_t* bp = fpga_.ddrBankVirt(ibank);
                        if (!bp) {
                            {
                                std::lock_guard<std::mutex> lk(ringMu);
                                bankHeld[static_cast<size_t>(ibank)] = false;
                            }
                            log("media: stick ingest: ddrBankVirt failed: " +
                                fpga_.lastError());
                            pipelineFatal.store(true);
                            break;
                        }
                        ringPtr[static_cast<size_t>(readSlot)] = bp;
                        ringIngestBank[static_cast<size_t>(readSlot)] = ibank;
                    }
                }
                size_t got = 0;
                uint8_t* dst = ringPtr[static_cast<size_t>(readSlot)];
                auto lastWaitProgress = std::chrono::steady_clock::now();
                const auto tr0 = lastWaitProgress;
#ifdef MPX_HAVE_LIBAV
                if (useInproc) {
                    std::string ierr;
                    const int rc = inprocDec.readI420(dst, frameBytes, ierr);
                    if (pipeProfile)
                        pipeReadUs.fetch_add(
                            pipeUs(tr0, std::chrono::steady_clock::now()));
                    if (rc == 1) {
                        got = frameBytes;
                        totalBytes += frameBytes;
                    } else if (rc == 0) {
                        videoEof = true;
                    } else {
                        log("media: inproc_decode read failed: " + ierr);
                        {
                            std::lock_guard<std::mutex> lk(ringMu);
                            releaseIngestBank(readSlot);
                        }
                        break;
                    }
                } else
#endif
                {
                while (got < frameBytes && !stop_.load() && !pipelineFatal.load()) {
                    if (servicePipelinePause())
                        continue;
                    fd_set rfds;
                    FD_ZERO(&rfds);
                    FD_SET(rfd, &rfds);
                    timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = 200000;
                    const int pr = ::select(rfd + 1, &rfds, nullptr, nullptr, &tv);
                    if (pr < 0) {
                        if (errno == EINTR)
                            continue;
                        log("media: pipeline select err errno=" + std::to_string(errno));
                        break;
                    }
                    if (servicePipelinePause())
                        continue;
                    const auto nowWait = std::chrono::steady_clock::now();
                    if (nowWait - lastWaitProgress >= std::chrono::seconds(1)) {
                        lastWaitProgress = nowWait;
                        const int64_t wallWait = pipelineElapsedUs(nowWait) / 1000;
                        const int64_t tms = startMs + std::max<int64_t>(0, wallWait);
                        positionMs_.store(tms);
                        lastPosMs_.store(tms);
                        if (!paused_.load())
                            companionTimelineTick();
                    }
                    if (pr == 0)
                        continue;
                    const ssize_t n =
                        ::read(rfd, dst + got, frameBytes - got);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        log("media: pipeline read err errno=" + std::to_string(errno));
                        break;
                    }
                    if (n == 0) {
                        videoEof = true;
                        break;
                    }
                    got += static_cast<size_t>(n);
                    totalBytes += static_cast<size_t>(n);
                }
                if (pipeProfile)
                    pipeReadUs.fetch_add(pipeUs(tr0, std::chrono::steady_clock::now()));
                }
                if (pipelineFatal.load()) {
                    std::lock_guard<std::mutex> lk(ringMu);
                    releaseIngestBank(readSlot);
                    break;
                }
                if (got < frameBytes) {
                    shortRead = true;
                    shortReadGot = got;
                    shortReadWant = frameBytes;
                    log("media: pipeline short read got=" + std::to_string(got) + "/" +
                        std::to_string(frameBytes));
                    {
                        std::lock_guard<std::mutex> lk(ringMu);
                        releaseIngestBank(readSlot);
                    }
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        last_good_offset_ = totalBytes;
                        ++range_retry_;
                    }
                    break;
                }
                // Legacy pipeline only. true480 and 720p strict present are paced
                // in the present thread from the audible/wall clock.
                if (!strictDdrPipeline && (!wantAudio || !audioActive_.load())) {
                    const int64_t frameMs =
                        frameContentMs(frameIndex + 1, fpsNum, fpsDen) + avOffsetMs_.load();
                    const int64_t clockMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count();
                    if (frameMs + leadMs < clockMs) {
                        // behind — present ASAP
                    } else if (frameMs > clockMs + 2) {
                        const int64_t sleepMs = std::min<int64_t>(frameMs - clockMs, 5);
                        if (sleepMs > 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                    }
                    avDriftMs_.store(clockMs - frameMs);
                }
                ++frameIndex;
                {
                    std::lock_guard<std::mutex> lk(ringMu);
                    ringFrameIndex[static_cast<size_t>(readSlot)] = frameIndex;
                    readSlot = (readSlot + 1) % 2;
                    ++fullCount;
                }
                ringCv.notify_all();

                const auto now = std::chrono::steady_clock::now();
                if (now - lastPipeLog > std::chrono::seconds(1)) {
                    lastPipeLog = now;
                    const int64_t wall2 = pipelineElapsedUs(now) / 1000;
                    const double vfps =
                        wall2 > 0 ? (1000.0 * static_cast<double>(frameIndex) /
                                     static_cast<double>(wall2))
                                  : 0.0;
                    const double pfps =
                        wall2 > 0 ? (1000.0 * static_cast<double>(
                                                   pipelinePresentCount.load()) /
                                     static_cast<double>(wall2))
                                  : 0.0;
                    const int64_t abytes = audioBytes_.load();
                    const double a_sec = static_cast<double>(abytes) / (48000.0 * 4.0);
                    log("media: frames=" + std::to_string(frameIndex) +
                        " vfps=" + std::to_string(vfps).substr(0, 4) +
                        " pfps=" + std::to_string(pfps).substr(0, 4) +
                        " audio_s=" + std::to_string(a_sec).substr(0, 5) +
                        " wall_s=" + std::to_string(wall2 / 1000.0).substr(0, 5) +
                        " audio=" + (audioActive_.load() ? "on" : "off") +
                        " clock=av-lock" +
                        " av_drift_ms=" + std::to_string(avDriftMs_.load()) +
                        " drops=" + std::to_string(droppedFrames_.load()) +
                        hardwarePresentTelemetry() +
                        " fps=" + std::to_string(fpsNum) + "/" + std::to_string(fpsDen) +
                        " decode=" + std::to_string(outW_) + "x" + std::to_string(outH_) +
                        (useInproc ? " pipe=inproc" : " pipe=1"));
                    if (pipeProfile && frameIndex > 0) {
                        const int64_t n = frameIndex;
                        const int64_t p = std::max<int64_t>(1, pipelinePresentCount.load());
                        log("media: pipe_profile n=" + std::to_string(n) +
                            " p=" + std::to_string(p) +
                            " read_us_f=" + std::to_string(pipeReadUs.load() / n) +
                            " slot_wait_us_f=" + std::to_string(pipeSlotWaitUs.load() / n) +
                            " present_wait_us_p=" +
                            std::to_string(pipePresentWaitUs.load() / p) +
                            " send_us_p=" + std::to_string(pipeSendUs.load() / p) +
                            " copy_us_p=" + std::to_string(pipeCopyUs.load() / p));
                    }
                    // DDR 2-slot ring never hits the non-pipe progress path (frameIndex%15).
                    // Without this, Companion/Plex Web scrubber freezes at plant offset.
                    const int64_t tms = startMs + wall2;
                    positionMs_.store(tms);
                    lastPosMs_.store(tms);
                    companionTimelineTick();
                }
            }
            readerEof.store(true);
            ringCv.notify_all();
            if (presentThr.joinable())
                presentThr.join();
            if (pipeProfile && frameIndex > 0) {
                const int64_t n = frameIndex;
                const int64_t p = std::max<int64_t>(1, pipelinePresentCount.load());
                log("media: pipe_profile_end n=" + std::to_string(n) +
                    " p=" + std::to_string(p) +
                    " read_us_f=" + std::to_string(pipeReadUs.load() / n) +
                    " slot_wait_us_f=" + std::to_string(pipeSlotWaitUs.load() / n) +
                    " present_wait_us_p=" +
                    std::to_string(pipePresentWaitUs.load() / p) +
                    " send_us_p=" + std::to_string(pipeSendUs.load() / p) +
                    " copy_us_p=" + std::to_string(pipeCopyUs.load() / p));
            }
            releaseFabricSlots();
            true480PipelineAborted = pipelineFatal.load();
            if (true480PipelineAborted)
                log("ERROR media: true480 DDR pipeline aborted; suppressing natural EOF");
            presentCount_ = pipelinePresentCount.load();
            ddrBank_ = localBank;
            if (rfd >= 0) {
                ::close(rfd);
                rfd = -1;
            }
        }

        bool pauseClockHeld = false;
        bool audioBindLogged480i = false;
        bool pausedOverlayWasVisible = false;
        std::chrono::steady_clock::time_point pauseStarted{};
        size_t got = 0;
        while (!stop_.load() && !pipelineDdr) {
            int64_t seekTo = seekReqMs_.exchange(-1);
            if (seekTo >= 0) {
                log("media: seek requested " + std::to_string(seekTo));
                break;
            }

            if (paused_.load()) {
                if (!pauseClockHeld) {
                    pauseClockHeld = true;
                    pauseStarted = std::chrono::steady_clock::now();
                }
                const bool overlayNow = overlay_.visible();
                if ((overlayNow || pausedOverlayWasVisible) && frameIndex > 0) {
                    presentCleanFrame(frame.data(), /*countPresent*/ false);
                    pausedOverlayWasVisible = overlayNow;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            } else if (pauseClockHeld) {
                t0 += std::chrono::steady_clock::now() - pauseStarted;
                pauseClockHeld = false;
            }

            int64_t frameReadCalls = 0;
            int64_t frameReadOkCalls = 0;
            int64_t frameReadEagain = 0;
            int64_t frameReadEintr = 0;
            int64_t frameReadZero = 0;
            int64_t frameReadBytes = 0;
            int64_t frameReadMaxBytes = 0;
            int64_t frameReadSyscallUs = 0;
            int64_t frameReadSleepUs = 0;
            std::chrono::steady_clock::time_point readStart;
            std::chrono::steady_clock::time_point readEnd;
            int64_t readCpuStart = 0;
            int64_t readCpuEnd = 0;
            // Zero-intermediate present: read rawvideo straight into the free DDR
            // bank (retires heap frame + second uncached memcpy). FPGA-only YUV path.
            // PRESENT=fpga only: no fb0 blit needed, so payload can land in-bank.
            const bool directDdrIngest =
                useDdrF1_ && wantFpgaFrameStore &&
                videoFmt == RawVideoFormat::Yuv420p && presentMode_ == "fpga";
            int ingestBank = ddrBank_;
            uint8_t* readDst = frame.data();
            bool usingDirectIngest = false;
            // Hold presentMu_ for begin→read→commit so OSD/idle cannot race bank.
            std::unique_lock<std::mutex> presentLk(presentMu_, std::defer_lock);
            if (directDdrIngest) {
                presentLk.lock();
                uint8_t* bankPtr =
                    fpga_.beginDdrBankIngest(frameBytes, ddrBank_, ingestBank);
                if (bankPtr) {
                    readDst = bankPtr;
                    usingDirectIngest = true;
                } else {
                    presentLk.unlock();
                }
            }
            if (profilePresent)
                readStart = std::chrono::steady_clock::now();
            if (profilePresent)
                readCpuStart = threadCpuMicros();
            while (got < frameBytes && !stop_.load() && !paused_.load()) {
                ++frameReadCalls;
                ssize_t n = 0;
                if (profilePresent) {
                    const auto syscall0 = std::chrono::steady_clock::now();
                    n = ::read(rfd, readDst + got, frameBytes - got);
                    const auto syscall1 = std::chrono::steady_clock::now();
                    frameReadSyscallUs += microsBetween(syscall0, syscall1);
                } else {
                    n = ::read(rfd, readDst + got, frameBytes - got);
                }
                if (n < 0) {
                    if (errno == EINTR) {
                        ++frameReadEintr;
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        ++frameReadEagain;
                        // Prefer 1ms over 2ms (budget) or 200us spin (starves ffmpeg).
                        if (profilePresent) {
                            const auto sleep0 = std::chrono::steady_clock::now();
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            const auto sleep1 = std::chrono::steady_clock::now();
                            frameReadSleepUs += microsBetween(sleep0, sleep1);
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        continue;
                    }
                    log("media: read err errno=" + std::to_string(errno));
                    break;
                }
                if (n == 0) {
                    ++frameReadZero;
                    videoEof = true;
                    break;
                }
                got += static_cast<size_t>(n);
                ++frameReadOkCalls;
                frameReadBytes += n;
                if (n > frameReadMaxBytes)
                    frameReadMaxBytes = n;
                totalBytes += static_cast<size_t>(n);
            }
            if (profilePresent) {
                readCpuEnd = threadCpuMicros();
                readEnd = std::chrono::steady_clock::now();
            }
            if (paused_.load()) {
                // Bank was reserved but frame incomplete — drop reservation by not committing.
                usingDirectIngest = false;
                continue;
            }
            if (got < frameBytes) {
                shortRead = true;
                shortReadGot = got;
                shortReadWant = frameBytes;
                log("media: short read got=" + std::to_string(got) + "/" +
                    std::to_string(frameBytes) + " totalBytes=" + std::to_string(totalBytes) +
                    (videoEof ? " eof=1" : ""));
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    last_good_offset_ = totalBytes;
                    ++range_retry_;
                }
                break;
            }
            got = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                range_retry_ = 0;
            }

            if (videoFmt == RawVideoFormat::Yuv420p) {
                if (profilePresent) {
                    const auto pix0 = std::chrono::steady_clock::now();
                    const int64_t pixCpu0 = threadCpuMicros();
                    clearYuv420pCropPadding(readDst, ddrGeometry);
                    if (uvUBias_ != 0 || uvVBias_ != 0)
                        applyYuv420pUvBias(readDst, rawW, rawH, uvUBias_, uvVBias_);
                    const int64_t pixCpu1 = threadCpuMicros();
                    const auto pix1 = std::chrono::steady_clock::now();
                    prof.pixelUs += microsBetween(pix0, pix1);
                    prof.pixelCpuUs += pixCpu1 - pixCpu0;
                } else {
                    clearYuv420pCropPadding(readDst, ddrGeometry);
                    if (uvUBias_ != 0 || uvVBias_ != 0)
                        applyYuv420pUvBias(readDst, rawW, rawH, uvUBias_, uvVBias_);
                }
            }
            // Release present lock during A/V pacing; re-acquire only to commit.
            if (usingDirectIngest && presentLk.owns_lock())
                presentLk.unlock();

            ++frameIndex;
            if (profilePresent) {
                ++prof.frames;
                prof.readCalls += frameReadCalls;
                prof.readOkCalls += frameReadOkCalls;
                prof.readEagain += frameReadEagain;
                prof.readEintr += frameReadEintr;
                prof.readZero += frameReadZero;
                prof.readBytes += frameReadBytes;
                if (frameReadMaxBytes > prof.readMaxBytes)
                    prof.readMaxBytes = frameReadMaxBytes;
                prof.readWallUs += microsBetween(readStart, readEnd);
                prof.readCpuUs += readCpuEnd - readCpuStart;
                prof.readSyscallUs += frameReadSyscallUs;
                prof.readSleepUs += frameReadSleepUs;
            }

            // A/V lock: wait until the master clock reaches this frame's content time,
            // or drop the frame when we are too far behind to catch up by waiting.
            // Content time comes from the EXACT rational rate — a bucketed integer fps
            // (23.976 → 24) leaks ~1 ms/s, invisible in a 12 s clip but ~234 ms by 3:54.
            bool present = true;
            int64_t framePacingWaitUs = 0;
            int64_t framePacingWaitCpuUs = 0;
            {
                // Live OSD trim is read every frame so the menu takes effect at once.
                const int bindNum = is480iFrameRate ? (fpsNum * 2) : fpsNum;
                const int bindDen = fpsDen;
                // Even field of this frame: 2*F-1 at 2*N is one field earlier than F at N.
                // Do not use fieldIndex=frameIndex*2 with bindNum=fpsNum*2 (that is a no-op).
                const int64_t fieldIndex =
                    is480iFrameRate ? (frameIndex * 2 - 1) : frameIndex;
                const char* fieldParity = (fieldIndex & 1) ? "odd" : "even";
                const int64_t frameMs =
                    frameContentMs(fieldIndex, bindNum, bindDen) + avOffsetMs_.load();
                if (is480iPtsBind && !audioBindLogged480i) {
                    audioBindLogged480i = true;
                    log(std::string("media: 480i audio PTS bind field_clock=") +
                        std::to_string(bindNum) + "/" + std::to_string(bindDen) +
                        " field=" + fieldParity +
                        " fieldIndex=" + std::to_string(fieldIndex) +
                        " frameIndex=" + std::to_string(frameIndex) +
                        " avOffsetMs=" + std::to_string(avOffsetMs_.load()));
                }
                for (;;) {
                    if (stop_.load() || paused_.load())
                        break;
                    int64_t clockMs = 0;
                    if (wantAudio && audioActive_.load()) {
                        // What has actually been HEARD, not what has been handed
                        // to the driver. Falls back to the submitted-byte clock
                        // when the ring depth is unavailable.
                        clockMs = misterplex::audibleClockMs(audioBytes_.load(),
                                                             audioQueuedBytes_.load());
                    } else {
                        clockMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                    }
                    const int64_t drift = misterplex::avDriftMs(clockMs, frameMs);
                    avDriftMs_.store(drift);
                    videoPts_ = frameMs;
                    audioPts_ = clockMs;
                    pts_drift_ms_ = frameMs - clockMs;
                    {
                        const int64_t absDrift =
                            pts_drift_ms_ < 0 ? -pts_drift_ms_ : pts_drift_ms_;
                        if (absDrift <= drift_threshold)
                            ptsDriftGuardFired = false;
                        const PtsDriftGuardAct guardAct = ptsDriftGuardAct(
                            videoPts_, audioPts_, drift_threshold, ptsDriftGuardFired);
                        if (guardAct != PtsDriftGuardAct::None) {
                            ptsDriftGuardFired = true;
                            log("media: AV_PTS_DRIFT guard fire video_pts_ms=" +
                                std::to_string(videoPts_) +
                                " audio_pts_ms=" + std::to_string(audioPts_) +
                                " delta_ms=" + std::to_string(pts_drift_ms_));
                            if (guardAct == PtsDriftGuardAct::Hold) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                continue;
                            }
                            present = false;
                            break;
                        }
                    }
                    const AvAction act = avDecide(drift, leadMs, dropMs, dropRun);
                    if (act == AvAction::Hold) {
                        if (profilePresent) {
                            const auto hold0 = std::chrono::steady_clock::now();
                            const int64_t holdCpu0 = threadCpuMicros();
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            const int64_t holdCpu1 = threadCpuMicros();
                            const auto hold1 = std::chrono::steady_clock::now();
                            framePacingWaitUs += microsBetween(hold0, hold1);
                            framePacingWaitCpuUs += holdCpu1 - holdCpu0;
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        continue;
                    }
                    present = (act != AvAction::Drop);
                    break;
                }
            }
            if (profilePresent) {
                prof.pacingWaitUs += framePacingWaitUs;
                prof.pacingWaitCpuUs += framePacingWaitCpuUs;
            }

            if (!present) {
                ++dropRun;
                droppedFrames_.fetch_add(1);
                if (profilePresent)
                    ++prof.drops;
                if ((droppedFrames_.load() % 24) == 1)
                    log("media: A/V resync drop drift_ms=" + std::to_string(avDriftMs_.load()) +
                        " drops=" + std::to_string(droppedFrames_.load()));
                // Direct ingest: payload is in a free bank but we drop — do not kick.
                if (presentLk.owns_lock())
                    presentLk.unlock();
            } else {
                dropRun = 0;
                if (usingDirectIngest) {
                    if (!presentLk.owns_lock())
                        presentLk.lock();
                    const int64_t ddrCpu0 = profilePresent ? threadCpuMicros() : 0;
                    const bool ok = fpga_.commitDdrBankIngest(ingestBank, frameBytes);
                    if (profilePresent && ok) {
                        const int64_t ddrCpu1 = threadCpuMicros();
                        const auto dt = fpga_.lastDdrTiming();
                        const int64_t accounted = dt.prep_wait_us + dt.copy_us + dt.flush_us +
                                                  dt.doorbell_us + dt.post_wait_us;
                        prof.ddrPrepWaitUs += dt.prep_wait_us;
                        prof.ddrCopyUs += dt.copy_us;
                        prof.ddrFlushUs += dt.flush_us;
                        prof.ddrDoorbellUs += dt.doorbell_us;
                        prof.ddrPostWaitUs += dt.post_wait_us;
                        prof.ddrBankReuseWaitUs += dt.bank_reuse_wait_us;
                        prof.ddrTotalUs += dt.total_us;
                        prof.ddrCpuUs += ddrCpu1 - ddrCpu0;
                        if (dt.total_us > accounted)
                            prof.ddrUnaccountedUs += dt.total_us - accounted;
                        ++prof.presented;
                    }
                    if (ok) {
                        ++presentCount_;
                        ddrBank_ = ingestBank ^ 1;
                        if ((presentCount_ % 48) == 0) {
                            const auto dt = fpga_.lastDdrTiming();
                            log(std::string("media: fpga frame_tx ok via DDR_INGEST") +
                                " presents=" + std::to_string(presentCount_) +
                                " frames=" + std::to_string(frameIndex) +
                                " ms=" + std::to_string(static_cast<int>(fpga_.lastPushMs())) +
                                " prep_us=" + std::to_string(dt.prep_wait_us) +
                                " copy_us=" + std::to_string(dt.copy_us) +
                                " flush_us=" + std::to_string(dt.flush_us) +
                                " doorbell_us=" + std::to_string(dt.doorbell_us) +
                                " plxd_us=" + std::to_string(dt.plxa_poll_us) +
                                " plxd_iters=" + std::to_string(dt.plxa_poll_iters) +
                                " total_us=" + std::to_string(dt.total_us));
                        }
                    } else {
                        log("media: DDR_INGEST commit failed: " + fpga_.lastError());
                    }
                    if (presentLk.owns_lock())
                        presentLk.unlock();
                } else {
                    if (presentLk.owns_lock())
                        presentLk.unlock();
                    presentCleanFrame(frame.data(), /*countPresent*/ true);
                }
            }

            auto now = std::chrono::steady_clock::now();
            const int64_t wall2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      now - t0)
                                      .count();
            if (now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                const double vfps =
                    wall2 > 0 ? (1000.0 * static_cast<double>(frameIndex) /
                                 static_cast<double>(wall2))
                              : 0.0;
                const double pfps =
                    wall2 > 0 ? (1000.0 * static_cast<double>(presentCount_) /
                                 static_cast<double>(wall2))
                              : 0.0;
                const int64_t abytes = audioBytes_.load();
                const double a_sec = static_cast<double>(abytes) / (48000.0 * 4.0);
                log("media: frames=" + std::to_string(frameIndex) +
                    " vfps=" + std::to_string(vfps).substr(0, 4) +
                    " pfps=" + std::to_string(pfps).substr(0, 4) +
                    " audio_s=" + std::to_string(a_sec).substr(0, 5) +
                    " wall_s=" + std::to_string(wall2 / 1000.0).substr(0, 5) +
                    " audio=" + (audioActive_.load() ? "on" : "off") +
                    " clock=av-lock" +
                    " av_drift_ms=" + std::to_string(avDriftMs_.load()) +
                    " drops=" + std::to_string(droppedFrames_.load()) +
                    hardwarePresentTelemetry() +
                    " fps=" + std::to_string(fpsNum) + "/" + std::to_string(fpsDen) +
                    " decode=" + std::to_string(outW_) + "x" + std::to_string(outH_));
            }

            {
                int64_t tms = startMs + wall2;
                positionMs_.store(tms);
                lastPosMs_.store(tms);
                overlay_.setProgress(tms, durationMs);
                if ((frameIndex % 15) == 0)
                    companionTimelineTick();
            }
            if (profilePresent && prof.frames >= 300)
                logProfile();
        }

        if (profilePresent)
            logProfile();
        closeRemuxHolds();
        if (rfd >= 0)
            ::close(rfd);
    }

    killChildren();
#ifdef MPX_HAVE_LIBAV
    if (inprocPcm_)
        inprocPcm_->requestStop();
#endif
    if (audioThr_.joinable())
        audioThr_.join();
#ifdef MPX_HAVE_LIBAV
    inprocPcm_ = nullptr;
#endif
    if (streamThr_.joinable())
        streamThr_.join();

    if (!stop_.load())
        eosAudioDrain_();

    if (streamEnabled_ && frameIndex == 0) {
        FpgaSpi::BitstreamStatus st;
        if (fpga_.readBitstreamStatus(st)) {
            log("ERROR media: frames=0 with STREAM=1; DDR bitstream telemetry "
                "session=" + std::to_string(st.session_id) +
                " active=" + (st.active ? "1" : "0") +
                " paused=" + (st.paused ? "1" : "0") +
                " ring=" + std::to_string(st.ring_level) + "/" +
                std::to_string(st.ring_capacity) +
                " producer_bytes=" + std::to_string(st.producer_count) +
                " consumer_bytes=" + std::to_string(st.consumer_count) +
                " consumer_seq=" + std::to_string(st.consumer_seq) +
                " underrun=" + std::to_string(st.underrun_count) +
                " overrun=" + std::to_string(st.overrun_count) +
                " desync=" + std::to_string(st.desync_count) +
                " last_bad_seq=" + std::to_string(st.last_bad_seq) +
                " flags=u" + (st.underrun ? "1" : "0") +
                "o" + (st.overrun ? "1" : "0") +
                "d" + (st.desync ? "1" : "0") +
                "f" + (st.fatal ? "1" : "0"));
        } else {
            log("ERROR media: frames=0 with STREAM=1; DDR bitstream telemetry unreadable: " +
                fpga_.lastError());
        }
    }

    if (boundedReopenPts_.load() <= 0)
        playing_.store(false);
    {
        std::lock_guard<std::mutex> lock(summaryMu_);
        lastSummary_.rawFrames = frameIndex;
        lastSummary_.presentedFrames = presentCount_;
        lastSummary_.reconFrames = reconFrames_.load();
        lastSummary_.totalBytes = static_cast<int64_t>(totalBytes);
        lastSummary_.usedRawVideo = usedRawVideo;
        lastSummary_.streamEnabled = streamEnabled_;
        lastSummary_.skipRgb = skipRgb;
        lastSummary_.shortRead = shortRead;
        lastSummary_.videoEof = videoEof;
        lastSummary_.true480PipelineAborted = true480PipelineAborted;
        lastSummary_.shortReadGot = shortReadGot;
        lastSummary_.shortReadWant = shortReadWant;
    }
    // Only natural EOF with content may report "ended" and trigger auto-next.
    // A strict true480 transport failure is success-shaped without this guard:
    // frameIndex>0 survives teardown even though presentation aborted mid-title.
    if (onProgress_) {
        const bool hadContent = usedRawVideo ? (frameIndex > 0) : (reconFrames_.load() > 0 ||
                                                                   positionMs_.load() > startMs + 500);
        const PlaybackTerminalState terminal = classifyPlaybackTerminalState(
            stop_.load(), true480PipelineAborted, hadContent);
        if (terminal == PlaybackTerminalState::Ended) {
            onProgress_("ended", positionMs_.load(), durationMs);
        } else if (terminal == PlaybackTerminalState::Stopped) {
            int64_t stoppedAt = true480PipelineAborted ? positionMs_.load() : lastKnownPositionMs_.load();
            if (stoppedAt < 0)
                stoppedAt = 0;
            onProgress_("stopped", stoppedAt, durationMs);
            const int64_t pos = positionMs_.load();
            if (shouldBoundedReopen(stop_.load(), outH_ <= 480, pos, startMs)) {
                log("media: early stop at " + std::to_string(pos) +
                    " ms — boundedReopen");
                (void)boundedReopen(0);
            }
        }
    }
    // The frame store latches the last frame written; without this the final frame
    // of the video stays on screen until something else paints over it.
    if (!holdLastFrameAtEnd() && boundedReopenPts_.load() <= 0) {
        paintIdle();
        startIdle();
    }

    log("media: session end frames=" + std::to_string(frameIndex) +
        " recon=" + std::to_string(reconFrames_.load()) +
        " cabac=" + (cabacSkip_.load() ? "1" : "0") +
        " stream=" + (streamEnabled_ ? "on" : "off") +
        " rawvideo=" + (usedRawVideo ? "on" : "off") +
        " true480_pipeline_aborted=" + (true480PipelineAborted ? "1" : "0") +
        " present=" + presentMode_ +
        " skip_rgb=" + (skipRgb ? "1" : "0") +
        " stop=" + (stop_.load() ? "1" : "0") +
        " video_eof=" + (videoEof ? "1" : "0") +
        " short_read=" + (shortRead ? "1" : "0") +
        " short_got=" + std::to_string(shortReadGot) +
        " short_want=" + std::to_string(shortReadWant));
}

} // namespace misterplex
