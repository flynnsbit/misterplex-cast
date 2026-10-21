#include <atomic>
#include "plexui_images.hpp"

#include "plexui_http.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#include "third_party/stb_image_resize2.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace misterplex {
namespace plexui {

namespace {

constexpr float kFocusScale = 1.08f;
// OB port: set by the controller (setImageGeometry) and read by the decode workers.
std::atomic<int> gScreenW{640}, gScreenH{480}, gCornerR{8}, gHubW{96}, gHubH{144};
constexpr int kBackdropScreenW = 640;
constexpr int kBackdropScreenH = 480;
bool r1Geometry() { return gScreenW.load() == 640 && gScreenH.load() == 480; }

void boxBlurRgb(std::uint8_t* data, int w, int h, int stride, std::uint8_t* tmp) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= h)
                    continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= w)
                        continue;
                    const std::uint8_t* p = data + (yy * stride + xx * 3);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++n;
                }
            }
            std::uint8_t* o = tmp + (y * stride + x * 3);
            o[0] = static_cast<std::uint8_t>(r / n);
            o[1] = static_cast<std::uint8_t>(g / n);
            o[2] = static_cast<std::uint8_t>(b / n);
        }
    }
    std::memcpy(data, tmp, static_cast<std::size_t>(stride) * h);
}

void buildArtBackdrop(const DecodedImage& src, std::vector<std::uint8_t>& backdropOut) {
    if (src.w < 4 || src.h < 4)
        return;
    constexpr int bw = 160;
    constexpr int bh = 90;
    std::vector<std::uint8_t> small(static_cast<std::size_t>(bw) * bh * 3);
    stbir_resize_uint8_linear(src.rgba.data(), src.w, src.h, 0, small.data(), bw, bh, 0, STBIR_RGB);
    std::vector<std::uint8_t> blurTmp(static_cast<std::size_t>(bw) * bh * 3);
    for (int pass = 0; pass < 3; ++pass)
        boxBlurRgb(small.data(), bw, bh, bw * 3, blurTmp.data());
    std::vector<std::uint8_t> full(static_cast<std::size_t>(kBackdropScreenW) * kBackdropScreenH * 3);
    stbir_resize_uint8_linear(small.data(), bw, bh, 0, full.data(), kBackdropScreenW, kBackdropScreenH,
                              0, STBIR_RGB);
    backdropOut.resize(static_cast<std::size_t>(kBackdropScreenW) * kBackdropScreenH * 4);
    for (int y = 0; y < kBackdropScreenH; ++y) {
        const float vGrad = static_cast<float>(y) / static_cast<float>(kBackdropScreenH);
        const float bottomDark = 0.25f + 0.75f * vGrad;
        for (int x = 0; x < kBackdropScreenW; ++x) {
            const std::uint8_t* s =
                full.data() + (static_cast<std::size_t>(y) * kBackdropScreenW + x) * 3;
            std::uint8_t* d =
                backdropOut.data() + (static_cast<std::size_t>(y) * kBackdropScreenW + x) * 4;
            const float dim = 0.62f * bottomDark;
            d[0] = static_cast<std::uint8_t>(s[0] * dim);
            d[1] = static_cast<std::uint8_t>(s[1] * dim);
            d[2] = static_cast<std::uint8_t>(s[2] * dim);
            d[3] = 255;
        }
    }
}

bool roundedCornerSkip(int cx, int cy, int w, int h, int radius) {
    if (cx >= radius && cx < w - radius)
        return false;
    if (cy >= radius && cy < h - radius)
        return false;
    int rcx = cx, rcy = cy;
    if (cx < radius)
        rcx = radius;
    else if (cx >= w - radius)
        rcx = w - radius - 1;
    if (cy < radius)
        rcy = radius;
    else if (cy >= h - radius)
        rcy = h - radius - 1;
    const int dx = cx - rcx;
    const int dy = cy - rcy;
    return dx * dx + dy * dy > radius * radius;
}

void applyCornerMask(int w, int h, std::vector<std::uint8_t>& rgba) {
    const int radius = gCornerR.load();
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if (roundedCornerSkip(col, row, w, h, radius))
                rgba[(static_cast<std::size_t>(row) * w + col) * 4 + 3] = 0;
        }
    }
}

