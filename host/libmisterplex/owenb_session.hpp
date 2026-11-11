#pragma once
// OWENB-P1B T5: the OB play loop. Demuxed packets (Annex-B AUs with 90 kHz PTS, 48 kHz s16le stereo
// PCM) go out to OwenbRing (owenb_ring.hpp) in container order, with the cast timeline's controls.
// Design: OWENB-P1B-CAST-DESIGN.md §a.4 (CAVLC rewrite + rate guard + lowered retry), §a.5 (audio,
// volume), §b.3 (pause / resume / seek held / stop / end hold / position), §c.4 (no DDR paint).
//
// It writes only the owenb ring (ctrl page 0x31400000 + ring 0x31800000, through the OwenbRing
// window). It never touches the idle FB at 0x31600000 and never paints DDR.
//
// Threads: run() is the play thread. pause / resume / seek / stop / setVolumePercent may be called
// from any thread; they queue commands that run() takes between records. positionMs() and the other
// readers are lock-free.
//   - Video AUs: dropped until the first keyframe of a stream (session start, seek). The last AU before
//     the end of the input carries END_OF_STREAM, so the core finishes it and holds it (one AU of
//     lookahead; audio behind it waits, so the order is the container's).
//   - CAVLC: the library rewrites it to CABAC (OwenbRing::setCavlcRewrite, default on). The guard
//     ends the session with NeedsTranscode when a CAVLC stream runs above cavlcMaxKbps
//     (OB_CAVLC_MAX_KBPS, default 4,000), or when the rewrite refuses a unit (-EOPNOTSUPP /
//     -EBADMSG). The daemon then asks PMS for one lowered (re-encoded, High/CABAC) request.
//   - Pause: h264ring_set_pause(1), and the input is not read until resume. Ignored before the first
//     picture (design §b.3: cast clients pause while buffering).
//   - Seek (held): pause (the picture stays), open the new input, read to its first keyframe, then
//     h264ring_flush (a new stream) and the keyframe, then resume unless the user paused.
//   - Stop: IDLE_SCREEN=last pauses and releases (the picture is held, nothing more plays); black
//     flushes and ends.
//   - End of input: END_OF_STREAM on the last AU, wait until the core has taken every record, then
//     end (hold) or flush + end (black).

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "net_resume.hpp"
#include "owenb_ring.hpp"
#include "embedded_text_map.hpp"

