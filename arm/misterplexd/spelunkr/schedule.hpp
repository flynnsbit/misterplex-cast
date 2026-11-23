#pragma once
// Video-only wrap of the official cycle-0 startIndex / epochMs model.
// Bumps, ads, and deck cards from gc()/wv() are OMITTED. Official-site sync is UNVERIFIED
// until a measured title match. Constants below are from index-zkq2Puf2.js (2026-11-07).

#include "libmisterplex/pluto_live.hpp"
#include "spelunkr/broadcast.hpp"
#include "spelunkr/pluto_live.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace misterplex {
namespace spelunkr {

inline constexpr int64_t kOfficialCardMs = 10000;          // pp
inline constexpr int64_t kOfficialRiMs = 20000;            // Ri
inline constexpr int64_t kOfficialSlideMinMs = 2500;       // ev
inline constexpr int64_t kOfficialSlideBaseMs = 1500;      // tv
inline constexpr int64_t kOfficialSlidePerCharMs = 55;     // nv

// Largest unix-second (or duration in seconds) whose millisecond form fits in int64.
// Used to reject untrusted Back When s/e timestamps before any *1000 or +.
inline constexpr int64_t kMaxSecRepresentableAsMs = std::numeric_limits<int64_t>::max() / 1000;
inline constexpr int64_t kScheduleMaxSaneOffsetMs = 24LL * 3600 * 1000;

// ADDENDUM 9 reviewer offset guard: clamp every schedule offset before prefetch/seek/OSD.
// Missing/unset absolute start (startUnix<=1), or offset > 24 h or ≥ duration → 0 when
// untrusted; cycle items (startUnix==0) keep sane in-item positions (< duration, ≤ 24 h).
inline int64_t sanitizeScheduleOffsetMs(int64_t offsetMs, int64_t durationMs, int64_t itemStartUnixSec) {
#ifdef MUT_SCHEDULE_NO_OFFSET_CLAMP
    return offsetMs < 0 ? 0 : offsetMs;
#else
    if (offsetMs < 0)
        return 0;
    if (itemStartUnixSec < 0)
        return 0;
    if (itemStartUnixSec == 1)
        return 0;
    if (offsetMs > kScheduleMaxSaneOffsetMs)
        return 0;
    if (durationMs > 0 && offsetMs >= durationMs)
        return durationMs - 1;
    return offsetMs;
#endif
}

inline bool addInt64(int64_t a, int64_t b, int64_t& out) {
    const int64_t maxv = std::numeric_limits<int64_t>::max();
    const int64_t minv = std::numeric_limits<int64_t>::min();
    if (b > 0 && a > maxv - b)
        return false;
    if (b < 0 && a < minv - b)
        return false;
    out = a + b;
    return true;
}

// Non-negative seconds -> milliseconds, or false if the product is not representable.
inline bool secToMs(int64_t sec, int64_t& ms) {
    if (sec < 0 || sec > kMaxSecRepresentableAsMs)
        return false;
    ms = sec * 1000;
    return true;
}

// Inclusive-start, exclusive-end window in unix seconds. Requires end > start
// and both values representable as milliseconds.
inline bool itemWindowSec(const Item& it, int64_t& startSec, int64_t& endSec) {
    startSec = it.startUnix;
    if (startSec <= 0 || startSec > kMaxSecRepresentableAsMs)
        return false;
    if (it.endUnix > 0) {
        if (it.endUnix <= startSec || it.endUnix > kMaxSecRepresentableAsMs)
            return false;
        endSec = it.endUnix;
        return true;
    }
    int64_t end = 0;
    if (!addInt64(startSec, static_cast<int64_t>(it.durationSec), end))
        return false;
    if (end <= startSec || end > kMaxSecRepresentableAsMs)
        return false;
    endSec = end;
    return true;
}

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
    bool unsupported = false;
    std::string unsupportedReason;
};

inline bool manifestIsAbsolute(const Manifest& man) {
    if (man.absoluteClock)
        return true;
    for (const auto& it : man.items)
        if (it.startUnix > 0)
            return true;
    return false;
}

inline bool itemHasLocator(const Item& it) {
    if (it.id == "pluto" && !it.file.empty() && misterplex::plutoLiveEnabled())
        return true;
    if (!it.id.empty() && !it.file.empty() && it.id != "pluto")
        return true;
    return hostAllowed(it.directUrl) || hostAllowed(it.fallbackUrl);
}

inline std::string itemPlayUrl(const Item& it) {
    if (!it.id.empty() && !it.file.empty()) {
        const std::string u = archiveUrl(it.id, it.file);
        if (!u.empty())
            return u;
    }
    if (hostAllowed(it.directUrl))
        return it.directUrl;
    if (hostAllowed(it.fallbackUrl))
        return it.fallbackUrl;
    return {};
}

