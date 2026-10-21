#include "libmisterplex/browse_fetch.hpp"
#include "plexui_images.hpp"
#include "plexui_render.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace misterplex::plexui;

static misterplex::BrowseHttpResult fakePngGet(const std::string&, int, int,
                                               const misterplex::BrowseCancelFn&) {
    static const unsigned char kMinPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
        0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x08,
        0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xdd, 0x8d,
        0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    misterplex::BrowseHttpResult r;
    r.status = 200;
    r.body.assign(reinterpret_cast<const char*>(kMinPng), sizeof(kMinPng));
    return r;
}

int main() {
    const GridLayout gl = computeGridLayout(true);
    const int slots = 2 * (gridVisibleRowCount(gl) + 1) * gl.cols;
    const std::size_t budget =
        computeImageCacheBudgetBytes(5, gl.posterW, gl.posterH, slots);
    assert(budget > 100000u);

    ImageCache cache(64u * 1024u);
    std::atomic<int> arrivals{0};
    cache.start(
        fakePngGet, [&arrivals](const ImageCacheKey&) { arrivals.fetch_add(1); }, [](const std::string&) {});
    assert(parseImagePathUpdatedAt("/library/metadata/6/thumb/1790977412") == "1790977412");
    const ImageCacheKey k = thumbCacheKey("mid", "rk1", 32, 48, "rev1");
    const std::string url = "http://example/photo?width=32&height=48";
    cache.request(k, url, 1, 10);
    cache.request(k, url, 1, 20);
    for (int i = 0; i < 200 && !cache.get(k); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(cache.get(k) != nullptr);
    assert(arrivals.load() == 1);

    const ImageCacheKey k2 = thumbCacheKey("mid", "rk2", 32, 48, "rev2");
    cache.setPinned({k});
    cache.request(k2, url, 2, 10);
    for (int i = 0; i < 200 && !cache.get(k2); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(cache.get(k) != nullptr);

    cache.stop();
    std::printf("test_plexui_imgcache ok budget=%zu\n", budget);
    return 0;
}
