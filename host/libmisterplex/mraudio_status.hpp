#pragma once
// Parse the status line exposed by MiSTer's MrAudio driver.
//
// /dev/MrAudio is a 512 KB DMA ring (sound/drivers/MiSTer-audio-spi.c). Its
// write path NEVER blocks and never consults the read pointer — it just copies
// into the ring and wraps. Two consequences drive this file:
//
//   1. write() returning tells you NOTHING about playback. Measured on the lab
//      unit, 10 s of PCM is accepted in 116 ms. So a clock built from bytes
//      *submitted* runs ahead of the audio you actually hear, which is why the
//      video used to need a hand-tuned delay to compensate.
//   2. Because there is no backpressure, feeding faster than realtime silently
//      overwrites audio that has not been played yet. Nothing reports this.
//
// The driver's open() handler does an spi_read of the FPGA's read pointer and
// formats one line:
//
//     rptr: 234576, wptr: 234576, len:      0, comp: 0
//
// `len` is (wptr - rptr) wrapped — bytes not yet requested from DDR. ALSA
// advances rptr before the response and prefetches two stereo samples, so this
// is a queue/overwrite bound, NOT an exact consumed clock or session ACK.
// The FPGA-H264 path uses audio_session.hpp feedback for actual consumption.
// open()/release do not reset or move the DMA origin. Only device_write()
// publishes full SPI buffer metadata; there is no periodic publisher to wait
// for after quiescing the producer. Audio control carries the stable wptr.

#include <cstdint>

namespace misterplex {

// s16le stereo @ 48 kHz — the only format the pump feeds.
constexpr int64_t kMrAudioBytesPerSec = 48000LL * 4LL;
inline constexpr int kAudioPrefillDefaultMs = 100; // kFeedTargetBytes / 192000 * 1000
inline constexpr int kAudioPrefillMaxMs = 200;
inline int64_t audioPrefillBudgetBytes(int ms) {
    if (ms <= 0)
        return 0;
    if (ms > kAudioPrefillMaxMs)
        ms = kAudioPrefillMaxMs;
    return (static_cast<int64_t>(ms) * kMrAudioBytesPerSec) / 1000;
}

// Size of the driver's DMA ring (BUFFER_LEN). Anything at or beyond this is
// nonsense and means we misparsed.
constexpr int64_t kMrAudioRingBytes = 512LL * 1024LL;

struct MrAudioStatus {
    int64_t readPointer = -1;
    int64_t writePointer = -1;
    int64_t queuedBytes = -1;

    bool valid() const {
        return readPointer >= 0 && writePointer >= 0 && queuedBytes >= 0;
    }
};

inline int64_t parseMrAudioUnsignedField(const char* s, int64_t n,
                                         const char* key, int64_t keyLen,
                                         bool allowRingEnd) {
    if (!s || !key || n <= 0 || keyLen <= 0)
        return -1;
    for (int64_t i = 0; i + keyLen <= n; ++i) {
        bool hit = true;
        for (int64_t k = 0; k < keyLen; ++k) {
            if (s[i + k] != key[k]) {
                hit = false;
                break;
            }
        }
        if (!hit)
            continue;
        int64_t j = i + keyLen;
        while (j < n && (s[j] == ' ' || s[j] == '\t'))
            ++j;
        if (j >= n || s[j] < '0' || s[j] > '9')
            return -1;
        int64_t value = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            value = value * 10 + (s[j] - '0');
            if (value > kMrAudioRingBytes)
                return -1;
            ++j;
        }
        if (allowRingEnd ? value > kMrAudioRingBytes : value >= kMrAudioRingBytes)
            return -1;
        return value;
    }
    return -1;
}

inline MrAudioStatus parseMrAudioStatus(const char* s, int64_t n) {
    MrAudioStatus out;
    out.readPointer = parseMrAudioUnsignedField(s, n, "rptr:", 5, true);
    out.writePointer = parseMrAudioUnsignedField(s, n, "wptr:", 5, true);
    out.queuedBytes = parseMrAudioUnsignedField(s, n, "len:", 4, false);
    if (!out.valid())
        return {};
    return out;
}

// Pull the `len:` field out of the driver's status line.
// Returns bytes queued, or -1 if the line is absent/malformed/out of range.
// Deliberately hand-rolled: this parses a kernel string on a hot path and must
// not throw, allocate, or depend on locale.
inline int64_t parseMrAudioQueuedBytes(const char* s, int64_t n) {
    return parseMrAudioUnsignedField(s, n, "len:", 4, false);
}

// Continue polling the MrAudio ring during EOS drain.
// queuedBytes is parseMrAudioQueuedBytes: -1 unknown, 0 empty, >0 still playing.
// userStop true (MediaPlayer::stop_) must not wait the ring out.
inline bool eosAudioDrainShouldWait(int64_t queuedBytes, bool userStop) {
    if (userStop)
        return false;
    return queuedBytes > 0;
}

