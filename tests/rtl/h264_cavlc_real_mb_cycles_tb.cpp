// Real-bitstream CAVLC residual cy/MB — product h264_cavlc_residual_block.
// Stream: tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264
// Walk order matches host walkISliceResiduals (I-slice residual only).
//
// LABEL: CAVLC residual parse ONLY.
//   includes: luma I4/I16-DC/I16-AC + chroma DC/AC residual blocks
//   excludes: slice/mb headers, dequant/IDCT/recon (see +34), deblock, MC

#include "Vh264_cavlc_residual_tb_top.h"
#include "libmisterplex/h264_slice_walk.hpp"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr const char* kDefaultStream =
    "tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264";

struct StageSum {
    uint64_t token = 0, sign = 0, level = 0, tz = 0, run = 0, place = 0, other = 0, total = 0;
    void add_probe(Vh264_cavlc_residual_tb_top* d) {
#ifdef CAVLC_CYCLE_PROBE
        token += d->cy_token;
        sign += d->cy_sign;
        level += d->cy_level;
        tz += d->cy_total_zeros;
        run += d->cy_run_before;
        place += d->cy_place;
        other += d->cy_other;
        total += d->cy_total;
#else
        (void)d;
#endif
    }
};

struct MbCy {
    int cy_luma = 0;
    int cy_chroma = 0;
    int cy_all = 0;
    int n_luma = 0;
    int n_chroma = 0;
};

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

