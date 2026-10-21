#pragma once
// Phase 2 media player: single-process FFmpeg → /dev/fb0 + /dev/MrAudio.
// Transitional ARM decode; FPGA owns scanout (MiSTer_fb) + SPI audio (MrAudio).
// STREAM=1: annex-B demux → host I-slice recon → F1 + F3; optional RGB skip.

#include "libmisterplex/osd_menu.hpp"
#include "libmisterplex/present_bank.hpp"
#include "libmisterplex/fabric_direct.hpp"
#include "libmisterplex/library_browser.hpp"
#include "libmisterplex/browse_library_list.hpp"
#include "libmisterplex/video_backend.hpp"
#include "libmisterplex/h264_bitstream_transport.hpp"
#include "log_redact.hpp"

namespace misterplex {

// Content owns DECODE/PMS; Display owns video_mode only (RASTER-CMD).
// persist/doPlay/osdRetarget use GLASS-MAX(content), not Display geom.
// true480 never grows to 1280×720. L4 never shrinks below 1280×720 except
// the P3/P5 960×540 bank (MPX_BUDGET_960, content already 960×540, or a
// PRESENT_BEAM_960 prefix8 — do not snap those stores up to 1280).
inline bool osdRetargetDecodeSizeFromPresented(int& outW, int& outH,
                                               const ContentResolution& content,
                                               LiveGlass glass,
                                               const std::string& prefix8 = std::string()) {
    const ContentResolution bank = glassMax(content.width, content.height, glass, prefix8);
    if (outW == bank.width && outH == bank.height)
        return false;
    outW = bank.width;
    outH = bank.height;
    return true;
}

// Active height for a raster store (240 or 480): stored display height
// (black bars, no stretch). Full raster means no top/bottom crop. Guard zero.
inline int plexRasterActiveHeight(int coded_height, int raster_lines) {
    if (coded_height <= 0 || raster_lines <= 0)
        return 0;
    return coded_height >= raster_lines ? raster_lines : coded_height;
}

// 240p active height: stored display height for 240-line output (black bars,
// no stretch). Full 240 means no top/bottom crop. Guard zero source height.
inline int plex240pActiveHeight(int coded_height) {
    if (coded_height <= 0)
        return 0;
    return coded_height >= 240 ? 240 : coded_height;
}

struct PlexActiveHeightRow {
    const char* mode;
    int active_height;
    int pic_height;
};
// Shared active-height table for all modes (240p / 480p / 480i). Never x3.
inline constexpr PlexActiveHeightRow kPlexActiveHeights[] = {
    {"240p", 240, 0},
    {"480p", 480, 0},
    {"480i", 480, 240},
};

// Shared active-height for 240p/480p/480i from coded height.
// 240-line -> 240, 480-line -> 480, never 720 (240*3). Guard zero.
inline int plexSharedActiveHeight(int coded_height) {
    if (coded_height <= 0)
        return 0;
    if (coded_height >= 480)
        return 480;
    if (coded_height >= 240)
        return 240;
    return coded_height;
}

inline int plex480pActiveHeight(int coded_height) {
    return plexRasterActiveHeight(coded_height, 480);
}

// Shared G2 letterbox crop for 240p / 480p / 480i banks. Never x3 (720p).
// codedH is the stored bank height (240 or 480), displayH is the source picture.
// Even-top: ((raster - displayH) / 2) & ~1.
// Worked: 480-bank 404 -> 38/38; 350 -> 64/66; 480-tall -> false;
// 240-bank 180 -> 30/30; codedH >= 720 -> false.
inline bool plexSharedLetterboxCrop(int codedH, int displayH, int& outTop, int& outBottom) {
    outTop = 0;
    outBottom = 0;
    if (codedH <= 0 || displayH <= 0)
        return false;
    if (codedH >= 720)
        return false;
    displayH &= ~1;
    const int raster = codedH >= 480 ? 480 : (codedH >= 240 ? 240 : codedH);
    if (displayH >= raster)
        return false;
    if (displayH > codedH)
        displayH = codedH;
    const int lbTop = ((raster - displayH) / 2) & ~1;
    const int lbBottom = raster - displayH - lbTop;
    if (lbTop < 0 || lbBottom < 0)
        return false;
    outTop = lbTop;
    outBottom = lbBottom;
    return true;
}

// G2 even-top letterbox bars into rasterLines (240 or 480). A coded/display
// height that already fills the raster is passthrough (0/0).
constexpr int k240pLetterboxRasterLines = 240;
constexpr int k480pLetterboxRasterLines = 480;
inline void letterboxBarsN(int rasterLines, int codedHeight, int displayHeight, int* top,
                           int* bottom) {
    if (!top || !bottom)
        return;
    *top = 0;
    *bottom = 0;
    if (rasterLines <= 0 || codedHeight <= 0 || displayHeight <= 0)
        return;
    if (codedHeight >= rasterLines || displayHeight >= rasterLines)
        return;
    *top = ((rasterLines - displayHeight) / 2) & ~1;
    *bottom = rasterLines - displayHeight - *top;
}
inline void letterboxBars240(int codedHeight, int displayHeight, int* top, int* bottom) {
    letterboxBarsN(k240pLetterboxRasterLines, codedHeight, displayHeight, top, bottom);
}
inline void letterboxBars480(int codedHeight, int displayHeight, int* top, int* bottom) {
    letterboxBarsN(k480pLetterboxRasterLines, codedHeight, displayHeight, top, bottom);
}

// FPGA 240p/480p/480i banks that share A/V start-gate and first-PTS bind.
inline bool fpgaAvStartGateBank(int outW, int outH) {
    return (outH == 240 && (outW == 320 || outW == 640)) || (outH == 480 && outW >= 640);
}
inline bool fpgaAudioPtsBindBank(int outW, int outH) {
    return fpgaAvStartGateBank(outW, outH);
}
// 480i letterbox/crop rect resolution: returns true when the crop rect
// (left/right/top/bottom) was adjusted for 480i aspect so glass does not
// cut off. Called behind is480i in the 480i present path.
bool letterboxCrop480i(int& outLeft, int& outRight, int& outTop, int& outBottom,
                       int codedW, int codedH, int displayW, int displayH,
                       bool isLetterbox, bool isCropTo43);

} // namespace misterplex

#ifndef MPX_OSD_DECODE_SIZE_ONLY

#include "fb_present.hpp"
#include "fpga_spi.hpp"
#include "libmisterplex/r1_plxh.hpp"
#include "libmisterplex/r1_idle_paint.hpp"
#include "libmisterplex/last_frame_hold.hpp"
#include "libmisterplex/idle_fb_map.hpp"
#include "libmisterplex/cached_src_phys.hpp"
#include "libmisterplex/idle_screen.hpp"
#include "libmisterplex/mraudio_status.hpp"
#include "libmisterplex/playback_overlay.hpp"
#include "libmisterplex/fpga_terminal.hpp"
#include "libmisterplex/fpga_av_trace.hpp"
#include "libmisterplex/av_sync.hpp"
#include "libmisterplex/browse_controller.hpp"
#include "libmisterplex/plexui/plexui_controller.hpp"
#include "libmisterplex/gate_owner.hpp"
#include "libmisterplex/pts_wrap.hpp"
#include "libmisterplex/stalled_pic_watchdog.hpp"

#include "libmisterplex/owenb_core_identity.hpp"
#include "libmisterplex/plexob_yc.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace misterplex {

// The R1 PLX slots through FpgaSpi's gated, uncached doorbell page (r1_plxh.hpp PlxPort).
class FpgaSpiPlxPort final : public r1::PlxPort {
public:
    explicit FpgaSpiPlxPort(FpgaSpi& fpga) : fpga_(fpga) {}
    bool store32(uint32_t phys, uint32_t value) override { return fpga_.plxStore32(phys, value); }
    bool store64(uint32_t phys, uint64_t value) override { return fpga_.plxStore64(phys, value); }
    bool load64(uint32_t phys, uint64_t* value) override { return fpga_.plxLoad64(phys, value); }
    void barrier() override { __sync_synchronize(); }
    // A core reload re-primes the next PLXH request (review 20:23 RULING C).
    uint64_t coreChanges() override { return CoreGate::process().changes(); }

private:
    FpgaSpi& fpga_;
};

// The F1 PLXL beat's one daemon call (f1_beat.hpp BeatPort -> FpgaSpi::plxlBeatStore).
class FpgaSpiBeatPort final : public f1::BeatPort {
public:
    explicit FpgaSpiBeatPort(FpgaSpi& fpga) : fpga_(fpga) {}
    f1::BeatStore store(uint64_t gateSnap, uint32_t plxlPhys, uint32_t* hb, bool* seeded) override {
        return fpga_.plxlBeatStore(gateSnap, plxlPhys, hb, seeded);
    }

private:
    FpgaSpi& fpga_;
};

#ifdef MPX_HAVE_LIBAV
class AvInprocDecoder;
#endif

