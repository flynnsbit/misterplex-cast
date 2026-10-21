#pragma once
#include "mraudio_status.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
// 720p A/V keep-up gates. 480p gold Trek (rk 40868) is the class:
//   pfps≈23.7, audio_s≈wall, drops=0, clock=av-lock, hold-only.
// Unique ~12 with audio held to presentCount is the stutter FAIL
// (audio_s/wall≈0.49). Soft-skip is not a pass.

namespace misterplex {
namespace p720_av {

inline double audioKeepupRatio(double audio_s, double wall_s) {
    if (wall_s <= 0.0)
        return 0.0;
    if (audio_s < 0.0)
        return 0.0;
    return audio_s / wall_s;
}

inline bool uniqueMeets24(double pfps) { return pfps >= 23.5; }

// True 24000/1001 on a 24.10 Hz beam: content rate, not the 23.5 soft floor.
inline bool uniqueMeetsTrue24p(double pfps) { return pfps >= 23.97 && pfps <= 24.20; }

// FPGA swap count (hw_fps from plxd_swap_count) must also hold 24 unique.
// ARM pfps=23.8 with hw_fps=22.2 is lost kicks, not a scanout PASS.
inline bool uniqueScanoutMeets24(double pfps, double hw_fps) {
    return uniqueMeets24(pfps) && hw_fps >= 23.5;
}

inline bool audioKeepupMeets480pClass(double audio_s, double wall_s) {
    return audioKeepupRatio(audio_s, wall_s) >= 0.95;
}

inline bool productAvKeepupOk(double pfps, double audio_s, double wall_s, int drops) {
    return uniqueMeets24(pfps) && audioKeepupMeets480pClass(audio_s, wall_s) && drops == 0;
}

inline bool productScanoutKeepupOk(double pfps, double hw_fps, double audio_s, double wall_s,
                                   int drops) {
    return uniqueScanoutMeets24(pfps, hw_fps) && audioKeepupMeets480pClass(audio_s, wall_s) &&
           drops == 0;
}

// 720p24 CATCH is one unique per blank (H=1312 V=762 @ 24 MHz ≈ 24.10 Hz).
// Waiting THIS kick's swap, then doing avDecide/overlay/copy, then kicking
// adds post_swap_us to every period: 24.10 Hz + 2200 us → ~22.8 unique
// (Star Trek stutter soak). Kick-on-swap: wait the PREVIOUS swap, then
// doorbell; post_swap_us ≈ 0 and unique = scanout.
inline double uniqueFpsWaitThisSwap(double scanout_hz, double post_swap_us) {
    if (scanout_hz <= 0.0)
        return 0.0;
    if (post_swap_us < 0.0)
        post_swap_us = 0.0;
    return 1000000.0 / (1000000.0 / scanout_hz + post_swap_us);
}

inline bool kickOnSwapMeets24(double scanout_hz, double post_swap_us) {
    return uniqueMeets24(uniqueFpsWaitThisSwap(scanout_hz, post_swap_us));
}

// PCM bytes at 48 kHz s16le stereo for `presents` frames at num/den.
// 720p pipe warmup: drain this many gated bytes so MrAudio starts at picture
// time (audio running from t=0 while unique~8 left +1.4 s drift).
inline std::int64_t pcmBytesForPresents(std::int64_t presents, int fps_num, int fps_den) {
    if (presents <= 0 || fps_num <= 0 || fps_den <= 0)
        return 0;
    return (presents * 48000LL * 4LL * static_cast<std::int64_t>(fps_den)) /
           static_cast<std::int64_t>(fps_num);
}

inline std::size_t trimGatedPcmDrop(std::size_t gated_bytes, std::int64_t presents, int fps_num,
                                    int fps_den) {
    const std::int64_t want = pcmBytesForPresents(presents, fps_num, fps_den);
    if (want <= 0)
        return 0;
    if (want >= static_cast<std::int64_t>(gated_bytes))
        return gated_bytes;
    return static_cast<std::size_t>(want);
}

// After dropping already-presented audio, do not dump seconds of future PCM
// into MrAudio (inproc Star Trek lead ~1.2 s). Keep at most lead_ms.
inline std::size_t capGatedPcmRemain(std::size_t remain_bytes, int lead_ms) {
    if (lead_ms < 0)
        lead_ms = 0;
    const std::size_t cap =
        static_cast<std::size_t>((48000LL * 4LL * static_cast<std::int64_t>(lead_ms)) / 1000);
    return remain_bytes > cap ? cap : remain_bytes;
}

// Keep the newest `lead_ms` of gated PCM (open-gate can be seconds long).
inline std::size_t capGatedPcmEraseFront(std::size_t gated_bytes, int lead_ms) {
    const std::size_t keep = capGatedPcmRemain(gated_bytes, lead_ms);
    if (keep >= gated_bytes)
        return 0;
    return gated_bytes - keep;
}

// 480p gold Trek lipsync is tens of ms, not +1.4 s warmup lead.
inline bool driftTensOfMs(std::int64_t av_drift_ms) {
    if (av_drift_ms < 0)
        av_drift_ms = -av_drift_ms;
    return av_drift_ms <= 80;
}

inline bool productTrue24pAvOk(double pfps, double audio_s, double wall_s, int drops,
                               std::int64_t av_drift_ms) {
    return uniqueMeetsTrue24p(pfps) && audioKeepupMeets480pClass(audio_s, wall_s) && drops == 0 &&
           driftTensOfMs(av_drift_ms);
}

// 480p starts MrAudio with video. Inproc HTTP gated until first kick and
// left audio_s/wall≈0.91 at 57 s (5 s startup hole, unique already 23.7).
inline bool combined720pAudioStartsWithVideo(bool useInproc) {
    (void)useInproc;
    return true;
}

// Second HTTP ffmpeg for inproc audio started ~5 s late (av_drift_ms≈−5000).
// 720p inproc must decode PCM from the same AVFormatContext as I420.
inline bool inprocSameDemuxAudioWanted(bool is720p, bool useInproc) {
    return is720p && useInproc;
}

// Numeric loopback does not need glibc NSS. Direct HTTP inproc is allowed
// as a remux-fail fallback; product path remuxes A+V mpegts to a fifo.
inline bool urlIsLoopbackHttp(const char* url) {
    if (!url || !url[0])
        return false;
    return std::strncmp(url, "http://127.0.0.1", 16) == 0 ||
           std::strncmp(url, "HTTP://127.0.0.1", 16) == 0;
}

// 720p inproc remux must copy audio. ARM libav has no AAC/HTTP, so the box
// ffmpeg remux writes annex-B + 48k PCM fifos (one HTTP). `-an` annex-B
// forced spawnAudioOnly (second HTTP, locked av_drift_ms≈−5000).
inline bool inprocRemuxMustCopyAudio(bool is720p, bool useInproc) {
    return inprocSameDemuxAudioWanted(is720p, useInproc);
}

// 480p gold holds MrAudio to presentCount. 720p must not: inproc+prefill
// deadlocks, and combined 720p + hold locked unique at 23.5 while the
// MrAudio queue swung 180–310 ms (heard stutter). Wall-48 kHz + servo.
inline bool holdAudioToPicturesWanted(bool is720p, bool useInproc) {
    (void)useInproc;
    return !is720p;
}

// WC bank ingest is opt-in (MPX_STICK_I420). Default off: reader WC stores
// serialized produce (soak unique 23.5). Heap + overlap copy during
// kick-on-swap is the 24 unique publication path.
inline bool plex720pWcBankIngest(bool is720p) { return is720p; }

inline bool stickIngestWantedOn720pPipe(bool stickWanted, bool useInproc) {
    (void)useInproc;
    return stickWanted;
}

// 720p combined: kick-on-swap is the 24.10 Hz pace. avDecide Hold slaved
// unique to heard clock (pfps 23.5, hw_fps 22.8, audio_s/wall 0.98).
// 480p still Holds (copy 4.3 ms, unique already ~24).
inline bool combined720pSkipAvHold(bool is720p, bool useInproc) {
    (void)useInproc;
    return is720p;
}

// Skip WC bank memcpy only when FPGA DYN_BASE_EN=1 will latch doorbell phys.
// dynPhys=0 or dynBaseRbf=false must still copy (03f1b95a reads WC 0x30180000).
inline bool plex720pSkipBankMemcpy(std::uint32_t dynPhys, bool dynBaseRbf) {
    return dynBaseRbf && dynPhys != 0u;
}

// avDecide uses HEARD (written−queued). Target ring depth is ~100 ms, lead
// is 40 ms → Hold forever at drift≈−100 and unique 23.5. Add queued ms to
// lead so Hold matches 480p submitted-clock behaviour.
inline int holdLeadMsWithQueued(int presentLeadMs, std::int64_t queuedBytes) {
    if (presentLeadMs < 0)
        presentLeadMs = 0;
    if (queuedBytes <= 0)
        return presentLeadMs;
    const int qms =
        static_cast<int>((queuedBytes * 1000LL) / misterplex::kMrAudioBytesPerSec);
    constexpr int kCapMs = 200;
    const int add = qms > kCapMs ? kCapMs : qms;
    return presentLeadMs + add;
}

} // namespace p720_av
} // namespace misterplex
