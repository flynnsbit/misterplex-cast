#pragma once
// PrefetchSpelunkr's result -> the play loop's skip-before-fail decision, pulled out as
// pure functions so the ordering is unit-testable without the play loop's threads/locks/player.
// A dead-air timeout or a non-H.264 input tries the next item first; other prefetch
// failures (network, format) still fail the channel.

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>

namespace misterplex {
namespace spelunkr {

enum class AdvanceTimeoutAction {
    CompleteCutover,
    SkipItem,
    LandCachedHeader,
    Fail,
};

struct AdvanceTimeoutInput {
    int tryingItem = -1;
    int currentItem = -1;
    bool prefetchReady = false;
    bool headerCached = false;
    bool canSkip = false;
};

inline int bindTryingItem(int tryingItem, int currentItem) {
    return tryingItem >= 0 ? tryingItem : currentItem;
}

inline AdvanceTimeoutAction decideAdvanceTimeout(const AdvanceTimeoutInput& in) {
    if (in.prefetchReady)
        return AdvanceTimeoutAction::CompleteCutover;
    if (in.canSkip && bindTryingItem(in.tryingItem, in.currentItem) >= 0)
        return AdvanceTimeoutAction::SkipItem;
    if (in.headerCached)
        return AdvanceTimeoutAction::LandCachedHeader;
    return AdvanceTimeoutAction::Fail;
}

// True: attempt takeSkip(skipDead(np), "prefetch timeout") before any fail(), mirroring the
// existing "unsupported resolution" skip path. A dead-air prefetch timeout (the 15 s cap) must
// not hard-fail the channel when a next item exists.
inline bool shouldSkipOnPrefetchTimeout(bool prefetchTimedOut) {
    return prefetchTimedOut;
}

// True when prefetch failed because the core only plays H.264. Skip to the next item
// instead of ending the channel on PREFETCH FAILED.
inline bool shouldSkipOnNonH264(const std::string& prefetchFail) {
    return prefetchFail.find("H.264 only") != std::string::npos;
}

// The skip-attempt cap: a channel whose items are all dead/timing out must terminate instead
// of looping forever. skipsAfterIncrement is the caller's skip counter already incremented for
// this attempt (main.cpp's ++skips); itemCount is the channel's item count. A 0- or 1-item
// channel allows no skip at all (every attempt is already exhausted): std::max(1, itemCount)
// keeps the cap at least 1 so the comparison below is always well-formed.
inline bool skipBudgetExhausted(int skipsAfterIncrement, int itemCount) {
    const int cap = itemCount > 1 ? itemCount : 1;
    return skipsAfterIncrement >= cap;
}

// browse-lane #647 P2-7 / P3-5: overall zap budget so a 31.6 MB moov cannot sit in
// PREFETCH TIMEOUT for 15 s. Prefetch is slightly shorter than the job deadline
// so a timeout can hit storeTuneTimeout before the job cancel path fires.
inline constexpr int64_t kSpelunkrZapBudgetMs = 8000;
inline constexpr int64_t kSpelunkrPrefetchTimeoutMs = 7000;
// Glass pr685-13eca668 / reviewer ADDENDUM 7: Pluto gets a split budget. Metadata
// (resolve/probe) is cancelled on kSpelunkrPlutoZapBudgetMs. The remux open is a
// separate cap, re-armed at prefetch start so slow metadata cannot eat it.
// Default remux-open cap is 15 s: a healthy ARM first byte is 4–10 s, and libav
// open + avStartGate (up to 1500 ms) + two video packets still have to fit after
// that byte. 15 s leaves ~5 s after a 10 s-class first byte. The 8 s cap was
// tighter than the 10 s first-stall and killed an 8–10 s first byte (or a first
// byte plus a short gate) before the stall could. kLiveFirstStallRestartMs stays
// 10000: a 10.1 s ARM tail is still killed by that stall (Finding 5).
// MPX_PLUTO_REMUX_OPEN_MS=8000 is exactly today's 8 s cap (and today's re-arm
// against the 15 s zap window). Unset, empty, or non-numeric uses 15000. Any
// other positive value is an override.
inline constexpr int64_t kSpelunkrPlutoRemuxOpenBudgetDefaultMs = 15000;
inline constexpr int64_t kSpelunkrPlutoRemuxOpenBudgetLegacyMs = 8000;
inline constexpr int64_t kSpelunkrPlutoMetadataBudgetMs = 7000;
// Metadata/job cancel stays today's 15 s (metadata 7 s + the legacy 8 s remux
// cap). Do not add the 15 s default onto this: resolve would then run 22 s
// before cancelled() fires. The open's own window is the re-arm below.
inline constexpr int64_t kSpelunkrPlutoZapBudgetMs =
    kSpelunkrPlutoMetadataBudgetMs + kSpelunkrPlutoRemuxOpenBudgetLegacyMs;
inline constexpr int64_t kSpelunkrPlutoPrefetchTimeoutMs = kSpelunkrPlutoZapBudgetMs;

// Runtime remux-open cap. MUT_PLUTO_REMUX_OPEN_8S ignores the 15 s default.
inline int64_t plutoRemuxOpenBudgetMs() {
#ifdef MUT_PLUTO_REMUX_OPEN_8S
    return kSpelunkrPlutoRemuxOpenBudgetLegacyMs;
#else
    const char* e = std::getenv("MPX_PLUTO_REMUX_OPEN_MS");
    if (!e || !e[0])
        return kSpelunkrPlutoRemuxOpenBudgetDefaultMs;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(e, &end, 10);
    if (end == e || *end != '\0' || errno == ERANGE || v <= 0)
        return kSpelunkrPlutoRemuxOpenBudgetDefaultMs;
    return static_cast<int64_t>(v);
#endif
}

inline int64_t spelunkrPrefetchTimeoutMsForPlay(bool plutoLiveHlsItem) {
#ifdef MUT_PLUTO_SHARED_BUDGET
    (void)plutoLiveHlsItem;
    return kSpelunkrPrefetchTimeoutMs;
#else
    return plutoLiveHlsItem ? kSpelunkrPlutoPrefetchTimeoutMs : kSpelunkrPrefetchTimeoutMs;
#endif
}

inline int64_t spelunkrZapBudgetMsForPlay(bool plutoLiveHlsItem) {
#ifdef MUT_PLUTO_SHARED_BUDGET
    (void)plutoLiveHlsItem;
    return kSpelunkrZapBudgetMs;
#else
    return plutoLiveHlsItem ? kSpelunkrPlutoZapBudgetMs : kSpelunkrZapBudgetMs;
#endif
}

// v1.0.1 glass (lab 2026-11-23, eb6e7dbc): the "split" Pluto budget was not split. cancelled() measures ONE
// 15 s window from the job start, so a slow metadata resolve (boot/master/variant probes/audio rendition) ate the
// remux open: 1063 zap "stage gen=3 attempt=1 name=timeout dur_ms=3116 t_ms=15062 reason=advance timeout"
// (metadata ~11.9 s, the open got 3.1 s), and the 1066 resume after the Plex->Live TV switch (cold daemon: no
// Pluto boot session, cold DNS/TLS, the first video_prepare of the process) hit "advance timeout" on all 3 opens ->
// PREFETCH TIMEOUT. When a Pluto prefetch starts, the window is re-armed so it ends no sooner than
// plutoRemuxOpenBudgetMs() (default 15 s, or 8 s when MPX_PLUTO_REMUX_OPEN_MS=8000) after that start. It never
// shortens the window, needs no zap-window restart (kSpelunkrMaxZapWindowRestarts stays for warm partials / open
// retries), and the metadata stage itself is still cancelled at 15 s. One attempt is bounded by that 15 s
// metadata cancel plus one full remux-open cap — not by adding the new cap onto the zap budget.
#define MPX_PLUTO_REMUX_OPEN_REARM 1
inline int64_t plutoRemuxOpenWindowStartMs(bool plutoLiveHlsItem, int64_t windowStartMs, int64_t prefetchStartMs) {
#ifdef MUT_PLUTO_OPEN_BUDGET_FROM_JOB_START
    (void)plutoLiveHlsItem;
    (void)prefetchStartMs;
    return windowStartMs;
#else
    if (!plutoLiveHlsItem)
        return windowStartMs;
    const int64_t remuxMs = plutoRemuxOpenBudgetMs();
    const int64_t floorStart = prefetchStartMs + remuxMs - kSpelunkrPlutoZapBudgetMs;
    return std::max(windowStartMs, floorStart);
#endif
}

inline int64_t remainingZapBudgetMs(int64_t elapsedMs, int64_t budgetMs = kSpelunkrZapBudgetMs) {
    if (elapsedMs >= budgetMs)
        return 0;
    return budgetMs - elapsedMs;
}

// Remaining prefetch slice for this zap window. Pluto live HLS uses a dedicated remux-open cap
// (not the leftover of a single shared prefetch slice).
inline int64_t spelunkrPrefetchBudgetMs(bool plutoLiveHlsItem, int64_t zapWindowElapsedMs) {
    if (!plutoLiveHlsItem)
        return remainingZapBudgetMs(zapWindowElapsedMs, kSpelunkrPrefetchTimeoutMs);
#ifdef MUT_PLUTO_SHARED_BUDGET
    return remainingZapBudgetMs(zapWindowElapsedMs, kSpelunkrPrefetchTimeoutMs);
#else
    if (zapWindowElapsedMs >= kSpelunkrPlutoZapBudgetMs)
        return 0;
    return plutoRemuxOpenBudgetMs();
#endif
}

// Chan-change regression (lab 1df5f4ae, 19:15-19:19 CT): the 8 s budget was one deadline for the
// whole job. A first item whose header outran the 7 s prefetch slice (11-22 MB moov at ~3.7 MB/s)
// left ~1 s for the next item, whose metadata/prefetch was then cancelled by the same job
// deadline ("advance timeout ... terminal=1"), so every such zap burned two or more 8 s windows
// (channel 53: gens 10-13, ~34 s) before a picture. An item skipped after a prefetch timeout now
// gets its own 8 s window (bounded), so a zap needs ~7 s + one normal item, not 8 s + 8 s + ...
// The per-attempt constants above are unchanged.
inline constexpr int kSpelunkrMaxZapWindowRestarts = 2;

inline bool mayRestartZapWindow(int restartsSoFar) {
    return restartsSoFar >= 0 && restartsSoFar < kSpelunkrMaxZapWindowRestarts;
}

// Re-arms the zap window for a bounded Pluto live-open retry (already capped by
// kPlutoOpenRetryMax). Resets windowRestarts to 0 so prior in-place restarts
// cannot leave windowStartMs stale and kill retry 1/2 immediately (r2-03f 55 s bug).
#define MPX_PLUTO_55S_FIX 1
inline void rearmZapWindowForPlutoRetry(int64_t nowMs, int64_t& windowStartMs, int& windowRestarts) {
#ifdef MUT_PLUTO_RETRY_REUSES_RESTART_CAP
    if (!mayRestartZapWindow(windowRestarts))
        return;
    ++windowRestarts;
    windowStartMs = nowMs;
#else
    windowRestarts = 0;
    windowStartMs = nowMs;
#endif
}

inline int64_t zapWindowElapsedMs(int64_t nowMs, int64_t windowStartMs) {
    return nowMs > windowStartMs ? nowMs - windowStartMs : 0;
}

inline bool zapWindowExpired(int64_t nowMs, int64_t windowStartMs, int64_t budgetMs = kSpelunkrZapBudgetMs) {
    return zapWindowElapsedMs(nowMs, windowStartMs) >= budgetMs;
}

// Glass 1c230276 Z2: UP onto pluto-vevo-90s right after the previous live relay
// was SIGKILL'd (ffmpeg rc=-9). playjob/manifest/schedule returned immediately,
// then metadata resolve (boot/master/probe) was still in flight at 8079 ms and
// 8187 ms with no prefetch stage. plutoLiveHlsBudget used to flip true only
// after liveRelay().start(), so cancelled() was still on the generic 8 s budget,
// decideAdvanceTimeout saw item 0 / no skip / no cached header / prefetch not
// started, and the first window terminal-failed PREFETCH TIMEOUT. The same dial
// later landed inside the 15 s Pluto budget (zap05: prefetch 3750 ms,
// first_picture 8906 ms). Neighbor-header prefetch cannot land this dial
// (NeighborSkipReason::PlutoLiveHls). MEMSTAT probe_q=64 is the open demuxer's
// max_probe_packets cap, not a saturated queue — it was 64 on the failures and
// on that later success. Arm the split budget from the schedule identity, before
// resolve, so that first window is the 15 s budget. "pluto" matches
// pluto_live::kPlutoItemId.
inline bool scheduleItemUsesPlutoZapBudget(bool plutoLiveOn, const std::string& archiveId,
                                           const std::string& file) {
#ifdef MUT_Z2_PLUTO_BUDGET_LATE
    (void)plutoLiveOn;
    (void)archiveId;
    (void)file;
    return false;
#else
    return plutoLiveOn && archiveId == "pluto" && !file.empty();
#endif
}

// True when a zap-window poll before prefetch has started would OSD
// PREFETCH TIMEOUT for this schedule item. A Pluto live item is not terminal
// at the glass 8079/8187 ms marks. It is terminal once the 15 s split budget
// is exhausted. A non-Pluto item is still terminal at 8 s. canSkip / a warm
// header take the existing skip or land path instead of this terminal.
inline bool prefetchNotStartedTerminal(bool plutoLiveOn, const std::string& archiveId, const std::string& file,
                                       int64_t elapsedMs, bool canSkip, bool headerCached) {
    const int64_t budget = spelunkrZapBudgetMsForPlay(
        scheduleItemUsesPlutoZapBudget(plutoLiveOn, archiveId, file));
    if (!zapWindowExpired(elapsedMs, 0, budget))
        return false;
    if (canSkip || headerCached)
        return false;
    const AdvanceTimeoutInput in{0, 0, /*prefetchReady=*/false, /*headerCached=*/false, /*canSkip=*/false};
    return decideAdvanceTimeout(in) == AdvanceTimeoutAction::Fail;
}

}  // namespace spelunkr
}  // namespace misterplex