namespace misterplex::owenb {

struct MediaPacket {
    enum class Kind : uint8_t { Video, Audio };
    Kind kind = Kind::Video;
    std::vector<uint8_t> data;              // Video: one Annex-B AU; Audio: 48 kHz s16le stereo
    int64_t pts90k = h264stream::kNoPts90k;
    bool keyframe = false;                  // Video: an IDR (random access point)
};

// In-place track change. Failed: the caller may reopen the input at the current position.
// Unsupported: leave the current track (image subtitles, a stream this input cannot open).
enum class StreamSwitch : uint8_t { Applied, Unsupported, Failed };

enum class SourceRead : uint8_t { Packet, Again, Eof, Error };

// One demuxed input (OwenbAvSource on the daemon: libav; a scripted source in tests).
class IMediaSource {
public:
    virtual ~IMediaSource() = default;
    virtual SourceRead read(MediaPacket& p, std::string& err) = 0;
    virtual void cancel() {}                // unblock a read from another thread (stop)
    // 0008: the container's display aspect (num:den), when the container states one (MP4 pasp, MKV
    // display size); false: none, the picture's own shape applies
    virtual bool containerAspect(int64_t& num, int64_t& den) const {
        (void)num;
        (void)den;
        return false;
    }
    struct VideoFormat {
        int width = 0, height = 0;
        int fpsNum = 0, fpsDen = 0;
        bool interlaced = false;
    };
    virtual VideoFormat videoFormat() const { return {}; }
    // MEMSTAT. Defaults so a scripted test source stays zero.
    virtual size_t readyDepth() const { return 0; }
    virtual unsigned probePackets() const { return 0; }
    // 90 kHz PTS of media time 0 when this input's timestamps are absolute (a direct-play
    // file). h264stream::kNoPts90k when they are not: a transcode segment, or a raw
    // elementary stream whose PTS is synthesised from zero after every open. Captured on
    // the play thread at open; updatePosition does not call back into the source.
    virtual int64_t startPts90k() const { return h264stream::kNoPts90k; }
    // audioIndex / subtitleIndex are container indexes. subtitleIndex < 0 turns subtitles off.
    // Called on the play thread only, between reads.
    virtual StreamSwitch selectAudioStream(int audioIndex, std::string& err) {
        (void)audioIndex;
        err = "this input cannot switch audio";
        return StreamSwitch::Unsupported;
    }
    virtual StreamSwitch selectSubtitleStream(int subtitleIndex, std::string& err) {
        (void)subtitleIndex;
        err = "this input has no text subtitles";
        return StreamSwitch::Unsupported;
    }
    // The cue covering positionMs, or empty. Play thread only. This daemon does not decode cues.
    virtual std::string activeSubtitle(int64_t positionMs) {
        (void)positionMs;
        return {};
    }
    // Container index of the audio stream that is open, or -1. Play thread only.
    virtual int activeAudioIndex() const { return -1; }
    // The audio decoder is allocated and open. The index can stay selected after
    // the decoder is freed; that is silence, not a live track.
    virtual bool audioDecoderOpen() const { return false; }
    // Identity of the embedded text track whose PMS index may not match this file.
    virtual void setEmbeddedTextIdentity(const misterplex::PlexTextIdentity&) {}
    // NET_RESUME stall watch. The default is a no-op, so a scripted source and a
    // flag-off play keep today's interrupt (stop only).
    virtual void setStallWatch(misterplex::netresume::StallWatch) {}
    virtual int lastAvErr() const { return 0; }
    virtual int lastHttpStatus() const { return 0; }
};

struct PrefetchedInput;

struct OpenRequest {
    std::string url;
    std::string audioUrl;                  // separate DASH audio; empty when the url is muxed
    std::string headers;
    int64_t startMs = 0;                    // the position this input starts at (timeline)
    bool containerSeek = false;             // seek the container to startMs (a file); a PMS URL
                                            // carries the offset itself
    bool retryTransientOpen = false;        // empty body / I/O / helper 5xx: retry, stay on this play
    int64_t previousMs = -1;                // seek resume point; -1 when this open is not a seek
    // One googlevideo 403: rewrite url/audio and open again. Empty when this
    // open is not a YouTube cast. The callback clears itself after one try.
    std::function<bool(std::string& url, std::string& audio)> reresolve403;
    // Container indexes from the PMS Stream@index. -1: the default (best audio, no subtitle).
    int audioStreamIndex = -1;
    int subtitleStreamIndex = -1;
    // Container index of embedded text to decode (SOFT_SUBS, or mov_text #260).
    // -1: none. PMS Stream@index. The demuxer remaps it onto the opened file.
    int textSubtitleIndex = -1;
    std::string textCodec;
    std::string textLanguage;
    std::string textTitle;
    bool textForced = false;
    int textOrdinal = -1;
    // #231 Run H: polled by the source's interrupt while it opens (the source is not in src_
    // yet, so stop() cannot cancel it). True aborts the open.
    std::function<bool()> cancelled;
    // >0: abandon this open attempt after this many ms (interrupt deadline); the error then
    // reads "open: Connection timed out (attempt cap)". 0: no cap (libav rw_timeout only).
    int64_t attemptTimeoutMs = 0;
    std::shared_ptr<PrefetchedInput> prefetched; // Spelunkr only: single-use, opened off-ring
    // Spelunkr header (moov) cache, keyed by (url, size, etag). size < 0: the header cache is
    // off for this open (default; Plex plays never set these) and AvSource opens url normally.
    int64_t headerCacheSize = -1;
    std::string headerCacheEtag;
    // Test-only (and diagnostic) hook: when non-null, filled with HeaderCacheAvio::Stats right
    // after find_stream_info (the same numbers in the "header-cache find_stream_info" log line),
    // so a test can assert cachedBytesServed>0 / liveBytesRead==0 on a warm open without
    // re-deriving stats from stderr. Left untouched when headerCacheSize < 0.
    struct HeaderCacheStatsOut {
        bool cacheHit = false;
        int64_t cachedBytesServed = 0;
        int64_t liveBytesRead = 0;
    };
    HeaderCacheStatsOut* headerCacheStatsOut = nullptr;
};

using SourceFactory = std::function<std::unique_ptr<IMediaSource>(const OpenRequest&, std::string& err)>;

// setSubtitle: store the Plex stream id on the open request and tell the caption sink.
// subtitleIndex is the Plex Stream@id (0 = off), not a container index — sidecars
// are not in the demux.
inline bool applySubtitleCommand(OpenRequest& req, int subtitleIndex, bool setSubtitle,
                                 const std::function<void(int)>& onSelect) {
    if (!setSubtitle)
        return false;
    req.subtitleStreamIndex = subtitleIndex;
    if (onSelect)
        onSelect(subtitleIndex);
    return true;
}

// Media time of a 33-bit 90 kHz PTS against the container origin. A delta in the upper
// half of the field is treated as "behind" and reports 0, matching the STC guard below.
inline int64_t mediaMsFromPts90k(int64_t pts90k, int64_t origin90k) {
    constexpr uint64_t kMask = (uint64_t{1} << 33) - 1;
    constexpr uint64_t kBehind = uint64_t{1} << 32;
    const uint64_t d = (static_cast<uint64_t>(pts90k) - static_cast<uint64_t>(origin90k)) & kMask;
    if (d > kBehind)
        return 0;
    return static_cast<int64_t>(d / 90);
}

// Published position stamp: seek epoch in the high bits, phase in the low 8.
//   0  the seek's requested target (placeholder, published when the seek is queued)
//   1  the landing commit (container seek whose keyframe is the new media time)
//   2  a live measurement from the core STC
// A newer stamp wins. Another phase-2 sample of the same seek may advance the position.
// A placeholder or an older seek must not overwrite a landing already committed.
inline uint32_t posStamp(uint32_t epoch, uint8_t phase) { return (epoch << 8) | phase; }
inline bool posStampWins(uint32_t current, uint32_t incoming) {
    if (incoming > current)
        return true;
    return incoming == current && (incoming & 0xffu) >= 2u;
}

// The play thread's view of the OB position clock. Readers see two atomics, not this
// struct: the packed stamp+ms is the reported position (bar, timeline, skip base),
// and mediaMs() is the true media time. No heap: a seek only rewrites these fields.
//
// baseMs / mediaMs follow the keyframe a container seek actually lands on. A short
// forward skip whose keyframe is still behind the picture on screen latches
// holdTargetMs and reports that target until the picture catches it. The latch is
// not the clock.
//
// After an absolute seek, one running STC is the new picture when it is not the
// old clock. The rules, in order:
//
//   1. Old-clock path. The STC is still the picture this seek left when it sits
//      between 200 ms behind the pre-seek sample and 200 ms ahead of the wall
//      time since that sample. The 200 ms is slack on that band, not a rate
//      limit added again on every sample. The band grows with wall time while
//      the STC is low, frozen, paused, or not sampled, so a clock that kept
//      counting through any of those gaps stays on the path.
//   2. Agreement. A sample on that path is the new picture when the STC that
//      started its current run sits within 500 ms of the keyframe. One step
//      breaks the run when it moves more than 100 ms away from real time over
//      both the wall gap and the unpaused gap. A drop does too. A pause is
//      not a separate reset: a clock that froze with the pause matches 1x
//      over the unpaused gap, and a clock that kept running matches 1x over
//      the wall gap, so both stay on the run. A repeated sample, a frame-sized
//      hold, or a few milliseconds of read jitter does not break the run, so
//      those stay tied to the value the run started on.
//      The bar's dead reckoning is a separate display guess and is not this
//      test. A stale clock's run starts about a skip away from the keyframe,
//      so a skip of at most 500 ms may still anchor on the old value. A fresh
//      clock within 100 ms of where that run would be is the same stream and
//      is not anchored. A lead of more than 500 ms that sits on this path is
//      the same stream as an old clock that kept counting, and the 5 s last
//      resort does not take it. A lead that large is anchored only once it is
//      off the path, which is a far skip. The window and both timeouts use
//      this one test. There is no separate flush-instant exemption.
//   3. Window. An STC that is not behind the keyframe by more than 500 ms, and
//      not ahead of it by more than the unpaused time since the flush plus
//      500 ms, is the picture when it is not the old clock (rules 1 and 2).
//   4. Restart. After 2 s of continuous STC_RUN, a post-flush drop, and a lead
//      inside elapsed + 2 s, the same "not the old clock" test accepts it.
//   5. Last resort. After 5 s of continuous STC_RUN, any lead is accepted when
//      it is not the old clock. Short STC_RUN drops are not bridged; a drop is
//      how this timeout tells a restart from a stall.
//
// Until a sample is accepted, the bar may leave the landing only while the
// rejected STC itself is moving, and by at most kHoldTimeoutMs. A frozen
// STC_RUN does not move the bar. The hold already shows the target, so the
// guess stays quiet under it and is dropped when the hold expires. mediaMs
// stays on the landing until a sample is accepted. A user pause is not a
// flush, and pause time does not open the window.
struct PlaybackClock {
    static constexpr int64_t kMask = (int64_t{1} << 33) - 1;
    static constexpr int64_t kBehind = int64_t{1} << 32;
    // How far a fresh STC may sit from the first AU. Wide enough that a clock which
    // anchored on audio, or on a picture a few hundred ms past the first AU, is the
    // new picture at once. A re-anchor can also land a tick under that PTS.
    static constexpr int64_t kAnchorTol90k = 90 * 500;
    // Continuous STC_RUN after a flush that actually stopped the clock. The timeout
    // accepts a sample only inside this extra slack past the elapsed window, so a
    // stale clock cannot jump the bar by a whole skip.
    static constexpr int64_t kAnchorTimeoutMs = 2000;
    static constexpr int64_t kAnchorTimeoutCap90k = 90 * 2000;
    // A fresh STC that never went low, or that leads by more than the 2 s cap.
    // Continuous unpaused STC_RUN. The old clock (on its path, and far from the
    // reckoned picture) is not taken here.
    static constexpr int64_t kAnchorLastResortMs = 5000;
    // Slack on the wall-time band around the pre-seek sample. Inside the band,
    // the sample is that clock unless it also agrees with the current run.
    static constexpr int64_t kPreSeekTol90k = 90 * 200;
    // One step this far from real time starts a new run. Wider than this, the
    // MF-E rows (a fresh clock 200 ms off a running old one) stop anchoring.
    // A fresh clock inside the slack is not separable from a stale clock.
    // The other side of that trade: one step of more than about 110 ms, or a
    // freeze of more than 100 ms that snaps back on the keyframe, is the same
    // evidence as a fresh clock and is taken, with the skip as error. A 33 ms
    // frame polled every 10 ms stays inside this. A coarser STC quantum does
    // not: one step larger than 100 ms plus the poll restarts the run. The
    // real core's STC step size, and whether that STC freezes or keeps
    // running across a user pause, has to be measured on glass.
    static constexpr int64_t kRunSlack90k = 90 * 100;
    // A 10 s GOP plus the default 30 s skip can take about 40 s of playback to
    // reach the target. The latch gives up after this much unpaused time.
    static constexpr int64_t kHoldTimeoutMs = 60000;

