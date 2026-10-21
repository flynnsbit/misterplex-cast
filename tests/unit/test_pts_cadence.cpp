// Fail-first then green: compressed PTS allow-list includes 30 / 30000/1001.
#include "../../host/libmisterplex/pts_cadence.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

static int fails = 0;
#define EXPECT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

int main() {
    using misterplex::expected_fps_supported;
    using misterplex::pts_delta_is_n_frames;
    using misterplex::pts_delta_is_one_frame;
    using misterplex::pts_long_run_rate_ok;

    EXPECT(expected_fps_supported(0, 0), "unset fps ok");
    EXPECT(expected_fps_supported(24, 1), "24/1 ok");
    EXPECT(expected_fps_supported(24000, 1001), "24000/1001 ok");
    EXPECT(expected_fps_supported(30, 1), "30/1 ok");
    EXPECT(expected_fps_supported(30000, 1001), "30000/1001 ok");
    EXPECT(!expected_fps_supported(25, 1), "25/1 reject");
    EXPECT(!expected_fps_supported(60, 1), "60/1 reject");

    const long double num = 1;
    const long double den = 1200000;
    const long double d24 = den / 24;
    const long double d30 = den / 30;
    const long double d23976 = den * 1001 / 24000;
    const long double d2997 = den * 1001 / 30000;

    EXPECT(pts_delta_is_one_frame(d24, num, den), "24fps delta");
    EXPECT(pts_delta_is_one_frame(d30, num, den), "30fps delta");
    EXPECT(pts_delta_is_one_frame(d23976, num, den), "23.976 delta");
    EXPECT(pts_delta_is_one_frame(d2997, num, den), "29.97 delta");
    EXPECT(!pts_delta_is_one_frame(den / 25, num, den), "25fps delta reject");
    EXPECT(!pts_delta_is_one_frame(den / 60, num, den), "60fps delta reject");

    EXPECT(pts_delta_is_n_frames(d24 * 3, num, den, 48), "3x 24fps skip");
    EXPECT(pts_delta_is_n_frames(d30 * 3, num, den, 48), "3x 30fps skip");
    EXPECT(!pts_delta_is_n_frames(den / 25 * 3, num, den, 48), "3x 25fps skip reject");

    const long double tb90k_num = 1;
    const long double tb90k_den = 90000;

    {
        const long double step = tb90k_den * 1001.0L / 24000.0L;
        long double pts = 0;
        for (uint64_t f = 1; f <= 6600; ++f) {
            pts = std::round(static_cast<long double>(f) * step) +
                  static_cast<long double>(f / 260);
            EXPECT(pts_long_run_rate_ok(pts, f, tb90k_num, tb90k_den, 24000, 1001),
                   "23.976 long-run with drift");
        }
    }

    {
        const long double base = 3003;
        long double pts = 0;
        for (uint64_t f = 1; f <= 6600; ++f) {
            const long double jitter = (f % 2 == 0) ? 1.0L : -1.0L;
            pts += base + jitter;
            EXPECT(pts_long_run_rate_ok(pts, f, tb90k_num, tb90k_den, 30000, 1001),
                   "29.97 long-run with jitter");
        }
    }

    {
        const long double step24 = tb90k_den / 24.0L;
        const long double step30 = tb90k_den / 30.0L;
        EXPECT(pts_long_run_rate_ok(std::round(6000 * step24), 6000, tb90k_num, tb90k_den, 24, 1),
               "exact 24/1 long-run");
        EXPECT(pts_long_run_rate_ok(std::round(6000 * step30), 6000, tb90k_num, tb90k_den, 30, 1),
               "exact 30/1 long-run");
    }

    {
        const long double step25 = tb90k_den / 25.0L;
        bool sawFail = false;
        for (uint64_t f = 1; f <= 200; ++f) {
            const long double pts = std::round(static_cast<long double>(f) * step25);
            if (!pts_long_run_rate_ok(pts, f, tb90k_num, tb90k_den, 24000, 1001))
                sawFail = true;
        }
        EXPECT(sawFail, "25fps judged as 23.976 fails by frame 200");
    }

    {
        const long double step30 = tb90k_den / 30.0L;
        EXPECT(pts_long_run_rate_ok(std::round(100 * step30), 100, tb90k_num, tb90k_den, 30000, 1001),
               "30/1 judged as 29.97 ok at frame 100");
        bool sawFail = false;
        for (uint64_t f = 1; f <= 3000; ++f) {
            const long double pts = std::round(static_cast<long double>(f) * step30);
            if (!pts_long_run_rate_ok(pts, f, tb90k_num, tb90k_den, 30000, 1001))
                sawFail = true;
        }
        EXPECT(sawFail, "30/1 judged as 29.97 fails by frame 3000");
    }

    {
        const long double step23976 = tb90k_den * 1001.0L / 24000.0L;
        bool sawFail = false;
        for (uint64_t f = 1; f <= 3000; ++f) {
            const long double pts = std::round(static_cast<long double>(f) * step23976);
            if (!pts_long_run_rate_ok(pts, f, tb90k_num, tb90k_den, 24, 1))
                sawFail = true;
        }
        EXPECT(sawFail, "23.976 judged as 24/1 fails by frame 3000");
    }

    EXPECT(pts_long_run_rate_ok(999999, 50, tb90k_num, tb90k_den, 24000, 1001),
           "frames < 120 always true");
    EXPECT(!pts_long_run_rate_ok(0, 200, tb90k_num, tb90k_den, 0, 1001), "fps_num<=0 false");
    EXPECT(!pts_long_run_rate_ok(0, 200, tb90k_num, tb90k_den, 24000, 0), "fps_den<=0 false");
    EXPECT(!pts_long_run_rate_ok(0, 200, 0, tb90k_den, 24000, 1001), "tb_num<=0 false");
    EXPECT(!pts_long_run_rate_ok(0, 200, tb90k_num, 0, 24000, 1001), "tb_den<=0 false");

    if (fails) {
        std::fprintf(stderr, "%d pts cadence test(s) failed\n", fails);
        return 1;
    }
    std::printf("test_pts_cadence: OK\n");
    return 0;
}
