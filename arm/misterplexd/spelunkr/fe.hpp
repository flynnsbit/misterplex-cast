#pragma once
// SPELUNKR=1 TV frontend. Isolated from Plex/YouTube/AirPlay.
// Does not add SessionArbiter Owner (idx is `& 3`).

#include "libmisterplex/live_disc_rebase.hpp"
#include "spelunkr/banner.hpp"
#include "spelunkr/schedule.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace misterplex {
namespace spelunkr {

struct TuneResult {
    uint64_t generation = 0;
    NowPlaying now;
};

struct PlayRequest {
    uint64_t generation = 0;
    int idx = -1;
    std::string id;
    int dial = 0;
    bool advanceItem = false;
    int endedItemIndex = -1;  // browse-lane N-2 ended item
    std::string endedArchiveId;  // identity for remapping after a manifest swap
    std::string endedFile;
    std::string endedUrl;
    int64_t endedOffsetMs = -1;  // start offset of the ended play; -1 = unknown
    int64_t endedPlayedMs = -1;  // EOF progress; from-zero plays only, -1 = unknown
    unsigned firstPictureAttempt = 0;  // nonzero: watchdog recovery, not natural EOF
    unsigned failedSkips = 0;  // P2 11-10 nonzero: skip endedItemIndex after a mid-play failure / advance timeout
    int requestedItemIndex = -1;  // warm-header retry: land this exact item in a fresh generation
};

// Identity of a playing / failing item, so a remapped successor can find it
// after replaceSection without assuming the numeric index is still right.
struct ItemIdent {
    std::string archiveId;
    std::string file;
    std::string url;
    static ItemIdent from(const NowPlaying& np) {
        return {np.archiveId, np.file, np.url};
    }
    bool empty() const { return archiveId.empty() && file.empty() && url.empty(); }
    void applyTo(PlayRequest& r) const {
        r.endedArchiveId = archiveId;
        r.endedFile = file;
        r.endedUrl = url;
    }
};

inline int channelIndexById(const Lineup& L, const std::string& id) {
    for (int i = 0; i < static_cast<int>(L.channels.size()); ++i)
        if (L.channels[static_cast<size_t>(i)].id == id)
            return i;
    return -1;
}

// Worker: a stale primetime idx must not keep CH 8 when the request carries another id.
inline int resolvePlayIndex(const Lineup& L, const PlayRequest& r) {
    const int n = static_cast<int>(L.channels.size());
    int idx = r.idx;
    if (idx < 0 || idx >= n || (!r.id.empty() && L.channels[static_cast<size_t>(idx)].id != r.id))
        idx = channelIndexById(L, r.id);
    return idx;
}

// Re-resolve a play index by id and confirm it is in both the lineup and mans.
inline bool resolvePlayIndexInSession(const Lineup& L, const std::vector<Manifest>& mans,
                                      const PlayRequest& r, int& idx) {
    idx = resolvePlayIndex(L, r);
    return idx >= 0 && idx < static_cast<int>(L.channels.size()) &&
           idx < static_cast<int>(mans.size());
}

inline PlayRequest playRequestFor(const Lineup& L, uint64_t gen, int idx) {
    PlayRequest r;
    r.generation = gen;
    r.idx = idx;
    if (idx >= 0 && idx < static_cast<int>(L.channels.size())) {
        r.id = L.channels[static_cast<size_t>(idx)].id;
        r.dial = L.channels[static_cast<size_t>(idx)].dial;
    }
    return r;
}

// Dial order is lineup order (observed: already sorted by dial).
// Pad auto-repeat is 120 ms. Absorb held Up/Down so 34 channels cannot wrap
// back to CH 8 on a long press.
inline constexpr int64_t kZapDebounceMs = 280;
inline bool acceptZap(int64_t nowMs, int64_t& lastZapMs, int64_t gapMs = kZapDebounceMs) {
    if (lastZapMs != 0 && nowMs - lastZapMs < gapMs)
        return false;
    lastZapMs = nowMs;
    return true;
}

inline int channelDelta(const Lineup& L, int idx, int delta) {
    const int n = static_cast<int>(L.channels.size());
    if (n <= 0 || idx < 0 || idx >= n)
        return -1;   // GH#411: do not snap to 0 then Up onto CH 8
    int o = idx + delta;
    o %= n;
    if (o < 0)
        o += n;
    return o;
}

class TvState {
public:
    uint64_t bumpGeneration() { return gen_.fetch_add(1) + 1; }
    uint64_t generation() const { return gen_.load(); }
    bool current(uint64_t g) const { return g != 0 && gen_.load() == g; }

