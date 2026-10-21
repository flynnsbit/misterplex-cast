// Phase 3.3l-0/1/2: host inv-quant + 4x4 IDCT goldens + full 16-coeff scan export.
// Synthetic DC residual + real Baseline first I_NxN 4x4 (pred=128, no neighbours).
// Exit: locked coeffs + res_csum (XOR 0x14 / 20 — NOT stale sum -20) + recon pixels
//       for FPGA compare / paint (3.3l-1/2+).
// Regression: res_dc = coeff[0] = -24 must stay (test_f3_residual / FPGA expose).
#include "libmisterplex/h264_cavlc.hpp"
#include "libmisterplex/h264_recon.hpp"
#include "libmisterplex/h264_residual_gold.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace gold = misterplex::residual_gold;

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

static void printBlock4x4(const char* label, const uint8_t y[4][4]) {
    std::printf("%s:\n", label);
    for (int r = 0; r < 4; ++r) {
        std::printf("  ");
        for (int c = 0; c < 4; ++c)
            std::printf("%3d", y[r][c]);
        std::printf("\n");
    }
}

static void printCoeffs(const char* label, const int16_t coeff[16]) {
    std::printf("%s:", label);
    for (int i = 0; i < 16; ++i)
        std::printf(" %d", coeff[i]);
    std::printf("\n");
}

// Machine-readable residual dump for FPGA sim / status gate (3.3l-1).
static void printFpgaGoldResidual(const int16_t coeff[16], uint8_t csum) {
    std::printf("FPGA_GOLD coeff_scan=");
    for (int i = 0; i < 16; ++i) {
        if (i)
            std::printf(",");
        std::printf("%d", coeff[i]);
    }
    std::printf("\n");
    std::printf("FPGA_GOLD res_dc=%d res_csum=%u res_csum_hex=0x%02x res_tc=%d res_t1=%d qp=%d\n",
                gold::kDc, static_cast<unsigned>(csum), static_cast<unsigned>(csum), gold::kTc,
                gold::kT1, gold::kQp);
    std::printf("FPGA_GOLD status_plan residual_dc[103:96] residual_csum8[111:104] "
                "stream_bytes[127:112]\n");
    std::printf("FPGA_GOLD csum_algo=XOR_sat8  (NOT arith_sum=%d / 0x%02x)\n", gold::kStaleArithSum,
                static_cast<unsigned>(gold::kStaleSumAsU8));
}

// Machine-readable 3.3l-2 inv_quant + IDCT + paint targets (pred=128).
static void printFpgaGoldRecon(const int16_t deq[4][4], const uint8_t y[4][4], int mean) {
    std::printf("FPGA_GOLD pred=%d qp=%d\n", gold::kPred, gold::kQp);
    std::printf("FPGA_GOLD dequant_rowmajor=");
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (i || j)
                std::printf(",");
            std::printf("%d", deq[i][j]);
        }
    }
    std::printf("\n");
    std::printf("FPGA_GOLD recon_y_rowmajor=");
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (i || j)
                std::printf(",");
            std::printf("%u", static_cast<unsigned>(y[i][j]));
        }
    }
    std::printf("\n");
    std::printf("FPGA_GOLD recon_y00=%d recon_mean4x4=%d paint_y00_rgb565=0x%04x "
                "stub_dc_paint_y=%d stub_rgb565=0x%04x\n",
                y[0][0], mean, static_cast<unsigned>(gold::kPaintY00Rgb565), gold::kStubDcPaintY,
                static_cast<unsigned>(gold::kStubDcPaintRgb565));
    std::printf("FPGA_GOLD status_plan_3l2 recon_y00|recon_mean (optional telem); keep "
                "res_dc=-24 res_csum=20\n");
}

