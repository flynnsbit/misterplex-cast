#pragma once
// Archive.org item URL cache + preflight classify. Network stays in the caller
// (tlsVerifiedGet). A cache hit of a known-MP4 locator skips both GETs.

#include "log_redact.hpp"
#include "spelunkr/broadcast.hpp"
#include "spelunkr/pluto_live.hpp"
#include "libmisterplex/remote_h264_transcode.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace misterplex {
namespace spelunkr {

enum class ArchiveHead { Unknown, Mp4, NotMp4 };

struct ArchiveCacheEntry {
    std::string url;
    ArchiveHead head = ArchiveHead::Unknown;
    // P2 header cache key material, parsed once from archive.org file metadata and reused by
    // every later resolve of the same (id, file) without an extra fetch.
    int64_t size = -1;
    std::string etag;
    // Session cache of a DeadTailMoov verdict. Stored with the warmth it was decided under;
    // invalidated when the on-disk header cache later has an entry (warmth flipped),
    // or when the TTL expires (browse-lane #647 P3-1).
    bool deadTailMoov = false;
    bool deadDecidedWarm = false;
    int64_t deadAtMs = 0;
    // browse-lane #647 P2-7: a prefetch that timed out on a large item is skipped for
    // the rest of the session (same invalidation as DeadTailMoov when the header cache warms).
    bool tuneTimeout = false;
    // browse-lane #647 P2-1: wall-clock stamp for on-disk locator rows (not persisted for
    // dead/timeout flags). 0 means the row was never stamped and must not be restored.
    int64_t savedAtMs = 0;
};

// browse-lane #647 P3-1: a cached DeadTailMoov verdict is retried after this TTL (10 min).
inline constexpr int64_t kDeadTailMoovTtlMs = 10ll * 60 * 1000;
// browse-lane #647 P2-1: persisted locators expire after 24 h and are dropped on 404/403/410
// or a size/etag mismatch. Use wall time so a reboot does not reset the clock.
inline constexpr int64_t kLocatorPersistTtlMs = 24ll * 60 * 60 * 1000;

inline int64_t archiveCacheDefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int64_t archiveCacheWallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline bool archiveLocatorGoneStatus(int status) {
    return status == 404 || status == 403 || status == 410;
}

inline bool archiveLocatorGoneMessage(const std::string& msg) {
    if (msg.empty())
        return false;
    const auto has = [&](const char* a) { return msg.find(a) != std::string::npos; };
    return has(" 404") || has("HTTP 404") || has("404 Not Found") || has("status 404") ||
           has(" 403") || has("HTTP 403") || has("403 Forbidden") || has("status 403") ||
           has(" 410") || has("HTTP 410") || has("410 Gone") || has("status 410");
}

inline bool locatorSizeEtagMismatch(int64_t cachedSize, const std::string& cachedEtag, int64_t size,
                                    const std::string& etag) {
    if (cachedSize > 0 && size > 0 && cachedSize != size)
        return true;
    if (!cachedEtag.empty() && !etag.empty() && cachedEtag != etag)
        return true;
    return false;
}

class ArchiveCache {
public:
    void storeItemMeta(const std::string& id, ArchiveItemMeta meta) {
        std::lock_guard<std::mutex> g(mu_);
        items_[id] = std::move(meta);
    }
    // True means the item is cached, even if this file has no metadata row.
    bool lookupFileMeta(const std::string& id, const std::string& file, std::string& url,
                        ArchiveFileMeta& out) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = items_.find(id);
        if (it == items_.end())
            return false;
        url = archiveItemUrl(it->second.d1, it->second.dir, file);
        const auto row = it->second.files.find(file);
        out = row == it->second.files.end() ? ArchiveFileMeta{} : row->second;
        return true;
    }
    bool lookup(const std::string& id, const std::string& file, ArchiveCacheEntry& out) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = m_.find(key(id, file));
        if (it == m_.end())
            return false;
        out = it->second;
        return true;
    }
    void storeUrl(const std::string& id, const std::string& file, std::string url) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = m_[key(id, file)];
        e.url = std::move(url);
        e.savedAtMs = locatorNowMsLocked();
        persistLocked();
    }
    void storeHead(const std::string& id, const std::string& file, ArchiveHead h) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = m_[key(id, file)];
        e.head = h;
        e.savedAtMs = locatorNowMsLocked();
        persistLocked();
    }
    void storeSizeEtag(const std::string& id, const std::string& file, int64_t size, std::string etag) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = m_[key(id, file)];
        e.size = size;
        e.etag = std::move(etag);
        e.savedAtMs = locatorNowMsLocked();
        persistLocked();
    }
    void dropLocator(const std::string& id, const std::string& file) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = m_.find(key(id, file));
        if (it == m_.end())
            return;
        it->second.url.clear();
        it->second.head = ArchiveHead::Unknown;
        it->second.size = -1;
        it->second.etag.clear();
        it->second.savedAtMs = 0;
        if (!it->second.deadTailMoov && !it->second.tuneTimeout)
            m_.erase(it);
        persistLocked();
    }
    void setNowMsForTest(int64_t n) { nowOverrideMs_ = n; }
    int64_t nowMs() const { return nowOverrideMs_ >= 0 ? nowOverrideMs_ : archiveCacheDefaultNowMs(); }
    int64_t locatorNowMs() const { return nowOverrideMs_ >= 0 ? nowOverrideMs_ : archiveCacheWallNowMs(); }

    bool locatorExpired(const ArchiveCacheEntry& e) const {
        if (e.savedAtMs <= 0)
            return true;
        return locatorNowMs() - e.savedAtMs >= kLocatorPersistTtlMs;
    }

    bool deadVerdictExpired(const ArchiveCacheEntry& e) const {
        if (!e.deadTailMoov)
            return false;
        if (e.deadAtMs <= 0)
            return false;
        return nowMs() - e.deadAtMs >= kDeadTailMoovTtlMs;
    }

    void storeDeadTailMoov(const std::string& id, const std::string& file, bool decidedWarm) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = m_[key(id, file)];
        e.deadTailMoov = true;
        e.deadDecidedWarm = decidedWarm;
        e.deadAtMs = nowOverrideMs_ >= 0 ? nowOverrideMs_ : archiveCacheDefaultNowMs();
    }
    void clearDeadTailMoov(const std::string& id, const std::string& file) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = m_.find(key(id, file));
        if (it == m_.end())
            return;
        it->second.deadTailMoov = false;
        it->second.deadDecidedWarm = false;
        it->second.deadAtMs = 0;
    }
    void storeTuneTimeout(const std::string& id, const std::string& file) {
        std::lock_guard<std::mutex> g(mu_);
        m_[key(id, file)].tuneTimeout = true;
    }
    void clearTuneTimeout(const std::string& id, const std::string& file) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = m_.find(key(id, file));
        if (it == m_.end())
            return;
        it->second.tuneTimeout = false;
    }
    bool cachedTuneTimeout(const std::string& id, const std::string& file) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = m_.find(key(id, file));
        return it != m_.end() && it->second.tuneTimeout;
    }
    size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return m_.size();
    }

    // #647 restart-warm: persist locator rows (url/size/etag/head) next to the header cache
    // so a sidecar restart can skip the 2 s metadata GET. Session flags (dead, timeout) stay RAM-only.
    void setPersistPath(std::string path) { persistPath_ = std::move(path); }
    void loadPersisted() {
        if (persistPath_.empty())
            return;
        FILE* f = std::fopen(persistPath_.c_str(), "rb");
        if (!f)
            return;
        std::lock_guard<std::mutex> g(mu_);
        char line[8192];
        while (std::fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            // v1 rows have no timestamp; drop them. v2 is id file url size etag head savedAtMs.
            if (s.size() < 4 || s.compare(0, 3, "v2\t") != 0)
                continue;
            std::string parts[7];
            int n = 0;
            size_t pos = 3;
            while (n < 7) {
                const auto tab = s.find('\t', pos);
                if (tab == std::string::npos) {
                    parts[n++] = s.substr(pos);
                    break;
                }
                parts[n++] = s.substr(pos, tab - pos);
                pos = tab + 1;
            }
            if (n != 7)
                continue;
            const int64_t saved = std::strtoll(parts[6].c_str(), nullptr, 10);
            if (saved <= 0)
                continue;
            ArchiveCacheEntry probe;
            probe.savedAtMs = saved;
            if (locatorExpired(probe))
                continue;
            auto& e = m_[key(parts[0], parts[1])];
            e.url = parts[2];
            e.size = std::strtoll(parts[3].c_str(), nullptr, 10);
            e.etag = parts[4];
            e.savedAtMs = saved;
            const int h = std::atoi(parts[5].c_str());
            if (h == 1)
                e.head = ArchiveHead::Mp4;
            else if (h == 2)
                e.head = ArchiveHead::NotMp4;
            else
                e.head = ArchiveHead::Unknown;
        }
        std::fclose(f);
    }