    uint32_t epoch = 1;
    int64_t baseMs = 0;          // true media time of this open's first AU (or the start offset)
    int64_t mediaMs = 0;         // landing, then each accepted sample; never a guess
    int64_t basePts = -1;
    int64_t originPts = 0;
    int64_t posAtSeekMs = 0;
    int64_t seekTargetMs = 0;    // requested target; the placeholder until the first AU
    int64_t holdTargetMs = -1;   // < 0: the bar follows mediaMs
    int64_t holdSinceMs = -1;
    int64_t tFlushMs = -1;       // steady time of the flush ack, else the first AU
    int64_t stcRunSinceMs = -1;  // start of the current unbroken STC_RUN stretch
    int64_t pausedAccumMs = 0;   // user-pause time since the flush, excluded from the window
    int64_t pauseStartMs = -1;
    int64_t holdPausedAtStart = 0;
    int64_t preSeekStc = -1;     // last STC of the picture this seek left
    int64_t preSeekAtMs = 0;
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
    int64_t oldHigh90 = 0;       // observed run; production grows the band by wall time
    int64_t oldSeenAtMs = -1;
#endif
    int64_t reckonFromMs = -1;   // start of the open stretch whose STC was seen moving
    int64_t reckonAccumMs = 0;   // closed stretches while this open is still unanchored
    int64_t reckonLastStc = -1;  // previous rejected STC, so a frozen value does not count
    int64_t reckonLastAtMs = -1;
    int64_t runFromMs = -1;      // start time of the rejected STC's current continuous run
    int64_t runLastStc = -1;
    int64_t runLastAtMs = -1;
    int64_t runStartStc = -1;    // STC at the start of that run; agreement uses this, not the wall time
    int64_t runLastPausedMs = 0; // pausedAccumMs when runLastAtMs was sampled
    bool absolute = false;       // container timestamps: media time is (STC - origin)
    bool forward = false;        // this seek's target is at or ahead of the position shown
    bool anchored = false;       // the STC has re-anchored on this seek's first AU
    bool flushNoted = false;     // a flush ack (or the first AU, when there is no flush)
    bool sawClockDrop = false;   // STC_RUN fell after that ack, and the user was not paused

    void reset(int64_t startMs) {
        epoch = 1;
        baseMs = startMs;
        mediaMs = startMs;
        basePts = -1;
        originPts = 0;
        posAtSeekMs = startMs;
        seekTargetMs = startMs;
        holdTargetMs = -1;
        holdSinceMs = -1;
        holdPausedAtStart = 0;
        tFlushMs = -1;
        stcRunSinceMs = -1;
        pausedAccumMs = 0;
        pauseStartMs = -1;
        preSeekStc = -1;
        preSeekAtMs = 0;
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
        oldHigh90 = 0;
        oldSeenAtMs = -1;
#endif
        clearReckon();
        absolute = false;
        forward = false;
        anchored = false;
        flushNoted = false;
        sawClockDrop = false;
    }

    // containerSeek with a known origin uses absolute PTS. origin90k < 0 keeps the
    // requested startMs as the base (a transcode segment, a raw elementary stream).
    void arm(uint32_t ep, bool containerSeek, int64_t origin90k) {
        epoch = ep;
        absolute = containerSeek && origin90k >= 0;
        originPts = absolute ? origin90k : 0;
        basePts = -1;
        anchored = false;
        sawClockDrop = false;
    }

    void prepareSeek(uint32_t ep, int64_t prevMs, int64_t targetMs) {
        epoch = ep;
        posAtSeekMs = prevMs;
        forward = targetMs >= prevMs;
        seekTargetMs = targetMs;
        baseMs = targetMs; // phase-0 placeholder until the landing AU
        basePts = -1;
        absolute = false;
        anchored = false;
        sawClockDrop = false;
        flushNoted = false;
        tFlushMs = -1;
        stcRunSinceMs = -1;
        pausedAccumMs = 0;
        pauseStartMs = -1;
        clearReckon();
        clearHold();
    }

    // Flush acknowledgement. The anchor window starts here. A low before this, or a
    // low while the user is paused, is not "the flush took".
    void noteFlush(int64_t nowMs) {
        flushNoted = true;
        if (tFlushMs < 0)
            tFlushMs = nowMs;
        sawClockDrop = false;
        stcRunSinceMs = -1;
        pausedAccumMs = 0;
        pauseStartMs = -1;
        clearReckon();
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
        // The live band is wall time since the pre-seek sample. This high-water
        // is only what the rungrow mutant reads.
        oldHigh90 = 0;
        oldSeenAtMs = nowMs;
#endif
    }

    void clearHold() {
        holdTargetMs = -1;
        holdSinceMs = -1;
    }