struct PlaybackSummary {
    int64_t rawFrames = 0;
    int64_t presentedFrames = 0;
    int64_t lastPresentedAudioSamples = -1;
    int64_t reconFrames = 0;
    int64_t totalBytes = 0;
    bool usedRawVideo = false;
    bool streamEnabled = false;
    bool skipRgb = false;
    bool shortRead = false;
    bool videoEof = false;
    bool true480PipelineAborted = false;
    size_t shortReadGot = 0;
    size_t shortReadWant = 0;
    int codedWidth = 0, codedHeight = 0;
    int visibleWidth = 0, visibleHeight = 0;
    int macroblockColumns = 0, macroblockRows = 0;
    int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    int cropTop240p = 0, cropBottom240p = 0;
    int cropTopLb = 0, cropBottomLb = 0;
    SourceAspect sourceAspect{};

    int64_t deliveredFrames() const {
        return rawFrames > 0 ? rawFrames : (reconFrames > 0 ? reconFrames : presentedFrames);
    }
};

struct TtfpTimestamps {
    int64_t startUs = 0;           // playInternal after previous join (steady_clock)
    int64_t firstPacketUs = 0;     // first packet received
    int64_t firstDecodedUs = 0;    // first AU submitted to FPGA ring
    int64_t firstPresentedUs = 0;  // first frame presented to glass
    int64_t phase1Us = 0;          // start → first-packet
    int64_t phase2Us = 0;          // first-packet → first-decoded
    int64_t phase3Us = 0;          // first-decoded → first-presented
    bool complete = false;
    bool logged = false;
};

inline std::string formatTtfpLog(const TtfpTimestamps& t) {
    return std::string("media: TTFP ") + (t.complete ? "complete" : "incomplete") +
           " start=" + std::to_string(t.startUs) +
           " first_packet=" + std::to_string(t.firstPacketUs) +
           " first_decoded=" + std::to_string(t.firstDecodedUs) +
           " first_presented=" + std::to_string(t.firstPresentedUs) +
           " phase1=" + std::to_string(t.phase1Us) +
           " phase2=" + std::to_string(t.phase2Us) +
           " phase3=" + std::to_string(t.phase3Us) + " us" +
           " first_picture_ms=" +
           std::to_string((t.startUs && t.firstPresentedUs)
                              ? (t.firstPresentedUs - t.startUs) / 1000
                              : 0);
}
inline void ttfpTryComplete(TtfpTimestamps& t) {
    if (t.startUs && t.firstPacketUs && t.firstDecodedUs && t.firstPresentedUs) {
        t.phase1Us = t.firstPacketUs - t.startUs;
        t.phase2Us = t.firstDecodedUs - t.firstPacketUs;
        t.phase3Us = t.firstPresentedUs - t.firstDecodedUs;
        t.complete = true;
    }
}

namespace owenb {
class OwenbPlayer;
// Opaque here (owenb_session.hpp pulls in h264ring.h); must match owenb_session.hpp.
enum class EndReason : uint8_t;
struct ObMenuState;   // OWENB-P1B 0010 (owenb_menu.hpp)
}
namespace owenb_osd {
class UiBus;   // OWENB-P1B T6: the OB overlay's UI state (owenb_osd_renderer.hpp)
struct LibraryPage;   // OWENB-P1B 0010 (owenb_osd_draw.hpp)
}

class MediaPlayer {
public:
    ~MediaPlayer() { shutdown(); }

    using LogFn = std::function<void(const std::string&)>;
    using ProgressFn = std::function<void(const std::string& state, int64_t timeMs, int64_t durMs)>;
    // Reporting-identity bind after retirement, before new worker progress.
    using StartedFn = std::function<void()>;
    struct StoppedPosition {
        int64_t timeMs;
        int64_t durationMs;
    };
    struct StartedPlayback {
        uint64_t generation = 0;
        uint64_t epoch = 0;
    };
    // Fired when OSD content-resolution bits change (O[5:4]). Main uses this to
    // re-resolve the PMS weak ladder / restart the session at the same offset.
    using ContentResFn = std::function<void(const ContentResolution& res, bool playing)>;
    // Fired when resolved Display row changes (O[15:14], or Follow + content).
    // Live OSD Display only persists DISPLAY_RES. video_mode is latched at
    // daemon start / core reset (latchAndApplyDisplayRaster).
    using DisplayResFn = std::function<void(const ContentResolution& res)>;
    using LibraryFetchFn = std::function<std::string(const std::string& path)>;
    using LibraryPlayFn = std::function<void(const std::string& ratingKey)>;
    using AudioSessionControl = FpgaSpi::AudioControl;
    bool fpgaH264Backend() const { return backend_ == VideoBackend::FpgaH264; }
    // True when the loaded core is High/CABAC. The session row is empty until
    // CAPS, which is after resolve, so the open core gate's build id counts first.
    bool negotiatedHighCabac() const {
        const CoreGate& gate = CoreGate::process();
        return highCabacForPlay(fpga_.negotiatedBuild(), gate.open(), gate.buildId());
    }
    VideoBackend videoBackend() const { return backend_; }
    // OWENB-P1B T3: this session's backend. The core gate picks OwenbRing on a PlexOB core;
    // MPX_VIDEO_BACKEND=owenb-ring forces it (sessionVideoBackend, video_backend.hpp).
    VideoBackend sessionBackend() const {
        return sessionVideoBackend(backend_, CoreGate::process().ownsIdleInFabric());
    }
    // OWENB-P1B T5: the OB play (owenb_play.hpp) is linked when the daemon is built with
    // MPX_HAVE_OWENB_RING and MPX_HAVE_LIBAV. Without them the OB route refuses play with a clear
    // error. The OB route never falls back to the legacy path.
    bool owenbPlayActive() const;
    // OWENB-P1B T7: the OB idle session (overlay without a play). The card shows this name; the main loop's
    // PMS probe feeds the "Plex server unreachable" screen (-1 unknown, 0 unreachable, 1 connected).
    void setObIdleName(const std::string& name);
    // GCAST P4: a line under the idle chevron (R1) and the OB idle card's hint. Empty = unchanged.
    void setIdleReadyLine(const std::string& line);
    // MEMSTAT: owenb out_ / ready_ depths and the demuxer probe-packet bound. Zeros when no OB play.
    void owenbQueueDepths(unsigned long long& out, unsigned long long& ready, unsigned long long& probe) const;
    void setObPmsState(int state, const std::string& label);
    bool owenbIdleWanted() const;
    static void owenbFbconAtStart();   // OWENB-P1B 0009: give back a console a dead daemon left detached
    // The cast timeline's volume (Plex 0..100; companion setParameters). On OB it is the core's
    // attenuation (owenb::attenuationForVolume); the legacy path keeps full volume.
    void setVolumePercent(int volume);
    int volumePercent() const { return volumePercent_.load(); }
    int64_t consumedAudioSamples() const { return fpgaAudioConsumedSamples_.load(); }