    void setIndex(int i) { idx_.store(i); }
    int index() const { return idx_.load(); }

    // Coalesce zaps: last wins; older generations must not commit.
    TuneResult tune(const Lineup& L, const Manifest& man, int64_t nowMs, int idx) {
        TuneResult t;
        t.generation = bumpGeneration();
        if (idx < 0 || idx >= static_cast<int>(L.channels.size()))
            return t;
        setIndex(idx);
        // GH#411: a dead live schedule must not abort the zap (worker already uses this).
        t.now = nowPlayingOrFirst(L.channels[static_cast<size_t>(idx)], man, nowMs);
        return t;
    }

private:
    std::atomic<uint64_t> gen_{0};
    std::atomic<int> idx_{-1};  // GH#411: unset, not channel 0. Up from 0 is CH 8.
};

struct GuideRow {
    int dial = 0;
    std::string id;
    std::string name;
    std::string nowTitle;
    std::string section;
};

class Session {
public:
    Lineup lineup;
    std::vector<Manifest> mans;  // parallel to lineup.channels; empty items = not loaded
    TvState tv;
    bool guideOpen = false;
    int guideCursor = -1;  // GH#411: unset, matching tv.index(); pad must not wrap onto slot 0/CH 8
    int guideViewStart = 0;  // first visible row index; Left/Right page by kSpelunkrGuidePageRows
    // N16: consecutive "this channel has no next item" skips onto a neighbour channel. Reset by any user zap /
    // guide tune and by a held picture (main loop).
    unsigned channelFailSkips = 0;

