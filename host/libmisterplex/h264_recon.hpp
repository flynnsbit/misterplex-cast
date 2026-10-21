// Baseline I-slice reconstruct: CAVLC residual → inv quant → IDCT → Intra pred → YUV/RGB565.
// Phase 3.3h host path (golden vs FFmpeg Y).
#pragma once
#include "libmisterplex/h264_slice_walk.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace misterplex {
namespace recon {

struct ReconResult {
    int width = 0;
    int height = 0;
    int mb_decoded = 0;
    int mb_total = 0;
    int fail_mb = -1;
    const char* fail_reason = nullptr;
    std::vector<uint8_t> y;  // width*height
    std::vector<uint8_t> u;  // (w/2)*(h/2)
    std::vector<uint8_t> v;
};

struct Luma4x4Trace {
    int block = 0;       // decode order, 0..15
    int x = 0;           // macroblock-local pixel x
    int y = 0;           // macroblock-local pixel y
    int pred_mode = -1;  // H.264 Intra4x4 mode, or Intra16x16 mode for I16 MBs
    int total_coeff = 0;
    bool nA_available = false;
    int nA_total_coeff = -1;
    bool nB_available = false;
    int nB_total_coeff = -1;
    int predicted_nC = 0;
    int coeff_token_table = -1; // 0..3 for CAVLC luma coeff_token table, -1 if no token
    int bit_offset_start = -1; // RBSP bit offset for this luma residual, if coded
    int bit_offset_end = -1;
    std::array<int16_t, 16> coeff{}; // CAVLC scan-order coefficients
    std::array<uint8_t, 16> pred{};
    std::array<int16_t, 16> dequant{};
    std::array<int16_t, 16> idct{};
    std::array<uint8_t, 16> recon{};
};

struct LumaMbTrace {
    bool valid = false;
    int mb = 0;
    int mb_x = 0;
    int mb_y = 0;
    int mb_type = 0;
    int qp = 0;
    int pred_mode = -1;
    int chroma_mode = -1;
    int cbp_luma = -1;
    int cbp_chroma = -1;
    std::array<uint8_t, 256> pred{};
    std::array<uint8_t, 256> recon{};
    std::array<Luma4x4Trace, 16> blocks{};
};

struct ReconTrace {
    int target_mb = 0;
    LumaMbTrace mb;
};

namespace detail_r {

inline int clip8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

// H.264 LevelScale4x4 (normAdjust * sm) — FFmpeg dequant4_coeff base
static const int kNormAdjust[6][3] = {
    {10, 13, 16}, {11, 14, 18}, {13, 16, 20}, {14, 18, 23}, {16, 20, 25}, {18, 23, 29},
};
// scan pos → (i%4, i/4) zigzag
static const int kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

inline int levelScale(int qp, int i, int j) {
    // mf index: 0 if both even, 1 if one odd, 2 if both odd
    int mi = ((i & 1) + (j & 1)) == 0 ? 0 : (((i & 1) + (j & 1)) == 1 ? 1 : 2);
    return kNormAdjust[qp % 6][mi];
}

// FFmpeg-style dequant: block = (level * qmul + 32) >> 6
// qmul = (mf * scaling=16) << (qp/6 + 2); maxCoeff 15 skips DC scan slot
inline void dequant4x4(const int16_t coeffScan[16], int maxCoeff, int qp, int16_t blk[4][4]) {
    std::memset(blk, 0, 16 * sizeof(int16_t));
    const int shift = qp / 6 + 2;
    for (int k = 0; k < maxCoeff; ++k) {
        if (!coeffScan[k])
            continue;
        int zi = (maxCoeff == 15) ? kZigzag[k + 1] : kZigzag[k];
        int i = zi / 4, j = zi % 4;
        int qmul = (levelScale(qp, i, j) * 16) << shift;
        int v = (static_cast<int>(coeffScan[k]) * qmul + 32) >> 6;
        blk[i][j] = static_cast<int16_t>(v);
    }
}

// FFmpeg ff_h264_idct_add: block[0]+=32; butterflies; dst += x>>6
inline void idct4x4_add(int16_t blk[4][4], uint8_t* dst, int stride) {
    int b[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            b[i][j] = blk[i][j];
    b[0][0] += 32;
    int t[4][4];
    for (int i = 0; i < 4; ++i) {
        int z0 = b[i][0] + b[i][2];
        int z1 = b[i][0] - b[i][2];
        int z2 = (b[i][1] >> 1) - b[i][3];
        int z3 = b[i][1] + (b[i][3] >> 1);
        t[i][0] = z0 + z3;
        t[i][1] = z1 + z2;
        t[i][2] = z1 - z2;
        t[i][3] = z0 - z3;
    }

    for (int j = 0; j < 4; ++j) {
        int z0 = t[0][j] + t[2][j];
        int z1 = t[0][j] - t[2][j];
        int z2 = (t[1][j] >> 1) - t[3][j];
        int z3 = t[1][j] + (t[3][j] >> 1);
        dst[0 * stride + j] = static_cast<uint8_t>(clip8(dst[0 * stride + j] + ((z0 + z3) >> 6)));
        dst[1 * stride + j] = static_cast<uint8_t>(clip8(dst[1 * stride + j] + ((z1 + z2) >> 6)));
        dst[2 * stride + j] = static_cast<uint8_t>(clip8(dst[2 * stride + j] + ((z1 - z2) >> 6)));
        dst[3 * stride + j] = static_cast<uint8_t>(clip8(dst[3 * stride + j] + ((z0 - z3) >> 6)));
    }
}

// Same inverse transform as idct4x4_add, but returns the signed residual samples
// before they are added to prediction. This is the per-4x4 RTL golden payload.
inline void idct4x4_residual(const int16_t blk[4][4], int16_t residual[4][4]) {
    int b[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            b[i][j] = blk[i][j];
    b[0][0] += 32;
    int t[4][4];
    for (int i = 0; i < 4; ++i) {
        int z0 = b[i][0] + b[i][2];
        int z1 = b[i][0] - b[i][2];
        int z2 = (b[i][1] >> 1) - b[i][3];
        int z3 = b[i][1] + (b[i][3] >> 1);
        t[i][0] = z0 + z3;
        t[i][1] = z1 + z2;
        t[i][2] = z1 - z2;
        t[i][3] = z0 - z3;
    }
    for (int j = 0; j < 4; ++j) {
        int z0 = t[0][j] + t[2][j];
        int z1 = t[0][j] - t[2][j];
        int z2 = (t[1][j] >> 1) - t[3][j];
        int z3 = t[1][j] + (t[3][j] >> 1);
        residual[0][j] = static_cast<int16_t>((z0 + z3) >> 6);
        residual[1][j] = static_cast<int16_t>((z1 + z2) >> 6);
        residual[2][j] = static_cast<int16_t>((z1 - z2) >> 6);
        residual[3][j] = static_cast<int16_t>((z0 - z3) >> 6);
    }
}

// Intra16x16 DC Hadamard + dequant → 4x4 DC values (ITU 8.5.10)
// Reuse cavlc::invQuantHadamardDc4x4 which returns scaled DC for each 4x4.
// Those DCs are added into residual AC path as (0,0) before idct — or applied as
// prediction-free residual at DC. Standard: DC after Hadamard is dequant'd and becomes
// the DC coefficient of each 4x4 residual before inverse transform.
inline void placeI16Dc(const int16_t dc[4][4], int16_t acBlk[4][4], int by, int bx) {
    // Add DC into (0,0) of residual block prior to idct (scale already applied)
    acBlk[0][0] = static_cast<int16_t>(acBlk[0][0] + dc[by][bx]);
}

// --- Intra prediction ---
// Available samples: above row [-1..16], left col, top-left corner.
// Constrained: unavailable → 128 for DC modes when neither side avail.

inline void predI16_V(uint8_t* mb, int stride, const uint8_t* above) {
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            mb[y * stride + x] = above[x];
}
inline void predI16_H(uint8_t* mb, int stride, const uint8_t* left) {
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            mb[y * stride + x] = left[y];
}
inline void predI16_DC(uint8_t* mb, int stride, const uint8_t* above, const uint8_t* left,
                       bool hasA, bool hasL) {
    int sum = 0, n = 0;
    if (hasA) {
        for (int i = 0; i < 16; ++i)
            sum += above[i];
        n += 16;
    }
    if (hasL) {
        for (int i = 0; i < 16; ++i)
            sum += left[i];
        n += 16;
    }
    int v = n ? (sum + (n >> 1)) / n : 128;
    for (int y = 0; y < 16; ++y)
        std::memset(mb + y * stride, v, 16);
}
inline void predI16_Plane(uint8_t* mb, int stride, const uint8_t* above, const uint8_t* left,
                          uint8_t tl) {
    int H = 0, V = 0;
    for (int i = 0; i < 8; ++i) {
        H += (i + 1) * (above[8 + i] - above[6 - i]);
        V += (i + 1) * (left[8 + i] - left[6 - i]);
    }
    // above[-1] for H uses above[ -1 ] = top-left; for i=0: above[8]-above[6];
    // Spec uses p[8+i] - p[6-i] with p[-1]=top-left when i=7: above[15]-above[-1]
    H = 0;
    V = 0;
    for (int i = 0; i < 8; ++i) {
        int a_hi = above[8 + i];
        int a_lo = (6 - i >= 0) ? above[6 - i] : tl;
        H += (i + 1) * (a_hi - a_lo);
        int l_hi = left[8 + i];
        int l_lo = (6 - i >= 0) ? left[6 - i] : tl;
        V += (i + 1) * (l_hi - l_lo);
    }
    int a = 16 * (above[15] + left[15]);
    int b = (5 * H + 32) >> 6;
    int c = (5 * V + 32) >> 6;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int val = (a + b * (x - 7) + c * (y - 7) + 16) >> 5;
            mb[y * stride + x] = static_cast<uint8_t>(clip8(val));
        }
}

// Intra4x4 modes 0..8 — FFmpeg h264pred_template formulas
// above[0..3]=top, above[4..7]=top-right; left[0..3]; tl=top-left
inline void predI4(int mode, uint8_t* dst, int stride, const uint8_t* above, const uint8_t* left,
                   uint8_t tl, bool hasA, bool hasL) {
    const int t0 = above[0], t1 = above[1], t2 = above[2], t3 = above[3];
    const int t4 = above[4], t5 = above[5], t6 = above[6], t7 = above[7];
    const int l0 = left[0], l1 = left[1], l2 = left[2], l3 = left[3];
    auto put = [&](int x, int y, int v) {
        dst[y * stride + x] = static_cast<uint8_t>(clip8(v));
    };
    switch (mode) {
    case 0: // Vertical
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                put(x, y, above[x]);
        break;
    case 1: // Horizontal
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                put(x, y, left[y]);
        break;
    case 2: { // DC
        int v;
        if (hasA && hasL)
            v = (t0 + t1 + t2 + t3 + l0 + l1 + l2 + l3 + 4) >> 3;
        else if (hasA)
            v = (t0 + t1 + t2 + t3 + 2) >> 2;
        else if (hasL)
            v = (l0 + l1 + l2 + l3 + 2) >> 2;
        else
            v = 128;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                put(x, y, v);
        break;
    }
    case 3: // Diagonal Down-Left
        put(0, 0, (t0 + t2 + 2 * t1 + 2) >> 2);
        put(1, 0, (t1 + t3 + 2 * t2 + 2) >> 2);
        put(0, 1, (t1 + t3 + 2 * t2 + 2) >> 2);
        put(2, 0, (t2 + t4 + 2 * t3 + 2) >> 2);
        put(1, 1, (t2 + t4 + 2 * t3 + 2) >> 2);
        put(0, 2, (t2 + t4 + 2 * t3 + 2) >> 2);
        put(3, 0, (t3 + t5 + 2 * t4 + 2) >> 2);
        put(2, 1, (t3 + t5 + 2 * t4 + 2) >> 2);
        put(1, 2, (t3 + t5 + 2 * t4 + 2) >> 2);
        put(0, 3, (t3 + t5 + 2 * t4 + 2) >> 2);
        put(3, 1, (t4 + t6 + 2 * t5 + 2) >> 2);
        put(2, 2, (t4 + t6 + 2 * t5 + 2) >> 2);
        put(1, 3, (t4 + t6 + 2 * t5 + 2) >> 2);
        put(3, 2, (t5 + t7 + 2 * t6 + 2) >> 2);
        put(2, 3, (t5 + t7 + 2 * t6 + 2) >> 2);
        put(3, 3, (t6 + 3 * t7 + 2) >> 2);
        break;
    case 4: // Diagonal Down-Right (FFmpeg)
        put(0, 3, (l3 + 2 * l2 + l1 + 2) >> 2);
        put(0, 2, (l2 + 2 * l1 + l0 + 2) >> 2);
        put(1, 3, (l2 + 2 * l1 + l0 + 2) >> 2);
        put(0, 1, (l1 + 2 * l0 + tl + 2) >> 2);
        put(1, 2, (l1 + 2 * l0 + tl + 2) >> 2);
        put(2, 3, (l1 + 2 * l0 + tl + 2) >> 2);
        put(0, 0, (l0 + 2 * tl + t0 + 2) >> 2);
        put(1, 1, (l0 + 2 * tl + t0 + 2) >> 2);
        put(2, 2, (l0 + 2 * tl + t0 + 2) >> 2);
        put(3, 3, (l0 + 2 * tl + t0 + 2) >> 2);
        put(1, 0, (tl + 2 * t0 + t1 + 2) >> 2);
        put(2, 1, (tl + 2 * t0 + t1 + 2) >> 2);
        put(3, 2, (tl + 2 * t0 + t1 + 2) >> 2);
        put(2, 0, (t0 + 2 * t1 + t2 + 2) >> 2);
        put(3, 1, (t0 + 2 * t1 + t2 + 2) >> 2);
        put(3, 0, (t1 + 2 * t2 + t3 + 2) >> 2);
        break;
    case 5: // Vertical-Right
        put(0, 0, (tl + t0 + 1) >> 1);
        put(1, 2, (tl + t0 + 1) >> 1);
        put(1, 0, (t0 + t1 + 1) >> 1);
        put(2, 2, (t0 + t1 + 1) >> 1);
        put(2, 0, (t1 + t2 + 1) >> 1);
        put(3, 2, (t1 + t2 + 1) >> 1);
        put(3, 0, (t2 + t3 + 1) >> 1);
        put(0, 1, (l0 + 2 * tl + t0 + 2) >> 2);
        put(1, 3, (l0 + 2 * tl + t0 + 2) >> 2);
        put(1, 1, (tl + 2 * t0 + t1 + 2) >> 2);
        put(2, 3, (tl + 2 * t0 + t1 + 2) >> 2);
        put(2, 1, (t0 + 2 * t1 + t2 + 2) >> 2);
        put(3, 3, (t0 + 2 * t1 + t2 + 2) >> 2);
        put(3, 1, (t1 + 2 * t2 + t3 + 2) >> 2);
        put(0, 2, (tl + 2 * l0 + l1 + 2) >> 2);
        put(0, 3, (l0 + 2 * l1 + l2 + 2) >> 2);
        break;
    case 6: // Horizontal-Down
        put(0, 0, (tl + l0 + 1) >> 1);
        put(2, 1, (tl + l0 + 1) >> 1);
        put(1, 0, (l0 + 2 * tl + t0 + 2) >> 2);
        put(3, 1, (l0 + 2 * tl + t0 + 2) >> 2);
        put(2, 0, (tl + 2 * t0 + t1 + 2) >> 2);
        put(3, 0, (t0 + 2 * t1 + t2 + 2) >> 2);
        put(0, 1, (l0 + l1 + 1) >> 1);
        put(2, 2, (l0 + l1 + 1) >> 1);
        put(1, 1, (tl + 2 * l0 + l1 + 2) >> 2);
        put(3, 2, (tl + 2 * l0 + l1 + 2) >> 2);
        put(0, 2, (l1 + l2 + 1) >> 1);
        put(2, 3, (l1 + l2 + 1) >> 1);
        put(1, 2, (l0 + 2 * l1 + l2 + 2) >> 2);
        put(3, 3, (l0 + 2 * l1 + l2 + 2) >> 2);
        put(0, 3, (l2 + l3 + 1) >> 1);
        put(1, 3, (l1 + 2 * l2 + l3 + 2) >> 2);
        break;
    case 7: // Vertical-Left
        put(0, 0, (t0 + t1 + 1) >> 1);
        put(1, 0, (t1 + t2 + 1) >> 1);
        put(0, 2, (t1 + t2 + 1) >> 1);
        put(2, 0, (t2 + t3 + 1) >> 1);
        put(1, 2, (t2 + t3 + 1) >> 1);
        put(3, 0, (t3 + t4 + 1) >> 1);
        put(2, 2, (t3 + t4 + 1) >> 1);
        put(3, 2, (t4 + t5 + 1) >> 1);
        put(0, 1, (t0 + 2 * t1 + t2 + 2) >> 2);
        put(1, 1, (t1 + 2 * t2 + t3 + 2) >> 2);
        put(0, 3, (t1 + 2 * t2 + t3 + 2) >> 2);
        put(2, 1, (t2 + 2 * t3 + t4 + 2) >> 2);
        put(1, 3, (t2 + 2 * t3 + t4 + 2) >> 2);
        put(3, 1, (t3 + 2 * t4 + t5 + 2) >> 2);
        put(2, 3, (t3 + 2 * t4 + t5 + 2) >> 2);
        put(3, 3, (t4 + 2 * t5 + t6 + 2) >> 2);
        break;
    case 8: // Horizontal-Up
        put(0, 0, (l0 + l1 + 1) >> 1);
        put(1, 0, (l0 + 2 * l1 + l2 + 2) >> 2);
        put(2, 0, (l1 + l2 + 1) >> 1);
        put(0, 1, (l1 + l2 + 1) >> 1);
        put(3, 0, (l1 + 2 * l2 + l3 + 2) >> 2);
        put(1, 1, (l1 + 2 * l2 + l3 + 2) >> 2);
        put(2, 1, (l2 + l3 + 1) >> 1);
        put(0, 2, (l2 + l3 + 1) >> 1);
        put(3, 1, (l2 + 2 * l3 + l3 + 2) >> 2);
        put(1, 2, (l2 + 2 * l3 + l3 + 2) >> 2);
        put(3, 2, l3);
        put(1, 3, l3);
        put(0, 3, l3);
        put(2, 2, l3);
        put(2, 3, l3);
        put(3, 3, l3);
        break;
    default:
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                put(x, y, 128);
        break;
    }
}

// Chroma 8x8 Intra pred modes 0=DC, 1=H, 2=V, 3=Plane
inline void predChroma8(int mode, uint8_t* mb, int stride, const uint8_t* above,
                        const uint8_t* left, uint8_t tl, bool hasA, bool hasL) {
    if (mode == 0) { // DC — four 4x4 regions (ITU 8.3.4 / FFmpeg pred8x8_dc)
        auto fill = [&](int x0, int y0, int v) {
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    mb[(y0 + y) * stride + x0 + x] = static_cast<uint8_t>(v);
        };
        // Single-sum rounding: (s+2)>>2 or (sA+sL+4)>>3 — not nested avgs.
        auto sum4 = [](const uint8_t* p) {
            return p[0] + p[1] + p[2] + p[3];
        };
        if (hasA && hasL) {
            fill(0, 0, (sum4(above) + sum4(left) + 4) >> 3);
            fill(4, 0, (sum4(above + 4) + 2) >> 2);
            fill(0, 4, (sum4(left + 4) + 2) >> 2);
            fill(4, 4, (sum4(above + 4) + sum4(left + 4) + 4) >> 3);
        } else if (hasA) {
            fill(0, 0, (sum4(above) + 2) >> 2);
            fill(4, 0, (sum4(above + 4) + 2) >> 2);
            fill(0, 4, (sum4(above) + 2) >> 2);
            fill(4, 4, (sum4(above + 4) + 2) >> 2);
        } else if (hasL) {
            fill(0, 0, (sum4(left) + 2) >> 2);
            fill(4, 0, (sum4(left) + 2) >> 2);
            fill(0, 4, (sum4(left + 4) + 2) >> 2);
            fill(4, 4, (sum4(left + 4) + 2) >> 2);
        } else {
            for (int y = 0; y < 8; ++y)
                std::memset(mb + y * stride, 128, 8);
        }
    } else if (mode == 1) { // H
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                mb[y * stride + x] = left[y];
    } else if (mode == 2) { // V
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                mb[y * stride + x] = above[x];
    } else { // Plane
        int H = 0, V = 0;
        for (int i = 0; i < 4; ++i) {
            int alo = (i == 3) ? static_cast<int>(tl) : above[2 - i];
            int llo = (i == 3) ? static_cast<int>(tl) : left[2 - i];
            H += (i + 1) * (above[4 + i] - alo);
            V += (i + 1) * (left[4 + i] - llo);
        }
        int a = 16 * (above[7] + left[7]);
        int b = (17 * H + 16) >> 5;
        int c = (17 * V + 16) >> 5;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                int val = (a + b * (x - 3) + c * (y - 3) + 16) >> 5;
                mb[y * stride + x] = static_cast<uint8_t>(clip8(val));
            }
    }
}

// Chroma QP from luma QP (default table without pps offset — offset applied by caller)
static const int kChromaQP[52] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38,
    39, 39, 39, 39};

inline int chromaQp(int qpy, int offset) {
    int q = qpy + offset;
    if (q < 0)
        q = 0;
    if (q > 51)
        q = 51;
    return kChromaQP[q];
}

inline int wrapQpY(int qpy, int delta) {
    // H.264 8.5.1: QP_Y advances modulo 52 for 8-bit video. Clamping negative
    // deltas to zero corrupts later low-QP macroblocks after rate-control wraps.
    int v = qpy + delta;
    v %= 52;
    if (v < 0)
        v += 52;
    return v;
}

// Inverse chroma DC 2x2 Hadamard + dequant (4:2:0).
// coeff[] is CAVLC scan order matching FFmpeg ff_h264_chroma_dc_scan:
//   scan 0 → (0,0), 1 → (1,0), 2 → (0,1), 3 → (1,1).
// Hadamard + qmul>>7 matches ff_h264_chroma_dc_dequant_idct
// with qmul = (mf[0]*16) << (qp/6 + 2)  (same dequant4 pos0 scale).
inline void invChromaDc2x2(const int16_t coeff[4], int qp, int16_t dc[2][2]) {
    // Raster from FFmpeg scan: a=(0,0), b=(1,0), c=(0,1), d=(1,1)
    const int a0 = coeff[0];
    const int b0 = coeff[1];
    const int c0 = coeff[2];
    const int d0 = coeff[3];
    int a = a0 + b0;
    int e = a0 - b0;
    int b = c0 - d0;
    int c = c0 + d0;
    static const int mf0[6] = {10, 11, 13, 14, 16, 18};
    // FFmpeg: (had * qmul) >> 7; qmul = dequant4_coeff[qp][0]
    const int qmul = (mf0[qp % 6] * 16) << (qp / 6 + 2);
    dc[0][0] = static_cast<int16_t>(((a + c) * qmul) >> 7);
    dc[0][1] = static_cast<int16_t>(((e + b) * qmul) >> 7);
    dc[1][0] = static_cast<int16_t>(((a - c) * qmul) >> 7);
    dc[1][1] = static_cast<int16_t>(((e - b) * qmul) >> 7);
}

} // namespace detail_r