int parseTranscodeDim(const std::string& url, const char* key) {
    const std::string needle = std::string(key) + "=";
    const size_t p = url.find(needle);
    if (p == std::string::npos)
        return 0;
    return std::atoi(url.c_str() + p + needle.size());
}

std::size_t envSizeMb(const char* name, std::size_t defaultMb) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw)
        return defaultMb;
    const int v = std::atoi(raw);
    return v > 0 ? static_cast<std::size_t>(v) : defaultMb;
}

} // namespace

std::size_t ImageCacheKeyHash::operator()(const ImageCacheKey& k) const {
    std::size_t h = std::hash<std::string>{}(k.machineIdentifier);
    h ^= std::hash<std::string>{}(k.ratingKey) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<std::size_t>(k.kind) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<std::size_t>(k.w) << 16 ^ static_cast<std::size_t>(k.h);
    h ^= std::hash<std::string>{}(k.updatedAt) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

std::string imageCacheKeyString(const ImageCacheKey& k) {
    return k.machineIdentifier + "|" + k.ratingKey + "|" +
           std::to_string(static_cast<int>(k.kind)) + "|" + std::to_string(k.w) + "x" +
           std::to_string(k.h) + "|" + k.updatedAt;
}

std::string parseImagePathUpdatedAt(const std::string& path) {
    const char* markers[] = {"/thumb/", "/art/"};
    for (const char* marker : markers) {
        const std::size_t p = path.find(marker);
        if (p == std::string::npos)
            continue;
        const std::size_t start = p + std::strlen(marker);
        const std::size_t end = path.find('/', start);
        return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    return {};
}

std::string itemThumbRevision(const PlexItem& it) {
    const std::string fromPath = parseImagePathUpdatedAt(it.thumb);
    return !fromPath.empty() ? fromPath : it.updatedAt;
}

std::string itemArtRevision(const PlexItem& it) {
    const std::string fromArt = parseImagePathUpdatedAt(it.art);
    if (!fromArt.empty())
        return fromArt;
    const std::string fromThumb = parseImagePathUpdatedAt(it.thumb);
    if (!fromThumb.empty())
        return fromThumb;
    return it.updatedAt;
}

ImageCacheKey thumbCacheKey(const std::string& machineId, const std::string& ratingKey, int w,
                            int h, const std::string& updatedAt) {
    ImageCacheKey k;
    k.machineIdentifier = machineId;
    k.ratingKey = ratingKey;
    k.kind = ImageKind::Thumb;
    k.w = w;
    k.h = h;
    k.updatedAt = updatedAt;
    return k;
}

ImageCacheKey artCacheKey(const std::string& machineId, const std::string& ratingKey, int w, int h,
                          const std::string& updatedAt) {
    ImageCacheKey k;
    k.machineIdentifier = machineId;
    k.ratingKey = ratingKey;
    k.kind = ImageKind::Art;
    k.w = w;
    k.h = h;
    k.updatedAt = updatedAt;
    return k;
}

std::size_t decodedImageBytes(int w, int h) {
    const int fw = static_cast<int>(w * kFocusScale);
    const int fh = static_cast<int>(h * kFocusScale);
    std::size_t b = static_cast<std::size_t>(w) * h * 4u;
    if (fw > 0 && fh > 0 && static_cast<std::size_t>(fw) * fh * 4u <= 256u * 1024u)
        b += static_cast<std::size_t>(fw) * fh * 4u;
    return b;
}

namespace {

constexpr int kBackdropFinalW = 640;
constexpr int kBackdropFinalH = 480;

// R1 keeps a prebuilt 640x480 backdrop per art item; other canvases blur at paint time (a
// 1280x720 backdrop per hub item would be 3.7 MB each and thrash the RAM tier).
std::size_t hubArtSlotBytes() {
    return decodedImageBytes(gHubW.load(), gHubH.load()) +
           (r1Geometry() ? static_cast<std::size_t>(kBackdropFinalW) * kBackdropFinalH * 4u : 0u);
}

} // namespace

std::size_t computeImageCacheBudgetBytes(int hubItemCount, int gridPosterW, int gridPosterH,
                                         int gridTileSlots) {
    const std::size_t home = static_cast<std::size_t>(hubItemCount) * hubArtSlotBytes();
    const std::size_t gridPoster = decodedImageBytes(gridPosterW, gridPosterH);
    const std::size_t grid = static_cast<std::size_t>(gridTileSlots) * gridPoster;
    return home + grid;
}

std::size_t computeRamCacheBudgetBytes(int hubItemCount, int gridPosterW, int gridPosterH,
                                       int gridVisibleRows, int gridCols) {
    const std::size_t overrideMb = envSizeMb("MPX_PLEXUI_RAM_CACHE_MB", 0);
    if (overrideMb > 0)
        return overrideMb * 1024u * 1024u;

    const int pageRows = std::max(1, gridVisibleRows + 1);
    const int slotsPerPage = pageRows * std::max(1, gridCols);
    const std::size_t home = static_cast<std::size_t>(hubItemCount) * hubArtSlotBytes();
    const std::size_t gridPoster = decodedImageBytes(gridPosterW, gridPosterH);
    const std::size_t grid = 3u * static_cast<std::size_t>(slotsPerPage) * gridPoster;
    std::size_t budget = home + grid;
    budget += budget / 5u;

    constexpr std::size_t kMinBytes = 32u * 1024u * 1024u;
    constexpr std::size_t kMaxBytes = 96u * 1024u * 1024u;
    budget = std::max(kMinBytes, budget);

    std::size_t memCap = kMaxBytes;
    const std::size_t memKb = readMemAvailableKb();
    if (memKb > 0)
        memCap = std::min(kMaxBytes, (memKb / 3) * 1024u);
    return std::min(budget, memCap);
}

void setImageGeometry(int screenW, int screenH, int cornerRadius, int hubPosterW, int hubPosterH) {
    gScreenW.store(screenW);
    gScreenH.store(screenH);
    gCornerR.store(std::max(1, cornerRadius));
    gHubW.store(hubPosterW);
    gHubH.store(hubPosterH);
}

int ImageCache::envWorkers() {
    const char* raw = std::getenv("MPX_PLEXUI_IMG_WORKERS");
    if (!raw || !*raw)
        return 3;
    const int v = std::atoi(raw);
    return v > 0 ? v : 3;
}

ImageCache::ImageCache(std::size_t maxBytes)
    : maxBytes_(maxBytes > 0 ? maxBytes : 32u * 1024u * 1024u) {
    memAvailKb_ = readMemAvailableKb();
}

ImageCache::~ImageCache() { stop(); }

void ImageCache::setMaxBytes(std::size_t maxBytes) {
    std::lock_guard<std::mutex> lk(mu_);
    maxBytes_ = maxBytes > 0 ? maxBytes : maxBytes_;
    // 0012: a lowered budget (MemAvailable/3 falls as YouTube-on-OB eats RAM) is enforced NOW.
    // Before, the art already held stayed (soak G7: imgcache_bytes=29180700/18965504).
#ifndef MUT_PLEXUI_NO_TRIM_ON_LOWER
    evictFor(0);
#endif
}

void ImageCache::setLayerBytes(std::size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    layerBytes_ = bytes;
    evictFor(0);
}

std::size_t ImageCache::totalLocked() const {
    return bytes_ + layerBytes_ + inflightBytes_ + disk_.queuedBytes();
}

std::size_t ImageCache::totalBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return totalLocked();
}

void ImageCache::notePeakLocked() {
    const std::size_t t = totalLocked();
    std::size_t p = peakTotal_.load();
    while (t > p && !peakTotal_.compare_exchange_weak(p, t)) {
    }
}

bool ImageCache::reserveLocked(std::size_t bytes) {
    evictFor(bytes);
    if (totalLocked() + bytes > maxBytes_) {
        ++refused_;
        return false;
    }
    inflightBytes_ += bytes;
    ++inflightJobs_;
    notePeakLocked();
    return true;
}

void ImageCache::releaseLocked(std::size_t bytes) {
    inflightBytes_ -= std::min(bytes, inflightBytes_);
    if (inflightJobs_ > 0)
        --inflightJobs_;
}

ImageCache::MemStats ImageCache::memStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    MemStats m;
    m.artBytes = bytes_;
    m.artEntries = map_.size();
    m.layerBytes = layerBytes_;
    m.inflightBytes = inflightBytes_;
    m.inflight = inflightJobs_;
    m.diskqBytes = disk_.queuedBytes();
    m.total = totalLocked();
    m.budget = maxBytes_;
    m.evictions = evictions_;
    m.refused = refused_;
    m.pinnedEvictions = pinnedEvictions_;
    return m;
}

