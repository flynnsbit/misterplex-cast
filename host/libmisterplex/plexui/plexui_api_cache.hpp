#pragma once

#include "libmisterplex/browse_fetch.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace misterplex {
namespace plexui {

using HttpGetFn = std::function<BrowseHttpResult(const std::string& url, int connectMs,
                                                 int timeoutMs, const BrowseCancelFn&)>;

class PlexUiApiCache {
public:
    PlexUiApiCache() = default;
    ~PlexUiApiCache();
    PlexUiApiCache(const PlexUiApiCache&) = delete;
    PlexUiApiCache& operator=(const PlexUiApiCache&) = delete;

    bool get(const std::string& key, std::string& bodyOut);
    void put(const std::string& key, const std::string& body);
    void refreshAsync(const std::string& cacheKey, const std::string& fetchUrl, HttpGetFn get,
                      int connectMs, int timeoutMs, const BrowseCancelFn& cancel);
    // Join every metadata refresh. Safe to call more than once.
    void drainRefreshes();

#ifdef MPX_PLEXUI_UNIT_TEST
    void testClear();
#endif

private:
    struct Entry {
        std::string body;
        std::chrono::steady_clock::time_point expires{};
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    static constexpr int kTtlSec = 120;
    std::mutex refreshMu_;
    std::vector<std::thread> refreshes_;
};

} // namespace plexui
} // namespace misterplex
