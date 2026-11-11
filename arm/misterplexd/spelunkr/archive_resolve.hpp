#pragma once
// Archive.org item URL cache + preflight classify. Network stays in the caller
// (tlsVerifiedGet). A cache hit of a known-MP4 locator skips both GETs.

#include "spelunkr/broadcast.hpp"
#include "libmisterplex/remote_h264_transcode.hpp"

#include <functional>
#include <mutex>
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
};

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
        m_[key(id, file)].url = std::move(url);
    }
    void storeHead(const std::string& id, const std::string& file, ArchiveHead h) {
        std::lock_guard<std::mutex> g(mu_);
        m_[key(id, file)].head = h;
    }
    void storeSizeEtag(const std::string& id, const std::string& file, int64_t size, std::string etag) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = m_[key(id, file)];
        e.size = size;
        e.etag = std::move(etag);
    }
    size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return m_.size();
    }

private:
    static std::string key(const std::string& id, const std::string& file) { return id + '\n' + file; }
    mutable std::mutex mu_;
    std::unordered_map<std::string, ArchiveCacheEntry> m_;
    std::unordered_map<std::string, ArchiveItemMeta> items_;
};

enum class ArchiveResolveKind { Ok, NotMp4, Failed, Cancelled, DeadTailMoov, DeadUnsupported };

inline bool archiveFrameTooBig(int w, int h) {
    return w > 0 && h > 0 &&
           ((int64_t(w) + 15) / 16) * ((int64_t(h) + 15) / 16) > kRemoteDirectMaxMbs;
}

struct ArchiveResolve {
    ArchiveResolveKind kind = ArchiveResolveKind::Failed;
    std::string url;
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
};

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
    const bool warm = static_cast<bool>(headerCacheWarm) && headerCacheWarm(e.url, e.size, e.etag);
    ArchiveResolve c = classifyPreflight(pf.transportOk ? pf.status : 0, pf.body, e.size, warm);
    c.url = e.url;
    c.size = e.size;
    c.etag = e.etag;
    if (c.kind == ArchiveResolveKind::Failed && c.err.empty())
        c.err = pf.err.empty() ? "preflight failed" : pf.err;
    if (c.kind == ArchiveResolveKind::Ok)
        cache.storeHead(id, file, ArchiveHead::Mp4);
    else if (c.kind == ArchiveResolveKind::NotMp4)
        cache.storeHead(id, file, ArchiveHead::NotMp4);
    // DeadTailMoov is not cached as a head verdict: it depends on the header cache's warmth,
    // which can change between retunes (that is the whole point of the P2/P3 pairing).
    return c;
}

}  // namespace spelunkr
}  // namespace misterplex