static void tick(Vh264_cavlc_residual_tb_top* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

static int tableFromNc(int nC) {
    if (nC < 0) return 4;
    if (nC < 2) return 0;
    if (nC < 4) return 1;
    if (nC < 8) return 2;
    return 3;
}

static int sx16(int v) {
    v &= 0xffff;
    return (v & 0x8000) ? (v - 0x10000) : v;
}

static int runRtl(Vh264_cavlc_residual_tb_top* dut, const std::vector<uint8_t>& rbsp,
                  int bit_start, int bit_end, int nC, int max_coeff,
                  const misterplex::cavlc::ResidualResult& host, StageSum& st,
                  int& mismatches) {
    const int nbits = bit_end - bit_start;
    if (nbits <= 0) return 0;
    if (nbits > 512) {
        std::cerr << "FAIL window bits=" << nbits << "\n";
        return -1;
    }
    // Pack residual bits into a fresh buffer at bit 0 (same contract as
    // h264_cavlc_residual_tb.cpp). Avoids any bit_offset_start/window edge cases.
    std::array<uint8_t, 64> packed{};
    for (int i = 0; i < nbits; ++i) {
        const size_t abs = static_cast<size_t>(bit_start + i);
        const size_t by = abs / 8;
        const int bi = 7 - static_cast<int>(abs % 8);
        const int bit = (by < rbsp.size()) ? ((rbsp[by] >> bi) & 1) : 0;
        if (bit)
            packed[static_cast<size_t>(i / 8)] |=
                static_cast<uint8_t>(1u << (7 - (i & 7)));
    }
    for (int i = 0; i < 64; i++)
        dut->rbsp[i] = packed[static_cast<size_t>(i)];
    dut->coeff_token_table = tableFromNc(nC);
    dut->max_coeff = max_coeff;
    dut->bit_offset_start = 0;
    dut->bit_len = nbits;

    dut->start = 1;
    int cy = 0;
    tick(dut);
    cy++;
    dut->start = 0;
    int guard = 4096;
    while (!dut->done && guard-- > 0) {
        tick(dut);
        cy++;
    }
    if (guard <= 0 || !dut->done || !dut->ok) {
        std::cerr << "FAIL RTL done/ok bits=[" << bit_start << "," << bit_end << ") nbits="
                  << nbits << " nC=" << nC << " max=" << max_coeff
                  << " tab=" << tableFromNc(nC)
                  << " ok=" << int(dut->ok) << " done=" << int(dut->done)
                  << " host_tc=" << host.total_coeff << " host_t1=" << host.trailing_ones
                  << " rtl_tc=" << int(dut->total_coeff) << " rtl_t1=" << int(dut->trailing_ones)
                  << " rtl_tz=" << int(dut->total_zeros)
                  << " bit_end_rtl=" << int(dut->bit_offset_end) << "\n";
        std::cerr << "  host_coeff:";
        for (int k = 0; k < 16; k++) std::cerr << ' ' << host.coeff[k];
        std::cerr << "\n  rtl_coeff:";
        for (int k = 0; k < 16; k++) std::cerr << ' ' << sx16(dut->coeff[k]);
        std::cerr << "\n  rtl_level:";
        for (int k = 0; k < 16; k++) std::cerr << ' ' << sx16(dut->level_dbg[k]);
        std::cerr << "\n  rtl_run:";
        for (int k = 0; k < 16; k++) std::cerr << ' ' << int(dut->run_dbg[k]);
        std::cerr << "\n  packed:";
        for (int k = 0; k < (nbits + 7) / 8; k++)
            std::cerr << ' ' << std::hex << std::setw(2) << std::setfill('0')
                      << int(packed[static_cast<size_t>(k)]) << std::dec;
        std::cerr << "\n";
        return -1;
    }
    if (host.ok) {
        if (int(dut->total_coeff) != host.total_coeff ||
            int(dut->trailing_ones) != host.trailing_ones)
            mismatches++;
        for (int k = 0; k < 16; k++) {
            if (sx16(dut->coeff[k]) != host.coeff[k]) {
                mismatches++;
                break;
            }
        }
    }
    st.add_probe(dut);
    tick(dut);
    return cy;
}

static double pctile(std::vector<int> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double idx = p * (v.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, v.size() - 1);
    double f = idx - lo;
    return v[lo] * (1.0 - f) + v[hi] * f;
}

static void printStats(const char* name, std::vector<int> s) {
    if (s.empty()) {
        std::cout << "  " << name << ": n=0\n";
        return;
    }
    std::sort(s.begin(), s.end());
    long sum = std::accumulate(s.begin(), s.end(), 0L);
    // Parent requires p50/p95/p99/max (not mean-only). Mean kept as secondary.
    std::cout << "  " << name << ": n=" << s.size()
              << " min=" << s.front()
              << " p50=" << s[s.size() / 2]
              << " p95=" << std::fixed << std::setprecision(1) << pctile(s, 0.95)
              << " p99=" << pctile(s, 0.99)
              << " max=" << s.back()
              << " mean=" << std::setprecision(2)
              << (double)sum / (double)s.size() << "\n";
}

static void printStages(const char* name, const StageSum& s) {
    uint64_t t = s.total ? s.total : 1;
    auto p = [&](const char* lab, uint64_t v) {
        std::cout << "    " << lab << "=" << v
                  << " (" << std::fixed << std::setprecision(1)
                  << (100.0 * (double)v / (double)t) << "%)\n";
    };
    std::cout << "  STAGE_BREAKDOWN " << name << " probe_total=" << s.total << "\n";
    p("coeff_token", s.token);
    p("trailing_ones_signs", s.sign);
    p("level_prefix_suffix", s.level);
    p("total_zeros", s.tz);
    p("run_before", s.run);
    p("coeff_place", s.place);
    p("other_overhead", s.other);
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    std::string path = kDefaultStream;
    for (int a = 1; a < argc; a++) {
        if (std::string(argv[a]) == "--stream" && a + 1 < argc)
            path = argv[++a];
    }

    auto annexb = readFile(path);
    if (annexb.empty()) {
        std::cerr << "FAIL read " << path << "\n";
        return 2;
    }

    using namespace misterplex;
    auto chain = parseAnnexBChain(annexb.data(), annexb.size());
    if (!chain.sps.valid || !chain.pps.valid || !chain.slice.valid) {
        std::cerr << "FAIL annex-B chain\n";
        return 2;
    }

    size_t ii = 0;
    const uint8_t* pay = nullptr;
    size_t plen = 0;
    uint8_t ntype = 0;
    while (ii + 3 < annexb.size()) {
        size_t sc = 0;
        if (ii + 3 < annexb.size() && annexb[ii] == 0 && annexb[ii + 1] == 0 &&
            annexb[ii + 2] == 0 && annexb[ii + 3] == 1)
            sc = 4;
        else if (annexb[ii] == 0 && annexb[ii + 1] == 0 && annexb[ii + 2] == 1)
            sc = 3;
        else {
            ++ii;
            continue;
        }
        size_t j = ii + sc;
        while (j + 3 < annexb.size()) {
            if (annexb[j] == 0 && annexb[j + 1] == 0 &&
                (annexb[j + 2] == 1 ||
                 (j + 3 < annexb.size() && annexb[j + 2] == 0 && annexb[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 3 >= annexb.size()) j = annexb.size();
        uint8_t t = annexb[ii + sc] & 0x1f;
        if (t == 5 || t == 1) {
            pay = annexb.data() + ii + sc + 1;
            plen = j - (ii + sc + 1);
            ntype = t;
            break;
        }
        ii = j;
    }
    if (!pay) {
        std::cerr << "FAIL no VCL\n";
        return 2;
    }

    auto rbsp = detail::removeEpb(pay, plen);
    detail::BitReader br(rbsp.data(), rbsp.size());
    br.ue();
    br.ue();
    br.ue();
    br.u(chain.log2_max_frame_num);
    if (ntype == 5) {
        br.ue();
        br.u(1);
        br.u(1);
    }
    br.se();
    if (chain.pps.deblock_ctrl) {
        uint32_t d = br.ue();
        if (d != 1) {
            br.se();
            br.se();
        }
    }

    const int mbW = (chain.sps.width + 15) / 16;
    const int mbH = (chain.sps.height + 15) / 16;
    const int mbTotal = mbW * mbH;

    std::vector<int> tcLuma(static_cast<size_t>(mbTotal * 16), -1);
    std::vector<int> tcChr0(static_cast<size_t>(mbTotal * 4), -1);
    std::vector<int> tcChr1(static_cast<size_t>(mbTotal * 4), -1);
    auto tcatL = [&](int mbx, int mby, int lx, int ly) -> int* {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH) return nullptr;
        int& v = tcLuma[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)];
        return (v < 0) ? nullptr : &v;
    };
    auto tcsetL = [&](int mbx, int mby, int lx, int ly, int v) {
        tcLuma[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)] = v;
    };
    auto tcatC = [&](int plane, int mbx, int mby, int lx, int ly) -> int* {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH) return nullptr;
        auto& vec = plane ? tcChr1 : tcChr0;
        int& v = vec[static_cast<size_t>(((mby * mbW + mbx) * 4) + ly * 2 + lx)];
        return (v < 0) ? nullptr : &v;
    };
    auto tcsetC = [&](int plane, int mbx, int mby, int lx, int ly, int v) {
        auto& vec = plane ? tcChr1 : tcChr0;
        vec[static_cast<size_t>(((mby * mbW + mbx) * 4) + ly * 2 + lx)] = v;
    };

    auto* dut = new Vh264_cavlc_residual_tb_top;
    dut->reset = 1;
    dut->start = 0;
    tick(dut);
    tick(dut);
    dut->reset = 0;
    tick(dut);

    StageSum st_luma, st_chroma;
    std::vector<MbCy> mbs;
    mbs.reserve(static_cast<size_t>(mbTotal));
    int mismatches = 0;
    int rtl_errors = 0;

    auto feed = [&](int nC, int maxc, bool chroma, MbCy& mb) -> int {
        int b0 = static_cast<int>(br.bit);
        auto host = cavlc::residualBlock(br, nC, maxc);
        int b1 = static_cast<int>(br.bit);
        if (!host.ok) return -999;
        StageSum& st = chroma ? st_chroma : st_luma;
        int cy = runRtl(dut, rbsp, b0, b1, nC, maxc, host, st, mismatches);
        if (cy < 0) {
            rtl_errors++;
            return -999;
        }
        if (chroma) {
            mb.cy_chroma += cy;
            mb.n_chroma++;
        } else {
            mb.cy_luma += cy;
            mb.n_luma++;
        }
        mb.cy_all += cy;
        return host.total_coeff;
    };

    for (int mby = 0; mby < mbH; ++mby) {
        for (int mbx = 0; mbx < mbW; ++mbx) {
            MbCy mb{};
            if (!br.ok) {
                std::cerr << "FAIL br mb=" << (mby * mbW + mbx) << "\n";
                delete dut;
                return 1;
            }
            uint32_t mt = br.ue();
            if (mt > 25) {
                std::cerr << "FAIL mb_type=" << mt << "\n";
                delete dut;
                return 1;
            }
            if (mt == 25) {
                while (br.ok && (br.bit % 8) != 0) br.u(1);
                for (int k = 0; k < 384 && br.ok; ++k) br.u(8);
                for (int ly = 0; ly < 4; ++ly)
                    for (int lx = 0; lx < 4; ++lx) tcsetL(mbx, mby, lx, ly, 16);
                for (int p = 0; p < 2; ++p)
                    for (int b = 0; b < 4; ++b) {
                        int lx, ly;
                        walk_detail::chrXY(b, lx, ly);
                        tcsetC(p, mbx, mby, lx, ly, 16);
                    }
                mbs.push_back(mb);
                continue;
            }

            auto parseChr = [&](int cbp_c) -> bool {
                if (cbp_c) {
                    if (feed(-1, 4, true, mb) == -999) return false;
                    if (feed(-1, 4, true, mb) == -999) return false;
                }
                if (cbp_c == 2) {
                    for (int p = 0; p < 2; ++p) {
                        for (int b = 0; b < 4; ++b) {
                            int lx, ly;
                            walk_detail::chrXY(b, lx, ly);
                            int* nA = (lx > 0) ? tcatC(p, mbx, mby, lx - 1, ly)
                                               : tcatC(p, mbx - 1, mby, 1, ly);
                            int* nB = (ly > 0) ? tcatC(p, mbx, mby, lx, ly - 1)
                                               : tcatC(p, mbx, mby - 1, lx, 1);
                            int nC = walk_detail::ncFrom(nA, nB);
                            int tc = feed(nC, 15, true, mb);
                            if (tc == -999) return false;
                            tcsetC(p, mbx, mby, lx, ly, tc);
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
                return true;
            };

            if (mt == 0) {
                for (int k = 0; k < 16; ++k)
                    if (br.u(1) == 0) br.u(3);
                br.ue();
                uint32_t code = br.ue();
                if (code >= 48) {
                    std::cerr << "FAIL me_cbp\n";
                    delete dut;
                    return 1;
                }
                int cbp = walk_detail::kMeIntra[code];
                int cbp_l = cbp & 15;
                int cbp_c = cbp >> 4;
                if (cbp != 0) br.se();
                for (int i8 = 0; i8 < 4; ++i8) {
                    if ((cbp_l >> i8) & 1) {
                        for (int i4 = 0; i4 < 4; ++i4) {
                            int lx, ly;
                            walk_detail::blkXY(i8, i4, lx, ly);
                            int* nA = (lx > 0) ? tcatL(mbx, mby, lx - 1, ly)
                                               : tcatL(mbx - 1, mby, 3, ly);
                            int* nB = (ly > 0) ? tcatL(mbx, mby, lx, ly - 1)
                                               : tcatL(mbx, mby - 1, lx, 3);
                            int nC = walk_detail::ncFrom(nA, nB);
                            int tc = feed(nC, 16, false, mb);
                            if (tc == -999) {
                                std::cerr << "FAIL I4_res mb=" << (mby * mbW + mbx) << "\n";
                                delete dut;
                                return 1;
                            }
                            tcsetL(mbx, mby, lx, ly, tc);
                        }
                    } else {
                        for (int i4 = 0; i4 < 4; ++i4) {
                            int lx, ly;
                            walk_detail::blkXY(i8, i4, lx, ly);
                            tcsetL(mbx, mby, lx, ly, 0);
                        }
                    }
                }
                if (!parseChr(cbp_c)) {
                    std::cerr << "FAIL chroma I4 mb=" << (mby * mbW + mbx) << "\n";
                    delete dut;
                    return 1;
                }
            } else {
                int x = static_cast<int>(mt) - 1;
                int cbp_c = (x / 4) % 3;
                int cbp_l = (x / 12) ? 15 : 0;
                br.ue();
                br.se();
                int* nA = tcatL(mbx - 1, mby, 3, 0);
                int* nB = tcatL(mbx, mby - 1, 0, 3);
                int nC = walk_detail::ncFrom(nA, nB);
                int tc_dc = feed(nC, 16, false, mb);
                if (tc_dc == -999) {
                    std::cerr << "FAIL I16_dc mb=" << (mby * mbW + mbx) << "\n";
                    delete dut;
                    return 1;
                }
                if (cbp_l) {
                    for (int i8 = 0; i8 < 4; ++i8)
                        for (int i4 = 0; i4 < 4; ++i4) {
                            int lx, ly;
                            walk_detail::blkXY(i8, i4, lx, ly);
                            int* a = (lx > 0) ? tcatL(mbx, mby, lx - 1, ly)
                                              : tcatL(mbx - 1, mby, 3, ly);
                            int* b = (ly > 0) ? tcatL(mbx, mby, lx, ly - 1)
                                              : tcatL(mbx, mby - 1, lx, 3);
                            int tc = feed(walk_detail::ncFrom(a, b), 15, false, mb);
                            if (tc == -999) {
                                std::cerr << "FAIL I16_ac mb=" << (mby * mbW + mbx) << "\n";
                                delete dut;
                                return 1;
                            }
                            tcsetL(mbx, mby, lx, ly, tc);
                        }
                } else {
                    for (int ly = 0; ly < 4; ++ly)
                        for (int lx = 0; lx < 4; ++lx) tcsetL(mbx, mby, lx, ly, 0);
                }
                if (!parseChr(cbp_c)) {
                    std::cerr << "FAIL chroma I16 mb=" << (mby * mbW + mbx) << "\n";
                    delete dut;
                    return 1;
                }
            }
            mbs.push_back(mb);
        }
    }

    delete dut;

    if ((int)mbs.size() != mbTotal) {
        std::cerr << "FAIL mb count " << mbs.size() << " != " << mbTotal << "\n";
        return 1;
    }
    if (rtl_errors || mismatches) {
        std::cerr << "FAIL rtl_errors=" << rtl_errors << " coeff_mismatches=" << mismatches << "\n";
        return 1;
    }

    std::vector<int> all_cy, luma_cy, chroma_cy;
    for (auto& m : mbs) {
        all_cy.push_back(m.cy_all);
        luma_cy.push_back(m.cy_luma);
        chroma_cy.push_back(m.cy_chroma);
    }

    auto med = [](std::vector<int> v) -> int {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    const int recon_lb = 34;
    const int cavlc_luma_med = med(luma_cy);
    const int cavlc_all_med = med(all_cy);
    const double cavlc_luma_p95 = pctile(luma_cy, 0.95);
    const double cavlc_all_p95 = pctile(all_cy, 0.95);

    std::cout << "CAVLC_CY_PER_MB  verification_target=RTL_CAVLC_RESIDUAL\n"
              << "  stream=" << path << " bytes=" << annexb.size() << "\n"
              << "  geometry=" << chain.sps.width << "x" << chain.sps.height
              << " mb=" << mbW << "x" << mbH << " n=" << mbTotal << "\n"
              << "  LABEL=CAVLC_RESIDUAL_ONLY"
              << " missing=slice_mb_headers,dequant_idct_recon,deblock,MC\n"
              << "  coeff_match=PASS mismatches=0 rtl_errors=0\n";

    printStats("cy_MB_luma_residual", luma_cy);
    printStats("cy_MB_chroma_residual", chroma_cy);
    printStats("cy_MB_luma+chroma_residual", all_cy);

#ifndef CAVLC_CYCLE_PROBE
    std::cout << "  STAGE_BREAKDOWN: build with -DCAVLC_CYCLE_PROBE for stage splits\n";
#else
    printStages("luma", st_luma);
    printStages("chroma", st_chroma);
    StageSum st_all = st_luma;
    st_all.token += st_chroma.token;
    st_all.sign += st_chroma.sign;
    st_all.level += st_chroma.level;
    st_all.tz += st_chroma.tz;
    st_all.run += st_chroma.run;
    st_all.place += st_chroma.place;
    st_all.other += st_chroma.other;
    st_all.total += st_chroma.total;
    printStages("luma+chroma", st_all);
#endif

    const double cavlc_all_p50 = pctile(all_cy, 0.50);
    const double cavlc_all_p99 = pctile(all_cy, 0.99);
    const int cavlc_all_max = all_cy.empty() ? 0 : *std::max_element(all_cy.begin(), all_cy.end());
    const double sum_luma_med = cavlc_luma_med + recon_lb;
    const double sum_luma_p95 = cavlc_luma_p95 + recon_lb;
    const double sum_all_med = cavlc_all_med + recon_lb;
    const double sum_all_p50 = cavlc_all_p50 + recon_lb;
    const double sum_all_p95 = cavlc_all_p95 + recon_lb;
    const double sum_all_p99 = cavlc_all_p99 + recon_lb;
    const double sum_all_max = cavlc_all_max + recon_lb;
    // Parent-verified budgets @20 MHz:
    const double bud_sd = 712.3;   // 624x480@24
    const double bud_hd30 = 185.2; // 1280x720@30
    const double bud_hd24 = 231.5; // 1280x720@24 — binding product target

    auto verdict = [&](double cost, double budget) -> const char* {
        return (cost < budget) ? "UNDER_BUDGET" : "OVER_BUDGET";
    };

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "ARCH_COMBINE recon_lower_bound_cy=" << recon_lb << "\n"
              << "  CAVLC_all_p50+recon=" << sum_all_p50
              << "  CAVLC_all_p95+recon=" << sum_all_p95
              << "  CAVLC_all_p99+recon=" << sum_all_p99
              << "  CAVLC_all_max+recon=" << sum_all_max << "\n"
              << "  CAVLC_luma_med+recon=" << sum_luma_med
              << "  CAVLC_luma_p95+recon=" << sum_luma_p95 << "\n"
              << "  vs_624x480@24_budget_" << bud_sd << "_@20MHz:\n"
              << "    all_p50 " << verdict(sum_all_p50, bud_sd)
              << " all_p95 " << verdict(sum_all_p95, bud_sd)
              << " all_p99 " << verdict(sum_all_p99, bud_sd) << "\n"
              << "  vs_1280x720@24_budget_" << bud_hd24 << "_@20MHz:\n"
              << "    all_p50 " << verdict(sum_all_p50, bud_hd24)
              << " all_p95 " << verdict(sum_all_p95, bud_hd24)
              << " all_p99 " << verdict(sum_all_p99, bud_hd24) << "\n"
              << "  vs_1280x720@30_budget_" << bud_hd30 << "_@20MHz:\n"
              << "    all_p50 " << verdict(sum_all_p50, bud_hd30)
              << " all_p95 " << verdict(sum_all_p95, bud_hd30)
              << " all_p99 " << verdict(sum_all_p99, bud_hd30) << "\n";

    bool hd24_p95_ok = sum_all_p95 < bud_hd24;
    bool hd30_p95_ok = sum_all_p95 < bud_hd30;
    bool sd_p95_ok = sum_all_p95 < bud_sd;
    std::cout << "HEADLINE_720P24_AT_20MHz: ";
    if (hd24_p95_ok)
        std::cout << "PASS (CAVLC+recon p95=" << sum_all_p95 << " < " << bud_hd24 << ")\n";
    else
        std::cout << "FAIL (CAVLC+recon p95=" << sum_all_p95 << " >= " << bud_hd24
                  << " — TIME wall; need CAVLC speedup and/or faster decode domain)\n";
    std::cout << "HEADLINE_720P_AT_20MHz: ";
    if (hd30_p95_ok)
        std::cout << "FIT (CAVLC+recon p95 UNDER " << bud_hd30 << ")\n";
    else if (hd24_p95_ok)
        std::cout << "FIT_24_NOT_30 (p95 under 231.5 but over 185.2)\n";
    else if (sd_p95_ok)
        std::cout << "NO_FIT_720P_YES_SD (p95 under 624x480@24 only)\n";
    else
        std::cout << "NO_FIT_EVEN_SD (p95 over 712)\n";

    std::cout << "CAVLC_REAL_MB_CYCLES PASS n_mb=" << mbTotal << "\n";
    return 0;
}
