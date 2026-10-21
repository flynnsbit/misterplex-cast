#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace {
struct Spec {
    int width;
    int height;
    int fps;
    int coded_width = 0;
};

constexpr double kWorstBlackoutUs = 500.0;
constexpr double kBlackoutPeriodUs = 5000.0;
constexpr double kBlackoutPhaseUs = 1000.0;
constexpr double kLineReadLatencyCycles = 128.0; // Conservative bridge response latency before burst data.

uint16_t pattern(int bank, int row, int col, int width) {
    return static_cast<uint16_t>((bank ? 0x8000 : 0x1000) ^ (row * width + col));
}

struct Line {
    bool valid = false;
    int bank = -1;
    int y = -1;
    std::vector<uint16_t> pix;
};

void load_line(const Spec& s, Line& line, int bank, int y) {
    line.valid = true;
    line.bank = bank;
    line.y = y;
    line.pix.resize(s.width);
    for (int x = 0; x < s.width; ++x)
        line.pix[x] = pattern(bank, y, x, s.width);
}

int simulate_broken_single_set(const Spec& s) {
    constexpr int kLines = 4;
    std::vector<Line> lines(kLines);
    int disp_bank = 0;
    bool swap_pending = false;
    int mismatches = 0;

    for (int y = 0; y < s.height; ++y) {
        if (y == s.height / 2) {
            swap_pending = true;
            for (auto& line : lines)
                line.valid = false;
        }
        const int prefetch_bank = swap_pending ? !disp_bank : disp_bank;
        for (int ahead = 0; ahead < kLines; ++ahead)
            load_line(s, lines[ahead], prefetch_bank, std::min(y + ahead, s.height - 1));
        for (int x = 0; x < s.width; ++x) {
            const uint16_t got = (lines[0].valid && lines[0].y == y) ? lines[0].pix[x] : 0;
            const uint16_t want = pattern(disp_bank, y, x, s.width);
            if (got != want)
                ++mismatches;
        }
    }
    return mismatches;
}

int simulate_fixed_dual_set(const Spec& s, int line_count) {
    std::vector<std::vector<Line>> sets(2, std::vector<Line>(line_count));
    int disp_bank = 0;
    int disp_set = 0;
    bool swap_pending = false;
    int mismatches = 0;

    for (int y = 0; y < s.height; ++y) {
        if (y == s.height / 2)
            swap_pending = true;
        for (int ahead = 0; ahead < line_count; ++ahead)
            load_line(s, sets[disp_set][ahead], disp_bank, std::min(y + ahead, s.height - 1));
        if (swap_pending) {
            const int prep_set = !disp_set;
            for (int ahead = 0; ahead < line_count; ++ahead)
                load_line(s, sets[prep_set][ahead], !disp_bank, ahead);
        }
        for (int x = 0; x < s.width; ++x) {
            const auto& line = sets[disp_set][0];
            const uint16_t got = (line.valid && line.bank == disp_bank && line.y == y) ? line.pix[x] : 0;
            const uint16_t want = pattern(disp_bank, y, x, s.width);
            if (got != want)
                ++mismatches;
        }
    }
    return mismatches;
}

bool in_blackout(double t) {
    if (t < kBlackoutPhaseUs)
        return false;
    const double rel = std::fmod(t - kBlackoutPhaseUs, kBlackoutPeriodUs);
    return rel >= 0.0 && rel < kWorstBlackoutUs;
}

double blackout_end(double t) {
    const double rel = std::fmod(t - kBlackoutPhaseUs, kBlackoutPeriodUs);
    return t + (kWorstBlackoutUs - rel);
}

double next_blackout_start(double t) {
    if (t < kBlackoutPhaseUs)
        return kBlackoutPhaseUs;
    const double periods = std::floor((t - kBlackoutPhaseUs) / kBlackoutPeriodUs) + 1.0;
    return kBlackoutPhaseUs + periods * kBlackoutPeriodUs;
}

double service_done_after_blackouts(double start, double service_us) {
    double t = start;
    double remaining = service_us;
    while (remaining > 1e-9) {
        if (in_blackout(t)) {
            t = blackout_end(t);
            continue;
        }
        const double next_stop = next_blackout_start(t);
        const double run = std::min(remaining, next_stop - t);
        t += run;
        remaining -= run;
    }
    return t;
}

struct PrefetchResult {
    int underruns = 0;
    int first_underrun_line = -1;
};

PrefetchResult simulate_ddr_prefetch(const Spec& s, int line_count, double ddr_clk_mhz) {
    const double line_period_us = 1000000.0 / (static_cast<double>(s.height) * s.fps);
    const int coded_width = s.coded_width ? s.coded_width : s.width;
    // YUV420p: one luma line (W bytes) plus one U and one V chroma line
    // amortized over each pair of luma lines: W/8 + W/16 qwords per source row.
    const double line_qwords = static_cast<double>(coded_width) * 3.0 / 16.0;
    const double service_us = (kLineReadLatencyCycles + line_qwords) / ddr_clk_mhz;

    std::map<int, double> complete_at;
    std::set<int> scheduled;
    for (int y = 0; y < std::min(line_count, s.height); ++y) {
        complete_at[y] = -1.0; // pending bank warmed before VSync.
        scheduled.insert(y);
    }

    double ddr_free_us = 0.0;
    PrefetchResult result;

    for (int y = 0; y < s.height; ++y) {
        const double now = y * line_period_us;
        auto it = complete_at.find(y);
        if (it == complete_at.end() || it->second > now) {
            ++result.underruns;
            if (result.first_underrun_line < 0)
                result.first_underrun_line = y;
        }

        for (int ahead = 0; ahead < line_count; ++ahead) {
            const int target = std::min(y + ahead, s.height - 1);
            if (scheduled.count(target))
                continue;
            const double start = std::max(ddr_free_us, now);
            const double done = service_done_after_blackouts(start, service_us);
            ddr_free_us = done;
            scheduled.insert(target);
            complete_at[target] = done;
        }
    }
    return result;
}