// Compile-time + table consistency (no bitstream). Also rejects stale sum-fold csum.
static bool testGoldTable() {
    if (gold::kCsum8 != 0x14 || gold::kCsum8 != 20) {
        std::printf("FAIL gold table: kCsum8=0x%02x (%u) want 0x14 (20)\n", gold::kCsum8,
                    static_cast<unsigned>(gold::kCsum8));
        return false;
    }
    if (gold::coeffCsum8(gold::kCoeffScan) != gold::kCsum8) {
        std::printf("FAIL gold table: runtime csum mismatch\n");
        return false;
    }
    if (misterplex::cavlc::residualCsum8(gold::kCoeffScan) != gold::kCsum8) {
        std::printf("FAIL gold table: cavlc::residualCsum8 != residual_gold\n");
        return false;
    }
    if (gold::kCoeffScan[0] != gold::kDc || gold::kCoeffScan[1] != gold::kCoeff1) {
        std::printf("FAIL gold table: coeff0/1 mismatch\n");
        return false;
    }

    // Prove XOR ≠ arithmetic sum (stale drafts used sum → -20 / 0xEC).
    int arith = 0;
    for (int i = 0; i < 16; ++i)
        arith += static_cast<int>(gold::satS8(gold::kCoeffScan[i]));
    if (arith != gold::kStaleArithSum) {
        std::printf("FAIL gold table: arith sum=%d want stale anti-golden %d\n", arith,
                    gold::kStaleArithSum);
        return false;
    }
    if (static_cast<uint8_t>(arith) != gold::kStaleSumAsU8) {
        std::printf("FAIL gold table: arith as u8=0x%02x want 0xEC\n",
                    static_cast<unsigned>(static_cast<uint8_t>(arith)));
        return false;
    }
    if (gold::kCsum8 == gold::kStaleSumAsU8 || static_cast<int>(gold::kCsum8) == gold::kStaleArithSum) {
        std::printf("FAIL gold table: kCsum8 must not equal stale sum-fold\n");
        return false;
    }

    if (gold::kPred != 128 || gold::kY00 != 73 || gold::kMean4x4 != 62 || gold::kSynthY != 62 ||
        gold::kStubDcPaintY != 104) {
        std::printf("FAIL gold table: recon constants pred=%d y00=%d mean=%d synth=%d stub=%d\n",
                    gold::kPred, gold::kY00, gold::kMean4x4, gold::kSynthY, gold::kStubDcPaintY);
        return false;
    }
    if (gold::kPaintY00Rgb565 != 0x4A49 || gold::kStubDcPaintRgb565 != 0x6B4D) {
        std::printf("FAIL gold table: paint rgb565 true=0x%04x stub=0x%04x\n",
                    static_cast<unsigned>(gold::kPaintY00Rgb565),
                    static_cast<unsigned>(gold::kStubDcPaintRgb565));
        return false;
    }

    std::printf("OK gold table: coeff[16] locked csum=0x%02x (%u XOR, not sum %d) dc=%d tc=%d t1=%d\n",
                gold::kCsum8, static_cast<unsigned>(gold::kCsum8), gold::kStaleArithSum, gold::kDc,
                gold::kTc, gold::kT1);
    return true;
}

// Known path: single DC coeff in scan pos 0, qp=25, pred=128.
static bool testSyntheticDc() {
    int16_t coeff[16]{};
    coeff[0] = static_cast<int16_t>(gold::kDc);
    int16_t blk[4][4];
    misterplex::recon::detail_r::dequant4x4(coeff, 16, gold::kQp, blk);

    if (blk[0][0] != gold::kDeq00) {
        std::printf("FAIL synth: dequant blk00=%d want %d\n", blk[0][0], gold::kDeq00);
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (i == 0 && j == 0)
                continue;
            if (blk[i][j] != 0) {
                std::printf("FAIL synth: non-zero AC dequant at [%d][%d]=%d\n", i, j, blk[i][j]);
                return false;
            }
        }
    }

    uint8_t dst[16];
    for (int i = 0; i < 16; ++i)
        dst[i] = static_cast<uint8_t>(gold::kPred);
    misterplex::recon::detail_r::idct4x4_add(blk, dst, 4);

    for (int i = 0; i < 16; ++i) {
        if (dst[i] != gold::kSynthY) {
            std::printf("FAIL synth: y[%d]=%d want uniform %d\n", i, dst[i], gold::kSynthY);
            return false;
        }
    }
    std::printf("OK synth: qp=%d coeff0=%d deq00=%d pred=%d → y=%d (uniform 4x4)\n", gold::kQp,
                coeff[0], gold::kDeq00, gold::kPred, gold::kSynthY);
    return true;
}

