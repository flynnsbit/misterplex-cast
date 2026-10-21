// MPX OWENB-P1B T6: the ARM overlay renderer (host/libmisterplex/owenb_osd_{layout,draw,renderer}.hpp,
// ARM-RENDERER-SPEC-1017) on a fake core, and the daemon wiring.
//   L*  layout() for every mode x fb0 size (1.38 / 3.69 / 8.29 MB / 8 MiB): overlay_check() == 0, the bands inside
//       smem, the bar inside the safe and picture rects, even lines at 480i / 576i, the page plan (Decisions 2-3)
//   P*  pixels: straight BGRA in the page; a C++ model of the core's composite (h264/ovl_plane.sv:446-467 unpack +
//       premultiply, the (1,2,1) anti-twitter, h264/ovl_blend.sv out = P + div255(V x (255 - Pa))) over a test
//       picture -> PNG artifacts + sha256 goldens (tests/fixtures/owenb_t6/goldens.txt), mode x state
//   F*  the flip state machine on a fake port: double-buffered flips, a superseded generation, a refusal, an ack
//       timeout (the retired page never written), the single-page path with its blank frame, re-layout, hide
//   A*  Decision 1 + 0006: the user's Analog Aspect saved, Letterbox at 31 kHz progressive on Auto, the user's
//       value restored at every end (stop / error / teardown / crash), never sticky
//   W*  OwenbPlayer + OwenbRing + the paced fake core (descriptor words 72-79 -> word 46): the overlay shown,
//       the Letterbox request applied, hidden and cleared at stop; MPX_OB_OSD=0; source rows (no FbPresent, no
//       0x31600000, no ioctl 6 on the OB route)
// Mutants (each must FAIL): MUT_OSD_PREMULT, MUT_OSD_REUSE_BEFORE_GEN, MUT_OSD_8MB_NOT_SMEM, MUT_OSD_SINGLE_NO_BLANK.
// --dump DIR writes the PNGs and the page / descriptor files tests/unit/owenb_osd_ovl_ref.py cross-checks;
// --update-goldens rewrites the goldens file.
#include "libmisterplex/owenb_play.hpp"
#include "libmisterplex/owenb_aspect_guard.hpp"
#include "libmisterplex/owenb_input.hpp"
#include "libmisterplex/owenb_library.hpp"
#include "libmisterplex/owenb_menu.hpp"
#include "fake_owenb_core.hpp"
#include "h264_sps_builder.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex;
using namespace misterplex::owenb_osd;