    bool setLineup(Lineup L) {
        lineup = std::move(L);
        mans.assign(lineup.channels.size(), Manifest{});
        return !lineup.channels.empty();
    }
    bool setManifest(int idx, Manifest m) {
        if (idx < 0 || idx >= static_cast<int>(mans.size()))
            return false;
        mans[static_cast<size_t>(idx)] = std::move(m);
        return true;
    }
    bool hasManifest(int idx) const {
        return idx >= 0 && idx < static_cast<int>(mans.size()) &&
               !mans[static_cast<size_t>(idx)].items.empty();
    }
    // Replace every channel whose section equals `section`, or append if none.
    // Remaps tv.index() and guideCursor by channel id. Does not bump generation.
    bool replaceSection(const std::string& section, Lineup incoming, std::vector<Manifest> incomingMans) {
        if (incoming.channels.size() != incomingMans.size())
            return false;
        std::string curId, cursorId;
        const int cur = tv.index();
        if (cur >= 0 && cur < static_cast<int>(lineup.channels.size()))
            curId = lineup.channels[static_cast<size_t>(cur)].id;
        if (guideCursor >= 0 && guideCursor < static_cast<int>(lineup.channels.size()))
            cursorId = lineup.channels[static_cast<size_t>(guideCursor)].id;
        Lineup kept;
        kept.engine = lineup.engine;
        kept.generatedAt = lineup.generatedAt;
        kept.build = lineup.build;
        kept.snapshot = lineup.snapshot;
        std::vector<Manifest> keptMans;
        kept.channels.reserve(lineup.channels.size() + incoming.channels.size());
        for (size_t i = 0; i < lineup.channels.size(); ++i) {
            if (lineup.channels[i].section == section)
                continue;
            kept.channels.push_back(std::move(lineup.channels[i]));
            keptMans.push_back(i < mans.size() ? std::move(mans[i]) : Manifest{});
        }
        for (size_t i = 0; i < incoming.channels.size(); ++i) {
            kept.channels.push_back(std::move(incoming.channels[i]));
            keptMans.push_back(std::move(incomingMans[i]));
        }
        lineup = std::move(kept);
        mans = std::move(keptMans);
        auto findId = [&](const std::string& id) -> int {
            if (id.empty())
                return -1;
            for (size_t i = 0; i < lineup.channels.size(); ++i) {
                if (lineup.channels[i].id == id)
                    return static_cast<int>(i);
            }
            return -1;
        };
        const int mapped = findId(curId);
        if (mapped >= 0)
            tv.setIndex(mapped);
        else if (!curId.empty() || tv.index() >= static_cast<int>(lineup.channels.size()))
            tv.setIndex(lineup.channels.empty() ? -1 : 0);
        const int gmapped = findId(cursorId);
        if (gmapped >= 0)
            guideCursor = gmapped;
        else if (!cursorId.empty() || guideCursor >= static_cast<int>(lineup.channels.size()))
            guideCursor = tv.index();
        return true;
    }
    TuneResult tuneIndex(int idx, int64_t nowMs) {
        channelFailSkips = 0;
        TuneResult t;
        if (idx < 0 || idx >= static_cast<int>(lineup.channels.size()) || !hasManifest(idx))
            return t;
        t = tv.tune(lineup, mans[static_cast<size_t>(idx)], nowMs, idx);
        guideCursor = idx;
        return t;
    }
    // Bump generation and move the dial without waiting on a manifest GET.
    TuneResult selectIndex(int idx) {
        TuneResult t;
        if (idx < 0 || idx >= static_cast<int>(lineup.channels.size()))
            return t;
        t.generation = tv.bumpGeneration();
        tv.setIndex(idx);
        guideCursor = idx;
        return t;
    }
    TuneResult zap(int delta, int64_t nowMs) {
        channelFailSkips = 0;
        const int from = guideOpen ? guideCursor : tv.index();
        const int next = channelDelta(lineup, from, delta);
        if (next < 0)
            return {};
        if (guideOpen) {
            guideCursor = next;
            TuneResult t;
            t.generation = tv.generation();
            return t;
        }
        return tuneIndex(next, nowMs);
    }
    TuneResult zapSelect(int delta) {
        channelFailSkips = 0;
        const int from = guideOpen ? guideCursor : tv.index();
        const int next = channelDelta(lineup, from, delta);
        if (next < 0)
            return {};
        if (guideOpen) {
            guideCursor = next;
            TuneResult t;
            t.generation = tv.generation();
            return t;
        }
        return selectIndex(next);
    }
    void syncGuideViewToCursor() {
        const int n = static_cast<int>(lineup.channels.size());
        if (n <= 0) {
            guideViewStart = 0;
            return;
        }
        const int step = std::min(kSpelunkrGuidePageRows, n);
        const int maxStart = std::max(0, n - step);
        int cur = guideCursor;
        if (cur < 0)
            cur = tv.index();
        if (cur < 0)
            cur = 0;
        if (cur < guideViewStart)
            guideViewStart = cur;
        if (cur >= guideViewStart + step)
            guideViewStart = cur - step + 1;
        if (guideViewStart < 0)
            guideViewStart = 0;
        if (guideViewStart > maxStart)
            guideViewStart = maxStart;
    }
    bool pageGuide(int delta) {
        if (!guideOpen || lineup.channels.empty())
            return false;
#ifdef MUT_SPELUNKR_GUIDE_PAGE_NOP
        return false;
#endif
        const int n = static_cast<int>(lineup.channels.size());
        const int step = std::min(kSpelunkrGuidePageRows, n);
        const int maxStart = std::max(0, n - step);
#ifdef MUT_SPELUNKR_GUIDE_PAGE_SIGN
        delta = -delta;
#endif
        int next = guideViewStart + delta * step;
        if (next < 0)
            next = 0;
        if (next > maxStart)
            next = maxStart;
        if (next == guideViewStart)
            return false;
        int offset = 0;
        if (guideCursor >= 0 && guideCursor >= guideViewStart)
            offset = guideCursor - guideViewStart;
        guideViewStart = next;
#ifndef MUT_SPELUNKR_GUIDE_PAGE_CURSOR
        int newCur = guideViewStart + offset;
        if (newCur < guideViewStart)
            newCur = guideViewStart;
        const int pageEnd = std::min(guideViewStart + step, n) - 1;
        if (newCur > pageEnd)
            newCur = pageEnd;
        guideCursor = newCur;
#endif
        return true;
    }
    void toggleGuide() {
        guideOpen = !guideOpen;
        guideCursor = tv.index();
        if (guideOpen)
            syncGuideViewToCursor();
        else
            guideViewStart = 0;
    }
    std::vector<GuideRow> guideRows(int64_t nowMs) const {
        std::vector<GuideRow> rows;
        rows.reserve(lineup.channels.size());
        for (size_t i = 0; i < lineup.channels.size(); ++i) {
            GuideRow r;
            r.dial = lineup.channels[i].dial;
            r.id = lineup.channels[i].id;
            r.name = lineup.channels[i].name;
            r.section = lineup.channels[i].section;
            if (hasManifest(static_cast<int>(i))) {
                const NowPlaying np = nowPlayingOrFirst(lineup.channels[i], mans[i], nowMs);
                if (np.ok)
                    r.nowTitle = np.title;
            }
            rows.push_back(std::move(r));
        }
        return rows;
    }
};

inline PlayRequest advanceAfterEof(Session& session, uint64_t eofGeneration, int endedItemIndex,
                                   int64_t endedOffsetMs = -1, int64_t endedPlayedMs = -1) {
    if (!session.tv.current(eofGeneration))
        return {};
    const int idx = session.tv.index();
    const auto tune = session.selectIndex(idx);
    if (tune.generation == 0)
        return {};
    auto req = playRequestFor(session.lineup, tune.generation, idx);
    req.advanceItem = true;
    req.endedItemIndex = endedItemIndex;
    req.endedOffsetMs = endedOffsetMs;
    req.endedPlayedMs = endedPlayedMs;
    return req;
}

inline constexpr int64_t kFirstPictureTimeoutMs = 3000;
// Hard ceiling on an ad-hold exemption, measured from arm(). The relay's own
// hold cap (MPX_PLUTO_ADSKIP_HOLD_CAP_MS, default 600000) ends a real hold
// sooner; this fires if adHold stays stuck.
inline constexpr int64_t kFirstPictureAdHoldMaxMs = 660000;
// A play-pending chevron with no picture and no play this long is wedged.
inline constexpr int64_t kPlayPendingStaleMs = 45000;

// True when play-pending should keep the idle card off. Playing or a picture
// stays busy (GH#648). A pending tune that has sat longer than
// kPlayPendingStaleMs with neither does not.
inline bool playPendingBlocksIdle(bool pending, bool playing, bool hasPicture, int64_t pendingForMs) {
#ifdef MUT_PENDING_NEVER_CLEARS
    (void)playing;
    (void)hasPicture;
    (void)pendingForMs;
    return pending;
#else
    if (!pending)
        return false;
    if (playing || hasPicture)
        return true;
    return pendingForMs <= kPlayPendingStaleMs;
#endif
}

// Clears play-pending for this job's generation if the job returns without
// play_started. disarm() at play_started. clear() must be gen-matched so a
// newer tune is left pending.
template <class ClearFn>
class SpelunkrPendingGuard {
public:
    SpelunkrPendingGuard(uint64_t gen, ClearFn clear) : gen_(gen), clear_(static_cast<ClearFn&&>(clear)) {}
    ~SpelunkrPendingGuard() {
#ifdef MUT_PENDING_NEVER_CLEARS
        (void)gen_;
        (void)clear_;
        (void)armed_;
#else
        if (armed_)
            clear_(gen_);
#endif
    }
    SpelunkrPendingGuard(const SpelunkrPendingGuard&) = delete;
    SpelunkrPendingGuard& operator=(const SpelunkrPendingGuard&) = delete;
    void disarm() { armed_ = false; }

private:
    uint64_t gen_;
    ClearFn clear_;
    bool armed_ = true;
};

// Armed only after cutover, never during archive resolution / IDR prefetch.
// The caller serializes arm/poll with channel selection.
// adHold (default false): while a Pluto ad-skip hold is active the 3000 ms
// timer restarts, so it counts from the end of the hold. Ignored after
// kFirstPictureAdHoldMaxMs from arm(). MUT_FPW_NO_AD_HOLD_EXEMPT ignores adHold.
class FirstPictureWatchdog {
public:
    void arm(uint64_t generation, int64_t nowMs, unsigned attempt = 0) {
        generation_ = generation;
        cutoverMs_ = nowMs;
        armedMs_ = nowMs;
        attempt_ = attempt;
    }