    void setLog(LogFn f) { log_ = std::move(f); }
    // A boundedReopen restart is in flight: the "stopped" terminal of the old session is not the end.
    bool reopenPending() const { return boundedReopenPts_.load() > 0; }
    void setProgress(ProgressFn f) { onProgress_ = std::move(f); }
    // BUG1 (lab 7AE78D80): the OB play's stream entropy (OwenbPlaySession onEntropy), for the per-PMS memory.
    // Set once at start-up, before any play (read on the play thread).
    using ObEntropyFn = std::function<void(bool cavlc, unsigned kbps, unsigned loadPct)>;
    void setObEntropyHook(ObEntropyFn f) { obEntropyHook_ = std::move(f); }
    void setOnContentResolutionChanged(ContentResFn f) { onContentRes_ = std::move(f); }
    void setOnDisplayResolutionChanged(DisplayResFn f) { onDisplayRes_ = std::move(f); }
    void setLibraryFetch(LibraryFetchFn f) { libraryFetch_ = std::move(f); }
    void setLibraryPlay(LibraryPlayFn f) { libraryPlay_ = std::move(f); }
    // MPX_BROWSE_LIBLIST exactly "1" (browse_library_list.hpp). Off leaves browse and play alone.
    void setLibraryListEnabled(bool on) { libraryListOn_ = on; }
    bool libraryListEnabled() const { return libraryListOn_; }
    void openLibraryBrowser();
    // MPX_BROWSE_V2=1 (browse_v2.hpp), before startInputPoll/startOsdPoll and the first
    // paint: the browser runs on its own mpx-browse worker (browse_controller.hpp) with the
    // helper/PMS fetch in cfg, plays through play, paints off presentMu_ and reads the
    // mailboxes without it. navCommands (MPX_BROWSE_MVPB=1): also PLXI commands 6-11.
    // Never called when the flag is off, so nothing below changes then.
    void enableBrowseV2(BrowseController::Config cfg,
                        std::function<uint64_t(const BrowsePlayTicket&)> play,
                        bool navCommands = false);
    // MPX_PLEXUI=1 with browse v2 + MVP-b: direct PMS Plex TV UI instead of row browse.
    void enablePlexUi(plexui::PlexUiController::Config cfg,
                      std::function<uint64_t(const BrowsePlayTicket&)> play);
    void plexUiCompanionNav(const std::string& op);
    bool browseV2() const { return browseV2_; }
    bool plexUi() const { return plexUi_ != nullptr; }
    // Core reset / daemon start: latch Display and write video_mode once.
    void latchAndApplyDisplayRaster(const ContentResolution& display);
    void setFfmpegPath(std::string p) { ffmpeg_ = std::move(p); }
    // Conf FFMPEG_SWS_FLAGS: bicubic (quality) | fast_bilinear (rate ladder) | neighbor |
    // skip|none|off|identity (omit scale) | exact[_fast_bilinear|_neighbor] (force WxH,
    // no foar/pad — size contract for DDR I420 without dual-pass scale+pad).
    void setFfmpegSwsFlags(std::string flags) {
        if (!flags.empty())
            swsFlags_ = std::move(flags);
    }
    // Conf FFMPEG_FPS_FILTER=off: omit fps= filter when content is already CFR at
    // the paced rate (saves dual-A9 filtergraph cost). Default on (safe for VFR).
    void setFfmpegFpsFilter(bool on) { fpsFilter_ = on; }
    // Conf DEINTERLACE=yadif|bwdif|off. Host bob before DDR present — DVD-style
    // progressive glass without waiting for VGA_F1/ascal bob on the RBF.
    // Empty/off: no filter (film progressive sources stay untouched).
    void setDeinterlaceMode(std::string mode) { deinterlaceMode_ = std::move(mode); }
    // AUDIO_PREFILL_MS: milliseconds of decoded PCM to hold in userspace before first picture. Default 100 (kFeedTargetBytes). 0 = off. Not wired from conf in this change.
    void setAudioPrefillMs(int ms) {
        if (ms < 0)
            ms = 0;
        if (ms > 200)
            ms = 200;
        audioPrefillMs_ = ms;
    }
    int audioPrefillMs() const { return audioPrefillMs_; }
    // Conf UV_U_BIAS / UV_V_BIAS: add to every chroma sample before DDR present.
    // Lab HDMI@B6 showed systematic U≈−8 vs source (fluorescent green L/R foliage).
    // Positive U bias pulls green toward neutral. Range clamped ±32.
    void setUvBias(int uBias, int vBias) {
        if (uBias < -32) uBias = -32;
        if (uBias > 32) uBias = 32;
        if (vBias < -32) vBias = -32;
        if (vBias > 32) vBias = 32;
        uvUBias_ = uBias;
        uvVBias_ = vBias;
    }
    void setAudioPath(std::string p) { audioDev_ = std::move(p); }
    void setAudioEnabled(bool on) { audioEnabled_ = on; }
    // present: "fb0" (default) and/or "fpga" (DDR YUV420p → frame_store)
    void setPresentMode(std::string mode) { presentMode_ = std::move(mode); }
    void setDdrMemSync(bool on) { fpga_.setDdrMemSync(on); }
    void setDdrMemFlush(bool on) { fpga_.setDdrMemFlush(on); }
    void setDdrFrameFormat(DdrFrameFormat format) { ddrFrameFormat_ = format; }
    void setPresentProfile(bool on) { presentProfile_ = on; }
    // STREAM=1: demux annex-B H.264 → host I-slice recon (I420 → F1) + F3 stub feed
    void setStreamEnabled(bool on) { streamEnabled_ = on; }
    // When STREAM recon owns F1, optionally drop heavy FFmpeg RGB decode (keep audio).
    // "auto" | "1"/"on" = skip RGB from session start when PRESENT=fpga (audio + demux only).
    // PRESENT=both/fb0 always keeps RGB (continuous fb0). CABAC + skip → black F1: set
    // STREAM_SKIP_RGB=0 or PRESENT=both for fb0 fallback.
    // "0"/"off" = always full RGB decode for fb0/diagnostic fallback.
    void setStreamSkipRgb(std::string mode) { streamSkipRgb_ = std::move(mode); }
    // STREAM=0 only: optional FFmpeg subtitles filter for local file paths (see docs/subtitles-burnin.md).
    // "off" | "ffmpeg" — PMS burn-in is handled in resolve (WeakLadder::burnSubtitles).
    void setSubtitleMode(std::string mode) { subtitleMode_ = std::move(mode); }
    void setSubtitleStreamIndex(int idx) { subtitleStreamIndex_ = idx; }
    // Intentional A/V lead compensation via FFmpeg adelay (ms). Default 0.
    // Prefer contentFps wall/audio pacing first; use adelay only for small residual.
    void setAudioDelayMs(int ms) { audioDelayMs_ = ms < 0 ? 0 : ms; }
    int audioDelayMs() const { return audioDelayMs_; }
    // Clamp audio-lead vs video on 480p: if audio PTS exceeds video PTS by more
    // than this bound, hold audio back so it cannot run ahead into video cut-off.
    // 0 disables the clamp. Default 300 ms. Conf AV_480P_AUDIO_LEAD_CLAMP_MS.
    void setAudioLeadClamp480pMs(int ms) { audioLeadClamp480pMs_ = ms < 0 ? 0 : ms; }
    int audioLeadClamp480pMs() const { return audioLeadClamp480pMs_; }
    // FPGA audio PTS bind: fpgaAudioPump pins first audio PTS to the video clock
    // so 240p/480p/480i playback holds lip sync. Default on. Bank-gated.
    void setAudioPtsBind(bool on) { audioPtsBindEnabled_ = on; }
    bool audioPtsBind() const { return audioPtsBindEnabled_; }
    // FPGA-budget direct vs proxy. Shared 240p/480p/480i. level cap is 30.
    static bool directProxyPolicy(int profile_idc, bool has_cavlc, int max_ref_frames,
                                  bool has_bframe, int level);
    // Conf VIDEO_DRAIN_ON=1: on stream EOF, drain the FPGA decode pipeline so
    // the last decoded pictures play out instead of cutting off (MPX_H264_VIDEO_DRAIN).
    // Default OFF (0) — today's behaviour (immediate stop on EOF).
    void setVideoDrain(bool on) { videoDrainOn_ = on; }
    bool videoDrain() const { return videoDrainOn_; }
    // Seed for the feed-rate servo, in ppm, and the open-loop fallback if the
    // driver's ring depth is unreadable. NOT a calibration any more — see
    // feedRateBytesPerSec().
    void setAudioClockPpm(int ppm) {
        if (ppm < -20000)
            ppm = -20000;
        if (ppm > 20000)
            ppm = 20000;
        audioClockPpmConf_ = ppm;
        if (audioClockTrimEnabled_)
            audioClockPpm_ = ppm;
    }
    // OSD O[3]: disable the feed-rate trim entirely (debug). Off means seed the
    // servo at exactly nominal 48 kHz; on restores the configured seed.
    void setAudioClockTrimEnabled(bool en) {
        audioClockTrimEnabled_ = en;
        audioClockPpm_ = en ? audioClockPpmConf_ : 0;
    }
    int audioClockPpm() const { return audioClockPpm_; }
    // Exact content frame rate as a rational (24000/1001 for 23.976 NTSC film).
    // This drives A/V pacing, so it must NOT be bucketed: pacing 23.976 content at 24
    // makes video lead by ~1 ms/s (~234 ms by 3:54, ~5.5 s over a 91-minute episode).
    // 0/0 = unknown → fall back to 24/1 and lean on the drift corrector.
    void setContentFpsRational(int num, int den);
    // Convenience shim for integer rates (12/24/30/60) and tests.
    void setContentFps(int fps) { setContentFpsRational(fps, 1); }
    int contentFpsNum() const { return fpsNum_; }
    int contentFpsDen() const { return fpsDen_; }
    // Present lead (ms) so the vsync path is not starved. Conf AV_PRESENT_LEAD_MS.
    void setPresentLeadMs(int ms) { presentLeadMs_ = ms < 0 ? 0 : ms; }
    // After the first 720p video kick, wait this many ms before the first
    // MrAudio write. Lab d30e7461 + MS2109 @100 ms: HDMI median a−v ≈ +12 ms.
    // Auto 100 on native 1280 inproc only. Conf AV_HDMI_AUDIO_LAG_MS.
    void setAvHdmiAudioLagMs(int ms) { avHdmiAudioLagMs_ = ms; }
    int avHdmiAudioLagMs() const { return avHdmiAudioLagMs_; }
    // Drift (ms) past which a late frame is dropped to re-converge. Conf AV_RESYNC_DROP_MS.
    // 0 disables dropping (hold-only pacing). 720p L4 ignores this and presents
    // every decoded frame; FPGA NACK is a send failure, not an A/V drop.
    void setResyncDropMs(int ms) { resyncDropMs_ = ms < 0 ? 0 : ms; }
    static constexpr int kDefaultResyncDropMs = 80;
    // Signed live A/V trim (ms), applied in the pacing loop rather than via an
    // FFmpeg filter, so the OSD can move it mid-playback with no respawn.
    //   > 0  hold video back  -> audio plays EARLIER relative to picture
    //   < 0  advance video    -> audio plays LATER  ("fixes" lips-ahead)
    void setAvOffsetMs(int ms) {
        if (ms < -1000)
            ms = -1000;
        if (ms > 1000)
            ms = 1000;
        avOffsetMs_.store(ms);
    }
    int avOffsetMs() const { return avOffsetMs_.load(); }
    // Idle/screensaver painting. Without this the frame store keeps the last frame
    // of the previous video on screen forever.
    void setIdleMode(IdleMode m) { idleMode_.store(static_cast<int>(m)); }
    IdleMode idleMode() const { return static_cast<IdleMode>(idleMode_.load()); }
    void setHoldLastFrame(bool on) { lastFrameHoldEnabled_.store(on); }
    bool holdLastFrame() const { return lastFrameHoldEnabled_.load(); }
    // IDLE-BLACK: the hold skips the end-of-session idle paint only where the core can keep the last AU, i.e.
    // not once the R1 host idle FB is armed (that core scans the idle FB after END: black). last_frame_hold.hpp.
    bool holdLastFrameAtEnd() const {
        return last_frame_hold::holdAtEnd(lastFrameHoldEnabled_.load(), r1Armed_.load());
    }
    void startIdle();
    void stopIdle();