// 3.3l-1: full 16-coeff dump + FPGA status expose map (host vs on-wire today).
static bool testFirstResidualDumpAndFpgaExpose(const uint8_t* annexb, size_t n) {
    auto r = misterplex::cavlc::probeFirstI16Dc(annexb, n);
    if (!r.ok || r.total_coeff != gold::kTc || r.trailing_ones != gold::kT1) {
        std::printf("FAIL 3l1: probe ok=%d tc=%d t1=%d\n", r.ok, r.total_coeff, r.trailing_ones);
        return false;
    }
    if (!gold::coeffScanMatches(r.coeff) ||
        !misterplex::cavlc::residualCoeffsMatch(r.coeff, gold::kCoeffScan)) {
        std::printf("FAIL 3l1: full coeff[16] != residual_gold\n");
        printCoeffs("  got", r.coeff);
        printCoeffs("  exp", gold::kCoeffScan);
        return false;
    }

    const uint8_t csum = misterplex::cavlc::dumpResidualCoeffs("HOST first residual scan", r.coeff);
    if (csum != gold::kCsum8) {
        std::printf("FAIL 3l1: dump csum=0x%02x want 0x%02x\n", csum, gold::kCsum8);
        return false;
    }

    auto ex = misterplex::cavlc::hostToFpgaResidualExpose(r);
    std::printf("FPGA expose map:\n");
    std::printf("  ON WIRE (lab RBF / test_f3_residual): res_ok=%d res_tc=%u res_t1=%u res_dc=%d\n",
                ex.residual_ok ? 1 : 0, ex.residual_tc, ex.residual_t1, ex.residual_dc);
    std::printf("  HOST FULL-16 (3.3l-1 golden): res_csum=0x%02x (%u) res_c1=%d full_on_wire=%d\n",
                static_cast<unsigned>(ex.residual_csum), static_cast<unsigned>(ex.residual_csum),
                ex.residual_c1, ex.full_coeffs_on_wire ? 1 : 0);
    std::printf("  COMPARE: keep res_dc=-24; when telem packs csum expect res_csum=20 (0x14 XOR, "
                "not sum -20)\n");
    printFpgaGoldResidual(r.coeff, csum);

    if (!ex.residual_ok || ex.residual_tc != gold::kTc || ex.residual_t1 != gold::kT1 ||
        ex.residual_dc != gold::kDc || ex.residual_csum != gold::kCsum8 ||
        ex.residual_c1 != gold::kCoeff1 || ex.full_coeffs_on_wire) {
        std::printf("FAIL 3l1: expose ok=%d tc=%d t1=%d dc=%d csum=0x%02x c1=%d on_wire=%d\n",
                    ex.residual_ok ? 1 : 0, ex.residual_tc, ex.residual_t1, ex.residual_dc,
                    static_cast<unsigned>(ex.residual_csum), ex.residual_c1,
                    ex.full_coeffs_on_wire ? 1 : 0);
        return false;
    }
    int host_nz = 0;
    for (int i = 0; i < 16; ++i)
        if (r.coeff[i])
            ++host_nz;
    if (host_nz != gold::kTc) {
        std::printf("FAIL 3l1: nz=%d want tc=%d\n", host_nz, gold::kTc);
        return false;
    }
    std::printf("OK 3l1: full-16 dump + csum=0x%02x locked for FPGA compare (res_dc=-24)\n",
                gold::kCsum8);
    return true;
}