private:
    static std::string key(const std::string& id, const std::string& file) { return id + '\n' + file; }
    static std::string unkeyId(const std::string& k) {
        const auto n = k.find('\n');
        return n == std::string::npos ? k : k.substr(0, n);
    }
    static std::string unkeyFile(const std::string& k) {
        const auto n = k.find('\n');
        return n == std::string::npos ? std::string() : k.substr(n + 1);
    }
    int64_t locatorNowMsLocked() const {
        return nowOverrideMs_ >= 0 ? nowOverrideMs_ : archiveCacheWallNowMs();
    }
    static bool fieldHasTab(const std::string& s) { return s.find('\t') != std::string::npos; }
    void persistLocked() const {
        if (persistPath_.empty())
            return;
        const std::string tmp = persistPath_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f)
            return;
        for (const auto& kv : m_) {
            const auto& e = kv.second;
            if (e.url.empty() || e.head == ArchiveHead::Unknown || e.savedAtMs <= 0)
                continue;
            const std::string id = unkeyId(kv.first);
            const std::string file = unkeyFile(kv.first);
            if (fieldHasTab(id) || fieldHasTab(file) || fieldHasTab(e.url) || fieldHasTab(e.etag))
                continue;
            const int h = e.head == ArchiveHead::Mp4 ? 1 : 2;
            if (std::fprintf(f, "v2\t%s\t%s\t%s\t%lld\t%s\t%d\t%lld\n", id.c_str(), file.c_str(),
                             e.url.c_str(), static_cast<long long>(e.size), e.etag.c_str(), h,
                             static_cast<long long>(e.savedAtMs)) < 0) {
                std::fclose(f);
                std::remove(tmp.c_str());
                return;
            }
        }
        if (std::fclose(f) != 0) {
            std::remove(tmp.c_str());
            return;
        }
        std::rename(tmp.c_str(), persistPath_.c_str());
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, ArchiveCacheEntry> m_;
    std::unordered_map<std::string, ArchiveItemMeta> items_;
    int64_t nowOverrideMs_ = -1;
    std::string persistPath_;
};

