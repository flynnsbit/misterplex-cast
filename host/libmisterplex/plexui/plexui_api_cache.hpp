#pragma once

#include "libmisterplex/browse_fetch.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace misterplex {
namespace plexui {

using HttpGetFn = std::function<BrowseHttpResult(const std::string& url, int connectMs,
                                                 int timeoutMs, const BrowseCancelFn&)>;

class PlexUiApiCache {
public:
    bool get(const std::string& key, std::string& bodyOut);
    void put(const std::string& key, const std::string& body);
    void refreshAsync(const std::string& cacheKey, const std::string& fetchUrl, HttpGetFn get,
                      int connectMs, int timeoutMs, const BrowseCancelFn& cancel);

#ifdef MPX_PLEXUI_UNIT_TEST
    void testClear();
    // Wait until detached metadata refreshes have finished. Tests join this way
    // so a refresh cannot outlive the hook that it calls.
    bool testDrainRefreshes(int timeoutMs = 2000);
#endif

private:
    struct Entry {
        std::string body;
        std::chrono::steady_clock::time_point expires{};
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    static constexpr int kTtlSec = 120;
#ifdef MPX_PLEXUI_UNIT_TEST
    std::atomic<int> refreshLive_{0};
#endif
};

} // namespace plexui
} // namespace misterplex
