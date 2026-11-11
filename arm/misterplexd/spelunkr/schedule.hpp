#pragma once
// Video-only wrap of the official cycle-0 startIndex / epochMs model.
// Bumps, ads, and deck cards from gc()/wv() are OMITTED. Official-site sync is UNVERIFIED
// until a measured title match. Constants below are from index-zkq2Puf2.js (2026-11-07).

#include "spelunkr/broadcast.hpp"

#include <cstdint>
#include <string>

namespace misterplex {
namespace spelunkr {

inline constexpr int64_t kOfficialCardMs = 10000;          // pp
inline constexpr int64_t kOfficialRiMs = 20000;            // Ri
inline constexpr int64_t kOfficialSlideMinMs = 2500;       // ev
inline constexpr int64_t kOfficialSlideBaseMs = 1500;      // tv
inline constexpr int64_t kOfficialSlidePerCharMs = 55;     // nv

struct NowPlaying {
    bool ok = false;
    bool firstItemFallback = false;
    std::string channelId;
    std::string channelName;
    int dial = 0;
    int itemIndex = -1;
    std::string title;
    std::string year;
    std::string archiveId;
    std::string file;
    std::string url;
    int64_t offsetMs = 0;
    int64_t remainingMs = 0;
    int64_t durationMs = 0;
    int64_t moov = 0;
    int64_t startMs = 0;
    int64_t endMs = 0;
    int cycle = 0;
    std::string note;  // "video-only; bumps omitted"
};

// Cycle 0: items[startIndex..] then items[0..startIndex). Later cycles: items[0..).
// nowMs < epochMs → not ok.
inline NowPlaying nowPlayingVideoOnly(const Channel& ch, const Manifest& man, int64_t nowMs) {
    NowPlaying np;
    np.note = "video-only; bumps/ads omitted; official sync UNVERIFIED";
    const VersionPtr* ver = currentVersion(ch);
    if (!ver || man.items.empty())
        return np;
    const int n = static_cast<int>(man.items.size());
    int start = ver->startIndex;
#ifdef MUT_SP_SKIP_STARTINDEX
    start = 0;
#endif
    if (start < 0 || start >= n)
        return np;
    if (nowMs < ver->epochMs)
        return np;

    auto durMs = [&](int i) -> int64_t {
        return static_cast<int64_t>(man.items[static_cast<size_t>(i)].durationSec) * 1000;
    };
    int64_t cycle0 = 0;
    for (int i = 0; i < n; ++i)
        cycle0 += durMs((start + i) % n);
    if (cycle0 <= 0)
        return np;
    int64_t full = 0;
    for (int i = 0; i < n; ++i)
        full += durMs(i);
    if (full <= 0)
        return np;

    const int64_t elapsed = nowMs - ver->epochMs;
    int cycle = 0;
    int64_t into = elapsed;
    int origin = start;
    if (elapsed >= cycle0) {
        const int64_t rem = elapsed - cycle0;
        cycle = 1 + static_cast<int>(rem / full);
        into = rem % full;
        origin = 0;
    }

    int64_t acc = 0;
    for (int k = 0; k < n; ++k) {
        const int idx = (origin + k) % n;
        const int64_t d = durMs(idx);
        if (acc + d > into) {
            const Item& it = man.items[static_cast<size_t>(idx)];
            np.ok = true;
            np.channelId = ch.id;
            np.channelName = ch.name;
            np.dial = ch.dial;
            np.itemIndex = idx;
            np.title = it.title;
            np.year = it.year;
            np.archiveId = it.id;
            np.file = it.file;
            np.url = archiveUrl(it.id, it.file);
            np.offsetMs = into - acc;
            np.durationMs = d;
            np.remainingMs = d - np.offsetMs;
            np.moov = it.moov;
            np.startMs = nowMs - np.offsetMs;
            np.endMs = np.startMs + d;
            np.cycle = cycle;
            return np;
        }
        acc += d;
    }
    return np;
}

// Zap must leave the previous channel even when nowMs is before epochMs.
inline NowPlaying nowPlayingOrFirst(const Channel& ch, const Manifest& man, int64_t nowMs) {
    NowPlaying np = nowPlayingVideoOnly(ch, man, nowMs);
    if (np.ok)
        return np;
    if (man.items.empty())
        return np;
    const Item& it = man.items[0];
    const int64_t d = static_cast<int64_t>(it.durationSec) * 1000;
    if (d <= 0 || it.id.empty() || it.file.empty())
        return np;
    np.ok = true;
    np.channelId = ch.id;
    np.channelName = ch.name;
    np.dial = ch.dial;
    np.itemIndex = 0;
    np.title = it.title;
    np.year = it.year;
    np.archiveId = it.id;
    np.file = it.file;
    np.url = archiveUrl(it.id, it.file);
    np.offsetMs = 0;
    np.durationMs = d;
    np.remainingMs = d;
    np.moov = it.moov;
    np.startMs = nowMs;
    np.endMs = nowMs + d;
    np.cycle = 0;
    np.firstItemFallback = true;
    np.note = "first item; live schedule not ok";
    return np;
}

// Only the original fallback gets one unconditional from-zero replay at EOF.
inline int64_t stampedPlayOffsetMs(const NowPlaying& np) {
    return np.firstItemFallback ? -1 : np.offsetMs;
}

// Next manifest item at offset 0. Same channel, no generation bump. Used when the
// scheduled file's Range body is empty or not ISO Media.
inline NowPlaying skipToNextItem(const Channel& ch, const Manifest& man, const NowPlaying& cur, int64_t nowMs) {
    NowPlaying np;
    np.note = cur.note;
    if (man.items.empty())
        return np;
    const int n = static_cast<int>(man.items.size());
    int idx = cur.itemIndex;
    if (idx < 0 || idx >= n)
        idx = -1;
    const int next = (idx + 1) % n;
    const Item& it = man.items[static_cast<size_t>(next)];
    const int64_t d = static_cast<int64_t>(it.durationSec) * 1000;
    if (d <= 0 || it.id.empty() || it.file.empty())
        return np;
    np.ok = true;
    np.channelId = ch.id;
    np.channelName = ch.name;
    np.dial = ch.dial;
    np.itemIndex = next;
    np.title = it.title;
    np.year = it.year;
    np.archiveId = it.id;
    np.file = it.file;
    np.url = archiveUrl(it.id, it.file);
    np.offsetMs = 0;
    np.durationMs = d;
    np.remainingMs = d;
    np.moov = it.moov;
    np.startMs = nowMs;
    np.endMs = nowMs + d;
    np.cycle = cur.cycle;
    return np;
}

// Allow container EOF up to two seconds early, but never an immediate EOF.
// Unknown played length is conservative: a from-zero play cannot loop again.
// Slack must not exceed the item duration: a 1s clip that EOFs at 1ms is still short.
inline constexpr int64_t kEofReplaySlackMs = 2000;

inline bool fromZeroPlayIsShort(int64_t endedPlayedMs, int64_t durationMs) {
    if (endedPlayedMs <= 0)
        return true;
    const int64_t minHealthyMs =
        durationMs > kEofReplaySlackMs ? durationMs - kEofReplaySlackMs : durationMs;
    return endedPlayedMs < minHealthyMs;
}

inline NowPlaying nextAfterEof(const Channel& ch, const Manifest& man, NowPlaying live,
                              int endedItemIndex, int64_t nowMs, int64_t endedOffsetMs = -1,
                              int64_t endedPlayedMs = -1) {
    if (!live.ok)
        return {};
    NowPlaying from = live;
    if (endedItemIndex >= 0)
        from.itemIndex = endedItemIndex;
    // Schedule-order successor of the ended item (N-8). Never treat a *different*
    // live.itemIndex as "already moved past" — that replayed an overlong item's tail.
    NowPlaying skipped = skipToNextItem(ch, man, from, nowMs);
    if (!skipped.ok)
        return {};
    const int n = static_cast<int>(man.items.size());
    // N-9: one-item / wrap-to-same. Key on the *ended play's start offset* (carried
    // with the EOF), not live.offsetMs — main recomputes live from the clock each
    // time. A from-zero play loops only if it reached essentially the full duration.
    // endedOffsetMs < 0 (unknown / first-item fallback) or > 0 (joined mid-item)
    // still clamps to 0 once, regardless of played length.
    if (n <= 1 || (endedItemIndex >= 0 && skipped.itemIndex == endedItemIndex)) {
        if (endedOffsetMs == 0 && fromZeroPlayIsShort(endedPlayedMs, skipped.durationMs))
            return {};
        skipped.offsetMs = 0;
        skipped.remainingMs = skipped.durationMs;
        skipped.startMs = nowMs;
        skipped.endMs = nowMs + skipped.durationMs;
        return skipped;
    }
    // N-2: if live-now is already that successor, keep its mid-programme offset.
    if (live.itemIndex == skipped.itemIndex)
        return live;
    return skipped;
}

}  // namespace spelunkr
}  // namespace misterplex
