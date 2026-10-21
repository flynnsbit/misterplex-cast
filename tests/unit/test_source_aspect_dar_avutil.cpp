// DAR-TRUNC canonical criterion (review 17:40): foldSourceAspect is bit-exact with FFmpeg
// av_reduce(&n, &d, x, y, kSourceAspectMax), the call the demux paths make. Built only when
// pkg-config finds libavutil (Makefile AVUTIL_LIBS); otherwise make unit prints SKIP.
#include "libmisterplex/source_aspect.hpp"
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include <cstdint>
#include <cstdio>
#include <random>

using namespace misterplex;

int main() {
    std::mt19937_64 rng(0xAB12);
    uint64_t pairs = 0, bad = 0;
    auto one = [&](int64_t x, int64_t y) {
        int n = 0, d = 0;
        av_reduce(&n, &d, x, y, kSourceAspectMax);
        const auto f = foldSourceAspect(x, y);
        const bool avValid = n > 0 && d > 0;
        const bool same = f.valid ? (avValid && f.x == n && f.y == d) : !avValid;
        if (!same && bad++ < 10)
            std::fprintf(stderr, "MISMATCH %lld:%lld av_reduce=%d:%d fold=%s\n", (long long)x,
                         (long long)y, n, d,
                         f.valid ? (std::to_string(f.x) + ":" + std::to_string(f.y)).c_str() : "invalid");
        ++pairs;
    };
    for (int64_t x = 1; x <= 8300; x += 3)                    // around the 4095 bound
        for (int64_t y = 1; y <= 8300; y += 1 + (x % 13))
            one(x, y);
    for (int i = 0; i < 1000000; ++i)                          // 16-bit wire terms
        one(int64_t(rng() % 65535) + 1, int64_t(rng() % 65535) + 1);
    for (int i = 0; i < 1000000; ++i)                          // demux products up to 2^31-1
        one(int64_t(rng() % ((1ull << 31) - 1)) + 1, int64_t(rng() % ((1ull << 31) - 1)) + 1);
    const unsigned v = avutil_version();
    std::printf("source aspect DAR vs av_reduce: libavutil %u.%u.%u pairs=%llu mismatches=%llu\n",
                v >> 16, (v >> 8) & 255, v & 255, (unsigned long long)pairs, (unsigned long long)bad);
    if (bad) return 1;
    std::printf("PASS source aspect DAR fold == av_reduce(..., 4095)\n");
    return 0;
}