namespace {
int g_fails = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

std::string g_dump;            // --dump DIR
bool g_update = false;         // --update-goldens

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string readSrc(const std::string& rel) { return readFile(std::string(MPX_SOURCE_ROOT) + "/" + rel); }

// ---- SHA-256 (FIPS 180-4), for the goldens ----
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t n = 0;
    uint64_t total = 0;
    static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i)
            w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len) {
            const size_t m = std::min(len, 64 - n);
            std::memcpy(buf + n, p, m);
            n += m; p += m; len -= m;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (n != 56) add(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (56 - 8 * i));
        add(l, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};
std::string sha256(const std::vector<uint8_t>& v) {
    Sha256 s;
    s.add(v.data(), v.size());
    return s.hex();
}

// ---- PNG (RGB8, stored deflate blocks): the visual-check artifacts ----
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    c = ~c;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
void writePng(const std::string& path, unsigned w, unsigned h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    for (unsigned y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0;;) {
        const size_t m = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + m >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(m)); z.push_back(uint8_t(m >> 8));
        z.push_back(uint8_t(~m)); z.push_back(uint8_t(~m >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + m);
        off += m;
        if (last) break;
    }
    const uint32_t ad = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(uint8_t(ad >> (8 * i)));
    std::ofstream f(path, std::ios::binary);
    auto be32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) f.put(char(v >> (8 * i))); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        be32(uint32_t(d.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        f.write(reinterpret_cast<const char*>(td.data()), std::streamsize(td.size()));
        be32(crc32(td.data(), td.size()));
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ih{uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                            uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ih);
    chunk("IDAT", z);
    chunk("IEND", {});
}

// ---- the core's composite, modelled (not linked): ovl_plane.sv unpack + premultiply, AT, ovl_blend.sv ----
inline uint32_t rdiv255(uint32_t x) { return (2 * x + 255) / 510; }   // tools/ovl_ref.py rdiv255
struct Px4 { uint32_t r = 0, g = 0, b = 0, a = 0; };

Px4 canvasPremul(const uint8_t* fbVirt, uint32_t fbPhys, const struct ::h264ring_overlay& d, int c, int x) {
    if (c < int(d.band_first) || c > int(d.band_last) || c < 0 || c >= int(d.height))
        return {};
    const uint8_t* p = fbVirt + (d.base - fbPhys) + size_t(c) * d.stride + size_t(x) * 4;
    const uint32_t v = uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    const uint32_t r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255, a = v >> 24;   // 8888: A R G B
    return {rdiv255(r * a), rdiv255(g * a), rdiv255(b * a), a};
}

// The raster's premultiplied overlay (rw x rh RGBA) and the composite over `video` (RGB) -> RGB.
void composite(const uint8_t* fbVirt, uint32_t fbPhys, const struct ::h264ring_overlay* d, unsigned rw, unsigned rh,
               bool at, const std::vector<uint8_t>& video, std::vector<uint8_t>& ovl, std::vector<uint8_t>& out) {
    ovl.assign(size_t(rw) * rh * 4, 0);
    out = video;
    if (!d)
        return;
    const int ox = (int(rw) - int(d->width)) >> 1, oy = (int(rh) - int(d->height)) >> 1;
    for (unsigned L = 0; L < rh; ++L) {
        const int c = int(L) - oy;
        for (int x = 0; x < int(d->width); ++x) {
            const int X = ox + x;
            if (X < 0 || X >= int(rw))
                continue;
            Px4 q;
            if (at) {
                const Px4 t0 = canvasPremul(fbVirt, fbPhys, *d, c - 1, x), t1 = canvasPremul(fbVirt, fbPhys, *d, c, x),
                          t2 = canvasPremul(fbVirt, fbPhys, *d, c + 1, x);
                q = {(t0.r + 2 * t1.r + t2.r + 2) >> 2, (t0.g + 2 * t1.g + t2.g + 2) >> 2,
                     (t0.b + 2 * t1.b + t2.b + 2) >> 2, (t0.a + 2 * t1.a + t2.a + 2) >> 2};
            } else {
                q = canvasPremul(fbVirt, fbPhys, *d, c, x);
            }
            uint8_t* o = &ovl[(size_t(L) * rw + size_t(X)) * 4];
            o[0] = uint8_t(q.r); o[1] = uint8_t(q.g); o[2] = uint8_t(q.b); o[3] = uint8_t(q.a);
            uint8_t* v = &out[(size_t(L) * rw + size_t(X)) * 3];
            const uint32_t pc[3] = {q.r, q.g, q.b};
            for (int i = 0; i < 3; ++i)
                v[i] = uint8_t(std::min<uint32_t>(255, pc[i] + rdiv255(uint32_t(v[i]) * (255 - q.a))));
        }
    }
}

std::vector<uint8_t> testPicture(const Raster& r) {
    std::vector<uint8_t> v(size_t(r.w) * r.h * 3, 0);
    const Layout L = layoutFor(r);
    for (int y = L.pic.y; y < L.pic.bottom(); ++y)
        for (int x = L.pic.x; x < L.pic.right(); ++x) {
            uint8_t* p = &v[(size_t(y) * r.w + size_t(x)) * 3];
            p[0] = uint8_t(x * 255 / int(r.w));
            p[1] = uint8_t(y * 255 / int(r.h));
            p[2] = uint8_t(((x / 32 + y / 32) & 1) ? 160 : 96);
        }
    return v;
}

// ---- the modes of the spec's §4 table (and the edges around it) ----
struct ModeCase {
    const char* name;
    Raster r;
};
Raster mk(unsigned w, unsigned h, bool il, unsigned vout, unsigned ax, unsigned ay, int px = 0, int py = 0,
          unsigned pw = 0, unsigned ph = 0, bool lb = false) {
    Raster r;
    r.w = w; r.h = h; r.interlaced = il; r.videoOut = vout; r.arX = ax; r.arY = ay;
    r.picX0 = px; r.picY0 = py; r.picW = pw; r.picH = ph; r.lb = lb;
    return r;
}
std::vector<ModeCase> modes() {
    const unsigned P = H264RING_VOUT_PROGRESSIVE, I = H264RING_VOUT_INTERLACED, N = H264RING_VOUT_NATIVE;
    return {
        {"480p", mk(720, 480, false, P, 4, 3, 0, 0, 720, 480)},
        {"480p-lb169", mk(720, 480, false, P, 4, 3, 0, 60, 720, 360)},          // Letterbox: 16:9 in 4:3
        {"480p-crop", mk(720, 480, false, P, 4, 3, -120, 0, 960, 480)},          // Crop: x0 < 0
        {"480p-nopic", mk(720, 480, false, P, 4, 3)},                            // before the first picture
        {"480i", mk(720, 480, true, I, 4, 3, 0, 0, 720, 480)},
        {"480i-lb169", mk(720, 480, true, I, 4, 3, 0, 60, 720, 360)},
        {"576i", mk(720, 576, true, I, 4, 3, 0, 0, 720, 576)},
        {"576p", mk(720, 576, false, P, 4, 3, 0, 72, 720, 432)},
        {"native-hd", mk(1280, 720, false, N, 16, 9, 0, 0, 1280, 720)},
        {"native-sd", mk(854, 480, false, N, 16, 9, 0, 0, 854, 480)},
        {"native-fhd", mk(1920, 1080, false, N, 16, 9, 0, 0, 1920, 1080)},
    };
}

void checkPlan(const char* name, const PagePlan& p, uint32_t smem, int full) {
    for (int i = 0; i < p.pages(); ++i) {
        const struct ::h264ring_overlay d = descriptorFor(p, i);
        const uint64_t lo = uint64_t(d.base) + uint64_t(d.band_first) * d.stride;
        const uint64_t hi = uint64_t(d.base) + uint64_t(d.band_last) * d.stride + uint64_t(d.width) * 4;
        CHECK(h264ring_overlay_check(&d) == 0, "L3 %s smem %u full %d page %d: overlay_check %d", name, smem, full, i,
              h264ring_overlay_check(&d));
        CHECK(lo >= H264RING_FB_BYTE && hi <= uint64_t(H264RING_FB_BYTE) + smem,
              "L3 %s smem %u full %d page %d: the band inside fb0", name, smem, full, i);
        CHECK(d.stride % 8 == 0 && d.format == H264RING_OVL_FMT_8888, "L3 %s: 8888, stride %% 8", name);
    }
    if (p.buf == Buffering::Double) {
        const uint64_t a0 = p.base[0] + uint64_t(p.bandFirst) * p.stride,
                       a1 = p.base[1] + uint64_t(p.bandFirst) * p.stride,
                       n = uint64_t(p.bandLast - p.bandFirst + 1) * p.stride;
        CHECK(a0 + n <= a1 || a1 + n <= a0, "L3 %s smem %u full %d: the two bands disjoint", name, smem, full);
    }
}

// L1-L5: every mode x fb0 size.
void testLayout() {
    const uint32_t sizes[] = {1382400u, 3686400u, 8294400u, 8388608u};
    for (const auto& m : modes()) {
        const Layout L = layoutFor(m.r);
        const Rect area = intersect(L.pic, L.safe);
        CHECK(inside(L.bar, L.safe) && inside(L.bar, L.canvas), "L1 %s: the bar inside the safe rect", m.name);
        CHECK(area.empty() || inside(L.bar, area), "L1 %s: the bar inside picture ∩ safe (%d,%d %dx%d)", m.name,
              L.bar.x, L.bar.y, L.bar.w, L.bar.h);
        CHECK(L.bar.h <= int(L.bandH) && L.bar.h >= 64, "L1 %s: bar height %d (band %u)", m.name, L.bar.h, L.bandH);
        CHECK(!L.evenLines || ((L.bar.y & 1) == 0 && (L.bar.h & 1) == 0 && (L.safe.y & 1) == 0),
              "L2 %s: even lines at 480i / 576i", m.name);
        CHECK(L.evenLines == m.r.interlaced, "L2 %s: even lines exactly when interlaced", m.name);
        for (uint32_t smem : sizes)
            for (int full = 0; full < 2; ++full) {
                const unsigned bf = full ? 0 : unsigned(L.bar.y), bl = full ? L.H - 1 : unsigned(L.bar.bottom() - 1);
                const PagePlan p = planPages(H264RING_FB_BYTE, smem, L.W, L.H, bf, bl);
                checkPlan(m.name, p, smem, full);
                // Decision 2: double-buffered 8888 in every mode of the table with 8 MiB, the full canvas too
                if (smem == 8388608u && L.W * L.H * 4u * 2u <= smem)
                    CHECK(p.buf == Buffering::Double && !p.compact, "L4 %s full %d: double at 8 MiB (%s)", m.name, full,
                          bufferingName(p.buf));
                // 1080 native is past it: a second 8.29 MB canvas does not fit the 8 MiB region, even offset by
                // one band (the core's RANGE check is on the whole canvas) -> single (T6-OSD-SPEC-GAPS.md G3)
                if (std::string(m.name) == "native-fhd" && smem >= 8294400u)
                    CHECK(p.buf == Buffering::Single, "L4 native-fhd smem %u full %d: single (%s)", smem, full,
                          bufferingName(p.buf));
            }
    }
    const Layout hd = layoutFor(modes()[8].r), p480 = layoutFor(modes()[0].r), i576 = layoutFor(modes()[6].r);
    CHECK(hd.safe == (Rect{40, 28, 1200, 664}) && hd.bandH == 96, "L1 native HD: 40x28 margins, band 96");
    CHECK(p480.safe == (Rect{72, 48, 576, 384}) && p480.bandH == 80, "L1 480p: 72x48 margins, band 80");
    CHECK(i576.safe.x == 72 && i576.safe.y == 58, "L1 576i: 72x57 margins, 57 -> 58 (even)");
    const Layout lb = layoutFor(modes()[1].r);
    CHECK(lb.bar.bottom() == 420 && lb.bar.y >= 60, "L1 letterbox: the bar on the picture's bottom (%d)", lb.bar.bottom());
    // L4: the fallbacks from smem_len (Decision 3 is a runtime path): 720x480 in a 1.38 MB fb0
    // the band trick (§1: (bf + 2 bandH) x stride <= smem): a band near the top (toasts) fits twice ...
    const PagePlan top = planPages(H264RING_FB_BYTE, 1382400u, 720, 480, 48, 127);
    CHECK(top.buf == Buffering::Double && top.compact && top.base[1] == H264RING_FB_BYTE + 80u * 2880u,
          "L4 720x480 band 48..127 in 1.38 MB: double compact, page 1 at phys + bandH x stride");
    // ... the bar at the bottom does not ((352 + 160) x 2880 > 1.38 MB): single
    const PagePlan bar = planPages(H264RING_FB_BYTE, 1382400u, 720, 480, 352, 431);
    CHECK(bar.buf == Buffering::Single, "L4 720x480 bar in 1.38 MB: single (%s)", bufferingName(bar.buf));
    const PagePlan fullCanvas = planPages(H264RING_FB_BYTE, 1382400u, 720, 480, 0, 479);
    CHECK(fullCanvas.buf == Buffering::Single, "L4 720x480 full canvas in 1.38 MB: single (%s)",
          bufferingName(fullCanvas.buf));
    CHECK(planPages(H264RING_FB_BYTE, 3686400u, 1920, 1080, 0, 1079).buf == Buffering::None,
          "L4 1080 full canvas in 3.69 MB: no page");
    CHECK(planPages(H264RING_FB_BYTE, 8388608u, 720, 480, 0, 479, false).buf == Buffering::Single,
          "L4 allowDouble = false (MPX_OB_OSD_SINGLE=1): single");
    CHECK(planPages(H264RING_FB_BYTE + 4, 8388608u, 720, 480, 0, 479).buf == Buffering::None, "L4 a misaligned fb0");
    // L5: 0x31600000 (the idle framebuffer) is outside the overlay's region: no plan can reach it
    CHECK(planPages(0x31600000u, 8388608u, 720, 480, 0, 479).buf == Buffering::None,
          "L5 a page at 0x31600000 is refused (outside [FB_BYTE, +FB_SIZE))");
}

// ---- a fake core behind IOverlayPort: descriptors taken at vblank(), the shown page watched ----
struct FakePort final : IOverlayPort {
    FakePort(const uint8_t* v, uint32_t p) : virt(v), phys(p) {}
    const uint8_t* virt;
    uint32_t phys;
    Raster raster = mk(720, 480, false, H264RING_VOUT_PROGRESSIVE, 4, 3, 0, 0, 720, 480);
    unsigned osdAA = 0, osdAR = 0;   // 0008: the menu's Aspect Ratio too
    unsigned wordMask = 0, wordAa = 0, wordAr = 0, reqAr = 0, arRequests = 0;
    bool session = true, take = true, forceRefuse = false;
    uint32_t writtenGen = 0, gen = 0;
    bool wEnable = false, shownValid = false;
    struct ::h264ring_overlay wDesc{}, shown{};
    unsigned state = H264RING_OVL_S_OFF, err = 0;
    uint64_t shownHash = 0;
    int violations = 0, takes = 0, requests = 0, clears = 0;
    unsigned reqMask = 0, reqVal = 0;
    bool applySettings = true;          // the core takes a settings request within a poll
    unsigned setSeq = 0, setErr = 0, word = 0;   // word: the request word's Analog Aspect + 1, 0 cleared
    std::vector<unsigned> reqVals;
    std::vector<std::string> ops;
    std::vector<uint32_t> bases;   // the pages taken, in order

    int show(const struct ::h264ring_overlay& o) override {
        if (h264ring_overlay_check(&o))
            return -EINVAL;
        wDesc = o;
        wEnable = true;
        ops.push_back("show");
        return int(++writtenGen);
    }
    int hide() override {
        wEnable = false;
        ops.push_back("hide");
        return int(++writtenGen);
    }
    OvlStatus status() override {
        OvlStatus s;
        s.valid = true;
        s.gen = gen;
        s.state = state;
        s.err = err;
        s.raster = raster;
        s.osdAnalogAspect = osdAA;
        s.osdAspect = osdAR;
        s.setSeq = setSeq;
        s.setErr = setErr;
        s.session = session;
        return s;
    }
    int requestSettings(unsigned mask, const unsigned v[H264RING_OPT_COUNT]) override {
        ++requests;
        reqMask = mask;
        reqVal = v[H264RING_OPT_ANALOG_ASPECT];
        reqAr = v[H264RING_OPT_ASPECT];
        if (mask & (1u << H264RING_OPT_ASPECT))
            ++arRequests;
        reqVals.push_back(reqVal);
        if (!session)
            return -ENOTCONN;
        wordMask = mask;
        wordAa = reqVal;
        wordAr = reqAr;
        word = (mask & (1u << H264RING_OPT_ANALOG_ASPECT)) ? reqVal + 1 : 100 + reqAr;
        if (applySettings) {
            setSeq = unsigned(requests);
            if (mask & (1u << H264RING_OPT_ANALOG_ASPECT))
                osdAA = reqVal;
            if (mask & (1u << H264RING_OPT_ASPECT))
                osdAR = reqAr;
        }
        return requests;
    }
    int clearSettings() override {
        ++clears;
        word = 0;
        wordMask = 0;
        return 0;
    }
    void sessionStart() {   // the core applies the request word again at each session's start
        if (wordMask & (1u << H264RING_OPT_ANALOG_ASPECT))
            osdAA = wordAa;
        if (wordMask & (1u << H264RING_OPT_ASPECT))
            osdAR = wordAr;
    }
    void resend() { ++writtenGen; }   // a session start re-writes the descriptor under a new generation
    uint64_t hashShown() const {
        uint64_t h = 1469598103934665603ull;
        for (unsigned y = shown.band_first; y <= shown.band_last; ++y)
            h = fnvMix(h, virt + (shown.base - phys) + size_t(y) * shown.stride, size_t(shown.width) * 4);
        return h;
    }
    void checkPage() {   // the core reads the shown page every refresh: nobody may write it
        if (shownValid && hashShown() != shownHash) {
            ++violations;
            shownHash = hashShown();
        }
    }
    void vblank() {
        checkPage();
        if (!take || writtenGen == gen)
            return;
        gen = writtenGen;
        if (!wEnable) {
            state = H264RING_OVL_S_OFF;
            err = 0;
            shownValid = false;
        } else if (forceRefuse) {
            state = H264RING_OVL_S_REFUSED;
            err = H264RING_OVL_E_RANGE;
            shownValid = false;
        } else {
            state = H264RING_OVL_S_ACCEPTED;
            err = 0;
            shown = wDesc;
            shownValid = true;
            shownHash = hashShown();
            ++takes;
            bases.push_back(shown.base);
        }
    }
};

struct Rig {
    explicit Rig(uint32_t len = 8388608u, bool allowDouble = true)
        : mem(8388608u, 0), fb{mem.data(), H264RING_FB_BYTE, len}, port(mem.data(), H264RING_FB_BYTE),
          r(port, fb, allowDouble, [this](const std::string& s) { logs.push_back(s); }) {}
    std::vector<uint8_t> mem;
    FbMap fb;
    FakePort port;
    std::vector<std::string> logs;
    Renderer r;
    int64_t now = 0, nextTick = 0;
    std::function<UiState(int64_t)> ui;
    // ms of time in 1 ms steps: a refresh every `vb` ms, the renderer when its delay is up
    void run(int64_t ms, int vb = 16) {
        for (int64_t end = now + ms; now < end; ++now) {
            if (vb > 0 && now % vb == 0)
                port.vblank();
            if (now >= nextTick) {
                nextTick = now + std::max<int64_t>(1, r.tick(now, ui(now)));
                port.checkPage();
            }
        }
    }
    bool guardClean() const {   // nothing written past fb0's length
        for (size_t i = fb.len; i < mem.size(); ++i)
            if (mem[i])
                return false;
        return true;
    }
    int count(const char* op) const {
        int n = 0;
        for (const auto& o : port.ops)
            n += o == op;
        return n;
    }
    const uint8_t* shownPx(int x, int y) const {
        const auto& d = port.shown;
        return mem.data() + (d.base - fb.phys) + size_t(y) * d.stride + size_t(x) * 4;
    }
};

UiState pausedAt(int64_t posMs) {
    UiState s;
    s.bar = Bar::Paused;
    s.title = "Big Buck Bunny";
    s.positionMs = posMs;
    s.durationMs = 2 * 3600 * 1000;
    s.bufferedMs = posMs + 600000;
    return s;
}

// F1 + P1: double-buffered flips: pages alternate, the shown page is never written, straight alpha in the page.
void testFlipsDouble() {
    Rig g;
    g.ui = [](int64_t now) { return pausedAt(3723000 + now); };   // a new second every 1000 ms
    g.run(5200);
    CHECK(g.r.plan().buf == Buffering::Double && !g.r.plan().compact, "F1 double, full pages (%s)",
          bufferingName(g.r.plan().buf));
    CHECK(g.port.takes >= 5 && g.port.violations == 0, "F1 %d flips taken, %d writes into the shown page",
          g.port.takes, g.port.violations);
    bool alt = g.port.bases.size() >= 4;
    for (size_t i = 1; i < g.port.bases.size(); ++i)
        alt = alt && g.port.bases[i] != g.port.bases[i - 1];
    CHECK(alt && g.port.bases[0] == g.r.plan().base[0], "F1 the pages alternate (base0, base1, ...)");
    CHECK(g.count("hide") == 0 && g.guardClean(), "F1 no hide between flips; nothing past fb0's length");
    const Layout& L = g.r.layout();
    const uint8_t* p = g.shownPx(L.bar.x, L.bar.y);
    CHECK(p[0] == kPanel.b && p[1] == kPanel.g && p[2] == kPanel.r && p[3] == kPanel.a,
          "P1 the panel's corner is straight B,G,R,A = %u,%u,%u,%u (got %u,%u,%u,%u)", kPanel.b, kPanel.g, kPanel.r,
          kPanel.a, p[0], p[1], p[2], p[3]);
    const struct ::h264ring_overlay& d = g.port.shown;
    CHECK(d.band_first == unsigned(L.bar.y) && d.band_last == unsigned(L.bar.bottom() - 1) &&
              d.format == H264RING_OVL_FMT_8888 && d.width == 720 && d.height == 480,
          "F1 the band is the bar's lines (%u..%u), the canvas the raster", d.band_first, d.band_last);
    // F8: the same second again and again -> no redraw (the content key), at most 8 Hz
    Rig k;
    k.ui = [](int64_t now) { return pausedAt(10000 + now % 900); };
    k.run(3000);
    CHECK(k.port.takes == 1 && k.r.stats().skippedSameKey > 10, "F8 one draw for unchanged content (%d, %u skipped)",
          k.port.takes, k.r.stats().skippedSameKey);
}

// F2-F4: a superseded generation, a refusal, a core that stops taking descriptors.
void testFlipEdges() {
    {   // F2: the flip's generation is superseded by a session start's re-send before the core reports it
        Rig g;
        g.ui = [](int64_t now) { return pausedAt(now); };
        g.run(10, 0);                // a show, no refresh yet
        g.port.resend();
        g.port.resend();
        g.run(1500);
        CHECK(g.r.stats().timeouts == 0 && g.port.takes >= 2 && g.port.violations == 0,
              "F2 ovl_gen past the flip's generation counts as taken (signed compare): %d takes", g.port.takes);
    }
    {   // F3: refused -> log ovl_err, hide, no retry loop; a new layout tries again
        Rig g;
        g.port.forceRefuse = true;
        g.ui = [](int64_t now) { return pausedAt(now); };
        g.run(3000);
        CHECK(g.r.stats().refused == 1 && g.r.stats().lastErr == H264RING_OVL_E_RANGE && g.count("show") == 1 &&
                  g.count("hide") == 1 && !g.port.wEnable,
              "F3 refused once: ovl_err %u logged, hidden, no retry (%d shows)", g.r.stats().lastErr, g.count("show"));
        bool logged = false;
        for (const auto& l : g.logs)
            logged = logged || l.find("refused, ovl_err 6") != std::string::npos;
        CHECK(logged, "F3 the refusal is logged with ovl_err");
        g.port.forceRefuse = false;
        g.port.raster = mk(720, 480, true, H264RING_VOUT_INTERLACED, 4, 3, 0, 0, 720, 480);
        g.run(500);
        CHECK(g.port.takes == 1 && g.port.wEnable, "F3 a new layout shows again");
    }
    {   // F4: the core stops reading descriptors: log the timeout, wait, never touch the retired page
        Rig g;
        g.ui = [](int64_t now) { return pausedAt(now); };
        g.run(1100);
        const int shows = g.count("show");
        g.port.take = false;
        g.run(2500);
        CHECK(g.r.stats().timeouts == 1 && g.count("show") == shows + 1 && g.port.violations == 0,
              "F4 timeout logged once, one flip outstanding (%d -> %d shows), the shown page untouched",
              shows, g.count("show"));
        g.port.take = true;
        g.run(1500);
        CHECK(g.count("show") > shows + 1 && g.port.violations == 0, "F4 the core takes again: flips resume");
    }
}

// H: the final stop asks for the idle card while a flip is still untaken. After the
// ack timeout the new screen is drawn; the page the core is still reading is not written.
void testIdleAfterUntaken() {
    Rig g;
    g.ui = [](int64_t now) {
        if (now < 2200)
            return pausedAt(now);
        UiState s;
        s.idle = true;
        s.idleName = "MiSTer";
        s.idleHint = "Ready to cast";
        return s;
    };
    g.run(1100);
    g.port.take = false;
    const int shows = g.count("show");
    g.run(2800);
    CHECK(g.r.stats().timeouts >= 1 && g.count("show") > shows && g.port.violations == 0,
          "H idle card issued after an untaken generation (timeouts %u shows %d -> %d violations %d)",
          g.r.stats().timeouts, shows, g.count("show"), g.port.violations);
    g.port.take = true;
    g.run(400);
    CHECK(g.port.wEnable && g.port.shown.band_first == 0 && g.port.violations == 0,
          "H the core takes the idle card (band %u violations %d)", g.port.shown.band_first, g.port.violations);
}

// F5: the single-page path (Decision 3): hide, wait, draw, show -- one blank frame a redraw.
void testSingle() {
    Rig g(8388608u, false);
    g.ui = [](int64_t now) { return pausedAt(now); };
    g.run(3200);
    CHECK(g.r.plan().buf == Buffering::Single, "F5 MPX_OB_OSD_SINGLE: single (%s)", bufferingName(g.r.plan().buf));
    CHECK(g.r.stats().blankFrames >= 2 && g.count("hide") >= 2 && g.port.violations == 0,
          "F5 a blank frame before each redraw (%u), the shown page never written (%d)", g.r.stats().blankFrames,
          g.port.violations);
    bool order = !g.port.ops.empty() && g.port.ops[0] == "show";
    for (size_t i = 1; i < g.port.ops.size(); ++i)
        order = order && g.port.ops[i] != g.port.ops[i - 1];
    CHECK(order, "F5 show, hide, show, hide ...");
    // smem too small for two full canvases (1.38 MB) + an error screen (the whole canvas): single from smem_len
    Rig e(1382400u);
    e.ui = [](int64_t) {
        UiState s;
        s.error = ErrorScreen::Refused;
        s.errorDetail = "CAVLC (not CABAC)";
        return s;
    };
    e.run(500);
    CHECK(e.r.plan().buf == Buffering::Single && e.port.takes == 1 && e.guardClean(),
          "F5 an error screen in a 1.38 MB fb0: single, inside fb0 (%s)", bufferingName(e.r.plan().buf));
}

// F6-F7: a raster change re-lays out (hide, wait, new pages); a bar that times out is hidden.
void testRelayoutHide() {
    Rig g;
    g.ui = [](int64_t now) { return pausedAt(now); };
    g.run(300);
    g.port.raster = mk(720, 480, true, H264RING_VOUT_INTERLACED, 4, 3, 0, 0, 720, 480);
    g.run(300);
    const auto& d = g.port.shown;
    CHECK(g.r.stats().replans >= 1 && g.count("hide") == 1 && g.port.wEnable && g.r.layout().evenLines,
          "F6 480p -> 480i: hidden, laid out again, shown (%u replans)", g.r.stats().replans);
    CHECK((d.band_first & 1) == 0 && (d.band_last & 1) == 1, "F6 480i: the band starts even and ends odd (%u..%u)",
          d.band_first, d.band_last);
    Rig h;
    UiBus bus;
    bus.setPlayback(true, false, 5000, 60000);
    bus.showBar(0);
    h.ui = [&bus](int64_t now) { return bus.snapshot().resolve(now); };
    h.run(3900);
    CHECK(h.port.wEnable && h.count("hide") == 0, "F7 Playing: shown for 4 s");
    h.run(300);
    CHECK(!h.port.wEnable && h.count("hide") == 1 && h.r.shownPage() < 0, "F7 hidden 4 s after the show");
    bus.update([](UiModel& m) { m.paused = true; });
    h.run(6000);
    CHECK(h.port.wEnable && h.count("hide") == 1, "F7 Paused: pinned");
}

// A1-A9: Decision 1 (Letterbox at 31 kHz progressive on Auto) + 0006: the user's value restored.
void testAspect() {
    const AspectGuard::Sleep nosleep;
    auto session = [](FakePort& p, AspectGuard& g) {
        g.newSession();
        p.sessionStart();
        g.onStatus(p, p.status(), {});
    };
    {   // A1 + A5: Auto at progressive -> one Letterbox request, the user's Auto saved, restored at the end
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        session(p, g);
        CHECK(p.requests == 1 && p.reqMask == (1u << H264RING_OPT_ANALOG_ASPECT) && p.reqVal == 2 && p.osdAA == 2,
              "A1 progressive + Auto: one Letterbox request, Analog Aspect only (mask %#x value %u)", p.reqMask, p.reqVal);
        CHECK(st->has && st->val == 0 && st->saves == 1, "A5 the user's Auto saved before the request");
        g.onStatus(p, p.status(), nosleep);
        CHECK(p.requests == 1, "A1 once a session");
        const AspectEnd e = g.endSession(p, nosleep);
        CHECK(e == AspectEnd::Restored && p.reqVals.size() == 2 && p.reqVals[1] == 0 && p.osdAA == 0,
              "A5 the end requests the user's Auto back and the core took it (%s, menu %u)", aspectEndName(e), p.osdAA);
        CHECK(p.clears == 1 && p.word == 0 && !st->has, "A5 then the word cleared, the saved value dropped");
        session(p, g);   // the next session: Auto again (not a sticky Letterbox) -> Letterbox again, restored again
        CHECK(p.requests == 3 && p.reqVal == 2, "A5 the next session sees the user's Auto, not a stuck Letterbox");
        CHECK(g.endSession(p, nosleep) == AspectEnd::Restored && p.osdAA == 0, "A5 restored again");
    }
    {   // A2-A3: no request at 480i, over a user's Fit, or on Native HDMI; nothing saved, nothing to restore
        for (int k = 0; k < 3; ++k) {
            FakePort p(nullptr, 0);
            if (k == 0)
                p.raster = mk(720, 480, true, H264RING_VOUT_INTERLACED, 4, 3);
            if (k == 1)
                p.osdAA = 1;
            if (k == 2)
                p.raster = mk(1280, 720, false, H264RING_VOUT_NATIVE, 16, 9);
            auto st = std::make_shared<MemAspectStore>();
            AspectGuard g(st);
            session(p, g);
            CHECK(p.requests == 0 && st->saves == 0, "A2 no request, nothing saved (%s)",
                  k == 0 ? "480i" : k == 1 ? "the user's Fit" : "Native HDMI");
            CHECK(g.endSession(p, nosleep) == AspectEnd::Nothing && p.clears == 0 && p.osdAA == (k == 1 ? 1u : 0u),
                  "A3 nothing to restore, the menu untouched");
        }
    }
    {   // A4: no session yet -> nothing; the session opens -> the request
        FakePort p(nullptr, 0);
        p.session = false;
        AspectGuard g;
        g.newSession();
        g.onStatus(p, p.status(), nosleep);
        CHECK(p.requests == 0, "A4 no session: no request");
        p.session = true;
        g.onStatus(p, p.status(), nosleep);
        CHECK(p.requests == 1, "A4 the session opens: the request");
    }
    {   // A6: the user picks Crop during the session: theirs wins, no restore over it
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        session(p, g);
        p.osdAA = 3;
        const AspectEnd e = g.endSession(p, nosleep);
        CHECK(e == AspectEnd::UserChanged && p.requests == 1 && p.osdAA == 3 && p.clears == 1 && !st->has,
              "A6 the user's Crop during the session kept (%s): no restore request, word cleared", aspectEndName(e));
    }
    {   // A7: teardown with no session (error / OSD Reset): the restore is kept, applied by the next session
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        session(p, g);
        p.applySettings = false;   // the core stopped taking requests
        const AspectEnd e = g.endSession(p, nosleep, 10);
        CHECK(e == AspectEnd::Pending && st->has && st->val == 0 && p.clears == 0 && p.word == 1,
              "A7 an untaken restore: pending (%s), the word left holding the user's Auto, the value kept",
              aspectEndName(e));
        p.applySettings = true;
        p.raster = mk(720, 480, true, H264RING_VOUT_INTERLACED, 4, 3);   // the next session (here 480i)
        session(p, g);
        CHECK(p.osdAA == 0 && p.clears == 1 && !st->has && g.stats().startRestores == 1,
              "A7 the next session restores first, clears, drops the value (menu %u)", p.osdAA);
        CHECK(g.endSession(p, nosleep, 10) == AspectEnd::Nothing, "A7 nothing asked: nothing to restore");
        p.raster = mk(720, 480, false, H264RING_VOUT_PROGRESSIVE, 4, 3);
        session(p, g);
        p.session = false;   // the session is gone entirely at the end: -ENOTCONN is pending too
        CHECK(g.endSession(p, nosleep, 10) == AspectEnd::Pending && st->has, "A7 -ENOTCONN at the end: pending");
    }
    {   // A8: a crash: the state file survives, the core re-applied our Letterbox; a new daemon restores
        const std::string path = "/tmp/test_owenb_osd_aspect_state." + std::to_string(::getpid());
        {
            FileAspectStore fs(path);
            unsigned v = 9;
            CHECK(fs.save(1) && fs.load(v) && v == 1, "A8 the state file round trip");
        }
        FakePort p(nullptr, 0);
        p.osdAA = 2;   // a stuck Letterbox, re-applied at the session's start
        p.raster = mk(1280, 720, false, H264RING_VOUT_NATIVE, 16, 9);
        AspectGuard g(std::make_shared<FileAspectStore>(path));
        session(p, g);
        unsigned v = 9;
        CHECK(p.osdAA == 1 && p.requests == 1 && p.reqVal == 1 && p.clears == 1 && !FileAspectStore(path).load(v),
              "A8 the user's Fit from the state file restored over the stuck Letterbox (menu %u)", p.osdAA);
        FakePort q(nullptr, 0);
        q.osdAA = 2;
        AspectGuard g2(std::make_shared<FileAspectStore>(path));
        FileAspectStore(path).save(0);
        session(q, g2);
        CHECK(q.reqVal == 2 && q.requests == 1 && FileAspectStore(path).load(v) && v == 0,
              "A8 at progressive the saved Auto (not the stuck Letterbox) decides: Letterbox again, Auto kept");
        CHECK(g2.endSession(q, nosleep) == AspectEnd::Restored && q.osdAA == 0 && !FileAspectStore(path).load(v),
              "A8 and restored at the end");
        std::remove(path.c_str());
    }
    {   // A9: a refused restore (SET_ERR != 0) is not taken for applied
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        session(p, g);
        p.setErr = 1;
        CHECK(g.endSession(p, nosleep, 10) == AspectEnd::Pending && st->has, "A9 SET_ERR 1: still pending");
    }
}


// B1-B8 (0008): the Aspect Ratio 4:3 override of a VUI-less 4:3 stream, saved and restored like 0006's.
void testAspect43() {
    const AspectGuard::Sleep nosleep;
    const unsigned AA = 1u << H264RING_OPT_ANALOG_ASPECT, AR = 1u << H264RING_OPT_ASPECT;
    auto play = [](FakePort& p, AspectGuard& g, bool force43) {
        g.newSession(true);
        p.sessionStart();
        g.onStatus(p, p.status(), {});   // B7: nothing before the stream is known
        StreamAspect a;
        a.known = true;
        a.force43 = force43;
        g.setStream(a);
        if (g.wantsStatus())
            g.onStatus(p, p.status(), {});
    };
    {   // B1: progressive, both menus Auto, a VUI-less 4:3 stream: one request with both; both restored
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        play(p, g, true);
        CHECK(p.requests == 1 && p.reqMask == (AA | AR) && p.reqVal == 2 && p.reqAr == 1 && p.osdAA == 2 && p.osdAR == 1,
              "B1 one request: Letterbox + Aspect Ratio 4:3 (mask %#x, AA %u AR %u)", p.reqMask, p.reqVal, p.reqAr);
        CHECK(st->has && st->val == 0 && st->hasAr && st->ar == 0 && g.stats().aspect43Requests == 1,
              "B1 both of the user's Autos saved before the request");
        const AspectEnd e = g.endSession(p, nosleep);
        CHECK(e == AspectEnd::Restored && p.reqMask == (AA | AR) && p.osdAA == 0 && p.osdAR == 0 && p.word == 0 &&
                  !st->has && !st->hasAr,
              "B1 the end restores both (%s, AA %u AR %u), clears, drops the values", aspectEndName(e), p.osdAA, p.osdAR);
    }
    {   // B2: the user's Aspect Ratio is 16:9 (or 4:3): never overridden
        for (unsigned user : {1u, 2u}) {
            FakePort p(nullptr, 0);
            p.osdAR = user;
            auto st = std::make_shared<MemAspectStore>();
            AspectGuard g(st);
            play(p, g, true);
            CHECK(p.arRequests == 0 && p.osdAR == user && !st->hasAr && p.reqMask == AA,
                  "B2 the user's Aspect Ratio %u kept: Letterbox only (mask %#x)", user, p.reqMask);
            g.endSession(p, nosleep);
            CHECK(p.osdAR == user, "B2 still the user's %u after the end", user);
        }
    }
    {   // B3: a stream with an aspect (or not 4:3): no Aspect Ratio request
        FakePort p(nullptr, 0);
        AspectGuard g;
        play(p, g, false);
        CHECK(p.arRequests == 0 && p.reqMask == AA, "B3 no 4:3 stream: Letterbox only");
    }
    {   // B4: 480i (no Letterbox) + VUI-less 4:3: Aspect Ratio only; the idle session never asks it
        FakePort p(nullptr, 0);
        p.raster = mk(720, 480, true, H264RING_VOUT_INTERLACED, 4, 3);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        play(p, g, true);
        CHECK(p.requests == 1 && p.reqMask == AR && p.osdAR == 1 && !st->has && st->hasAr,
              "B4 480i: Aspect Ratio 4:3 only (mask %#x), its Auto saved", p.reqMask);
        CHECK(g.endSession(p, nosleep) == AspectEnd::Restored && p.osdAR == 0, "B4 restored");
        AspectGuard idle;
        idle.newSession();
        StreamAspect a;
        a.known = true;
        a.force43 = true;
        idle.setStream(a);
        idle.onStatus(p, p.status(), nosleep, false);
        CHECK(p.requests == 2, "B4 the idle session asks nothing");
    }
    {   // B9 (BUG6, glass 1017): Video Output Native (HDMI) + a square-sample 4:3 picture (PMS 960x720 transcode):
        // shown at its own pixels, pillarboxed in the 1280x720 raster -> no Aspect Ratio 4:3 (it squeezed the raster)
        FakePort p(nullptr, 0);
        p.raster = mk(1280, 720, false, H264RING_VOUT_NATIVE, 16, 9);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        g.newSession(true);
        p.sessionStart();
        StreamAspect a;
        a.known = true;
        a.force43 = true;
        a.square43 = true;
        g.setStream(a);
        g.onStatus(p, p.status(), {});
        CHECK(p.arRequests == 0 && p.osdAR == 0 && !st->hasAr && g.stats().aspect43Requests == 0,
              "B9 Native + square 4:3 picture: no Aspect Ratio 4:3 (%u AR requests, osd %u)", p.arRequests, p.osdAR);
        // the same picture on a fitted output (Progressive) still needs the 4:3: the core scales it to 720x480
        FakePort q(nullptr, 0);
        AspectGuard h;
        h.newSession(true);
        q.sessionStart();
        h.setStream(a);
        h.onStatus(q, q.status(), {});
        CHECK(q.osdAR == 1 && h.stats().aspect43Requests == 1, "B9 Progressive + square 4:3: Aspect Ratio 4:3 kept");
        // Native + a non-square 4:3 (720x480 with a 4:3 container DAR): the raster is the picture, 4:3 still asked
        FakePort n(nullptr, 0);
        n.raster = mk(720, 480, false, H264RING_VOUT_NATIVE, 16, 9);
        AspectGuard k;
        k.newSession(true);
        n.sessionStart();
        StreamAspect b;
        b.known = true;
        b.force43 = true;
        k.setStream(b);
        k.onStatus(n, n.status(), {});
        CHECK(n.osdAR == 1, "B9 Native + anamorphic 4:3: Aspect Ratio 4:3 kept");
    }
    {   // B5: the user picks 16:9 during the play: kept; Letterbox still restored
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        play(p, g, true);
        p.osdAR = 2;
        const AspectEnd e = g.endSession(p, nosleep);
        CHECK(e == AspectEnd::Restored && p.reqMask == AA && p.osdAR == 2 && p.osdAA == 0 && !st->has && !st->hasAr,
              "B5 the user's 16:9 kept, only Analog Aspect restored (%s, mask %#x)", aspectEndName(e), p.reqMask);
        FakePort q(nullptr, 0);
        AspectGuard h;
        play(q, h, true);
        q.osdAR = 2;
        q.osdAA = 1;
        CHECK(h.endSession(q, nosleep) == AspectEnd::UserChanged && q.osdAR == 2 && q.osdAA == 1 && q.word == 0,
              "B5 both changed by the user: both kept, the word cleared");
    }
    {   // B6: an untaken restore of both: pending, the next session (here the idle one) restores both
        FakePort p(nullptr, 0);
        auto st = std::make_shared<MemAspectStore>();
        AspectGuard g(st);
        play(p, g, true);
        p.applySettings = false;
        CHECK(g.endSession(p, nosleep, 10) == AspectEnd::Pending && st->has && st->hasAr && p.osdAR == 1,
              "B6 the restore not taken: pending, both kept");
        p.applySettings = true;
        g.newSession();
        p.sessionStart();
        g.onStatus(p, p.status(), nosleep, false);
        CHECK(p.osdAA == 0 && p.osdAR == 0 && !st->has && !st->hasAr && g.stats().startRestores == 1,
              "B6 the idle session restored both (AA %u AR %u)", p.osdAA, p.osdAR);
    }
    {   // B7: a play waits for the stream; B8: the state file keeps both, and a 0006 file still loads
        AspectGuard g;
        g.newSession(true);
        CHECK(!g.wantsStatus(), "B7 a play's request waits for its stream's aspect");
        StreamAspect a;
        a.known = true;
        g.setStream(a);
        CHECK(g.wantsStatus(), "B7 then it is asked");
        const std::string path = "/tmp/test_owenb_osd_aspect43." + std::to_string(::getpid());
        AspectSaved sv;
        sv.aa = true;
        sv.aaVal = 3;
        sv.ar = true;
        sv.arVal = 0;
        AspectSaved back;
        CHECK(FileAspectStore(path).save(sv) && FileAspectStore(path).load(back) && back.aa && back.aaVal == 3 &&
                  back.ar && back.arVal == 0,
              "B8 the state file keeps both");
        std::FILE* f = std::fopen(path.c_str(), "w");
        std::fputs("analog_aspect=1\n", f);
        std::fclose(f);
        AspectSaved old;
        CHECK(FileAspectStore(path).load(old) && old.aa && old.aaVal == 1 && !old.ar, "B8 a 0006 file still loads");
        std::remove(path.c_str());
    }
}

// ---- goldens: mode x state through the renderer, composited as the core does ----
struct Golden {
    std::string name;
    Raster r;
    UiState s;
};
std::vector<Golden> goldens() {
    auto m = modes();
    auto R = [&](const char* n) {
        for (auto& c : m)
            if (std::string(c.name) == n)
                return c.r;
        return m[0].r;
    };
    UiState play = pausedAt(3723000);
    play.bar = Bar::Playing;
    UiState paused = pausedAt(3723000);
    UiState buf = play;
    buf.bar = Bar::Buffering;
    buf.bufferingPct = 40;
    UiState seek = play;
    seek.bar = Bar::Seeking;
    seek.seekTargetMs = 5400000;
    UiState tr = play;
    tr.transport = "+30s";
    UiState chips;
    chips.chip = "Owner's iPhone";
    chips.toasts = {"Subtitles off", "Transcoding instead"};
    UiState menu;
    menu.menu.open = true;
    menu.menu.title = "Video Output";
    menu.menu.items = {"Auto", "Interlaced", "Progressive", "Native (HDMI)"};
    menu.menu.selected = 2;
    UiState refused;
    refused.error = ErrorScreen::Refused;
    refused.errorDetail = "CAVLC (not CABAC)";
    UiState pms;
    pms.error = ErrorScreen::PmsUnreachable;
    pms.errorDetail = "192.0.2.20:32400";
    UiState pausedToast = paused;
    pausedToast.toasts = {"Audio: English AAC"};
    UiState playChip = play;
    playChip.chip = "Plex Web";
    UiState idle;   // T7: the idle session's card
    idle.idle = true;
    idle.idleName = "MiSTer Living Room";
    UiState idleIp;   // bug 5: the name main.cpp:1137-1143 gives the companion (the Plex Web cast list's)
    idleIp.idle = true;
    idleIp.idleName = "MiSTerPlex-192.0.2.110";
    UiState idleToast = idle;
    idleToast.toasts = {"Plex server found"};
    UiState idleChip = idle;
    idleChip.idleHint = "Connecting...";
    idleChip.chip = "Owner's iPhone";
    UiState stream;
    stream.error = ErrorScreen::StreamError;
    stream.errorDetail = "the decoder stopped: ring error 5";
    // 0010: the menu panel from the daemon's state (owenb_menu.hpp), over a play and on the idle card
    misterplex::owenb::ObMenuState ms;
    ms.playing = true;
    ms.paused = true;
    ms.title = "Blade Runner";
    ms.positionMs = 3723000;
    ms.durationMs = 7020000;
    ms.libraryAvailable = true;
    ms.pmsLabel = "Home";
    ms.pmsState = 1;
    misterplex::owenb::ObProgramMenu pm;
    pm.open(ms);
    UiState progPlay = paused;
    progPlay.menu = pm.view();
    misterplex::owenb::ObMenuState mi;
    mi.libraryAvailable = true;
    mi.castName = "MiSTer Living Room";
    mi.pmsLabel = "Home";
    mi.pmsState = 0;
    pm.open(mi);
    UiState progIdle = idle;
    progIdle.menu = pm.view();
    // 0010: the library page (owenb_library.hpp's window of the daemon's browser), over the idle card
    std::vector<misterplex::LibraryRow> lib;
    for (const char* t : {"Blade Runner", "Brazil", "Children of Men", "Contact", "Dune", "Gattaca", "Her", "Inception",
                          "Interstellar", "Moon", "Primer", "Solaris", "Stalker", "Sunshine", "The Thing", "Tron",
                          "Videodrome", "WALL-E", "Zardoz"}) {
        misterplex::LibraryRow r;
        r.title = t;
        lib.push_back(r);
    }
    UiState library = idle;
    library.library = misterplex::owenb::obLibraryPage(true, lib, 6, "", "Plex / Movies");
    std::vector<misterplex::LibraryRow> top(3);
    top[0].title = "Plex server Home";
    top[0].mpxKind = "header";
    top[1].title = "Movies";
    top[1].directory = true;
    top[2].title = "TV Shows";
    top[2].directory = true;
    UiState libTop = idle;
    libTop.library = misterplex::owenb::obLibraryPage(true, top, 1, "", "Plex");
    UiState libLoading = idle;
    libLoading.library = misterplex::owenb::obLibraryPage(true, {}, 0, "LOADING...", "Plex / TV Shows");
    UiState libMenu = library;   // the panel over the page
    libMenu.menu = pm.view();
    return {
        {"480p-progmenu-paused", R("480p"), progPlay}, {"480i-progmenu-idle", R("480i"), progIdle},
        {"native-hd-progmenu-paused", R("native-hd"), progPlay},
        {"480p-library", R("480p"), library},       {"480i-library", R("480i"), library},
        {"576i-library-top", R("576i"), libTop},    {"native-hd-library", R("native-hd"), library},
        {"480p-library-loading", R("480p"), libLoading}, {"480p-library-menu", R("480p"), libMenu},
        {"480p-idle", R("480p"), idle},             {"480i-idle-toast", R("480i"), idleToast},
        {"576i-idle", R("576i"), idle},             {"native-hd-idle-chip", R("native-hd"), idleChip},
        {"native-fhd-idle", R("native-fhd"), idle}, {"480i-error-stream", R("480i"), stream},
        {"480p-playing", R("480p"), play},          {"480p-paused", R("480p"), paused},
        {"480p-buffering", R("480p"), buf},         {"480p-seeking", R("480p"), seek},
        {"480p-transport", R("480p"), tr},          {"480p-toasts-chip", R("480p"), chips},
        {"480p-menu", R("480p"), menu},             {"480p-error-refused", R("480p"), refused},
        {"480p-lb169-playing", R("480p-lb169"), play}, {"480p-crop-paused", R("480p-crop"), paused},
        {"480i-paused-toast", R("480i"), pausedToast}, {"480i-lb169-playing", R("480i-lb169"), play},
        {"576i-playing", R("576i"), play},          {"576p-paused", R("576p"), paused},
        {"native-hd-playing-chip", R("native-hd"), playChip}, {"native-hd-error-pms", R("native-hd"), pms},
        {"native-sd-paused", R("native-sd"), paused},
        // bug 5: Interlaced, Auto (a core before 0.14 reads vout 0: Auto, Interlaced by raster_il) and 480p
        {"480i-idle-ip", R("480i"), idleIp}, {"480i-auto-idle-ip", mk(720, 480, true, 0, 4, 3, 0, 0, 720, 480), idleIp},
        {"576i-idle-ip", R("576i"), idleIp}, {"480p-idle-ip", R("480p"), idleIp},
    };
}

void dumpFile(const std::string& path, const void* p, size_t n) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(p), std::streamsize(n));
}

