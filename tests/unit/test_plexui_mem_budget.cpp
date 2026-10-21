// 0012 soak OOM: ONE hard PlexUI byte budget (art LRU + view-layer cache + in-flight decodes +
// disk write queue). Soak G7 on ada08f22 held imgcache_bytes=29180700/18965504.
#include "plexui_images.hpp"
#include "plexui_render.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace misterplex::plexui;

static int gFail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++gFail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace {
constexpr std::size_t MB = 1024u * 1024u;

std::vector<std::uint8_t> makePng(int w, int h, int seed) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i] = static_cast<std::uint8_t>((x * 7 + seed) & 255);
            rgba[i + 1] = static_cast<std::uint8_t>((y * 3 + seed) & 255);
            rgba[i + 2] = static_cast<std::uint8_t>((x ^ y ^ seed) & 255);
            rgba[i + 3] = 255;
        }
    std::vector<std::uint8_t> out;
    stbi_write_png_to_func([](void* c, void* d, int n) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(c);
        v->insert(v->end(), static_cast<std::uint8_t*>(d), static_cast<std::uint8_t*>(d) + n);
    }, &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

// Serves a PNG at the URL's width/height (as the PMS photo transcoder does).
misterplex::BrowseHttpResult fakeGet(const std::string& url, int, int,
                                     const misterplex::BrowseCancelFn&) {
    static std::mutex mu;
    static std::vector<std::pair<std::string, std::vector<std::uint8_t>>> bodies;
    auto dim = [&](const char* k) {
        const auto p = url.find(k);
        return p == std::string::npos ? 64 : std::atoi(url.c_str() + p + std::strlen(k));
    };
    const int w = dim("width="), h = dim("height=");
    const std::string key = std::to_string(w) + "x" + std::to_string(h);
    misterplex::BrowseHttpResult r;
    r.status = 200;
    {
        std::lock_guard<std::mutex> lk(mu);
        const std::vector<std::uint8_t>* b = nullptr;
        for (auto& e : bodies)
            if (e.first == key) b = &e.second;
        if (!b) { bodies.emplace_back(key, makePng(w, h, w + h)); b = &bodies.back().second; }
        r.body.assign(reinterpret_cast<const char*>(b->data()), b->size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1 + (std::rand() % 6)));
    return r;
}

long rssKb() {
    std::ifstream f("/proc/self/statm");
    long pages = 0, res = 0;
    f >> pages >> res;
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif
#else
constexpr bool kAsan = false;
#endif
} // namespace

static ImageCacheKey tk(int i, int w = 180, int h = 270) {
    return thumbCacheKey("mid", "rk" + std::to_string(i), w, h, "r");
}

// G7 root cause: the budget recomputed from MemAvailable/3 fell to 18.97 MB while 29 MB was held.
static void testLowerTrims() {
    ImageCache c(32 * MB);
    std::vector<ImageCacheKey> pins;
    for (int i = 0; i < 400; ++i) { c.testInsertDecoded(tk(i), 180, 270); pins.push_back(tk(i)); }
    c.setPinned(pins);   // the current view pins what it shows
    CHECK(c.totalBytes() > 25 * MB, "seed %zu", c.totalBytes());
    c.setMaxBytes(18965504);
    CHECK(c.totalBytes() <= 18965504, "lowered budget not enforced: %zu/18965504", c.totalBytes());
    std::printf("lower_trims total=%zu/18965504 pinned_evictions=%llu\n", c.totalBytes(),
                static_cast<unsigned long long>(c.memStats().pinnedEvictions));
}

static void testPinsCannotHoldOver() {
    ImageCache c(8 * MB);
    std::vector<ImageCacheKey> pins;
    for (int i = 0; i < 200; ++i) pins.push_back(tk(i));
    c.setPinned(pins);
    for (int i = 0; i < 200; ++i) c.testInsertDecoded(tk(i), 180, 270);
    CHECK(c.totalBytes() <= 8 * MB, "pins held %zu over 8 MB", c.totalBytes());
}