    // Reported position: the latch, until the picture reaches it.
    int64_t reportedMs() const {
        if (holdTargetMs >= 0 && mediaMs < holdTargetMs)
            return holdTargetMs;
        return mediaMs;
    }

    // First video AU pushed for this open. True when landingMs is this open's media
    // time (an absolute container seek). The clock always rebases to that landing.
    // A forward seek whose keyframe is still behind the on-screen position latches
    // the requested target for the bar only.
    // nowMs defaults to the flush instant for tests. The session passes nowMs().
    bool noteFirstAu(int64_t pts90k, int64_t& landingMs, int64_t nowMs = 0) {
        if (pts90k < 0 || basePts >= 0)
            return false;
        basePts = pts90k;
        anchored = false;
        // A low that already followed the flush stays. Clearing it here would hide
        // the restart the timeout path requires.
        stcRunSinceMs = -1;
        if (tFlushMs < 0) {
            tFlushMs = nowMs;
            flushNoted = true;
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
            oldHigh90 = 0;
            oldSeenAtMs = nowMs;
#endif
        }
        if (!absolute)
            return false;
        const int64_t land = mediaMsFromPts90k(pts90k, originPts);
        baseMs = land;
        mediaMs = land;
        landingMs = land;
        if (forward && land <= posAtSeekMs && seekTargetMs > land) {
            holdTargetMs = seekTargetMs;
            holdSinceMs = nowMs;
            holdPausedAtStart = pausedAccumMs;
        }
        return true;
    }

    // Signed (STC - ref) inside the 33-bit field, in (-2^32, 2^32].
    static int64_t circularDelta(int64_t stc90k, int64_t ref90k) {
        const uint64_t d = (static_cast<uint64_t>(stc90k) - static_cast<uint64_t>(ref90k)) &
                           static_cast<uint64_t>(kMask);
        if (d > static_cast<uint64_t>(kBehind))
            return static_cast<int64_t>(d) - (int64_t{1} << 33);
        return static_cast<int64_t>(d);
    }

    // One STC sample captured under sampleEpoch. False: the sample is not the picture.
    // outMs is the reported position (the hold, when one is latched). mediaMs is the
    // picture. nowMs is steady time. The default 0 is the flush instant in tests;
    // the session always passes nowMs(). userPaused excludes that stretch from the
    // anchor window and does not count as a clock drop.
    bool takeSample(uint32_t sampleEpoch, int64_t stc90k, bool stcRun, int64_t& outMs,
                    bool userPaused = false, int64_t nowMs = 0) {
        if (sampleEpoch != epoch)
            return false;
        if (userPaused) {
            if (pauseStartMs < 0)
                pauseStartMs = nowMs;
#ifndef MUT_OWENB_SESS_RECKON_PAUSE
            // Close the open stretch at the pause. The bar holds that value;
            // pause time itself is not playback.
            noteReckonStop(nowMs);
#endif
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
            noteOldClock(-1, false, nowMs);
#endif
            stcRunSinceMs = -1;
            return false;
        }
        if (pauseStartMs >= 0) {
            if (nowMs > pauseStartMs)
                pausedAccumMs += nowMs - pauseStartMs;
            pauseStartMs = -1;
        }
        if (!stcRun || stc90k < 0 || basePts < 0) {
            if (stcRun && stc90k >= 0 && basePts < 0)
                rememberPreSeek(stc90k, nowMs);
            if (!stcRun && basePts >= 0 && flushNoted)
                sawClockDrop = true;
            if (!stcRun) {
                noteReckonStop(nowMs);
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
                noteOldClock(stc90k, false, nowMs);
#endif
                runLastStc = -1;
                stcRunSinceMs = -1;
            }
            return false;
        }
        const int64_t delta = circularDelta(stc90k, basePts);
        if (!anchored) {
            if (stcRunSinceMs < 0)
                stcRunSinceMs = nowMs;
            if (absolute && !acceptAnchor(stc90k, delta, nowMs)) {
                noteReckonSample(stc90k, nowMs);
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
                noteOldClock(stc90k, true, nowMs);
#endif
                {
                    const int64_t rs = runStartFor(stc90k, nowMs);
                    if (rs == nowMs || runStartStc < 0)
                        runStartStc = stc90k;
                    runFromMs = rs;
                }
                runLastStc = stc90k;
                runLastAtMs = nowMs;
#ifndef MUT_OWENB_SESS_ANCHOR_NOSTOREPAUSE
                // The pause time at this sample. Without it, the next pause
                // counts every earlier pause again and restarts the run.
                runLastPausedMs = pausedAccumMs;
#endif
                return false;
            }
            anchored = true;
            clearReckon();
        }
        // A delta in the upper half of the field (the clock is before this AU) reports
        // no progress, matching the guard this clock replaced.
        const int64_t elapsed = delta < 0 ? 0 : delta;
        mediaMs = baseMs + elapsed / 90;
        if (holdTargetMs >= 0 && (mediaMs >= holdTargetMs || holdAgeMs(nowMs) >= kHoldTimeoutMs))
            clearHold();
        outMs = reportedMs();
        rememberPreSeek(stc90k, nowMs);
        return true;
    }

    // The short-forward latch, checked even when no STC sample was accepted. True
    // when it aged out and the bar should return to the picture.
    bool pollHold(int64_t nowMs) {
        if (holdTargetMs < 0 || holdAgeMs(nowMs) < kHoldTimeoutMs)
            return false;
        clearHold();
        // Time counted while the hold was showing the target is not a jump
        // waiting for the latch to age out.
        clearReckon();
        return true;
    }

    // While an absolute seek is still unanchored, the bar may leave the landing
    // by the wall time during which the rejected STC was actually moving. A
    // frozen STC_RUN does not count. The total is capped, and a hold already
    // shows the target, so this stays quiet under one. The value is a display
    // guess: mediaMs is not advanced by it.
    bool pollReckon(int64_t nowMs, int64_t& outMs) const {
        if (!absolute || anchored || basePts < 0)
            return false;
#ifndef MUT_OWENB_SESS_RECKON_HOLD
        if (holdTargetMs >= 0)
            return false;
#endif
        int64_t adv = reckonAccumMs;
        if (reckonFromMs >= 0 && nowMs > reckonFromMs)
            adv += nowMs - reckonFromMs;
        if (adv <= 0)
            return false;
        if (adv > kHoldTimeoutMs)
            adv = kHoldTimeoutMs;
        outMs = baseMs + adv;
        if (outMs > 0x7fffffff)
            outMs = 0x7fffffff;
        return true;
    }

private:
    // Time since the flush during which the user was not paused.
    int64_t elapsedSinceFlushMs(int64_t nowMs) const {
        if (tFlushMs < 0 || nowMs <= tFlushMs)
            return 0;
        int64_t paused = pausedAccumMs;
        if (pauseStartMs >= 0 && nowMs > pauseStartMs)
            paused += nowMs - pauseStartMs;
        const int64_t e = nowMs - tFlushMs - paused;
        return e < 0 ? 0 : e;
    }