    // Live OSD menu control. Only enable against a core whose CONF_STR uses the
    // v7 bit layout (see libmisterplex/osd_menu.hpp) — older layouts put Pattern
    // and Content FPS on the same bits, which would be read as an A/V offset.
    void setOsdControl(bool on) { osdControl_ = on; }
    void startOsdPoll();
    void stopOsdPoll();
    void setSkipDeltasMs(int64_t forwardMs, int64_t backMs);
    void startInputPoll();
    void stopInputPoll();
    // Layout/DAR commits can replace the shared DDR mapping. Retire every
    // background FPGA user before the commit, then resume pollers after the new
    // playback starts (or restore idle on a failed handoff).
    void suspendFpgaWorkers();
    void resumeFpgaWorkers(bool restoreIdle);
    uint16_t lastOsdWord() const { return lastOsd_.load(); }
    // Paint one idle frame right now (used at session end).
    void paintIdle();
    // Core gate (core_gate.hpp), on the daemon's idle tick: re-read /tmp/CORENAME and
    // log one line per transition. Back on Plex: reset the per-core FPGA state and
    // repaint the idle screen.
    void coreGateTick();
    // Lab --play-file runs before the main loop: tick the gate until it has verified the
    // loaded core (settle + CAPS probe), or timeoutMs runs out. True when open.
    bool waitCoreGateOpen(int timeoutMs);
    // MAST-WEDGE (review 21:14, mast_reload.hpp): called once the hand-off of ONE guarded
    // core reload to $MPX_MAST_RELOAD_CMD has started; the caller makes misterplexd exit with
    // mast::kReloadExitCode. Unset: a wedge is a clean fail (no reload, no blind retries).
    void setMastReloadHandler(std::function<void(const std::string& where)> h) {
        mastReloadHandler_ = std::move(h);
    }
    bool mastReloadRequested() const { return mastReloadRequested_.load(); }
    // SPS-gate layer 3 (L3 paper v1 c1f55425): the gate fallback hand-off. A play the stream or CAPS gate refuses
    // with 0 AUs written (rows R2, R6) ends WITHOUT a terminal report; the player calls this handler instead, once,
    // from its worker, and main re-resolves the SAME item with forceLadder (one fallback per item). A user stop or a
    // new play between the refusal and the worker's terminal report bumps the epoch and cancels the hand-off. The
    // handler must not block: main dispatches the re-play on its own thread.
    struct GateFallbackRequest {
        uint64_t generation = 0;  // the refused play's generation (main drops a stale one)
        int64_t offsetMs = 0;     // the refused play's start: the fallback restarts the SAME item there
        std::string reason;       // printed after "gate fallback: " in resolve's r.detail
        std::string row;          // R2 or R6
    };
    void setGateFallbackHandler(std::function<void(const GateFallbackRequest&)> h) {
        gateFallbackHandler_ = std::move(h);
    }
    // The on-screen gate error (pre-reg s1/v1/n1/r2; L3 b4). Rows R1, R7, R8 and R10h set it; while it is set the
    // idle painters draw it in place of the chevron. Only a USER stop or a new PLAY request (main), a core change or
    // a CAPS re-read (the core gate's Opened) clears it, never the player's own failFpgaSession teardown. A play that
    // a newer PLAY request or a stop has replaced can no longer set it (its epoch or generation is stale).
    void clearGateError(const char* why, bool repaint);
    std::string gateErrorText() const;
    // A new PLAY request of generation gen: clears the error and fences off every older play's setGateError.
    void noteGatePlayRequest(uint64_t gen);
    // Live A/V drift: audio clock − content time of the last presented frame.
    // Negative = video ahead of audio (audio sounds late).
    int64_t avDriftMs() const { return avDriftMs_.load(); }
    int64_t droppedFrames() const { return droppedFrames_.load(); }
    void setDecodeSize(int w, int h);
    void setLiveGlass(LiveGlass glass) { liveGlass_ = glass; }
    void setRbfPrefix8(std::string prefix8) {
        if (fpgaH264Backend() && playing_.load() && prefix8 != rbfPrefix8_) {
            std::lock_guard<std::mutex> lock(mu_);
            playEpoch_.fetch_add(1);
            stop_.store(true);
            currentUrl_.clear();
            currentHeaders_.clear();
        }
        rbfPrefix8_ = std::move(prefix8);
    }
    LiveGlass liveGlass() const { return liveGlass_; }
    SourceAspect probeSourceAspect(const std::string& urlOrPath,
                                   const std::string& httpHeaders = {},
                                   std::string* failDetail = nullptr,
                                   int* codedW = nullptr,
                                   int* codedH = nullptr,
                                   int* fpsNum = nullptr,
                                   int* fpsDen = nullptr) const;
    bool setSourceAspect(const SourceAspect& aspect);
    // PMS Media/Stream or local-file coded size (0 = unknown).
    // Identity skip requires probed source == DECODE bank (not bank==coded).
    void setSourceMediaSize(int w, int h) {
        sourceMediaW_ = w > 0 ? w : 0;
        sourceMediaH_ = h > 0 ? h : 0;
    }
    void setSourceInterlaced(bool interlaced) { sourceInterlaced_ = interlaced; }
    bool sourceInterlaced() const { return sourceInterlaced_; }
    uint32_t coreId480i() const { return coreId480i_; }
    uint32_t coreId240p() const { return coreId240p_; }
    // Hex BUILD_ID for the own-core RBF (480i/240p/prod 480p). Logs only; no load_core.
    std::string selectCoreRbfForMode(int width, int height) const;
    // Goal F: menu bounce + named Utility load_core when the loaded core id != targetBuild.
    bool tryMenuReloadForTargetBuild(uint32_t targetBuild);
    // Menu core after an INT between menu.rbf and PlexOB, or at the next
    // start: write the PlexOB load_core. A loaded PlexOB core is left alone.
    // atStart: a bare daemon on the menu core loads PlexOB with no bounce
    // marker. A later call (process exit) still requires the marker.
    bool recoverStrandedMenuCore(bool atStart);
    // 176: a profile refuse must be torn down by the next play, not only by the
    // worker exit (that exit never logged on 176).
    void noteGateRefuse();
    bool takeGateRefuse();
    void teardownAfterGateRefuse();
    // When false, do not open ffmpeg pipe:3 (source has no audio stream).
    void setSourceHasAudio(bool v) { sourceHasAudio_ = v; }
    // Resolve served a transcode (ladder proxy or PMS universal), not the Part: the FPGA path then
    // sends the stream's DAR over the metadata (firstAuSourceAspect, review 19:24 L2).
    void setSourceTranscoded(bool v) { sourceTranscoded_.store(v); }
    // R1 PLXH host side for the idle painter: paint flips through r1PlxhPolicy().requestIdle,
    // F5 pacing through r1PlxhPacer() (r1_plxh.hpp; review 19:16 (1)(c), 19:27).
    r1::PlxhLink& r1PlxhLink() { return plxhLink_; }
    r1::PlxhSessionPolicy& r1PlxhPolicy() { return plxhPolicy_; }
    r1::PlxhF5Pacer& r1PlxhPacer() { return plxhPacer_; }
    // review 06:57 D3 + 07:30 C1: the PLXH en=1 gate closes at the PLAY request, before the
    // resolve, with a hold keyed by the request's play generation (r1_plxh.hpp beginPlayRequest).
    // main.cpp begins it at every PLAY bump of playGen; each doPlay owns it while it runs
    // (r1_plxh.hpp PlayRequestHold on r1PlxhPolicy()); the session of that generation takes it over
    // at its enter; a STOP ends the holds before its own generation.
    uint64_t r1BeginPlayRequest(const std::function<uint64_t()>& bump) {
        const uint64_t gen = plxhPolicy_.beginPlayRequest(bump);
        if (gen != 0)
            noteGatePlayRequest(gen);   // SPS-gate L3 b4: a new PLAY request clears the gate error
        return gen;
    }
    void r1PlayRequestsEndedBefore(uint64_t gen) { plxhPolicy_.playRequestsEndedBefore(gen); }
    bool sourceHasAudio() const { return sourceHasAudio_; }
    int sourceMediaW() const { return sourceMediaW_; }
    int sourceMediaH() const { return sourceMediaH_; }
    // Host recon frames presented this session (I/IDR only)
    int64_t reconFrames() const { return reconFrames_.load(); }
    bool reconPresentOk() const { return reconPresentOk_.load(); }
    PlaybackSummary lastPlaybackSummary() const;
    TtfpTimestamps ttfpTimestamps() const;