// Legacy playback estimate (ms) = what we handed the driver, minus what is
// still sitting in the ring. `queuedBytes` < 0 means "unknown", in which case we
// fall back to the submitted-byte clock so a kernel without this line still
// plays (just with the old hand-tuned offset).
inline int64_t audibleClockMs(int64_t writtenBytes, int64_t queuedBytes) {
    int64_t played = writtenBytes;
    if (queuedBytes >= 0)
        played -= queuedBytes;
    if (played <= 0)
        return 0;
    return (played * 1000LL) / kMrAudioBytesPerSec;
}

inline int64_t audibleClockUs(int64_t writtenBytes, int64_t queuedBytes) {
    int64_t played = writtenBytes;
    if (queuedBytes >= 0)
        played -= queuedBytes;
    if (played <= 0)
        return 0;
    return (played * 1000000LL) / kMrAudioBytesPerSec;
}

// ---------------------------------------------------------------------------
// Feed-rate servo
// ---------------------------------------------------------------------------
//
// The ring has no backpressure, so nothing stops the pump from feeding faster
// (or slower) than the FPGA actually plays. Any mismatch integrates directly
// into the ring depth: measured on the lab unit, an open-loop feed trimmed by
// AUDIO_CLOCK_PPM=+685 grew the ring by ~255 B/s (~1330 ppm too fast), i.e.
// ~80 ms/min, which overruns the 512 KB ring roughly 30 minutes into a
// 45-minute episode and silently shreds unplayed audio.
//
// That constant was not wrong through carelessness: it was measured over HDMI
// capture back when video paced off *submitted* bytes, so a growing ring was
// indistinguishable from a fast playback clock. The two only separate once the
// read pointer is visible.
//
// So do not trim the feed with a constant at all. The ring depth IS the error
// signal: hold it at a target by adjusting the feed rate. Depth is the integral
// of (feed - drain), so proportional control here is a stable first-order loop
// with time constant kFeedServoTauSec and no steady-state error in *rate* (the
// residual appears as a small constant depth offset, which is harmless — only
// its stability matters, because the audible clock subtracts it anyway).
//
// This also removes the per-board calibration: whatever the real audio clock
// is, the loop finds it.

// Ring depth the servo aims to hold, in bytes (~100 ms). Big enough to ride out
// scheduler jitter on a 20 ms chunk cadence, small enough that a pause or seek
// does not leave a long tail of stale audio to flush.
inline constexpr int64_t kFeedTargetBytes = kMrAudioBytesPerSec / 10;

// 240p FPGA pump drains pcm[3840] = 20 ms of s16le stereo @ 48 kHz.
// 6 chunks = 120 ms; same byte cap as host-paint kMaxQ, tighter than QueueHigh 200 ms.
inline constexpr int64_t kAudioQueueBound240pBytes = (kMrAudioBytesPerSec * 12) / 100;

inline bool audioQueueBound240p(int outW, int outH, int64_t queuedBytes) {
    if (outW != 320 || outH != 240)
        return false;
    return queuedBytes > kAudioQueueBound240pBytes;
}

// Loop time constant, seconds. A depth error decays with roughly this time
// constant.
inline constexpr double kFeedServoTauSec = 8.0;

// Hard limit on how far the servo may bend the feed rate, as a fraction.
// 5% (~50000 ppm) still far beyond crystal error, but drains a 480i host-paint
// startup overshoot in a few seconds instead of a minute at the old 1% cap.
inline constexpr double kFeedServoMaxFrac = 0.05;

// Feed rate (bytes/sec) for the next chunk.
//   nominalBytesPerSec - 48 kHz stereo s16le, optionally pre-trimmed by
//                        AUDIO_CLOCK_PPM (now only a seed; the loop converges
//                        from any sane starting point).
//   queuedBytes        - smoothed ring depth, or < 0 if unknown.
// With an unknown depth the servo disengages and the caller gets the open-loop
// rate, preserving the old behaviour on a kernel lacking the status line.
inline double feedRateBytesPerSec(double nominalBytesPerSec, int64_t queuedBytes) {
    if (queuedBytes < 0)
        return nominalBytesPerSec;
    const double err = static_cast<double>(queuedBytes - kFeedTargetBytes);
    double corr = -err / kFeedServoTauSec;
    const double lim = nominalBytesPerSec * kFeedServoMaxFrac;
    if (corr > lim)
        corr = lim;
    if (corr < -lim)
        corr = -lim;
    const double r = nominalBytesPerSec + corr;
    return r < 1.0 ? 1.0 : r;
}

} // namespace misterplex
