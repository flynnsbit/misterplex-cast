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

#include <atomic>
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
        // Both streams enter the new timeline at a splice/respawn; clear the
        // other stream's pre-splice raw PTS so its first post-splice packet
        // shares this shift instead of firing a bogus second rebase.
        (audio ? s.lastRawV : s.lastRawA) = kNoPts90k;
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

struct RetuneOutcome {
    bool firstPicture = false;
    bool nextTuneAccepted = false;
};

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

}  // namespace live_disc
}  // namespace misterplex