    int64_t holdAgeMs(int64_t nowMs) const {
        if (holdSinceMs < 0)
            return 0;
        int64_t paused = pausedAccumMs;
        if (pauseStartMs >= 0 && nowMs > pauseStartMs)
            paused += nowMs - pauseStartMs;
        const int64_t age = nowMs - holdSinceMs - (paused - holdPausedAtStart);
        return age < 0 ? 0 : age;
    }

    void clearReckon() {
        reckonFromMs = -1;
        reckonAccumMs = 0;
        reckonLastStc = -1;
        reckonLastAtMs = -1;
        runFromMs = -1;
        runLastStc = -1;
        runLastAtMs = -1;
        runStartStc = -1;
        runLastPausedMs = 0;
    }

    void noteReckonStop(int64_t nowMs) {
        if (reckonFromMs < 0)
            return;
        if (nowMs > reckonFromMs)
            reckonAccumMs += nowMs - reckonFromMs;
        reckonFromMs = -1;
    }

    // A rejected running sample. Wall time counts only once the STC has moved
    // by at least half of the gap since the previous rejected sample. The first
    // sample only records the value, so a frozen clock never opens a stretch.
    // A hold still updates the stretch: pollReckon is what stays quiet, and
    // dropping that guard must have a non-zero value to leak.
    void noteReckonSample(int64_t stc90k, int64_t nowMs) {
        const bool havePrev = reckonLastStc >= 0;
        const int64_t prevStc = reckonLastStc;
        const int64_t prevAt = reckonLastAtMs;
        reckonLastStc = stc90k;
        reckonLastAtMs = nowMs;
        if (!havePrev || nowMs <= prevAt)
            return;
        const int64_t dt = nowMs - prevAt;
        const int64_t moved = circularDelta(stc90k, prevStc);
        if (moved < (dt * 90) / 2) {
            noteReckonStop(prevAt);
            return;
        }
        if (reckonFromMs < 0)
            reckonFromMs = prevAt;
    }

    void rememberPreSeek(int64_t stc90k, int64_t nowMs) {
        preSeekStc = stc90k;
        preSeekAtMs = nowMs;
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
        oldHigh90 = 0;
        oldSeenAtMs = nowMs;
#endif
    }

    // How far past the pre-seek STC the old clock could be. Wall time since that
    // sample, plus 200 ms of slack. A low, a freeze, a pause, and a gap with no
    // sample all stay inside it.
    int64_t oldRunCeiling90(int64_t nowMs) const {
#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
        // Mutant: grow only while a running sample was observed, by the gap
        // since that sample plus 200 ms. An old clock that advanced unseen
        // falls off, and the 5 s path takes it at any lead.
        int64_t gap90 = 0;
        if (oldSeenAtMs >= 0 && nowMs > oldSeenAtMs)
            gap90 = (nowMs - oldSeenAtMs) * 90;
        return oldHigh90 + gap90 + kPreSeekTol90k;
#else
        if (nowMs <= preSeekAtMs)
            return kPreSeekTol90k;
        return (nowMs - preSeekAtMs) * 90 + kPreSeekTol90k;
#endif
    }

#ifdef MUT_OWENB_SESS_ANCHOR_RUNGROW
    void noteOldClock(int64_t stc90k, bool running, int64_t nowMs) {
        if (preSeekStc < 0)
            return;
        if (running && stc90k >= 0) {
            const int64_t d = circularDelta(stc90k, preSeekStc & kMask);
            if (d >= -kPreSeekTol90k && d <= oldRunCeiling90(nowMs) && d > oldHigh90)
                oldHigh90 = d;
        }
        oldSeenAtMs = nowMs;
    }
#endif

    // Start of the run this sample belongs to. A drop begins one here, and so
    // does a step more than 100 ms off 1x over both the wall gap and the
    // unpaused gap. A hold, a frame quantum, a few milliseconds of jitter, or
    // a pause that froze the clock does not.
    int64_t runStartFor(int64_t stc90k, int64_t nowMs) const {
        if (runLastStc < 0 || runFromMs < 0)
            return nowMs;
        const int64_t dt = nowMs - runLastAtMs;
        if (dt <= 0)
            return runFromMs;
        const int64_t moved = circularDelta(stc90k, runLastStc);
        const int64_t dev = moved - dt * 90;
        // A clock that stopped with the user's pause matches 1x once that
        // pause is taken out of the gap. A clock that kept running matches
        // the wall gap. Either one is the same run.
        const int64_t gapPaused = pausedAccumMs - runLastPausedMs;
        const int64_t devPaused = moved - (dt - gapPaused) * 90;
        if ((dev > kRunSlack90k || dev < -kRunSlack90k) &&
            (gapPaused <= 0 || devPaused > kRunSlack90k || devPaused < -kRunSlack90k))
            return nowMs;
#ifdef MUT_OWENB_SESS_ANCHOR_HALFSPEED
        // Mutant: one sample slower than half speed restarts the run, so a
        // frozen tick or a quantized hold near the keyframe accepts the old clock.
        if (moved < (dt * 90) / 2)
            return nowMs;
#endif
        return runFromMs;
    }

    // The run's starting STC against the keyframe, within 500 ms. A fresh
    // clock that began at the keyframe does. A stale clock is off by about
    // the skip. A frozen sample does not invent a new start. This is not the
    // bar's accumulated guess.
    bool agreesWithRun(int64_t delta, int64_t nowMs, int64_t stc90k) const {
        const bool fresh = runStartFor(stc90k, nowMs) == nowMs || runStartStc < 0;
        const int64_t err = fresh ? delta : delta - circularDelta(stc90k, runStartStc);
        return err <= kAnchorTol90k && err >= -kAnchorTol90k;
    }

    // True when this STC is still the picture the seek left: within 200 ms
    // behind the pre-seek sample, or up to wall time since then plus 200 ms.
    bool onPreSeekPath(int64_t stc90k, int64_t nowMs) const {
        if (preSeekStc < 0)
            return false;
        const int64_t d = circularDelta(stc90k, preSeekStc & kMask);
        return d >= -kPreSeekTol90k && d <= oldRunCeiling90(nowMs);
    }