// P2-P4: each golden drawn by the renderer, the shown page composited (AT at 480i / 576i, as the core's Auto).
void testGoldens() {
    const std::string gpath = std::string(MPX_SOURCE_ROOT) + "/tests/fixtures/owenb_t6/goldens.txt";
    std::map<std::string, std::string> want;
    {
        std::istringstream in(readFile(gpath));
        std::string n, h;
        while (in >> n >> h)
            want[n] = h;
    }
    std::ostringstream got;
    if (!g_dump.empty())
        ::mkdir(g_dump.c_str(), 0755);
    for (const auto& gd : goldens()) {
        Rig g;
        g.port.raster = gd.r;
        const UiState s = gd.s;
        g.ui = [s](int64_t) { return s; };
        g.run(100);
        CHECK(g.port.takes == 1 && g.port.shownValid, "P2 %s: drawn and taken", gd.name.c_str());
        if (!g.port.shownValid)
            continue;
        const struct ::h264ring_overlay d = g.port.shown;
        const std::vector<uint8_t> video = testPicture(gd.r);
        std::vector<uint8_t> ovl, out;
        composite(g.mem.data(), g.fb.phys, &d, gd.r.w, gd.r.h, gd.r.interlaced, video, ovl, out);
        const std::string h = sha256(out);
        got << gd.name << " " << h << "\n";
        CHECK(g_update || want[gd.name] == h, "P2 %s: composite sha256 %s (golden %s)", gd.name.c_str(), h.c_str(),
              want.count(gd.name) ? want[gd.name].c_str() : "missing");
        // P3: outside the band the picture is untouched (the core reads no other line)
        const unsigned yOut = d.band_first > 2 ? 0 : gd.r.h - 1;
        const bool covers = d.band_first == 0 && d.band_last + 1u >= gd.r.h;
        CHECK(gd.s.error != ErrorScreen::None || covers ||
                  std::memcmp(&out[size_t(yOut) * gd.r.w * 3], &video[size_t(yOut) * gd.r.w * 3], gd.r.w * 3) == 0,
              "P3 %s: line %u outside the band is the picture", gd.name.c_str(), yOut);
        // P4: no ARM pre-filter -- the page's rows are drawn hard; the (1,2,1) is the fabric's
        if (gd.r.interlaced && gd.s.bar != Bar::Hidden) {
            const Layout& L = g.r.layout();
            const uint8_t* top = g.shownPx(L.bar.x + 1, L.bar.y);
            CHECK(top[3] == kPanel.a && (L.bar.y & 1) == 0, "P4 %s: the bar's first line is the panel's alpha %u, even",
                  gd.name.c_str(), top[3]);
        }
        if (!g_dump.empty()) {
            const std::string stem = g_dump + "/" + gd.name;
            writePng(stem + ".png", gd.r.w, gd.r.h, out);
            const size_t off = d.base - g.fb.phys, n = size_t(d.band_last + 1) * d.stride;
            dumpFile(stem + ".page.bin", g.mem.data() + off, n);
            dumpFile(stem + ".ovl.rgba", ovl.data(), ovl.size());
            std::ofstream t(stem + ".desc");
            t << d.base << " " << d.width << " " << d.height << " " << d.stride << " " << unsigned(d.format) << " "
              << d.band_first << " " << d.band_last << " " << gd.r.w << " " << gd.r.h << " " << int(gd.r.interlaced)
              << "\n";
        }
    }
    if (g_update) {
        std::ofstream f(gpath);
        f << got.str();
        std::printf("goldens written: %s\n", gpath.c_str());
    }
}

