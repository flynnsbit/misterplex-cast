#include "plexui_images.hpp"
#include "plexui_render.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace misterplex::plexui;

namespace {

std::vector<std::uint8_t> makePng(int w, int h, int seed) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    std::mt19937 rng(static_cast<unsigned>(seed));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i] = static_cast<std::uint8_t>((x + seed) & 255);
            rgba[i + 1] = static_cast<std::uint8_t>((y * 3 + seed) & 255);
            rgba[i + 2] = static_cast<std::uint8_t>((x ^ y ^ seed) & 255);
            rgba[i + 3] = 255;
        }
    }
    std::vector<std::uint8_t> out;
    auto write = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
        const auto* p = static_cast<const std::uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    stbi_write_png_to_func(write, &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

misterplex::BrowseHttpResult fakeGet(const std::string&, int, int,
                                     const misterplex::BrowseCancelFn&) {
    static std::mutex mu;
    static std::vector<std::vector<std::uint8_t>> bodies;
    static std::once_flag once;
    std::call_once(once, [] {
        bodies.push_back(makePng(32, 48, 1));
        bodies.push_back(makePng(48, 72, 2));
        bodies.push_back(makePng(64, 96, 3));
        bodies.push_back(makePng(96, 144, 4));
    });
    std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> pick(0, static_cast<int>(bodies.size()) - 1);
    std::uniform_int_distribution<int> latency(0, 40);
    std::this_thread::sleep_for(std::chrono::milliseconds(latency(rng)));
    misterplex::BrowseHttpResult r;
    r.status = 200;
    {
        std::lock_guard<std::mutex> lk(mu);
        const int idx = pick(rng);
        r.body.assign(reinterpret_cast<const char*>(bodies[static_cast<std::size_t>(idx)].data()),
                      bodies[static_cast<std::size_t>(idx)].size());
    }
    return r;
}

} // namespace

int main() {
    const char* durRaw = std::getenv("MPX_PLEXUI_STRESS_SEC");
    const int sec = durRaw && *durRaw ? std::atoi(durRaw) : 20;

    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "plexui_cache_stress";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    setenv("MPX_PLEXUI_DISK_CACHE", tmp.string().c_str(), 1);
    setenv("MPX_PLEXUI_DISK_CACHE_MB", "1", 1);
    setenv("MPX_PLEXUI_RAM_CACHE_MB", "1", 1);
    setenv("MPX_PLEXUI_IMG_WORKERS", "3", 1);

    const GridLayout gl = computeGridLayout(true);
    ImageCache cache(256u * 1024u);
    std::atomic<bool> stop{false};
    cache.start(fakeGet, [](const ImageCacheKey&) {}, [](const std::string&) {});

    std::atomic<uint64_t> viewGen{1};
    std::atomic<int> navCol{0};
    std::atomic<int> navRow{0};

    auto navigator = [&] {
        std::mt19937 rng(42);
        while (!stop.load()) {
            const uint64_t vg = ++viewGen;
            cache.beginView(vg);
            std::vector<ImageCacheKey> pins;
            for (int burst = 0; burst < 8; ++burst) {
                const int row = navRow.load();
                const int col = navCol.load();
                const std::string rk = "rk" + std::to_string(row * gl.cols + col);
                ImageCacheKey k =
                    thumbCacheKey("mid", rk, gl.posterW, gl.posterH, "rev" + rk);
                pins.push_back(k);
                cache.request(k, "http://fake/w=" + std::to_string(gl.posterW) + "&height=" +
                                      std::to_string(gl.posterH),
                              vg, 100);
                ImageCacheKey ak = artCacheKey("mid", rk, 160, 90, "art" + rk);
                cache.request(ak, "http://fake/art?width=160&height=90", vg, 50);
                if (burst % 2 == 0)
                    navCol.fetch_add(1);
                else
                    navRow.fetch_add(1);
            }
            cache.setPinned(pins);
            std::this_thread::sleep_for(std::chrono::milliseconds(30 + (rng() % 40)));
        }
    };

    auto renderer = [&] {
        while (!stop.load()) {
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < gl.cols; ++col) {
                    const std::string rk = "rk" + std::to_string(row * gl.cols + col);
                    ImageCacheKey k =
                        thumbCacheKey("mid", rk, gl.posterW, gl.posterH, "rev" + rk);
                    (void)cache.get(k);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    std::thread tNav(navigator);
    std::thread tRen(renderer);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, sec));
    while (std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    tNav.join();
    tRen.join();
    cache.stop();
    std::printf("test_plexui_cache_stress ok sec=%d bytes=%zu\n", sec, cache.bytesUsed());
    return 0;
}
