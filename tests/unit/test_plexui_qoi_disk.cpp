#include "plexui_disk_cache.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace misterplex::plexui;

static void testDiskRoundTrip() {
    const std::string dir = std::string("/tmp/plexui-disk-rt-") + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    setenv("MPX_PLEXUI_DISK_CACHE", dir.c_str(), 1);
    setenv("MPX_PLEXUI_DISK_CACHE_MB", "8", 1);
    PlexUiDiskCache cache;
    cache.start();
    constexpr int w = 17;
    constexpr int h = 11;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = static_cast<std::uint8_t>(i & 255);
    const std::string key = "roundtrip-key";
    cache.queueWrite(key, w, h, rgba.data(), rgba.size());
    std::vector<std::uint8_t> loaded;
    for (int i = 0; i < 100; ++i) {
        if (cache.loadRgba(key, w, h, loaded))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(loaded == rgba);
    cache.stop();
    std::filesystem::remove_all(dir);
    unsetenv("MPX_PLEXUI_DISK_CACHE");
    unsetenv("MPX_PLEXUI_DISK_CACHE_MB");
}

static void testDiskEviction() {
    const std::string dir = std::string("/tmp/plexui-disk-test-") + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    setenv("MPX_PLEXUI_DISK_CACHE", dir.c_str(), 1);
    setenv("MPX_PLEXUI_DISK_CACHE_MB", "1", 1);
    PlexUiDiskCache cache;
    assert(cache.enabled());
    cache.start();
    constexpr int w = 8;
    constexpr int h = 8;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 200);
    for (int i = 0; i < 80; ++i) {
        const std::string key = "key-" + std::to_string(i);
        cache.queueWrite(key, w, h, px.data(), px.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    int qoiCount = 0;
    for (const auto& ent : std::filesystem::directory_iterator(dir)) {
        if (ent.path().extension() == ".qoi")
            ++qoiCount;
    }
    assert(qoiCount > 0 && qoiCount < 80);
    cache.stop();
    std::filesystem::remove_all(dir);
    unsetenv("MPX_PLEXUI_DISK_CACHE");
    unsetenv("MPX_PLEXUI_DISK_CACHE_MB");
}

int main() {
    testDiskRoundTrip();
    testDiskEviction();
    std::printf("test_plexui_qoi_disk ok\n");
    return 0;
}