bool expect_clean(const Spec& s, int lines, double ddr_clk_mhz) {
    const auto r = simulate_ddr_prefetch(s, lines, ddr_clk_mhz);
    if (r.underruns != 0) {
        std::fprintf(stderr,
                     "%dx%d@%d coded=%d %.0fMHz line_count=%d: expected clean, underruns=%d first_line=%d\n",
                     s.width, s.height, s.fps, s.coded_width ? s.coded_width : s.width,
                     ddr_clk_mhz, lines, r.underruns, r.first_underrun_line);
        return false;
    }
    std::printf("test_frame_store_ddr_prefetch_sim: YUV420p %dx%d@%d coded=%d %.0fMHz line_count=%d OK under %.0fus DDR blackout\n",
                         s.width, s.height, s.fps, s.coded_width ? s.coded_width : s.width,
                         ddr_clk_mhz, lines, kWorstBlackoutUs);
    return true;
}

bool expect_underrun(const Spec& s, int lines, double ddr_clk_mhz) {
    const auto r = simulate_ddr_prefetch(s, lines, ddr_clk_mhz);
    if (r.underruns == 0) {
        std::fprintf(stderr, "%dx%d@%d coded=%d %.0fMHz line_count=%d: expected underrun under %.0fus blackout\n",
                     s.width, s.height, s.fps, s.coded_width ? s.coded_width : s.width,
                     ddr_clk_mhz, lines, kWorstBlackoutUs);
        return false;
    }
    std::printf("test_frame_store_ddr_prefetch_sim: YUV420p %dx%d@%d coded=%d %.0fMHz line_count=%d expected underrun=%d first_line=%d\n",
                s.width, s.height, s.fps, s.coded_width ? s.coded_width : s.width,
                ddr_clk_mhz, lines, r.underruns, r.first_underrun_line);
    return true;
}
} // namespace

int main() {
    const Spec s30{640, 480, 30, 624};
    const int broken = simulate_broken_single_set(s30);
    const int want_broken = s30.width * s30.height / 2;
    if (broken != want_broken) {
        std::fprintf(stderr, "expected old single-set mismatches=%d, got %d\n", want_broken, broken);
        return 1;
    }
    const int fixed = simulate_fixed_dual_set(s30, 8);
    if (fixed != 0) {
        std::fprintf(stderr, "fixed dual-set ownership mismatches=%d\n", fixed);
        return 1;
    }
    std::printf("test_frame_store_ddr_prefetch_sim: page-flip ownership OK (old mismatches=%d; fixed clean)\n",
                broken);

    bool ok = true;
    ok &= expect_clean(Spec{640, 480, 24, 624}, 8, 20.0);
    ok &= expect_underrun(Spec{640, 480, 30, 624}, 4, 20.0);
    ok &= expect_clean(Spec{640, 480, 30, 624}, 8, 20.0);
    ok &= expect_underrun(Spec{640, 480, 60, 624}, 8, 20.0);
    ok &= expect_clean(Spec{640, 480, 60, 624}, 16, 20.0);

    ok &= expect_clean(Spec{640, 480, 30, 624}, 8, 80.0);
    ok &= expect_underrun(Spec{640, 480, 60, 624}, 8, 80.0);
    ok &= expect_clean(Spec{640, 480, 60, 624}, 16, 80.0);
    ok &= expect_underrun(Spec{1280, 720, 30}, 8, 80.0);
    ok &= expect_clean(Spec{1280, 720, 30}, 16, 80.0);

    ok &= expect_clean(Spec{640, 480, 30, 624}, 8, 85.0);
    ok &= expect_underrun(Spec{640, 480, 60, 624}, 8, 85.0);
    ok &= expect_clean(Spec{640, 480, 60, 624}, 16, 85.0);
    ok &= expect_underrun(Spec{1280, 720, 30}, 8, 85.0);
    ok &= expect_clean(Spec{1280, 720, 30}, 16, 85.0);

    ok &= expect_clean(Spec{640, 480, 30, 624}, 8, 90.0);
    ok &= expect_underrun(Spec{640, 480, 60, 624}, 8, 90.0);
    ok &= expect_clean(Spec{640, 480, 60, 624}, 16, 90.0);
    ok &= expect_underrun(Spec{1280, 720, 30}, 8, 90.0);
    ok &= expect_clean(Spec{1280, 720, 30}, 16, 90.0);

    ok &= expect_clean(Spec{640, 480, 30, 624}, 8, 100.0);
    ok &= expect_underrun(Spec{640, 480, 60, 624}, 8, 100.0);
    ok &= expect_clean(Spec{640, 480, 60, 624}, 16, 100.0);
    ok &= expect_underrun(Spec{1280, 720, 30}, 8, 100.0);
    ok &= expect_clean(Spec{1280, 720, 30}, 16, 100.0);
    return ok ? 0 : 1;
}
