#pragma once
// AD-REC A/B/C/D/E/F (#1066 Vevo split-HLS): play-all ads with a smooth splice, a full
// audio/PTS rebase at every discontinuity that matches a channel-change / tune reset,
// and a bounded mux interleave so each ~5 s Pluto audio EXTINF is not a video-only
// HDMI starve (glass 2932ecf8: 5.00 s gap starts, 5.41 s soak median).
//
// A zap reopens remux + demux + the HDMI audio clock, so both clocks share a new origin.
// Disc rebase on pts_jump / EXT-X-DISCONTINUITY does not fire on the 5 s cycle (glass B:
// 8 rebases, all on video_stall). The cycle is audio AU starvation at segment fetch.
// Glass B AUDIO-DROPOUTS.csv: the 5.00 s grid is POST-STALL (t=664+). First spawn
// is two small gaps in 10 min. C applied the 1 s wait to first spawn too: first
// video AU missed the 3000 ms RC_PIC watchdog, SIGKILL, 1-item live landed
// FIRST PICTURE FAILED: NO NEXT (glass a1c0af44).
//
// Glass D 77fa2b7e: first spawn argv 0 fixed C death (first_picture 14458 ms) but
// FAIL-FAST at soak t=30.877 with a 1.123 s HDMI hole, video living, 0 stall /
// 0 respawn / 0 rebase. Remux age ≈ 14.5 s first_picture + ~25 s confirm/t0 +
// 30.9 s soak ≈ 70 s = glass B first-spawn gap 70.704-71.847 (1.143 s) =
// 14 × 5.0 s Pluto audio EXTINF. Player-side 1.5 s ready_ cushion AFTER the
// first key video AU (argv stays 0) dumps into the H264 ring so that hole drains
// the ring, not HDMI. Split-rendition live only; VOD / muxed Pluto keep FIFO.
// This is post-first-key-AU, not FPGA first_picture. Fill is interrupt-bounded.
//
// Glass F b36a6c5a: D's t~31 s hole is gone, but fill logged audio_bytes=0
// (AAC sample_rate 0 → no PCM) and blocked every video AU after the first key
// for up to 2 s (zap first_picture vs B +348/+1149 ms; zap04 PREFETCH TIMEOUT).
// A later 1.39 s audio-EXTINF fetch hole at HDMI t=221.837 (same class, clip /
// playlist boundary, 0 rebase / 0 respawn, video living) then hit HDMI because
// the 1.5 s pipeline never filled. G returns video during the watermark so zap
// stays within 300 ms of B, and openSwr uses 48 kHz when codecpar.rate is 0.
// Glass G ded620c4: fill logged audio_bytes=290816 but HDMI sat at idle floor
// −81.9 dB from t=0 (8.99 s FAIL-FAST). G popped every video AU while PCM sat
// in ready_; the ring presented video, then the dump was AUD_LATE. H FIFOs
// A+V after the first key so paced PCM reaches the consumer. Soak client_gone
// was 0 (tune-window ffmpeg gone is the previous channel's end=stop).
//
// MUT_LIVE_NO_DISC_REBASE: keep the pre-fix soft continue (tests must FAIL).
// MUT_LIVE_NO_PLAY_ALL: snap back to the ad-break card (tests must FAIL).
// MUT_LIVE_ZERO_INTERLEAVE: respawn/soak interleave 0 (5 s HDMI starve; FAIL).
// MUT_LIVE_FIRST_SPAWN_INTERLEAVE: 1 s wait on first spawn (C first_picture; FAIL).
// MUT_LIVE_NO_PIC_RETRY: 1-item live watchdog lands NO NEXT (C death; FAIL).
// MUT_LIVE_NO_FIRST_SPAWN_PACE: no post-picture cushion (D 1.12 s HDMI hole; FAIL).
// MUT_LIVE_PACE_HOLD_AUDIO: G video-only watermark (HDMI silent after fill; FAIL).
// MUT_LIVE_NO_VIDEO_LEAD_CAP: after first key, emit video with ≥0.2 s lead (H 3.42 s
//   startup hole, glass 8a497afb r3 soak t=0.00–3.42; FAIL).
// MUT_LIVE_FLUSH_PCM_ON_DISC: pts_jump flushes adec+swr and drops the PCM reserve
//   while interleave-0 video keeps moving (H 2.64 s hole @58.38; FAIL).
// MUT_LIVE_NO_CRYPTO_PREFETCH_RETRY: return-after-zap crypto stall sent=0 becomes
//   PREFETCH TIMEOUT: NO NEXT and later inputs stay wedged (glass ff11ef73 zap02; FAIL).
// MUT_LIVE_NO_NONEXT_RETRY: re-zap 0-format remux stall stays "unsupported
//   resolution: NO NEXT" and fail() opens the guide (glass 85629316 zap4; FAIL).
// MUT_LIVE_NO_AUDIO_GAP_RECOVER: a Vevo track splice with no pts_jump leaves AAC/SWR
//   stale and HDMI silent ≥2.767 s (glass 4beb780b r1 t=23.234; FAIL).
// MUT_LIVE_RELAY_KILL_WHILE_READING: playjob kills /yt-live.ts while the previous
//   session still reads it (zap02 playjob, no first_picture; FAIL).
// MUT_LIVE_PREMATURE_RESUME: live remux EOF is a VOD premature seek (SourceError
//   wedges later zaps until daemon restart; FAIL).
// MUT_LIVE_ACCEPT_RATE0_DEMUX_STALL: zap04+ demux with codecpar.sample_rate=0 is
//   allowed to hang before prefetch (glass dbd11006: 3/6 clean, return wedged on
//   attempt 4, rate=0, no SourceError/premature; FAIL until reviewer next I).
// MUT_LIVE_NO_CLOSED_SHORT_GAP_COVER: mid-soak closed 0.454 s @ t=22.241 with
//   zero audio_gap_recover is ignored (distinct from I 2.767 s track-boundary; FAIL).
// MUT_LIVE_NO_INBAND_SILENT_BUMPER_SKIP: Vevo track-change in-band silent PCM
//   (glass ff11ef73 1.264 s @ 14.436, video living, 0 rate0/disc/pts_jump; FAIL).
// MUT_LIVE_NO_CRYPTO_PREFETCH_RETRY: return-after-zap crypto stall sent=0 becomes
//   PREFETCH TIMEOUT: NO NEXT and later inputs stay wedged (glass ff11ef73 zap02; FAIL).
// MUT_LIVE_SILENT_BUMPER_PEAK_ONLY: d81e4826 detector (peak<33 only). Glass
//   GATE-V2 latch soak: skip logged once (peak=0 after stall), escape 0, 47
//   DECODER_ONLY holes with HDMI RMS −66..−82 dBFS and peak 59–2892 (FAIL).
// MUT_LIVE_SHORT_ENERGETIC_STAMP: 9e6d1814 fill is one 4096-byte AU stamped at
//   the video PTS. Glass 9e6d1814 r1 holes are capture-floor underruns on a
//   5.000 s EXTINF grid (peak 11–17, 0 samples |s|>64); that stamp does not
//   occupy the gap (FAIL).
// MUT_LIVE_EXTINF_COVER_NO_READ_WAKE: dfc960ee sample-clock cover stays armed
//   only on a PTS lead seen after av_read_frame returns. Glass dfc960ee r1
//   is 38 DECODER_ONLY holes with the cover firing once, at the end of the
//   last hole (FAIL).
// MUT_LIVE_EXTINF_COVER_STICKY_EXIT: the wall-clock wake still returns 1 from
//   onInterrupt, and avio keeps that AVERROR_EXIT. The next read is
//   "Immediate exit requested" and the session ends (abfe266e r1: one 200 ms
//   cover, then SourceError; e8755089 r1's 34 holes stay if the wake never
//   arms). FAIL.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace misterplex {
namespace live_disc {

inline constexpr int64_t kNoPts90k = -1;
// Larger than a frame / B-reorder (~<200 ms), smaller than a typical HLS clip restart.
inline constexpr int64_t kDiscJumpTicks90k = 90000;  // 1.0 s at 90 kHz
// HDMI client: never emit a step bigger than this (decoder stalls, ring fills, client_write).
inline constexpr int64_t kMaxHdmiEmitJumpTicks90k = 90000 * 2;  // 2.0 s
inline constexpr int64_t kReorderTicks90k = 90000 / 4;          // 250 ms B-reorder
inline constexpr int64_t kFrameTicks90k = 90000 / 50;           // 20 ms
// Shawn gate: A/V within ±45 ms after N splices.
inline constexpr int64_t kAvSyncBound90k = 45 * 90;
// s16le stereo @ 48 kHz → 90 kHz: (bytes/4) * 90000 / 48000 = (bytes/4) * 15 / 8
inline constexpr int64_t kPcmBytesPer90kTickNum = 15;
inline constexpr int64_t kPcmBytesPer90kTickDen = 8;
// Pluto split-audio EXTINF is ~5.0 s (fixtures 5.013 / 4.992). Glass 2932ecf8
// HDMI gap STARTS in the post-stall clusters are 5.00 s apart (median spacing
// 5.41 s over the whole soak because stall-sized holes sit on the same grid).
inline constexpr int64_t kPlutoAudioSegTicks90k = 90000 * 5;
// ffmpeg AV_OPT_TYPE_DURATION integer is microseconds. 0 = emit the ready
// rendition immediately (glass: video-only stretches while the audio HLS
// segment is still fetching → HDMI underrun, then catch-up). 1 s covers the
// early-cycle 0.8 s fetch lag so the lag cannot accumulate, and still flushes
// at an ad-edge PTS jump (Gate B wedge was an unbounded wait).
inline constexpr int64_t kMuxInterleaveDeltaUs = 1000000;
inline constexpr int64_t kLongGapTicks90k = 90000 * 8 / 10;  // 0.8 s ACCEPT long-gap
// spelunkr kFirstPictureTimeoutMs. Glass ITER-PASS: first_picture not worse than B by >300 ms.
inline constexpr int64_t kFirstPicWatchdogMs = 3000;
inline constexpr int64_t kFirstPicVsBBudgetMs = 300;
// 1-item Pluto live: one retry reopens remux (zap recovered Vevo). Second miss lands.
inline constexpr unsigned kLivePicWatchdogRetries = 2;
// Glass D 1.123 s hole at remux ~70 s (soak 30.877). Glass F 1.390 s hole at
// HDMI t=221.837 (same audio-EXTINF fetch class, clip/playlist boundary). 1.5 s
// ready_ cushion covers each with margin once PCM actually fills; soak gate is
// 0.2 s. Fill deadline is checked in onInterrupt so a blocking av_read_frame
// returns, but video is still delivered during the watermark. s16le stereo @ 48 kHz.
inline constexpr int64_t kFirstSpawnGapRemuxTicks90k = 90000 * 70;
inline constexpr int64_t kFirstSpawnGapDurTicks90k = 1123 * 90;  // glass D 1.123 s
inline constexpr int64_t kMidSoakGapRemuxTicks90k = 221837 * 90; // glass F silence_start
inline constexpr int64_t kMidSoakGapDurTicks90k = 1390 * 90;     // glass F 1.390 s
inline constexpr int64_t kSoakGapGateTicks90k = 200 * 90;        // fail-fast ≥0.2 s
inline constexpr int64_t kFirstSpawnPaceUs = 1500000;
inline constexpr int64_t kFirstSpawnPaceAudioBytes = 48000 * 4 * 3 / 2;  // 1.5 s
inline constexpr int64_t kFirstSpawnPaceFillMs = 2000;
// Glass H 8a497afb r3 (2026-11-21 22:08–22:09 CT): soak FAIL-FAST at 61 s.
// silence_start 0 duration 3.424771; silence_start 58.383333 duration 2.636375
// on `live remux disc rebase kind=pts_jump splices=1 emit_pts=5853000`.
inline constexpr int64_t kHStartupGapDurTicks90k = 3425 * 90;
inline constexpr int64_t kHMidSoakGapDurTicks90k = 2636 * 90;
inline constexpr int64_t kHMidSoakEmitPts90k = 5853000;
// Glass ff11ef73 zap table: attempt 1 clean (1066→1063); attempt 2 return
// 1066 crypto stall sent=0 → PREFETCH TIMEOUT: NO NEXT; 03–06 stay wedged.
inline constexpr int kZap5ReturnWedgeAttempt = 2;
// Glass H 4beb780b r1 (2026-11-22 00:10 CT): soak FAIL-FAST at 25 s. HDMI living
// −28.3 dB for 23 s, then silence_start 23.234 duration ≥2.767 s (open until
// capture stop) at a Vevo content/track boundary. 0 disc rebase / 0 pts_jump.
inline constexpr int64_t kITrackBoundaryGapDurTicks90k = 2767 * 90;
inline constexpr int64_t kITrackBoundaryStartTicks90k = 23234 * 90;
// Glass dbd11006 I r1 (2026-11-22 01:02–01:03 CT): soak FAIL-FAST at 24 s.
// One closed gap start 22.241 duration 0.454 s; title unchanged; 0 disc_rebase /
// 0 pts_jump / 0 audio_gap_recover. Distinct from kITrackBoundary* (2.767 @ 23.234).
inline constexpr int64_t kIClosedShortGapDurTicks90k = 454 * 90;
inline constexpr int64_t kIClosedShortGapStartTicks90k = 22241 * 90;
// Glass dbd11006 zap table: attempts 01–03 clean (rate=48000); attempt 4 demux
// rate=0 and never reaches prefetch/first_picture; 05–06 stay wedged.
inline constexpr int kIZap4WedgeAttempt = 4;
// Glass ff11ef73 zap4 r1 (2026-11-22 03:41 CT): soak FAIL-FAST at 15 s.
// HDMI gap start 14.436 duration 1.264 s at a Vevo title change (Go-Go's →
// Bananarama, mostly black video). 0 disc_rebase / 0 pts_jump / 0 rate0_reject /
// 0 closed_short_cover; media frames kept ~30 fps. Distinct from I 0.454 s
// same-title closed-short and from I 2.767 s track-boundary.
inline constexpr int64_t kZap5TrackChangeGapDurTicks90k = 1264 * 90;
inline constexpr int64_t kZap5TrackChangeStartTicks90k = 14436 * 90;
// ~−60 dBFS on s16 (20*log10(33/32768)); matches the HDMI soak bar.
// d81e4826 used this as a *peak* gate. Glass HDMI holes have RMS 3–15
// (below 33) with peaks 59–2892 (above 33), so peak-only never armed skip.
inline constexpr int kSilentBumperPeakAbs = 33;
// Loud music on the same soak is RMS ~1191 (−28.8 dBFS). Do not save a
// hole AU (RMS 3–15, peak still ≥33) as lastEnergeticPcm_.
inline constexpr int kEnergeticPcmRmsAbs = 200;  // ~−44 dBFS
// Glass GATE-V2 6a5fbcc4 r2: mid-soak DECODER_ONLY while host-HLS AUDIO stays
// ~−27 dBFS. Longest hole 35.383 s @ 87.962; rhythmic ~5 s clusters. Silent
// bumper fired once and does not cover host-loud / HDMI-mute. Hold last
// energetic PCM across audio starve + video_stall/respawn.
inline constexpr int64_t kGateV2DecoderOnlyLong1StartTicks90k = 87962 * 90;
inline constexpr int64_t kGateV2DecoderOnlyLong1DurTicks90k = 35383 * 90;
inline constexpr int64_t kGateV2DecoderOnlyClusterStartTicks90k = 148522 * 90;
inline constexpr int64_t kGateV2DecoderOnlyClusterDurTicks90k = 332 * 90;
// Glass GATE-V2 684967fa r1: 261.411 s stuck State Farm bumper (353.589–615)
// while host-HLS ~−25 dBFS. Title-change bumper is 1.264 s; escape after 1.5 s
// of continuous in-band silence so long ad/bumper cannot park HDMI mute.
inline constexpr int64_t kGateV2StateFarmStallStartTicks90k = 353589 * 90;
inline constexpr int64_t kGateV2StateFarmStallDurTicks90k = 261411 * 90;
inline constexpr int64_t kSilentBumperEscapeTicks90k = 1500 * 90;
// Glass r2 zap01: rate=0 x3 after crypto Unable → terminal; recover once;
// zap02–06 ob=0. Weak recover (guideOpen only) is not enough.
inline constexpr int kZap5Rate0TerminalAttempt = 1;
inline constexpr int kZap5Rate0OpenTriesBeforeTerminal = 3;

inline int64_t muxInterleaveDeltaUs() {
#ifdef MUT_LIVE_ZERO_INTERLEAVE
    return 0;
#else
    return kMuxInterleaveDeltaUs;
#endif
}

// argv for -max_interleave_delta on a RESPAWN / soak child (microseconds, no unit).
inline const char* muxInterleaveDeltaArg() {
#ifdef MUT_LIVE_ZERO_INTERLEAVE
    return "0";
#else
    return "1000000";
#endif
}

// First spawn / zap (tsOffsetSec == 0): emit the ready rendition immediately so the
// first video AU beats the 3000 ms RC_PIC watchdog (glass a1c0af44: 1 s wait,
// CABAC seen, no picture, SIGKILL rc=-9). Zap first_picture must stay within
// 300 ms of B. The 5.00 s starve grid is post-stall (glass B t=664+), so the
// 1 s wait belongs on the respawn argv.
inline int64_t muxInterleaveDeltaUsFirstSpawn() {
#ifdef MUT_LIVE_FIRST_SPAWN_INTERLEAVE
    return muxInterleaveDeltaUs();
#else
    return 0;
#endif
}

inline const char* muxInterleaveDeltaArgFirstSpawn() {
#ifdef MUT_LIVE_FIRST_SPAWN_INTERLEAVE
    return muxInterleaveDeltaArg();
#else
    return "0";
#endif
}

// Bytes of decoded PCM to produce AFTER the first key video AU (post-first-key-AU,
// split-rendition live only). Mutant 0 restores the D 1.12 s HDMI hole and the
// F 1.39 s mid-soak hole. Not FPGA first_picture. After the first key, A+V FIFO
// into the ring (G's video-only pop left HDMI silent). MUT_LIVE_PACE_HOLD_AUDIO
// restores that hold and must FAIL.
inline int64_t firstSpawnPaceAudioBytes() {
#ifdef MUT_LIVE_NO_FIRST_SPAWN_PACE
    return 0;
#else
    return kFirstSpawnPaceAudioBytes;
#endif
}

// LiveRelay::start stores whether the current remux is split-rendition (two HLS
// inputs). AvSource paces only when liveRemux AND (this flag OR a two-program
// MPEG-TS OR OpenRequest.splitLiveRendition). Muxed Pluto / VOD stay 0.
inline std::atomic<bool>& splitLiveRemuxActive() {
    static std::atomic<bool> v{false};
    return v;
}

// 1-item live (Pluto Vevo): skip-next has no successor (attempt >= size).
// Retry the same item so the play job reopens remux instead of landing.
inline bool livePicWatchdogRetries(unsigned attempt, int itemCount) {
#ifdef MUT_LIVE_NO_PIC_RETRY
    (void)attempt;
    (void)itemCount;
    return false;
#else
    return itemCount == 1 && attempt > 0 && attempt < kLivePicWatchdogRetries;
#endif
}

struct FirstTuneOutcome {
    bool firstPicture = false;
    bool respawned = false;
    bool landedNoNext = false;
    int64_t extraWaitMs = 0;
};

// First spawn 1 s wait (C) vs 0 (B/D). Extra wait >300 ms fails ITER-PASS vs B.
// If the watchdog still fires, retry-same-item reopens remux (zap path).
inline FirstTuneOutcome simulateFirstTune(bool firstSpawnWait1s, bool retrySameItem) {
    FirstTuneOutcome o;
    o.extraWaitMs = firstSpawnWait1s ? 1000 : 0;
    if (o.extraWaitMs <= kFirstPicVsBBudgetMs) {
        o.firstPicture = true;
        return o;
    }
    if (retrySameItem) {
        o.respawned = true;
        o.firstPicture = true;
        return o;
    }
    o.landedNoNext = true;
    return o;
}

// Slow HLS / reconnect: first AU misses 3000 ms even with interleave 0.
// C landed NO NEXT. D retries the same live item.
inline FirstTuneOutcome simulateFirstTuneSlowOpen(bool retrySameItem) {
    FirstTuneOutcome o;
    o.extraWaitMs = kFirstPicWatchdogMs;
    if (retrySameItem) {
        o.respawned = true;
        o.firstPicture = true;
        return o;
    }
    o.landedNoNext = true;
    return o;
}

enum class DiscKind { None, MediaSequence, DiscontinuityTag, AdSplice, PtsJump };

inline const char* discKindName(DiscKind k) {
    switch (k) {
    case DiscKind::None: return "none";
    case DiscKind::MediaSequence: return "media_sequence";
    case DiscKind::DiscontinuityTag: return "discontinuity";
    case DiscKind::AdSplice: return "ad_splice";
    case DiscKind::PtsJump: return "pts_jump";
    }
    return "?";
}

enum class AdPath { PlayAllSmoothSplice, AdBreakCard };

inline AdPath liveAdPath() {
#ifdef MUT_LIVE_NO_PLAY_ALL
    return AdPath::AdBreakCard;
#else
    return AdPath::PlayAllSmoothSplice;
#endif
}

// Play-all: no snap-back to the ad-break card mid-ad. Intentional cards stay
// (ffmpeg actually respawning / first-byte fail / operator hold).
inline bool playAllAllowsAdCard(bool intentional) {
    if (liveAdPath() == AdPath::AdBreakCard)
        return true;
    return intentional;
}

// Frame-stall while bytes still flow is a splice hiccup, not a dropped ad pod.
inline bool playAllShowsFrameStallCard() {
    return liveAdPath() == AdPath::AdBreakCard;
}

// Glass 47abfbbb: RelaySilence painted the (black) ad-break card ~47 s in while
// play-all was supposed to keep ads on. A 15 s byte gap is a splice/stall, not
// a dropped pod. Respawn (ffmpeg actually dead) stays intentional.
inline bool playAllShowsRelaySilenceCard() {
    return liveAdPath() == AdPath::AdBreakCard;
}

inline std::string trimHlsLine(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    return line.substr(i);
}

inline bool hlsLineIsDiscontinuity(const std::string& raw) {
    const std::string line = trimHlsLine(raw);
    return line == "#EXT-X-DISCONTINUITY" || line.rfind("#EXT-X-DISCONTINUITY:", 0) == 0;
}

inline bool hlsLineIsAdSplice(const std::string& raw) {
    const std::string line = trimHlsLine(raw);
    if (line.rfind("#EXT-X-CUE-OUT", 0) == 0 || line.rfind("#EXT-X-CUE-IN", 0) == 0)
        return true;
    if (line.rfind("#EXT-X-DISCONTINUITY-SEQUENCE", 0) == 0)
        return false;
    if (line.find("SCTE35") != std::string::npos)
        return true;
    if (line.rfind("#EXT-X-DATERANGE", 0) == 0 &&
        (line.find("scte") != std::string::npos || line.find("SCTE") != std::string::npos ||
         line.find("interstitial") != std::string::npos || line.find("PLUTO-AD") != std::string::npos))
        return true;
    return false;
}

inline bool hlsLineIsMediaSequence(const std::string& raw, int64_t& seq) {
    const std::string line = trimHlsLine(raw);
    const char k[] = "#EXT-X-MEDIA-SEQUENCE:";
    if (line.rfind(k, 0) != 0)
        return false;
    seq = static_cast<int64_t>(std::strtoll(line.c_str() + sizeof(k) - 1, nullptr, 10));
    return true;
}

// Live sliding window advances by 1. A skip, rewind, or reset is a discontinuity.
inline bool mediaSequenceJumped(int64_t prev, int64_t next) {
    if (prev < 0 || next < 0)
        return false;
    return next != prev && next != prev + 1;
}

inline DiscKind playlistEvent(const std::string& line, int64_t& mediaSeq) {
    if (hlsLineIsDiscontinuity(line))
        return DiscKind::DiscontinuityTag;
    if (hlsLineIsAdSplice(line))
        return DiscKind::AdSplice;
    int64_t seq = -1;
    if (hlsLineIsMediaSequence(line, seq)) {
        const int64_t prev = mediaSeq;
        mediaSeq = seq;
        if (mediaSequenceJumped(prev, seq))
            return DiscKind::MediaSequence;
    }
    return DiscKind::None;
}

// 33-bit MPEG-TS PTS wrap.
inline int64_t ptsDelta90k(int64_t now, int64_t last) {
    int64_t d = now - last;
    const int64_t wrap = int64_t{1} << 33;
    if (d > wrap / 2)
        d -= wrap;
    if (d < -wrap / 2)
        d += wrap;
    return d;
}

inline bool ptsJumpIsDiscontinuity(int64_t now, int64_t last) {
    if (last == kNoPts90k || now == kNoPts90k)
        return false;
    const int64_t d = ptsDelta90k(now, last);
    if (d > kDiscJumpTicks90k)
        return true;
    if (d < -kDiscJumpTicks90k / 4)  // backward more than 250 ms: new clip, not B-reorder
        return true;
    return false;
}

struct RebaseState {
    int64_t originRaw = kNoPts90k;  // first raw PTS of the current clip (tune origin)
    int64_t shift = 0;              // emit = raw + shift (monotonic across splices)
    int64_t lastRaw = kNoPts90k;
    int64_t lastRawV = kNoPts90k;  // per-stream: a video burst is not an audio pts_jump
    int64_t lastRawA = kNoPts90k;
    int64_t lastEmit = kNoPts90k;
    int64_t audioClock90k = 0;  // PCM master clock; a tune starts this at 0
    int splices = 0;
};

// Tune-class rebase: new origin = this PTS, HDMI/audio clock shares the new origin.
// Presentation stays smooth: emitted PTS continue from lastEmit (the splice instant).
inline void rebaseLikeTune(RebaseState& s, int64_t rawPts) {
#ifdef MUT_LIVE_NO_DISC_REBASE
    (void)s;
    (void)rawPts;
    return;
#else
    if (s.lastEmit != kNoPts90k)
        s.shift = s.lastEmit - rawPts;
    else
        s.shift = 0;
    s.originRaw = rawPts;
    s.audioClock90k = (s.lastEmit != kNoPts90k) ? s.lastEmit : 0;
    ++s.splices;
#endif
}

inline int64_t emitPts90k(const RebaseState& s, int64_t rawPts) {
#ifdef MUT_LIVE_NO_DISC_REBASE
    (void)s;
    return rawPts;
#else
    if (rawPts == kNoPts90k)
        return rawPts;
    return rawPts + s.shift;
#endif
}

// Bound the PTS that actually goes to the HDMI ring. A non-monotonic or oversized
// emit (shared A/V RebaseState extra-rebase, or a respawn-sized raw jump that
// escaped the splice path) is what dropped the 47abfbbb client after disc rebase.
inline int64_t clampHdmiEmit(int64_t lastEmit, int64_t emit) {
#ifdef MUT_LIVE_NO_EMIT_CLAMP
    (void)lastEmit;
    return emit;
#else
    if (lastEmit == kNoPts90k || emit == kNoPts90k)
        return emit;
    const int64_t d = emit - lastEmit;
    if (d > kMaxHdmiEmitJumpTicks90k)
        return lastEmit + kFrameTicks90k;
    if (d < -kReorderTicks90k)
        return lastEmit;
    return emit;
#endif
}

// Feed one A or V packet. playlist is a parsed HLS event (or None). Returns the
// kind that fired a rebase, or None.
// audio=true compares against lastRawA so a 5 s video burst (one Pluto audio
// EXTINF, glass 2932ecf8 extra-rebase at out_q=237) is not a backward audio jump.
inline DiscKind notePacket(RebaseState& s, int64_t rawPts, DiscKind playlist = DiscKind::None,
                           bool audio = false) {
    int64_t& lastRawSt = audio ? s.lastRawA : s.lastRawV;
    if (rawPts == kNoPts90k) {
        if (playlist != DiscKind::None && playlist != DiscKind::PtsJump) {
#ifndef MUT_LIVE_NO_DISC_REBASE
            // Playlist event with no PTS yet: remember it for the next packet.
            if (s.lastRaw != kNoPts90k)
                rebaseLikeTune(s, s.lastRaw);
#endif
            return playlist;
        }
        return DiscKind::None;
    }
    DiscKind kind = DiscKind::None;
    if (s.lastRaw == kNoPts90k) {
        s.originRaw = rawPts;
        s.shift = 0;
        s.lastRaw = rawPts;
        lastRawSt = rawPts;
        s.lastEmit = emitPts90k(s, rawPts);
        s.audioClock90k = s.lastEmit;
        if (playlist != DiscKind::None && playlist != DiscKind::PtsJump)
            return playlist;  // first packet of a session that opened on a tag: no rebase yet
        return DiscKind::None;
    }
    if (lastRawSt == kNoPts90k) {
        // The other stream already set origin/shift. Start tracking this one;
        // a late audio burst after a video-only stretch is not a new clip.
        lastRawSt = rawPts;
        s.lastRaw = rawPts;
        s.lastEmit = clampHdmiEmit(s.lastEmit, emitPts90k(s, rawPts));
        if (playlist != DiscKind::None && playlist != DiscKind::PtsJump)
            return playlist;
        return DiscKind::None;
    }
    const bool jumped = ptsJumpIsDiscontinuity(rawPts, lastRawSt);
    const bool tagged = playlist != DiscKind::None && playlist != DiscKind::PtsJump;
    if (jumped || tagged) {
        kind = tagged ? playlist : DiscKind::PtsJump;
        rebaseLikeTune(s, rawPts);
    }
    lastRawSt = rawPts;
    s.lastRaw = rawPts;
    s.lastEmit = clampHdmiEmit(s.lastEmit, emitPts90k(s, rawPts));
    return kind;
}

inline void notePcmBytes(RebaseState& s, size_t bytes) {
    const int64_t frames = static_cast<int64_t>(bytes / 4);
    s.audioClock90k += frames * kPcmBytesPer90kTickNum / kPcmBytesPer90kTickDen;
}

inline int64_t avDrift90k(const RebaseState& s) {
    if (s.lastEmit == kNoPts90k)
        return 0;
    return s.audioClock90k - s.lastEmit;
}

// HDMI silent ticks while video emit ran ahead of PCM (ring underrun).
inline int64_t hdmiStarveTicks90k(const RebaseState& s) {
    const int64_t d = avDrift90k(s);
    return d < 0 ? -d : 0;
}

struct SegGap {
    int64_t startTicks = 0;
    int64_t durTicks = 0;
};

// Model of split-HLS remux over nSeg Pluto audio EXTINF periods.
// Video is continuous. Audio fetch lag starts at 0.8 s (glass gap 4 after the
// first stall). With max_interleave_delta 0 the muxer emits video immediately,
// HDMI starves for `lag`, and lag grows ~110 ms/seg (glass 0.81 → 4.86 s).
// A 1 s wait paces video to audio so lag does not accumulate and starve is 0.
inline std::vector<SegGap> simulateSplitSegGaps(int nSeg, bool zeroInterleave) {
    std::vector<SegGap> gaps;
    if (nSeg <= 0)
        return gaps;
    const int64_t seg = kPlutoAudioSegTicks90k;
    const int64_t waitBound = zeroInterleave ? 0 : (kMuxInterleaveDeltaUs * 90 / 1000);
    int64_t lag = kLongGapTicks90k;          // 0.8 s first long gap
    const int64_t lagGrowth = 110 * 90;      // ~110 ms/seg (glass cluster)
    int64_t t = 0;
    for (int i = 0; i < nSeg; ++i) {
        const int64_t wait = lag < waitBound ? lag : waitBound;
        const int64_t starve = lag > wait ? lag - wait : 0;
        if (starve > 0) {
            SegGap g;
            g.startTicks = t;
            g.durTicks = starve;
            gaps.push_back(g);
        }
        if (zeroInterleave)
            lag += lagGrowth;
        else
            lag = kLongGapTicks90k;
        t += seg;
    }
    return gaps;
}

// First-spawn soak (interleave argv 0). Glass D hole at remux t=70 s is one
// audio-EXTINF fetch, not the accumulating post-stall 5 s grid. A 1.5 s
// post-first-key-AU cushion covers 1.123 s; mutant 0 leaves a ≥0.2 s HDMI gap.
inline std::vector<SegGap> simulateFirstSpawnSoakGaps(bool paceAfterFirstPicture) {
    std::vector<SegGap> gaps;
    const int64_t hole = kFirstSpawnGapDurTicks90k;
    const int64_t cushion =
        paceAfterFirstPicture ? (kFirstSpawnPaceUs * 90 / 1000) : 0;
    const int64_t starve = hole > cushion ? hole - cushion : 0;
    if (starve > 0) {
        SegGap g;
        g.startTicks = kFirstSpawnGapRemuxTicks90k;
        g.durTicks = starve;
        gaps.push_back(g);
    }
    return gaps;
}

// Glass G: first-spawn logged audio_bytes≥need while HDMI was idle-floor silent
// from t=0. The paced PCM reaches the consumer only if (1) the fill actually
// produced ≥1.5 s of non-zero PCM and (2) video lead before the first audio
// packet is inside the 0.2 s soak gate. A 1.5 s video-only watermark (G) is
// AUD_LATE / −81.9 dB even when the log says 290816. Zero PCM (bad SWR) also
// fails. Mutant hold makes the libav fixture miss this gate.
// After the first key AU (first_picture already allowed), further video may
// not lead the last emitted or queued audio by the 0.2 s soak gate. Mutant
// restores H's 3.42 s startup hole (popReadyByPts dumps video-only).
inline bool videoLeadAllowed(int64_t videoPts90k, int64_t audioPts90k) {
#ifdef MUT_LIVE_NO_VIDEO_LEAD_CAP
    (void)videoPts90k;
    (void)audioPts90k;
    return true;
#else
    if (audioPts90k == kNoPts90k)
        return false;
    if (videoPts90k == kNoPts90k)
        return true;
    return (videoPts90k - audioPts90k) < kSoakGapGateTicks90k;
#endif
}

// Glass H r3 mid-soak: a pts_jump rebase that flushes the audio decoder / SWR
// drops residual + queued PCM while the first-spawn child still has
// max_interleave_delta 0, so video keeps moving (vfps≈31) and HDMI starves
// 2.64 s. Keeping PCM and holding video until the next audio AU leaves no
// ≥0.2 s hole. Mutant restores the flush.
inline bool keepPcmOnPtsJump() {
#ifdef MUT_LIVE_FLUSH_PCM_ON_DISC
    return false;
#else
    return true;
#endif
}

// H r3 soak: startup 3.42 s if video lead is uncapped; mid-soak 2.64 s if a
// pts_jump flush drops the PCM reserve. Both must be 0 with the fix.
inline std::vector<SegGap> simulateHGlassSoakGaps(bool capVideoLead, bool keepPcmOnDisc) {
    std::vector<SegGap> gaps;
    if (!capVideoLead) {
        SegGap g;
        g.startTicks = 0;
        g.durTicks = kHStartupGapDurTicks90k;
        gaps.push_back(g);
    }
    if (!keepPcmOnDisc) {
        SegGap g;
        g.startTicks = kHMidSoakEmitPts90k;
        g.durTicks = kHMidSoakGapDurTicks90k;
        gaps.push_back(g);
    }
    return gaps;
}

// Glass 4beb780b r1: after the first audible AU, video leading last emitted audio
// by the 0.2 s soak gate is a track/ad splice that never logged pts_jump. AAC/SWR
// must reopen so PCM resumes. First-spawn (no audio yet) is the lead-cap path.
inline bool audioGapNeedsRecover(int64_t videoPts90k, int64_t lastAudioPts90k) {
#ifdef MUT_LIVE_NO_AUDIO_GAP_RECOVER
    (void)videoPts90k;
    (void)lastAudioPts90k;
    return false;
#else
    if (lastAudioPts90k == kNoPts90k || videoPts90k == kNoPts90k)
        return false;
    return (videoPts90k - lastAudioPts90k) >= kSoakGapGateTicks90k;
#endif
}

inline std::vector<SegGap> simulateITrackBoundaryGaps(bool recoverAudioGap) {
    std::vector<SegGap> gaps;
    if (!recoverAudioGap) {
        SegGap g;
        g.startTicks = kITrackBoundaryStartTicks90k;
        g.durTicks = kITrackBoundaryGapDurTicks90k;
        gaps.push_back(g);
    }
    return gaps;
}

// Glass 4beb780b r1 zap02: playjob 1066, no first_picture, HDMI idle floor; zaps
// 03–06 never confirm tune until a daemon restart. Two cooperating bugs: the
// playjob killed /yt-live.ts while the previous session still read it, and live
// remux EOF was a VOD premature seek (duration ~INT64_MAX) that SourceError'd.
struct RetuneOutcome {
    bool firstPicture = false;
    bool nextTuneAccepted = false;
};

inline bool stopPlayerBeforeRelayKill() {
#ifdef MUT_LIVE_RELAY_KILL_WHILE_READING
    return false;
#else
    return true;
#endif
}

inline bool liveRemuxSkipsPrematureResume() {
#ifdef MUT_LIVE_PREMATURE_RESUME
    return false;
#else
    return true;
#endif
}

inline bool liveRemuxUrl(const std::string& url) {
    return url.find("/yt-live.ts") != std::string::npos;
}

inline RetuneOutcome simulateZapReturnTune(bool stopPlayerFirst, bool skipLivePremature) {
    RetuneOutcome o;
    if (!stopPlayerFirst || !skipLivePremature)
        return o;
    o.firstPicture = true;
    o.nextTuneAccepted = true;
    return o;
}

// Glass dbd11006: I's stop-before-kill + live-EOF skip make zap01–03 clean (no
// premature / SourceError). On attempt 4 the new remux demux opens with
// codecpar.sample_rate=0 (log: codec=aac rate=0 decoder=1) and never reaches
// prefetch / play_started / first_picture; HDMI stays −81.9; later ENTERs stay
// wedged. simulateZapReturnTune(true,true) is one-shot and misses this class.
// rejectZeroRateStall is the reviewer next-I product hook (retry/wait/abort before
// prefetch when rate==0). dbd11006 / 80c3867e leave it false → host suite RED.
inline bool liveRemuxRejectsZeroRateStall() {
#ifdef MUT_LIVE_ACCEPT_RATE0_DEMUX_STALL
    return false;
#else
    // Product (reviewer/1066-i-zap4-fix-1122): reject demux open when
    // codecpar.sample_rate==0 and no settled adec rate; pluto open retries.
    return true;
#endif
}

// fix6 (v3 1066-starve, cursor-box 942696de): a Vevo split relay can publish
// the audio rendition's first PES ~4.5 s after video (glass 30708c1a tune:
// rate=0 after 448-652 KB). Rejecting at open burns all three playjob opens
// (PREFETCH FAILED, first_picture only at gen=32); 7e54cf8e, which predates
// this reject, opens and plays (942696de 14/14). With a split rendition and an
// opened AAC decoder, defer the reject: if no sample rate settles within the
// budget the read fails with the same 'live remux demux sample_rate=0'
// (a retriable prefetch fail), so the dbd11006 zap4 stall still retries.
// MUT_LIVE_REJECT_LATE_AUDIO_RENDITION: reject at open (starve tune FAILs).
inline constexpr int64_t kLiveLateAudioSettleMs = 6000;
inline bool liveRemuxDefersLateAudioReject() {
#ifdef MUT_LIVE_REJECT_LATE_AUDIO_RENDITION
    return false;
#else
    return true;
#endif
}

inline RetuneOutcome simulateIZapNReturnTune(int attempt, bool stopPlayerFirst,
                                             bool skipLivePremature, int demuxSampleRate,
                                             bool rejectZeroRateStall) {
    RetuneOutcome o;
    if (!stopPlayerFirst || !skipLivePremature)
        return o;
    if (attempt < kIZap4WedgeAttempt) {
        o.firstPicture = true;
        o.nextTuneAccepted = true;
        return o;
    }
    if (demuxSampleRate <= 0 && !rejectZeroRateStall)
        return o;
    o.firstPicture = true;
    o.nextTuneAccepted = true;
    return o;
}

// Glass dbd11006 soak: closed 0.454 s @ 22.241 with zero audio_gap_recover and
// unchanged title. I track-boundary recover covers 2.767 s @ 23.234 only.
// covered=false reproduces the glass hole; reviewer next I flips the hook.
inline bool closedShortSoakGapCovered() {
#ifdef MUT_LIVE_NO_CLOSED_SHORT_GAP_COVER
    return false;
#else
    // Product: mid-soak PCM floor + gap recover covers glass 0.454s@22.241.
    return true;
#endif
}

// Stereo s16 @48 kHz floor that covers glass closed-short 0.454 s with margin.
// Mutant leaves floor at 0 so the hole remains in the model/product path.
inline int64_t closedShortSoakFloorBytes() {
#ifdef MUT_LIVE_NO_CLOSED_SHORT_GAP_COVER
    return 0;
#else
    return closedShortSoakGapCovered() ? (48000 * 4 / 2) : 0;  // 0.5 s
#endif
}

inline std::vector<SegGap> simulateIClosedShortSoakGaps(bool covered) {
    std::vector<SegGap> gaps;
    if (!covered) {
        SegGap g;
        g.startTicks = kIClosedShortGapStartTicks90k;
        g.durTicks = kIClosedShortGapDurTicks90k;
        gaps.push_back(g);
    }
    return gaps;
}

// Glass ff11ef73: in-band silent AAC at a title change. PTS/rate/queue-floor
// models stay empty (video living, 0 disc). Skipping silent PCM lets remux
// look-ahead splice the next song without a ≥0.2 s HDMI hole.
inline int pcmPeakAbsS16(const uint8_t* data, size_t n) {
    int peak = 0;
    if (!data || n < 2)
        return 0;
    const size_t samples = n / 2;
    const auto* s = reinterpret_cast<const int16_t*>(data);
    for (size_t i = 0; i < samples; ++i) {
        const int v = s[i];
        const int a = v < 0 ? -v : v;
        if (a > peak)
            peak = a;
    }
    return peak;
}

// HDMI silencedetect is RMS −60 dBFS, not peak. Glass d81e4826 h2 is
// peak=2892 / RMS=15.4 — peak-only skip misses it.
inline int pcmRmsAbsS16(const uint8_t* data, size_t n) {
    if (!data || n < 2)
        return 0;
    const size_t samples = n / 2;
    const auto* s = reinterpret_cast<const int16_t*>(data);
    double ss = 0;
    for (size_t i = 0; i < samples; ++i) {
        const double v = static_cast<double>(s[i]);
        ss += v * v;
    }
    return static_cast<int>(std::sqrt(ss / static_cast<double>(samples)) + 0.5);
}

inline bool inBandSilentBumperSkipped() {
#ifdef MUT_LIVE_NO_INBAND_SILENT_BUMPER_SKIP
    return false;
#else
    return true;
#endif
}

// Peak-only (d81e4826 / zap5 digital-zero). Glass latch soak holes do not
// hit this: skipInBandSilentPcm(60) is false.
inline bool skipInBandSilentPcm(int peakAbs) {
    if (!inBandSilentBumperSkipped())
        return false;
    return peakAbs < kSilentBumperPeakAbs;
}

inline bool skipInBandSilentUsesHdmiRms() {
#ifdef MUT_LIVE_SILENT_BUMPER_PEAK_ONLY
    return false;
#else
    return inBandSilentBumperSkipped();
#endif
}

// HDMI-class quiet: RMS < 33 (−60 dBFS) OR digital-zero peak. Mutant
// MUT_LIVE_SILENT_BUMPER_PEAK_ONLY restores d81e4826 (peak only).
inline bool skipInBandSilentPcm(int peakAbs, int rmsAbs) {
    if (!inBandSilentBumperSkipped())
        return false;
#ifdef MUT_LIVE_SILENT_BUMPER_PEAK_ONLY
    (void)rmsAbs;
    return peakAbs < kSilentBumperPeakAbs;
#else
    return peakAbs < kSilentBumperPeakAbs || rmsAbs < kSilentBumperPeakAbs;
#endif
}

inline bool pcmIsEnergetic(int rmsAbs) {
    return rmsAbs >= kEnergeticPcmRmsAbs;
}

inline std::vector<SegGap> simulateZap5TrackChangeGaps(bool skipSilentBumper) {
    std::vector<SegGap> gaps;
    if (!skipSilentBumper) {
        SegGap g;
        g.startTicks = kZap5TrackChangeStartTicks90k;
        g.durTicks = kZap5TrackChangeGapDurTicks90k;
        gaps.push_back(g);
    }
    return gaps;
}

// Glass ff11ef73 zap02: crypto Unable to open, remux sent=0, prefetch timeout
// on 1-item live → PREFETCH TIMEOUT: NO NEXT terminal=1; later ENTERs post no
// playjob until daemon restart. Retry remux before NO NEXT; restore guide so
// the next SELECT is accepted even if retries exhaust.
inline bool liveRemuxRetriesCryptoPrefetchStall() {
#ifdef MUT_LIVE_NO_CRYPTO_PREFETCH_RETRY
    return false;
#else
    return true;
#endif
}

inline bool liveRemuxRecoversInputsAfterTerminalPrefetch() {
#ifdef MUT_LIVE_NO_CRYPTO_PREFETCH_RETRY
    return false;
#else
    return liveRemuxRetriesCryptoPrefetchStall();
#endif
}

inline RetuneOutcome simulateZap5ReturnTune(int attempt, bool retryCryptoPrefetch,
                                            bool recoverInputsAfterTerminal) {
    RetuneOutcome o;
    if (attempt < kZap5ReturnWedgeAttempt) {
        o.firstPicture = true;
        o.nextTuneAccepted = true;
        return o;
    }
    if (retryCryptoPrefetch) {
        o.firstPicture = true;
        o.nextTuneAccepted = true;
        return o;
    }
    o.firstPicture = false;
    o.nextTuneAccepted = recoverInputsAfterTerminal;
    return o;
}

// Hold last energetic PCM so HDMI cannot sit at the floor while host-HLS is
// loud across video_stall/respawn + rhythmic audio starve (GATE-V2 r2).
inline bool holdEnergeticPcmAcrossAudioStarve() {
#ifdef MUT_LIVE_NO_ENERGETIC_PCM_HOLD
    return false;
#else
    return true;
#endif
}

// Glass r2: 65 DECODER_ONLY holes with simultaneous host energy. Without the
// energetic hold, long1 + a 5 s cluster hole remain ≥0.2 s.
inline std::vector<SegGap> simulateGateV2DecoderOnlyHoles(bool holdEnergetic) {
    std::vector<SegGap> gaps;
    if (!holdEnergetic) {
        SegGap long1;
        long1.startTicks = kGateV2DecoderOnlyLong1StartTicks90k;
        long1.durTicks = kGateV2DecoderOnlyLong1DurTicks90k;
        gaps.push_back(long1);
        SegGap cluster;
        cluster.startTicks = kGateV2DecoderOnlyClusterStartTicks90k;
        cluster.durTicks = kGateV2DecoderOnlyClusterDurTicks90k;
        gaps.push_back(cluster);
    }
    return gaps;
}

// Escape continuous in-band silent bumper after the title-change window so a
// stuck State Farm / ad bumper cannot hold video forever (684967fa r1).
inline bool escapeSilentBumperAfterSoakGate() {
#ifdef MUT_LIVE_NO_SILENT_BUMPER_ESCAPE
    return false;
#else
    return true;
#endif
}

inline bool silentBumperHoldExpired(int64_t silentHeldTicks90k) {
    if (!escapeSilentBumperAfterSoakGate())
        return false;
    return silentHeldTicks90k >= kSilentBumperEscapeTicks90k;
}

// One-shot energetic fill covers a brief starve but not a 261 s stuck bumper
// (glass logged fill once then HDMI stayed −81.9 while host stayed loud).
inline bool energeticPcmFillRepeatsAcrossStarve() {
#ifdef MUT_LIVE_ONE_SHOT_ENERGETIC_FILL
    return false;
#else
    return holdEnergeticPcmAcrossAudioStarve();
#endif
}

// GBE residual risk: after escape, the next silent AU re-armed bumper for
// another 1.5 s drop cycle. Latch escape until a non-silent AU arrives.
inline bool silentBumperEscapeLatchesUntilEnergetic() {
#ifdef MUT_LIVE_NO_SILENT_BUMPER_ESCAPE_LATCH
    return false;
#else
    return escapeSilentBumperAfterSoakGate();
#endif
}

// Glass 684967fa r1: 261.411 s DECODER_ONLY State Farm stall. Escape + repeating
// energetic fill leave zero ≥0.2 s holes; one-shot fill / no escape leave it.
inline std::vector<SegGap> simulateGateV2StateFarmStall(bool escapeBumper,
                                                        bool repeatEnergeticFill) {
    std::vector<SegGap> gaps;
    if (!escapeBumper || !repeatEnergeticFill) {
        SegGap g;
        g.startTicks = kGateV2StateFarmStallStartTicks90k;
        g.durTicks = kGateV2StateFarmStallDurTicks90k;
        gaps.push_back(g);
    }
    return gaps;
}

// Without latch, continuous silence re-arms bumper → periodic ≥0.2 s holes
// (~1.5 s drop / short fill). Latch until energetic clears that class.
inline constexpr int64_t kGateV2RearmCycleHoleDurTicks90k = 1500 * 90;
inline std::vector<SegGap> simulateGateV2SilentBumperRearmCycle(bool latchEscape) {
    std::vector<SegGap> gaps;
    if (!latchEscape) {
        SegGap g;
        g.startTicks = kGateV2StateFarmStallStartTicks90k + kSilentBumperEscapeTicks90k;
        g.durTicks = kGateV2RearmCycleHoleDurTicks90k;
        gaps.push_back(g);
    }
    return gaps;
}

// Glass d81e4826 GATE-V2 latch soak: skip peak=0 once after stall, escape 0.
// HDMI-quiet AUs (RMS 3–15, peak 59–2892) never armed skip. Substitute last
// energetic PCM on the first quiet AU (do not wait 1.5 s).
inline bool silentBumperEscapesImmediatelyOnQuiet() {
#ifdef MUT_LIVE_SILENT_BUMPER_PEAK_ONLY
    return false;
#else
    return escapeSilentBumperAfterSoakGate();
#endif
}

struct GlassD81eHdmiHole {
    int peak = 0;
    int rms = 0;
    int64_t startTicks = 0;
    int64_t durTicks = 0;
};

// Measured from ev/C-1066-soak.wav (d81e4826 r1). Representative of the
// 47 DECODER_ONLY class: 5 s-grid quiet PCM + 215 s remux-dead floor.
inline constexpr GlassD81eHdmiHole kGlassD81eHdmiHoles[] = {
    {60, 6, 19225 * 90, 550 * 90},       // h1
    {2892, 15, 24213 * 90, 1084 * 90},   // h2 peak well above 33
    {1958, 9, 44229 * 90, 5537 * 90},    // h6 ~5.5 s cluster end
    {59, 3, 219227 * 90, 5013 * 90},     // h31 ~5.0 s
    {62, 3, 399533 * 90, 215466 * 90},   // h47 215 s (relay dead / floor)
};

inline bool glassD81eHoleArmsSkip(const GlassD81eHdmiHole& h) {
    return skipInBandSilentPcm(h.peak, h.rms);
}

// d81e peak-only: every measured hole stays ≥0.2 s. RMS skip + immediate
// energetic fill covers the class (escape logs on the first quiet AU).
inline std::vector<SegGap> simulateGlassD81eQuietPcmHoles(bool rmsSkip,
                                                         bool immediateFill) {
    std::vector<SegGap> gaps;
    for (const auto& h : kGlassD81eHdmiHoles) {
        const bool hit = rmsSkip ? skipInBandSilentPcm(h.peak, h.rms)
                                 : skipInBandSilentPcm(h.peak);
        if (!hit || !immediateFill) {
            SegGap g;
            g.startTicks = h.startTicks;
            g.durTicks = h.durTicks;
            gaps.push_back(g);
        }
    }
    return gaps;
}

// s16le stereo @ 48 kHz → 90 kHz ticks. Inverse of notePcmBytes.
inline int64_t pcmTicksForBytes(size_t bytes) {
    const int64_t frames = static_cast<int64_t>(bytes / 4);
    return frames * kPcmBytesPer90kTickNum / kPcmBytesPer90kTickDen;
}

inline size_t pcmBytesForTicks(int64_t ticks90k) {
    if (ticks90k <= 0)
        return 0;
    const int64_t frames = ticks90k * kPcmBytesPer90kTickDen / kPcmBytesPer90kTickNum;
    return static_cast<size_t>(frames) * 4u;
}

// Glass 9e6d1814: RMS skip fired once (peak=38 rms=7) and did not touch the
// 73 DECODER_ONLY holes. Those interiors are digital floor (peak 11–17),
// bordered by loud PCM (RMS ~1000). The hole is absent samples, not a quiet
// AU. A 4096-byte energetic fill stamped at the video PTS leaves the gap.
inline bool extinfSampleClockCover() {
#ifdef MUT_LIVE_SHORT_ENERGETIC_STAMP
    return false;
#else
    return true;
#endif
}

struct ExtinfCoverPlan {
    int64_t pts = kNoPts90k;
    int64_t fillTicks = 0;
    bool leavesHole = true;
};

// Cover `targetPts - pcmCoveredEnd` with energetic PCM placed AT the sample
// clock. One pop covers up to 200 ms; the caller repeats until the lead is
// inside the 0.2 s gate. The 9e6d1814 stamp puts 21 ms at the video PTS and
// leaves the rest of the EXTINF as HDMI silence.
inline ExtinfCoverPlan planExtinfSampleCover(int64_t targetPts, int64_t pcmCoveredEnd) {
    ExtinfCoverPlan p;
    if (targetPts == kNoPts90k || pcmCoveredEnd == kNoPts90k)
        return p;
    const int64_t lead = targetPts - pcmCoveredEnd;
    if (lead < kSoakGapGateTicks90k) {
        p.leavesHole = false;
        return p;
    }
    if (!extinfSampleClockCover()) {
        p.pts = targetPts;
        p.fillTicks = pcmTicksForBytes(4096);
        p.leavesHole = (lead - p.fillTicks) >= kSoakGapGateTicks90k;
        return p;
    }
    const int64_t chunk = 200 * 90;
    p.fillTicks = lead < chunk ? lead : chunk;
    p.pts = pcmCoveredEnd;
    p.leavesHole = false;
    return p;
}

// dfc960ee r1: planExtinfSampleCover is only reached when a packet's PTS lead
// is already visible. The HDMI holes are the wall-clock wait inside
// av_read_frame, and that read's interrupt poll must arm the fill deadline.
// 100 ms is under the 0.2 s soak gate. The HTTP wait polls onInterrupt at
// most every 100 ms, so a slice that starts just under this quiet stays
// under the gate once the previous 200 ms cover is in the FIFO.
inline constexpr int64_t kExtinfCoverWakeQuietMs = 100;

inline int64_t extinfCoverWakeQuietMs() { return kExtinfCoverWakeQuietMs; }

inline bool extinfCoverWakesBlockedRead() {
#ifdef MUT_LIVE_EXTINF_COVER_NO_READ_WAKE
    return false;
#else
    return extinfSampleClockCover();
#endif
}

// av_read_frame stores a non-zero interrupt in AVIOContext.error and the next
// read returns it without calling the callback. The cover wake must clear
// that sticky AVERROR_EXIT or the session ends on the packet after the cover.
// Glass d0045771 r1 (6 holes 73–95 s, video live): the wall-clock wake fired on
// normal >=100 ms relay burst gaps, each emitted 200 ms of PCM, and the late-PES
// snap restamped real audio onto that inflated sample clock — audio PTS ran tens
// of seconds ahead of video (host burst e2e: +43.7 s in 10 s of media). A wall
// cover is only right when HDMI is actually underrunning: PCM emitted since the
// paced start is behind wall time. MUT_LIVE_COVER_IGNORES_WALL_DEBT is d0045771.
inline constexpr int64_t kCoverUnderrunSlackMs = 50;
inline constexpr int64_t kPcmBytesPerSec = 48000 * 4;
inline bool coverGatedOnWallDebt() {
#ifdef MUT_LIVE_COVER_IGNORES_WALL_DEBT
    return false;
#else
    return true;
#endif
}
// emittedPcmBytes since wallAnchorMs (first post-pace audio emit).
inline bool hdmiAudioUnderrun(int64_t emittedPcmBytes, int64_t wallElapsedMs) {
    const int64_t emittedMs = emittedPcmBytes * 1000 / kPcmBytesPerSec;
    return emittedMs - wallElapsedMs < kCoverUnderrunSlackMs;
}

// Glass 40c9feb4 r1: through the 16:21:22-16:21:51 crypto video_stall the
// frames counter sat at 5746 while MEMSTAT out_q climbed 1 -> 35 -> 85 with no
// source bytes. That queue was wall-clock PCM cover. The FPGA audio is slaved
// to the video clock (DECODER_ONLY holes with the picture frozen), so the cover
// did not reach HDMI. It piled up, and after the respawn audio ran ~30 s ahead
// of video (host O: 9.0 s of cover for a 9.1 s outage, A lead +10.3 s).
// Wall-clock cover (and the read wake that drives it) now stops after this much
// PCM without a real video AU. A video AU resets it, so audio-only EXTINF holes
// with live video (d0045771 / e875 shapes) are covered as before.
// MUT_LIVE_40C9_COVER_OUTRUNS_VIDEO restores 40c9feb4.
inline constexpr int64_t kWallCoverNoVideoCapMs = 3000;
inline bool extinfWakeKeepsVideoFrames() {
#ifdef MUT_LIVE_40C9_COVER_OUTRUNS_VIDEO
    return false;
#else
    return true;
#endif
}
// Relay side of the same glass: a reconnect GET gets the 200 header at accept
// instead of with the new child's first byte. MUT_LIVE_RECONNECT_HEADER_ON_FIRST_BYTE
// restores 40c9feb4 (17 reconnects die in the header read, frames stuck at 6629).
inline bool liveReconnectHeaderAtAccept() {
#ifdef MUT_LIVE_RECONNECT_HEADER_ON_FIRST_BYTE
    return false;
#else
    return true;
#endif
}
// Glass ef980632 r1 (19:24:32 CT, 5 DECODER_ONLY holes from 120.94 s, video
// live, host-ref clean): the Pluto live edge delivers ~2.7% under real time in
// 5 s bursts (build-host cap4: 150.07 s of media in 154.15 s of wall). The wall-debt
// cover fills each shortfall with 200 ms of PCM, then the late-PES clamp moved
// every later real AU onto that inflated sample clock, so each cover became
// permanent A lead over V (cap4 COVERSTAT a_lead 2.26 -> 5.25 -> 18.0 s). The
// PlexOB STC follows video and its queue holds 5.12 s: past that, audio is
// early and HDMI is silent. Real audio now keeps its PTS; the part that a
// cover already played is trimmed. MUT_LIVE_EF98_CLAMP_SHIFTS_AUDIO is ef980632.
inline bool coverYieldsToRealAudio() {
#ifdef MUT_LIVE_EF98_CLAMP_SHIFTS_AUDIO
    return false;
#else
    return true;
#endif
}
// Bytes of a PCM AU at pts90k that end at or before coveredEnd90k (s16 stereo
// 48 kHz, whole frames). Returns size when the AU is entirely covered.
inline size_t coveredPcmHeadBytes(int64_t pts90k, int64_t coveredEnd90k, size_t size) {
    if (pts90k >= coveredEnd90k)
        return 0;
    const int64_t frames = ((coveredEnd90k - pts90k) * 48000 + 89999) / 90000;
    const int64_t bytes = frames * 4;
    return bytes >= static_cast<int64_t>(size) ? size : static_cast<size_t>(bytes);
}

inline bool wallCoverWithinVideoBudget(int64_t coverBytesSinceVideo) {
    return coverBytesSinceVideo * 1000 / kPcmBytesPerSec < kWallCoverNoVideoCapMs;
}

inline bool extinfCoverResumesAfterAvExit() {
#ifdef MUT_LIVE_EXTINF_COVER_STICKY_EXIT
    return false;
#else
    return extinfCoverWakesBlockedRead();
#endif
}

struct BlockedReadCover {
    int holesGeGate = 0;
    int coverFires = 0;
};

// Glass dfc960ee r1 classified durations (ms), all DECODER_ONLY and all ≥0.2 s.
// Soak 615 s, SOURCE_ALIGNED=0. The single cover log is one 200 ms pop at the
// end of the last hole, not a fill of the interiors.
inline constexpr int kGlassDfc960HoleDurMs[] = {
    26696, 326, 545, 1228, 1134, 1510, 1631, 2667, 2814, 3052,
    3468, 4120, 4152, 13082, 30513, 320, 600, 913, 2017, 614,
    1662, 421, 1879, 258, 441, 1517, 1538, 1647, 2591, 2726,
    519, 2771, 3539, 4233, 4757, 4665, 32772, 3721,
};
inline constexpr int kGlassDfc960HoleCount =
    static_cast<int>(sizeof(kGlassDfc960HoleDurMs) / sizeof(kGlassDfc960HoleDurMs[0]));

// wakeInsideRead false matches the glass log: every hole stays ≥0.2 s and the
// cover fires once. wakeInsideRead slices each hole at extinfCoverWakeQuietMs.
// It does not model avio's sticky AVERROR_EXIT. abfe266e passes this and
// still ends the glass session. That failure is simulateGlassE87534Holes.
inline BlockedReadCover simulateGlassDfc960BlockedRead(bool wakeInsideRead) {
    BlockedReadCover o;
    const int gate = 200;
    const int wake = static_cast<int>(extinfCoverWakeQuietMs());
    if (!wakeInsideRead || wake <= 0 || wake >= gate) {
        for (int d : kGlassDfc960HoleDurMs) {
            if (d >= gate)
                ++o.holesGeGate;
        }
        o.coverFires = 1;
        return o;
    }
    for (int d : kGlassDfc960HoleDurMs)
        o.coverFires += (d + wake - 1) / wake;
    o.holesGeGate = 0;
    return o;
}

struct GlassE875Cover {
    int holesGeGate = 0;
    int coverFires = 0;
    bool sessionEnded = false;
    bool zapAudible = false;
    bool muxedReached = false;
    int longestMs = 0;
};

// e8755089 r1 AUDIO-DROPOUTS.csv durations, milliseconds. 34 DECODER_ONLY
// after t=5, median start spacing 5.012 s, longest 49.024 s, cover logged 0.
// Interiors are the capture floor (peak 11). abfe266e applies the same grid
// to a wake that aborts av_read_frame: one 200 ms cover, then SourceError,
// zaps 0/6, muxed miss.
inline constexpr int kGlassE875HoleDurMs[] = {
    1676, 2749, 30462, 549, 956, 1541, 1879, 2005, 2098, 3530, 3027, 4221,
    3042, 3528, 4423, 4529, 49024, 30470, 270, 859, 1086, 1343, 1506, 1719,
    2516, 2559, 3373, 3728, 4168, 4551, 27739, 583, 972, 466,
};
inline constexpr int kGlassE875HoleCount =
    static_cast<int>(sizeof(kGlassE875HoleDurMs) / sizeof(kGlassE875HoleDurMs[0]));

// wakeInsideRead false: e8755089, deadline never armed inside the blocked
// read, cover fires 0, session stays up, 34 holes remain.
// wakeInsideRead without resumeAfterExit: abfe266e, one cover then sticky
// AVERROR_EXIT ends the session. The 200 ms pop does not occupy a hole.
// Both true: each hole is filled in 200 ms slices and the read continues.
inline GlassE875Cover simulateGlassE87534Holes(bool wakeInsideRead, bool resumeAfterExit) {
    GlassE875Cover o;
    for (int d : kGlassE875HoleDurMs)
        if (d > o.longestMs)
            o.longestMs = d;
    const int gate = 200;
    const int chunk = 200;
    if (!wakeInsideRead) {
        for (int d : kGlassE875HoleDurMs)
            if (d >= gate)
                ++o.holesGeGate;
        o.coverFires = 0;
        o.sessionEnded = false;
        o.zapAudible = true;
        o.muxedReached = true;
        return o;
    }
    if (!resumeAfterExit) {
        o.holesGeGate = kGlassE875HoleCount;
        o.coverFires = 1;
        o.sessionEnded = true;
        o.zapAudible = false;
        o.muxedReached = false;
        return o;
    }
    for (int d : kGlassE875HoleDurMs) {
        int remain = d;
        while (remain >= gate) {
            remain -= chunk;
            ++o.coverFires;
        }
        if (remain >= gate)
            ++o.holesGeGate;
    }
    o.sessionEnded = false;
    o.zapAudible = true;
    o.muxedReached = true;
    return o;
}

// 9e6d1814 r1 cluster (holes 2–8) plus the two video_stall wedges.
// sampleClockCover repeats energetic PCM across a PTS lead that is already
// visible to the planner. It does not model dfc960ee, where the lead is the
// wall-clock block inside av_read_frame (see simulateGlassDfc960BlockedRead).
// Short stamp reproduces the grid (spacing 5.000 s, durations growing) and
// the 29.8 s / 30.5 s stall holes.
inline std::vector<SegGap> simulateGlass9e6dExtinfGrid(bool sampleClockCover) {
    std::vector<SegGap> gaps;
    const int64_t seg = 5 * 90000;
    int64_t lag = 583 * 90;
    const int64_t growth = 532 * 90;
    int64_t t = 28195 * 90;
    for (int i = 0; i < 7; ++i) {
        int64_t hole = lag;
        if (sampleClockCover) {
            hole = 0;
        } else {
            const int64_t stamp = pcmTicksForBytes(4096);
            hole = lag > stamp ? lag - stamp : 0;
        }
        if (hole >= kSoakGapGateTicks90k) {
            SegGap g;
            g.startTicks = t;
            g.durTicks = hole;
            gaps.push_back(g);
        }
        lag += growth;
        t += seg;
    }
    if (!sampleClockCover) {
        SegGap crypto;
        crypto.startTicks = 159032 * 90;
        crypto.durTicks = 29806 * 90;
        gaps.push_back(crypto);
        SegGap wedge;
        wedge.startTicks = 312707 * 90;
        wedge.durTicks = 30533 * 90;
        gaps.push_back(wedge);
    }
    return gaps;
}

// Glass 9e6d1814 GATE-V2 latch-RMS REJECT: SOURCE_ALIGNED=0 DECODER_ONLY=73,
// total silence 265.75 s, median spacing ~5.21 s, longest 33.789 s. Tip logged
// skip+escape+immediate-sub+energetic-fill once each (peak=38 rms=7) and
// audio_gap_recover=98, yet HDMI holes persist. Detector premise of d81e
// (RMS miss) is closed; covering the ~5 s seg-grid quiet class is not.
// ACCEPT: coversGlass9e6dHdmiQuietSegGrid wires to extinfSampleClockCover (product).
// MUT_LIVE_ACCEPT_9E6D_RMS_STILL_HOLES must FAIL.
inline constexpr int kGlass9e6dDecoderOnlyCount = 73;
inline constexpr int64_t kGlass9e6dLongestHoleDurTicks90k = 33789 * 90;  // 33.789 s
inline constexpr int64_t kGlass9e6dMedianSpacingTicks90k = 5210 * 90;    // ~5.21 s
inline constexpr int kGlass9e6dSkipEscapeFillOnce = 1;
inline constexpr int kGlass9e6dAudioGapRecoverCount = 98;

struct Glass9e6dHdmiHole {
    int peak = 0;
    int rms = 0;
    int64_t startTicks = 0;
    int64_t durTicks = 0;
};

// Full-window peak/RMS from ev/C-1066-soak.wav over each correlate DECODER_ONLY
// interval (glass-1066-latch-rms-9e6d1814-r1). All 73 have RMS < 33 (would arm
// skipInBandSilentPcm(peak,rms)); several have peak >= 33.
inline constexpr Glass9e6dHdmiHole kGlass9e6dHdmiHoles[] = {
    {32, 7, 13201 * 90, 276 * 90},
    {34, 3, 28195 * 90, 583 * 90},
    {33, 3, 33192 * 90, 2037 * 90},
    {30, 3, 38184 * 90, 2558 * 90},
    {32, 3, 43193 * 90, 2817 * 90},
    {31, 3, 48177 * 90, 3648 * 90},
    {32, 3, 53209 * 90, 3418 * 90},
    {32, 3, 58188 * 90, 3777 * 90},
    {35, 3, 83137 * 90, 1573 * 90},
    {35, 3, 88124 * 90, 849 * 90},
    {31, 3, 93108 * 90, 1631 * 90},
    {31, 3, 98137 * 90, 1735 * 90},
    {29, 3, 103113 * 90, 2822 * 90},
    {30, 3, 108128 * 90, 2311 * 90},
    {25, 3, 113129 * 90, 2988 * 90},
    {30, 3, 118123 * 90, 3211 * 90},
    {30, 3, 123131 * 90, 4183 * 90},
    {30, 3, 128118 * 90, 3712 * 90},
    {30, 3, 133106 * 90, 2141 * 90},
    {32, 3, 141968 * 90, 3133 * 90},
    {26, 3, 159032 * 90, 29806 * 90},
    {33, 4, 203851 * 90, 391 * 90},
    {30, 3, 208840 * 90, 1048 * 90},
    {32, 3, 213865 * 90, 1215 * 90},
    {12, 3, 218856 * 90, 1620 * 90},
    {34, 3, 223904 * 90, 1787 * 90},
    {22, 3, 229049 * 90, 2215 * 90},
    {29, 3, 234082 * 90, 2666 * 90},
    {34, 3, 239062 * 90, 2752 * 90},
    {33, 3, 244115 * 90, 2958 * 90},
    {31, 3, 249172 * 90, 3547 * 90},
    {32, 3, 254122 * 90, 3404 * 90},
    {19, 3, 259129 * 90, 3808 * 90},
    {34, 3, 264190 * 90, 4471 * 90},
    {31, 3, 269220 * 90, 4498 * 90},
    {30, 3, 274179 * 90, 33789 * 90},
    {32, 3, 312707 * 90, 30533 * 90},
    {31, 6, 353232 * 90, 241 * 90},
    {37, 3, 358194 * 90, 541 * 90},
    {32, 4, 363186 * 90, 896 * 90},
    {34, 3, 368192 * 90, 1515 * 90},
    {32, 3, 373176 * 90, 1933 * 90},
    {31, 3, 378153 * 90, 1975 * 90},
    {32, 3, 383188 * 90, 2519 * 90},
    {31, 3, 388168 * 90, 2933 * 90},
    {33, 3, 393193 * 90, 3536 * 90},
    {49, 3, 398174 * 90, 1000 * 90},
    {12, 3, 399175 * 90, 2948 * 90},
    {32, 4, 413120 * 90, 220 * 90},
    {33, 5, 418115 * 90, 256 * 90},
    {31, 4, 423136 * 90, 664 * 90},
    {30, 3, 428124 * 90, 967 * 90},
    {33, 3, 433138 * 90, 2988 * 90},
    {32, 4, 438136 * 90, 2029 * 90},
    {32, 4, 443116 * 90, 2258 * 90},
    {32, 3, 448137 * 90, 2830 * 90},
    {30, 3, 453124 * 90, 4713 * 90},
    {35, 3, 458119 * 90, 3615 * 90},
    {34, 3, 463125 * 90, 3996 * 90},
    {30, 3, 468120 * 90, 4119 * 90},
    {75, 3, 518173 * 90, 262 * 90},
    {12, 3, 528186 * 90, 384 * 90},
    {29, 3, 533178 * 90, 615 * 90},
    {23, 3, 538148 * 90, 1994 * 90},
    {72, 3, 543176 * 90, 1286 * 90},
    {31, 3, 548179 * 90, 1732 * 90},
    {31, 3, 553193 * 90, 1941 * 90},
    {31, 3, 558191 * 90, 3935 * 90},
    {31, 3, 563189 * 90, 2903 * 90},
    {31, 3, 568199 * 90, 3338 * 90},
    {33, 3, 573194 * 90, 3332 * 90},
    {31, 3, 578169 * 90, 9616 * 90},
    {32, 3, 593184 * 90, 5426 * 90},
};

inline bool glass9e6dHoleWouldArmRmsSkip(const Glass9e6dHdmiHole& h) {
    return skipInBandSilentPcm(h.peak, h.rms);
}

// Tip 9e6d1814: RMS skip + immediate escape fire, but do NOT eliminate the
// 73-hole class (glass fact). Product fix must make this true with a real
// soak-persistent cover — not a poke.
// Product cover for the 73-hole class is EXTINF sample-clock fill (reviewer RCA
// WRONG-PREMISE: missing AUs, not quiet AUs). Mutant ACCEPT_9E6D claims cover
// without the real extinfSampleClockCover path.
inline bool coversGlass9e6dHdmiQuietSegGrid() {
#ifdef MUT_LIVE_ACCEPT_9E6D_RMS_STILL_HOLES
    return true;
#else
    return extinfSampleClockCover();
#endif
}

// Glass-faithful: even with rmsSkip+immediateEscape (9e6d tip behavior), holes
// remain unless coversPersist. reviewer's fix must pass coversPersist=true via
// coversGlass9e6dHdmiQuietSegGrid() wired to product.
inline std::vector<SegGap> simulateGlass9e6dRmsStillHoles(bool rmsSkipArmed,
                                                          bool immediateEscape,
                                                          bool coversPersist) {
    std::vector<SegGap> gaps;
    if (rmsSkipArmed && immediateEscape && coversPersist)
        return gaps;
    for (const auto& h : kGlass9e6dHdmiHoles) {
        SegGap g;
        g.startTicks = h.startTicks;
        g.durTicks = h.durTicks;
        gaps.push_back(g);
    }
    return gaps;
}

// Strong terminal recover: clear leftover picture + landing + guide so later
// GuideTune posts a playjob and OB inputs are accepted again (r2 zap02+).
inline bool liveRemuxStrongTerminalRecover() {
#ifdef MUT_LIVE_WEAK_TERMINAL_RECOVER
    return false;
#else
    return liveRemuxRecoversInputsAfterTerminalPrefetch();
#endif
}

struct Rate0TerminalOutcome {
    bool firstPicture = false;
    bool nextObAccepted = false;
    bool nextPlayjobPosted = false;
};

// Glass r2 zap01: crypto Unable → demux rate=0 x3 → PREFETCH FAILED terminal;
// recover logs once; zap02+ ob=0 / no playjob. Distinct from simulateZap5ReturnTune
// (crypto retry that reaches first_picture without exhausting rate=0).
inline Rate0TerminalOutcome simulateZap5Rate0TerminalRecover(bool retryRate0,
                                                             bool weakGuideOnlyRecover,
                                                             bool strongRecover) {
    Rate0TerminalOutcome o;
    if (retryRate0 && strongRecover) {
        // Retries may still exhaust; strong recover restores OB + playjob path.
        o.firstPicture = false;
        o.nextObAccepted = true;
        o.nextPlayjobPosted = true;
        return o;
    }
    if (retryRate0 && weakGuideOnlyRecover && !strongRecover) {
        // Tip 6a5fbcc4: guideOpen=true + publishGuide, but leftover picture /
        // error OSD leaves zap02+ with ob=0 and no playjob.
        o.firstPicture = false;
        o.nextObAccepted = false;
        o.nextPlayjobPosted = false;
        return o;
    }
    o.firstPicture = false;
    o.nextObAccepted = false;
    o.nextPlayjobPosted = false;
    return o;
}

inline bool pacedPcmWouldReachHdmi(int64_t audioBytes, int64_t videoLeadTicks90k,
                                   int64_t pcmPeakAbs) {
    if (audioBytes < kFirstSpawnPaceAudioBytes)
        return false;
    if (pcmPeakAbs <= 0)
        return false;
    if (videoLeadTicks90k >= kSoakGapGateTicks90k)
        return false;
    return true;
}

// Glass F HDMI t=221.837 / 1.390 s: same audio-EXTINF fetch class as D's 70 s
// hole, at a later playlist/clip boundary (Europe → Audioslave; 0 rebase /
// 0 respawn / video living). Catch-up audio restores the 1.5 s pipeline
// between holes, so each hole is compared to the full cushion. Mutant 0
// (audio_bytes=0) leaves both ≥0.2 s HDMI gaps.
inline std::vector<SegGap> simulateMidSoakSegGaps(bool paceAfterFirstPicture) {
    std::vector<SegGap> gaps;
    const int64_t cushion =
        paceAfterFirstPicture ? (kFirstSpawnPaceUs * 90 / 1000) : 0;
    const int64_t holes[][2] = {
        {kFirstSpawnGapRemuxTicks90k, kFirstSpawnGapDurTicks90k},
        {kMidSoakGapRemuxTicks90k, kMidSoakGapDurTicks90k},
    };
    for (const auto& h : holes) {
        const int64_t starve = h[1] > cushion ? h[1] - cushion : 0;
        if (starve > 0) {
            SegGap g;
            g.startTicks = h[0];
            g.durTicks = starve;
            gaps.push_back(g);
        }
    }
    return gaps;
}

// Glass 85629316 zap4: fail() used to set guideOpen=true so later SELECT
// posted a playjob (ff11ef73). ChannelUp/Down on an open guide are cursor
// moves ("zap swallowed by open guide"). Keep the guide closed; bump gen
// is enough for later zaps. Mutant restores the open-guide recovery.
inline bool liveRemuxTerminalFailOpensGuide() {
#ifdef MUT_LIVE_NO_NONEXT_RETRY
    return true;
#else
    return false;
#endif
}

inline bool liveRemuxRetriesUnknownResolutionStall() {
#ifdef MUT_LIVE_NO_NONEXT_RETRY
    return false;
#else
    return true;
#endif
}

// Glass 85629316 zap4: 1066 pictured on gen 3 and gen 5; re-zap gen=7 remux
// SIGKILL sent=38728, no first-spawn pace, prefetchFail labeled unsupported
// resolution, 1-item live NO NEXT terminal=1, fail() opened the guide, zaps
// 5-6 swallowed. Retry the remux; if retries exhaust, keep guide closed.
struct Zap4NoNextOutcome {
    bool retriedRemux = false;
    bool terminalNoNext = false;
    bool guideOpen = false;
    bool laterZapSwallowed = false;
};

inline Zap4NoNextOutcome simulateZap4UnknownResStall(bool retryUnknown, bool openGuideOnTerminal) {
    Zap4NoNextOutcome o;
    if (retryUnknown) {
        o.retriedRemux = true;
        return o;
    }
    o.terminalNoNext = true;
    o.guideOpen = openGuideOnTerminal;
    o.laterZapSwallowed = openGuideOnTerminal;
    return o;
}

// ---------------------------------------------------------------------------
// ACCEPT e8755089 (glass-1066-holes-fix-e8755089-r1, 13:22 CT): 34 DECODER_ONLY,
// needle `live remux extinf pcm cover` count 0, energetic_pcm_fill=0,
// audio_gap_recover=0. Holes are measured from C-1066-soak.wav (peak/RMS of the
// HDMI-quiet span) + C-1066-gate-v2-correlate.json start/dur.
//
// Glass-faithful premise: in PTS space audio never lagged video (fill=0 and
// gap_recover=0 both key on a PTS lag), so popReadyByPts never saw a gap. The
// holes are wall-clock: av_read_frame blocks on the EXTINF fetch / relay stall
// and nothing reaches HDMI. The only cover path for that is FillTimeout, whose
// deadline e8755089 arms *before* av_read_frame when now-lastByteMs_ >= 150 ms.
// lastByteMs_ was just reset by noteByte() for the previous packet, so the
// pre-read quiet is ~0 ms and the deadline is never armed; onInterrupt only
// checks an already-armed deadline. The blocked read is never woken: cover=0.
struct GlassE875Hole {
    int peak;
    int rms;
    int64_t startMs;
    int64_t durMs;
};
inline constexpr int kGlassE875DecoderOnlyCount = 34;
inline constexpr int64_t kGlassE875LongestHoleMs = 49020;
inline constexpr int kGlassE875ExtinfCoverLogCount = 0;
inline constexpr int kGlassE875EnergeticFillCount = 0;
inline constexpr int kGlassE875AudioGapRecoverCount = 0;
// Quiet seen by the pre-read arm check: noteByte() ran for the packet that
// returned just before; demuxOne is re-entered in the same read() loop.
inline constexpr int64_t kGlassE875PreReadQuietMs = 0;
inline constexpr int64_t kExtinfWedgeQuietMs = 100;
// FFmpeg HTTP wait polls the interrupt callback at most every 100 ms, so the
// first wake lands within quiet + 99 ms of the last byte.
inline constexpr int64_t kInterruptPollSliceMs = 100;
// 30708c1a / 9c536d47 arm lead (fix6): armExtinfCoverWake used to store the fill
// deadline as now, so the poll that armed it returned FillTimeout before
// av_read_frame (82b99128 vfps 212.6->1.5). One interrupt slice in the future
// enters the read: a ready PES returns, only a read that stays blocked is covered.
inline int64_t extinfWakeArmLeadMs() {
#if defined(MUT_LIVE_WAKE_PREEMPTS_READ) || defined(MUT_LIVE_WAKE_ABORT_STORM)
    return 0;
#else
    return kInterruptPollSliceMs;
#endif
}
inline constexpr int64_t kExtinfWedgeCoverMs = 200;
inline constexpr GlassE875Hole kGlassE875HdmiHoles[] = {
    {11, 3, 64174, 1671},
    {13, 3, 65845, 2749},
    {13, 3, 70930, 30462},
    {26, 5, 106357, 545},
    {18, 3, 111386, 950},
    {22, 3, 116397, 1537},
    {21, 3, 121388, 1876},
    {14, 3, 126601, 2003},
    {13, 3, 131633, 2093},
    {21, 3, 136614, 3530},
    {13, 3, 141665, 3022},
    {28, 3, 146650, 4218},
    {15, 3, 151690, 3037},
    {12, 3, 156636, 3528},
    {14, 3, 161678, 4423},
    {13, 3, 166678, 4524},
    {14, 3, 171710, 49020},
    {23, 3, 225401, 30466},
    {13, 3, 265852, 266},
    {12, 3, 270828, 855},
    {14, 3, 275838, 1081},
    {18, 3, 280844, 1339},
    {12, 3, 285836, 1500},
    {12, 3, 290845, 1713},
    {13, 3, 295840, 2512},
    {14, 3, 300822, 2555},
    {16, 3, 305847, 3368},
    {12, 3, 310828, 3723},
    {13, 3, 315830, 4164},
    {13, 3, 320846, 4544},
    {18, 3, 325836, 27735},
    {11, 3, 363584, 576},
    {25, 4, 368595, 969},
    {14, 3, 548586, 461}
};

// True when the product wakes a blocked av_read_frame from onInterrupt once
// quiet >= kExtinfWedgeQuietMs (fix2). e8755089 only arms pre-read.
// d0045771 line: the interrupt-poll wake is extinfCoverWakesBlockedRead()
// (MUT_LIVE_EXTINF_COVER_NO_READ_WAKE restores the e8755089 pre-read arm).
inline bool extinfWedgeArmsInInterrupt() {
    return extinfCoverWakesBlockedRead();
}

// Wall-clock model of one blocked read per glass hole. armInInterrupt: quiet is
// sampled while the read is blocked (grows to the hole length). Otherwise quiet
// is sampled once pre-read (kGlassE875PreReadQuietMs). Once armed, FillTimeout
// emits kExtinfWedgeCoverMs of PCM per wake; the residual HDMI hole is the
// arm latency. Returns holes >= the 0.2 s soak gate; covers = cover emits.
inline std::vector<SegGap> simulateGlassE875BlockedReads(bool coverEnabled,
                                                          bool armInInterrupt,
                                                          int64_t preReadQuietMs,
                                                          int& covers) {
    std::vector<SegGap> out;
    covers = 0;
    for (const auto& h : kGlassE875HdmiHoles) {
        int64_t armAt = -1;
        if (coverEnabled) {
            if (preReadQuietMs >= kExtinfWedgeQuietMs)
                armAt = kExtinfWedgeCoverMs;  // pre-read deadline = now + 200
            else if (armInInterrupt)
                armAt = kExtinfWedgeQuietMs + kInterruptPollSliceMs - 1;
        }
        int64_t residual = h.durMs;
        if (armAt >= 0 && armAt < h.durMs) {
            residual = armAt;
            covers += static_cast<int>((h.durMs - armAt + kExtinfWedgeCoverMs - 1) /
                                       kExtinfWedgeCoverMs);
        }
        if (residual * 90 >= kSoakGapGateTicks90k) {
            SegGap g;
            g.startTicks = h.startMs * 90;
            g.durTicks = residual * 90;
            out.push_back(g);
        }
    }
    return out;
}

}  // namespace live_disc
}  // namespace misterplex