std::string ImageCache::memStatsLine() const {
    const MemStats m = memStats();
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "plexui: mem total=%zu/%zu art_bytes=%zu art_entries=%zu layer_bytes=%zu "
                  "inflight_bytes=%zu inflight=%zu diskq_bytes=%zu evictions=%llu "
                  "pinned_evictions=%llu refused=%llu",
                  m.total, m.budget, m.artBytes, m.artEntries, m.layerBytes, m.inflightBytes,
                  m.inflight, m.diskqBytes, static_cast<unsigned long long>(m.evictions),
                  static_cast<unsigned long long>(m.pinnedEvictions),
                  static_cast<unsigned long long>(m.refused));
    return buf;
}

void ImageCache::statsLoop() {
    const char* raw = std::getenv("MPX_PLEXUI_MEM_STATS_SEC");
    const int sec = raw && *raw && std::atoi(raw) > 0 ? std::atoi(raw) : 60;
    std::unique_lock<std::mutex> lk(mu_);
    while (!stopping_) {
        statsCv_.wait_for(lk, std::chrono::seconds(sec), [this] { return stopping_.load(); });
        if (stopping_)
            break;
        lk.unlock();
        if (logLine_)
            logLine_(memStatsLine());
        lk.lock();
    }
}

std::size_t ImageCache::maxBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return maxBytes_;
}

