#pragma once

#include "libmisterplex/browse_fetch.hpp"
#include "plexui_data.hpp"
#include "plexui_disk_cache.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace misterplex {
namespace plexui {

struct DecodedImage {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
    int focusW = 0;
    int focusH = 0;
    std::vector<std::uint8_t> focusRgba;
    std::vector<std::uint8_t> backdropRgba;
};

enum class ImageKind : uint8_t { Thumb, Art };

struct ImageCacheKey {
    std::string machineIdentifier;
    std::string ratingKey;
    ImageKind kind = ImageKind::Thumb;
    int w = 0;
    int h = 0;
    std::string updatedAt;

    bool operator==(const ImageCacheKey& o) const {
        return machineIdentifier == o.machineIdentifier && ratingKey == o.ratingKey &&
               kind == o.kind && w == o.w && h == o.h && updatedAt == o.updatedAt;
    }
    bool operator!=(const ImageCacheKey& o) const { return !(*this == o); }
};

struct ImageCacheKeyHash {
    std::size_t operator()(const ImageCacheKey& k) const;
};

std::string imageCacheKeyString(const ImageCacheKey& k);

std::string parseImagePathUpdatedAt(const std::string& path);
std::string itemThumbRevision(const PlexItem& it);
std::string itemArtRevision(const PlexItem& it);

ImageCacheKey thumbCacheKey(const std::string& machineId, const std::string& ratingKey, int w,
                            int h, const std::string& updatedAt = {});
ImageCacheKey artCacheKey(const std::string& machineId, const std::string& ratingKey, int w, int h,
                          const std::string& updatedAt = {});

std::size_t decodedImageBytes(int w, int h);
std::size_t computeImageCacheBudgetBytes(int hubItemCount, int gridPosterW, int gridPosterH,
                                         int gridTileSlots);
std::size_t computeRamCacheBudgetBytes(int hubItemCount, int gridPosterW, int gridPosterH,
                                       int gridVisibleRows, int gridCols);

// OB port: the canvas the decoded art is prepared for (corner radius, R1 backdrops, budget).
// Defaults are R1 (640x480, radius 8, hub 96x144). Thread-safe.
void setImageGeometry(int screenW, int screenH, int cornerRadius, int hubPosterW, int hubPosterH);

class ImageCache {
public:
    struct SrcStats {
        int ram = 0;
        int disk = 0;
        int net = 0;
    };

    explicit ImageCache(std::size_t maxBytes = 0);
    ~ImageCache();

    void start(HttpGetFn get, std::function<void(const ImageCacheKey& key)> onArrival,
               std::function<void(const std::string&)> logLine = {});
    void stop();

    void setMaxBytes(std::size_t maxBytes);
    std::size_t maxBytes() const;
    void logBudgetOnce(std::function<void(const std::string&)> logLine);

    void beginView(uint64_t viewGen);
    void setPinned(const std::vector<ImageCacheKey>& keys);
    void request(const ImageCacheKey& key, const std::string& fetchUrl, uint64_t viewGen,
                 int priority);

    std::shared_ptr<const DecodedImage> get(const ImageCacheKey& key) const;
    std::size_t bytesUsed() const;
    bool busy() const;
    SrcStats srcStats() const;

    // 0012 soak OOM: ONE hard byte budget over every PlexUI pixel tier (art LRU, the renderer's
    // view-layer cache, in-flight decodes/disk reads, queued disk writes). Evict before allocating;
    // when still over, the decode is refused (re-requested by the next view).
    struct MemStats {
        std::size_t artBytes = 0, artEntries = 0, layerBytes = 0, inflightBytes = 0,
                    inflight = 0, diskqBytes = 0, total = 0, budget = 0;
        uint64_t evictions = 0, refused = 0, pinnedEvictions = 0;
    };
    MemStats memStats() const;
    std::size_t totalBytes() const;
    void setLayerBytes(std::size_t bytes);    // renderer view-layer cache tier
    std::string memStatsLine() const;

#ifdef MPX_PLEXUI_UNIT_TEST
    void testInsertDecoded(const ImageCacheKey& key, int w, int h, std::uint8_t fill = 200);
    // reserveLocked + release. False: the bytes did not fit without evicting pins.
    bool testAdmitInflight(std::size_t bytes);
    std::size_t testPeakTotal() const { return peakTotal_.load(); }
#endif

private:
    struct Job {
        ImageCacheKey key;
        std::string url;
        uint64_t viewGen = 0;
        int priority = 0;
        uint64_t seq = 0;
        std::chrono::steady_clock::time_point enqueued{};
    };
    struct JobCmp {
        bool operator()(const Job& a, const Job& b) const {
            if (a.viewGen != b.viewGen)
                return a.viewGen > b.viewGen;
            if (a.priority != b.priority)
                return a.priority > b.priority;
            return a.seq > b.seq;
        }
    };

    void workerLoop(int workerId);
    void touchLru(const ImageCacheKey& key) const;
    void evictFor(std::size_t need);
    std::size_t totalLocked() const;
    bool reserveLocked(std::size_t bytes);   // evict, then reserve in-flight bytes or refuse
    void releaseLocked(std::size_t bytes);
    void notePeakLocked();
    void statsLoop();
    std::size_t imageFootprint(const DecodedImage& img) const;
    void insertImage(const ImageCacheKey& key, std::shared_ptr<DecodedImage> img, bool queueDisk);
    bool loadFromDisk(const ImageCacheKey& key, std::shared_ptr<DecodedImage>& out);
    void finishFocusAndBackdrop(const ImageCacheKey& key, DecodedImage& img);
    void logSrc(const ImageCacheKey& key, const char* src, long long ms);
    static int envWorkers();

    HttpGetFn get_;
    std::function<void(const ImageCacheKey& key)> onArrival_;
    std::function<void(const std::string&)> logLine_;
    PlexUiDiskCache disk_;
    std::size_t maxBytes_;
    mutable std::mutex mu_;
    std::unordered_map<ImageCacheKey, std::shared_ptr<const DecodedImage>, ImageCacheKeyHash> map_;
    mutable std::deque<ImageCacheKey> lru_;
    std::unordered_set<ImageCacheKey, ImageCacheKeyHash> pinned_;
    std::size_t bytes_ = 0;
    std::priority_queue<Job, std::vector<Job>, JobCmp> queue_;
    std::unordered_map<ImageCacheKey, Job, ImageCacheKeyHash> queued_;
    std::unordered_set<ImageCacheKey, ImageCacheKeyHash> inFlight_;
    uint64_t viewGen_ = 0;
    uint64_t seq_ = 0;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> inFlightCount_{0};
    std::atomic<int> srcRam_{0};
    std::atomic<int> srcDisk_{0};
    std::atomic<int> srcNet_{0};
    std::condition_variable cv_;
    bool budgetLogged_ = false;
    std::size_t layerBytes_ = 0;
    std::size_t inflightBytes_ = 0;
    std::size_t inflightJobs_ = 0;
    uint64_t evictions_ = 0, refused_ = 0, pinnedEvictions_ = 0;
    std::atomic<std::size_t> peakTotal_{0};
    std::thread statsThread_;
    std::condition_variable statsCv_;
    std::size_t memAvailKb_ = 0;
};

} // namespace plexui
} // namespace misterplex