// ---- W: OwenbPlayer + OwenbRing + the paced fake core ----
using namespace misterplex::owenb;

struct LoopSource final : IMediaSource {   // test_owenb_play.cpp's: AUD-only AUs, 24 fps, PCM
    int64_t n = 0;
    bool audioNext = false;
    std::vector<uint8_t> sps;    // 0008: put in front of each keyframe (in band, as h264_mp4toannexb does)
    int64_t cNum = 0, cDen = 0;  // 0008: the container's display aspect (0: none)
    bool containerAspect(int64_t& num, int64_t& den) const override {
        if (cNum <= 0 || cDen <= 0)
            return false;
        num = cNum;
        den = cDen;
        return true;
    }
    SourceRead read(MediaPacket& p, std::string&) override {
        if (audioNext) {
            audioNext = false;
            p.kind = MediaPacket::Kind::Audio;
            p.data.assign(3840, 0x11);
            p.pts90k = (n - 1) * 3750;
            return SourceRead::Packet;
        }
        p.kind = MediaPacket::Kind::Video;
        p.data = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x0C, 0xFF, 0xFF, 0x80};
        p.keyframe = n % 12 == 0;
        if (p.keyframe && !sps.empty())
            p.data.insert(p.data.begin() + 6, sps.begin(), sps.end());
        p.pts90k = n * 3750;
        ++n;
        audioNext = true;
        return SourceRead::Packet;
    }
};
struct ProxyWindow final : OwenbDdrWindow {
    explicit ProxyWindow(HeapDdrWindow& h) : w(h) {}
    HeapDdrWindow& w;
    volatile uint64_t* ctrl() override { return w.ctrl(); }
    volatile uint64_t* ring() override { return w.ring(); }
    uint32_t ringSize() const override { return w.ringSize(); }
    bool mapped() const override { return true; }
    void sleepUs(unsigned us) override { w.sleepUs(us); }
    uint64_t nowUs() override { return w.nowUs(); }
};
bool waitFor(const std::function<bool()>& f, int ms = 4000) {
    for (int i = 0; i < ms; ++i) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f();
}