    // A fresh clock is ahead of the keyframe by at most the time since the flush.
    // A running stale clock stays about a seek-distance ahead, so the same bound
    // keeps rejecting it, and a lead past the tolerance is also rejected when it
    // is still the pre-seek clock. The 2 s timeout is a capped backstop for a
    // clock that restarted after the flush. The 5 s last resort takes a fresh
    // clock the cap or a missing drop would otherwise leave on the landing.
    bool acceptAnchor(int64_t stc90k, int64_t delta, int64_t nowMs) const {
        const int64_t elapsed90 = elapsedSinceFlushMs(nowMs) * 90;
#ifdef MUT_OWENB_SESS_ANCHOR_OPEN
        // Mutant: any STC at or ahead of the keyframe is the new picture, so a
        // pre-seek clock that is still running ahead is accepted at once.
        (void)stc90k;
        (void)elapsed90;
        (void)nowMs;
        return delta >= -kAnchorTol90k;
#else
        // On the old path and far from this run's picture: still the old clock,
        // whatever the lead. A sample that agrees with the run is the new
        // picture even when the two clocks overlap.
        const bool stale = onPreSeekPath(stc90k, nowMs) && !agreesWithRun(delta, nowMs, stc90k);
        if (!stale && delta >= -kAnchorTol90k && delta <= elapsed90 + kAnchorTol90k)
            return true;
        if (stcRunSinceMs < 0 || nowMs - stcRunSinceMs < kAnchorTimeoutMs)
            return false;
        if (delta < -kAnchorTol90k || stale)
            return false;
#ifdef MUT_OWENB_SESS_ANCHOR_NO_DROP
        const bool dropOk = true; // mutant: the 2 s path does not need a post-flush drop
#else
        const bool dropOk = sawClockDrop;
#endif
#ifdef MUT_OWENB_SESS_ANCHOR_NO_CAP
        const bool withinCap = true; // mutant: the 2 s path has no lead cap
#else
        const bool withinCap = delta <= elapsed90 + kAnchorTimeoutCap90k;
#endif
        if (dropOk && withinCap)
            return true;
        // Short STC_RUN drops are not bridged into this stretch. A drop is how
        // the timeout tells a restart from a stall; bridging it would take the
        // resumed old clock sooner. The reckon cap bounds a flap that never
        // quite reaches this long a run.
        return nowMs - stcRunSinceMs >= kAnchorLastResortMs;
#endif
    }
};

enum class EndReason : uint8_t { None, Eof, Stopped, NeedsTranscode, CoreLost, RingError, SourceError };
const char* toString(EndReason r);
// P0 11-10 (reviewer): on a Spelunkr channel these failure ends hand off to the failed-item skip
// (EOF has its own advance branch in owenb_play.cpp), so the session end keeps the held frame and the
// tuning banner (PLAY-PENDING) and never paints the "Open the Guide" idle card in between.
// CoreLost / RingError still land (the core is gone or broken; no skip follows).
inline bool spelunkrFailEndHoldsFrame(EndReason r) {
    return r == EndReason::NeedsTranscode || r == EndReason::SourceError;
}

// NET_RESUME overlay. active: the bar shows RECONNECTING attempt/maxTries.
// reconnected: the 3s toast. Both false clears the banner.
struct RecoverStatus {
    bool active = false;
    int attempt = 0;
    int maxTries = 8;
    int64_t targetMs = 0;
    bool reconnected = false;
};

// Plex volume 0..100 to the core's attenuation, 0.5 dB steps (design Q7): 0 mutes (255), 100 is 0,
// else min(254, round(-40 log10(v / 100))), so 50 -> 12 (-6 dB).
unsigned attenuationForVolume(int volume);

// The PPS's entropy_coding_mode_flag in an Annex-B AU: 1 CABAC, 0 CAVLC, -1 no (parsable) PPS.
int ppsEntropyCabac(const uint8_t* au, size_t len);

struct SessionConfig {
    bool holdLastPicture = true;        // IDLE_SCREEN=last (default); false = black (flush) at stop / end
    // RAOP's spectrum is one frame a second. Clearing the idle card at start
    // shows the latched mp4 until that frame. Keep the card until the first picture.
    bool keepIdleUntilPicture = false;
    unsigned cavlcMaxKbps = 4000;       // OB_CAVLC_MAX_KBPS; 0 = no rate guard
    unsigned cavlcWindowMs = 2000;      // the guard's measuring window, in PTS time
    // v2-bugfix ( lab bug 1): the guard ends a CAVLC stream only when the rate is above cavlcMaxKbps AND the
    // library's rewrite (timed around each CAVLC AU write) costs more than this share of one A9 over the
    // window. A stream whose rewrite keeps real time plays on. 0 = the rate alone decides (the T5 rule).
    // OB_CAVLC_MAX_LOAD_PCT. Design Q6: 70 % is the A9 budget next to the demux.
    unsigned cavlcMaxLoadPct = 70;
    // BUG1-CAP.md: above this the play ends whatever the measured load (a backstop for a clock that lies):
    // ~7.7 M bins/s x ~100 ns + the per-macroblock part is ~80 % of one A9 even at 24p. OB_CAVLC_HARD_MAX_KBPS.
    unsigned cavlcHardMaxKbps = 7000;
    // BUG1 (lab 7AE78D80): what the stream's entropy turned out to be, for the per-PMS memory
    // (owenb_pms_traits.hpp): (false, 0, 0) once when the PPS says CABAC; (true, kbps, load %) at each CAVLC
    // window whose load is known (also the one that ends the play). Called on the play thread.
    std::function<void(bool cavlc, unsigned kbps, unsigned loadPct)> onEntropy;
    std::function<int64_t()> nowNs;     // the rewrite timer's clock (steady_clock when unset; tests inject one)
    unsigned pcmRecordBytes = 3840;     // PCM records of at most 20 ms (48 kHz x 4 bytes x 0.02 s)
    unsigned backoffUs = 2000;          // a full ring: wait, then try again
    unsigned idleUs = 10000;            // paused / draining: the poll period
    unsigned drainTimeoutMs = 30000;    // end of input: give up when the core made NO progress for this long
    unsigned drainMaxMs = 300000;       // end of input: absolute cap on the drain, progress or not
    // The loop's sleep. The daemon's is a real sleep; a test's advances its fake core.
    std::function<void(unsigned)> sleepUs;
    // Called once per loop iteration (tests inject commands here; the daemon leaves it unset).
    std::function<void()> onIteration;
    std::function<void(const std::string&)> log;
    // GCAST open retry: lounge BUFFERING, overlay stays off the idle card.
    std::function<void(int64_t posMs)> onBuffering;
    // Unused by the burn-in path. Left so a caller can still clear a caption on seek.
    std::function<void(const std::string& text)> onSubtitle;
    // Soft-cue clock. Called with mediaMs() (landing + STC), never the bar hold.
    // packetText is the embedded cue covering that media time when SOFT_SUBS is on,
    // otherwise empty. Sidecar cues stay on the sink.
    std::function<void(int64_t mediaMs, const std::string& packetText)> onCaptionClock;
    // Plex subtitle stream id for the caption sink (0 = off). Not a container index.
    std::function<void(int subtitleStreamId)> onSubtitleSelect;
    // The container audio index libav actually opened (after open and after a seek).
    std::function<void(int index)> onAudioOpened;
    // In-place audio switch finished. Applied is the index now open. Anything else kept the previous one.
    std::function<void(int index, StreamSwitch result)> onAudioSwitch;
    // In-place audio switch could not be done and the input is not a container we can seek
    // (a PMS URL carries its own offset). The player reopens at this position.
    std::function<void(int64_t positionMs)> onStreamRestart;
    // TRON-542 (2026-10-25,  lab): a PMS transcode ended its HTTP stream at 5:42 of a 95 min film
    // (libav: "File ended prematurely", then AVERROR_EOF). The session took that for a clean end of
    // the title and the play stopped. When the input reports EOF while the known duration is more
    // than prematureNearEndMs away, the session calls onPrematureEof(resumeMs, attempt) and keeps
    // playing: the callback re-requests the input at resumeMs (the player queues a seek, which for a
    // PMS transcode bakes offset= into a new start URL). It returns false when it will not resume
    // (not a PMS URL); the end is then the old clean Eof. At most prematureResumeMax resumes in
    // prematureResumeWindowMs; the next premature EOF ends SourceError, never a clean end. No resume
    // when the duration is unknown (durationMs unset or <= 0), after stop(), or with a command
    // pending (a user seek wins).
    std::function<int64_t()> durationMs;
    std::function<bool(int64_t resumeMs, unsigned attempt)> onPrematureEof;
    unsigned prematureResumeMax = 3;
    unsigned prematureResumeWindowMs = 300000;
    unsigned prematureNearEndMs = 10000;
    // fr-resume-1025 (glass 1025: a direct-play PMS Part died at 5:20 with libav "Stream ends
    // prematurely at 258415018, should be 2873254601" = AVERROR(EIO), a read ERROR not an EOF, and the
    // play ended SourceError with no reopen). The same resume (cap, window, near-end rule) now also
    // answers a read Error; a reopen after an Error waits attempt * this many ms first (stop cuts it).
    unsigned prematureErrorBackoffMs = 1000;
    // #231: a premature-EOF reopen that fails on transport retries 1,2,4,8 s inside this window
    // (MPX_HTTP_RANGE_RESUME=0 turns it off). Tests shrink it.
    int64_t reopenRetryBudgetMs = 20000;
    int64_t reopenAttemptCapMs = 5000;   // per reopen attempt: min(cap, max(1000, budget left))
    // rc-1034 (glass 1033, AirPlay URL clip end): xcode finishes early and closes the loopback stream, so libav
    // reports EIO (a read ERROR) after the whole clip was delivered. When set, a read Error whose input already
    // reached within prematureNearEndMs of the known duration is the clean end (inputEof_: drain, EOS, hold the
    // last frame), not SourceError. An Error further from the end stays SourceError. Needs a known duration.
    bool nearEndErrorIsEof = false;
    // NET_RESUME. Off (the default) leaves read errors and early EOF on the path above
    // (TRON-542 / fr-resume-1025 when onPrematureEof is set, otherwise SourceError).
    // On: a recoverable fault reopens at the last good picture; a fatal fault stays fatal.
    bool netResume = false;
    uint32_t netResumeSeed = 1;
    std::function<void(const RecoverStatus&)> onRecover;
};

struct SessionStats {
    uint64_t videoAus = 0, audioRecords = 0, audioBytes = 0;
    uint64_t droppedBeforeKey = 0, droppedAudioBeforeKey = 0;
    uint64_t flushes = 0, seeks = 0, pauses = 0, resumes = 0, fullWaits = 0;
    uint64_t cavlcAus = 0;
    bool eosSent = false;
};

class OwenbPlaySession {
public:
    OwenbPlaySession(OwenbRing& ring, SourceFactory factory, SessionConfig cfg);
    OwenbPlaySession(const OwenbPlaySession&) = delete;
    OwenbPlaySession& operator=(const OwenbPlaySession&) = delete;