void ImageCache::logBudgetOnce(std::function<void(const std::string&)> logLine) {
    std::lock_guard<std::mutex> lk(mu_);
    if (budgetLogged_ || !logLine)
        return;
    budgetLogged_ = true;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "plexui: imgcache ram_budget_mb=%zu ram_budget_bytes=%zu memavail_kb=%zu",
                  maxBytes_ / (1024u * 1024u), maxBytes_, memAvailKb_);
    logLine(buf);
}

void ImageCache::start(HttpGetFn get, std::function<void(const ImageCacheKey& key)> onArrival,
                       std::function<void(const std::string&)> logLine) {
    get_ = std::move(get);
    onArrival_ = std::move(onArrival);
    logLine_ = std::move(logLine);
    stopping_ = false;
    disk_.start();
    const int n = envWorkers();
    workers_.clear();
    for (int i = 0; i < n; ++i)
        workers_.emplace_back([this, i] { workerLoop(i); });
    statsThread_ = std::thread([this] { statsLoop(); });
}

void ImageCache::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    statsCv_.notify_all();
    if (statsThread_.joinable())
        statsThread_.join();
    for (auto& t : workers_) {
        if (t.joinable())
            t.join();
    }
    workers_.clear();
    disk_.stop();
}

void ImageCache::beginView(uint64_t viewGen) {
    std::lock_guard<std::mutex> lk(mu_);
    viewGen_ = viewGen;
    std::priority_queue<Job, std::vector<Job>, JobCmp> kept;
    while (!queue_.empty()) {
        Job j = queue_.top();
        queue_.pop();
        if (j.viewGen >= viewGen_)
            kept.push(j);
        else
            queued_.erase(j.key);
    }
    queue_ = std::move(kept);
}

void ImageCache::setPinned(const std::vector<ImageCacheKey>& keys) {
    std::lock_guard<std::mutex> lk(mu_);
    pinned_.clear();
    for (const auto& k : keys)
        pinned_.insert(k);
}

void ImageCache::request(const ImageCacheKey& key, const std::string& fetchUrl, uint64_t viewGen,
                         int priority) {
    if (fetchUrl.empty() || key.ratingKey.empty())
        return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (viewGen < viewGen_)
            return;
        if (map_.count(key) || inFlight_.count(key))
            return;
        const auto qit = queued_.find(key);
        if (qit != queued_.end()) {
            if (qit->second.priority <= priority)
                return;
            queued_.erase(qit);
        }
        Job j;
        j.key = key;
        j.url = fetchUrl;
        j.viewGen = viewGen;
        j.priority = priority;
        j.seq = ++seq_;
        j.enqueued = std::chrono::steady_clock::now();
        queue_.push(j);
        queued_[key] = j;
    }
    cv_.notify_one();
}