enum class ArchiveResolveKind { Ok, NotMp4, Failed, Cancelled, DeadTailMoov, DeadUnsupported };

inline bool archiveFrameTooBig(int w, int h) {
    return w > 0 && h > 0 &&
           ((int64_t(w) + 15) / 16) * ((int64_t(h) + 15) / 16) > kRemoteDirectMaxMbs;
}

struct ArchiveResolve {
    ArchiveResolveKind kind = ArchiveResolveKind::Failed;
    std::string url;
    // #686: Pluto live only -- the separate EXT-X-MEDIA audio rendition playlist when the chosen video rung has
    // no audio of its own (empty = audio is muxed in `url`). Carries the session query: never log it unredacted.
    std::string audioUrl;
    std::string err;
    // P2 header cache key material (from archive.org file metadata); -1 / empty when unknown.
    int64_t size = -1;
    std::string etag;
    Mp4HeadKind headKind = Mp4HeadKind::NotMp4;
};

// P3: a tail moov this large is not worth a live scan for channel tuning until the header
// cache (A) already has a warm entry for this exact (url, size, etag) — then the open reads
// the cached header and makes one range read at the keyframe, so the size no longer matters.
inline constexpr int64_t kTailMoovDeadBytes = 4ll * 1024 * 1024;

// size: the file's byte length when known (-1 otherwise). headerCacheWarm: true when the P2
// header cache already holds this item's header (see header_cache.hpp).
inline ArchiveResolve classifyPreflight(int status, const std::string& body, int64_t size = -1,
                                        bool headerCacheWarm = false) {
    ArchiveResolve o;
    if (status != 200 && status != 206) {
        o.kind = ArchiveResolveKind::Failed;
        o.err = "preflight HTTP " + std::to_string(status);
        return o;
    }
    const Mp4HeadKind k = classifyMp4Head(body);
    o.headKind = k;
    if (k == Mp4HeadKind::NotMp4) {
        o.kind = ArchiveResolveKind::NotMp4;
        o.err = "not mp4";
        return o;
    }
    if (k == Mp4HeadKind::TailMoov && !headerCacheWarm && size >= kTailMoovDeadBytes) {
        o.kind = ArchiveResolveKind::DeadTailMoov;
        o.err = "tail moov dead for live tuning";
        return o;
    }
    o.kind = ArchiveResolveKind::Ok;
    return o;
}