inline void fillNowPlayingFromItem(NowPlaying& np, const Channel& ch, const Item& it, int idx,
                                   int64_t nowMs, int64_t offsetMs, int64_t durationMs,
                                   int64_t itemStartUnixSec = 0) {
    np.ok = true;
    np.channelId = ch.id;
    np.channelName = ch.name;
    np.dial = ch.dial;
    np.itemIndex = idx;
    np.title = it.title;
    np.year = it.year;
    np.archiveId = it.id;
    np.file = it.file;
    np.url = itemPlayUrl(it);
    np.offsetMs = sanitizeScheduleOffsetMs(offsetMs, durationMs, itemStartUnixSec);
    np.durationMs = durationMs;
    np.remainingMs = durationMs > np.offsetMs ? durationMs - np.offsetMs : 0;
    np.moov = it.moov;
    np.startMs = (nowMs >= np.offsetMs) ? nowMs - np.offsetMs : 0;
    int64_t endMs = 0;
    np.endMs = addInt64(np.startMs, durationMs, endMs) ? endMs : std::numeric_limits<int64_t>::max();
    np.unsupported = !it.playable || !it.flag.empty();
    np.unsupportedReason = it.flag;
    if (np.unsupported && np.title.empty())
        np.title = it.flag;
}

inline int64_t itemDurationMs(const Item& it) {
    int64_t st = 0, en = 0, ms = 0;
    if (itemWindowSec(it, st, en)) {
        if (secToMs(en - st, ms) && ms > 0)
            return ms;
        return 0;
    }
    // Invalid absolute window: do not fall back to durationSec (a huge end
    // can sit beside a normal d). Cycle items have startUnix == 0; a
    // negative start is also untrusted, not a cycle item.
    if (it.startUnix != 0)
        return 0;
    if (!secToMs(static_cast<int64_t>(it.durationSec), ms) || ms <= 0)
        return 0;
    return ms;
}

inline NowPlaying nowPlayingAbsolute(const Channel& ch, const Manifest& man, int64_t nowMs) {
    NowPlaying np;
    np.note = "backwhen absolute clock; ads omitted; official sync UNVERIFIED";
    if (man.items.empty())
        return np;
    const int64_t nowSec = nowMs / 1000;
    for (int i = 0; i < static_cast<int>(man.items.size()); ++i) {
        const Item& it = man.items[static_cast<size_t>(i)];
        if (it.ad)
            continue;
        int64_t st = 0, en = 0;
        if (!itemWindowSec(it, st, en))
            continue;
        if (st <= nowSec && nowSec < en) {
            const int64_t durMs = itemDurationMs(it);
            int64_t deltaMs = 0, offMs = 0;
            if (durMs <= 0 || !secToMs(nowSec - st, deltaMs) || !addInt64(deltaMs, nowMs % 1000, offMs))
                continue;
            fillNowPlayingFromItem(np, ch, it, i, nowMs, offMs, durMs, st);
            return np;
        }
    }
    return np;
}

// Cycle 0: items[startIndex..] then items[0..startIndex). Later cycles: items[0..).
// nowMs < epochMs → not ok.
inline NowPlaying nowPlayingVideoOnly(const Channel& ch, const Manifest& man, int64_t nowMs) {
    if (manifestIsAbsolute(man))
        return nowPlayingAbsolute(ch, man, nowMs);
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
            np.offsetMs = sanitizeScheduleOffsetMs(into - acc, d, it.startUnix);
            np.durationMs = d;
            np.remainingMs = d - np.offsetMs;
            np.moov = it.moov;
            np.startMs = (nowMs >= np.offsetMs) ? nowMs - np.offsetMs : 0;
            int64_t endMs = 0;
            np.endMs = addInt64(np.startMs, d, endMs) ? endMs : std::numeric_limits<int64_t>::max();
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
    if (manifestIsAbsolute(man)) {
        const int n = static_cast<int>(man.items.size());
        const int64_t nowSec = nowMs / 1000;
        int best = -1;
        int64_t bestStart = 0;
        for (int i = 0; i < n; ++i) {
            const Item& it = man.items[static_cast<size_t>(i)];
            if (it.ad)
                continue;
            if (it.startUnix >= nowSec && (best < 0 || it.startUnix < bestStart)) {
                best = i;
                bestStart = it.startUnix;
            }
        }
        // Do not fall back to an expired first item: that loops stale entries
        // after the Back When window ends. Upcoming (start >= now) only.
        if (best < 0)
            return np;
        const Item& it = man.items[static_cast<size_t>(best)];
        const int64_t d = itemDurationMs(it);
        if (d <= 0 && it.playable)
            return np;
        fillNowPlayingFromItem(np, ch, it, best, nowMs, 0, d > 0 ? d : 0, it.startUnix);
        np.firstItemFallback = true;
        np.note = "first/next item; live schedule not ok";
        return np;
    }
    const Item& it = man.items[0];
    const int64_t d = static_cast<int64_t>(it.durationSec) * 1000;
    if (d <= 0 || !itemHasLocator(it))
        return np;
    fillNowPlayingFromItem(np, ch, it, 0, nowMs, 0, d, it.startUnix);
    np.cycle = 0;
    np.firstItemFallback = true;
    np.note = "first item; live schedule not ok";
    return np;
}

