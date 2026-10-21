// Verilator testbench: sweep dequant across QP 0–51.
// Compares RTL h264_dequant4x4 output against H.264 spec formula for
// every QP with a set of coefficient patterns covering:
//   - All 3 normAdjust matrix index categories (mi=0,1,2)
//   - Positive and negative coefficients
//   - Small, medium, and maximum-magnitude coefficients
//   - All 16 scan positions

#include "Vh264_dequant4x4.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const int kNormAdjust[6][3] = {
    {10, 13, 16}, {11, 14, 18}, {13, 16, 20}, {14, 18, 23}, {16, 20, 25}, {18, 23, 29},
};
static const int kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

struct DequantRef {
    int32_t values[16];
};

// Host-side dequant: identical to h264_recon.hpp dequant4x4 with maxCoeff=16
DequantRef hostDequant(const int16_t coeff[16], int qp) {
    DequantRef out;
    std::memset(&out, 0, sizeof(out));
    int shift = qp / 6 + 2;
    for (int k = 0; k < 16; ++k) {
        if (!coeff[k])
            continue;
        int zi = kZigzag[k];
        int i = zi / 4, j = zi % 4;
        int mi = ((i & 1) + (j & 1)) == 0 ? 0 : (((i & 1) + (j & 1)) == 1 ? 1 : 2);
        int qmul = (kNormAdjust[qp % 6][mi] * 16) << shift;
        int v = (static_cast<int>(coeff[k]) * qmul + 32) >> 6;
        out.values[zi] = v;
    }
    return out;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto dut = new Vh264_dequant4x4;

    int failures = 0;
    int tests = 0;

    // Coefficient test patterns:
    // Single nonzero at each scan position, with various magnitudes
    struct Pattern {
        const char* name;
        int16_t coeffs[16];
    };

    std::vector<Pattern> patterns;

    // Pattern 1: all-ones
    {
        Pattern p;
        p.name = "all_ones";
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = 1;
        patterns.push_back(p);
    }
    // Pattern 2: all-minus-ones
    {
        Pattern p;
        p.name = "all_neg_ones";
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = -1;
        patterns.push_back(p);
    }
    // Pattern 3: max positive (9-bit signed max = 255)
    {
        Pattern p;
        p.name = "max_pos";
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = 255;
        patterns.push_back(p);
    }
    // Pattern 4: max negative (-256)
    {
        Pattern p;
        p.name = "max_neg";
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = -256;
        patterns.push_back(p);
    }
    // Pattern 5: single coefficient per scan position
    for (int pos = 0; pos < 16; ++pos) {
        Pattern p;
        char name[64];
        snprintf(name, sizeof(name), "single_pos%d", pos);
        p.name = strdup(name);
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = (i == pos) ? 42 : 0;
        patterns.push_back(p);
    }
    // Pattern 6: alternating sign
    {
        Pattern p;
        p.name = "alternating";
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = (i & 1) ? -17 : 17;
        patterns.push_back(p);
    }
    // Pattern 7: typical residual-like
    {
        Pattern p;
        p.name = "typical_residual";
        int16_t vals[] = {12, -3, 5, 0, -1, 2, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(p.coeffs, vals, sizeof(vals));
        patterns.push_back(p);
    }
    // Pattern 8: DC only
    {
        Pattern p;
        p.name = "dc_only";
        for (int i = 0; i < 16; ++i)
            p.coeffs[i] = 0;
        p.coeffs[0] = 100;
        patterns.push_back(p);
    }

    for (int qp = 0; qp <= 51; ++qp) {
        for (const auto& pat : patterns) {
            // Set up RTL inputs
            dut->qp = static_cast<uint8_t>(qp);
            dut->max_coeff = 16;
            for (int i = 0; i < 16; ++i)
                dut->coeff[i] = static_cast<uint16_t>(pat.coeffs[i] & 0x1FF);
            dut->eval();

            // Compute host reference
            DequantRef ref = hostDequant(pat.coeffs, qp);

            // Compare
            bool ok = true;
            for (int i = 0; i < 16; ++i) {
                // RTL output is signed 29-bit; sign-extend
                int32_t rtl_val = dut->dequant[i];
                if (rtl_val & (1 << 28))
                    rtl_val |= ~((1 << 29) - 1);
                if (rtl_val != ref.values[i]) {
                    if (ok) {
                        fprintf(stderr, "MISMATCH QP=%d pattern=%s:\n", qp, pat.name);
                        ok = false;
                    }
                    int zi = kZigzag[i]; // Note: comparison is already in row-major
                    fprintf(stderr, "  pos[%d] row-major: RTL=%d host=%d diff=%d\n",
                            i, rtl_val, ref.values[i], rtl_val - ref.values[i]);
                    ++failures;
                }
            }
            ++tests;
        }
    }

    // Print QP→dequant summary for spot-checking
    printf("QP sweep summary (coefficient=1 at scan pos 0, mi=0):\n");
    for (int qp = 0; qp <= 51; qp += 3) {
        int16_t c[16] = {};
        c[0] = 1;
        DequantRef ref = hostDequant(c, qp);
        dut->qp = static_cast<uint8_t>(qp);
        dut->max_coeff = 16;
        for (int i = 0; i < 16; ++i)
            dut->coeff[i] = c[i] & 0x1FF;
        dut->eval();
        int32_t rtl_val = dut->dequant[0];
        if (rtl_val & (1 << 17))
            rtl_val |= ~((1 << 18) - 1);
        printf("  QP=%2d: host=%6d RTL=%6d %s\n",
               qp, ref.values[0], rtl_val,
               ref.values[0] == rtl_val ? "OK" : "MISMATCH");
    }

    if (failures) {
        fprintf(stderr, "DEQUANT QP SWEEP FAILED: %d mismatches in %d tests\n",
                failures, tests);
        return 1;
    }
    printf("DEQUANT QP SWEEP PASS: %d tests across QP 0–51, all patterns exact\n", tests);
    return 0;
}