bool ImageCache::busy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return inFlightCount_.load() > 0 || !queue_.empty();
}

std::shared_ptr<const DecodedImage> ImageCache::get(const ImageCacheKey& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(key);
    if (it == map_.end())
        return {};
    touchLru(key);
    return it->second;
}

ImageCache::SrcStats ImageCache::srcStats() const {
    SrcStats s;
    s.ram = srcRam_.load();
    s.disk = srcDisk_.load();
    s.net = srcNet_.load();
    return s;
}

std::size_t ImageCache::bytesUsed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes_;
}

void ImageCache::touchLru(const ImageCacheKey& key) const {
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
        if (*it == key) {
            lru_.erase(it);
            break;
        }
    }
    lru_.push_back(key);
}

void ImageCache::evictFor(std::size_t need) {
    // 0012: the budget is global (art + view layers + in-flight + disk queue). Unpinned art goes
    // first. A hard shrink (need == 0) also drops pins so a lowered MemAvailable cap holds.
    // An arriving decode (need > 0) does not: refusing it beats evicting a visible poster and
    // immediately re-fetching it.
    while (totalLocked() + need > maxBytes_ && !lru_.empty()) {
        ImageCacheKey evictKey{};
        bool found = false;
        for (auto it = lru_.begin(); it != lru_.end(); ++it) {
            if (pinned_.count(*it))
                continue;
            evictKey = *it;
            lru_.erase(it);
            found = true;
            break;
        }
#ifndef MUT_PLEXUI_PINS_OVER_BUDGET
        if (!found) {
            // A decode or in-flight reserve (need > 0) must not unpin the visible row.
            // Evicting a pin to admit the next poster makes onArrival re-request the
            // one just dropped, which evicts the next: a fetch loop while the grid
            // is larger than the budget. Refuse that decode instead. need == 0 is a
            // hard shrink (setMaxBytes / setLayerBytes) and still drops pins.
#ifndef MUT_PLEXUI_EVICT_PINS_FOR_INFLIGHT
            if (need > 0)
                break;
#endif
            evictKey = lru_.front();
            lru_.pop_front();
            found = true;
            ++pinnedEvictions_;
        }
#endif
        if (!found)
            break;
        ++evictions_;
        const auto it = map_.find(evictKey);
        if (it != map_.end()) {
            bytes_ -= imageFootprint(*it->second);
            map_.erase(it);
        }
    }
}

std::size_t ImageCache::imageFootprint(const DecodedImage& img) const {
    return img.rgba.size() + img.focusRgba.size() + img.backdropRgba.size();
}

void ImageCache::insertImage(const ImageCacheKey& key, std::shared_ptr<DecodedImage> img,
                             bool queueDisk) {
    const std::size_t need = imageFootprint(*img);
    evictFor(need);
    if (totalLocked() + need > maxBytes_) {
        ++refused_;
        return;
    }
    map_[key] = img;
    touchLru(key);
    bytes_ += need;
    notePeakLocked();
    // The disk queue copies the RGBA: only when that copy fits the budget too.
    if (queueDisk && disk_.enabled() && !img->rgba.empty() &&
        totalLocked() + img->rgba.size() <= maxBytes_)
        disk_.queueWrite(imageCacheKeyString(key), img->w, img->h, img->rgba.data(), img->rgba.size());
}

void ImageCache::finishFocusAndBackdrop(const ImageCacheKey& key, DecodedImage& img) {
    const int outW = img.w;
    const int outH = img.h;
    if (key.kind == ImageKind::Thumb) {
        applyCornerMask(outW, outH, img.rgba);
        const int fw = static_cast<int>(outW * kFocusScale);
        const int fh = static_cast<int>(outH * kFocusScale);
        if (fw > 0 && fh > 0 &&
            static_cast<std::size_t>(fw) * fh * 4u <= 256u * 1024u) {
            img.focusW = fw;
            img.focusH = fh;
            img.focusRgba.resize(static_cast<std::size_t>(fw) * fh * 4);
            stbir_resize_uint8_linear(img.rgba.data(), outW, outH, 0, img.focusRgba.data(), fw, fh,
                                      0, STBIR_RGBA);
            applyCornerMask(fw, fh, img.focusRgba);
        }
    } else if (key.kind == ImageKind::Art && r1Geometry())
        buildArtBackdrop(img, img.backdropRgba);
}