    bool initPresent();

    // gateCtx (SPS-gate L3): fallbackUsed / helperStream / helperConfigured for THIS play request. An in-player seek
    // replay (seekMs) keeps the current play's context.
    bool play(const std::string& urlOrPath, int64_t startOffsetMs = 0,
              const std::string& httpHeaders = {}, int64_t durationMs = 0,
              uint64_t generation = 0, const gate::GatePlayContext& gateCtx = {});
    // Next OB open (a YouTube cast) retries an empty body, an I/O failure, or a
    // helper 5xx without leaving this generation. previousMs >= 0 is where a
    // seek resumes when the new position still will not open.
    void armCastOpenRetry(int64_t previousMs);
    // YouTube cast: one googlevideo 403 re-resolve, then play. Cleared on the
    // FPGA path and after the OB open consumes it.
    void armYt403Reresolve(std::function<bool(std::string& url, std::string& audio)> fn);
    // Resume after a daemon respawn at lastStoppedMs, or lastPosMs_ when that
    // argument is <= 0. Mid-title resume uses the same seek-safe URL rewrite as
    // seekMs(). Nothing in this tree calls it yet (main.cpp is out of this fix).
    bool resumeAfterRespawn(const std::string& urlOrPath, int64_t lastStoppedMs,
                            const std::string& httpHeaders, int64_t durationMs, uint64_t generation);
    void pause();
    void resume();
    StoppedPosition stop();
    // On-screen playback overlay API for input/transport workers.
    // showPlaybackOverlay() only affects visual feedback: it latches the state,
    // progress and a short auto-hide timer. flashPlaybackSkip() adds transient
    // "<< Ns" / "Ns >>" feedback; callers still own the actual seek/skip.
    void showPlaybackOverlay(PlaybackOverlayState state, int64_t positionMs, int64_t durationMs);
    void flashPlaybackSkip(int64_t deltaMs);
    void setPlaybackTitle(std::string title);
    // Companion/PMS timeline tick: onProgress_ is bound in main.cpp to
    // PmsTimelineReporter::reportState and Companion::setState.
    void companionTimelineTick();
    // The Plex core came back after another core ran, for the core gate's coreReturned
    // hook: with MPX_OVERLAY_ONCHANGE=1 the overlay publisher sends at its next tick, as
    // it does after show or skip feedback. This lineage has no core gate, so nothing
    // calls it yet: here a returned core always starts a new session, whose first tick
    // sends.
    void onCoreReturned();
    // Process-exit teardown: joins every worker thread without touching the FPGA
    // or reloading Main. A std::thread that is still joinable when ~MediaPlayer
    // runs calls std::terminate(), which is how the daemon used to abort on
    // SIGTERM whenever a session had ended on its own (thread finished but never
    // joined, because only stop()/play() join thr_).
    void shutdown();
    void seekMs(int64_t ms, const StartedFn& started = {}, uint64_t generation = 0);

    bool playing() const { return playing_.load(); }
    // Status block heartbeat (status byte 6 = H, status_heartbeat.hpp): only while the core gate is
    // open on a statusBlock build row; otherwise no SPI access.
    bool statusBlockActive() const { return fpga_.statusBlockActive(); }
    void setStatusBlockForce(bool on) { fpga_.setStatusBlockForce(on); }
    bool writeStatusHeartbeat(uint8_t h) { return fpga_.writeStatusHeartbeat(h); }
    std::string fpgaLastError() const { return fpga_.lastError(); }
    uint64_t playbackEpoch() const { return playEpoch_.load(); }
    StartedPlayback startedPlayback() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (currentUrl_.empty() || startedPlayback_.epoch != playEpoch_.load())
            return {};
        return startedPlayback_;
    }
    bool audioActive() const { return audioActive_.load(); }
    int64_t positionMs() const { return positionMs_.load(); }
    int64_t lastPosMs() const { return lastPosMs_.load(); }
    int64_t currentPos() const { return lastKnownPositionMs_.load(); }
    void setCurrentPos(int64_t ms) { lastKnownPositionMs_.store(ms); }
    int64_t durationMs() const {
        std::lock_guard<std::mutex> lock(mu_);
        return durationMs_;
    }
    int decodeW() const { return outW_; }
    int decodeH() const { return outH_; }
    std::string lastError() const;
    std::string currentUrl() const;
    void setSpsRejectCode480p(std::string code);
    std::string spsRejectCode480p() const;