    PlayRequest poll(Session& session, int64_t nowMs, bool live, bool decodedPicture, int itemIndex,
                     bool adHold = false) {
        if (!session.tv.current(generation_) || !live || decodedPicture) {
            generation_ = 0;
            return {};
        }
#ifndef MUT_FPW_NO_AD_HOLD_EXEMPT
        if (adHold && nowMs - armedMs_ < kFirstPictureAdHoldMaxMs) {
            cutoverMs_ = nowMs;
            return {};
        }
        if (!adHold && nowMs - cutoverMs_ < kFirstPictureTimeoutMs)
            return {};
#else
        (void)adHold;
        if (nowMs - cutoverMs_ < kFirstPictureTimeoutMs)
            return {};
#endif
        auto req = advanceAfterEof(session, generation_, itemIndex);
        generation_ = 0;
        if (req.generation != 0)
            req.firstPictureAttempt = attempt_ + 1;
        return req;
    }

private:
    uint64_t generation_ = 0;
    int64_t cutoverMs_ = 0;
    int64_t armedMs_ = 0;
    unsigned attempt_ = 0;
};

inline NowPlaying nextAfterFirstPictureTimeout(const Channel& ch, const Manifest& man,
                                               int failedItemIndex, unsigned attempt, int64_t nowMs,
                                               const std::string& endedId = {},
                                               const std::string& endedFile = {},
                                               const std::string& endedUrl = {}) {
    // Visit at most one manifest's worth of failed pictures, then show an error.
    const int idx = effectiveEndedItemIndex(man, failedItemIndex, endedId, endedFile, endedUrl);
    if (idx < 0 || idx >= static_cast<int>(man.items.size()) ||
        attempt >= man.items.size())
        return {};
    NowPlaying failed;
    failed.itemIndex = idx;
    auto next = skipToNextItem(ch, man, failed, nowMs);
    if (next.itemIndex == idx)
        return {};
    return next;
}

// Glass a1c0af44: Pluto live is ONE item. nextAfterFirstPictureTimeout with
// attempt=1 returns {} (attempt >= size) and the play job lands
// FIRST PICTURE FAILED: NO NEXT without reopening remux. Zap (new tune,
// attempt=0) recovered living Vevo. Retry the same item once so the play job
// starts a new remux instead of landing.
inline NowPlaying retryLiveFirstPicture(const Channel& ch, const Manifest& man,
                                        int failedItemIndex, unsigned attempt, int64_t nowMs,
                                        const std::string& endedId = {},
                                        const std::string& endedFile = {},
                                        const std::string& endedUrl = {}) {
    const int idx = effectiveEndedItemIndex(man, failedItemIndex, endedId, endedFile, endedUrl);
    if (!misterplex::live_disc::livePicWatchdogRetries(attempt, static_cast<int>(man.items.size())))
        return {};
    if (idx < 0)
        return {};
    return playItemAtIndex(ch, man, idx, nowMs);
}

// P2 11-10: a mid-play failure (NeedsTranscode / SourceError) or an EOF/skip advance that
// overruns its 20 s deadline skips the failed item with a banner instead of landing the guide.
// failedSkips counts consecutive failed items on this channel (reset by any zap or natural EOF);
// at most one manifest's worth is visited, then the caller lands with the error.
inline NowPlaying nextAfterFailedItem(const Channel& ch, const Manifest& man, int failedItemIndex,
                                      unsigned failedSkips, int64_t nowMs,
                                      const std::string& endedId = {},
                                      const std::string& endedFile = {},
                                      const std::string& endedUrl = {}) {
    if (failedSkips == 0)
        return {};
    return nextAfterFirstPictureTimeout(ch, man, failedItemIndex, failedSkips, nowMs,
                                        endedId, endedFile, endedUrl);
}

// Same channel, new generation, carrying the failed item. Returns {} (no generation bump) for a
// stale generation or an unknown failed item, so the caller lands with the error instead.
// N16 (glass 067d79f7, dial 1062): a Pluto live channel is ONE item. When its relay gave up (crash storm), the
// failed-item skip had no other item to go to and the job died with "SKIP FAILED: NO NEXT", a dead end. A channel
// with nothing left to skip to now moves to the NEXT channel (a fresh tune, a fresh budget), at most
// kChannelSkipMax in a row (an all-bad lineup must not cycle forever). Returns {} when there is no other channel
// or the budget is spent: the caller lands on the clean error.
inline constexpr unsigned kChannelSkipMax = 3;
inline bool channelHasNoNextItem(const Manifest& man, unsigned failedSkips) {
    return man.items.size() <= 1 || failedSkips >= man.items.size();
}
inline PlayRequest advanceAfterFailedItem(Session& session, uint64_t generation, int failedItemIndex,
                                          unsigned failedSkips) {
    if (failedItemIndex < 0 || failedSkips == 0 || !session.tv.current(generation))
        return {};
#ifndef MUT_N16_NO_NEXT_DEADEND
    const int cur = session.tv.index();
    if (cur >= 0 && cur < static_cast<int>(session.mans.size()) &&
        channelHasNoNextItem(session.mans[static_cast<size_t>(cur)], failedSkips)) {
#ifndef MUT_N16_CHANNEL_SKIP_UNBOUNDED
        if (session.channelFailSkips >= kChannelSkipMax)
            return {};
#endif
        const int next = channelDelta(session.lineup, cur, 1);
        if (next < 0 || next == cur)
            return {};
        const auto tune = session.selectIndex(next);
        if (tune.generation == 0)
            return {};
        ++session.channelFailSkips;
        return playRequestFor(session.lineup, tune.generation, next);   // a fresh tune: no skip carried
    }
#endif
    auto req = advanceAfterEof(session, generation, failedItemIndex);
    if (req.generation != 0)
        req.failedSkips = failedSkips;
    return req;
}

// browse-lane #620 second review P2: the first-picture watchdog chain and the failed-item chain share ONE
// consecutive-failure count, so alternating "no first picture" / "mid-play failure or advance
// timeout" still exhausts one manifest's worth and lands. Reset only by a real first picture
// (main loop), a zap, or a natural EOF (both carry 0).
inline unsigned sharedSkipBudget(const PlayRequest& r) {
    return r.firstPictureAttempt > r.failedSkips ? r.firstPictureAttempt : r.failedSkips;
}

// P3 (browse-lane second review): only an undecodable / failed source skips the item. CoreLost (OSD Reset,
// another program took the core) and RingError must not turn into an item skip.
inline bool midPlayEndSkipsItem(bool needsTranscode, bool sourceError) {
    return needsTranscode || sourceError;
}

// browse-lane #621 P2-a: the shared-budget reset condition as one pure predicate (each term unit-tested).
inline bool skipBudgetResetGate(bool open, bool stopping, bool playing, bool decodedPicture, bool currentGen) {
    return open && !stopping && playing && decodedPicture && currentGen;
}

// browse-lane #621 P3-a: a NeedsTranscode (CAVLC rate/load) verdict can come a few seconds AFTER the first
// picture, so resetting on the first picture alone let an all-high-rate channel cycle forever. The
// budget resets only after the current play has held a real picture for kSkipBudgetResetPlayMs.
inline constexpr int64_t kSkipBudgetResetPlayMs = 20000;

class SkipBudgetReset {
public:
    // True exactly once per play generation, when the gate has held continuously for
    // kSkipBudgetResetPlayMs. Any gap (gate false, other generation) restarts the clock.
    bool poll(uint64_t playGen, bool open, bool stopping, bool playing, bool decodedPicture, bool currentGen,
              int64_t nowMs) {
        if (playGen == 0 || !skipBudgetResetGate(open, stopping, playing, decodedPicture, currentGen)) {
            gen_ = 0;
            fired_ = false;
            return false;
        }
        if (gen_ != playGen) {
            gen_ = playGen;
            sinceMs_ = nowMs;
            fired_ = false;
        }
        if (fired_ || nowMs - sinceMs_ < kSkipBudgetResetPlayMs)
            return false;
        fired_ = true;
        return true;
    }
    int64_t heldMs(int64_t nowMs) const { return gen_ == 0 ? 0 : nowMs - sinceMs_; }

private:
    uint64_t gen_ = 0;
    int64_t sinceMs_ = 0;
    bool fired_ = false;
};

struct FailedItemSkip {
    uint64_t generation = 0;
    int itemIndex = -1;
    unsigned failedSkips = 0;  // already incremented for this failure
    std::string why;
    ItemIdent ident;  // the failing item, not necessarily the last one that actually played
};

// Play-thread / play-job -> main-loop handoff (never takes tvMu on the play thread, browse-lane P1-2).
class FailedItemHandoff {
public:
    void post(uint64_t generation, int itemIndex, unsigned priorSkips, std::string why,
              ItemIdent ident = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        pending_ = {generation, itemIndex, priorSkips + 1, std::move(why), std::move(ident)};
    }
    FailedItemSkip take() {
        std::lock_guard<std::mutex> lock(mu_);
        FailedItemSkip out = std::move(pending_);
        pending_ = {};
        return out;
    }

private:
    std::mutex mu_;
    FailedItemSkip pending_;
};

struct CachedItemLanding {
    uint64_t generation = 0;
    int itemIndex = -1;
};

class CachedItemHandoff {
public:
    void post(uint64_t generation, int itemIndex) {
        std::lock_guard<std::mutex> lock(mu_);
        pending_ = {generation, itemIndex};
    }
    CachedItemLanding take() {
        std::lock_guard<std::mutex> lock(mu_);
        const CachedItemLanding out = pending_;
        pending_ = {};
        return out;
    }

private:
    std::mutex mu_;
    CachedItemLanding pending_;
};

inline PlayRequest landCachedItem(Session& session, uint64_t generation, int itemIndex) {
    if (itemIndex < 0 || !session.tv.current(generation))
        return {};
    const int idx = session.tv.index();
    const auto tune = session.selectIndex(idx);
    if (tune.generation == 0)
        return {};
    auto req = playRequestFor(session.lineup, tune.generation, idx);
    req.requestedItemIndex = itemIndex;
    return req;
}

inline PlayRequest resumeLastChannel(Session& session, const std::string& id) {
    session.guideOpen = false;
    const int idx = channelIndexById(session.lineup, id);
    if (id.empty() || idx < 0)
        return {};
    const auto tune = session.selectIndex(idx);
    return playRequestFor(session.lineup, tune.generation, idx);
}

// Cold first core-open ALWAYS lands on the guide, even when SPELUNKR_LAST_CHANNEL is set.
// In-session core reopens (preferCurrent=true) keep resuming the current/last channel.
enum class DeferredResume : uint8_t { Tune, Land, Defer };

inline DeferredResume firstOpenLastChannelAction(const Lineup& lineup, const std::string& lastId,
                                                 bool backWhenPending) {
#ifdef MUT_COLD_BOOT_RESUMES_LAST_CHANNEL
    if (lastId.empty())
        return DeferredResume::Land;
    if (channelIndexById(lineup, lastId) >= 0)
        return DeferredResume::Tune;
    return backWhenPending ? DeferredResume::Defer : DeferredResume::Land;
#else
    (void)lineup;
    (void)lastId;
    (void)backWhenPending;
    return DeferredResume::Land;
#endif
}

// browse-lane #654 P1: a real function, never a block-local lambda captured by
// setSpelunkrCmd. That [&] callback outlives the lineup-success else, so a
// local lambda would be a use-after-free on every later pad command.
template <typename ClearChrome>
inline void abandonDeferredResume(bool& awaiting, ClearChrome&& clearChrome) {
    if (!awaiting)
        return;
    awaiting = false;
    clearChrome();
}

// GH#411: in-session core reopen (preferCurrentDial=true) keeps the dial in progress,
// falling back to lastId if unset. Cold boot / first core open (preferCurrentDial=false)
// ALWAYS lands on the guide (empty PlayRequest), even when SPELUNKR_LAST_CHANNEL is set.
inline PlayRequest resumeCurrentOrLast(Session& session, const std::string& lastId,
                                       bool preferCurrentDial = true) {
    session.guideOpen = false;
    int idx = -1;
    if (preferCurrentDial) {
        idx = session.tv.index();
        if (idx < 0 || idx >= static_cast<int>(session.lineup.channels.size()))
            idx = channelIndexById(session.lineup, lastId);
    }
#ifdef MUT_COLD_BOOT_RESUMES_LAST_CHANNEL
    else {
        idx = channelIndexById(session.lineup, lastId);
    }
#endif
    if (idx < 0)
        return {};
    const auto tune = session.selectIndex(idx);
    return playRequestFor(session.lineup, tune.generation, idx);
}

// browse-lane #623 frame 153: the channel the next core-open edge will resume, WITHOUT selecting it (no
// generation bump), or -1 when the open edge will land. Mirrors resumeCurrentOrLast so main arms tuning
// chrome only on in-session reopens (preferCurrentDial=true); cold boot lands on the guide (-1).
inline int resumeIndexPreview(const Session& session, const std::string& lastId, bool preferCurrentDial) {
    const int n = static_cast<int>(session.lineup.channels.size());
    int idx = -1;
    if (preferCurrentDial) {
        idx = session.tv.index();
        if (idx < 0 || idx >= n)
            idx = channelIndexById(session.lineup, lastId);
    }
#ifdef MUT_COLD_BOOT_RESUMES_LAST_CHANNEL
    else if (!lastId.empty()) {
        idx = channelIndexById(session.lineup, lastId);
    }
#endif
    return idx >= 0 && idx < n ? idx : -1;
}

struct ResumeChrome {
    bool arm = false;
    std::string dial;
    std::string name;
};

inline ResumeChrome resumeChromeToArm(const Session& s, const std::string& lastId, bool coreOpen,
                                      bool hadCoreOpenOnce, bool armed, bool uiPending) {
    if (coreOpen || (armed && uiPending))
        return {};
    const int idx = resumeIndexPreview(s, lastId, hadCoreOpenOnce);
    if (idx < 0)
        return {};
    const auto& channel = s.lineup.channels[static_cast<size_t>(idx)];
    ResumeChrome chrome;
    chrome.arm = true;
    chrome.dial = "CH " + std::to_string(channel.dial);
    chrome.name = channel.name;
    return chrome;
}

inline bool rememberPresentedChannel(const Session& session, uint64_t startedGeneration,
                                     std::string& savedId,
                                     const std::function<bool(const std::string&)>& persist) {
    if (!session.tv.current(startedGeneration))
        return true;
    const int idx = session.tv.index();
    if (idx < 0 || idx >= static_cast<int>(session.lineup.channels.size()))
        return true;
    const std::string& id = session.lineup.channels[static_cast<size_t>(idx)].id;
    if (id == savedId)
        return true;
    if (!persist(id))
        return false;
    savedId = id;
    return true;
}

// Last-wins play mailbox. post() does not wait for network. A newer post
// makes cancelled() true for the in-flight job.
class PlayQueue {
public:
    using Job = std::function<void(PlayRequest req, const std::function<bool()>& cancelled)>;