// Reconstruct full I-slice of first IDR/I NAL into YUV420 planar. When trace is
// non-null, captures stable luma prediction/residual/recon data for one MB.
inline ReconResult reconISlice(const uint8_t* annexb, size_t n, ReconTrace* trace = nullptr) {
    using namespace detail_r;
    ReconResult out;
    if (trace)
        trace->mb = LumaMbTrace{};
    auto chain = parseAnnexBChain(annexb, n);
    if (!chain.sps.valid || !chain.pps.valid || !chain.slice.valid) {
        // Distinguish CABAC High-profile (PMS default ladder) from corrupt NALs.
        // parsePpsRbsp rejects entropy_coding_mode_flag=1 → pps.valid=false.
        if (chain.sps.valid && !chain.pps.valid) {
            // Re-probe PPS for CABAC flag without requiring valid CAVLC chain
            size_t ii = 0;
            while (ii + 3 < n) {
                size_t sc = 0;
                if (ii + 3 < n && annexb[ii] == 0 && annexb[ii + 1] == 0 && annexb[ii + 2] == 0 &&
                    annexb[ii + 3] == 1)
                    sc = 4;
                else if (annexb[ii] == 0 && annexb[ii + 1] == 0 && annexb[ii + 2] == 1)
                    sc = 3;
                else {
                    ++ii;
                    continue;
                }
                size_t jj = ii + sc;
                while (jj + 3 < n) {
                    if (annexb[jj] == 0 && annexb[jj + 1] == 0 &&
                        (annexb[jj + 2] == 1 ||
                         (jj + 3 < n && annexb[jj + 2] == 0 && annexb[jj + 3] == 1)))
                        break;
                    ++jj;
                }
                if (jj + 3 >= n)
                    jj = n;
                if ((annexb[ii + sc] & 0x1f) == 8 && ii + sc + 1 < jj) {
                    auto pr = misterplex::detail::removeEpb(annexb + ii + sc + 1, jj - (ii + sc + 1));
                    misterplex::detail::BitReader pbr(pr.data(), pr.size());
                    pbr.ue();
                    pbr.ue();
                    if (pbr.u(1) != 0) {
                        out.fail_reason = "cabac";
                        return out;
                    }
                    break;
                }
                ii = jj;
            }
        }
        out.fail_reason = "no chain";
        return out;
    }
    out.width = chain.sps.width;
    out.height = chain.sps.height;
    const int mbW = (out.width + 15) / 16;
    const int mbH = (out.height + 15) / 16;
    out.mb_total = mbW * mbH;
    out.y.assign(static_cast<size_t>(out.width * out.height), 128);
    const int cw = (out.width + 1) / 2, ch = (out.height + 1) / 2;
    out.u.assign(static_cast<size_t>(cw * ch), 128);
    out.v.assign(static_cast<size_t>(cw * ch), 128);

    // Find VCL
    size_t i = 0;
    const uint8_t* pay = nullptr;
    size_t plen = 0;
    uint8_t ntype = 0;
    while (i + 3 < n) {
        size_t sc = 0;
        if (i + 3 < n && annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 &&
            annexb[i + 3] == 1)
            sc = 4;
        else if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1)
            sc = 3;
        else {
            ++i;
            continue;
        }
        size_t j = i + sc;
        while (j + 3 < n) {
            if (annexb[j] == 0 && annexb[j + 1] == 0 &&
                (annexb[j + 2] == 1 || (j + 3 < n && annexb[j + 2] == 0 && annexb[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 3 >= n)
            j = n;
        uint8_t t = annexb[i + sc] & 0x1f;
        if (t == 5 || t == 1) {
            pay = annexb + i + sc + 1;
            plen = j - (i + sc + 1);
            ntype = t;
            break;
        }
        i = j;
    }
    if (!pay) {
        out.fail_reason = "no VCL";
        return out;
    }
    auto rbsp = misterplex::detail::removeEpb(pay, plen);
    misterplex::detail::BitReader br(rbsp.data(), rbsp.size());
    br.ue();
    br.ue();
    br.ue();
    br.u(chain.log2_max_frame_num);
    if (ntype == 5) {
        br.ue();
        br.u(1);
        br.u(1);
    }
    br.se(); // slice_qp_delta already in chain.slice.slice_qp
    if (chain.pps.deblock_ctrl) {
        uint32_t d = br.ue();
        if (d != 1) {
            br.se();
            br.se();
        }
    }

    int qp = chain.slice.slice_qp;
    // chroma_qp_index_offset from PPS — re-parse roughly; default often -2 for x264
    // Our PPS parser doesn't export it; default 0, x264 uses -2. Probe from known SEI not available.
    // Read chroma offset by re-parsing PPS quickly:
    int chroma_offset = 0;
    {
        // scan PPS NAL
        size_t ii = 0;
        while (ii + 3 < n) {
            size_t sc = 0;
            if (ii + 3 < n && annexb[ii] == 0 && annexb[ii + 1] == 0 && annexb[ii + 2] == 0 &&
                annexb[ii + 3] == 1)
                sc = 4;
            else if (annexb[ii] == 0 && annexb[ii + 1] == 0 && annexb[ii + 2] == 1)
                sc = 3;
            else {
                ++ii;
                continue;
            }
            size_t jj = ii + sc;
            while (jj + 3 < n) {
                if (annexb[jj] == 0 && annexb[jj + 1] == 0 &&
                    (annexb[jj + 2] == 1 ||
                     (jj + 3 < n && annexb[jj + 2] == 0 && annexb[jj + 3] == 1)))
                    break;
                ++jj;
            }
            if (jj + 3 >= n)
                jj = n;
            if ((annexb[ii + sc] & 0x1f) == 8) {
                auto pr = misterplex::detail::removeEpb(annexb + ii + sc + 1, jj - (ii + sc + 1));
                misterplex::detail::BitReader pbr(pr.data(), pr.size());
                pbr.ue();
                pbr.ue();
                pbr.u(1);
                pbr.u(1);
                if (pbr.ue() > 0)
                    break;
                pbr.ue();
                pbr.ue();
                pbr.u(1);
                pbr.u(2);
                pbr.se(); // pic_init_qp
                pbr.se(); // qs
                chroma_offset = pbr.se();
                break;
            }
            ii = jj;
        }
    }

    std::vector<int> tcLuma(static_cast<size_t>(mbW * mbH * 16), -1);
    std::vector<int> tcChr[2] = {std::vector<int>(static_cast<size_t>(mbW * mbH * 4), -1),
                                  std::vector<int>(static_cast<size_t>(mbW * mbH * 4), -1)};
    // Intra4x4 pred modes; -1 = not available (MPM uses DC if either neighbour N/A)
    std::vector<int8_t> i4mode(static_cast<size_t>(mbW * mbH * 16), -1);

    auto tcatL = [&](int mbx, int mby, int lx, int ly) -> int* {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH || lx < 0 || ly < 0 || lx > 3 || ly > 3)
            return nullptr;
        int& v = tcLuma[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)];
        return (v < 0) ? nullptr : &v;
    };
    auto tcsetL = [&](int mbx, int mby, int lx, int ly, int v) {
        tcLuma[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)] = v;
    };
    auto tcatC = [&](int plane, int mbx, int mby, int lx, int ly) -> int* {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH || lx < 0 || ly < 0 || lx > 1 || ly > 1)
            return nullptr;
        int& v = tcChr[plane][static_cast<size_t>(((mby * mbW + mbx) * 4) + ly * 2 + lx)];
        return (v < 0) ? nullptr : &v;
    };
    auto tcsetC = [&](int plane, int mbx, int mby, int lx, int ly, int v) {
        tcChr[plane][static_cast<size_t>(((mby * mbW + mbx) * 4) + ly * 2 + lx)] = v;
    };
    auto modeAt = [&](int mbx, int mby, int lx, int ly) -> int {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH)
            return -1;
        return i4mode[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)];
    };
    auto coeffTokenTable = [](int nC) -> int {
        return nC < 2 ? 0 : (nC < 4 ? 1 : (nC < 8 ? 2 : 3));
    };

    auto yAt = [&](int x, int y) -> uint8_t {
        if (x < 0 || y < 0 || x >= out.width || y >= out.height)
            return 128;
        return out.y[static_cast<size_t>(y * out.width + x)];
    };
    auto setY = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < out.width && y < out.height)
            out.y[static_cast<size_t>(y * out.width + x)] = v;
    };
    // 4x4 decode order index within MB (matches walk_detail::blkXY / i8,i4 loops)
    auto blkOrd = [](int lx, int ly) -> int {
        int i8 = (ly / 2) * 2 + (lx / 2);
        int i4 = (ly % 2) * 2 + (lx % 2);
        return i8 * 4 + i4;
    };
    // True if luma pixel (px,py) belongs to a 4x4 already reconstructed before
    // the 4x4 at (mbx,mby,lx,ly). Critical for Intra4x4 top-right samples:
    // scan order leaves (1,1)/(1,3) TR and all (3,ly>0) TR not yet written.
    auto lumaReady = [&](int px, int py, int mbx, int mby, int lx, int ly) -> bool {
        if (px < 0 || py < 0 || px >= out.width || py >= out.height)
            return false;
        int pmbx = px / 16, pmby = py / 16;
        int cur = mby * mbW + mbx;
        int prev = pmby * mbW + pmbx;
        if (prev < cur)
            return true;
        if (prev > cur)
            return false;
        int plx = (px % 16) / 4, ply = (py % 16) / 4;
        return blkOrd(plx, ply) < blkOrd(lx, ly);
    };

    for (int mby = 0; mby < mbH; ++mby) {
        for (int mbx = 0; mbx < mbW; ++mbx) {
            int mb = mby * mbW + mbx;
            if (!br.ok) {
                out.fail_mb = mb;
                out.fail_reason = "br";
                return out;
            }
            uint32_t mt = br.ue();
            if (mt > 25) {
                out.fail_mb = mb;
                out.fail_reason = "mb_type";
                return out;
            }
            const int baseX = mbx * 16;
            const int baseY = mby * 16;
            const bool hasLeft = mbx > 0;
            const bool hasAbove = mby > 0;
            const bool traceMb = trace && (mb == trace->target_mb);
            if (traceMb) {
                trace->mb.valid = true;
                trace->mb.mb = mb;
                trace->mb.mb_x = mbx;
                trace->mb.mb_y = mby;
                trace->mb.mb_type = static_cast<int>(mt);
                trace->mb.qp = qp;
            }
            auto finishTraceMb = [&]() {
                if (!traceMb)
                    return;
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 16; ++xx)
                        trace->mb.recon[static_cast<size_t>(yy * 16 + xx)] =
                            yAt(baseX + xx, baseY + yy);
            };
            auto traceBlock = [&](int block, int lx, int ly, int predMode, int totalCoeff,
                                  bool nAAvail, int nA, bool nBAvail, int nB, int nC,
                                  int table, const uint8_t pred[16], const int16_t coeff[16],
                                  int bitStart, int bitEnd, const int16_t blkq[4][4]) {
                if (!traceMb)
                    return;
                Luma4x4Trace& tb = trace->mb.blocks[static_cast<size_t>(block)];
                tb.block = block;
                tb.x = lx * 4;
                tb.y = ly * 4;
                tb.pred_mode = predMode;
                tb.total_coeff = totalCoeff;
                tb.nA_available = nAAvail;
                tb.nA_total_coeff = nA;
                tb.nB_available = nBAvail;
                tb.nB_total_coeff = nB;
                tb.predicted_nC = nC;
                tb.coeff_token_table = table;
                tb.bit_offset_start = bitStart;
                tb.bit_offset_end = bitEnd;
                int16_t idct[4][4];
                idct4x4_residual(blkq, idct);
                for (int k = 0; k < 16; ++k)
                    tb.coeff[static_cast<size_t>(k)] = coeff ? coeff[k] : 0;
                for (int yy = 0; yy < 4; ++yy) {
                    for (int xx = 0; xx < 4; ++xx) {
                        const size_t k = static_cast<size_t>(yy * 4 + xx);
                        tb.pred[k] = pred[k];
                        tb.dequant[k] = blkq[yy][xx];
                        tb.idct[k] = idct[yy][xx];
                        tb.recon[k] = yAt(baseX + lx * 4 + xx, baseY + ly * 4 + yy);
                        trace->mb.pred[static_cast<size_t>((ly * 4 + yy) * 16 + lx * 4 + xx)] =
                            pred[k];
                    }
                }
            };

            if (mt == 25) {
                while (br.ok && (br.bit % 8) != 0)
                    br.u(1);
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 16; ++xx)
                        setY(baseX + xx, baseY + yy, static_cast<uint8_t>(br.u(8)));
                for (int p = 0; p < 2; ++p)
                    for (int k = 0; k < 64; ++k)
                        br.u(8);
                for (int ly = 0; ly < 4; ++ly)
                    for (int lx = 0; lx < 4; ++lx)
                        tcsetL(mbx, mby, lx, ly, 16);
                finishTraceMb();
                out.mb_decoded++;
                continue;
            }

            if (mt == 0) {
                // I_NxN — 16 pred modes in 8x8-group / 4x4 scan order
                int predModes[16];
                for (int blk = 0; blk < 16; ++blk) {
                    int i8 = blk / 4, i4 = blk % 4;
                    int lx, ly;
                    walk_detail::blkXY(i8, i4, lx, ly);
                    // Spec 8.3.1.2 / FFmpeg pred_intra_mode: if either neighbour
                    // unavailable (mode < 0), pred = DC (2); else min(A, B).
                    int modeA = (lx > 0) ? modeAt(mbx, mby, lx - 1, ly)
                                         : modeAt(mbx - 1, mby, 3, ly);
                    int modeB = (ly > 0) ? modeAt(mbx, mby, lx, ly - 1)
                                         : modeAt(mbx, mby - 1, lx, 3);
                    int pred = (modeA < 0 || modeB < 0) ? 2 : std::min(modeA, modeB);
                    if (br.u(1)) {
                        predModes[blk] = pred;
                    } else {
                        int rem = static_cast<int>(br.u(3));
                        predModes[blk] = rem + (rem >= pred ? 1 : 0);
                    }
                    i4mode[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)] =
                        static_cast<int8_t>(predModes[blk]);
                }
                int chromaMode = static_cast<int>(br.ue());
                uint32_t code = br.ue();
                if (code >= 48) {
                    out.fail_mb = mb;
                    out.fail_reason = "me_cbp";
                    return out;
                }
                int cbp = walk_detail::kMeIntra[code];
                int cbp_l = cbp & 15;
                int cbp_c = cbp >> 4;
                if (traceMb) {
                    trace->mb.chroma_mode = chromaMode;
                    trace->mb.cbp_luma = cbp_l;
                    trace->mb.cbp_chroma = cbp_c;
                }
                if (cbp != 0) {
                    int d = br.se();
                    qp = wrapQpY(qp, d);
                }
                if (traceMb) {
                    trace->mb.qp = qp;
                    trace->mb.pred_mode = -1; // per-4x4 modes live in blocks[]
                }

                // Predict + residual each 4x4
                for (int i8 = 0; i8 < 4; ++i8) {
                    for (int i4 = 0; i4 < 4; ++i4) {
                        int lx, ly;
                        walk_detail::blkXY(i8, i4, lx, ly);
                        int blk = i8 * 4 + i4;
                        int mode = predModes[blk];
                        int x0 = baseX + lx * 4;
                        int y0 = baseY + ly * 4;
                        uint8_t above[8], left[4], tl = 128;
                        // Top/left neighbours must already be reconstructed (spec availability)
                        bool ha = (y0 > 0) && lumaReady(x0, y0 - 1, mbx, mby, lx, ly);
                        bool hl = (x0 > 0) && lumaReady(x0 - 1, y0, mbx, mby, lx, ly);
                        for (int t = 0; t < 4; ++t)
                            above[t] = ha ? yAt(x0 + t, y0 - 1) : 128;
                        // Top-right: only use if each sample's 4x4 is already decoded;
                        // else replicate above[3] (ITU 8.3.1.2 / FFmpeg).
                        for (int t = 0; t < 4; ++t) {
                            int tx = x0 + 4 + t;
                            if (ha && lumaReady(tx, y0 - 1, mbx, mby, lx, ly))
                                above[4 + t] = yAt(tx, y0 - 1);
                            else
                                above[4 + t] = above[3];
                        }
                        for (int t = 0; t < 4; ++t)
                            left[t] = hl ? yAt(x0 - 1, y0 + t) : 128;
                        if (ha && hl && lumaReady(x0 - 1, y0 - 1, mbx, mby, lx, ly))
                            tl = yAt(x0 - 1, y0 - 1);
                        else if (ha)
                            tl = above[0];
                        else if (hl)
                            tl = left[0];

                        // Unavailable mode restrictions: V needs above, H needs left
                        int useMode = mode;
                        if (!ha && (mode == 0 || mode == 3 || mode == 7))
                            useMode = 2;
                        if (!hl && (mode == 1 || mode == 8))
                            useMode = 2;
                        if ((!ha || !hl) && (mode == 4 || mode == 5 || mode == 6))
                            useMode = 2;

                        uint8_t pred[16];
                        predI4(useMode, pred, 4, above, left, tl, ha, hl);
                        for (int yy = 0; yy < 4; ++yy)
                            for (int xx = 0; xx < 4; ++xx)
                                setY(x0 + xx, y0 + yy, pred[yy * 4 + xx]);

                        int16_t blkq[4][4]{};
                        int16_t coeff[16]{};
                        int totalCoeff = 0;
                        int bitStart = -1;
                        int bitEnd = -1;
                        int* nAptr = (lx > 0) ? tcatL(mbx, mby, lx - 1, ly)
                                              : tcatL(mbx - 1, mby, 3, ly);
                        int* nBptr = (ly > 0) ? tcatL(mbx, mby, lx, ly - 1)
                                              : tcatL(mbx, mby - 1, lx, 3);
                        const bool nAAvail = nAptr != nullptr;
                        const bool nBAvail = nBptr != nullptr;
                        const int nA = nAAvail ? *nAptr : -1;
                        const int nB = nBAvail ? *nBptr : -1;
                        const int nC = walk_detail::ncFrom(nAptr, nBptr);
                        const int table = coeffTokenTable(nC);
                        if ((cbp_l >> i8) & 1) {
                            bitStart = static_cast<int>(br.bit);
                            auto r = cavlc::residualBlock(br, nC, 16);
                            bitEnd = static_cast<int>(br.bit);
                            if (!r.ok) {
                                out.fail_mb = mb;
                                out.fail_reason = "I4_res";
                                return out;
                            }
                            tcsetL(mbx, mby, lx, ly, r.total_coeff);
                            totalCoeff = r.total_coeff;
                            std::memcpy(coeff, r.coeff, sizeof(coeff));
                            dequant4x4(r.coeff, 16, qp, blkq);
                            uint8_t tmp[16];
                            for (int yy = 0; yy < 4; ++yy)
                                for (int xx = 0; xx < 4; ++xx)
                                    tmp[yy * 4 + xx] = yAt(x0 + xx, y0 + yy);
                            idct4x4_add(blkq, tmp, 4);
                            for (int yy = 0; yy < 4; ++yy)
                                for (int xx = 0; xx < 4; ++xx)
                                    setY(x0 + xx, y0 + yy, tmp[yy * 4 + xx]);
                        } else {
                            tcsetL(mbx, mby, lx, ly, 0);
                        }
                        traceBlock(blk, lx, ly, useMode, totalCoeff, nAAvail, nA, nBAvail, nB,
                                   nC, table, pred, coeff, bitStart, bitEnd, blkq);
                    }
                }

                // Chroma
                int qpc = chromaQp(qp, chroma_offset);
                uint8_t* planes[2] = {out.u.data(), out.v.data()};
                for (int p = 0; p < 2; ++p) {
                    uint8_t cmb[64];
                    uint8_t above[8], left[8], tl = 128;
                    int cx = mbx * 8, cy = mby * 8;
                    bool ha = mby > 0, hl = mbx > 0;
                    for (int t = 0; t < 8; ++t) {
                        above[t] = ha ? planes[p][(cy - 1) * cw + cx + t] : 128;
                        left[t] = hl ? planes[p][(cy + t) * cw + cx - 1] : 128;
                    }
                    if (ha && hl)
                        tl = planes[p][(cy - 1) * cw + cx - 1];
                    predChroma8(chromaMode, cmb, 8, above, left, tl, ha, hl);
                    for (int yy = 0; yy < 8; ++yy)
                        for (int xx = 0; xx < 8; ++xx)
                            if (cx + xx < cw && cy + yy < ch)
                                planes[p][(cy + yy) * cw + cx + xx] = cmb[yy * 8 + xx];
                }
                if (cbp_c) {
                    auto r0 = cavlc::residualBlock(br, -1, 4);
                    auto r1 = cavlc::residualBlock(br, -1, 4);
                    if (!r0.ok || !r1.ok) {
                        out.fail_mb = mb;
                        out.fail_reason = "chrDC";
                        return out;
                    }
                    int16_t dcU[2][2], dcV[2][2];
                    invChromaDc2x2(r0.coeff, qpc, dcU);
                    invChromaDc2x2(r1.coeff, qpc, dcV);
                    int16_t(*dcs[2])[2][2] = {&dcU, &dcV};
                    for (int p = 0; p < 2; ++p) {
                        for (int by = 0; by < 2; ++by)
                            for (int bx = 0; bx < 2; ++bx) {
                                int16_t blkq[4][4]{};
                                if (cbp_c == 2) {
                                    int b = by * 2 + bx;
                                    int lx = b % 2, ly = b / 2;
                                    int* nA = (lx > 0) ? tcatC(p, mbx, mby, lx - 1, ly)
                                                       : tcatC(p, mbx - 1, mby, 1, ly);
                                    int* nB = (ly > 0) ? tcatC(p, mbx, mby, lx, ly - 1)
                                                       : tcatC(p, mbx, mby - 1, lx, 1);
                                    auto rr =
                                        cavlc::residualBlock(br, walk_detail::ncFrom(nA, nB), 15);
                                    if (!rr.ok) {
                                        out.fail_mb = mb;
                                        out.fail_reason = "chrAC";
                                        return out;
                                    }
                                    tcsetC(p, mbx, mby, lx, ly, rr.total_coeff);
                                    dequant4x4(rr.coeff, 15, qpc, blkq);
                                } else {
                                    tcsetC(p, mbx, mby, bx, by, 0);
                                }
                                // Chroma DC into (0,0) of each 4x4 residual
                                blkq[0][0] = (*dcs[p])[by][bx];
                                int x0 = mbx * 8 + bx * 4;
                                int y0 = mby * 8 + by * 4;
                                uint8_t tmp[16];
                                for (int yy = 0; yy < 4; ++yy)
                                    for (int xx = 0; xx < 4; ++xx)
                                        tmp[yy * 4 + xx] =
                                            (x0 + xx < cw && y0 + yy < ch)
                                                ? planes[p][(y0 + yy) * cw + x0 + xx]
                                                : 128;
                                idct4x4_add(blkq, tmp, 4);
                                for (int yy = 0; yy < 4; ++yy)
                                    for (int xx = 0; xx < 4; ++xx)
                                        if (x0 + xx < cw && y0 + yy < ch)
                                            planes[p][(y0 + yy) * cw + x0 + xx] = tmp[yy * 4 + xx];
                            }
                    }
                } else {
                    for (int p = 0; p < 2; ++p)
                        for (int b = 0; b < 4; ++b) {
                            int lx, ly;
                            walk_detail::chrXY(b, lx, ly);
                            tcsetC(p, mbx, mby, lx, ly, 0);
                        }
                }
            } else {
                // I_16x16
                int x = static_cast<int>(mt) - 1;
                int predMode = x % 4;
                int cbp_c = (x / 4) % 3;
                int cbp_l = (x / 12) ? 15 : 0;
                int chromaMode = static_cast<int>(br.ue());
                int d = br.se();
                qp = wrapQpY(qp, d);
                if (traceMb) {
                    trace->mb.qp = qp;
                    trace->mb.chroma_mode = chromaMode;
                    trace->mb.cbp_luma = cbp_l;
                    trace->mb.cbp_chroma = cbp_c;
                }

                uint8_t above[16], left[16], tl = 128;
                for (int t = 0; t < 16; ++t) {
                    above[t] = hasAbove ? yAt(baseX + t, baseY - 1) : 128;
                    left[t] = hasLeft ? yAt(baseX - 1, baseY + t) : 128;
                }
                if (hasAbove && hasLeft)
                    tl = yAt(baseX - 1, baseY - 1);

                uint8_t mbpred[256];
                if (predMode == 0 && hasAbove)
                    predI16_V(mbpred, 16, above);
                else if (predMode == 1 && hasLeft)
                    predI16_H(mbpred, 16, left);
                else if (predMode == 3 && hasAbove && hasLeft)
                    predI16_Plane(mbpred, 16, above, left, tl);
                else
                    predI16_DC(mbpred, 16, above, left, hasAbove, hasLeft);

                if (traceMb) {
                    trace->mb.pred_mode = predMode;
                    for (int yy = 0; yy < 16; ++yy)
                        for (int xx = 0; xx < 16; ++xx)
                            trace->mb.pred[static_cast<size_t>(yy * 16 + xx)] =
                                mbpred[yy * 16 + xx];
                }
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 16; ++xx)
                        setY(baseX + xx, baseY + yy, mbpred[yy * 16 + xx]);

                int* nA = tcatL(mbx - 1, mby, 3, 0);
                int* nB = tcatL(mbx, mby - 1, 0, 3);
                auto rdc = cavlc::residualBlock(br, walk_detail::ncFrom(nA, nB), 16);
                if (!rdc.ok) {
                    out.fail_mb = mb;
                    out.fail_reason = "I16_dc";
                    return out;
                }
                int16_t dc[4][4];
                cavlc::invQuantHadamardDc4x4(rdc.coeff, qp, dc);

                for (int i8 = 0; i8 < 4; ++i8)
                    for (int i4 = 0; i4 < 4; ++i4) {
                        int lx, ly;
                        walk_detail::blkXY(i8, i4, lx, ly);
                        int16_t blkq[4][4]{};
                        int16_t coeff[16]{};
                        int totalCoeff = (dc[ly][lx] != 0) ? 1 : 0;
                        int bitStart = -1;
                        int bitEnd = -1;
                        int* nAptr = (lx > 0) ? tcatL(mbx, mby, lx - 1, ly)
                                              : tcatL(mbx - 1, mby, 3, ly);
                        int* nBptr = (ly > 0) ? tcatL(mbx, mby, lx, ly - 1)
                                              : tcatL(mbx, mby - 1, lx, 3);
                        const bool nAAvail = nAptr != nullptr;
                        const bool nBAvail = nBptr != nullptr;
                        const int nA = nAAvail ? *nAptr : -1;
                        const int nB = nBAvail ? *nBptr : -1;
                        const int nC = walk_detail::ncFrom(nAptr, nBptr);
                        const int table = cbp_l ? coeffTokenTable(nC) : -1;
                        if (cbp_l) {
                            bitStart = static_cast<int>(br.bit);
                            auto rr = cavlc::residualBlock(br, nC, 15);
                            bitEnd = static_cast<int>(br.bit);
                            if (!rr.ok) {
                                out.fail_mb = mb;
                                out.fail_reason = "I16_ac";
                                return out;
                            }
                            tcsetL(mbx, mby, lx, ly, rr.total_coeff);
                            totalCoeff += rr.total_coeff;
                            std::memcpy(coeff, rr.coeff, sizeof(coeff));
                            dequant4x4(rr.coeff, 15, qp, blkq);
                        } else {
                            tcsetL(mbx, mby, lx, ly, 0);
                        }
                        // Inject DC (already fully scaled for idct add path)
                        // FFmpeg: DC is stored and used as block[0] before idct with different scale.
                        // invQuantHadamardDc4x4 produces values meant as residual DC after >> .
                        // Add into dequant'd (0,0) then idct.
                        blkq[0][0] = static_cast<int16_t>(blkq[0][0] + (dc[ly][lx] << 6) / 1);
                        // Standard: after Hadamard+dequant, DC is at full residual domain before
                        // inverse transform; idct does (x+32)>>6. So DC should be pre-scaled
                        // by not shifting extra. cavlc::invQuantHadamard already applies >>1 in had.
                        // Match reconFirstI16DcMeanY: val = 128 + dc[by][bx] without idct for DC-only.
                        // For full idct, DC coefficient before idct is dc*64? Actually JM puts
                        // dequantized DC directly as block[0][0] and runs idct.
                        // Re-do: use dc as block[0][0] directly (overwrite add).
                        blkq[0][0] = dc[ly][lx];
                        if (cbp_l) {
                            // re-dequant AC only into blk, then set DC
                            // already did; just set DC after dequant of AC (AC dequant leaves 0,0=0
                            // for max15)
                        }
                        int x0 = baseX + lx * 4, y0 = baseY + ly * 4;
                        uint8_t tmp[16];
                        for (int yy = 0; yy < 4; ++yy)
                            for (int xx = 0; xx < 4; ++xx)
                                tmp[yy * 4 + xx] = yAt(x0 + xx, y0 + yy);
                        idct4x4_add(blkq, tmp, 4);
                        for (int yy = 0; yy < 4; ++yy)
                            for (int xx = 0; xx < 4; ++xx)
                                setY(x0 + xx, y0 + yy, tmp[yy * 4 + xx]);
                        i4mode[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)] = 2;
                        uint8_t predBlk[16];
                        for (int yy = 0; yy < 4; ++yy)
                            for (int xx = 0; xx < 4; ++xx)
                                predBlk[yy * 4 + xx] =
                                    mbpred[(ly * 4 + yy) * 16 + lx * 4 + xx];
                        traceBlock(i8 * 4 + i4, lx, ly, predMode, totalCoeff, nAAvail, nA,
                                   nBAvail, nB, nC, table, predBlk, coeff, bitStart, bitEnd,
                                   blkq);
                    }

                // Chroma for I16
                int qpc = chromaQp(qp, chroma_offset);
                uint8_t* planes[2] = {out.u.data(), out.v.data()};
                for (int p = 0; p < 2; ++p) {
                    uint8_t cmb[64];
                    uint8_t ab[8], lf[8], tlp = 128;
                    int cx = mbx * 8, cy = mby * 8;
                    bool ha = mby > 0, hl = mbx > 0;
                    for (int t = 0; t < 8; ++t) {
                        ab[t] = ha ? planes[p][(cy - 1) * cw + cx + t] : 128;
                        lf[t] = hl ? planes[p][(cy + t) * cw + cx - 1] : 128;
                    }
                    if (ha && hl)
                        tlp = planes[p][(cy - 1) * cw + cx - 1];
                    predChroma8(chromaMode, cmb, 8, ab, lf, tlp, ha, hl);
                    for (int yy = 0; yy < 8; ++yy)
                        for (int xx = 0; xx < 8; ++xx)
                            if (cx + xx < cw && cy + yy < ch)
                                planes[p][(cy + yy) * cw + cx + xx] = cmb[yy * 8 + xx];
                }
                if (cbp_c) {
                    auto r0 = cavlc::residualBlock(br, -1, 4);
                    auto r1 = cavlc::residualBlock(br, -1, 4);
                    if (!r0.ok || !r1.ok) {
                        out.fail_mb = mb;
                        out.fail_reason = "chrDC";
                        return out;
                    }
                    int16_t dcU[2][2], dcV[2][2];
                    invChromaDc2x2(r0.coeff, qpc, dcU);
                    invChromaDc2x2(r1.coeff, qpc, dcV);
                    int16_t(*dcs[2])[2][2] = {&dcU, &dcV};
                    for (int p = 0; p < 2; ++p) {
                        for (int by = 0; by < 2; ++by)
                            for (int bx = 0; bx < 2; ++bx) {
                                int16_t blkq[4][4]{};
                                if (cbp_c == 2) {
                                    int lx = bx, ly = by;
                                    int* a = (lx > 0) ? tcatC(p, mbx, mby, lx - 1, ly)
                                                      : tcatC(p, mbx - 1, mby, 1, ly);
                                    int* b = (ly > 0) ? tcatC(p, mbx, mby, lx, ly - 1)
                                                      : tcatC(p, mbx, mby - 1, lx, 1);
                                    auto rr =
                                        cavlc::residualBlock(br, walk_detail::ncFrom(a, b), 15);
                                    if (!rr.ok) {
                                        out.fail_mb = mb;
                                        out.fail_reason = "chrAC";
                                        return out;
                                    }
                                    tcsetC(p, mbx, mby, lx, ly, rr.total_coeff);
                                    dequant4x4(rr.coeff, 15, qpc, blkq);
                                } else {
                                    tcsetC(p, mbx, mby, bx, by, 0);
                                }
                                blkq[0][0] = (*dcs[p])[by][bx];
                                int x0 = mbx * 8 + bx * 4, y0 = mby * 8 + by * 4;
                                uint8_t tmp[16];
                                for (int yy = 0; yy < 4; ++yy)
                                    for (int xx = 0; xx < 4; ++xx)
                                        tmp[yy * 4 + xx] =
                                            (x0 + xx < cw && y0 + yy < ch)
                                                ? planes[p][(y0 + yy) * cw + x0 + xx]
                                                : 128;
                                idct4x4_add(blkq, tmp, 4);
                                for (int yy = 0; yy < 4; ++yy)
                                    for (int xx = 0; xx < 4; ++xx)
                                        if (x0 + xx < cw && y0 + yy < ch)
                                            planes[p][(y0 + yy) * cw + x0 + xx] = tmp[yy * 4 + xx];
                            }
                    }
                } else {
                    for (int p = 0; p < 2; ++p)
                        for (int b = 0; b < 4; ++b) {
                            int lx, ly;
                            walk_detail::chrXY(b, lx, ly);
                            tcsetC(p, mbx, mby, lx, ly, 0);
                        }
                }
            }
            finishTraceMb();
            out.mb_decoded++;
        }
    }
    return out;
}

