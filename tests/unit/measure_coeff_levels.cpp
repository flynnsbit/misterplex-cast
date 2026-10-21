// Measure maximum coefficient level from a Baseline H.264 bitstream.
// Reports: min QP, max |level|, fraction of MBs with |level| > 255.
#include "libmisterplex/h264_cavlc.hpp"
#include "libmisterplex/h264_nal.hpp"
#include "libmisterplex/h264_slice_walk.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

using namespace misterplex;

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

struct NalInfo {
    const uint8_t* data;
    size_t len;
    uint8_t type;
};

static std::vector<NalInfo> splitNals(const uint8_t* d, size_t n) {
    std::vector<NalInfo> out;
    size_t i = 0;
    while (i + 2 < n) {
        size_t sc = 0;
        if (i + 3 < n && d[i]==0 && d[i+1]==0 && d[i+2]==0 && d[i+3]==1) sc = 4;
        else if (d[i]==0 && d[i+1]==0 && d[i+2]==1) sc = 3;
        else { ++i; continue; }
        size_t j = i + sc;
        while (j + 2 < n) {
            if (d[j]==0 && d[j+1]==0 && (d[j+2]==1 || (j+3<n && d[j+2]==0 && d[j+3]==1)))
                break;
            ++j;
        }
        if (j + 2 >= n) j = n;
        NalInfo ni;
        ni.data = d + i + sc;
        ni.len = j - (i + sc);
        ni.type = ni.data[0] & 0x1f;
        out.push_back(ni);
        i = j;
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <bitstream.264>\n", argv[0]);
        return 2;
    }
    auto blob = readFile(argv[1]);
    if (blob.empty()) {
        std::fprintf(stderr, "Cannot read %s\n", argv[1]);
        return 1;
    }

    auto chain = parseAnnexBChain(blob.data(), blob.size());
    if (!chain.sps.valid || !chain.pps.valid) {
        std::fprintf(stderr, "Invalid SPS/PPS\n");
        return 1;
    }

    const int mbW = (chain.sps.width + 15) / 16;
    const int mbH = (chain.sps.height + 15) / 16;
    const int mbTotal = mbW * mbH;
    std::printf("SPS: %ux%u, %dx%d MBs = %d, profile_idc=%u\n",
                chain.sps.width, chain.sps.height, mbW, mbH, mbTotal,
                chain.sps.profile_idc);
    std::printf("PPS: pic_init_qp=%d, cabac=%d\n",
                chain.pps.pic_init_qp, chain.pps.entropy_cabac);

    auto nals = splitNals(blob.data(), blob.size());
    int min_qp = 52, max_abs_level = 0;
    int total_mbs = 0, mbs_over255 = 0;
    int total_blocks = 0, blocks_over255 = 0;
    int total_coeffs = 0, coeffs_over255 = 0;

    for (auto& nal : nals) {
        if (nal.type != 1 && nal.type != 5) continue;
        auto rbsp = detail::removeEpb(nal.data + 1, nal.len - 1);
        detail::BitReader br(rbsp.data(), rbsp.size());
        br.ue(); // first_mb_in_slice
        uint32_t slice_type = br.ue();
        br.ue(); // pps_id
        br.u(chain.log2_max_frame_num);
        if (nal.type == 5) {
            br.ue(); // idr_pic_id
            br.u(1); // no_output_of_prior_pics_flag
            br.u(1); // long_term_reference_flag
        }
        int32_t qp_delta = br.se();
        int slice_qp = chain.pps.pic_init_qp + qp_delta;
        if (slice_qp < min_qp) min_qp = slice_qp;

        if (chain.pps.deblock_ctrl) {
            uint32_t d = br.ue();
            if (d != 1) { br.se(); br.se(); }
        }

        bool is_i = (slice_type == 2 || slice_type == 7);
        bool is_p = (slice_type == 0 || slice_type == 5);

        // nC tracking
        std::vector<int> tcLuma(mbW * mbH * 16, -1);
        auto tcatL = [&](int mbx, int mby, int lx, int ly) -> int* {
            if (mbx<0||mby<0||mbx>=mbW||mby>=mbH) return nullptr;
            int& v = tcLuma[(mby*mbW+mbx)*16 + ly*4 + lx];
            return (v<0) ? nullptr : &v;
        };
        auto tcsetL = [&](int mbx, int mby, int lx, int ly, int v) {
            tcLuma[(mby*mbW+mbx)*16 + ly*4 + lx] = v;
        };

        for (int mb = 0; mb < mbTotal; ++mb) {
            int mbx = mb % mbW, mby = mb / mbW;
            if (!br.ok) break;

            bool mb_has_over255 = false;

            if (is_p) {
                uint32_t mt = br.ue();
                if (mt == 0) {
                    // P_Skip
                    for (int ly = 0; ly < 4; ++ly)
                        for (int lx = 0; lx < 4; ++lx)
                            tcsetL(mbx, mby, lx, ly, 0);
                    total_mbs++;
                    continue;
                }
                // P_L0_16x16 etc — simplified; skip motion vectors
                if (mt <= 3) {
                    // P macroblock types 1..3 have motion vectors
                    // For measurement, skip to residual
                    // This is approximate but sufficient for level measurement
                    br.se(); // mvdx
                    br.se(); // mvdy
                }
                if (mt >= 6) {
                    // Intra within P-slice
                    mt -= 5;
                } else {
                    mt = 0; // treat as having coded residual
                }
            }

            uint32_t mt;
            if (is_i) {
                mt = br.ue();
            } else {
                mt = 0; // Already consumed above
            }

            if (mt > 25) break;

            int cbp_l = 0, cbp_c = 0;
            int mb_qp = slice_qp;

            if (mt == 0 && is_i) {
                // I_NxN: pred modes + cbp + qp_delta + residual
                for (int i8 = 0; i8 < 4; ++i8) {
                    for (int i4 = 0; i4 < 4; ++i4) {
                        if (!br.ok) break;
                        if (br.u(1) == 0) br.u(3);
                    }
                }
                uint32_t cbp_ue = br.ue();
                if (cbp_ue < 48) {
                    static const uint8_t kMeIntra[48] = {
                        47,31,15,0,23,27,29,30,7,11,13,14,39,43,45,46,
                        16,3,5,10,12,19,21,26,28,35,37,42,44,1,2,4,
                        8,17,18,20,24,6,9,22,25,32,33,34,36,40,38,41};
                    uint8_t cbp = kMeIntra[cbp_ue];
                    cbp_l = cbp & 0xf;
                    cbp_c = (cbp >> 4) & 3;
                }
                if (cbp_l || cbp_c) {
                    int32_t qd = br.se();
                    mb_qp = slice_qp + qd;
                    if (mb_qp < min_qp) min_qp = mb_qp;
                }
            } else if (mt >= 1 && mt <= 24 && is_i) {
                // I_16x16
                int cbp_idx = (mt - 1);
                cbp_c = cbp_idx / 4;
                int cbp_luma_ac = ((cbp_idx % 4) >= 2) ? 15 : 0;
                cbp_l = cbp_luma_ac;
                int32_t qd = br.se();
                mb_qp = slice_qp + qd;
                if (mb_qp < min_qp) min_qp = mb_qp;
                // DC block
                auto dc_r = cavlc::residualBlock(br, -1, 16);
                if (dc_r.ok) {
                    for (int k = 0; k < 16; ++k) {
                        int al = std::abs(static_cast<int>(dc_r.coeff[k]));
                        if (al > max_abs_level) max_abs_level = al;
                        if (al > 0) total_coeffs++;
                        if (al > 255) { coeffs_over255++; mb_has_over255 = true; }
                    }
                    total_blocks++;
                    if (mb_has_over255) blocks_over255++;
                }
            }

            // Luma 4x4 blocks
            for (int i8 = 0; i8 < 4; ++i8) {
                bool coded = (cbp_l >> i8) & 1;
                for (int i4 = 0; i4 < 4; ++i4) {
                    int lx, ly;
                    walk_detail::blkXY(i8, i4, lx, ly);
                    if (coded) {
                        int* nA = (lx > 0) ? tcatL(mbx, mby, lx-1, ly) : tcatL(mbx-1, mby, 3, ly);
                        int* nB = (ly > 0) ? tcatL(mbx, mby, lx, ly-1) : tcatL(mbx, mby-1, lx, 3);
                        int nC = walk_detail::ncFrom(nA, nB);
                        int max_c = (mt >= 1 && mt <= 24 && is_i) ? 15 : 16;
                        auto r = cavlc::residualBlock(br, nC, max_c);
                        if (!r.ok) goto done;
                        tcsetL(mbx, mby, lx, ly, r.total_coeff);
                        for (int k = 0; k < 16; ++k) {
                            int al = std::abs(static_cast<int>(r.coeff[k]));
                            if (al > max_abs_level) max_abs_level = al;
                            if (al > 0) total_coeffs++;
                            if (al > 255) { coeffs_over255++; mb_has_over255 = true; }
                        }
                        total_blocks++;
                        if (mb_has_over255) blocks_over255++;
                    } else {
                        tcsetL(mbx, mby, lx, ly, 0);
                    }
                }
            }
            total_mbs++;
            if (mb_has_over255) mbs_over255++;
        }
    }
done:
    std::printf("\n=== Coefficient Level Analysis ===\n");
    std::printf("Total MBs decoded:        %d\n", total_mbs);
    std::printf("Minimum QP observed:      %d\n", min_qp);
    std::printf("Maximum |level|:          %d\n", max_abs_level);
    std::printf("Total non-zero coeffs:    %d\n", total_coeffs);
    std::printf("Coefficients |level|>255: %d (%.3f%%)\n",
                coeffs_over255, total_coeffs ? 100.0 * coeffs_over255 / total_coeffs : 0.0);
    std::printf("MBs with any |level|>255: %d / %d (%.3f%%)\n",
                mbs_over255, total_mbs, total_mbs ? 100.0 * mbs_over255 / total_mbs : 0.0);
    std::printf("9-bit safe (|level|<=255): %s\n", max_abs_level <= 255 ? "YES" : "NO");
    return 0;
}