struct PlayRig {
    std::unique_ptr<HeapDdrWindow> heap;
    std::unique_ptr<fakeob::PacedCore> core;
    std::vector<uint8_t> fbmem = std::vector<uint8_t>(8388608u, 0);
    std::atomic<int> maps{0}, unmaps{0};
    std::unique_ptr<OwenbPlayer> player;
    bool keepCore = false;   // the same core (and its menu) across plays
    bool deadCore = false;   // the core's heartbeat never moves
    std::atomic<int> windows{0};
    std::shared_ptr<MemAspectStore> store = std::make_shared<MemAspectStore>();
    std::vector<uint8_t> sps;    // 0008: the stream's SPS (LoopSource)
    int64_t cNum = 0, cDen = 0;
    PlayRig(bool osdOn) {
        player = std::make_unique<OwenbPlayer>(
            [this](std::string&) -> std::unique_ptr<OwenbDdrWindow> {
                ++windows;
                if (keepCore && core)
                    return std::make_unique<ProxyWindow>(*heap);
                core.reset();
                heap = std::make_unique<HeapDdrWindow>(128u * 1024u);
                core = std::make_unique<fakeob::PacedCore>(*heap);
                core->alive = !deadCore;
                return std::make_unique<ProxyWindow>(*heap);
            },
            [this](const OpenRequest&, std::string&) -> std::unique_ptr<IMediaSource> {
                auto s = std::make_unique<LoopSource>();
                s->sps = sps;
                s->cNum = cNum;
                s->cDen = cDen;
                return s;
            });
        OsdConfig c;
        c.enabled = osdOn;
        c.mapFb = [this](FbMap& m, std::string&) {
            ++maps;
            m.virt = fbmem.data();
            m.phys = H264RING_FB_BYTE;
            m.len = uint32_t(fbmem.size());
            return true;
        };
        c.unmapFb = [this] { ++unmaps; };
        player->setOsd(c);
        AspectConfig a;
        a.store = store;
        player->setAspect(a);
    }
    bool idle(std::string& err) {
        IdleConfig c;
        c.name = "MiSTer Living Room";
        c.keepaliveMs = 2;
        c.sleepMs = [](unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
        c.stepLocked = [this] {
            if (core)
                core->win.sleepUs(200);
        };
        return player->startIdle(c, err);
    }
    bool start() {
        OpenRequest q;
        q.url = "loop";
        SessionConfig cfg;
        cfg.idleUs = 1000;
        cfg.sleepUs = [this](unsigned) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (core)
                core->win.sleepUs(100);
        };
        std::string err;
        return player->start(q, 7, cfg, OwenbPlayCallbacks{}, err);
    }
};