// YUV420 → RGB565 (BT.601 full-range style, integer)
inline void yuv420ToRgb565(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h,
                           std::vector<uint16_t>& rgb) {
    rgb.resize(static_cast<size_t>(w * h));
    const int cw = (w + 1) / 2;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            int Y = y[j * w + i];
            int U = u[(j / 2) * cw + (i / 2)] - 128;
            int V = v[(j / 2) * cw + (i / 2)] - 128;
            int R = Y + ((360 * V) >> 8);
            int G = Y - ((88 * U + 184 * V) >> 8);
            int B = Y + ((455 * U) >> 8);
            R = detail_r::clip8(R);
            G = detail_r::clip8(G);
            B = detail_r::clip8(B);
            rgb[static_cast<size_t>(j * w + i)] =
                static_cast<uint16_t>(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
        }
    }
}

// Mean absolute error of Y vs golden (or -1 if size mismatch)
inline double maeY(const ReconResult& r, const uint8_t* golden, size_t glen) {
    if (!golden || r.y.size() != glen)
        return -1.0;
    double s = 0;
    for (size_t i = 0; i < glen; ++i)
        s += std::abs(static_cast<int>(r.y[i]) - static_cast<int>(golden[i]));
    return s / static_cast<double>(glen);
}

} // namespace recon
} // namespace misterplex
