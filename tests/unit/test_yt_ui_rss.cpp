// Gate 7: a run of OB plays plus a PlexUI poster cache must not grow RSS.
// Built with ASan and run with quarantine_size_mb=0 so the allocator, not the
// quarantine, is what the samples measure. LSan is detect_leaks=1.
#include "libmisterplex/owenb_session.hpp"
#include "fake_owenb_core.hpp"
#include "plexui_images.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex::owenb;
using misterplex::plexui::ImageCache;
using misterplex::plexui::thumbCacheKey;
using fakeob::PacedCore;

namespace {
int g_fail = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_fail;                                                                              \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

constexpr uint32_t kRing = 128u * 1024u;

long rssKb() {
    std::ifstream f("/proc/self/statm");
    long pages = 0, res = 0;
    f >> pages >> res;
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

std::vector<MediaPacket> clip() {
    const std::string path = std::string(MPX_SOURCE_ROOT) + "/tests/fixtures/owenb_t5/cabac_160x96_2s.264";
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<MediaPacket> out;
    const size_t n = h264ring_split_annexb(b.data(), b.size(), nullptr, 0);
    std::vector<h264ring_au> aus(n);
    if (n)
        h264ring_split_annexb(b.data(), b.size(), aus.data(), n);
    for (size_t i = 0; i < n; ++i) {
        MediaPacket v;
        v.kind = MediaPacket::Kind::Video;
        v.data.assign(b.begin() + aus[i].off, b.begin() + aus[i].off + aus[i].len);
        v.pts90k = static_cast<int64_t>(i) * 3750;
        v.keyframe = true;
        out.push_back(std::move(v));
        MediaPacket a;
        a.kind = MediaPacket::Kind::Audio;
        a.data.assign(8000, static_cast<uint8_t>(i));
        a.pts90k = v.pts90k;
        out.push_back(std::move(a));
    }
    return out;
}

EndReason playOnce(const std::vector<MediaPacket>& packets) {
    misterplex::owenb::HeapDdrWindow win(kRing);
    PacedCore core(win);
    OwenbRing ring(win, 250);
    SessionConfig cfg;
    cfg.sleepUs = [&](unsigned us) { win.sleepUs(us); };
    OwenbPlaySession session(
        ring,
        [&](const OpenRequest&, std::string&) -> std::unique_ptr<IMediaSource> {
            struct Src : IMediaSource {
                const std::vector<MediaPacket>* pk = nullptr;
                size_t i = 0;
                SourceRead read(MediaPacket& p, std::string&) override {
                    if (!pk || i >= pk->size())
                        return SourceRead::Eof;
                    p = (*pk)[i++];
                    return SourceRead::Packet;
                }
            };
            auto s = std::make_unique<Src>();
            s->pk = &packets;
            return s;
        },
        cfg);
    OpenRequest req;
    req.url = "fixture";
    return session.run(req, 7);
}
} // namespace

int main() {
    const std::vector<MediaPacket> packets = clip();
    CHECK(!packets.empty(), "cabac fixture produced no packets");
    if (packets.empty())
        return 1;
    const long rss0 = rssKb();
    CHECK(playOnce(packets) == EndReason::Eof, "warm-up play did not reach Eof");
    const long warm = rssKb();
    for (int i = 0; i < 23; ++i)
        CHECK(playOnce(packets) == EndReason::Eof, "play %d did not reach Eof", i + 2);
    const long afterPlay = rssKb();
    {
        ImageCache cache(8u * 1024u * 1024u);
        for (int i = 0; i < 400; ++i) {
            const auto key = thumbCacheKey("mid", "rk" + std::to_string(i % 30), 180, 270, "r");
            cache.testInsertDecoded(key, 180, 270);
        }
        CHECK(cache.totalBytes() <= 8u * 1024u * 1024u, "poster cache %zu over 8 MB", cache.totalBytes());
    }
    const long end = rssKb();
    std::printf("yt_ui_rss kb start=%ld warm=%ld after_play=%ld end=%ld growth_play=%ld growth_total=%ld\n",
                rss0, warm, afterPlay, end, afterPlay - warm, end - warm);
    CHECK(afterPlay - warm <= 8 * 1024, "play RSS grew %ld kB after warm-up", afterPlay - warm);
    CHECK(end - warm <= 16 * 1024, "play+posters RSS grew %ld kB after warm-up", end - warm);
    if (g_fail) {
        std::printf("test_yt_ui_rss FAILED %d\n", g_fail);
        return 1;
    }
    std::printf("test_yt_ui_rss ok\n");
    return 0;
}
