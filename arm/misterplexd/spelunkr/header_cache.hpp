#pragma once
// P2 header (moov) cache. Keyed by (url, size, etag) and persisted on the MiSTer SD so a
// re-tune of the same archive.org item does not re-fetch its header over the network. Pure
// disk + bookkeeping here; header_cache_avio.hpp (libav, MPX_HAVE_LIBAV) is what actually
// serves the cached bytes back through a custom AVIOContext.
//
// Cache key: url is the archive.org item URL; size is the file's byte length and etag is the
// archive.org "md5" file-metadata field (used as an ETag-equivalent content fingerprint —
// archive.org's direct item URL does not send a real ETag). A missing etag still keys on
// (url, size); a size or etag mismatch against a stored entry is treated as a miss, not an
// error, and the caller re-fetches.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace misterplex {
namespace spelunkr {

// MPX_SPELUNKR_HEADER_CACHE_DIR overrides the directory (host tests use a temp dir); the
// default is the path requested for the MiSTer SD.
inline std::string headerCacheDir() {
    if (const char* e = std::getenv("MPX_SPELUNKR_HEADER_CACHE_DIR"); e && *e)
        return e;
    return "/media/fat/linux/misterplex-cache/spelunkr-headers/";
}

struct HeaderCacheKey {
    std::string url;
    int64_t size = -1;   // -1: unknown. Still part of the key; see note above.
    std::string etag;    // empty: key on (url, size) only.
};

struct HeaderRange {
    int64_t offset = 0;
    std::string data;
};

struct HeaderCacheData {
    HeaderCacheKey key;
    int64_t moovOffset = -1;  // logging only; -1 when not found during the scan.
    int64_t moovSize = -1;
    std::vector<HeaderRange> ranges;  // sparse, offset-ascending recorded header bytes.
};

namespace detail {

inline uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// A process-wide monotonic counter so two HeaderCacheStore instances (e.g. one per
// OwenbAvSource, per owenb_av_source.cpp) never collide on the same tmp file name even when
// storing the same URL at the same pid-clock-tick.
inline uint64_t nextTmpTag() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

inline bool mkdirsFor(const std::string& dir) {
    if (dir.empty())
        return false;
    std::string path;
    path.reserve(dir.size());
    for (size_t i = 0; i < dir.size(); ++i) {
        path += dir[i];
        if (dir[i] == '/' && path.size() > 1) {
            if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

inline void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out += char((v >> (i * 8)) & 0xFF);
}
inline void putU64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out += char((v >> (i * 8)) & 0xFF);
}
inline void putStr(std::string& out, const std::string& s) {
    putU32(out, static_cast<uint32_t>(s.size()));
    out += s;
}

struct Reader {
    const std::string& buf;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        // GBE #623 P2-1: pos <= buf.size() is invariant; `pos + n` wraps on 32-bit ARM for a
        // corrupt u32 length near 2^32 and walks pos backwards (unbounded loop + copies).
        if (n > buf.size() - pos) {
            ok = false;
            return false;
        }
        return true;
    }
    uint32_t getU32() {
        if (!need(4))
            return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= uint32_t(static_cast<unsigned char>(buf[pos + i])) << (i * 8);
        pos += 4;
        return v;
    }
    uint64_t getU64() {
        if (!need(8))
            return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= uint64_t(static_cast<unsigned char>(buf[pos + i])) << (i * 8);
        pos += 8;
        return v;
    }
    std::string getStr() {
        const uint32_t n = getU32();
        if (!ok || !need(n))
            return {};
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }
};

}  // namespace detail

// kMaxCacheBytes bounds a single cache file: this stores header material (moov/ftyp/free,
// or whatever ranges the AVIO actually touched before find_stream_info), never the whole
// video. A pathological recording past this cap is simply not persisted.
inline constexpr int64_t kMaxHeaderCacheBytes = 8ll * 1024 * 1024;
// P2: nothing else bounds the directory (one *.hdr file per URL ever visited), so surfing
// would otherwise grow the SD without bound. store() evicts the oldest *.hdr files by mtime
// after a successful write until the directory is at or under this cap. The entry just
// written is never evicted to make room for itself.
// GBE #623 P3-1: load() reads at most this much; store() never writes more than kMaxHeaderCacheBytes of
// range data plus per-range (12 B) and key overhead, so anything larger is corrupt (FAT cross-link).
inline constexpr int64_t kMaxHeaderCacheFileBytes = kMaxHeaderCacheBytes + 1024 * 1024;
inline constexpr int64_t kMaxHeaderCacheDirBytes = 64ll * 1024 * 1024;
inline constexpr char kHeaderCacheMagic[8] = {'S', 'P', 'H', 'C', '0', '0', '1', '\0'};

class HeaderCacheStore {
public:
    explicit HeaderCacheStore(std::string dir = headerCacheDir(), int64_t maxDirBytes = kMaxHeaderCacheDirBytes)
        : dir_(std::move(dir)), maxDirBytes_(maxDirBytes) {}

