#pragma once

// Pure helpers for neighbour-header prefetch: cache-key identity with the
// on-path tune, skip policy so a large moov cannot OOM lab, and the
// SPELUNKR_NEIGHBOR_PREFETCH flag (defaults on; 0/off/false/no disables).

#include "spelunkr/archive_resolve.hpp"
#include "spelunkr/header_cache.hpp"

#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace misterplex {
namespace spelunkr {

// 6.3x libav parse heap + 1.4x recorded ranges at 3 MiB ~= 23.1 MiB, under
// the 24 MB growth gate. Unknown or larger moovs are not prefetched; the
// recorder also aborts at kMaxHeaderCacheBytes as a backstop.
inline constexpr int64_t kNeighborPrefetchMaxMoovBytes = 3ll * 1024 * 1024;
// Start only after first picture plus this settle, so cutover RAM is left
// alone and a failing tune never starts a neighbour job.
inline constexpr int64_t kNeighborPrefetchSettleMs = 2000;

struct NeighborPrefetchSpec {
    std::string id;
    std::string url;
    int64_t size = -1;
    std::string etag;
    std::string archiveId;
    std::string file;
    int64_t moov = 0;
};

inline bool neighborPrefetchWanted() {
    const char* e = std::getenv("SPELUNKR_NEIGHBOR_PREFETCH");
    if (!e || !*e)
        return true;
    std::string v;
    for (; *e; ++e) {
        const char c = *e;
        v.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return !(v == "0" || v == "n" || v == "no" || v == "f" || v == "false" || v == "off");
}

inline bool isCanonicalArchiveDownloadUrl(const std::string& url) {
    return url.find("://archive.org/download/") != std::string::npos ||
           url.find("://www.archive.org/download/") != std::string::npos;
}

// Always prefer a resolved data-node locator when archiveId is set, even if
// the spec already carries the canonical archive.org/download URL (or a
// bogus host). That is the same (url, size, etag) the on-path tune keys on.
inline bool applyArchiveLocator(NeighborPrefetchSpec& s, const ArchiveCache& cache) {
    if (s.archiveId.empty())
        return false;
    ArchiveCacheEntry e;
    if (!cache.lookup(s.archiveId, s.file, e) || e.url.empty())
        return false;
    if (cache.locatorExpired(e))
        return false;
    // Do not copy a cached NotMp4 or a disallowed host (browse-lane #647 P3-C/D).
    if (e.head == ArchiveHead::NotMp4)
        return false;
    if (!hostAllowed(e.url))
        return false;
    s.url = e.url;
    if (e.size > 0)
        s.size = e.size;
    if (!e.etag.empty())
        s.etag = e.etag;
    return true;
}

inline HeaderCacheKey neighborHeaderKey(const NeighborPrefetchSpec& s) {
    return HeaderCacheKey{s.url, s.size, s.etag};
}

// True when the spec is keyed on a resolved data-node URL (or a non-archive
// direct URL). A canonical archive.org/download URL must not be stored: the
// on-path tune would miss it.
inline bool neighborKeyReady(const NeighborPrefetchSpec& s) {
    if (s.url.empty())
        return false;
    if (!s.archiveId.empty() && isCanonicalArchiveDownloadUrl(s.url))
        return false;
    // Same host gate as on-path resolveScheduledPlay (browse-lane #647 P3-C).
    if (!hostAllowed(s.url))
        return false;
    return true;
}

enum class NeighborSkipReason {
    None = 0,
    NoUrl,
    UnknownMoov,
    OverCacheCap,
    OverRamBudget,
    DeadTailMoov,
    TuneTimeout,
    CapHit,
    NotMp4,
    HostNotAllowed,
    NoResolvedKey,
    PlutoLiveHls,  // pluto channel: live HLS, there is no MP4 moov to prefetch (not "unknown_moov")
};

inline std::mutex& neighborCapHitMu() {
    static std::mutex m;
    return m;
}
inline std::set<std::string>& neighborCapHitKeys() {
    static std::set<std::string> s;
    return s;
}
inline std::string neighborCapHitRow(const std::string& id, const std::string& file) {
    return id + "\n" + file;
}
inline void markNeighborCapHit(const std::string& id, const std::string& file) {
    if (id.empty())
        return;
    std::lock_guard<std::mutex> g(neighborCapHitMu());
    neighborCapHitKeys().insert(neighborCapHitRow(id, file));
}
inline bool neighborHadCapHit(const std::string& id, const std::string& file) {
    if (id.empty())
        return false;
    std::lock_guard<std::mutex> g(neighborCapHitMu());
    return neighborCapHitKeys().count(neighborCapHitRow(id, file)) != 0;
}
inline void clearNeighborCapHitsForTest() {
    std::lock_guard<std::mutex> g(neighborCapHitMu());
    neighborCapHitKeys().clear();
}

inline NeighborSkipReason neighborSkipReason(const NeighborPrefetchSpec& s, const ArchiveCache* cache) {
#ifndef MUT_NEIGHBOR_PLUTO_AS_UNKNOWN_MOOV
    if (s.archiveId == "pluto")
        return NeighborSkipReason::PlutoLiveHls;
#endif
    if (s.moov <= 0)
        return NeighborSkipReason::UnknownMoov;
    if (s.moov > kMaxHeaderCacheBytes)
        return NeighborSkipReason::OverCacheCap;
    if (s.moov > kNeighborPrefetchMaxMoovBytes)
        return NeighborSkipReason::OverRamBudget;
    if (cache && !s.archiveId.empty()) {
        ArchiveCacheEntry e;
        if (cache->lookup(s.archiveId, s.file, e)) {
            if (e.tuneTimeout)
                return NeighborSkipReason::TuneTimeout;
            if (e.deadTailMoov && !cache->deadVerdictExpired(e))
                return NeighborSkipReason::DeadTailMoov;
        }
        if (neighborHadCapHit(s.archiveId, s.file))
            return NeighborSkipReason::CapHit;
        if (e.head == ArchiveHead::NotMp4)
            return NeighborSkipReason::NotMp4;
    }
    if (!s.url.empty() && !isCanonicalArchiveDownloadUrl(s.url) && !hostAllowed(s.url))
        return NeighborSkipReason::HostNotAllowed;
    if (s.url.empty() && s.archiveId.empty())
        return NeighborSkipReason::NoUrl;
    return NeighborSkipReason::None;
}

inline const char* neighborSkipName(NeighborSkipReason r) {
    switch (r) {
    case NeighborSkipReason::None:
        return "none";
    case NeighborSkipReason::NoUrl:
        return "no_url";
    case NeighborSkipReason::UnknownMoov:
        return "unknown_moov";
    case NeighborSkipReason::OverCacheCap:
        return "over_cache_cap";
    case NeighborSkipReason::OverRamBudget:
        return "over_ram_budget";
    case NeighborSkipReason::DeadTailMoov:
        return "dead_tail_moov";
    case NeighborSkipReason::TuneTimeout:
        return "tune_timeout";
    case NeighborSkipReason::CapHit:
        return "cap_hit";
    case NeighborSkipReason::NotMp4:
        return "not_mp4";
    case NeighborSkipReason::HostNotAllowed:
        return "host_not_allowed";
    case NeighborSkipReason::NoResolvedKey:
        return "no_resolved_key";
    case NeighborSkipReason::PlutoLiveHls:
        return "pluto_live_hls";
    }
    return "unknown";
}

// 8.8x parse heap of a 2 MiB metadata body is ~18 MiB on x86 (browse-lane #647 P3-F).
// Cap neighbour metadata GETs so the peak stays under the 24 MB growth gate
// with margin on ARM. Preflight stays at 1024 bytes and is unaffected.
inline constexpr size_t kNeighborMetaMaxBody = 768u * 1024u;

inline size_t neighborGetMaxBody(size_t requested) {
    return requested > kNeighborMetaMaxBody ? kNeighborMetaMaxBody : requested;
}

enum class NeighborFetchDecision { Skip, Warm, Fetch };

struct NeighborFetchOutcome {
    NeighborFetchDecision decision = NeighborFetchDecision::Skip;
    NeighborSkipReason skip = NeighborSkipReason::None;
    HeaderCacheKey key;
    NeighborPrefetchSpec spec;
};

inline NeighborSkipReason neighborUnreadyReason(const NeighborPrefetchSpec& s) {
    if (s.url.empty())
        return NeighborSkipReason::NoUrl;
    if (!s.archiveId.empty() && isCanonicalArchiveDownloadUrl(s.url))
        return NeighborSkipReason::NoResolvedKey;
    if (!hostAllowed(s.url))
        return NeighborSkipReason::HostNotAllowed;
    return NeighborSkipReason::None;
}

// Pure fetch-lambda decision: apply locator, optional resolve result, skip
// policy, hostAllowed, and hasEntry (url+size+etag, not filePath).
inline NeighborFetchOutcome decideNeighborFetch(
    NeighborPrefetchSpec spec, const ArchiveCache& cache,
    const std::function<bool(const HeaderCacheKey&)>& hasEntry,
    const ArchiveResolve* resolved = nullptr) {
    applyArchiveLocator(spec, cache);
    if (resolved) {
        if (resolved->kind == ArchiveResolveKind::Ok) {
            if (!resolved->url.empty())
                spec.url = resolved->url;
            if (resolved->size > 0)
                spec.size = resolved->size;
            if (!resolved->etag.empty())
                spec.etag = resolved->etag;
        } else if (resolved->kind == ArchiveResolveKind::NotMp4) {
            NeighborFetchOutcome o;
            o.spec = std::move(spec);
            o.skip = NeighborSkipReason::NotMp4;
            return o;
        }
    }
    NeighborFetchOutcome o;
    o.spec = spec;
    o.skip = neighborSkipReason(spec, &cache);
    if (o.skip != NeighborSkipReason::None)
        return o;
    o.skip = neighborUnreadyReason(spec);
    if (o.skip != NeighborSkipReason::None)
        return o;
    // Same direct-play cache key as on-path resolveScheduledPlay (size==0 sentinel, not -1).
    // Draft #619 still resolves archive neighbours only through resolveArchivePlay; first-party
    // Back When URLs skip resolveArchivePlay but must share the tune key (#656 N3).
    applyDirectPlayHeaderCacheKey(spec.size, spec.archiveId, spec.file, spec.url);
    o.key = neighborHeaderKey(spec);
    if (hasEntry && hasEntry(o.key)) {
        o.decision = NeighborFetchDecision::Warm;
        return o;
    }
    o.decision = NeighborFetchDecision::Fetch;
    return o;
}

inline bool neighborNeedsResolve(const NeighborPrefetchSpec& spec, const ArchiveCache& cache) {
    NeighborPrefetchSpec s = spec;
    applyArchiveLocator(s, cache);
    if (neighborSkipReason(s, &cache) == NeighborSkipReason::NotMp4)
        return false;
    return !neighborKeyReady(s) && !s.archiveId.empty();
}

}  // namespace spelunkr
}  // namespace misterplex