uint32_t ctrlLo(PlayRig& r, unsigned w) { return uint32_t(r.heap->ctrlWords()[w]); }

// W1-W3: the overlay on the OB play; MPX_OB_OSD=0; the raster through OwenbRing::overlayStatus.
void testPlayer() {
    {
        PlayRig r(true);
        CHECK(r.start(), "W1 started");
        CHECK(waitFor([&] { return r.player->started(); }), "W1 the first picture");
        r.player->ui().update([](UiModel& m) {
            m.title = "Sintel";
            m.durationMs = 888000;
            m.barUntilMs = INT64_MAX / 2;
        });
        CHECK(waitFor([&] { return r.core->ovlState == H264RING_OVL_S_ACCEPTED && r.core->ovlTakes >= 1; }),
              "W1 the core took an 8888 descriptor (state %u err %u)", r.core->ovlState, r.core->ovlErr);
        const struct ::h264ring_overlay d = r.core->ovl;
        CHECK(d.format == H264RING_OVL_FMT_8888 && d.width == 720 && d.height == 480 && d.base >= H264RING_FB_BYTE &&
                  d.base + uint64_t(d.band_last + 1) * d.stride <= uint64_t(H264RING_FB_BYTE) + r.fbmem.size(),
              "W1 the page inside fb0 (base %#x band %u..%u)", d.base, d.band_first, d.band_last);
        const uint8_t* px = r.fbmem.data() + (d.base - H264RING_FB_BYTE) + size_t(d.band_first) * d.stride;
        CHECK(px[4 * 80 + 3] == kPanel.a, "W1 the bar drawn in the page (alpha %u)", px[4 * 80 + 3]);
        CHECK(waitFor([&] { return r.core->osdAnalogAspect == 2u; }) && r.core->settingsApplied == 1,
              "W1 Decision 1: Analog Aspect Auto at progressive -> Letterbox applied once (%u, %u)",
              r.core->osdAnalogAspect, r.core->settingsApplied);
        r.core->rasterW = 1280;   // W3: the raster changes (Native HD): hide, re-plan, a 1280-wide canvas
        r.core->rasterH = 720;
        r.core->videoOut = H264RING_VOUT_NATIVE;
        r.core->arX = 16;
        r.core->arY = 9;
        CHECK(waitFor([&] { return r.core->ovl.width == 1280 && r.core->ovlState == H264RING_OVL_S_ACCEPTED; }),
              "W3 word 80 read through OwenbRing: a 1280x720 canvas (%u)", unsigned(r.core->ovl.width));
        r.player->stop();
        CHECK((ctrlLo(r, H264RING_W_OVL_CTL) & 1u) == 0 && ctrlLo(r, H264RING_W_SETTINGS) == 0,
              "W1 stop: the overlay hidden (ctl %#x), the settings request cleared (%#x)",
              ctrlLo(r, H264RING_W_OVL_CTL), ctrlLo(r, H264RING_W_SETTINGS));
        CHECK(r.core->osdAnalogAspect == 0 && r.core->settingsApplied == 2 && !r.store->has &&
                  r.player->lastAspectEnd() == AspectEnd::Restored && r.core->videoOutApplied == 0,
              "W1 0006: stop restored the user's Auto through the core (menu %u, %u applied, %s), never Video Output",
              r.core->osdAnalogAspect, r.core->settingsApplied, aspectEndName(r.player->lastAspectEnd()));
        CHECK(r.maps == 1 && r.unmaps == 1 && !r.player->osdRunning() && r.player->osdStats().shows >= 2,
              "W1 fb0 mapped once, unmapped at stop (%d / %d), %u shows", r.maps.load(), r.unmaps.load(),
              r.player->osdStats().shows);
    }
    {
        PlayRig r(false);
        CHECK(r.start() && waitFor([&] { return r.player->started(); }), "W2 MPX_OB_OSD=0: plays");
        r.player->ui().showBar(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(r.maps == 0 && !r.player->osdRunning() && (ctrlLo(r, H264RING_W_OVL_CTL) & 1u) == 0 &&
                  r.core->ovlState != H264RING_OVL_S_ACCEPTED,
              "W2 MPX_OB_OSD=0: no fb0, no overlay shown (maps %d ctl %#x state %u)",
              r.maps.load(), ctrlLo(r, H264RING_W_OVL_CTL), r.core->ovlState);
        CHECK(waitFor([&] { return r.core->osdAnalogAspect == 2u; }), "W2 the Letterbox default does not need the OSD");
        r.player->stop();
        CHECK(r.core->osdAnalogAspect == 0 && ctrlLo(r, H264RING_W_SETTINGS) == 0, "W2 restored at stop");
    }
    {   // W4: a teardown the core cannot take (settings stalled): pending; the next play restores first
        PlayRig r(false);
        r.keepCore = true;
        CHECK(r.start() && waitFor([&] { return r.core->osdAnalogAspect == 2u; }), "W4 Letterbox on the first play");
        r.core->settingsTake = false;
        r.player->stop();
        CHECK(r.player->lastAspectEnd() == AspectEnd::Pending && r.store->has && r.store->val == 0 &&
                  ctrlLo(r, H264RING_W_SETTINGS) != 0 && r.core->osdAnalogAspect == 2u,
              "W4 untaken restore: pending, the word holds the user's Auto, the value kept (%s)",
              aspectEndName(r.player->lastAspectEnd()));
        r.core->settingsTake = true;
        r.core->sess = false;   // an OSD Reset dropped the session: the word waits for the next start
        r.core->videoOut = H264RING_VOUT_INTERLACED;   // the next play at 480i: no Letterbox, just the restore
        r.core->rasterIl = true;
        CHECK(r.start() && waitFor([&] { return r.player->started(); }), "W4 the next play");
        CHECK(waitFor([&] { return !r.store->has; }) && r.core->osdAnalogAspect == 0 &&
                  r.player->aspectStats().startRestores == 1,
              "W4 the next play's session restored the user's Auto first (a new handle starts with no word, "
              "h264ring.c:175-181), then cleared (menu %u has %d sa %u sr %u)",
              r.core->osdAnalogAspect, int(r.store->has), r.core->startApplies, r.player->aspectStats().startRestores);
        r.player->stop();
        CHECK(r.player->lastAspectEnd() == AspectEnd::Nothing && ctrlLo(r, H264RING_W_SETTINGS) == 0 &&
                  r.core->osdAnalogAspect == 0,
              "W4 nothing left to restore; the word clear");
    }
}

// W5 (0008): end to end -- the play's first keyframe decides; the core's Aspect Ratio is 4:3 only for a
// VUI-less 4:3 stream, and the user's Auto is back after the stop.
void testPlayAspect43() {
    using spsb::Vui;
    struct Case {
        const char* name;
        std::vector<uint8_t> sps;
        int64_t cn, cd;
        bool want;
    };
    const std::vector<Case> cs = {
        {"640x480 no VUI", spsb::buildSps(640, 480, Vui::None), 0, 0, true},
        {"640x480 VUI, no aspect", spsb::buildSps(640, 480, Vui::NoAspect), 0, 0, true},
        {"640x480 VUI square (idc 1)", spsb::buildSps(640, 480, Vui::Aspect, 1), 0, 0, false},
        {"720x480 VUI 10:11 (idc 3)", spsb::buildSps(720, 480, Vui::Aspect, 3), 0, 0, false},
        {"720x480 no VUI, container SAR 8:9 (4:3)", spsb::buildSps(720, 480, Vui::None), 720 * 8, 480 * 9, true},
        {"720x480 no VUI, no container (3:2)", spsb::buildSps(720, 480, Vui::None), 0, 0, false},
        {"1280x720 no VUI", spsb::buildSps(1280, 720, Vui::None), 0, 0, false},
        {"640x480 no VUI, container 16:9", spsb::buildSps(640, 480, Vui::None), 16, 9, false},
        {"no SPS in band", {}, 0, 0, false},
    };
    for (const Case& c : cs) {
        PlayRig r(false);
        r.sps = c.sps;
        r.cNum = c.cn;
        r.cDen = c.cd;
        CHECK(r.start() && waitFor([&] { return r.player->started() && r.core->osdAnalogAspect == 2u; }),
              "W5 %s: playing, Letterbox asked", c.name);
        CHECK(r.core->osdAspect == (c.want ? 1u : 0u) && r.core->aspectApplied == (c.want ? 1u : 0u),
              "W5 %s: Aspect Ratio %s (menu %u, %u applied)", c.name, c.want ? "4:3 asked" : "untouched",
              r.core->osdAspect, r.core->aspectApplied);
        CHECK(r.player->aspectStats().aspect43Requests == (c.want ? 1u : 0u), "W5 %s: %u 4:3 requests", c.name,
              r.player->aspectStats().aspect43Requests);
        r.player->stop();
        CHECK(r.core->osdAspect == 0 && r.core->osdAnalogAspect == 0 && !r.store->has && !r.store->hasAr &&
                  r.core->aspectApplied == (c.want ? 2u : 0u),
              "W5 %s: stop restored the user's Autos (AR %u AA %u, %u applied)", c.name, r.core->osdAspect,
              r.core->osdAnalogAspect, r.core->aspectApplied);
    }
}

// I1-I7: T7, the idle session: the idle card and the error screens without a play.
// bug 5 (V2-BUGS-1017 #5/5a): the idle card's text fits, uncut, in every mode -- at 480i / 576i (Video Output
// Interlaced, or Auto on an analog TV) inside 70 % of the canvas, clear of a CRT's overscan.
void testIdleFit() {
    std::vector<std::pair<std::string, Raster>> rs;
    for (const auto& m : modes())
        rs.push_back({m.name, m.r});
    rs.push_back({"480i-auto", mk(720, 480, true, 0, 4, 3, 0, 0, 720, 480)});
    rs.push_back({"576i-auto", mk(720, 576, true, 0, 4, 3, 0, 0, 720, 576)});
    for (const char* name : {"MiSTerPlex-192.0.2.110", "MiSTerPlex-192.0.2.200", "MiSTer Living Room"})
        for (const auto& [mn, r] : rs) {
            UiState s;
            s.idle = true;
            s.idleName = name;
            const Layout L = layoutFor(r);
            const Frame f = planFrame(s, L);
            const int p = f.pad, tx = f.card.x + 2 * p + f.font.colX(12), tw = f.card.right() - p - tx;
            CHECK(f.idleField && inside(f.card, L.safe) && inside(f.card, L.canvas),
                  "F1 %s %s: the card inside title-safe, on the chevron field", mn.c_str(), name);
            bool uncut = true;
            for (const std::string& t : {std::string("MISTERPLEX"), upper(idleHintText(s)), upper(s.idleName)})
                uncut = uncut && f.cardFont.fit(t, tw) == t && tx + f.cardFont.width(t) <= f.card.right();
            CHECK(uncut, "F2 %s %s: every line whole (no \"...\"), inside the card (tw %d, need %d)", mn.c_str(), name, tw,
                  f.cardFont.width(upper(s.idleName)));
            if (L.mode == OutMode::Interlaced)
                CHECK(f.card.w <= int(L.W) * 7 / 10 && f.card.x >= int(L.W) * 3 / 20,
                      "F3 %s %s: inside 70 %% of the canvas (CRT overscan), card x %d w %d", mn.c_str(), name, f.card.x, f.card.w);
            CHECK(f.card.y > int(L.H) / 2 && f.card.bottom() <= L.safe.bottom() &&
                      std::abs((f.card.x + f.card.w / 2) - (L.safe.x + L.safe.w / 2)) <= 1,
                  "F5 FIELD-1018 %s %s: the status box bottom centre in title-safe (y %d h %d, safe bottom %d)",
                  mn.c_str(), name, f.card.y, f.card.h, L.safe.bottom());
            CHECK(f.cardFont.sy == f.font.sy && f.cardFont.sx256 >= 384 && f.cardFont.sx256 <= f.font.sx256,
                  "F4 %s %s: only the columns narrow (rows whole line pairs), >= 1.5 px a column", mn.c_str(), name);
        }
    const Raster il = mk(720, 480, true, H264RING_VOUT_INTERLACED, 4, 3, 0, 0, 720, 480);
    UiState s;
    s.idle = true;
    s.idleName = "MiSTerPlex-192.0.2.110";
    const Layout L = layoutFor(il);
    const Frame f = planFrame(s, L);
    Surface S;
    paint(S, s, L, f);
    const uint8_t* px = S.at(8, 8);
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0xFF,
          "F5 FIELD-1018 F3: the idle field is opaque black (not the old blue-grey, never the frame behind)");
    const uint8_t* cp = S.at(f.card.x + 4, f.card.y + 4);
    CHECK(cp[3] == 0xFF && cp[0] == kPanelOnBlack.b && cp[1] == kPanelOnBlack.g && cp[2] == kPanelOnBlack.r,
          "F5 FIELD-1018 F3: the card is an opaque clean fill (alpha %u): no last frame / pattern through it", cp[3]);
}

