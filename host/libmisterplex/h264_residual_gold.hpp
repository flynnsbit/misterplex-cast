// Phase 3.3l-0/1/2: locked host goldens for first residual of Baseline F3 vector.
// Clip: scripts/gen_test_annexb_real.py → build/plex_real_baseline.264
// Source of truth for FPGA ST_PLACE coeff[0:15] + residual_csum (3.3l-1)
// and inv_quant+IDCT first 4×4 paint / recon_y00·mean (3.3l-2).
//
// residual_csum algorithm (matches tree RTL slice_hdr_parser ST_PLACE):
//   csum = 0
//   for i in 0..15: csum ^= sat_s8(coeff[i])   // as uint8
// Golden Baseline first residual → 0x14 (20). Keep residual_dc = sat8(coeff[0]) = -24.
#pragma once
#include <cstdint>

namespace misterplex {
namespace residual_gold {

// First coded 4×4 residual after I_NxN modes/CBP (nC=0 CAVLC), scan-order placement.
// Matches probeFirstI16Dc / FFmpeg reverse-run place (coeff[0] = residual_dc).
inline constexpr int kTc = 8;
inline constexpr int kT1 = 3;
inline constexpr int kQp = 25;
inline constexpr int kDc = -24;   // sat8(coeff[0]) — FPGA residual_dc / host res_dc
inline constexpr int kCoeff1 = 4; // optional peek when FPGA packs res_c1
inline constexpr int kPred = 128; // first MB 4×4 unavailable neighbours → DC pred

inline constexpr int16_t kCoeffScan[16] = {
    -24, 4, 4, 0, -4, 0, -1, 0, 0, -1, 1, 0, 1, 0, 0, 0,
};

// --- residual_csum8 (FPGA status telemetry, 3.3l-1) ---------------------------------
// Fold scan-order coeffs into one byte for status bus (no new M10K).
// Status plan (when Plex.sv packs telem; keep residual_dc regression):
//   [103:96]  residual_dc      (unchanged; coeff[0])
//   [111:104] residual_csum8   (this value; was stream_bytes[7:0] pre-3.3l-1)
//   [127:112] stream_bytes[15:0] (truncated; AR still splices [122:121])
// Host prints res_csum=; HW soft-gate optional until 3.3l-1 RBF with status pack.

inline constexpr int8_t satS8(int v) {
    if (v > 127)
        return 127;
    if (v < -128)
        return static_cast<int8_t>(-128);
    return static_cast<int8_t>(v);
}

inline constexpr uint8_t coeffCsum8(const int16_t coeff[16]) {
    uint8_t c = 0;
    for (int i = 0; i < 16; ++i)
        c ^= static_cast<uint8_t>(satS8(coeff[i]));
    return c;
}

// Compile-time golden from kCoeffScan (must stay 0x14).
inline constexpr uint8_t kCsum8 = []() constexpr {
    uint8_t c = 0;
    for (int i = 0; i < 16; ++i)
        c ^= static_cast<uint8_t>(satS8(kCoeffScan[i]));
    return c;
}();

// STALE anti-golden (do NOT use as residual_csum):
//   arithmetic sum of sat_s8(coeff[i]) over 0..15 = -20 → as uint8 0xEC.
// Early dirty drafts used sum-fold; tree RTL ST_PLACE + host + HW soft-gate are XOR.
// Gates: res_csum=20 / 0x14 — never res_csum=-20 or 0xEC.
inline constexpr int kStaleArithSum = -20;
inline constexpr uint8_t kStaleSumAsU8 = 0xEC;

static_assert(kCsum8 == 0x14, "Baseline first residual csum golden (XOR sat8)");
static_assert(kCsum8 != kStaleSumAsU8, "csum is XOR 0x14, not arith sum 0xEC");
static_assert(kCsum8 == 20, "decimal form of XOR csum for push_frame res_csum=");
static_assert(kCoeffScan[0] == kDc, "coeff[0] is residual_dc");
static_assert(kCoeffScan[1] == kCoeff1, "coeff[1] optional telem");

// --- Inv quant domain (dequant4x4 after zigzag place, qp=25) — 3.3l-2 ------------
// Host: recon::detail_r::dequant4x4(kCoeffScan, 16, kQp, blk)
// row-major residual-domain 4×4 before IDCT (not scan order).
// Synth DC-only (coeff0=-24, rest 0): blk[0][0]=-4224, all AC 0.
inline constexpr int kDeq00 = -4224; // synth + real DC term (shared)
inline constexpr int16_t kDeq[4][4] = {
    {-4224, 896, 0, -224},
    {896, -1152, 0, 288},
    {0, 0, 0, 0},
    {-224, 288, 0, 0},
};

// --- 4×4 recon after dequant+idct onto pred=128 (unavailable neighbours) --------
// 3.3l-0 locked; 3.3l-2 FPGA paint / status must match these pixels (mae=0 on block).
// mean = (sum + 8) / 16  (integer round-nearest-ish used by test_idct_quant)
inline constexpr int kY00 = 73;
inline constexpr int kMean4x4 = 62;
inline constexpr uint8_t kY[4][4] = {
    {73, 72, 76, 76},
    {72, 74, 71, 73},
    {76, 71, 32, 27},
    {76, 73, 27, 24},
};

// Synth DC-only path (coeff0=kDc only): uniform 4×4 after idct_add onto 128.
inline constexpr int kSynthY = 62;

// 3.3k decode_stub diagnostic: paint entire MB0 as clamp(128 + residual_dc).
// NOT true recon — contrast vs kY for eyes-on when 3.3l-2 lands.
// 128 + (-24) = 104.
inline constexpr int kStubDcPaintY = kPred + kDc; // 104

// 3.3l-2 status telemetry sketch (optional pack after res_csum lands):
// Prefer eyes-on fields that do not thrash residual_dc regression:
//   recon_y00   = kY00 (73)   — single sample at (0,0) of first 4×4
//   recon_mean  = kMean4x4 (62)
// Suggested: spare status byte or re-use diagnostic path; keep [103:96]=res_dc=-24.
inline constexpr int kReconY00Status = kY00;
inline constexpr int kReconMeanStatus = kMean4x4;

// Gray RGB565 pack matching decode_stub.sv: {R[7:3], G[7:2], B[7:3]} with R=G=B=y.
inline constexpr uint16_t grayRgb565(uint8_t y) {
    return static_cast<uint16_t>(((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3));
}
// Top-left sample paint word for true recon (y00=73 → RGB565).
inline constexpr uint16_t kPaintY00Rgb565 = grayRgb565(static_cast<uint8_t>(kY00));
// Stub DC-only paint word (y=104).
inline constexpr uint16_t kStubDcPaintRgb565 = grayRgb565(static_cast<uint8_t>(kStubDcPaintY));

// Locked RGB565 for eyes-on / sim dumps (y00=73 → 0x4A49; stub 104 → 0x6B4D).
static_assert(kPaintY00Rgb565 == 0x4A49, "true recon y00 RGB565");
static_assert(kStubDcPaintRgb565 == 0x6B4D, "stub DC paint RGB565");
static_assert(kStubDcPaintY == 104, "3.3k stub MB0 gray = 128+dc");
static_assert(kY00 == 73 && kMean4x4 == 62, "3.3l-2 recon goldens");
static_assert(kDeq[0][0] == kDeq00, "dequant DC locked");
static_assert(kPred == 128, "first 4x4 unavailable neighbours → DC pred 128");

inline bool coeffScanMatches(const int16_t coeff[16]) {
    for (int i = 0; i < 16; ++i) {
        if (coeff[i] != kCoeffScan[i])
            return false;
    }
    return true;
}

inline bool deqMatches(const int16_t blk[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (blk[i][j] != kDeq[i][j])
                return false;
    return true;
}

inline bool yMatches(const uint8_t y[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (y[i][j] != kY[i][j])
                return false;
    return true;
}

// mean = (sum + 8) / 16 over 4×4 Y samples (matches test_idct_quant / HW soft gate).
inline int mean4x4(const uint8_t y[4][4]) {
    int sum = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            sum += y[i][j];
    return (sum + 8) / 16;
}

} // namespace residual_gold
} // namespace misterplex