private:
    bool playOwenbRoute(const std::string& urlOrPath, int64_t startOffsetMs, const std::string& httpHeaders,
                        int64_t durationMs, uint64_t expectedEpoch, const StartedFn& started,
                        uint64_t generation);
    // OWENB-P1B T5: the OB transport (pause / resume / seek held / stop / volume) and its teardown.
    bool owenbSeek(int64_t ms, const StartedFn& started, uint64_t generation);
    void stopOwenbPlay();
    void owenbPauseResume(bool pause);
    // OWENB-P1B T6: f runs on the OB player's overlay UI bus, if an OB player exists (no-op otherwise).
    void withObUi(const std::function<void(owenb_osd::UiBus&)>& f);
    // OWENB-P1B T7: from coreGateTick: arm the idle session on a verified PlexOB core, close it when the
    // core goes (a hop off OB), show / clear the PMS screen on it. ensureOwenbPlayerLocked: under obMu_.
    void owenbIdleTick();
    void ensureOwenbPlayerLocked();
    // OWENB-P1B 0010: the OB input thread (mpx-ob-input): the core's input region (owenb_input.hpp) -> the
    // menu panel / the routing (owenb_menu.hpp) -> dispatchPlaybackInput, as the legacy DDR mailbox poll.
    // Started with the idle session, stopped in shutdown(). MPX_OB_INPUT=0 keeps it off.
    void startObInput();
    void stopObInput();
    void obInputLoop();
    void obMenuState(const std::shared_ptr<owenb::OwenbPlayer>& ob, owenb::ObMenuState& s);
    // OWENB-P1B 0010: the daemon's browser (browse_ V2, else library_) as the overlay's library page; from
    // paintIdle (every browser repaint) and the input thread. Publishes only a change.
    void obLibraryPublish();
    std::mutex obInputMu_;
    std::atomic<bool> obInputRun_{false};
    std::thread obInputThr_;
    std::mutex obLibMu_;
    std::shared_ptr<owenb_osd::LibraryPage> obLibLast_;   // under obLibMu_
    // OB port: PlexUI (MPX_PLEXUI=1) on a PlexOB core draws into the OB overlay instead of the R1 idle FB:
    // a full-screen BGRA8888 surface (UiBus::setSurface) at the core's canvas, clamped to fb0's geometry and
    // smem. obPlexUiPublish renders and publishes (from paintIdle, i.e. every PlexUI repaint); obPlexUiTick
    // (core-gate tick) only hands the screen back when PlexUI closes / a play starts and re-targets on a
    // canvas change. obPlexUiShownAck: the OSD thread's "frame on screen" -> PlexUI's nav trace / focus anim.
    void obPlexUiPublish();
    void obPlexUiTick();
    void obPlexUiShownAck(uint64_t gen);
    void obPlexUiAckDrain();   // input thread: a stored ack -> onR1FlipAck, off the OSD thread
    std::mutex obPlexUiMu_;
    bool obPlexUiShown_ = false;              // under obPlexUiMu_
    uint64_t obPlexUiGen_ = 0;                // under obPlexUiMu_
    unsigned obPlexUiCanvasW_ = 0, obPlexUiCanvasH_ = 0;   // under obPlexUiMu_: the canvas last targeted
    std::atomic<uint64_t> obPlexUiSubmitGen_{0};
    std::atomic<int64_t> obPlexUiSubmitMs_{0};
    std::atomic<uint64_t> obPlexUiAckGen_{0};
    std::atomic<int64_t> obPlexUiAckMs_{0};
    std::atomic<bool> obPlexUiRepaintPending_{false};
    void owenbPlayEnded(uint64_t epoch, uint64_t generation, owenb::EndReason r, const std::string& err,
                        int64_t pos);
    bool playInternal(const std::string&, int64_t, const std::string&, int64_t,
                      uint64_t expectedEpoch, const StartedFn& started = {},
                      uint64_t generation = 0);
    void fpgaThreadMain(std::string url, int64_t startMs, std::string headers,
                        int64_t durationMs, uint64_t epoch, uint64_t generation,
                        gate::GatePlayContext gateCtx);
    // SPS-gate L3: R2/R6. Records the hand-off for this epoch and ends the session (0 AUs written).
    void requestGateFallback(uint64_t epoch, uint64_t generation, int64_t offsetMs, std::string reason,
                             gate::GateRow row);
    // Latches the on-screen error unless this play (epoch, generation) was replaced by a stop or a newer request.
    void setGateError(const std::string& text, uint64_t generation, uint64_t epoch);
    void fpgaAudioPump(MPX_AV_TRACE(std::shared_ptr<FpgaAvTrace> avTrace = {}));
    bool controlFpgaAudio(uint64_t session, AudioSessionControl command);
    void failFpgaSession(const std::string& error);
    // Bounded video drain for EOS: drains the FPGA decode pipeline so the last
    // decoded pictures play out instead of cutting off. Only active when videoDrainOn_
    // is true (conf VIDEO_DRAIN_ON=1). Returns true if drain completed or was skipped
    // (flag off).
    bool tryVideoDrain(uint64_t session);
    // avsync45: one presented frame's A/V offset (video content time against the content of
    // the audio consumed at its display pickup) into the servo; logs its events.
    void avSyncMeasure(int64_t videoContentUs, uint64_t consumedAtDisplay);
    void interruptInproc();
    void threadMain(std::string url, int64_t startMs, std::string headers, int64_t durationMs);
    void audioPump(int afd);
    void streamPump(int sfd, bool allowF1Present);
    void killChildren();
    void signalChildren(int sig);
    void dispatchPlaybackInput(PlaybackCommand command);
    void libraryActivate();
    bool libraryHandleInput(PlaybackCommand command);
    // MPX_BROWSE_LIBLIST=1: fetch home sections+items, scroll the idle-FB list,
    // A plays the item through the companion play hook, B returns to the chevron.
    void openLibraryList();
    void requestOpenLibraryList();
    bool libraryHandleInputList(PlaybackCommand command);
    // Browse is always-open. If playing, stop+open on a worker — never join OSD/input.
    void requestOpenLibraryBrowser();
    // MPX_BROWSE_V2 counterparts (the flag-off functions above are untouched).
    bool libraryHandleInputV2(PlaybackCommand command);
    void paintIdleV2(bool forceOnce = false);
    // R1 (a hostIdleFb build with the idle FB mapped): idle and browse paint into the idle FB
    // through r1Planner_/r1Target_ (r1_idle_paint.hpp) instead of the DDR present banks.
    bool r1IdleFbReady();
    void paintIdleR1(bool forceOnce = false);
    // LastFrame idle (no paint): owes the D2 line for a session END that will not re-show the idle FB.
    void r1NoteNoIdleRequest(const char* why);
    // The R1 idle-entry stream clear, under r1ClearMu_ (review 20:18 (3)).
    bool r1EntryStreamClear(std::string* why);
    void startInputPollV2();
    void startOsdPollV2();
    void resetPlaybackPauseClock();
    void transitionPlaybackPause(bool paused, std::chrono::steady_clock::time_point now);
    int64_t playbackPausedUs(std::chrono::steady_clock::time_point now) const;
    int64_t playbackActiveUs() const;
    // true when STREAM product path may omit heavy RGB video decode (audio + demux only)
    bool wantSkipRgbVideo() const;
    pid_t spawnFfmpeg(const std::vector<std::string>& args, int vWriteFd, int aWriteFd);
    pid_t spawnStreamDemux(const std::string& url, const std::string& headers, int64_t startMs,
                           int writeFd);
    pid_t spawnAudioOnly(const std::string& url, const std::string& headers, int64_t startMs,
                         int aWriteFd);
    pid_t spawnHttpRemuxMpegts(const std::string& url, const std::string& headers, int64_t startMs,
                               const std::string& fifoPath, bool keepAudio = false,
                               const std::string& audioFifo = {});
    pid_t spawnHttpPrefetchTs(const std::string& url, const std::string& headers, int64_t startMs,
                              const std::string& destPath);
    void log(const std::string& s) const;

    LogFn log_;
    // Incomplete log line (a token split across writes). The mutex is inside.
    mutable SyncedLogAssembler logAsm_;
    ProgressFn onProgress_;
    ObEntropyFn obEntropyHook_;
    ContentResFn onContentRes_;
    DisplayResFn onDisplayRes_;
    LibraryFetchFn libraryFetch_;
    LibraryPlayFn libraryPlay_;
    LibraryBrowser library_;
    bool libraryListOn_ = false;
    bool libraryListVisible_ = false;
    librarylist::LibraryList libraryList_;
    std::mutex libraryMu_;
    std::atomic<bool> browseOpenBusy_{false};
    std::mutex browseThrMu_;
    std::thread browseThr_;
    // MPX_BROWSE_V2 (enableBrowseV2). paintSeq_ is taken with the browse snapshot under
    // paintSnapMu_, and a paint commits under presentMu_ only if no later one has: two paints
    // rendering off-lock at once can never leave the older picture on screen.
    bool browseV2_ = false;
    uint8_t browseInputMax_ = kLegacyMaxInputCommand; // kBrowseMaxInputCommand with MVP-b
    std::unique_ptr<BrowseController> browse_;
    std::unique_ptr<plexui::PlexUiController> plexUi_;
    std::mutex paintSnapMu_;
    uint64_t paintSeq_ = 0;
    uint64_t paintCommitted_ = 0; // presentMu_
    ContentResolution lastContentRes_{};
    ContentResolution lastDisplayRes_{};
    ContentResolution latchedDisplayRes_{1280, 720, "720p", 1500};
    bool displayRasterLatched_ = false;
    // 480i → utility 480p: one video_mode write after the first presented picture.
    bool hdmiReprobeAfterTtfp_ = false;
    bool osdResSeeded_ = false;
    LiveGlass liveGlass_ = LiveGlass::True480;
    std::string rbfPrefix8_;
    std::string lastVideoModeCmd_;
    std::string ffmpeg_ = "/media/fat/mistercast/bin/ffmpeg";
    // Default bicubic: soft 480p→720 skies without vertical banding (see SCORE_BANDING_FIX).
    // Light present-rate ladder overrides via setFfmpegSwsFlags / FFMPEG_SWS_FLAGS.
    std::string swsFlags_ = "bicubic";
    bool fpsFilter_ = true;
    std::string deinterlaceMode_; // empty|off|yadif|bwdif
    int uvUBias_ = 0;
    int uvVBias_ = 0;
    int sourceMediaW_ = 0;
    int sourceMediaH_ = 0;
    bool sourceInterlaced_ = false;
    uint32_t coreId480i_ = 0;
    uint32_t coreId240p_ = 0;
    SourceAspect sourceAspect_{};
    bool sourceHasAudio_ = true; // fail-open until resolve says otherwise
    std::string audioDev_ = "/dev/MrAudio";
    std::string presentMode_ = "fb0"; // "fb0", "fpga", "both"
    bool audioEnabled_ = true;
    bool streamEnabled_ = false; // annex-B → host recon F1 + F3 stub
    std::string streamSkipRgb_ = "auto"; // auto | on | off
    std::string subtitleMode_ = "off"; // off | ffmpeg
    int subtitleStreamIndex_ = 0;
    // Conf AUDIO_DELAY_MS — default 0. Applied as FFmpeg adelay on product path.
    int audioDelayMs_ = 0;
    // 480p audio-lead clamp bound (ms). 0 = disabled.
    int audioLeadClamp480pMs_ = 300;
    bool audioPtsBindEnabled_ = true;
    // Conf VIDEO_DRAIN_ON=1: on stream EOF, drain the FPGA decode pipeline so
    // the last decoded pictures play out instead of cutting off.
    bool videoDrainOn_ = false;
    // AUDIO_PREFILL_MS — default 100. 0 = off.
    int audioPrefillMs_ = 100;
    // Starting point for the feed-rate servo, and the open-loop rate if the ring
    // depth cannot be read. Derived from the servo itself: seeded at the old
    // +685 ppm the loop settled holding a 254 B/s correction, so the FPGA's real
    // audio clock is 685 - 1323 = ~-638 ppm off nominal 48 kHz. (It plays
    // *slower* than nominal; the old +685 had the sign inverted because it was
    // measured when a growing ring looked identical to a fast playback clock.)
    // Seeding the truth means the ring settles on the target depth immediately
    // instead of being dragged there. Override with AUDIO_CLOCK_PPM.
    int audioClockPpm_ = -638;
    int audioClockPpmConf_ = -638;
    bool audioClockTrimEnabled_ = true;
    // Seeded with the calibrated default so the first frames of a session are
    // already in sync; the OSD poller overwrites it within ~100 ms.
    std::atomic<int> avOffsetMs_{misterplex::kOsdAvOffsetDefaultMs};
    std::atomic<int> idleMode_{static_cast<int>(IdleMode::Logo)};
    std::atomic<bool> lastFrameHoldEnabled_{true};
    std::atomic<bool> idleRun_{false};
    std::atomic<bool> sourceTranscoded_{false};
    // Latched by shutdown() so threadMain's session-end startIdle() cannot spawn
    // a fresh painter after we have already joined the old one.
    std::atomic<bool> shuttingDown_{false};
    std::atomic<int> idlePhase_{0};
    std::thread idleThr_;
    std::atomic<bool> idleWarned_{false};
    std::atomic<bool> idleLogged_{false};
    std::mutex idleMu_;
    std::mutex osdMu_; // same for osdThr_ // serialises idleThr_ create/join (play thread vs companion)
    std::mutex presentMu_;
    void applyOsd(uint16_t word);
    // Y/C: word 87 on the OB tick (owenbOpen), not the legacy OSD poll.
    void ycOwenbTick();
    void enqueueYcReload(const plexob::YcPollResult& job);
    void ycReloadWorker();
    void stopYcReloadWorker();
    bool reloadCoreForYcMode(const char* vgaMode, const char* ntscMode);
    void applyDisplayRaster(const ContentResolution& display, bool force = false);
    void applyLiveDisplayRaster(const ContentResolution& osdDisplay, bool force = false);
    // Snapshot the MrAudio ring pointers and occupancy. Cheap: one
    // open/read/close, no allocation.
    MrAudioStatus readMrAudioStatus();

    static std::string hex16(uint16_t v);

    bool osdControl_ = false;
    std::atomic<bool> osdRun_{false};
    std::atomic<uint16_t> lastOsd_{0};
    std::atomic<bool> osdSeen_{false};
    std::mutex ycMu_;
    plexob::YcTickState ycState_;
    // One control-page map for as long as the OB gate stays open. The tick
    // reads word 87 through it; it is not opened and closed on every tick.
    OwenbCtrlPageMap ycMap_;
    std::mutex ycJobMu_;
    std::condition_variable ycJobCv_;
    plexob::YcPollResult ycJob_{};
    bool ycJobPending_ = false;
    std::atomic<bool> ycRun_{false};
    std::thread ycThr_;
    std::thread osdThr_;
    std::mutex inputMu_;
    std::atomic<bool> inputRun_{false};
    std::thread inputThr_;
    int64_t skipForwardMs_ = 30000;
    int64_t skipBackMs_ = 10000;
    std::atomic<int64_t> ignoreInputUntilMs_{0};
    // Present pacing: keep video from free-running ahead of wall/audio (lipsync).
    // Exact rational content rate; 0/0 → treat as 24/1 when pacing with audio.
    int fpsNum_ = 0;
    int fpsDen_ = 0;
    // v0.3/v0.4 lock: 40 ms lead. 0 starves vsync (HEAD log-lie).
    int presentLeadMs_ = 40;
    int avHdmiAudioLagMs_ = -1;
    int resyncDropMs_ = 80;

    FbPresent fb_;
    FpgaSpi fpga_;
    // R1 PLXH host side (r1_plxh.hpp): one link and token sequence, the in-session en=0 policy
    // and the F5 pacer, shared by the FPGA session (fpgaThreadMain) and the idle painter.
    // Declared after fpga_ (they use it) and in dependency order.
    FpgaSpiPlxPort plxPort_{fpga_};
    r1::PlxhLink plxhLink_{plxPort_, 0, [this](const std::string& line) { log(line); }};
    r1::PlxhSessionPolicy plxhPolicy_{plxhLink_};
    r1::PlxhF5Pacer plxhPacer_{plxhPolicy_, plxhLink_, {},
                               [this](const std::string& line) { log(line); }};
    // R1 idle painter. The idle FB map (kmod WC, else /dev/mem) and the target are armed on first
    // use, when the core gate's build has hostIdleFb. r1PaintMu_ guards the planner/target state.
    // r1ClearMu_ orders the entry clear against a session start: fpgaThreadMain takes it right
    // after enterSession. r1SessionEnds_ counts session ENDs; one entry clear per value.
    std::mutex r1PaintMu_;
    std::mutex r1ClearMu_;
    std::atomic<uint64_t> r1SessionEnds_{0};
    IdleFbMap idleFbMap_;
    bool idleFbArmTried_ = false;
    std::unique_ptr<r1::R1IdleFbTarget> r1Target_;
    std::unique_ptr<browse::BrowsePaintPlanner> r1Planner_;   // allocated only on an R1 build
    int r1ViewportTop_ = 0;
    uint64_t r1CoreChanges_ = 0;
    bool r1PaintLogged_ = false;
    std::optional<std::chrono::steady_clock::time_point> plexuiR1SubmitAt_;
    int64_t plexuiPaintPlxhMs_ = 0;
    // review 06:57 D2: the first paint after each session END re-arms the planner and owes one
    // line (re-shown, or why not). D1: r1FlipOwed_ makes the idle thread retry within 250 ms.
    r1::R1SessionEndRearm r1SessionRearm_;
    bool r1AfterSession_ = false;
    std::string r1AfterSessionWhy_;
    std::atomic<bool> r1FlipOwed_{false};
    // F1 PLXL beat (f1_beat.hpp; F1-PREREG v2.2). Started where r1IdleFbReady arms the R1 target on
    // an idleBeatPlxl row; stopped in shutdown() before any member it reads goes away. Its idle
    // condition reads only atomics (review 01:39 k1): r1Armed_, playing_, fpgaSession_ and the
    // PLXH policy's inSession(). The idle paint logs its counters (logF1Beat).
    std::atomic<bool> r1Armed_{false};
    FpgaSpiBeatPort beatPort_{fpga_};
    f1::Beat f1Beat_{beatPort_, f1::BeatGate{[](uint64_t* snap, uint32_t* plxlPhys) {
                                                 return f1::f1GateOpenNow(snap, plxlPhys);
                                             },
                                             [this] { return f1IdleOk(); }}};
    bool f1IdleOk() const;
    void logF1Beat();
    std::chrono::steady_clock::time_point f1BeatLogAt_{};
    uint64_t f1BeatLogged_[6] = {0, 0, 0, 0, 0, 0};
    VideoBackend backend_ = configuredVideoBackend();
    // OWENB-P1B T5: the OB play worker (null until the first OB play; shared_ptr: the type is complete
    // only in media_player.cpp). obMu_ guards ob_ and obEpoch_.
    mutable std::mutex obMu_;
    std::shared_ptr<owenb::OwenbPlayer> ob_;
    uint64_t obEpoch_ = 0;
    std::string obIdleName_ = "MiSTerPlex";   // T7, under obMu_
    std::string obPmsLabel_;                  // T7, under obMu_
    std::string idleReadyLine_;               // GCAST P4, under obMu_
    std::atomic<int> obPmsState_{-1};
    int64_t obIdleRetryAtUs_ = 0;             // T7: main-loop thread only
    bool obPmsShown_ = false;                 // T7: main-loop thread only
    std::atomic<int> volumePercent_{100};
    std::atomic<uint64_t> playEpoch_{1};
    bool castOpenRetry_ = false;          // under mu_, consumed by the next OB play
    bool castOpenIdleOnFail_ = false;     // under mu_; a cast open failure ends on the chevron
    int64_t castOpenRetryPrevMs_ = -1;
    std::function<bool(std::string&, std::string&)> yt403Reresolve_;
    std::atomic<uint64_t> fpgaSession_{0};
    // CoreGate::changes() when fpgaSession_ began (core-gate stale-epoch drop).
    uint64_t fpgaSessionCoreChanges_ = 0;
    // Main-loop thread only: a Plex return still waiting for no session to run.
    bool coreReturnPending_ = false;
    // MAST-WEDGE: the gate-open liveness probe, and the one reload hand-off per process.
    void probeMastAtGateOpen();
    void onMastWedge(const std::string& where);
    // R5a (review 01:10): started on an 'attempted' marker and the gate never opened: a
    // terminal, loud cure-failed. Main-loop thread only (coreGateTick).
    void checkMastSilentGate();
    const int64_t mastStartUs_ = coreGateNowUs();
    bool mastSilentDone_ = false;
    std::function<void(const std::string&)> mastReloadHandler_;
    std::atomic<bool> mastReloadRequested_{false};
    int fpgaAudioOutput_ = -1; // Retained through DMA Reset and failed-epoch recovery.
    std::atomic<bool> fpgaVideoStarted_{false};
    std::atomic<bool> fpgaAudioDone_{true};
    // Set so a demux retry can join the audio pump before close(). Assigning a
    // new std::thread over a joinable one calls terminate.
    std::atomic<bool> fpgaAudioJoin_{false};
    std::atomic<bool> fpgaAudioStarted_{false};
    // G1 av_start: video start gate held until audio prefill is ready.
    std::atomic<bool> audioReadyForStart_{false};
    std::atomic<FpgaAudioExit> fpgaAudioExit_{FpgaAudioExit::NotStarted};
    std::atomic<int> fpgaAudioProgress_{-1};
    std::atomic<size_t> fpgaAudioHeldBytes_{0};
    std::atomic<int64_t> fpgaFirstVideoPtsUs_{ddr_bitstream_ring::kNoTimestamp};
    std::atomic<int64_t> fpgaVideoStartMonotonicUs_{0};
    std::atomic<int64_t> fpgaVideoStartPauseUs_{0};
    // TTFP breakdown: steady_clock microseconds for time-to-first-picture telemetry
    // (open → first packet → first decode → first display).
    std::atomic<int64_t> ttfpOpenTimeUs_{0};
    std::atomic<int64_t> ttfpFirstPacketTimeUs_{0};
    std::atomic<int64_t> ttfpFirstDecodeTimeUs_{0};
    std::atomic<int64_t> ttfpFirstPicTimeUs_{0};
    std::atomic<int64_t> ttfpFirstPicMonotonicUs_{0};
    std::atomic<int64_t> fpgaAudioConsumedSamples_{-1};
    std::atomic<int64_t> fpgaAudioClockChangedUs_{0};
    std::atomic<int64_t> fpgaAudioStartRelativeUs_{0};
    std::atomic<bool> fpgaHasConsumedClock_{false};
    int64_t last_ok_pts_ = INT64_MIN;
    // avsync45: display-boundary A/V servo for rows with avResync (fpga_build_ids.hpp). The
    // audio pump owns the resampler and feeds the map and the applied move; the video thread
    // feeds each presented frame's offset (MVPS frozen consumed count) and logs the events.
    std::mutex avSyncMu_;
    av_sync::AvSyncController avSync_;
    av_sync::AudioContentMap avMap_;
    std::atomic<bool> avSyncOn_{false};
    std::atomic<uint32_t> avSyncRowId_{0};
    std::mutex inprocMu_;
    // 720p idle: process-lifetime fabric-direct arena (flag-on). Heap yuv is STUB.
    FabricDirectAlloc idleFabric_{};
    DdrFrameFormat ddrFrameFormat_ = DdrFrameFormat::Yuv420p;
    bool presentProfile_ = false;
    bool useDdrF1_ = true; // F1 product presentation attempts DDR YUV420p only.
    int ddrBank_ = 0;      // ping-pong 0/1; stride comes from ddr_frame_layout.hpp
    // Bytes written to MrAudio this session (A/V clock diagnostics)
    std::atomic<int64_t> audioBytes_{0};
    // Bytes sitting in the MrAudio DMA ring, i.e. handed to the driver but not
    // yet played. -1 = unknown (kernel without the status line) and the clock
    // falls back to counting submitted bytes. See libmisterplex/mraudio_status.hpp.
    std::atomic<int64_t> audioQueuedBytes_{-1};
    // Live A/V drift + resync counters (per play/seek session)
    std::atomic<int64_t> avDriftMs_{0};
    std::atomic<int64_t> droppedFrames_{0};
    int64_t videoPts_ = 0;
    int64_t audioPts_ = 0;
    int64_t pts_drift_ms_ = 0;
    // FPGA presents this session (wall-clock capped)
    std::atomic<int64_t> presentCount_{0};
    mutable std::mutex mu_;
    mutable std::mutex summaryMu_;
    PlaybackSummary lastSummary_;
    std::mutex lifeMu_; // serializes play/stop thr_ join + spawn
    std::thread thr_;
    std::thread audioThr_;
    std::thread streamThr_;