void testIdleModel() {
    UiModel m;
    m.idle = true;
    m.idleName = "MiSTer Den";
    UiState s = m.resolve(0);
    CHECK(s.idle && s.idleName == "MiSTer Den" && s.anyVisible() && s.bar == Bar::Hidden, "I1 idle: the card, no bar");
    m.error = ErrorScreen::StreamError;
    m.errorUntilMs = 1000;
    CHECK(m.resolve(500).error == ErrorScreen::StreamError && !m.resolve(500).idle && m.nextChange(500) == 1000,
          "I1 a timed error screen covers the card until it expires (next change %lld)",
          static_cast<long long>(m.nextChange(500)));
    CHECK(m.resolve(1000).error == ErrorScreen::None && m.resolve(1000).idle, "I1 then back to the idle card");
    m.presented = true;
    CHECK(!m.resolve(2000).idle, "I1 a play's picture: no card");
    UiBus b;
    b.setIdle(true, "N", "Connecting...");
    b.setErrorFor(100, ErrorScreen::PmsUnreachable, "pms", 50);
    UiModel q = b.snapshot();
    CHECK(q.idle && q.idleHint == "Connecting..." && q.errorUntilMs == 150 && q.error == ErrorScreen::PmsUnreachable,
          "I1 UiBus: setIdle / setErrorFor");
    b.clearError();
    CHECK(b.snapshot().error == ErrorScreen::None && b.snapshot().resolve(0).idle, "I1 clearError: the card again");
}

int panelPixels(PlayRig& r) {   // the shown page's pixels with the panel's alpha
    const struct ::h264ring_overlay d = r.core->ovl;
    if (d.base < H264RING_FB_BYTE || !r.core->ovlEnabled)
        return 0;
    int n = 0;
    for (unsigned y = d.band_first; y <= d.band_last; ++y) {
        const uint8_t* row = r.fbmem.data() + (d.base - H264RING_FB_BYTE) + size_t(y) * d.stride;
        for (unsigned x = 0; x < d.width; ++x)
            n += row[4 * x + 3] == kPanel.a ||
                 (row[4 * x + 0] == kPanelOnBlack.b && row[4 * x + 1] == kPanelOnBlack.g && row[4 * x + 2] == kPanelOnBlack.r);
    }
    return n;
}

void testIdlePlayer() {
    {
        PlayRig r(true);
        r.keepCore = true;
        std::string err;
        CHECK(r.idle(err), "I2 startIdle (%s)", err.c_str());
        CHECK(waitFor([&] { return r.core->sess && r.core->ovlState == H264RING_OVL_S_ACCEPTED && panelPixels(r) > 1000; }),
              "I2 the idle session shows the idle card without a play (state %u, %d panel px)", r.core->ovlState,
              panelPixels(r));
        CHECK(r.player->idleSessionOk() && r.player->idleStarts() == 1 && r.windows == 1 && r.maps == 1 &&
                  r.core->sessions.size() == 1 && r.core->all().empty(),
              "I2 one idle session, no stream (starts %u, windows %d, maps %d, records %zu)", r.player->idleStarts(),
              r.windows.load(), r.maps.load(), r.core->all().size());
        r.core->ack = 0;   // I3: an OSD Reset: the core drops this handle's session
        r.core->sess = false;
        CHECK(waitFor([&] { return r.player->idleStarts() == 2 && r.player->idleSessionOk(); }),
              "I3 the keeper starts the lost idle session again (starts %u)", r.player->idleStarts());
        CHECK(waitFor([&] { return r.core->ovlEnabled && r.core->ovlState == H264RING_OVL_S_ACCEPTED && panelPixels(r) > 1000; }),
              "I3 the card is back (the session start re-sends the handle's overlay)");
        {   // FIELD-1018 F3: OSD Video Output -> Native while idle, the session still up: re-arm + repaint
            std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the keeper records the armed raster
            const unsigned starts = r.player->idleStarts(), rearms = r.player->idleRearms(), maps0 = r.maps.load();
            r.core->rasterW = 1280;
            r.core->rasterH = 720;
            r.core->videoOut = H264RING_VOUT_NATIVE;
            r.core->arX = 16;
            r.core->arY = 9;
            CHECK(waitFor([&] { return r.player->idleRearms() == rearms + 1; }) && r.player->idleStarts() == starts + 2,
                  "N3 a Native switch re-arms the idle card once (two session primes) without a lost session (rearms %u starts %u)",
                  r.player->idleRearms(), r.player->idleStarts());
            CHECK(waitFor([&] { return r.core->ovl.width == 1280 && r.core->ovlState == H264RING_OVL_S_ACCEPTED &&
                                       panelPixels(r) > 1000 && r.maps.load() == maps0; }),
                  "N3 the card repainted from scratch on the 1280x720 raster (width %u, maps %d)",
                  unsigned(r.core->ovl.width), r.maps.load());
            const struct ::h264ring_overlay d = r.core->ovl;
            const uint8_t* row = r.fbmem.data() + (d.base - H264RING_FB_BYTE) + size_t(d.band_first) * d.stride;
            CHECK(d.band_first == 0 && row[0] == 0 && row[1] == 0 && row[2] == 0 && row[3] == 0xFF,
                  "N3 the whole raster painted opaque black behind the card after the switch (band %u)", d.band_first);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(r.player->idleRearms() == rearms + 1, "N3 one re-arm per switch, no loop");
            r.core->rasterW = 720;
            r.core->rasterH = 480;
            r.core->videoOut = H264RING_VOUT_PROGRESSIVE;
            r.core->arX = 4;
            r.core->arY = 3;
            CHECK(waitFor([&] { return r.player->idleRearms() == rearms + 2 && r.core->ovl.width == 720; }),
                  "N3 back to Progressive 480p: re-armed again");
        }
        CHECK(r.start() && waitFor([&] { return r.player->started(); }), "I4 a play from the idle session");
        CHECK(r.windows == 1 && r.maps == 1 && !r.player->ui().snapshot().idle,
              "I4 the play reuses the handle and fb0 (windows %d maps %d), the card off", r.windows.load(), r.maps.load());
        r.player->stop();
        CHECK(r.windows == 1 && r.unmaps == 0 && r.player->osdRunning() && r.player->ui().snapshot().idle &&
                  r.player->idleWanted(),
              "I4 stop: back to the idle card, the handle and fb0 kept (unmaps %d)", r.unmaps.load());
        CHECK(waitFor([&] { return r.player->idleSessionOk() && (ctrlLo(r, H264RING_W_OVL_CTL) & 1u) && panelPixels(r) > 1000; }),
              "I4 the card shown on the play's session, now the idle one");
        CHECK(r.start() && waitFor([&] { return r.core->osdAnalogAspect == 2u; }), "I5 Letterbox on the play");
        r.core->settingsTake = false;   // I5: the restore at stop is not taken
        r.player->stop();
        CHECK(r.player->lastAspectEnd() == AspectEnd::Pending && r.store->has, "I5 the restore pending at stop");
        const unsigned lb = r.player->aspectStats().letterboxRequests;
        r.core->settingsTake = true;
        CHECK(waitFor([&] { return !r.store->has && r.core->osdAnalogAspect == 0u; }),
              "I5 the idle session restored the user's Auto (menu %u)", r.core->osdAnalogAspect);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(r.core->osdAnalogAspect == 0u && r.player->aspectStats().letterboxRequests == lb &&
                  ctrlLo(r, H264RING_W_SETTINGS) == 0,
              "I5 no Letterbox on the idle session; the word cleared");
        r.player->stopIdle();
        CHECK(r.unmaps == 1 && !r.player->osdRunning() && (ctrlLo(r, H264RING_W_OVL_CTL) & 1u) == 0 &&
                  !r.player->idleWanted(),
              "I6 stopIdle (a hop off OB): the overlay hidden, fb0 unmapped, the handle closed");
    }
    {
        PlayRig r(true);
        r.deadCore = true;
        std::string err;
        CHECK(!r.idle(err) && err.find("not alive") != std::string::npos && !r.player->idleWanted() && r.maps == 0,
              "I7 a dead core: startIdle fails (%s), nothing mapped, the caller retries", err.c_str());
    }
}