// Core 3.3l-2 host path: inv_quant + IDCT onto pred=128 for first 4×4.
// table_only uses residual_gold::kCoeffScan (RTL sim without bitstream);
// otherwise probes annex-B and must match the same goldens.
static bool runFirst4x4InvQuantIdct(const int16_t coeff[16], const char* tag) {
    if (!gold::coeffScanMatches(coeff)) {
        std::printf("FAIL %s: coeff[16] != residual_gold\n", tag);
        printCoeffs("  got", coeff);
        printCoeffs("  exp", gold::kCoeffScan);
        return false;
    }
    const uint8_t csum = gold::coeffCsum8(coeff);
    if (csum != gold::kCsum8 || csum == gold::kStaleSumAsU8) {
        std::printf("FAIL %s: res_csum=%u want XOR 0x%02x (not stale 0x%02x)\n", tag,
                    static_cast<unsigned>(csum), static_cast<unsigned>(gold::kCsum8),
                    static_cast<unsigned>(gold::kStaleSumAsU8));
        return false;
    }

    int16_t blk[4][4];
    misterplex::recon::detail_r::dequant4x4(coeff, 16, gold::kQp, blk);
    if (!gold::deqMatches(blk)) {
        std::printf("FAIL %s: dequant 4x4 != residual_gold::kDeq\n", tag);
        std::printf("  got:\n");
        for (int i = 0; i < 4; ++i) {
            std::printf("  ");
            for (int j = 0; j < 4; ++j)
                std::printf("%6d", blk[i][j]);
            std::printf("\n");
        }
        return false;
    }

    uint8_t flat[16];
    for (int i = 0; i < 16; ++i)
        flat[i] = static_cast<uint8_t>(gold::kPred); // first MB 4×4: neighbours unavailable
    misterplex::recon::detail_r::idct4x4_add(blk, flat, 4);

    uint8_t y[4][4];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            y[row][col] = flat[row * 4 + col];
    const int mean = gold::mean4x4(y);

    printCoeffs("GOLD coeffs scan", coeff);
    std::printf("GOLD res_csum=%u (0x%02x) — XOR sat8 (not arith sum %d)\n",
                static_cast<unsigned>(csum), static_cast<unsigned>(csum), gold::kStaleArithSum);
    std::printf("GOLD dequant 4x4 (row-major residual domain):\n");
    for (int i = 0; i < 4; ++i) {
        std::printf("  ");
        for (int j = 0; j < 4; ++j)
            std::printf("%6d", blk[i][j]);
        std::printf("\n");
    }
    printBlock4x4("GOLD recon Y 4x4 (pred=128, qp=25)", y);
    std::printf("GOLD y00=%d mean4x4=%d tc=%d t1=%d qp=%d csum8=0x%02x res_dc=%d paint_rgb565=0x%04x\n",
                y[0][0], mean, gold::kTc, gold::kT1, gold::kQp, gold::kCsum8, gold::kDc,
                static_cast<unsigned>(gold::kPaintY00Rgb565));
    std::printf("GOLD contrast: 3.3k stub paint y=%d (128+dc) vs true recon y00=%d\n",
                gold::kStubDcPaintY, gold::kY00);
    printFpgaGoldResidual(coeff, csum);
    printFpgaGoldRecon(blk, y, mean);

    if (y[0][0] != gold::kY00 || mean != gold::kMean4x4) {
        std::printf("FAIL %s: y00=%d mean=%d want y00=%d mean=%d\n", tag, y[0][0], mean, gold::kY00,
                    gold::kMean4x4);
        return false;
    }
    if (!gold::yMatches(y)) {
        std::printf("FAIL %s: recon Y 4x4 != residual_gold::kY\n", tag);
        return false;
    }
    if (gold::grayRgb565(y[0][0]) != gold::kPaintY00Rgb565) {
        std::printf("FAIL %s: paint rgb565=0x%04x want 0x%04x\n", tag,
                    static_cast<unsigned>(gold::grayRgb565(y[0][0])),
                    static_cast<unsigned>(gold::kPaintY00Rgb565));
        return false;
    }

    // Top-left 4×4 paint plan for decode_stub / frame_store (W=320 linear).
    std::printf("3.3l-2 paint vector (top-left 4x4, gray Y→RGB565, pred=%d):\n", gold::kPred);
    std::printf("  frame_store addrs (W=320):");
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            std::printf(" %d", row * 320 + col);
    std::printf("\n");
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const uint16_t px = gold::grayRgb565(y[row][col]);
            std::printf("  [%d][%d] Y=%3u RGB565=0x%04x\n", row, col,
                        static_cast<unsigned>(y[row][col]), static_cast<unsigned>(px));
        }
    }
    std::printf("  HW soft gate after inv_quant+idct: recon_y00==%d recon_mean==%d "
                "(keep res_dc=-24 res_csum=20 XOR not sum %d)\n",
                gold::kY00, gold::kMean4x4, gold::kStaleArithSum);
    return true;
}