    std::string filePath(const HeaderCacheKey& key) const {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(detail::fnv1a64(key.url)));
        std::string d = dir_;
        if (!d.empty() && d.back() != '/')
            d += '/';
        return d + hex + ".hdr";
    }

    // False: missing file, corrupt file, or a (size, etag) mismatch against this key — a cache
    // miss in every case, never thrown as an error.
    bool load(const HeaderCacheKey& key, HeaderCacheData& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string path = filePath(key);
        // GBE #623 P3-2: a structurally corrupt entry is a miss AND is deleted (mu_ is held: no remove()).
        const auto corrupt = [&path] {
            std::remove(path.c_str());
            return false;
        };
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        std::string buf;
        char chunk[4096];
        size_t n;
        bool oversize = false;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
            if (buf.size() + n > static_cast<size_t>(kMaxHeaderCacheFileBytes)) {
                oversize = true;   // GBE #623 P3-1: never slurp an unbounded file
                break;
            }
            buf.append(chunk, n);
        }
        std::fclose(f);
        if (oversize)
            return corrupt();
        if (buf.size() < sizeof(kHeaderCacheMagic) ||
            std::memcmp(buf.data(), kHeaderCacheMagic, sizeof(kHeaderCacheMagic)) != 0)
            return corrupt();
        detail::Reader r{buf, sizeof(kHeaderCacheMagic)};
        HeaderCacheData d;
        d.key.url = r.getStr();
        d.key.size = static_cast<int64_t>(r.getU64());
        d.key.etag = r.getStr();
        d.moovOffset = static_cast<int64_t>(r.getU64());
        d.moovSize = static_cast<int64_t>(r.getU64());
        const uint32_t rangeCount = r.getU32();
        if (!r.ok)
            return corrupt();
        // GBE #619 P1-1: rangeCount is untrusted (a corrupt .hdr on FAT after power loss). Bound the
        // reserve by what the file can actually hold (a range is at least 12 bytes on disk), or a
        // garbage count throws bad_alloc / length_error (32-bit ARM) out of the play/prefetch thread.
        const size_t rest = buf.size() - r.pos;
        d.ranges.reserve(std::min<size_t>(rangeCount, rest / 12));
        for (uint32_t i = 0; i < rangeCount && r.ok; ++i) {
            HeaderRange hr;
            hr.offset = static_cast<int64_t>(r.getU64());
            hr.data = r.getStr();
            d.ranges.push_back(std::move(hr));
        }
        if (!r.ok)
            return corrupt();
        if (d.key.url != key.url)
            return false;
        if (key.size >= 0 && d.key.size >= 0 && d.key.size != key.size)
            return false;  // size mismatch: stale (the source changed)
        if (!key.etag.empty() && !d.key.etag.empty() && d.key.etag != key.etag)
            return false;  // etag mismatch: stale
        out = std::move(d);
        return true;
    }

    bool hasEntry(const HeaderCacheKey& key) const {
        HeaderCacheData d;
        return load(key, d);
    }

    // Deletes this key's cache file, if any. Used when a hit's open/find_stream_info/seek
    // fails (P2-2: a stale or poisoned entry should not keep failing every tune).
    bool remove(const HeaderCacheKey& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return std::remove(filePath(key).c_str()) == 0;
    }

    // Atomic write (tmp file + rename). Creates the cache directory as needed. False on any
    // I/O failure, or when the recorded ranges exceed kMaxHeaderCacheBytes (the caller should
    // log and move on; a miss next time is correct, not fatal). On success, evicts the oldest
    // *.hdr files by mtime until the directory is at or under maxDirBytes_ (the entry just
    // written is kept; others are evicted first).
    bool store(const HeaderCacheData& data) const {
        int64_t total = 0;
        for (const auto& r : data.ranges)
            total += static_cast<int64_t>(r.data.size());
        if (total > kMaxHeaderCacheBytes)
            return false;
        std::lock_guard<std::mutex> lock(mu_);
        if (!detail::mkdirsFor(dir_))
            return false;
        std::string buf(kHeaderCacheMagic, sizeof(kHeaderCacheMagic));
        detail::putStr(buf, data.key.url);
        detail::putU64(buf, static_cast<uint64_t>(data.key.size));
        detail::putStr(buf, data.key.etag);
        detail::putU64(buf, static_cast<uint64_t>(data.moovOffset));
        detail::putU64(buf, static_cast<uint64_t>(data.moovSize));
        detail::putU32(buf, static_cast<uint32_t>(data.ranges.size()));
        for (const auto& r : data.ranges) {
            detail::putU64(buf, static_cast<uint64_t>(r.offset));
            detail::putStr(buf, r.data);
        }
        if (buf.size() > static_cast<size_t>(kMaxHeaderCacheFileBytes))
            return false;
        const std::string finalPath = filePath(data.key);
        // P3-3: pid alone collided when two HeaderCacheStore instances in the same process
        // (e.g. prefetch plus another open) stored the same URL concurrently. pid + a
        // process-wide atomic counter is unique across threads and instances.
        const std::string tmpPath = finalPath + ".tmp." + std::to_string(::getpid()) + "." +
                                    std::to_string(detail::nextTmpTag());
        FILE* f = std::fopen(tmpPath.c_str(), "wb");
        if (!f)
            return false;
        const size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
        const bool flushOk = std::fclose(f) == 0;
        if (written != buf.size() || !flushOk) {
            std::remove(tmpPath.c_str());
            return false;
        }
        if (std::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
            std::remove(tmpPath.c_str());
            return false;
        }
        evictLocked(finalPath);
        return true;
    }