std::string body(const std::string& src, const std::string& sig) {
    const size_t a = src.find(sig);
    if (a == std::string::npos)
        return {};
    const size_t b = src.find("\n}\n", a);
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// S1-S3: source rows: the OB route uses the renderer and fb0 through h264ring_fb_map only.
// 0010: the input region through OwenbPlayer + OwenbRing on the fake core; the library page in the model
// and the frame (the idle card waits under it); the panel's view through the bus.
void testObInput() {
    {
        PlayRig r(true);
        r.keepCore = true;
        struct ::h264ring_input in {};
        CHECK(r.player->readInput(in) == -ENODEV, "N1 no handle: -ENODEV");
        std::string err;
        CHECK(r.idle(err), "N1 startIdle (%s)", err.c_str());
        misterplex::owenb::ObInputDecoder dec;
        CHECK(r.player->readInput(in) == 0, "N1 the idle session's handle reads the region");
        dec.feed(in, 0);
        struct ::h264ring_input w {};
        auto ev = [&](uint32_t n, uint16_t e) {   // event n at its queue slot (h264ring.h:386, 0x38 + n/4)
            const unsigned slot = n % H264RING_IN_Q_LEN;
            w.w[H264RING_IN_Q_WORD - H264RING_IN_WORD + slot / 4] |= uint64_t(e) << (16 * (slot % 4));
            w.w[0] = uint64_t(n + 1) << 32;
        };
        ev(0, uint16_t((H264RING_EV_KEY << H264RING_EV_TYPE_LSB) | 1u << 9 | 1u << 8 | 0x72));   // Down pressed
        ev(1, uint16_t((H264RING_EV_OSD << H264RING_EV_TYPE_LSB) | 1));
        ev(2, uint16_t((H264RING_EV_ACTION << H264RING_EV_TYPE_LSB) | 1));                     // Program menu
        ev(3, uint16_t((H264RING_EV_OSD << H264RING_EV_TYPE_LSB) | 0));
        r.core->writeInput(w);
        CHECK(r.player->readInput(in) == 0, "N2 a copy after the core's write");
        std::string got;
        for (auto n : dec.feed(in, 15))
            got += std::string(got.empty() ? "" : ",") + misterplex::owenb::obNavName(n);
        w.w[H264RING_PAD0_JOY_W - H264RING_IN_WORD + 2] = H264RING_PAD_A;   // then pad 2's A, after the OSD closed
        r.core->writeInput(w);
        CHECK(r.player->readInput(in) == 0 && r.core->inSeq == 2, "N2 the second write read whole");
        for (auto n : dec.feed(in, 30))
            got += std::string(got.empty() ? "" : ",") + misterplex::owenb::obNavName(n);
        CHECK(got == "down,menu,select", "N2 the core's key, OSD action and pad reach the decoder (%s)", got.c_str());
        // the panel through the bus, then the library page: the idle card waits under the page
        misterplex::owenb::ObInputRouter rt;
        misterplex::owenb::ObMenuState st;
        st.libraryAvailable = true;
        bool ch = false;
        rt.handle(misterplex::owenb::ObNav::Menu, st, ch);
        r.player->ui().setMenu(rt.menu().view());
        CHECK(waitFor([&] { return r.player->ui().snapshot().menu.open; }), "N3 the panel's view on the bus");
        CHECK(rt.handle(misterplex::owenb::ObNav::Select, st, ch) == misterplex::PlaybackCommand::Browse,
              "N3 Browse the library -> the daemon's Browse");
        r.player->ui().setMenu(rt.menu().view());
        std::vector<misterplex::LibraryRow> rows(3);
        rows[0].title = "Movies";
        r.player->ui().setLibrary(misterplex::owenb::obLibraryPage(true, rows, 0, "", "Plex"));
        const UiState s = r.player->ui().snapshot().resolve(0);
        CHECK(s.library.open && s.idle && !s.menu.open, "N4 the page and the idle session resolve together, the panel closed");
        const Layout L = layoutFor(modes()[0].r);
        const Frame f = planFrame(s, L);
        CHECK(!f.lib.empty() && f.card.empty() && f.lib.y == L.safe.y && f.lib.h == L.safe.h,
              "N4 the page takes title-safe; no idle card under it");
        CHECK(f.idleField, "N4 idle library covers the raster so the core pattern stays hidden");
        Surface libS;
        paint(libS, s, L, f);
        const uint8_t* edge = libS.at(2, 2);
        CHECK(edge[0] == 0 && edge[1] == 0 && edge[2] == 0 && edge[3] == 0xFF,
              "N4 the field around the page is opaque black");
        const uint8_t* panel = libS.at(f.lib.x + 4, f.lib.y + 4);
        CHECK(panel[3] == 0xFF && panel[0] == kPanelOnBlack.b && panel[1] == kPanelOnBlack.g &&
                  panel[2] == kPanelOnBlack.r,
              "N4 the page is an opaque fill; the core pattern does not show through");
        r.player->ui().setLibrary(misterplex::owenb::obLibraryPage(false, rows, 0, "", "Plex"));
        const UiState s2 = r.player->ui().snapshot().resolve(0);
        CHECK(!s2.library.open && s2.idle && !planFrame(s2, L).card.empty(), "N4 the page closed: the card is back");
        r.player->stopIdle();
        CHECK(r.player->readInput(in) == -ENODEV, "N5 the idle session closed: no handle");
    }
}

void testSourceRows() {
    const std::string mp = readSrc("arm/misterplexd/media_player.cpp");
    const std::string route = body(mp, "bool MediaPlayer::playOwenbRoute(");
    const std::string cfg = body(mp, "static owenb::OsdConfig owenbOsdConfigFromEnv(");
    const std::string ens = body(mp, "void MediaPlayer::ensureOwenbPlayerLocked(");
    CHECK(!route.empty() && route.find("ensureOwenbPlayerLocked()") != std::string::npos &&
              ens.find("setOsd(owenbOsdConfigFromEnv())") != std::string::npos,
          "S1 playOwenbRoute uses the OB player that arms the overlay renderer");
    for (const char* bad : {"FbPresent", "fbPresent", "OverlayPublisher", "ioctl", "0x31600000", "fb_->"})
        CHECK(ens.find(bad) == std::string::npos, "S1 the OB player's setup never uses %s", bad);
    for (const char* bad : {"FbPresent", "fbPresent", "OverlayPublisher", "ioctl", "0x31600000", "fb_->"})
        CHECK(route.find(bad) == std::string::npos && cfg.find(bad) == std::string::npos,
              "S1 the OB route / overlay config never uses %s", bad);
    CHECK(cfg.find("h264ring_fb_map") != std::string::npos && cfg.find("MPX_OB_OSD") != std::string::npos &&
              cfg.find("MPX_OB_OSD_SINGLE") != std::string::npos,
          "S2 fb0 through h264ring_fb_map; MPX_OB_OSD / MPX_OB_OSD_SINGLE");
    const std::string acfg = body(mp, "static owenb::AspectConfig owenbAspectConfigFromEnv(");
    CHECK(ens.find("ob_->setAspect(owenbAspectConfigFromEnv())") != std::string::npos &&
              acfg.find("MPX_OB_LETTERBOX") != std::string::npos && acfg.find("MPX_OB_ASPECT_STATE") != std::string::npos &&
              acfg.find("FileAspectStore") != std::string::npos,
          "S4 0006: the OB player gets the aspect guard with the state file (MPX_OB_LETTERBOX / MPX_OB_ASPECT_STATE)");
    // S5 (T7): the idle session follows the gate; error screens on it; main feeds the name and the PMS probe
    const std::string gt = body(mp, "void MediaPlayer::coreGateTick(");
    const std::string it = body(mp, "void MediaPlayer::owenbIdleTick(");
    const std::string pe = body(mp, "void MediaPlayer::owenbPlayEnded(");
    CHECK(gt.find("owenbIdleTick();") != std::string::npos, "S5 coreGateTick runs the OB idle tick");
    CHECK(it.find("CoreGate::process().owenbOpen()") != std::string::npos && it.find("startIdle(") != std::string::npos &&
              it.find("stopIdle()") != std::string::npos && it.find("MPX_OB_IDLE") != std::string::npos &&
              it.find("PmsUnreachable") != std::string::npos,
          "S5 the idle tick: armed on a verified PlexOB core, closed off it, MPX_OB_IDLE, the PMS screen");
    for (const char* bad : {"FbPresent", "ioctl", "0x31600000", "paintIdle"})
        CHECK(it.find(bad) == std::string::npos, "S5 the idle tick never uses %s", bad);
    CHECK(pe.find("ErrorScreen::StreamError") != std::string::npos && pe.find("ErrorScreen::Refused") != std::string::npos,
          "S5 a play's error ends on an error screen (StreamError / Refused)");
    const std::string mainSrc = readSrc("arm/misterplexd/main.cpp");
    CHECK(mainSrc.find("player.setObIdleName(name)") != std::string::npos &&
              mainSrc.find("player.setObPmsState(") != std::string::npos,
          "S5 main.cpp: the cast name and the PMS probe reach the OB idle card");
    // S6 (0008): the play's input reports the container's aspect; the player sniffs the first keyframe
    const std::string avs = readSrc("arm/misterplexd/owenb_av_source.cpp");
    const std::string pl = readSrc("host/libmisterplex/owenb_play.cpp");
    CHECK(avs.find("bool containerAspect(int64_t& num, int64_t& den) const override") != std::string::npos &&
              avs.find("vs->sample_aspect_ratio") != std::string::npos,
          "S6 the libav input reports the container's display aspect (stream SAR, not the codec's VUI)");
    CHECK(pl.find("std::make_unique<AspectSniffSource>") != std::string::npos &&
              pl.find("aspect_.newSession(true)") != std::string::npos && pl.find("owenbStreamAspect(") != std::string::npos,
          "S6 every play input goes through the aspect sniffer; the play's request waits for it");
    // S7 (0009, G15): fbcon is detached around our fb0 pages, attached again after; a dead daemon's left-over at start
    {
        const std::string oc = body(mp, "static owenb::OsdConfig owenbOsdConfigFromEnv(");
        const size_t acq = oc.find("fbcon->acquire()"), map = oc.find("h264ring_fb_map(fb.get())"),
                     unm = oc.find("h264ring_fb_unmap(fb.get())"), rel = oc.rfind("fbcon->release()");
        CHECK(acq != std::string::npos && map != std::string::npos && acq < map && unm != std::string::npos &&
                  rel != std::string::npos && unm < rel,
              "S7 mapFb detaches fbcon before h264ring_fb_map; unmapFb attaches it after h264ring_fb_unmap");
        CHECK(readSrc("arm/misterplexd/main.cpp").find("player.owenbFbconAtStart()") != std::string::npos,
              "S7 main gives back a console a dead daemon left detached");
    }
    // S8 (0010): the OB input thread drives the daemon's own dispatch; the browser's repaint feeds the page
    {
        const std::string loop = body(mp, "void MediaPlayer::obInputLoop(");
        const std::string pub = body(mp, "void MediaPlayer::obLibraryPublish(");
        const std::string pi = body(mp, "void MediaPlayer::paintIdle(");
        const std::string sd = body(mp, "void MediaPlayer::shutdown(");
        CHECK(loop.find("readInput(in)") != std::string::npos && loop.find("dec.feed(") != std::string::npos &&
                  loop.find("router.handle(") != std::string::npos && loop.find("dispatchPlaybackInput(cmd)") != std::string::npos &&
                  loop.find("setMenu(router.menu().view())") != std::string::npos,
              "S8 the input loop: region -> decoder -> router -> dispatchPlaybackInput, the panel on the bus");
        CHECK(it.find("startObInput()") != std::string::npos && sd.find("stopObInput()") != std::string::npos &&
                  body(mp, "void MediaPlayer::startObInput(").find("MPX_OB_INPUT") != std::string::npos,
              "S8 started with the idle session, stopped at shutdown, MPX_OB_INPUT");
        const size_t gate = pi.find("ownsIdleInFabric()"), p = pi.find("obLibraryPublish()");
        CHECK(gate != std::string::npos && p != std::string::npos && p < gate &&
                  pub.find("CoreGate::process().ownsIdleInFabric()") != std::string::npos,
              "S8 paintIdle publishes the library page before the OB early return; the page only on a PlexOB core");
        CHECK(pub.find("browse_->r1View()") != std::string::npos && pub.find("obLibraryPage(library_") != std::string::npos &&
                  pub.find("ob->active()") != std::string::npos && pub.find("setLibrary(") != std::string::npos,
              "S8 the page from the V2 browser or the legacy list, hidden over a play");
        for (const char* bad : {"FbPresent", "ioctl", "0x31600000", "fb_->", "/dev/input"})
            CHECK(loop.find(bad) == std::string::npos && pub.find(bad) == std::string::npos,
                  "S8 the input loop / the page never use %s", bad);
    }
    for (const char* f : {"host/libmisterplex/owenb_aspect_guard.hpp", "host/libmisterplex/owenb_aspect.hpp"}) {
        const std::string g = readSrc(f);
        const size_t m = g.find("req.mask =");
        CHECK(!g.empty() && g.find("H264RING_OPT_VIDEO_OUTPUT") == std::string::npos &&
                  (m == std::string::npos || g.find("VIDEO_OUTPUT", m) == std::string::npos),
              "S4 0006: %s never asks for Video Output", f);
    }
    for (const char* f : {"host/libmisterplex/owenb_osd_layout.hpp", "host/libmisterplex/owenb_osd_draw.hpp",
                          "host/libmisterplex/owenb_osd_renderer.hpp"}) {
        const std::string s = readSrc(f);
        CHECK(!s.empty() && s.find("open(") == std::string::npos && s.find("mmap") == std::string::npos &&
                  s.find("FbPresent") == std::string::npos,
              "S3 %s maps nothing itself (fb0 comes from h264ring_fb_map)", f);
    }
}
}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dump" && i + 1 < argc)
            g_dump = argv[++i];
        else if (std::string(argv[i]) == "--update-goldens")
            g_update = true;
    }
    testLayout();
    testFlipsDouble();
    testFlipEdges();
    testIdleAfterUntaken();
    testSingle();
    testRelayoutHide();
    testAspect();
    testAspect43();
    testGoldens();
    testPlayer();
    testPlayAspect43();
    testIdleModel();
    testIdlePlayer();
    testIdleFit();
    testObInput();
    testSourceRows();
    std::printf("test_owenb_osd: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
