#pragma once
// SPELUNKR=1 TV frontend. Isolated from Plex/YouTube/AirPlay.
// Does not add SessionArbiter Owner (idx is `& 3`).

#include "spelunkr/schedule.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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
    int endedItemIndex = -1;  // GBE N-2 ended item
    int64_t endedOffsetMs = -1;  // start offset of the ended play; -1 = unknown
    int64_t endedPlayedMs = -1;  // EOF progress; from-zero plays only, -1 = unknown
    unsigned firstPictureAttempt = 0;  // nonzero: watchdog recovery, not natural EOF
    unsigned failedSkips = 0;  // P2 11-10 nonzero: skip endedItemIndex after a mid-play failure / advance timeout
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
};

class Session {
public:
    Lineup lineup;
    std::vector<Manifest> mans;  // parallel to lineup.channels; empty items = not loaded
    TvState tv;
    bool guideOpen = false;
    int guideCursor = -1;  // GH#411: unset, matching tv.index(); pad must not wrap onto slot 0/CH 8

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
    TuneResult tuneIndex(int idx, int64_t nowMs) {
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
    void toggleGuide() {
        guideOpen = !guideOpen;
        guideCursor = tv.index();
    }
    std::vector<GuideRow> guideRows(int64_t nowMs) const {
        std::vector<GuideRow> rows;
        rows.reserve(lineup.channels.size());
        for (size_t i = 0; i < lineup.channels.size(); ++i) {
            GuideRow r;
            r.dial = lineup.channels[i].dial;
            r.id = lineup.channels[i].id;
            r.name = lineup.channels[i].name;
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

// Armed only after cutover, never during archive resolution / IDR prefetch.
// The caller serializes arm/poll with channel selection.
class FirstPictureWatchdog {
public:
    void arm(uint64_t generation, int64_t nowMs, unsigned attempt = 0) {
        generation_ = generation;
        cutoverMs_ = nowMs;
        attempt_ = attempt;
    }

    PlayRequest poll(Session& session, int64_t nowMs, bool live, bool decodedPicture, int itemIndex) {
        if (!session.tv.current(generation_) || !live || decodedPicture) {
            generation_ = 0;
            return {};
        }
        if (nowMs - cutoverMs_ < kFirstPictureTimeoutMs)
            return {};
        auto req = advanceAfterEof(session, generation_, itemIndex);
        generation_ = 0;
        if (req.generation != 0)
            req.firstPictureAttempt = attempt_ + 1;
        return req;
    }

private:
    uint64_t generation_ = 0;
    int64_t cutoverMs_ = 0;
    unsigned attempt_ = 0;
};

inline NowPlaying nextAfterFirstPictureTimeout(const Channel& ch, const Manifest& man,
                                               int failedItemIndex, unsigned attempt, int64_t nowMs) {
    // Visit at most one manifest's worth of failed pictures, then show an error.
    if (failedItemIndex < 0 || failedItemIndex >= static_cast<int>(man.items.size()) ||
        attempt >= man.items.size())
        return {};
    NowPlaying failed;
    failed.itemIndex = failedItemIndex;
    auto next = skipToNextItem(ch, man, failed, nowMs);
    if (next.itemIndex == failedItemIndex)
        return {};
    return next;
}

// P2 11-10: a mid-play failure (NeedsTranscode / SourceError) or an EOF/skip advance that
// overruns its 20 s deadline skips the failed item with a banner instead of landing the guide.
// failedSkips counts consecutive failed items on this channel (reset by any zap or natural EOF);
// at most one manifest's worth is visited, then the caller lands with the error.
inline NowPlaying nextAfterFailedItem(const Channel& ch, const Manifest& man, int failedItemIndex,
                                      unsigned failedSkips, int64_t nowMs) {
    if (failedSkips == 0)
        return {};
    return nextAfterFirstPictureTimeout(ch, man, failedItemIndex, failedSkips, nowMs);
}

// Same channel, new generation, carrying the failed item. Returns {} (no generation bump) for a
// stale generation or an unknown failed item, so the caller lands with the error instead.
inline PlayRequest advanceAfterFailedItem(Session& session, uint64_t generation, int failedItemIndex,
                                          unsigned failedSkips) {
    if (failedItemIndex < 0 || failedSkips == 0 || !session.tv.current(generation))
        return {};
    auto req = advanceAfterEof(session, generation, failedItemIndex);
    if (req.generation != 0)
        req.failedSkips = failedSkips;
    return req;
}

// GBE #620 second review P2: the first-picture watchdog chain and the failed-item chain share ONE
// consecutive-failure count, so alternating "no first picture" / "mid-play failure or advance
// timeout" still exhausts one manifest's worth and lands. Reset only by a real first picture
// (main loop), a zap, or a natural EOF (both carry 0).
inline unsigned sharedSkipBudget(const PlayRequest& r) {
    return r.firstPictureAttempt > r.failedSkips ? r.firstPictureAttempt : r.failedSkips;
}

// P3 (GBE second review): only an undecodable / failed source skips the item. CoreLost (OSD Reset,
// another program took the core) and RingError must not turn into an item skip.
inline bool midPlayEndSkipsItem(bool needsTranscode, bool sourceError) {
    return needsTranscode || sourceError;
}

// GBE #621 P2-a: the shared-budget reset condition as one pure predicate (each term unit-tested).
inline bool skipBudgetResetGate(bool open, bool stopping, bool playing, bool decodedPicture, bool currentGen) {
    return open && !stopping && playing && decodedPicture && currentGen;
}

// GBE #621 P3-a: a NeedsTranscode (CAVLC rate/load) verdict can come a few seconds AFTER the first
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
};

// Play-thread / play-job -> main-loop handoff (never takes tvMu on the play thread, GBE P1-2).
class FailedItemHandoff {
public:
    void post(uint64_t generation, int itemIndex, unsigned priorSkips, std::string why) {
        std::lock_guard<std::mutex> lock(mu_);
        pending_ = {generation, itemIndex, priorSkips + 1, std::move(why)};
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

inline PlayRequest resumeLastChannel(Session& session, const std::string& id) {
    session.guideOpen = false;
    const int idx = channelIndexById(session.lineup, id);
    if (id.empty() || idx < 0)
        return {};
    const auto tune = session.selectIndex(idx);
    return playRequestFor(session.lineup, tune.generation, idx);
}

// GH#411: core reopen keeps the dial in progress. Cold boot / first open must
// use lastSaved only (preferCurrentDial=false) so startup selectIndex(primetime)
// cannot defeat SPELUNKR_LAST_CHANNEL or the logo + Open the Guide landing.
inline PlayRequest resumeCurrentOrLast(Session& session, const std::string& lastId,
                                       bool preferCurrentDial = true) {
    session.guideOpen = false;
    int idx = -1;
    if (preferCurrentDial) {
        idx = session.tv.index();
        if (idx < 0 || idx >= static_cast<int>(session.lineup.channels.size()))
            idx = channelIndexById(session.lineup, lastId);
    } else {
        idx = channelIndexById(session.lineup, lastId);
    }
    if (idx < 0)
        return {};
    const auto tune = session.selectIndex(idx);
    return playRequestFor(session.lineup, tune.generation, idx);
}

// GBE #623 frame 153: the channel the next core-open edge will resume, WITHOUT selecting it (no
// generation bump), or -1 when the open edge will land. Mirrors resumeCurrentOrLast / resumeLastChannel
// so main can arm the tuning chrome before the gate opens (owenbOpened then keeps it, no landing flash).
inline int resumeIndexPreview(const Session& session, const std::string& lastId, bool preferCurrentDial) {
    const int n = static_cast<int>(session.lineup.channels.size());
    int idx = -1;
    if (preferCurrentDial) {
        idx = session.tv.index();
        if (idx < 0 || idx >= n)
            idx = channelIndexById(session.lineup, lastId);
    } else if (!lastId.empty()) {
        idx = channelIndexById(session.lineup, lastId);
    }
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

}  // namespace spelunkr
}  // namespace misterplex
