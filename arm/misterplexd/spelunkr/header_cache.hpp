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
#include <chrono>
#include <memory>
#include <limits>
#include <condition_variable>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utime.h>
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

struct PartialFileSnapshot {
    HeaderCacheKey key;
    std::shared_ptr<FILE> file;
};

// Retain the opened layout, not the pathname that a subsequent spill may replace.
struct PartialRangeRef {
    int64_t offset = 0;
    int64_t dataPos = 0;  // byte offset in the `.hdr.partial` file where range data begins
    int32_t size = 0;
    std::shared_ptr<const PartialFileSnapshot> snapshot;
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

inline bool fwriteAll(FILE* f, const void* p, size_t n) {
    return n == 0 || std::fwrite(p, 1, n, f) == n;
}
inline bool writeU32(FILE* f, uint32_t v) {
    unsigned char b[4];
    for (int i = 0; i < 4; ++i)
        b[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
    return fwriteAll(f, b, 4);
}
inline bool writeU64(FILE* f, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
    return fwriteAll(f, b, 8);
}
inline bool writeStr(FILE* f, const std::string& s) {
    if (!writeU32(f, static_cast<uint32_t>(s.size())))
        return false;
    return fwriteAll(f, s.data(), s.size());
}
inline bool freadAll(FILE* f, void* p, size_t n) {
    return n == 0 || std::fread(p, 1, n, f) == n;
}
inline bool readU32(FILE* f, uint32_t& v) {
    unsigned char b[4];
    if (!freadAll(f, b, 4))
        return false;
    v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(b[i]) << (i * 8);
    return true;
}
inline bool readU64(FILE* f, uint64_t& v) {
    unsigned char b[8];
    if (!freadAll(f, b, 8))
        return false;
    v = 0;
    for (int i = 0; i < 8; ++i)
        v |= uint64_t(b[i]) << (i * 8);
    return true;
}
inline bool readStr(FILE* f, std::string& s, size_t maxLen) {
    uint32_t n = 0;
    if (!readU32(f, n) || static_cast<size_t>(n) > maxLen)
        return false;
    s.assign(n, '\0');
    return freadAll(f, s.data(), n);
}

inline std::atomic<int>& headerCachePendingStores() {
    static std::atomic<int> n{0};
    return n;
}
inline void waitHeaderCacheIdle(int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (headerCachePendingStores().load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct Reader {
    const std::string& buf;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        // browse-lane #623 P2-1: pos <= buf.size() is invariant; `pos + n` wraps on 32-bit ARM for a
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
// browse-lane #647 P2-8: 8 MiB per entry. A 32 MiB cap allowed up to 2R ~= 64 MiB of header-cache
// heap on a cold tune, which does not fit the 24 MB RAM gate. Typical 5.8-6.9 MB moovs
// still fit; a 31.6 MB moov is not cached (partial entries are never full hits).
// header_cache_avio kMaxRecordBytes is this same symbol.
inline constexpr int64_t kMaxHeaderCacheBytes = 12ll * 1024 * 1024;
// P3-2: drop a new storeAsync when this many workers are already in flight so an
// SD stall cannot stack 12 MiB recordings toward the 24 MB MemAvailable floor.
inline constexpr int kMaxPendingHeaderStores = 2;
inline constexpr int kMaxPendingNeighborStores = 1;
// P2: nothing else bounds the directory (one *.hdr file per URL ever visited), so surfing
// would otherwise grow the SD without bound. store() evicts the oldest *.hdr files by mtime
// after a successful write until the directory is at or under this cap. The entry just
// written is never evicted to make room for itself.
// browse-lane #623 P3-1: load() reads at most this much; store() never writes more than kMaxHeaderCacheBytes of
// range data plus per-range (12 B) and key overhead, so anything larger is corrupt (FAT cross-link).
inline constexpr int64_t kMaxHeaderCacheFileBytes = kMaxHeaderCacheBytes + 1024 * 1024;
// 35 channels x 4-6 MB is about 150-200 MB; 512 MB leaves room for many 8 MiB entries.
inline constexpr int64_t kMaxHeaderCacheDirBytes = 512ll * 1024 * 1024;
// browse-lane #647 P2-4: skip a write when the filesystem has less than 1 GiB free.
inline constexpr int64_t kMinHeaderCacheFreeBytes = 1024ll * 1024 * 1024;
// browse-lane #647 P3-8: reset() / ~HeaderCacheAvio must not block forever on an SD stall.
inline constexpr int64_t kHeaderCacheIdleTimeoutMs = 5000;
// SPHC002: load() rejects older magic so d5aabe0a 8 MiB partials cannot count warm (P3-5).
inline constexpr char kHeaderCacheMagic[8] = {'S', 'P', 'H', 'C', '0', '0', '2', '\0'};

// browse-lane P2-A (dial 500 "suplex"): a prefetch timeout/zap-window cancel mid-open must not drop
// every byte already read of a large moov -- the next attempt would start cold and the same
// 8 s window can never finish a ~35 MB header. storePartial()/loadPartial() persist that
// in-flight recording to a separate `.hdr.partial` path (never `.hdr`), so it can never satisfy
// hasEntry()/load() and count as a fully-warm complete entry, but the next open's attach() can
// resume from it (fetch only the remainder). Only the on-path (play) prefetch uses this path;
// neighbour prefetch stays on kMaxHeaderCacheBytes / over_cache_cap (see header_cache_avio.hpp
// neighborStore_). 48 MiB covers a 35,816,823-byte moov on disk with headroom.
inline constexpr int64_t kMaxPartialHeaderBytes = 48ll * 1024 * 1024;
inline constexpr int64_t kMaxPartialHeaderFileBytes = kMaxPartialHeaderBytes + 1024 * 1024;
// Normal 32 KiB ranges need under 64 KiB at the disk cap; allow bounded fragmentation.
inline constexpr size_t kMaxPartialIndexBytes = 1024 * 1024;
// browse-lane #663: on-path in-flight *RAM* for partial recording stays at the complete-header cap;
// overflow spills to `.hdr.partial` on SD (total partial may still reach kMaxPartialHeaderBytes).
inline constexpr int64_t kMaxPartialRecordRamBytes = kMaxHeaderCacheBytes;

// browse-lane #663 follow-up: dev-only glass fixture hook, off by default. lab glass often sees a
// ~10.4 MB header -- under kMaxPartialRecordRamBytes (12 MiB) -- so the over-cap spill/partial
// path (P3-1/P3-2) never exercises without a real 35.8 MB title. MPX_SPELUNKR_FORCE_PARTIAL_HEADER=1
// lowers the *effective* in-RAM recorder cap and the shouldPersistRecordedRanges complete-entry
// cap to a small value (256 KiB by default; MPX_SPELUNKR_PARTIAL_RAM_CAP_BYTES=<positive bytes>
// overrides it, clamped to (0, kMaxHeaderCacheBytes]) so a modest recording spills past it. Never
// lowers kMaxPartialHeaderBytes (the 48 MiB disk budget stays put). Process env only, read once
// (lazy-initialized) and logged once when active; unset/empty/0 leaves production behavior
// (kMaxHeaderCacheBytes) unchanged.
inline int64_t effectivePartialRamCapBytes() {
    static const int64_t cap = [] {
        const char* force = std::getenv("MPX_SPELUNKR_FORCE_PARTIAL_HEADER");
        if (!force || !*force || (force[0] == '0' && force[1] == '\0'))
            return kMaxHeaderCacheBytes;
        int64_t v = 256ll * 1024;
        if (const char* ov = std::getenv("MPX_SPELUNKR_PARTIAL_RAM_CAP_BYTES"); ov && *ov) {
            char* end = nullptr;
            const long long parsed = std::strtoll(ov, &end, 10);
            if (end && *end == '\0' && parsed > 0)
                v = parsed;
        }
        v = std::min<int64_t>(std::max<int64_t>(v, 1), kMaxHeaderCacheBytes);
        std::fprintf(stderr,
                     "misterplexd: spelunkr: MPX_SPELUNKR_FORCE_PARTIAL_HEADER active, partial "
                     "RAM/complete cap=%lld bytes\n",
                     static_cast<long long>(v));
        return v;
    }();
    return cap;
}

// browse-lane #663 follow-up: how many header bytes this in-flight attempt has recorded/served so far
// (live network reads and bytes served from an already-resumed partial both count -- see
// HeaderCacheAvio::doRead()). The play loop's cancelled() uses this as "has this open made
// progress" so an expiring zap window can restart in place instead of aborting a productive
// fetch. Cleared once per attempt, mirroring lastHeaderRecordCapHit()/clearHeaderRecordCapHit().
inline std::atomic<int64_t>& lastHeaderFetchProgressBytes() {
    static std::atomic<int64_t> v{0};
    return v;
}

inline void clearHeaderFetchProgress() {
    lastHeaderFetchProgressBytes().store(0, std::memory_order_release);
}

// Live-remux avStartGate skip_gop progress: true only while avStartGate() has both
// video and audio and is actively demuxing packets through a leading GOP (bounded by
// kLiveAvLongLeadWaitMs = 8 s). Allows a slow-but-healthy Pluto open (Trial e:
// 9.29 s demux + 6.53 s skip_gop = 15.83 s) to extend past 15 s without extending a
// stalled pre-demux or no-audio open (r2-03f).
inline std::atomic<bool>& liveRemuxSkipGopProgress() {
    static std::atomic<bool> v{false};
    return v;
}

inline void clearLiveRemuxSkipGopProgress() {
    liveRemuxSkipGopProgress().store(false, std::memory_order_release);
}

inline void noteLiveRemuxSkipGopProgress() {
#ifndef MUT_NO_LIVE_REMUX_SKIP_GOP_PROGRESS
    liveRemuxSkipGopProgress().store(true, std::memory_order_release);
#endif
}

inline bool consumeLiveRemuxSkipGopProgress() {
#ifdef MUT_NO_LIVE_REMUX_SKIP_GOP_PROGRESS
    return false;
#else
    return liveRemuxSkipGopProgress().exchange(false, std::memory_order_acq_rel);
#endif
}

class HeaderCacheStoreImpl : public std::enable_shared_from_this<HeaderCacheStoreImpl> {
public:
    explicit HeaderCacheStoreImpl(std::string dir = headerCacheDir(), int64_t maxDirBytes = kMaxHeaderCacheDirBytes)
        : dir_(std::move(dir)), maxDirBytes_(maxDirBytes) {}

    HeaderCacheStoreImpl(const HeaderCacheStoreImpl&) = delete;
    HeaderCacheStoreImpl& operator=(const HeaderCacheStoreImpl&) = delete;
    // P3-2: the worker holds shared ownership of Impl, so the public handle can drop
    // without waiting out an SD stall. Use waitIdle(-1) only when a caller needs to drain.
    ~HeaderCacheStoreImpl() = default;

    std::string filePath(const HeaderCacheKey& key) const {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(detail::fnv1a64(key.url)));
        std::string d = dir_;
        if (!d.empty() && d.back() != '/')
            d += '/';
        return d + hex + ".hdr";
    }

    // A timed-out partial recording lives at a distinct path from the complete `.hdr` entry
    // (never the same path) so load()/hasEntry() can never observe it.
    std::string partialFilePath(const HeaderCacheKey& key) const { return filePath(key) + ".partial"; }

    // Test hook: -1 uses statvfs; any other value is the free-byte figure store() sees.
    void setAvailableBytesForTest(int64_t n) { freeOverride_ = n; }
    // Test hook: fail the next n write attempts (each retry counts). 0 disables.
    void setStoreFailInjectForTest(int n) { failInject_ = n; }
    // Test hook: stall store() for this many ms (async path and waitIdle tests).
    void setStoreDelayMsForTest(int n) { storeDelayMs_ = n; }

    int64_t availableBytes() const {
        if (freeOverride_ >= 0)
            return freeOverride_;
        struct statvfs st {};
        std::string p = dir_;
        while (!p.empty() && ::statvfs(p.c_str(), &st) != 0) {
            auto slash = p.find_last_of('/');
            if (slash == std::string::npos || slash == 0)
                break;
            p.resize(slash);
        }
        if (p.empty() || ::statvfs(p.c_str(), &st) != 0)
            return kMinHeaderCacheFreeBytes;  // fail-open if we cannot measure
        return static_cast<int64_t>(st.f_bavail) * static_cast<int64_t>(st.f_frsize);
    }

    // timeoutMs < 0 waits forever. Returns true once no store is pending.
    bool waitIdle(int64_t timeoutMs = kHeaderCacheIdleTimeoutMs) const {
        std::unique_lock<std::mutex> lock(waitMu_);
        const auto ready = [this] { return pending_.load(std::memory_order_acquire) == 0 && neighborPending_.load(std::memory_order_acquire) == 0; };
        if (timeoutMs < 0) {
            waitCv_.wait(lock, ready);
            return true;
        }
        return waitCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
    }

    // Launch store() on a background thread so a cold zap can seek the IDR without
    // waiting on the SD write. ~HeaderCacheStore / waitIdle() drain the queue.
    void storeAsync(HeaderCacheData data) const {
        enqueueStore(std::move(data));
    }
    // Write from the caller's ranges without a second payload copy (browse-lane #647 P3-7).
    // The shared_ptr keeps the bytes alive if reset() times out waitIdle and drops its ref.
    void storeAsync(HeaderCacheKey key, int64_t moovOffset,
                    std::shared_ptr<const std::vector<HeaderRange>> ranges,
                    bool neighbor = false) const {
        if (!ranges)
            return;
        if (!tryBeginPendingStore(neighbor)) {
            std::fprintf(stderr,
                         "misterplexd: spelunkr: header store dropped pending>=%d neighbor=%d\n",
                         neighbor ? kMaxPendingNeighborStores : kMaxPendingHeaderStores,
                         neighbor ? 1 : 0);
            return;
        }
        const int delay = storeDelayMs_;
        auto self = shared_from_this();
        std::thread([self, key = std::move(key), moovOffset, ranges, delay, neighbor]() {
            if (delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            self->store(key, moovOffset, *ranges);
            self->finishPending(neighbor);
        }).detach();
    }

    // False: missing file, corrupt file, or a (size, etag) mismatch against this key — a cache
    // miss in every case, never thrown as an error.
    bool load(const HeaderCacheKey& key, HeaderCacheData& out) const {
        const std::string path = filePath(key);
        const auto corrupt = [this, &path] {
            std::lock_guard<std::mutex> lock(mu_);
            std::remove(path.c_str());
            return false;
        };
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        struct stat st {};
        if (::fstat(::fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < static_cast<off_t>(sizeof(kHeaderCacheMagic)) ||
            st.st_size > kMaxHeaderCacheFileBytes) {
            std::fclose(f);
            return corrupt();
        }
        HeaderCacheData d;
        uint32_t rangeCount = 0;
        if (!readKeyAndMeta(f, d, rangeCount)) {
            std::fclose(f);
            return corrupt();
        }
        if (!keyMatches(d.key, key)) {
            std::fclose(f);
            return false;
        }
        const size_t rest = static_cast<size_t>(st.st_size) - static_cast<size_t>(std::ftell(f));
        d.ranges.reserve(std::min<size_t>(rangeCount, rest / 12));
        for (uint32_t i = 0; i < rangeCount; ++i) {
            HeaderRange hr;
            uint64_t off = 0;
            if (!detail::readU64(f, off) || !detail::readStr(f, hr.data, static_cast<size_t>(kMaxHeaderCacheBytes))) {
                std::fclose(f);
                return corrupt();
            }
            hr.offset = static_cast<int64_t>(off);
            d.ranges.push_back(std::move(hr));
        }
        std::fclose(f);
        touchPath(path);
        out = std::move(d);
        return true;
    }

    // browse-lane #647 P2-2: yes/no only. Reads magic + key (stat + the file header), never the ranges.
    bool hasEntry(const HeaderCacheKey& key) const {
        const std::string path = filePath(key);
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        struct stat st {};
        if (::fstat(::fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < static_cast<off_t>(sizeof(kHeaderCacheMagic)) ||
            st.st_size > kMaxHeaderCacheFileBytes) {
            std::fclose(f);
            std::lock_guard<std::mutex> lock(mu_);
            std::remove(path.c_str());
            return false;
        }
        HeaderCacheData d;
        uint32_t rangeCount = 0;
        const bool ok = readKeyAndMeta(f, d, rangeCount);
        std::fclose(f);
        if (!ok) {
            std::lock_guard<std::mutex> lock(mu_);
            std::remove(path.c_str());
            return false;
        }
        if (!keyMatches(d.key, key))
            return false;
        touchPath(path);
        return true;
    }

    bool remove(const HeaderCacheKey& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return std::remove(filePath(key).c_str()) == 0;
    }

    // Atomic write (tmp file + rename). Creates the cache directory as needed. False on any
    // I/O failure, low free space (< 1 GiB), a recent failed write still in backoff, or when
    // the recorded ranges exceed kMaxHeaderCacheBytes. Writes each range with fwrite (no
    // serialized copy of the payload). On success, evicts the least-recently-used *.hdr
    // files (access time, utime'd on a hit) until the directory is at or under maxDirBytes_
    // (the entry just written is kept).
    bool store(const HeaderCacheData& data) const {
        return store(data.key, data.moovOffset, data.ranges, data.moovSize);
    }

    // Streams the caller's ranges; does not copy the payload into a second buffer (P3-7).
    bool store(const HeaderCacheKey& key, int64_t moovOffset, const std::vector<HeaderRange>& ranges,
               int64_t moovSize = -1) const {
        int64_t total = 0;
        size_t fileBytes = sizeof(kHeaderCacheMagic) + 4 + key.url.size() + 8 +
                           4 + key.etag.size() + 8 + 8 + 4;
        for (const auto& r : ranges) {
            total += static_cast<int64_t>(r.data.size());
            fileBytes += 8 + 4 + r.data.size();
        }
        if (total > kMaxHeaderCacheBytes || fileBytes > static_cast<size_t>(kMaxHeaderCacheFileBytes))
            return false;
        const std::string finalPath = filePath(key);
        if (inBackoff(finalPath))
            return false;
        if (availableBytes() < kMinHeaderCacheFreeBytes) {
            logLowSpaceOnce();
            return false;
        }
        if (!detail::mkdirsFor(dir_))
            return false;

        constexpr int kAttempts = 3;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (failInject_ > 0) {
                --failInject_;
                continue;
            }
            const std::string tmpPath = finalPath + ".tmp." + std::to_string(::getpid()) + "." +
                                        std::to_string(detail::nextTmpTag());
            FILE* f = std::fopen(tmpPath.c_str(), "wb");
            if (!f)
                continue;
            const bool wrote = writeFile(f, key, moovOffset, moovSize, ranges);
            const bool closed = std::fclose(f) == 0;
            if (!wrote || !closed) {
                std::remove(tmpPath.c_str());
                continue;
            }
            if (std::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
                std::remove(tmpPath.c_str());
                continue;
            }
            clearBackoff(finalPath);
            {
                std::lock_guard<std::mutex> lock(mu_);
                evictLocked(finalPath);
            }
            return true;
        }
        armBackoff(finalPath);
        return false;
    }

    // browse-lane P2-A: false, missing, corrupt, or a (size, etag) mismatch -- same contract as
    // load(), but against partialFilePath() and the kMaxPartialHeaderFileBytes cap so a
    // partial can hold up to kMaxPartialHeaderBytes of resumable recording.
    bool loadPartial(const HeaderCacheKey& key, HeaderCacheData& out) const {
        const std::string path = partialFilePath(key);
        const auto corrupt = [this, &path] {
            std::lock_guard<std::mutex> lock(mu_);
            std::remove(path.c_str());
            return false;
        };
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        struct stat st {};
        if (::fstat(::fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < static_cast<off_t>(sizeof(kHeaderCacheMagic)) ||
            st.st_size > kMaxPartialHeaderFileBytes) {
            std::fclose(f);
            return corrupt();
        }
        HeaderCacheData d;
        uint32_t rangeCount = 0;
        if (!readKeyAndMeta(f, d, rangeCount)) {
            std::fclose(f);
            return corrupt();
        }
        if (!keyMatches(d.key, key)) {
            std::fclose(f);
            return false;
        }
        const size_t rest = static_cast<size_t>(st.st_size) - static_cast<size_t>(std::ftell(f));
        d.ranges.reserve(std::min<size_t>(rangeCount, rest / 12));
        for (uint32_t i = 0; i < rangeCount; ++i) {
            HeaderRange hr;
            uint64_t off = 0;
            if (!detail::readU64(f, off) ||
                !detail::readStr(f, hr.data, static_cast<size_t>(kMaxPartialHeaderBytes))) {
                std::fclose(f);
                return corrupt();
            }
            hr.offset = static_cast<int64_t>(off);
            d.ranges.push_back(std::move(hr));
        }
        std::fclose(f);
        touchPath(path);
        out = std::move(d);
        return true;
    }

    // browse-lane P2-A: yes/no only, same shape as hasEntry() but against partialFilePath() -- a
    // partial can never make this true for filePath(key)/hasEntry(key), so a resumable
    // partial can never present as a fully-warm complete header.
    bool hasPartialEntry(const HeaderCacheKey& key) const {
        std::vector<PartialRangeRef> refs;
        int64_t bytes = 0, moov = -1;
        return loadPartialIndex(key, refs, bytes, moov);
    }

    bool removePartial(const HeaderCacheKey& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return std::remove(partialFilePath(key).c_str()) == 0;
    }

    static bool readPartialIndexRange(FILE* f, int64_t fileBytes, PartialRangeRef& ref) {
        uint64_t offset = 0;
        uint32_t length = 0;
        if (!detail::readU64(f, offset) || !detail::readU32(f, length))
            return false;
        const long dataPos = std::ftell(f);
        if (dataPos < 0 || dataPos > fileBytes || length == 0 ||
            length > static_cast<uint32_t>(kMaxPartialHeaderBytes) ||
            length > fileBytes - dataPos ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - length)
            return false;
        ref.offset = static_cast<int64_t>(offset);
        ref.dataPos = dataPos;
        ref.size = static_cast<int32_t>(length);
        return std::fseek(f, static_cast<long>(length), SEEK_CUR) == 0;
    }

    // Validate before allocating; each index retains the file that was parsed.
    bool loadPartialIndex(const HeaderCacheKey& key, std::vector<PartialRangeRef>& refs, int64_t& outBytes,
                          int64_t& outMoovOffset) const {
        std::vector<PartialRangeRef>().swap(refs);
        outBytes = 0;
        outMoovOffset = -1;
        const std::string path = partialFilePath(key);
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        FILE* raw = ::fdopen(fd, "rb");
        if (!raw) {
            ::close(fd);
            return false;
        }
        std::unique_ptr<FILE, int (*)(FILE*)> file(raw, &std::fclose);
        FILE* f = file.get();
        struct stat st {};
        if (::fstat(::fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size > kMaxPartialHeaderFileBytes)
            return false;
        HeaderCacheData d;
        uint32_t rangeCount = 0;
        if (!readKeyAndMeta(f, d, rangeCount) || !keyMatches(d.key, key))
            return false;
        const long rangesPos = std::ftell(f);
        if (rangesPos < 0 || rangesPos > st.st_size || rangeCount == 0 ||
            rangeCount > static_cast<uint64_t>(st.st_size - rangesPos) / 12 ||
            rangeCount > kMaxPartialIndexBytes / sizeof(PartialRangeRef))
            return false;
        int64_t total = 0;
        for (uint32_t i = 0; i < rangeCount; ++i) {
            PartialRangeRef ref;
            if (!readPartialIndexRange(f, st.st_size, ref) ||
                ref.size > kMaxPartialHeaderBytes - total)
                return false;
            total += ref.size;
        }
        if (std::ftell(f) != st.st_size || std::fseek(f, rangesPos, SEEK_SET) != 0)
            return false;
        refs.reserve(rangeCount);
        for (uint32_t i = 0; i < rangeCount; ++i) {
            PartialRangeRef ref;
            if (!readPartialIndexRange(f, st.st_size, ref)) {
                refs.clear();
                return false;
            }
            refs.push_back(std::move(ref));
        }
        const auto snapshot = std::make_shared<PartialFileSnapshot>(
            PartialFileSnapshot{std::move(d.key), std::shared_ptr<FILE>(std::move(file))});
        for (auto& ref : refs)
            ref.snapshot = snapshot;
        outBytes = total;
        outMoovOffset = d.moovOffset;
        touchPath(path);
        return true;
    }

    bool readPartialSpan(const HeaderCacheKey& key, const std::vector<PartialRangeRef>& refs, int64_t offset,
                         uint8_t* buf, int n, int& served) const {
        served = 0;
        if (!buf || n <= 0 || refs.empty())
            return false;
        for (const auto& r : refs) {
            if (!r.snapshot || !keyMatches(r.snapshot->key, key))
                continue;
            if (offset < r.offset || offset >= r.offset + r.size)
                continue;
            const int64_t within = offset - r.offset;
            const int64_t avail = static_cast<int64_t>(r.size) - within;
            const int count = static_cast<int>(std::min<int64_t>(avail, n));
            int done = 0;
            while (done < count) {
                const ssize_t got = ::pread(::fileno(r.snapshot->file.get()), buf + done,
                                            static_cast<size_t>(count - done), r.dataPos + within + done);
                if (got < 0 && errno == EINTR)
                    continue;
                if (got <= 0)
                    return false;
                done += static_cast<int>(got);
            }
            served = count;
            return true;
        }
        return false;
    }

    static int64_t partialBytesFromRefs(const std::vector<PartialRangeRef>& refs) {
        int64_t t = 0;
        for (const auto& r : refs)
            t += r.size;
        return t;
    }

    struct PartialSpillPiece {
        int64_t offset = 0;
        int32_t size = 0;
        int64_t diskDataPos = -1;
        const HeaderRange* ram = nullptr;
    };

    static bool copyRangePayload(FILE* out, FILE* inf, int64_t dataPos, int32_t size) {
        if (!out || !inf || size <= 0)
            return false;
        if (std::fseek(inf, dataPos, SEEK_SET) != 0)
            return false;
        char buf[65536];
        int32_t left = size;
        while (left > 0) {
            const int chunk = static_cast<int>(std::min<int32_t>(left, static_cast<int32_t>(sizeof(buf))));
            if (std::fread(buf, 1, static_cast<size_t>(chunk), inf) != static_cast<size_t>(chunk))
                return false;
            if (std::fwrite(buf, 1, static_cast<size_t>(chunk), out) != static_cast<size_t>(chunk))
                return false;
            left -= chunk;
        }
        return true;
    }

    static int64_t scanMoovOffsetSingle(const HeaderRange& r) {
        if (r.data.size() < 8)
            return -1;
        for (size_t i = 4; i + 4 <= r.data.size(); ++i) {
            if (std::memcmp(r.data.data() + i, "moov", 4) == 0)
                return r.offset + static_cast<int64_t>(i) - 4;
        }
        return -1;
    }

    static int64_t scanMoovInFile(FILE* f, const std::vector<PartialSpillPiece>& pieces) {
        for (const auto& p : pieces) {
            std::string sample;
            sample.resize(std::min<int32_t>(p.size, 256));
            if (p.ram) {
                if (p.ram->data.size() < sample.size())
                    sample.assign(p.ram->data.data(), p.ram->data.size());
                else
                    std::memcpy(sample.data(), p.ram->data.data(), sample.size());
            } else {
                if (!f || std::fseek(f, p.diskDataPos, SEEK_SET) != 0 ||
                    std::fread(sample.data(), 1, sample.size(), f) != sample.size())
                    continue;
            }
            HeaderRange hr{p.offset, std::move(sample)};
            const int64_t moov = scanMoovOffsetSingle(hr);
            if (moov >= 0)
                return moov;
        }
        return -1;
    }

    // browse-lane #663 follow-up (P3-2): the common case is a sequential forward read -- every new ram
    // range lands strictly after every byte already on disk. That case can append the new range
    // entries (and patch the range count / moov-offset fields) onto the existing `.hdr.partial`
    // in place, instead of spillPartialRangesRewrite()'s full read-everything-back-and-rewrite,
    // so a 35.8 MB item does not re-copy its already-spilled prefix out to a tmp file on every
    // spill call on the play/prefetch thread. False (not appended; `outTotal` untouched): no
    // existing partial for this key, a corrupt/truncated file, or any out-of-order/overlapping
    // new range -- the caller falls back to the full rewrite, which still produces a correct
    // result for those shapes.
    bool appendPartialRanges(const HeaderCacheKey& key, const std::vector<HeaderRange>& ram,
                             int64_t& outTotal) const {
        const std::string finalPath = partialFilePath(key);
        FILE* f = std::fopen(finalPath.c_str(), "r+b");
        if (!f)
            return false;  // no existing partial: let the rewrite path create it
        char magic[sizeof(kHeaderCacheMagic)];
        std::string url, etag;
        uint64_t size = 0, moovOff = 0, moovSz = 0;
        uint32_t rangeCount = 0;
        bool ok = detail::freadAll(f, magic, sizeof magic) &&
                  std::memcmp(magic, kHeaderCacheMagic, sizeof magic) == 0 &&
                  detail::readStr(f, url, 1 << 20) && detail::readU64(f, size) &&
                  detail::readStr(f, etag, 1 << 16);
        const long moovOffsetFieldPos = ok ? std::ftell(f) : -1;
        ok = ok && detail::readU64(f, moovOff) && detail::readU64(f, moovSz);
        const long rangeCountFieldPos = ok ? std::ftell(f) : -1;
        ok = ok && detail::readU32(f, rangeCount);
        if (!ok) {
            std::fclose(f);
            return false;
        }
        const HeaderCacheKey stored{url, static_cast<int64_t>(size), etag};
        if (!keyMatches(stored, key)) {
            std::fclose(f);
            return false;
        }
        int64_t existingTotal = 0;
        int64_t tailEnd = 0;  // max(offset + size) across every already-spilled range
        for (uint32_t i = 0; ok && i < rangeCount; ++i) {
            uint64_t off = 0;
            uint32_t len = 0;
            if (!detail::readU64(f, off) || !detail::readU32(f, len) ||
                std::fseek(f, static_cast<long>(len), SEEK_CUR) != 0) {
                ok = false;
                break;
            }
            existingTotal += len;
            tailEnd = std::max(tailEnd, static_cast<int64_t>(off) + static_cast<int64_t>(len));
        }
        if (!ok) {
            std::fclose(f);
            return false;
        }
        const long appendPos = std::ftell(f);
        // A corrupt/truncated file (trailing bytes the declared rangeCount does not account
        // for) must not be appended onto -- fall back so the rewrite path rebuilds it cleanly.
        if (std::fseek(f, 0, SEEK_END) != 0 || std::ftell(f) != appendPos) {
            std::fclose(f);
            return false;
        }
        // Every new range (already offset-sorted by the caller's mergeRanges) must land at or
        // after the on-disk tail and not overlap another new range -- otherwise this is not the
        // simple append case and the rewrite path must merge/reorder it.
        int64_t cursor = tailEnd;
        for (const auto& r : ram) {
            if (r.offset < cursor) {
                std::fclose(f);
                return false;
            }
            cursor = r.offset + static_cast<int64_t>(r.data.size());
        }
        int64_t newTotal = existingTotal;
        for (const auto& r : ram)
            newTotal += static_cast<int64_t>(r.data.size());
        if (newTotal > kMaxPartialHeaderBytes || inBackoff(finalPath) ||
            availableBytes() < kMinHeaderCacheFreeBytes) {
            if (availableBytes() < kMinHeaderCacheFreeBytes)
                logLowSpaceOnce();
            std::fclose(f);
            return false;
        }
        if (std::fseek(f, appendPos, SEEK_SET) != 0) {
            std::fclose(f);
            return false;
        }
        for (const auto& r : ram) {
            if (!detail::writeU64(f, static_cast<uint64_t>(r.offset)) || !detail::writeStr(f, r.data)) {
                ok = false;
                break;
            }
        }
        ok = ok && std::fflush(f) == 0;
        if (ok) {
            // The appended payload is durable before the index/moov fields are patched to
            // reference it, so a crash mid-patch leaves the old (still-valid) rangeCount.
            const int fd = ::fileno(f);
            if (fd >= 0)
                ::fsync(fd);
            const uint32_t newCount = rangeCount + static_cast<uint32_t>(ram.size());
            ok = std::fseek(f, rangeCountFieldPos, SEEK_SET) == 0 && detail::writeU32(f, newCount);
            // moov offset is logging-only (scanMoovOffset's header comment); best-effort only.
            if (ok && static_cast<int64_t>(moovOff) < 0) {
                int64_t moov = -1;
                for (const auto& r : ram) {
                    if (const int64_t m = scanMoovOffsetSingle(r); m >= 0) {
                        moov = m;
                        break;
                    }
                }
                if (moov >= 0 && std::fseek(f, moovOffsetFieldPos, SEEK_SET) == 0)
                    detail::writeU64(f, static_cast<uint64_t>(moov));
            }
            ok = ok && std::fflush(f) == 0;
            if (ok && fd >= 0)
                ::fsync(fd);
        }
        if (std::fclose(f) != 0)
            ok = false;
        if (!ok)
            return false;
        clearBackoff(finalPath);
        {
            std::lock_guard<std::mutex> lock(mu_);
            evictLocked(finalPath);
        }
        outTotal = newTotal;
        return true;
    }

    // Merge in-RAM ranges into the on-disk partial without holding the full partial in RAM.
    // Dispatches to the append-only fast path (appendPartialRanges) for the common sequential
    // spill case; falls back to a full read-and-rewrite (spillPartialRangesRewrite) otherwise.
    bool spillPartialRanges(const HeaderCacheKey& key, const std::vector<HeaderRange>& ram, int64_t& outTotal) const {
        outTotal = 0;
        if (ram.empty()) {
            std::vector<PartialRangeRef> refs;
            int64_t moov = -1;
            return loadPartialIndex(key, refs, outTotal, moov);
        }
        if (appendPartialRanges(key, ram, outTotal))
            return true;
        return spillPartialRangesRewrite(key, ram, outTotal);
    }

    // Full read-existing-index-back-and-rewrite path (the original #663 spillPartialRanges):
    // correct for every shape (new key, overlap, out-of-order, corrupt file) but re-copies
    // every already-spilled payload byte through a tmp file + rename on each call.
    bool spillPartialRangesRewrite(const HeaderCacheKey& key, const std::vector<HeaderRange>& ram,
                                   int64_t& outTotal) const {
        outTotal = 0;
        const std::string finalPath = partialFilePath(key);
        std::vector<PartialSpillPiece> merged;
        int64_t moovFromDisk = -1;
        const size_t maxPieces = kMaxPartialIndexBytes / sizeof(PartialSpillPiece);
        if (ram.size() > maxPieces)
            return false;
        std::unique_ptr<FILE, int (*)(FILE*)> disk(std::fopen(finalPath.c_str(), "rb"), &std::fclose);
        FILE* inf = disk.get();
        if (inf) {
            HeaderCacheData d;
            uint32_t rangeCount = 0;
            if (readKeyAndMeta(inf, d, rangeCount) && keyMatches(d.key, key)) {
                struct stat st {};
                const long rangesPos = std::ftell(inf);
                if (::fstat(::fileno(inf), &st) != 0 || !S_ISREG(st.st_mode) ||
                    st.st_size > kMaxPartialHeaderFileBytes || rangesPos < 0 || rangesPos > st.st_size ||
                    rangeCount > static_cast<uint64_t>(st.st_size - rangesPos) / 12 ||
                    rangeCount > maxPieces - ram.size())
                    return false;
                merged.reserve(rangeCount + ram.size());
                moovFromDisk = d.moovOffset;
                for (uint32_t i = 0; i < rangeCount; ++i) {
                    uint64_t off = 0;
                    uint32_t len = 0;
                    if (!detail::readU64(inf, off) || !detail::readU32(inf, len))
                        break;
                    if (len == 0 || len > static_cast<uint32_t>(kMaxPartialHeaderBytes) ||
                        off > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - len)
                        return false;
                    const long dataPos = std::ftell(inf);
                    merged.push_back({static_cast<int64_t>(off), static_cast<int32_t>(len),
                                      static_cast<int64_t>(dataPos), nullptr});
                    if (std::fseek(inf, static_cast<long>(len), SEEK_CUR) != 0)
                        break;
                }
            }
        }
        if (merged.empty())
            merged.reserve(ram.size());
        for (auto& r : ram)
            merged.push_back({r.offset, static_cast<int32_t>(r.data.size()), -1, &r});
        std::sort(merged.begin(), merged.end(),
                  [](const PartialSpillPiece& a, const PartialSpillPiece& b) { return a.offset < b.offset; });
        for (const auto& m : merged)
            outTotal += m.size;
        if (outTotal > kMaxPartialHeaderBytes || inBackoff(finalPath))
            return false;
        if (availableBytes() < kMinHeaderCacheFreeBytes) {
            logLowSpaceOnce();
            return false;
        }
        if (!detail::mkdirsFor(dir_))
            return false;
        int64_t moov = moovFromDisk;
        for (const auto& r : ram) {
            if (const int64_t m = scanMoovOffsetSingle(r); m >= 0) {
                moov = m;
                break;
            }
        }
        if (moov < 0)
            moov = scanMoovInFile(inf, merged);
        const std::string tmpPath = finalPath + ".tmp." + std::to_string(::getpid()) + "." +
                                    std::to_string(detail::nextTmpTag());
        FILE* out = std::fopen(tmpPath.c_str(), "wb");
        if (!out)
            return false;
        if (std::fwrite(kHeaderCacheMagic, 1, sizeof(kHeaderCacheMagic), out) != sizeof(kHeaderCacheMagic) ||
            !detail::writeStr(out, key.url) ||
            !detail::writeU64(out, static_cast<uint64_t>(key.size)) || !detail::writeStr(out, key.etag) ||
            !detail::writeU64(out, static_cast<uint64_t>(moov)) || !detail::writeU64(out, ~0ull) ||
            !detail::writeU32(out, static_cast<uint32_t>(merged.size()))) {
            std::fclose(out);
            std::remove(tmpPath.c_str());
            return false;
        }
        bool ok = true;
        for (const auto& m : merged) {
            if (!detail::writeU64(out, static_cast<uint64_t>(m.offset)) ||
                !detail::writeU32(out, static_cast<uint32_t>(m.size))) {
                ok = false;
                break;
            }
            if (m.ram) {
                if (!detail::fwriteAll(out, m.ram->data.data(), static_cast<size_t>(m.size))) {
                    ok = false;
                    break;
                }
            } else if (!copyRangePayload(out, inf, m.diskDataPos, m.size)) {
                ok = false;
                break;
            }
        }
        disk.reset();
        const bool closed = std::fclose(out) == 0;
        ok = ok && closed;
        if (!ok) {
            std::remove(tmpPath.c_str());
            return false;
        }
        if (std::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
            std::remove(tmpPath.c_str());
            return false;
        }
        clearBackoff(finalPath);
        {
            std::lock_guard<std::mutex> lock(mu_);
            evictLocked(finalPath);
        }
        return true;
    }

    // Same write path as store(), capped at kMaxPartialHeaderBytes and written to
    // partialFilePath() instead of filePath() (browse-lane P2-A dial-500 resume).
    bool storePartial(const HeaderCacheKey& key, int64_t moovOffset, const std::vector<HeaderRange>& ranges,
                      int64_t moovSize = -1) const {
        int64_t total = 0;
        size_t fileBytes = sizeof(kHeaderCacheMagic) + 4 + key.url.size() + 8 +
                           4 + key.etag.size() + 8 + 8 + 4;
        for (const auto& r : ranges) {
            total += static_cast<int64_t>(r.data.size());
            fileBytes += 8 + 4 + r.data.size();
        }
        if (total > kMaxPartialHeaderBytes || fileBytes > static_cast<size_t>(kMaxPartialHeaderFileBytes))
            return false;
        const std::string finalPath = partialFilePath(key);
        if (inBackoff(finalPath))
            return false;
        if (availableBytes() < kMinHeaderCacheFreeBytes) {
            logLowSpaceOnce();
            return false;
        }
        if (!detail::mkdirsFor(dir_))
            return false;

        constexpr int kAttempts = 3;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (failInject_ > 0) {
                --failInject_;
                continue;
            }
            const std::string tmpPath = finalPath + ".tmp." + std::to_string(::getpid()) + "." +
                                        std::to_string(detail::nextTmpTag());
            FILE* f = std::fopen(tmpPath.c_str(), "wb");
            if (!f)
                continue;
            const bool wrote = writeFile(f, key, moovOffset, moovSize, ranges);
            const bool closed = std::fclose(f) == 0;
            if (!wrote || !closed) {
                std::remove(tmpPath.c_str());
                continue;
            }
            if (std::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
                std::remove(tmpPath.c_str());
                continue;
            }
            clearBackoff(finalPath);
            {
                std::lock_guard<std::mutex> lock(mu_);
                evictLocked(finalPath);
            }
            return true;
        }
        armBackoff(finalPath);
        return false;
    }

    // Mirrors storeAsync(key, moovOffset, ranges): launches storePartial() on a background
    // thread, sharing the on-path pending-store accounting (never called for a neighbour
    // fetch -- see header_cache_avio.hpp persistPartial()).
    void storePartialAsync(HeaderCacheKey key, int64_t moovOffset,
                           std::shared_ptr<const std::vector<HeaderRange>> ranges) const {
        if (!ranges)
            return;
        if (!tryBeginPendingStore(false)) {
            std::fprintf(stderr,
                         "misterplexd: spelunkr: header partial store dropped pending>=%d\n",
                         kMaxPendingHeaderStores);
            return;
        }
        const int delay = storeDelayMs_;
        auto self = shared_from_this();
        std::thread([self, key = std::move(key), moovOffset, ranges, delay]() {
            if (delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            self->storePartial(key, moovOffset, *ranges);
            self->finishPending(false);
        }).detach();
    }

private:
    static bool keyMatches(const HeaderCacheKey& stored, const HeaderCacheKey& key) {
        if (stored.url != key.url)
            return false;
        if (key.size >= 0 && stored.size >= 0 && stored.size != key.size)
            return false;
        if (!key.etag.empty() && !stored.etag.empty() && stored.etag != key.etag)
            return false;
        return true;
    }

    static bool readKeyAndMeta(FILE* f, HeaderCacheData& d, uint32_t& rangeCount) {
        char magic[sizeof(kHeaderCacheMagic)];
        if (!detail::freadAll(f, magic, sizeof magic) ||
            std::memcmp(magic, kHeaderCacheMagic, sizeof magic) != 0)
            return false;
        uint64_t size = 0, moovOff = 0, moovSz = 0;
        if (!detail::readStr(f, d.key.url, 1 << 20) || !detail::readU64(f, size) ||
            !detail::readStr(f, d.key.etag, 1 << 16) || !detail::readU64(f, moovOff) ||
            !detail::readU64(f, moovSz) || !detail::readU32(f, rangeCount))
            return false;
        d.key.size = static_cast<int64_t>(size);
        d.moovOffset = static_cast<int64_t>(moovOff);
        d.moovSize = static_cast<int64_t>(moovSz);
        return true;
    }

    static bool writeFile(FILE* f, const HeaderCacheData& data) {
        return writeFile(f, data.key, data.moovOffset, data.moovSize, data.ranges);
    }
    static bool writeFile(FILE* f, const HeaderCacheKey& key, int64_t moovOffset, int64_t moovSize,
                          const std::vector<HeaderRange>& ranges) {
        if (std::fwrite(kHeaderCacheMagic, 1, sizeof(kHeaderCacheMagic), f) != sizeof(kHeaderCacheMagic))
            return false;
        if (!detail::writeStr(f, key.url) ||
            !detail::writeU64(f, static_cast<uint64_t>(key.size)) ||
            !detail::writeStr(f, key.etag) ||
            !detail::writeU64(f, static_cast<uint64_t>(moovOffset)) ||
            !detail::writeU64(f, static_cast<uint64_t>(moovSize)) ||
            !detail::writeU32(f, static_cast<uint32_t>(ranges.size())))
            return false;
        for (const auto& r : ranges) {
            if (!detail::writeU64(f, static_cast<uint64_t>(r.offset)) || !detail::writeStr(f, r.data))
                return false;
        }
        return true;
    }

    static void touchPath(const std::string& path) {
        // LRU eviction keys off mtime; bump it on a hit (browse-lane #647 P2-4).
        ::utime(path.c_str(), nullptr);
    }

    void logLowSpaceOnce() const {
        bool expected = false;
        if (loggedLowSpace_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "misterplexd: spelunkr: header-cache skip store: less than 1 GiB free on %s\n",
                         dir_.c_str());
        }
    }

    bool inBackoff(const std::string& path) const {
        std::lock_guard<std::mutex> lock(failMu_);
        const auto it = failUntil_.find(path);
        if (it == failUntil_.end())
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= it->second)
            return false;
        return true;
    }
    void armBackoff(const std::string& path) const {
        std::lock_guard<std::mutex> lock(failMu_);
        auto& until = failUntil_[path];
        const auto now = std::chrono::steady_clock::now();
        const auto step = std::chrono::seconds(60);
        until = (until > now ? until : now) + step;
        if (until > now + std::chrono::minutes(10))
            until = now + std::chrono::minutes(10);
    }
    void clearBackoff(const std::string& path) const {
        std::lock_guard<std::mutex> lock(failMu_);
        failUntil_.erase(path);
    }

    bool tryBeginPendingStore(bool neighbor = false) const {
        // Check-then-add under the matching counter. Neighbour stores have their
        // own cap so they cannot fill the on-path slots (browse-lane #647 P3-4).
        auto& ctr = neighbor ? neighborPending_ : pending_;
        const int cap = neighbor ? kMaxPendingNeighborStores : kMaxPendingHeaderStores;
        const int prev = ctr.fetch_add(1, std::memory_order_acq_rel);
        if (prev >= cap) {
            ctr.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        detail::headerCachePendingStores().fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    void enqueueStore(HeaderCacheData data) const {
        if (!tryBeginPendingStore()) {
            std::fprintf(stderr,
                         "misterplexd: spelunkr: header store dropped pending>=%d\n",
                         kMaxPendingHeaderStores);
            return;
        }
        const int delay = storeDelayMs_;
        auto self = shared_from_this();
        std::thread([self, data = std::move(data), delay]() {
            if (delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            self->store(data);
            self->finishPending();
        }).detach();
    }
    void finishPending(bool neighbor = false) const {
        // P3-1: take the mutex first so waitIdle cannot observe pending_==0 and
        // destroy the control block while this thread still needs waitMu_/waitCv_.
        std::lock_guard<std::mutex> lock(waitMu_);
        auto& ctr = neighbor ? neighborPending_ : pending_;
        ctr.fetch_sub(1, std::memory_order_acq_rel);
        detail::headerCachePendingStores().fetch_sub(1, std::memory_order_acq_rel);
        waitCv_.notify_all();
    }

    // Caller holds mu_. Evicts least-recently-used *.hdr files by mtime (touched on hit).
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
            // A `.hdr.partial` recording counts toward the directory cap and is LRU-evicted
            // the same as a complete `.hdr` entry (browse-lane P2-A); it is never itself a `.hdr` hit.
            const bool isHdr = (name.size() >= 4 && name.compare(name.size() - 4, 4, ".hdr") == 0) ||
                               (name.size() >= 12 && name.compare(name.size() - 12, 12, ".hdr.partial") == 0);
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
    mutable std::mutex failMu_;
    mutable std::mutex waitMu_;
    mutable std::condition_variable waitCv_;
    mutable std::atomic<int> pending_{0};
    mutable std::atomic<int> neighborPending_{0};
    mutable std::unordered_map<std::string, std::chrono::steady_clock::time_point> failUntil_;
    mutable std::atomic<bool> loggedLowSpace_{false};
    int64_t freeOverride_ = -1;
    mutable int failInject_ = 0;
    mutable int storeDelayMs_ = 0;
};



// Public handle. The worker thread holds a shared_ptr to HeaderCacheStoreImpl, so
// destroying this handle (AvSource / test scope) cannot UAF or block on an SD stall (P3-2).
class HeaderCacheStore {
public:
    explicit HeaderCacheStore(std::string dir = headerCacheDir(), int64_t maxDirBytes = kMaxHeaderCacheDirBytes)
        : impl_(std::make_shared<HeaderCacheStoreImpl>(std::move(dir), maxDirBytes)) {}

    HeaderCacheStore(const HeaderCacheStore&) = delete;
    HeaderCacheStore& operator=(const HeaderCacheStore&) = delete;
    ~HeaderCacheStore() = default;

    std::string filePath(const HeaderCacheKey& key) const { return impl_->filePath(key); }
    void setAvailableBytesForTest(int64_t n) { impl_->setAvailableBytesForTest(n); }
    void setStoreFailInjectForTest(int n) { impl_->setStoreFailInjectForTest(n); }
    void setStoreDelayMsForTest(int n) { impl_->setStoreDelayMsForTest(n); }
    int64_t availableBytes() const { return impl_->availableBytes(); }
    bool waitIdle(int64_t timeoutMs = kHeaderCacheIdleTimeoutMs) const { return impl_->waitIdle(timeoutMs); }
    void storeAsync(HeaderCacheData data) const { impl_->storeAsync(std::move(data)); }
    void storeAsync(HeaderCacheKey key, int64_t moovOffset,
                    std::shared_ptr<const std::vector<HeaderRange>> ranges,
                    bool neighbor = false) const {
        impl_->storeAsync(std::move(key), moovOffset, std::move(ranges), neighbor);
    }
    bool load(const HeaderCacheKey& key, HeaderCacheData& out) const { return impl_->load(key, out); }
    bool hasEntry(const HeaderCacheKey& key) const { return impl_->hasEntry(key); }
    bool remove(const HeaderCacheKey& key) const { return impl_->remove(key); }
    bool store(const HeaderCacheData& data) const { return impl_->store(data); }
    bool store(const HeaderCacheKey& key, int64_t moovOffset, const std::vector<HeaderRange>& ranges,
               int64_t moovSize = -1) const {
        return impl_->store(key, moovOffset, ranges, moovSize);
    }
    // browse-lane P2-A: partial (timed-out, incomplete, or on-path oversize) header recordings --
    // never a `hasEntry()`/`load()` hit, see partialFilePath().
    std::string partialFilePath(const HeaderCacheKey& key) const { return impl_->partialFilePath(key); }
    bool loadPartial(const HeaderCacheKey& key, HeaderCacheData& out) const { return impl_->loadPartial(key, out); }
    bool hasPartialEntry(const HeaderCacheKey& key) const { return impl_->hasPartialEntry(key); }
    bool removePartial(const HeaderCacheKey& key) const { return impl_->removePartial(key); }
    bool storePartial(const HeaderCacheKey& key, int64_t moovOffset, const std::vector<HeaderRange>& ranges,
                      int64_t moovSize = -1) const {
        return impl_->storePartial(key, moovOffset, ranges, moovSize);
    }
    void storePartialAsync(HeaderCacheKey key, int64_t moovOffset,
                           std::shared_ptr<const std::vector<HeaderRange>> ranges) const {
        impl_->storePartialAsync(std::move(key), moovOffset, std::move(ranges));
    }
    bool loadPartialIndex(const HeaderCacheKey& key, std::vector<PartialRangeRef>& refs, int64_t& outBytes,
                          int64_t& outMoovOffset) const {
        return impl_->loadPartialIndex(key, refs, outBytes, outMoovOffset);
    }
    bool readPartialSpan(const HeaderCacheKey& key, const std::vector<PartialRangeRef>& refs, int64_t offset,
                         uint8_t* buf, int n, int& served) const {
        return impl_->readPartialSpan(key, refs, offset, buf, n, served);
    }
    bool spillPartialRanges(const HeaderCacheKey& key, const std::vector<HeaderRange>& ram, int64_t& outTotal) const {
        return impl_->spillPartialRanges(key, ram, outTotal);
    }

private:
    std::shared_ptr<HeaderCacheStoreImpl> impl_;
};

// P3-3: a recorder that hit the per-entry cap must not persist a partial head (hasEntry
// would otherwise report warm and clear a dead verdict). The complete-entry cap follows
// effectivePartialRamCapBytes() so the MPX_SPELUNKR_FORCE_PARTIAL_HEADER glass fixture hook
// (browse-lane #663 follow-up) can force a modest recording over it; unset, this is kMaxHeaderCacheBytes
// exactly (unchanged production contract).
inline bool shouldPersistRecordedRanges(int64_t recordedBytes, bool capHit) {
    return !capHit && recordedBytes > 0 && recordedBytes <= effectivePartialRamCapBytes();
}

// browse-lane P2-A: distinct contract from shouldPersistRecordedRanges -- gates storePartial(), never
// store(), so a cap-hit or oversize-for-the-complete-cap recording can still be kept as a
// resumable partial (bounded by kMaxPartialHeaderBytes instead of kMaxHeaderCacheBytes) without
// ever being eligible to become a hasEntry()/load() complete hit.
inline bool shouldPersistPartialRecordedRanges(int64_t recordedBytes, bool capHit) {
    return !capHit && recordedBytes > 0 && recordedBytes <= kMaxPartialHeaderBytes;
}

inline std::atomic<bool>& lastHeaderRecordCapHit() {
    static std::atomic<bool> v{false};
    return v;
}

inline bool consumeHeaderRecordCapHit() {
    return lastHeaderRecordCapHit().exchange(false, std::memory_order_acq_rel);
}

inline void clearHeaderRecordCapHit() {
    lastHeaderRecordCapHit().store(false, std::memory_order_release);
}

}  // namespace spelunkr
}  // namespace misterplex
