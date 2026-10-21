#pragma once
// A/V presentation clock for the product cast path.
//
// The raw RGB pipe carries no PTS, so `frameIndex` is the only notion of video
// time. That makes the assumed content rate load-bearing: pacing 23.976 fps
// content at 24 fps makes video lead audio by ~1 ms/s, which is invisible in a
// 12 s test clip but reaches ~234 ms by 3:54 and ~5.5 s across a 91-minute
// episode. Everything here is exact integer math on a rational rate.

#include "fabric_direct.hpp"
#include <cstdint>

namespace misterplex {

// Default rate when PMS metadata gives us nothing. The drift corrector absorbs
// the error, but log it — an unknown rate means degraded lipsync.
constexpr int kDefaultFpsNum = 24;
constexpr int kDefaultFpsDen = 1;

// kDefaultPresentLeadMs (40) lives in fabric_direct.hpp next to
// avResyncDropMsForPresent so 480p hold-only present and tests share one symbol.

// Content timestamp (ms) of frame `frameIndex` at rate num/den.
// frameIndex is 1-based (the frame just presented), matching the present loop.
// int64 throughout: 131 400 frames * 1000 * 1001 stays far inside the range.
inline int64_t frameContentMs(int64_t frameIndex, int num, int den) {
    if (num <= 0 || den <= 0) {
        num = kDefaultFpsNum;
        den = kDefaultFpsDen;
    }
    return (frameIndex * 1000LL * static_cast<int64_t>(den)) / static_cast<int64_t>(num);
}

// Microsecond form for paths whose display period is too close to the source
// period for integer milliseconds to preserve one-frame-per-VSync eligibility.
// Truncation remains bounded below 1 us and never accumulates.
inline int64_t frameContentUs(int64_t frameIndex, int num, int den) {
    if (num <= 0 || den <= 0) {
        num = kDefaultFpsNum;
        den = kDefaultFpsDen;
    }
    return (frameIndex * 1000000LL * static_cast<int64_t>(den)) /
           static_cast<int64_t>(num);
}

// Wall-clock playback time excluding time intentionally spent paused.
inline int64_t activePlaybackClockUs(int64_t wallUs, int64_t pausedUs) {
    if (wallUs <= 0)
        return 0;
    if (pausedUs <= 0)
        return wallUs;
    return pausedUs >= wallUs ? 0 : wallUs - pausedUs;
}

struct CompressedPacingSnapshot {
    bool paused = false;
    int64_t activeUs = 0;
    int64_t lastPresentationActiveUs = 0;
    int64_t mediaUs = 0;
};

enum class CompressedPacingResult { Due, Cancelled, Stalled };

template<class Continue, class Poll, class Snapshot, class Wait>
CompressedPacingResult waitForCompressedAccessUnit(
    long double targetUs, Continue current, Poll poll, Snapshot snapshot, Wait wait,
    int64_t leadUs = 40000, int64_t timeoutUs = 2000000) {
    while (current()) {
        if (!poll()) return CompressedPacingResult::Cancelled;
        const auto clock = snapshot();
        if (!clock.paused && targetUs <= clock.mediaUs + leadUs)
            return CompressedPacingResult::Due;
        if (!clock.paused &&
            clock.activeUs - clock.lastPresentationActiveUs > timeoutUs)
            return CompressedPacingResult::Stalled;
        wait();
    }
    return CompressedPacingResult::Cancelled;
}

// Audio master clock (ms) from bytes handed to MrAudio (s16le stereo @ 48 kHz).
inline int64_t audioClockMs(int64_t audioBytes) {
    if (audioBytes <= 0)
        return 0;
    return (audioBytes * 1000LL) / (48000LL * 4LL);
}

inline int64_t audioClockUs(int64_t audioBytes) {
    if (audioBytes <= 0)
        return 0;
    return (audioBytes * 1000000LL) / (48000LL * 4LL);
}

// drift = audio clock − content time of the frame about to be shown.
//   drift < 0  video is ahead of audio (audio sounds late) → hold
//   drift > 0  video is behind audio (we are late) → drop to catch up
inline int64_t avDriftMs(int64_t audioMs, int64_t frameMs) { return audioMs - frameMs; }

enum class AvAction {
    Present, // show this frame
    Hold,    // wait, video is running ahead of the master clock
    Drop,    // skip presenting, we are too far behind to catch up by waiting
};

enum class PlaybackTerminalState {
    None,    // explicit stop teardown already owns the terminal report
    Ended,   // natural EOF with delivered content; auto-next is allowed
    Stopped, // empty or failed session; never auto-next
};

inline PlaybackTerminalState classifyPlaybackTerminalState(bool stopRequested,
                                                            bool pipelineAborted,
                                                            bool hadContent) {
    if (stopRequested)
        return PlaybackTerminalState::None;
    if (pipelineAborted || !hadContent)
        return PlaybackTerminalState::Stopped;
    return PlaybackTerminalState::Ended;
}

inline bool shouldBoundedReopen(bool stopRequested,
                                bool goldRaster,
                                int64_t posMs,
                                int64_t startMs) {
    if (stopRequested)
        return false;
    if (!goldRaster)
        return false;
    if (posMs <= 0 || posMs <= startMs)
        return false;
    return true;
}

inline PlaybackTerminalState classifyFpgaPlaybackTerminalState(
    bool currentGeneration, bool stopRequested, bool naturalEof, bool fullyDrained,
    bool released, bool hadPresentation) {
    if (!currentGeneration) return PlaybackTerminalState::None;
    return !stopRequested && naturalEof && fullyDrained && released && hadPresentation
        ? PlaybackTerminalState::Ended : PlaybackTerminalState::Stopped;
}

template<class Current, class Stopped, class Progress>
void reportFpgaPlaybackTerminal(PlaybackTerminalState terminal, Current current,
                               Stopped stopped, const Progress& progress,
                               int64_t positionMs, int64_t durationMs) {
    if (terminal == PlaybackTerminalState::None)
        return;
    const char* state = terminal == PlaybackTerminalState::Ended && !stopped()
        ? "ended" : "stopped";
    if (current())
        progress(state, positionMs, durationMs);
}

// Decide what to do with the frame we just decoded.
//   leadMs      small allowed video lead so the vsync path is never starved
//   dropMs      drift past which a late frame is dropped (0 disables dropping)
//   dropRun     how many frames we have dropped back-to-back
//   maxDropRun  cap on consecutive drops so a stall cannot shred the picture
//
// Dropping is only safe because the FFmpeg chain is forced to CFR at this same
// rate: supply then matches the schedule, so drift can only come from a real
// decode/transport stall, never from a rate mismatch. Without forced CFR a
// too-fast assumed rate would make this drop frames forever.
inline AvAction avDecide(int64_t driftMs, int64_t leadMs, int64_t dropMs, int dropRun,
                         int maxDropRun = 1) {
    if (dropMs > 0 && driftMs > dropMs && dropRun < maxDropRun)
        return AvAction::Drop;
    if (driftMs + leadMs < 0)
        return AvAction::Hold;
    return AvAction::Present;
}

enum class PtsDriftGuardAct { None, Hold, Drop };

// Named PTS drift is videoPtsMs - audioPtsMs (opposite sign of avDriftMs).
// Fires when abs(delta) exceeds thresholdMs. alreadyFired is one-shot per
// excursion; caller clears it when abs(delta) <= thresholdMs.
inline PtsDriftGuardAct ptsDriftGuardAct(int64_t videoPtsMs, int64_t audioPtsMs,
                                         int64_t thresholdMs, bool alreadyFired) {
    if (alreadyFired || thresholdMs <= 0)
        return PtsDriftGuardAct::None;
    const int64_t delta = videoPtsMs - audioPtsMs;
    const int64_t absDelta = delta < 0 ? -delta : delta;
    if (absDelta <= thresholdMs)
        return PtsDriftGuardAct::None;
    return delta > 0 ? PtsDriftGuardAct::Hold : PtsDriftGuardAct::Drop;
}

} // namespace misterplex