    PlayQueue() = default;
    PlayQueue(const PlayQueue&) = delete;
    PlayQueue& operator=(const PlayQueue&) = delete;
    ~PlayQueue() { shutdown(); }

    void start(Job job) {
        std::lock_guard<std::mutex> g(mu_);
        job_ = std::move(job);
        stop_ = false;
        if (!thr_.joinable())
            thr_ = std::thread([this] { loop(); });
    }

    void post(PlayRequest r) {
        std::lock_guard<std::mutex> g(mu_);
        ++revision_;
        pending_ = std::move(r);
        have_ = true;
        cv_.notify_one();
    }

    void cancel() {
      std::lock_guard<std::mutex> g(mu_);
      ++revision_;
      have_ = false;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
            have_ = true;
            cv_.notify_all();
        }
        if (thr_.joinable())
            thr_.join();
    }

private:
    void loop() {
        for (;;) {
            PlayRequest req;
            uint64_t revision = 0;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return have_ || stop_; });
                if (stop_)
                    return;
                req = pending_;
                revision = revision_;
                have_ = false;
            }
            auto cancelled = [this, revision]() -> bool {
              std::lock_guard<std::mutex> g(mu_);
              return stop_ || revision_ != revision;
            };
            if (cancelled())
                continue;
            if (job_)
                job_(req, cancelled);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    Job job_;
    PlayRequest pending_;
    bool have_ = false;
    bool stop_ = false;
    uint64_t revision_ = 0;
    std::thread thr_;
};

