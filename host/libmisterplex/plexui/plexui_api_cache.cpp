#include "plexui_api_cache.hpp"

#include <chrono>
#include <thread>

namespace misterplex {
namespace plexui {

bool PlexUiApiCache::get(const std::string& key, std::string& bodyOut) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(key);
    if (it == map_.end())
        return false;
    if (std::chrono::steady_clock::now() >= it->second.expires) {
        map_.erase(it);
        return false;
    }
    bodyOut = it->second.body;
    return true;
}

void PlexUiApiCache::put(const std::string& key, const std::string& body) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry e;
    e.body = body;
    e.expires = std::chrono::steady_clock::now() + std::chrono::seconds(kTtlSec);
    map_[key] = std::move(e);
}

void PlexUiApiCache::refreshAsync(const std::string& cacheKey, const std::string& fetchUrl,
                                  HttpGetFn get, int connectMs, int timeoutMs,
                                  const BrowseCancelFn& cancel) {
#ifdef MPX_PLEXUI_UNIT_TEST
    refreshLive_.fetch_add(1, std::memory_order_acq_rel);
#endif
    std::thread([this, cacheKey, fetchUrl, get, connectMs, timeoutMs, cancel] {
#ifdef MPX_PLEXUI_UNIT_TEST
        struct Guard {
            std::atomic<int>& live;
            ~Guard() { live.fetch_sub(1, std::memory_order_acq_rel); }
        } guard{refreshLive_};
#endif
        const BrowseHttpResult r = get(fetchUrl, connectMs, timeoutMs, cancel);
        if (r.ok() && !r.body.empty())
            put(cacheKey, r.body);
    }).detach();
}

#ifdef MPX_PLEXUI_UNIT_TEST
void PlexUiApiCache::testClear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
}

bool PlexUiApiCache::testDrainRefreshes(int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (refreshLive_.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
#endif

} // namespace plexui
} // namespace misterplex
