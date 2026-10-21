#pragma once
// Cheap I420 luma score for idle/stick bank logs. Not a glass PASS.

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace misterplex {

struct I420YScore {
    double mean = 0;
    double var = 0;
    int sample = 0;
    int zero = 0;
    int dark = 0;
    int amber = 0;
};

inline I420YScore scoreI420Y(const uint8_t* y, int w, int h, int step = 4) {
    I420YScore s;
    if (!y || w <= 0 || h <= 0)
        return s;
    if (step < 1)
        step = 1;
    double sum = 0;
    double sum2 = 0;
    for (int row = 0; row < h; row += step) {
        const uint8_t* p = y + static_cast<size_t>(row) * static_cast<size_t>(w);
        for (int x = 0; x < w; x += step) {
            const int v = p[x];
            sum += v;
            sum2 += static_cast<double>(v) * v;
            ++s.sample;
            if (v == 0)
                ++s.zero;
            if (v < 16)
                ++s.dark;
            if (v >= 40 && v <= 90)
                ++s.amber;
        }
    }
    if (s.sample > 0) {
        s.mean = sum / s.sample;
        s.var = sum2 / s.sample - s.mean * s.mean;
        if (s.var < 0)
            s.var = 0;
    }
    return s;
}

inline void formatStickBankScore(char* buf, size_t n, const I420YScore& s) {
    if (!buf || n == 0)
        return;
    std::snprintf(buf, n,
                  "y_mean=%.0f var=%.0f zero=%d/%d dark=%d amber=%d empty=%d chevronish=%d",
                  s.mean, s.var, s.zero, s.sample, s.dark, s.amber,
                  (s.mean < 8.0 && s.var < 20.0) ? 1 : 0,
                  (s.amber > 200 && s.mean > 20.0 && s.mean < 80.0) ? 1 : 0);
}

} // namespace misterplex