struct ArchiveHttpGet {
    bool transportOk = false;
    int status = 0;
    std::string body;
    std::string err;
    int64_t dnsMs = -1;
    int64_t connectMs = -1;
    int64_t tlsMs = -1;
    int64_t ttfbMs = -1;
    int64_t bodyBytes = 0;
};

// True when this (id, file) was already classified DeadTailMoov in this session
// and the header cache is still as cold as it was when that was decided. Used to
// skip the preflight GET on later zaps (gumshoe "Criss Cross" class).
inline bool cachedDeadTailMoovCold(
    const ArchiveCache& cache, const std::string& id, const std::string& file,
    const std::function<bool(const std::string& url, int64_t size, const std::string& etag)>& headerCacheWarm = {}) {
    ArchiveCacheEntry e;
    if (!cache.lookup(id, file, e) || !e.deadTailMoov)
        return false;
    if (cache.deadVerdictExpired(e))
        return false;
    const bool warm = static_cast<bool>(headerCacheWarm) && headerCacheWarm(e.url, e.size, e.etag);
    if (warm)
        return false;
    return !e.deadDecidedWarm;
}

// get(url, maxBody, range) — range true means Range: bytes=0-511. headerCacheWarm(url, size,
// etag), when supplied, reports whether the P2 header cache already holds this item's header;
// an empty callback is treated as "not warm" everywhere (the DeadTailMoov gate never opens on
// unknown warmth, matching the P3 "do not skip solely on the TailMoov mark" default-safe rule
// only when size is also unknown — a known large tail moov with no warm callback still skips).
template <typename GetFn>
ArchiveResolve resolveArchivePlay(
    ArchiveCache& cache, const std::string& id, const std::string& file, GetFn get,
    const std::function<bool()>& cancelled,
    const std::function<bool(const std::string& url, int64_t size, const std::string& etag)>& headerCacheWarm = {}) {
    auto dead = [&]() -> bool { return cancelled && cancelled(); };
    ArchiveResolve o;
    if (id.empty() || file.empty()) {
        o.err = "empty archive id";
        return o;
    }
    ArchiveCacheEntry e;
    if (cache.lookup(id, file, e)) {
        if ((e.head == ArchiveHead::Mp4 || e.head == ArchiveHead::NotMp4) && cache.locatorExpired(e)) {
            cache.dropLocator(id, file);
            e = ArchiveCacheEntry{};
        }
        if (e.head == ArchiveHead::Mp4 && !e.url.empty() && hostAllowed(e.url)) {
            o.kind = ArchiveResolveKind::Ok;
            o.url = e.url;
            o.size = e.size;
            o.etag = e.etag;
            return o;
        }
        if (e.head == ArchiveHead::NotMp4) {
            o.kind = ArchiveResolveKind::NotMp4;
            o.url = e.url;
            o.err = "not mp4";
            o.size = e.size;
            o.etag = e.etag;
            return o;
        }
        if (e.deadTailMoov) {
            const bool expired = cache.deadVerdictExpired(e);
            const bool warm = static_cast<bool>(headerCacheWarm) && headerCacheWarm(e.url, e.size, e.etag);
            if (!expired && !warm && !e.deadDecidedWarm) {
                o.kind = ArchiveResolveKind::DeadTailMoov;
                o.url = e.url;
                o.size = e.size;
                o.etag = e.etag;
                o.err = "tail moov dead for live tuning";
                o.headKind = Mp4HeadKind::TailMoov;
                return o;
            }
            cache.clearDeadTailMoov(id, file);
            e.deadTailMoov = false;
        }
    }
    if (dead()) {
        o.kind = ArchiveResolveKind::Cancelled;
        o.err = "cancelled";
        return o;
    }
    ArchiveFileMeta row;
    std::string itemUrl;
    const bool itemCached = cache.lookupFileMeta(id, file, itemUrl, row);
    if (e.url.empty()) {
        if (!itemCached) {
            const std::string metaUrl = archiveMetadataUrl(id);
            if (metaUrl.empty()) {
                o.err = "bad archive id";
                return o;
            }
            const ArchiveHttpGet meta = get(metaUrl, size_t(2u << 20), false);
            if (dead()) {
                o.kind = ArchiveResolveKind::Cancelled;
                o.err = "cancelled";
                return o;
            }
            if (!meta.transportOk || meta.status != 200) {
                if (archiveLocatorGoneStatus(meta.status))
                    cache.dropLocator(id, file);
                o.err = meta.err.empty() ? ("metadata HTTP " + std::to_string(meta.status)) : meta.err;
                return o;
            }
            ArchiveItemMeta item;
            if (!parseArchiveItemMeta(meta.body, item)) {
                o.err = "no archive locator";
                return o;
            }
            itemUrl = archiveItemUrl(item.d1, item.dir, file);
            if (itemUrl.empty()) {
                o.err = "no archive locator";
                return o;
            }
            const auto it = item.files.find(file);
            if (it != item.files.end())
                row = it->second;
            cache.storeItemMeta(id, std::move(item));
        }
        if (itemUrl.empty()) {
            o.err = "no archive locator";
            return o;
        }
        if (locatorSizeEtagMismatch(e.size, e.etag, row.size, row.etag))
            cache.dropLocator(id, file);
        e.url = itemUrl;
        e.size = row.size;
        e.etag = row.etag;
        cache.storeUrl(id, file, e.url);
        cache.storeSizeEtag(id, file, e.size, e.etag);
    }
    if (dead()) {
        o.kind = ArchiveResolveKind::Cancelled;
        o.err = "cancelled";
        return o;
    }
    if (archiveFrameTooBig(row.width, row.height)) {
        o.kind = ArchiveResolveKind::DeadUnsupported;
        o.url = e.url;
        o.size = e.size;
        o.etag = e.etag;
        o.err = "unsupported resolution " + std::to_string(row.width) + "x" +
                std::to_string(row.height) + " (metadata)";
        return o;
    }
    const ArchiveHttpGet pf = get(e.url, size_t(1024), true);
    if (dead()) {
        o.kind = ArchiveResolveKind::Cancelled;
        o.err = "cancelled";
        return o;
    }
    // browse-lane #647 P2-2: hasEntry must stay off the common path; only a large tail-moov
    // can become DeadTailMoov, so only then consult the header cache.
    bool warm = false;
    if (pf.transportOk && (pf.status == 200 || pf.status == 206) && e.size >= kTailMoovDeadBytes &&
        classifyMp4Head(pf.body) == Mp4HeadKind::TailMoov) {
        warm = static_cast<bool>(headerCacheWarm) && headerCacheWarm(e.url, e.size, e.etag);
    }
    ArchiveResolve c = classifyPreflight(pf.transportOk ? pf.status : 0, pf.body, e.size, warm);
    c.url = e.url;
    c.size = e.size;
    c.etag = e.etag;
    if (c.kind == ArchiveResolveKind::Failed && c.err.empty())
        c.err = pf.err.empty() ? "preflight failed" : pf.err;
    if (archiveLocatorGoneStatus(pf.transportOk ? pf.status : 0))
        cache.dropLocator(id, file);
    if (c.kind == ArchiveResolveKind::Ok)
        cache.storeHead(id, file, ArchiveHead::Mp4);
    else if (c.kind == ArchiveResolveKind::NotMp4)
        cache.storeHead(id, file, ArchiveHead::NotMp4);
    else if (c.kind == ArchiveResolveKind::DeadTailMoov)
        cache.storeDeadTailMoov(id, file, warm);
    return c;
}