    // The play thread: opens req, begins session sessionId on the ring, plays to the end or a stop.
    EndReason run(const OpenRequest& req, uint64_t sessionId);

    // Any thread.
    void pause();
    void resume();
    void seek(const OpenRequest& req);
    void stop();
    void setVolumePercent(int volume);
    // Play thread applies it between reads. setAudio / setSubtitle say which index is meaningful.
    // textContainerIndex: -2 leave the packet decoder alone (SOFT_SUBS off / sidecar
    // path). -1 close it. >=0 select that container index. Play thread only.
    void selectStreams(int audioIndex, bool setAudio, int subtitleIndex, bool setSubtitle,
                       int textContainerIndex = -2, misterplex::PlexTextIdentity text = {});

    // Reported position: the bar, the companion timeline, and the base of the next skip.
    // A short-forward hold lives here until the picture reaches the target.
    int64_t positionMs() const {
        return static_cast<int64_t>(packed_.load(std::memory_order_acquire) & 0xffffffffu);
    }
    // True media time of the picture on screen. The bar is positionMs(). This
    // is the time a subtitle track should follow; the daemon does not read it
    // yet. The milliseconds are the keyframe a container seek lands on, then
    // each accepted STC sample. The short-forward hold and the unanchored
    // advance never write this. Phase 0 (the stamp's low 8 bits) means the
    // landing is not committed yet and the milliseconds are still the previous
    // picture. Phase 1 is the landing. Phase 2 is a live sample.
    int64_t mediaMs() const {
        return static_cast<int64_t>(mediaPacked_.load(std::memory_order_acquire) & 0xffffffffu);
    }
    // stamp and milliseconds from one atomic load (a seek epoch cannot tear off its position)
    void readPosition(int64_t& ms, uint32_t& stamp) const {
        const uint64_t p = packed_.load(std::memory_order_acquire);
        stamp = static_cast<uint32_t>(p >> 32);
        ms = static_cast<int64_t>(p & 0xffffffffu);
    }
    // Same packing as readPosition. ms is mediaMs()'s contract: the landing,
    // never the bar's hold and never its guess.
    void readMedia(int64_t& ms, uint32_t& stamp) const {
        const uint64_t p = mediaPacked_.load(std::memory_order_acquire);
        stamp = static_cast<uint32_t>(p >> 32);
        ms = static_cast<int64_t>(p & 0xffffffffu);
    }
    bool paused() const { return userPaused_.load(); }
    bool started() const { return started_.load(); }              // the core showed this stream's first picture
    bool decodedPicture() const { return decodedPic_.load(); }    // #207 real first picture (RC_PIC moved)
    bool running() const { return running_.load(); }
    std::string error() const;
    SessionStats stats() const;
    size_t queuedBytes() const { return outBytes_; }
    size_t outDepth() const { return outDepth_.load(); }
    size_t readyDepth() const;
    unsigned probeBound() const;
    // Backpressure cap for out_. A sender that interrupts the ring write must not
    // keep reading the elementary stream into this deque.
    static constexpr size_t kOutByteCap = 4u * 1024u * 1024u;