// Visible posters that already fill the budget stay put when another decode arrives.
// Evicting one to admit the next is the refetch loop (onArrival asks for the one just dropped).
static void testVisiblePinsSurviveInflight() {
    constexpr int w = 180, h = 270;
    const std::size_t one = static_cast<std::size_t>(w) * h * 4u;
    ImageCache c(4 * one);
    std::vector<ImageCacheKey> pins;
    for (int i = 0; i < 4; ++i) {
        c.testInsertDecoded(tk(i), w, h);
        pins.push_back(tk(i));
    }
    c.setPinned(pins);
    CHECK(c.memStats().artEntries == 4, "seed entries %zu", c.memStats().artEntries);
    const bool admitted = c.testAdmitInflight(one);
    CHECK(!admitted, "in-flight decode evicted a visible poster");
    CHECK(c.memStats().pinnedEvictions == 0, "pinned_evictions %llu",
          static_cast<unsigned long long>(c.memStats().pinnedEvictions));
    CHECK(c.memStats().artEntries == 4, "entries %zu after reserve", c.memStats().artEntries);
    for (int i = 0; i < 4; ++i)
        CHECK(c.get(tk(i)) != nullptr, "visible poster %d was evicted", i);
    c.testInsertDecoded(tk(99), w, h);
    CHECK(c.get(tk(99)) == nullptr, "a poster that does not fit was stored");
    CHECK(c.totalBytes() <= 4 * one, "row grew to %zu", c.totalBytes());
}

static void testLayerTierCounts() {
    ImageCache c(19 * MB);
    for (int i = 0; i < 200; ++i) c.testInsertDecoded(tk(i), 180, 270);
    const std::size_t before = c.memStats().artBytes;
    c.setLayerBytes(2 * 720u * 480u * 4u * 2u);   // 2 snaps x (stat + backdrop) at 720x480 BGRA
    const auto m = c.memStats();
    CHECK(m.total <= 19 * MB, "art+layers %zu > 19 MB", m.total);
    CHECK(m.artBytes < before, "art not evicted for the layer tier (%zu -> %zu)", before, m.artBytes);
}

static void testRendererLayerCap() {
    setImageGeometry(720, 480, 8, 96, 144);
    Renderer r;
    r.setLayerCacheCap(3 * MB);
    CHECK(r.layerCacheBytes() <= 3 * MB, "layer cap");
}

// Dedupe: many threads requesting the same keys insert each key once (bytes == entries footprint).
static void testDedupe() {
    ImageCache c(64 * MB);
    c.start(fakeGet, [](const ImageCacheKey&) {});
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&] {
            for (int rep = 0; rep < 50; ++rep)
                for (int i = 0; i < 20; ++i)
                    c.request(tk(i), "http://f/p?width=180&height=270", 1, 100);
        });
    for (auto& t : ts) t.join();
    for (int i = 0; i < 400 && c.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto m = c.memStats();
    c.stop();
    CHECK(m.artEntries == 20, "entries %zu", m.artEntries);
    CHECK(m.artBytes == 20 * decodedImageBytes(180, 270), "bytes %zu (double insert?)", m.artBytes);
    CHECK(m.inflightBytes == 0 && m.inflight == 0, "in-flight leaked %zu/%zu", m.inflightBytes, m.inflight);
}

