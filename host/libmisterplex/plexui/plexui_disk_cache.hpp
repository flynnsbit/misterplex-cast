#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace misterplex {
namespace plexui {

struct DiskCacheWriteJob {
    std::string keyDigest;
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
};

class PlexUiDiskCache {
public:
    PlexUiDiskCache();
    ~PlexUiDiskCache();

    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    void start();
    void stop();

    bool loadRgba(const std::string& cacheKey, int w, int h, std::vector<std::uint8_t>& rgbaOut);
    void queueWrite(const std::string& cacheKey, int w, int h, const std::uint8_t* rgba,
                    std::size_t rgbaBytes);

    static std::string sha1Hex(const std::string& s);

private:
    void writerLoop();
    void evictToCap();
public:
    std::size_t queuedBytes() const;   // 0012: RGBA copies waiting in the write queue
private:
    std::size_t dirBytesOnDisk() const;
    bool writeFileAtomic(const std::string& path, const std::vector<std::uint8_t>& qoi);
    static std::string filePathFor(const std::string& dir, const std::string& digest);

    std::string dir_;
    std::size_t maxBytes_ = 0;
    mutable std::mutex mu_;
    std::vector<DiskCacheWriteJob> writeQ_;
    std::size_t queuedBytes_ = 0;   // queued + the job being encoded
    std::size_t writeQMax_ = 32;
    std::thread writer_;
    std::atomic<bool> stopping_{false};   // 0012: written by stop(), read by the writer (TSan)
};

std::size_t readMemAvailableKb();

} // namespace plexui
} // namespace misterplex