    // Callbacks (set before run; called on the play thread).
    std::function<void()> onFirstPicture;                 // once per run (play started)

private:
    struct Cmd {
        enum class K : uint8_t { Pause, Resume, Seek, Stop, Volume, Streams } k;
        OpenRequest req;
        int volume = 100;
        uint32_t epoch = 0;                 // seek: the generation seek() published
        int64_t prevMs = 0;                 // seek: the position on screen when it was queued
        int audioIndex = -1;
        int subtitleIndex = -1;
        int textContainerIndex = -2;
        misterplex::PlexTextIdentity textId;
        bool setAudio = false;
        bool setSubtitle = false;
    };
    enum class Push : uint8_t { Ok, Interrupted, Fatal };

    bool openSource(const OpenRequest& req, int64_t* playedMs = nullptr);
    bool takeCommands();                        // false: the session ends (stop, fatal)
    bool interruptPending() const;
    bool applySeek(const Cmd& c);
    void publishPacked(std::atomic<uint64_t>& word, uint32_t epoch, uint8_t phase, int64_t ms);
    void publishPosition(uint32_t epoch, uint8_t phase, int64_t ms);
    void publishMedia(uint32_t epoch, uint8_t phase, int64_t ms);
    bool checkVideoIn(const MediaPacket& p);   // false: end_ set (the CAVLC rate guard)
    bool readOne();                             // false: end_ set (error); inputEof_ at the end
    enum class EarlyEof { NotEarly, Resumed, Capped };
    bool inputNearEnd() const;   // rc-1034: the input already reached the end window
    EarlyEof classifyEof(bool fromError = false);                     // TRON-542: an EOF far from the duration is not an end
    enum class ResumeStep : uint8_t { Off, Natural, Fatal, Ended, Continue };
    bool netResumeOn() const;
    bool netResumeEligible(const OpenRequest& req) const;
    ResumeStep tryNetResume(bool eof, const std::string& err);
    bool openQuiet(const OpenRequest& req, std::string& err);
    void finishResumeOpen(const OpenRequest& req, uint32_t epoch, int64_t prevMs);
    bool waitBudget(int64_t ms);
    void giveUpNet(int64_t goodMs, const std::string& why);
    void noteNetRecovered();
    void armStallWatch();
    int64_t lastAuMs() const;
    int64_t resumePictureMs() const;
    void publishRecover(const RecoverStatus& st);
    bool sendReady(bool all);                   // false: end_ set or interrupted
    Push pushVideo(const MediaPacket& p, bool eos);
    Push pushAudio(const MediaPacket& p);
    Push pushWithBackoff(const std::function<h264stream::PushResult()>& f);
    void fatalFromRing(const char* what);
    void endWith(EndReason r, const std::string& why);
    void updatePosition();
    void publishSubtitle();
    bool drainToEnd();
    void finish();
    void logLine(const std::string& s) const;
    void sleep(unsigned us);
    int64_t nowNs() const;
    int64_t nowMs() const;

    OwenbRing& ring_;
    SourceFactory factory_;
    SessionConfig cfg_;
    uint64_t sid_ = 0;
    uint32_t seq_ = 0;

    mutable std::mutex cmdMu_;
    std::deque<Cmd> cmds_;
    std::atomic<int> pendingCmds_{0};

    mutable std::mutex srcMu_;
    std::unique_ptr<IMediaSource> src_;
    OpenRequest currentReq_{};
    std::string shownSubtitle_;

    std::deque<MediaPacket> out_;               // read, not yet written (container order)
    std::atomic<size_t> outDepth_{0};
    size_t outBytes_ = 0;
    void syncOutDepth() { outDepth_.store(out_.size()); }
    bool needKey_ = true;
    bool flushBeforeKey_ = false;               // a held seek: flush the ring before its first key
    bool inputEof_ = false;
    int64_t srcBaseMs_ = 0;                     // media ms of the current input's first video PTS
    int64_t srcFirstPts_ = -1, srcLastPts_ = -1;   // video PTS span read from the current input
    std::atomic<bool> reopenRetryArmed_{false};   // the next openSource is a premature-EOF reopen
    std::deque<int64_t> resumeAtMs_;            // steady ms of each premature-EOF resume (the retry window)
    std::optional<misterplex::netresume::Backoff> netBackoff_;
    bool netEpisode_ = false;                   // a NET_RESUME fault is still being retried
    bool netSeekTook_ = false;                  // a user seek during that wait already reopened
    bool stallShown_ = false;
    bool corePaused_ = false;                   // the ring's pause word is set
    int entropyCabac_ = -1;                     // 1 CABAC, 0 CAVLC, -1 unknown
    int64_t rateStartPts_ = -1;
    uint64_t rateBytes_ = 0;
    int64_t rwFirstPts_ = -1, rwLastPts_ = -1;   // BUG1-CAP: the PTS span of the CAVLC AUs timed this window
    int64_t rewriteNs_ = 0;                     // time spent writing CAVLC AUs (the library rewrite) this window
    bool cavlcOverRateLogged_ = false;
    PlaybackClock clock_;
    std::atomic<uint32_t> seekEpoch_{1};    // bumped in seek(), before the play thread applies it
    // High 32: posStamp. Low 32: position ms. The publication readers (positionMs) see.
    std::atomic<uint64_t> packed_{0};
    // True media time, packed like packed_: high 32 is posStamp, low 32 is ms.
    // seek()'s phase 0 keeps the previous picture's milliseconds.
    std::atomic<uint64_t> mediaPacked_{0};
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "position publication must not lock");
    int64_t lastVideoPts_ = -1;
    size_t videoInOut_ = 0;                     // video packets in out_
    size_t audioOffset_ = 0;                    // bytes of out_'s audio packet already written
    uint64_t drainStartUs_ = 0;                 // end of input: time since the core last made progress
    uint64_t drainTotalUs_ = 0;                 // end of input: total time spent waiting (cap)
    uint32_t drainLevel_ = UINT32_MAX;          // end of input: ring level at the last poll
    int volume_ = 100;

    EndReason end_ = EndReason::None;
    mutable std::mutex errMu_;
    std::string error_;
    mutable std::mutex statsMu_;
    SessionStats stats_;

    std::atomic<bool> userPaused_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> decodedPic_{false};
    uint32_t picsAtOpen_ = 0;  // #207: RC_PIC at the first status sample
    bool picsAtOpenSet_ = false;
    bool firstPicLogged_ = false;
    bool noPicWarned_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopReq_{false};
    std::atomic<int64_t> stopAtMs_{-1};        // nowMs() at stop(), for the end stamp
};

} // namespace misterplex::owenb