namespace pluto_live {
template <typename GetFn>
ArchiveResolve resolvePlutoChannelPlay(const std::string& channelId, GetFn get,
                                       const std::function<bool()>& cancelled, int64_t nowSec) {
    ArchiveResolve o;
    if (!misterplex::plutoLiveEnabled()) {
        o.kind = ArchiveResolveKind::DeadUnsupported;
        o.err = "UNSUPPORTED: Pluto live off";
        return o;
    }
    if (channelId.empty()) {
        o.kind = ArchiveResolveKind::Failed;
        o.err = "empty pluto channel id";
        return o;
    }
    PlutoBootSession boot;
    if (!plutoBootCacheGet(channelId, nowSec, boot))
        boot = PlutoBootSession();
    auto plutoFetch = [&](const std::string& url, size_t maxBody, const std::string& bearer) -> ArchiveHttpGet {
        ArchiveHttpGet deny;
        deny.transportOk = false;
        deny.err = "pluto fetch host denied";
        if (!plutoFetchUrlAllowed(url))
            return deny;
        std::string& slot = plutoAuthHeaderForThread();
        const std::string prev = slot;
        if (plutoAttachSessionBearer(url, bearer))
            slot = "Bearer " + bearer;
        ArchiveHttpGet r = get(url, maxBody, false);
        slot = prev;
        return r;
    };
    if (boot.stitcher.empty()) {
        const std::string dev = randomUuidV4();
        const std::string bootUrl = plutoBootStartUrl(dev);
        if (cancelled && cancelled()) {
            o.kind = ArchiveResolveKind::Cancelled;
            o.err = "cancelled";
            return o;
        }
        const ArchiveHttpGet br = plutoFetch(bootUrl, kPlutoMaxBootBody, "");
        if (!br.transportOk || br.status != 200) {
            o.kind = ArchiveResolveKind::Failed;
            o.err = br.err.empty() ? "pluto boot failed" : br.err;
            return o;
        }
        if (!parsePlutoBoot(br.body, boot)) {
            o.kind = ArchiveResolveKind::Failed;
            o.err = "pluto boot parse";
            return o;
        }
        plutoBootCachePut(channelId, boot, nowSec);
    }
    const std::string master = plutoStitchMasterUrl(boot.stitcher, channelId, boot.stitcherParams, boot.sessionToken);
    if (cancelled && cancelled()) {
        o.kind = ArchiveResolveKind::Cancelled;
        o.err = "cancelled";
        return o;
    }
    const ArchiveHttpGet mr = plutoFetch(master, kPlutoMaxPlaylistBody, boot.sessionToken);
    if (!mr.transportOk || mr.status != 200) {
        o.kind = ArchiveResolveKind::Failed;
        o.err = mr.err.empty() ? "pluto master failed" : mr.err;
        return o;
    }
    const auto vars = parsePlutoMaster(mr.body, master);
    std::string capWhy;
    const PlutoVariant* pick = nullptr;
    const auto masterQs =
        master.find('?') != std::string::npos ? master.substr(master.find('?') + 1) : std::string();
    auto mediaUrlForVariant = [&](const PlutoVariant& v) { return plutoMediaUrlForVariant(v.uri, masterQs); };
    PlutoProbedMedia probedMedia;
    // Tag path and probe path share this. The probe path already rejects an
    // over-cap segment. The tag path then confirmPlutoVariantUnderCap, because
    // a 720 tag or a missing RESOLUTION can still be 1920x1080 in the segments.
    auto probeVariant = [&](const PlutoVariant& v) -> PlutoStreamProbe {
        if (cancelled && cancelled())
            return PlutoStreamProbe();
        const std::string mediaUrl = mediaUrlForVariant(v);
        const auto pr = plutoProbeVariantForCaps(
            v, mediaUrl, boot.sessionToken, cancelled,
            [&]() -> PlutoMediaFetchResult {
                PlutoMediaFetchResult m;
                const ArchiveHttpGet pl = plutoFetch(mediaUrl, kPlutoMaxPlaylistBody, boot.sessionToken);
                if (!pl.transportOk || pl.status != 200)
                    return m;
                m.playlistOk = true;
                m.playlistBody = pl.body;
                if (!plutoPlaylistBlockReason(pl.body).empty()) {
                    m.blocked = true;
                    return m;
                }
                // Last successful probe playlist. Reused when the URL matches
                // the pick. A later miss does not keep a different URL.
                probedMedia.mediaUrl = mediaUrl;
                probedMedia.body = pl.body;
                return m;
            },
            [&](const PlutoMediaFetchResult& m) -> std::string {
                const std::string seg = plutoFirstSegmentUrl(m.playlistBody, mediaUrl);
                if (seg.empty())
                    return {};
                const ArchiveHttpGet sr = plutoFetch(seg, kPlutoMaxProbeBytes, boot.sessionToken);
                if (sr.transportOk && sr.status == 200)
                    return sr.body;
                return {};
            });
        if (!pr.note.empty())
            std::fprintf(stderr, "misterplexd: spelunkr: pluto: %s band=%d\n", pr.note.c_str(), v.bandwidth);
        return pr.probe;
    };
    if (plutoMasterNeedsSegmentProbe(vars))
        pick = selectPlutoVariantViaProbe(vars, capWhy, probeVariant);
    else
        pick = confirmPlutoVariantUnderCap(vars, selectPlutoVariant(vars, capWhy), capWhy, probeVariant);
    if (!pick) {
        o.kind = ArchiveResolveKind::DeadUnsupported;
        o.err = capWhy.empty() ? "UNSUPPORTED: Pluto no variant" : capWhy;
        return o;
    }
    std::string mediaUrl = mediaUrlForVariant(*pick);
    std::string mediaBody;
    if (plutoReuseProbedMedia(probedMedia, mediaUrl)) {
        mediaBody = probedMedia.body;
    } else {
        const ArchiveHttpGet pr = plutoFetch(mediaUrl, kPlutoMaxPlaylistBody, boot.sessionToken);
        if (!pr.transportOk || pr.status != 200) {
            o.kind = ArchiveResolveKind::Failed;
            o.err = pr.err.empty() ? "pluto media playlist failed" : pr.err;
            return o;
        }
        mediaBody = pr.body;
    }
    // DRM / host-scope still run on a reused probe body. Do not skip this.
    const std::string block = plutoPlaylistBlockReason(mediaBody);
    if (!block.empty()) {
        o.kind = ArchiveResolveKind::DeadUnsupported;
        o.err = block;
        return o;
    }
    // #686: the video rung of a demuxed master has no audio; remux needs the master's EXT-X-MEDIA audio rendition too.
    std::string audioUrl;
    std::string audioBody;
    const std::vector<PlutoAudioRendition> audioRends = parsePlutoAudioRenditions(mr.body, master);
    if (const PlutoAudioRendition* ar = selectPlutoAudioRendition(audioRends, pick->audioGroup)) {
        audioUrl = plutoMediaUrlForVariant(ar->uri, masterQs);
        if (!plutoPlayUrlAllowed(audioUrl)) {
            o.kind = ArchiveResolveKind::DeadUnsupported;
            o.err = "UNSUPPORTED: Pluto audio host";
            return o;
        }
        const ArchiveHttpGet ap = plutoFetch(audioUrl, kPlutoMaxPlaylistBody, boot.sessionToken);
        if (!ap.transportOk || ap.status != 200) {
            o.kind = ArchiveResolveKind::Failed;
            o.err = ap.err.empty() ? "pluto audio playlist failed" : ap.err;
            return o;
        }
        const std::string ablock = plutoPlaylistBlockReason(ap.body);
        if (!ablock.empty()) {
            o.kind = ArchiveResolveKind::DeadUnsupported;
            o.err = ablock;
            return o;
        }
        audioBody = ap.body;
        std::fprintf(stderr, "misterplexd: spelunkr: pluto: separate audio rendition group=%s\n",
                     pick->audioGroup.c_str());
    }
    if (plutoSessionBearerAllowedForMedia(mediaUrl, mediaBody) &&
        (audioUrl.empty() || plutoSessionBearerAllowedForMedia(audioUrl, audioBody)))
        plutoRemuxSessionToken() = boot.sessionToken;
    else
        plutoRemuxSessionToken().clear();
    o.kind = ArchiveResolveKind::Ok;
    o.url = mediaUrl;
    o.audioUrl = audioUrl;
    return o;
}
}  // namespace pluto_live