private:
    // Caller holds mu_. Scans dir_ once: sums *.hdr bytes (eviction candidates, newest mtime
    // kept) and opportunistically removes a stale .tmp.* left by a crashed/interrupted store
    // (older than five minutes -- far longer than any real write takes).
    void evictLocked(const std::string& justWrittenPath) const {
        DIR* d = ::opendir(dir_.c_str());
        if (!d)
            return;
        struct Entry {
            std::string path;
            int64_t size;
            time_t mtime;
        };
        std::vector<Entry> hdrEntries;
        int64_t total = 0;
        const time_t now = std::time(nullptr);
        struct dirent* de;
        while ((de = ::readdir(d)) != nullptr) {
            const std::string name = de->d_name;
            if (name == "." || name == "..")
                continue;
            std::string full = dir_;
            if (!full.empty() && full.back() != '/')
                full += '/';
            full += name;
            struct stat st {};
            if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            const bool isHdr = name.size() >= 4 && name.compare(name.size() - 4, 4, ".hdr") == 0;
            if (isHdr) {
                hdrEntries.push_back({full, static_cast<int64_t>(st.st_size), st.st_mtime});
                total += static_cast<int64_t>(st.st_size);
            } else if (name.find(".tmp.") != std::string::npos && now - st.st_mtime > 300) {
                std::remove(full.c_str());
            }
        }
        ::closedir(d);
        if (maxDirBytes_ <= 0 || total <= maxDirBytes_)
            return;
        std::sort(hdrEntries.begin(), hdrEntries.end(),
                 [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
        for (const auto& e : hdrEntries) {
            if (total <= maxDirBytes_)
                break;
            if (e.path == justWrittenPath)
                continue;  // never evict the entry just written to stay under cap
            if (std::remove(e.path.c_str()) == 0)
                total -= e.size;
        }
    }

    std::string dir_;
    int64_t maxDirBytes_ = kMaxHeaderCacheDirBytes;
    mutable std::mutex mu_;
};

}  // namespace spelunkr
}  // namespace misterplex