bool ImageCache::loadFromDisk(const ImageCacheKey& key, std::shared_ptr<DecodedImage>& out) {
    if (!disk_.enabled())
        return false;
    auto img = std::make_shared<DecodedImage>();
    if (!disk_.loadRgba(imageCacheKeyString(key), key.w, key.h, img->rgba))
        return false;
    img->w = key.w;
    img->h = key.h;
    finishFocusAndBackdrop(key, *img);
    out = std::move(img);
    return true;
}

void ImageCache::logSrc(const ImageCacheKey& key, const char* src, long long ms) {
    if (src[0] == 'r' && src[1] == 'a')
        srcRam_.fetch_add(1);
    else if (src[0] == 'd')
        srcDisk_.fetch_add(1);
    else
        srcNet_.fetch_add(1);
    if (!logLine_)
        return;
    char buf[200];
    std::snprintf(buf, sizeof(buf), "plexui: img src=%s ms=%lld key=%s", src,
                  static_cast<long long>(ms), imageCacheKeyString(key).c_str());
    logLine_(buf);
}

void ImageCache::workerLoop(int workerId) {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty())
                return;
            job = queue_.top();
            queue_.pop();
            queued_.erase(job.key);
            if (job.viewGen < viewGen_) {
                lk.unlock();
                continue;
            }
            if (map_.count(job.key) || inFlight_.count(job.key))
                continue;
            inFlight_.insert(job.key);
        }
        const auto workStart = std::chrono::steady_clock::now();
        const long long queueMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      workStart - job.enqueued)
                                      .count();

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (map_.count(job.key)) {
                inFlight_.erase(job.key);
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - workStart)
                                         .count();
                logSrc(job.key, "ram", ms);
                cv_.notify_all();
                continue;
            }
        }

        // 0012: a disk read-back holds the QOI file + the decode + the copy until insert.
        const std::size_t diskReserve =
            job.key.w > 0 && job.key.h > 0 ? decodedImageBytes(job.key.w, job.key.h) * 3u : 0u;
        bool diskReserved = false;
        if (disk_.enabled() && diskReserve > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            diskReserved = reserveLocked(diskReserve);
            if (!diskReserved) {
                inFlight_.erase(job.key);
                cv_.notify_all();
                continue;
            }
        }
        std::shared_ptr<DecodedImage> diskImg;
        const bool diskHit = loadFromDisk(job.key, diskImg);
        if (diskReserved) {
            std::lock_guard<std::mutex> lk(mu_);
            releaseLocked(diskReserve);
        }
        if (diskHit) {
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - workStart)
                                     .count();
            {
                std::lock_guard<std::mutex> lk(mu_);
                inFlight_.erase(job.key);
                if (!map_.count(job.key))
                    insertImage(job.key, diskImg, false);
            }
            logSrc(job.key, "disk", ms);
            if (onArrival_)
                onArrival_(job.key);
            cv_.notify_all();
            continue;
        }

        if (!get_) {
            std::lock_guard<std::mutex> lk(mu_);
            inFlight_.erase(job.key);
            continue;
        }

        inFlightCount_.fetch_add(1);
        const BrowseHttpResult r =
            get_(job.url, 4000, 15000, [this] { return stopping_.load(); });
        const auto fetchDone = std::chrono::steady_clock::now();
        inFlightCount_.fetch_sub(1);

        const long long fetchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      fetchDone - workStart)
                                      .count();
        const std::string host = httpUrlHost(job.url);

        if (!r.ok() || r.body.empty()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                inFlight_.erase(job.key);
            }
            if (logLine_) {
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                              "plexui: img queue_ms=%lld fetch_ms=%lld decode_ms=0 worker=%d "
                              "http=%d host=%s fail curl=%d key=%s",
                              queueMs, fetchMs, workerId, r.status, host.c_str(), r.curlExit,
                              imageCacheKeyString(job.key).c_str());
                logLine_(buf);
            }
            cv_.notify_all();
            continue;
        }

        const auto decodeStart = std::chrono::steady_clock::now();
        int w = 0, h = 0, comp = 0;
        // 0012: evict before allocating. Reserve the decode (stbi buffer + output copy + focus
        // copy) from the header; refuse when it does not fit (the next view re-requests it).
        std::size_t decodeReserve = 0;
        {
            int iw = 0, ih = 0, ic = 0;
            if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(r.body.data()),
                                       static_cast<int>(r.body.size()), &iw, &ih, &ic) ||
                iw <= 0 || ih <= 0 || static_cast<long long>(iw) * ih > 4096LL * 4096LL)
                iw = ih = 0;
            const int tw0 = parseTranscodeDim(job.url, "width");
            const int th0 = parseTranscodeDim(job.url, "height");
            decodeReserve = decodedImageBytes(iw, ih) +
                            2u * decodedImageBytes(tw0 > 0 ? tw0 : iw, th0 > 0 ? th0 : ih);
            std::unique_lock<std::mutex> lk(mu_);
            if (iw == 0 || !reserveLocked(decodeReserve)) {
                inFlight_.erase(job.key);
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                              "plexui: img refused need=%zu total=%zu/%zu key=%s", decodeReserve,
                              totalLocked(), maxBytes_, imageCacheKeyString(job.key).c_str());
                lk.unlock();
                if (logLine_)
                    logLine_(buf);
                cv_.notify_all();
                continue;
            }
        }
        stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(r.body.data()),
                                            static_cast<int>(r.body.size()), &w, &h, &comp, 4);
        if (!px) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                inFlight_.erase(job.key);
                releaseLocked(decodeReserve);
            }
            if (logLine_) {
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "plexui: img queue_ms=%lld fetch_ms=%lld decode_ms=0 worker=%d "
                              "host=%s fail decode key=%s",
                              queueMs, fetchMs, workerId, host.c_str(),
                              imageCacheKeyString(job.key).c_str());
                logLine_(buf);
            }
            cv_.notify_all();
            continue;
        }

        const int tw = parseTranscodeDim(job.url, "width");
        const int th = parseTranscodeDim(job.url, "height");
        const int outW = tw > 0 ? tw : w;
        const int outH = th > 0 ? th : h;

        auto img = std::make_shared<DecodedImage>();
        img->w = outW;
        img->h = outH;
        img->rgba.resize(static_cast<std::size_t>(outW) * outH * 4);
        if (w == outW && h == outH)
            std::memcpy(img->rgba.data(), px, img->rgba.size());
        else
            stbir_resize_uint8_linear(px, w, h, 0, img->rgba.data(), outW, outH, 0, STBIR_RGBA);
        stbi_image_free(px);
        finishFocusAndBackdrop(job.key, *img);

        const auto decodeDone = std::chrono::steady_clock::now();
        const long long decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       decodeDone - decodeStart)
                                       .count();
        const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      decodeDone - workStart)
                                      .count();

        if (logLine_) {
            char buf[260];
            std::snprintf(buf, sizeof(buf),
                          "plexui: img queue_ms=%lld fetch_ms=%lld decode_ms=%lld worker=%d "
                          "bytes=%zu w=%d h=%d host=%s key=%s",
                          queueMs, fetchMs, decodeMs, workerId, r.body.size(), outW, outH,
                          host.c_str(), imageCacheKeyString(job.key).c_str());
            logLine_(buf);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            inFlight_.erase(job.key);
            releaseLocked(decodeReserve);
            if (!map_.count(job.key))
                insertImage(job.key, img, true);
        }
        logSrc(job.key, "net", totalMs);
        if (onArrival_)
            onArrival_(job.key);
        cv_.notify_all();
    }
}

#ifdef MPX_PLEXUI_UNIT_TEST
// rc3 port of d255d85b2 onto the keyed cache (cc320067): seed a decoded image by its cache key.
void ImageCache::testInsertDecoded(const ImageCacheKey& key, int w, int h, std::uint8_t fill) {
    if (w <= 0 || h <= 0)
        return;
    auto img = std::make_shared<DecodedImage>();
    img->w = w;
    img->h = h;
    img->rgba.assign(static_cast<std::size_t>(w) * h * 4, fill);
    std::lock_guard<std::mutex> lk(mu_);
    if (map_.count(key))
        return;
    insertImage(key, std::move(img), false);
}

bool ImageCache::testAdmitInflight(std::size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!reserveLocked(bytes))
        return false;
    releaseLocked(bytes);
    return true;
}
#endif

} // namespace plexui
} // namespace misterplex