// 3.3l-2 table-only (no annex-B): RTL / sim can consume residual_gold coeffs alone.
static bool testTableFirst4x4InvQuantIdct() {
    if (!runFirst4x4InvQuantIdct(gold::kCoeffScan, "3l2-table"))
        return false;
    std::printf("OK 3l2-table: inv_quant+IDCT pred=%d → y00=%d mean=%d (no bitstream)\n",
                gold::kPred, gold::kY00, gold::kMean4x4);
    return true;
}

// Real annex-B first residual → same dequant+IDCT goldens as table path.
static bool testRealFirst4x4(const uint8_t* annexb, size_t n) {
    auto r = misterplex::cavlc::probeFirstI16Dc(annexb, n);
    if (!r.ok || r.total_coeff != gold::kTc || r.trailing_ones != gold::kT1 ||
        r.coeff[0] != gold::kDc) {
        std::printf("FAIL real: tc=%d t1=%d coeff0=%d (want %d/%d/%d)\n", r.total_coeff,
                    r.trailing_ones, r.ok ? r.coeff[0] : 0, gold::kTc, gold::kT1, gold::kDc);
        return false;
    }
    if (!runFirst4x4InvQuantIdct(r.coeff, "3l2-real"))
        return false;
    std::printf("OK 3l2-real: first I_NxN 4x4 bit-exact; inv_quant+IDCT locked for FPGA paint\n");
    return true;
}

int main(int argc, char** argv) {
    if (!testGoldTable())
        return 1;
    if (!testSyntheticDc())
        return 1;
    // Table path first so 3.3l-2 goldens are available even without annex-B tools.
    if (!testTableFirst4x4InvQuantIdct())
        return 1;



    const char* path = argc > 1 ? argv[1] : "build/plex_real_baseline.264";
    auto blob = readFile(path);
    if (blob.empty()) {
        if (std::system("python3 scripts/gen_test_annexb_real.py build/plex_real_baseline.264") !=


            0) {
            std::printf("FAIL: no bitstream (gen_test_annexb_real.py)\n");
            return 1;
        }


        blob = readFile("build/plex_real_baseline.264");
        path = "build/plex_real_baseline.264";


    }
    if (blob.empty()) {
        std::printf("FAIL: empty bitstream\n");
        return 1;
    }
    if (!testFirstResidualDumpAndFpgaExpose(blob.data(), blob.size()))
        return 1;
    if (!testRealFirst4x4(blob.data(), blob.size()))
        return 1;

    std::printf("test_idct_quant: OK (3.3l-0 synth + 3.3l-1 full-16/csum XOR0x14 + "
                "3.3l-2 inv_quant/IDCT pred=128)\n");
    return 0;
}
