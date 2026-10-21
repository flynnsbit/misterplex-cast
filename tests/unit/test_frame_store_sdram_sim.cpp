#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr int kLines = 4;

struct Spec {
    int width;
    int height;
    int stride;
};

int frame_words(const Spec& s) {
    return s.stride * s.height;
}

int pixels(const Spec& s) {
    return s.width * s.height;
}

uint32_t word_addr(const Spec& s, int bank, int row, int col) {
    return static_cast<uint32_t>(bank * frame_words(s) + row * s.stride + col);
}

uint32_t byte_addr(const Spec& s, int bank, int row, int col) {
    return word_addr(s, bank, row, col) * 2U;
}

uint16_t pattern(const Spec& s, int bank, int row, int col) {
    return static_cast<uint16_t>((bank ? 0x8000 : 0x1000) ^ (row * s.width + col));
}

struct Line {
    bool valid = false;
    int bank = -1;
    int y = -1;
    std::vector<uint16_t> pix;
};

using Memory = std::vector<uint16_t>;

void ensure_line_width(Line& line, int width) {
    if (static_cast<int>(line.pix.size()) != width)
        line.pix.assign(width, 0);
}

void fill_memory(const Spec& s, Memory& mem) {
    mem.assign(frame_words(s) * 2, 0);
    for (int bank = 0; bank < 2; ++bank) {
        for (int y = 0; y < s.height; ++y) {
            for (int x = 0; x < s.width; ++x) {
                const uint32_t wr = word_addr(s, bank, y, x);
                const uint32_t rd = static_cast<uint32_t>(bank * frame_words(s) + y * s.stride + x);
                if (wr != rd || byte_addr(s, bank, y, x) != wr * 2U) {
                    std::fprintf(stderr,
                                 "address formula mismatch %dx%d stride=%d bank=%d y=%d x=%d wr=%u rd=%u\n",
                                 s.width, s.height, s.stride, bank, y, x, wr, rd);
                    std::exit(1);
                }
                mem[wr] = pattern(s, bank, y, x);
            }
        }
    }
}

void load_line(const Spec& s, Line& line, const Memory& mem, int bank, int y) {
    ensure_line_width(line, s.width);
    line.valid = true;
    line.bank = bank;
    line.y = y;
    for (int x = 0; x < s.width; ++x)
        line.pix[x] = mem[word_addr(s, bank, y, x)];
}

// Models the rejected RTL ownership rule:
//   read_bank_sys = swap_pending ? ~disp_bank : disp_bank
// with one unbanked line-buffer set. As soon as a back buffer completes, the
// prefetcher repurposes the live scanout buffer before the vsync page flip.
int simulate_broken_single_set(const Spec& s, const Memory& mem) {
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
        for (int ahead = 0; ahead < kLines; ++ahead) {
            const int ly = (y + ahead < s.height) ? (y + ahead) : (s.height - 1);
            load_line(s, lines[ahead], mem, prefetch_bank, ly);
        }
        for (int x = 0; x < s.width; ++x) {
            const auto& line = lines[0];
            const uint16_t got = (line.valid && line.y == y) ? line.pix[x] : 0;
            const uint16_t want = pattern(s, disp_bank, y, x);
            if (got != want)
                ++mismatches;
        }
    }
    return mismatches;
}

int simulate_fixed_dual_set(const Spec& s, const Memory& mem) {
    std::vector<std::vector<Line>> sets(2, std::vector<Line>(kLines));
    int disp_bank = 0;
    int disp_set = 0;
    bool swap_pending = false;
    int mismatches = 0;

    for (int y = 0; y < s.height; ++y) {
        if (y == s.height / 2)
            swap_pending = true;

        for (int ahead = 0; ahead < kLines; ++ahead) {
            const int ly = (y + ahead < s.height) ? (y + ahead) : (s.height - 1);
            load_line(s, sets[disp_set][ahead], mem, disp_bank, ly);
        }
        if (swap_pending) {
            const int prep_set = !disp_set;
            for (int ahead = 0; ahead < kLines; ++ahead)
                load_line(s, sets[prep_set][ahead], mem, !disp_bank, ahead);
        }
        for (int x = 0; x < s.width; ++x) {
            const auto& line = sets[disp_set][0];
            const uint16_t got = (line.valid && line.bank == disp_bank && line.y == y) ? line.pix[x] : 0;
            const uint16_t want = pattern(s, disp_bank, y, x);
            if (got != want)
                ++mismatches;
        }
    }

    if (swap_pending) {
        disp_bank = !disp_bank;
        disp_set = !disp_set;
        swap_pending = false;
    }

    for (int y = 0; y < s.height; ++y) {
        for (int ahead = 0; ahead < kLines; ++ahead) {
            const int ly = (y + ahead < s.height) ? (y + ahead) : (s.height - 1);
            load_line(s, sets[disp_set][ahead], mem, disp_bank, ly);
        }
        for (int x = 0; x < s.width; ++x) {
            const auto& line = sets[disp_set][0];
            const uint16_t got = (line.valid && line.bank == disp_bank && line.y == y) ? line.pix[x] : 0;
            const uint16_t want = pattern(s, disp_bank, y, x);
            if (got != want)
                ++mismatches;
        }
    }
    return mismatches;
}

bool run_case(const Spec& s) {
    Memory mem;
    fill_memory(s, mem);

    const int broken_mismatches = simulate_broken_single_set(s, mem);
    const int fixed_mismatches = simulate_fixed_dual_set(s, mem);
    const int expected_broken = pixels(s) / 2;
    if (broken_mismatches != expected_broken) {
        std::fprintf(stderr, "%dx%d stride=%d: expected rejected single-buffer ownership mismatches=%d, got %d\n",
                     s.width, s.height, s.stride, expected_broken, broken_mismatches);
        return false;
    }
    if (fixed_mismatches != 0) {
        std::fprintf(stderr, "%dx%d stride=%d: fixed dual-buffer ownership mismatches=%d\n",
                     s.width, s.height, s.stride, fixed_mismatches);
        return false;
    }
    std::printf("test_frame_store_sdram_sim: %dx%d stride=%d OK (old path mismatches=%d; fixed path clean)\n",
                s.width, s.height, s.stride, broken_mismatches);
    return true;
}
} // namespace

int main() {
    const Spec cases[] = {
        {320, 240, 320},
        {640, 480, 640},
        {800, 600, 800},
        {1024, 768, 1024},
        {1280, 720, 1280},
        {1280, 1024, 1280},
        {1600, 900, 1600},
        {1920, 1080, 1920},
    };

    for (const auto& s : cases) {
        if (!run_case(s))
            return 1;
    }
    return 0;
}
