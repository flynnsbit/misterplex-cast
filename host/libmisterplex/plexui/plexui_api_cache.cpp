#include "plexui_api_cache.hpp"

#include <utility>

namespace misterplex {
namespace plexui {

PlexUiApiCache::~PlexUiApiCache() { drainRefreshes(); }

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
    std::lock_guard<std::mutex> lk(refreshMu_);
    refreshes_.emplace_back([this, cacheKey, fetchUrl, get, connectMs, timeoutMs, cancel] {
        const BrowseHttpResult r = get(fetchUrl, connectMs, timeoutMs, cancel);
        if (r.ok() && !r.body.empty())
            put(cacheKey, r.body);
    });
}

void PlexUiApiCache::drainRefreshes() {
    std::vector<std::thread> mine;
    {
        std::lock_guard<std::mutex> lk(refreshMu_);
        mine.swap(refreshes_);
    }
    for (auto& t : mine) {
        if (t.joinable())
            t.join();
    }
}

#ifdef MPX_PLEXUI_UNIT_TEST
void PlexUiApiCache::testClear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
}
#endif

} // namespace plexui
} // namespace misterplex