// Play path for a scheduled item: Archive id/file when present, otherwise a
// host-allowed first-party HTTPS URL (e.g. backwhen.stream Newswire MP4s).
// Empty id/file with no allowed URL still reports "empty archive id".
template <typename GetFn>
ArchiveResolve resolveScheduledPlay(
    ArchiveCache& cache, const std::string& id, const std::string& file, const std::string& directUrl,
    GetFn get, const std::function<bool()>& cancelled,
    const std::function<bool(const std::string& url, int64_t size, const std::string& etag)>& headerCacheWarm = {}) {
    if (id == pluto_live::kPlutoItemId && !file.empty()) {
        const int64_t now =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        return pluto_live::resolvePlutoChannelPlay(file, get, cancelled, now);
    }
    if (!id.empty() && !file.empty())
        return resolveArchivePlay(cache, id, file, get, cancelled, headerCacheWarm);
    ArchiveResolve o;
    if (hostAllowed(directUrl)) {
        o.kind = ArchiveResolveKind::Ok;
        o.url = directUrl;
        return o;
    }
    o.err = "empty archive id";
    return o;
}

// P2 header cache keys need headerCacheSize >= 0 on the libav open (owenb_av_source.cpp).
// resolveArchivePlay always fills size/etag from archive.org metadata; resolveScheduledPlay
// for a host-allowed direct URL (Back When first-party MP4s) leaves size at -1, which skipped
// the SD cache on channel change even though archive Spelunkr items were cached. Use size==0
// with an empty etag as the stable "unknown byte length" cache key only (HeaderCacheAvio must
// not treat 0 as the real AVIO size; libav probes Content-Length live). The store still keys
// only on the URL hash; empty etag on load does not reject a revised body at the same URL —
// Back When slots are expected to change URL when bytes change; a stable slot URL can serve a
// stale moov until a failed open evicts the entry or the URL changes (#656 N1).
inline void applyDirectPlayHeaderCacheKey(int64_t& size, const std::string& archiveId,
                                          const std::string& file, const std::string& url) {
    if (size >= 0 || !archiveId.empty() || !file.empty() || !hostAllowed(url))
        return;
    // Use size==0 with an empty etag as the stable direct-play key (see comment above).
    size = 0;
}

}  // namespace spelunkr
}  // namespace misterplex