// Rising edge of Main core OSD (EV_OSD open): close the Spelunkr guide so
// it is not stacked under the F12 OSD, and so pad Right can change Analog Raster.
inline bool closeGuideOnCoreOsdOpen(bool wasOsdOpen, bool nowOsdOpen, bool guideOpen) {
    return !wasOsdOpen && nowOsdOpen && guideOpen;
}

// Rising-edge tracker for the core OSD, held across input-loop iterations.
// reset() on decoder/handle recreate so a stale wasOpen cannot swallow the next edge.
struct OsdEdge {
    bool wasOpen = false;
    // Returns true on a rising edge (closed -> open). Updates wasOpen.
    bool step(bool nowOpen) {
        const bool rising = !wasOpen && nowOpen;
        wasOpen = nowOpen;
        return rising;
    }
    void reset() { wasOpen = false; }
};

// Idempotent guide close (browse-lane #646 P3-1 / #647 P2-5). True if the guide was open
// and is now closed (caller should publish). False if it was already closed.
inline bool applyGuideClose(bool& guideOpen) {
    if (!guideOpen)
        return false;
    guideOpen = false;
    return true;
}

// Stage-log helpers so duration/attempt/first-picture behaviour is unit-testable
// (browse-lane #647 surviving mutants were text pins on main.cpp).
struct StageLog {
    uint64_t gen = 0;
    int attempt = 0;
    bool tJobSet = false;
    int64_t tPrevMs = 0;
    std::chrono::steady_clock::time_point tJob{};

    int64_t elapsedMs() const {
        if (!tJobSet)
            return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - tJob)
            .count();
    }

    // dur_ms is the delta since the previous line, not the cumulative t_ms.
    std::string format(const char* name, bool terminal = false, const char* reason = nullptr) {
        const auto ms = elapsedMs();
        const auto dur = ms - tPrevMs;
        tPrevMs = ms;
        std::string s = "stage gen=" + std::to_string(gen) + " attempt=" + std::to_string(attempt) +
                        " name=" + (name ? name : "") + " dur_ms=" + std::to_string(dur) +
                        " t_ms=" + std::to_string(ms);
        if (reason && *reason) {
            s += " reason=";
            s += reason;
        }
        if (terminal)
            s += " terminal=1";
        return s;
    }
};

// One first_picture line per gen. False if picGen is 0 or this gen already logged.
inline bool claimFirstPictureLog(uint64_t picGen, std::atomic<uint64_t>& loggedGen) {
    uint64_t seen = loggedGen.load(std::memory_order_acquire);
    if (picGen == 0 || seen == picGen)
        return false;
    return loggedGen.compare_exchange_strong(seen, picGen, std::memory_order_acq_rel);
}

}  // namespace spelunkr
}  // namespace misterplex