// Production + host tests: stamp what we actually open (live remux pipe never inherits schedule ms).
inline int64_t stampedPlayOffsetMs(const NowPlaying& np) {
    if (np.firstItemFallback)
        return -1;
    return pluto_live::plutoSpelunkrOpenStartMs(np.url, np.offsetMs);
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
    int next = (idx + 1) % n;
    if (manifestIsAbsolute(man)) {
        int tries = 0;
        while (tries < n && man.items[static_cast<size_t>(next)].ad) {
            next = (next + 1) % n;
            ++tries;
        }
    }
    const Item& it = man.items[static_cast<size_t>(next)];
    const int64_t d = manifestIsAbsolute(man) ? itemDurationMs(it)
                                              : static_cast<int64_t>(it.durationSec) * 1000;
    if ((d <= 0 && it.playable) || (it.playable && !itemHasLocator(it)))
        return np;
    fillNowPlayingFromItem(np, ch, it, next, nowMs, 0, d > 0 ? d : 0, it.startUnix);
    np.cycle = cur.cycle;
    return np;
}

inline NowPlaying playItemAtIndex(const Channel& ch, const Manifest& man, int itemIndex, int64_t nowMs) {
    NowPlaying np;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(man.items.size()))
        return np;
    const Item& it = man.items[static_cast<size_t>(itemIndex)];
    const int64_t durationMs = manifestIsAbsolute(man) ? itemDurationMs(it)
                                                       : static_cast<int64_t>(it.durationSec) * 1000;
    if ((durationMs <= 0 && it.playable) || (it.playable && !itemHasLocator(it)))
        return np;
    fillNowPlayingFromItem(np, ch, it, itemIndex, nowMs, 0, durationMs > 0 ? durationMs : 0, it.startUnix);
    np.note = "cached header retry";
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

inline bool itemMatchesIdentity(const Item& it, const std::string& archiveId,
                                const std::string& file, const std::string& url) {
    if (!archiveId.empty() && !file.empty() && it.id == archiveId && it.file == file)
        return true;
    if (!url.empty() && (it.directUrl == url || it.fallbackUrl == url || itemPlayUrl(it) == url))
        return true;
    return false;
}

// Locate an item after a manifest swap. If `hint` still matches the identity,
// keep it (repeated A,B,A,D must not jump back to the first A). Otherwise
// pick the match closest to the hint.
inline int itemIndexByIdentity(const Manifest& man, const std::string& archiveId,
                               const std::string& file, const std::string& url,
                               int hint = -1) {
    const int n = static_cast<int>(man.items.size());
    if (hint >= 0 && hint < n &&
        itemMatchesIdentity(man.items[static_cast<size_t>(hint)], archiveId, file, url))
        return hint;
    int best = -1;
    int bestDist = 0;
    for (int i = 0; i < n; ++i) {
        if (!itemMatchesIdentity(man.items[static_cast<size_t>(i)], archiveId, file, url))
            continue;
        int dist = hint >= 0 ? i - hint : i;
        if (dist < 0)
            dist = -dist;
        if (best < 0 || dist < bestDist) {
            best = i;
            bestDist = dist;
        }
    }
    return best;
}

// Prefer the numeric index when that slot still matches the identity. Search
// only on a mismatch. If identity was given and nothing matches (pruned), -1.
inline int effectiveEndedItemIndex(const Manifest& man, int endedItemIndex,
                                   const std::string& archiveId, const std::string& file,
                                   const std::string& url) {
    const bool haveIdent = !archiveId.empty() || !file.empty() || !url.empty();
    if (haveIdent) {
        const int byId = itemIndexByIdentity(man, archiveId, file, url, endedItemIndex);
        return byId;
    }
    if (endedItemIndex >= 0 && endedItemIndex < static_cast<int>(man.items.size()))
        return endedItemIndex;
    return -1;
}

inline NowPlaying nextAfterEof(const Channel& ch, const Manifest& man, NowPlaying live,
                              int endedItemIndex, int64_t nowMs, int64_t endedOffsetMs = -1,
                              int64_t endedPlayedMs = -1, const std::string& endedId = {},
                              const std::string& endedFile = {}, const std::string& endedUrl = {}) {
    if (!live.ok)
        return {};
    NowPlaying from = live;
    const int ended = effectiveEndedItemIndex(man, endedItemIndex, endedId, endedFile, endedUrl);
    if (ended >= 0) {
        from.itemIndex = ended;
        endedItemIndex = ended;
    } else if (!endedId.empty() || !endedFile.empty() || !endedUrl.empty()) {
        // Ended item was pruned out of the new manifest: stay on live, do not
        // skip from a stale numeric slot that now holds a different programme.
        return live;
    }
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
        int64_t endMs = 0;
        skipped.endMs = addInt64(nowMs, skipped.durationMs, endMs) ? endMs
                                                                   : std::numeric_limits<int64_t>::max();
        return skipped;
    }
    // N-2: if live-now is already that successor, keep its mid-programme offset.
    if (live.itemIndex == skipped.itemIndex)
        return live;
    return skipped;
}

}  // namespace spelunkr
}  // namespace misterplex