#ifdef MPX_HAVE_LIBAV
    AvInprocDecoder* inprocPcm_ = nullptr;
#endif
    std::atomic<bool> stop_{false};
    std::atomic<bool> gateRefuse_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> audioActive_{false};
    // 720p: MrAudio stays closed-loop silent until warmup decode finishes, then
    // the HDMI lag hold lets it run. Default true so 480p is unchanged.
    std::atomic<bool> audioFeedRelease_{true};
    // Writer-hold on /tmp/mplex-inproc.pcm so ffmpeg can open. Must not be the
    // pump read fd: O_RDWR on the reader hides EOF when remux exits (HTTP spinner).
    std::atomic<int> remuxPcmHoldFd_{-1};
    std::atomic<int> remuxPcmReadFd_{-1};
    // 720p pipe: drain ffmpeg audio while unique warms, then start MrAudio at
    // presentCount content time. 0 = 480p / combined 720p (start with video).
    std::atomic<int> audioReleaseAfterPresents_{0};
    // 480p gold: pump waits if submitted audio is ahead of presentCount.
    // 720p inproc+prefill deadlocks; combined 720p matches 480p (true).
    std::atomic<bool> holdAudioToPictures_{true};
    std::atomic<int> audioAfterVideoMs_{0};
    // Extra audio slack vs presented frames (seconds). Native 1280: +0.080.
    // Scaled 240/480: -0.200 so HDMI pictures (later by ~200 ms) meet beeps.
    std::atomic<int> audioHoldSlackMs_{80};
    std::atomic<bool> streamActive_{false};
    std::mutex pauseControlMu_;
    mutable std::mutex pauseClockMu_;
    MPX_AV_TRACE(FpgaAvPauseClock tracePauseClock_;)
    int64_t pauseClockAccumulatedUs_ = 0;
    bool pauseClockHeld_ = false;
    std::chrono::steady_clock::time_point pauseClockStarted_{};
    std::atomic<int64_t> reconFrames_{0};
    std::atomic<bool> reconPresentOk_{false}; // at least one recon → F1/fb0 this session
    // Sticky: PPS entropy_coding_mode=1 or recon fail_reason=cabac; cleared only on
    // CAVLC PPS or new play() — not on every in-band SPS (would defeat sticky).
    std::atomic<bool> cabacSkip_{false};
    std::atomic<int64_t> seekReqMs_{-1};
    std::atomic<int64_t> positionMs_{0};
    std::atomic<int64_t> lastPosMs_{0};
    std::atomic<int64_t> lastKnownPositionMs_{0};
    TtfpTimestamps ttfp_{};
    mutable std::mutex ttfpMu_;
    PlaybackOverlay overlay_;
    // MPX_OVERLAY_ONCHANGE (overlay_publish.hpp): bumped on show, skip feedback and
    // onCoreReturned; the publisher sends at its next tick when it moves.
    std::atomic<uint64_t> overlayForceGen_{0};
    std::atomic<pid_t> childPid_{-1};
    std::atomic<pid_t> audioPid_{-1};
    std::atomic<pid_t> streamPid_{-1};
    std::string lastError_;
    std::string spsRejectCode480p_;
    // SPS-gate L3. gateCtx_, gateFallbackPending_ and gateFallbackEpoch_ are guarded by mu_; gateErrorText_ by
    // gateErrMu_ (its own lock: the idle painters read it under presentMu_/r1PaintMu_).
    std::function<void(const GateFallbackRequest&)> gateFallbackHandler_;
    gate::GatePlayContext gateCtx_;
    std::optional<GateFallbackRequest> gateFallbackPending_;
    uint64_t gateFallbackEpoch_ = 0;
    mutable std::mutex gateErrMu_;
    std::string gateErrorText_;
    uint64_t gateLatestRequestGen_ = 0;           // guarded by gateErrMu_
    std::atomic<bool> gateRepaintOnce_{false};    // a clear asked for one repaint, even in LastFrame mode
    std::string currentUrl_;
    std::string currentHeaders_;
    StartedPlayback startedPlayback_;
    std::string playbackTitle_;
    std::string fpgaOverlayTransport_;
    std::chrono::steady_clock::time_point fpgaOverlayTransportUntil_{};
    int64_t durationMs_ = 0;
    bool timelineUnknownDurLogged_ = false;
    int outW_ = 320;
    int outH_ = 240;
    // Shared fps gate for 480p/480i/240p: when true, setContentFpsRational
    // snaps through fps_gate.hpp (24/30 families, NTSC 1001 kept).
    bool fpsGate_ = false;
    // Shared 480p/480i/240p hang reopen: stash resume PTS for the play worker
    // to re-enter. Rejects ptsMs<=0 (never restart from zero).
    bool boundedReopen(uint64_t session);
    std::atomic<int64_t> boundedReopenPts_{-1};
    int boundedReopenRetryCount_ = 0;
    // Stalled-picture watchdog: detects when picture delivery halts.
    // stalledPicWatchdogHits_ counts reopen attempts in the current stall
    // sequence; notePicDelivery zeros it when a picture actually arrives.
    int64_t stalledPicWatchdogHits_ = 0;
    std::chrono::steady_clock::time_point lastPicTime_{};
    int picWatchdogTimeoutMs_{5000};
    std::atomic<std::chrono::steady_clock::rep> resumeArmTicks_{0};   // PAUSE-WATCHDOG: last resume()
    void notePicDelivery();
    bool isPicStalled() const;
    void setPicWatchdogTimeoutMs(int ms) { picWatchdogTimeoutMs_ = ms; }
    bool picStallLogged_{false};
    int picStallCount_{0};
    PtsWrapGuard ptsWrap_;
    int64_t last_good_offset_ = 0;
    int range_retry_ = 0;
};

// companion.cpp defines userStop_ for distinguishing user stop from daemon death.
extern bool userStop_;

} // namespace misterplex

#endif // MPX_OSD_DECODE_SIZE_ONLY