static void testStatsLine() {
    setenv("MPX_PLEXUI_MEM_STATS_SEC", "1", 1);
    std::mutex mu;
    std::vector<std::string> lines;
    ImageCache c(8 * MB);
    c.start(fakeGet, [](const ImageCacheKey&) {}, [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(mu);
        if (s.rfind("plexui: mem ", 0) == 0) lines.push_back(s);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    c.stop();
    unsetenv("MPX_PLEXUI_MEM_STATS_SEC");
    CHECK(lines.size() >= 2, "stats lines %zu", lines.size());
    if (!lines.empty()) {
        for (const char* f : {"total=", "art_bytes=", "art_entries=", "layer_bytes=", "inflight_bytes=",
                              "inflight=", "diskq_bytes=", "refused="})
            CHECK(lines[0].find(f) != std::string::npos, "missing %s in %s", f, lines[0].c_str());
        std::printf("%s\n", lines[0].c_str());
    }
}

// 30-min-equivalent soak (time-compressed): rapid grid scrolling across many libraries at the OB
// grid poster + backdrop sizes, 3 workers, disk cache on, the view-layer tier toggling 0..2 snaps,
// and the budget falling 32 -> 19 -> 12 MB as MemAvailable drops (as in G7). A sampler asserts the
// art + layers + disk-queue tiers never exceed the budget, and the total (with in-flight) never
// does once the lowering's in-flight decodes drained. RSS growth is bounded (non-ASan build).
static void testSoak() {
    const char* mv = std::getenv("MPX_PLEXUI_SOAK_MOVES");
    const int moves = mv && *mv ? std::atoi(mv) : 9000;   // 30 min x 5 moves/s
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "plexui_mem_soak";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    setenv("MPX_PLEXUI_DISK_CACHE", tmp.string().c_str(), 1);
    setenv("MPX_PLEXUI_DISK_CACHE_MB", "16", 1);
    setenv("MPX_PLEXUI_IMG_WORKERS", "3", 1);
    setImageGeometry(720, 480, 8, 96, 144);
    const GridLayout gl = computeGridLayout(true);

    ImageCache c(32 * MB);
    std::atomic<std::size_t> budget{32 * MB};
    c.start(fakeGet, [](const ImageCacheKey&) {}, [](const std::string&) {});
    std::atomic<bool> stop{false};
    std::atomic<long> violStored{0}, violTotal{0};
    std::atomic<std::size_t> maxStored{0};
    std::atomic<long long> lowerAtMs{0};
    const auto t0 = std::chrono::steady_clock::now();
    auto nowMs = [&] { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count(); };
    std::thread sampler([&] {
        while (!stop) {
            const auto m = c.memStats();
            const std::size_t stored = m.artBytes + m.layerBytes + m.diskqBytes;
            if (stored > maxStored) maxStored = stored;
            if (stored > m.budget) ++violStored;
            if (m.total > m.budget && nowMs() - lowerAtMs > 1500) ++violTotal;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    long rssWarm = 0, rssMax = 0;
    std::mt19937 rng(7);
    for (int i = 0; i < moves; ++i) {
        if (i == moves / 3 || i == 2 * moves / 3) {
            budget = i == moves / 3 ? 18965504u : 12 * MB;
            lowerAtMs = nowMs();
            c.setMaxBytes(budget);
        }
        const int lib = (i / 300) % 40;        // 40 libraries, a new one every 300 moves
        const int row = (i % 300) / 2;          // scroll down
        const uint64_t vg = static_cast<uint64_t>(i) + 1;
        c.beginView(vg);
        std::vector<ImageCacheKey> pins;
        for (int r = row; r < row + 4; ++r)
            for (int col = 0; col < gl.cols; ++col) {
                const std::string rk = "L" + std::to_string(lib) + "_" + std::to_string(r * gl.cols + col);
                ImageCacheKey k = thumbCacheKey("mid", rk, gl.posterW, gl.posterH, "r");
                pins.push_back(k);
                c.request(k, "http://f/p?width=" + std::to_string(gl.posterW) + "&height=" +
                                 std::to_string(gl.posterH), vg, 100 + col);
            }
        const std::string fk = "L" + std::to_string(lib) + "_" + std::to_string(row * gl.cols);
        c.request(artCacheKey("mid", fk, 160, 90, "a"), "http://f/a?width=160&height=90", vg, 300);
        c.setPinned(pins);
        c.setLayerBytes((i / 50) % 3 * 2u * 720u * 480u * 4u);   // 0, 1, 2 snaps
        if (i % 25 == 0)
            for (int r = 0; r < 8; ++r) (void)c.get(pins[rng() % pins.size()]);
        std::this_thread::sleep_for(std::chrono::microseconds(1500));
        if (i == moves / 10) rssWarm = rssKb();
        if (i > moves / 10) rssMax = std::max(rssMax, rssKb());
    }
    for (int i = 0; i < 300 && c.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    sampler.join();
    const auto m = c.memStats();
    c.stop();
    std::filesystem::remove_all(tmp, ec);
    std::printf("soak moves=%d max_stored=%zu final_total=%zu/%zu evictions=%llu pinned_ev=%llu "
                "refused=%llu rss_warm_kb=%ld rss_max_kb=%ld asan=%d\n", moves, maxStored.load(),
                m.total, m.budget, (unsigned long long)m.evictions,
                (unsigned long long)m.pinnedEvictions, (unsigned long long)m.refused, rssWarm,
                rssMax, kAsan ? 1 : 0);
    CHECK(violStored == 0, "stored tiers over budget in %ld samples", violStored.load());
    CHECK(violTotal == 0, "total over budget in %ld samples", violTotal.load());
    CHECK(m.total <= 12 * MB && m.inflight == 0, "final %zu inflight %zu", m.total, m.inflight);
    if (!kAsan)
        CHECK(rssMax - rssWarm <= 24 * 1024, "RSS grew %ld kB after warm-up", rssMax - rssWarm);
}

int main() {
    testLowerTrims();
    testPinsCannotHoldOver();
    testVisiblePinsSurviveInflight();
    testLayerTierCounts();
    testRendererLayerCap();
    testDedupe();
    testStatsLine();
    testSoak();
    if (gFail) { std::printf("test_plexui_mem_budget FAILED %d\n", gFail); return 1; }
    std::printf("test_plexui_mem_budget ok\n");
    return 0;
}
