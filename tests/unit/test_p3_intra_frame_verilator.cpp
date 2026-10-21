#include "Vp3_intra_frame_tb.h"
#include "libmisterplex/h264_recon.hpp"
#include "libmisterplex/h264_slice_walk.hpp"
#include "verilated.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kVectorPath = "tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264";
constexpr const char* kMaePath = "tests/fixtures/p3_host_recon/frame_mae_v1.csv";

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

uint32_t signed18(int v) {
    return static_cast<uint32_t>(v) & ((1u << 29) - 1u);
}

int blockOrder(int lx, int ly) {
    int i8 = (ly / 2) * 2 + (lx / 2);
    int i4 = (ly % 2) * 2 + (lx % 2);
    return i8 * 4 + i4;
}

bool lumaReady(int px, int py, int mbx, int mby, int lx, int ly, int width, int height, int mbW) {
    if (px < 0 || py < 0 || px >= width || py >= height)
        return false;
    int pmbx = px / 16;
    int pmby = py / 16;
    int cur = mby * mbW + mbx;
    int prev = pmby * mbW + pmbx;
    if (prev < cur)
        return true;
    if (prev > cur)
        return false;
    int plx = (px % 16) / 4;
    int ply = (py % 16) / 4;
    return blockOrder(plx, ply) < blockOrder(lx, ly);
}

uint8_t yAt(const std::vector<uint8_t>& y, int width, int height, int x, int yy) {
    if (x < 0 || yy < 0 || x >= width || yy >= height)
        return 128;
    return y[static_cast<size_t>(yy * width + x)];
}

int clip8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

struct SliceModes {
    int width = 0;
    int height = 0;
    std::vector<int> mbType;
    std::vector<int> chromaMode;
};

SliceModes parseSliceModes(const std::vector<uint8_t>& annexb) {
    using namespace misterplex;
    SliceModes out;
    auto chain = parseAnnexBChain(annexb.data(), annexb.size());
    if (!chain.sps.valid || !chain.pps.valid || !chain.slice.valid)
        return out;
    out.width = chain.sps.width;
    out.height = chain.sps.height;
    const int mbW = (out.width + 15) / 16;
    const int mbH = (out.height + 15) / 16;
    out.mbType.reserve(static_cast<size_t>(mbW * mbH));
    out.chromaMode.reserve(static_cast<size_t>(mbW * mbH));

    size_t i = 0;
    const uint8_t* pay = nullptr;
    size_t plen = 0;
    uint8_t ntype = 0;
    while (i + 3 < annexb.size()) {
        size_t sc = 0;
        if (i + 3 < annexb.size() && annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1)
            sc = 4;
        else if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1)
            sc = 3;
        else {
            ++i;
            continue;
        }
        size_t j = i + sc;
        while (j + 3 < annexb.size()) {
            if (annexb[j] == 0 && annexb[j + 1] == 0 &&
                (annexb[j + 2] == 1 || (j + 3 < annexb.size() && annexb[j + 2] == 0 && annexb[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 3 >= annexb.size())
            j = annexb.size();
        uint8_t t = annexb[i + sc] & 0x1f;
        if (t == 5 || t == 1) {
            pay = annexb.data() + i + sc + 1;
            plen = j - (i + sc + 1);
            ntype = t;
            break;
        }
        i = j;
    }
    if (!pay)
        return {};

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

    std::vector<int> tcL(static_cast<size_t>(mbW * mbH * 16), -1);
    std::vector<int> tcC[2] = {std::vector<int>(static_cast<size_t>(mbW * mbH * 4), -1),
                               std::vector<int>(static_cast<size_t>(mbW * mbH * 4), -1)};
    auto tcatL = [&](int mbx, int mby, int lx, int ly) -> int* {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH || lx < 0 || ly < 0 || lx > 3 || ly > 3)
            return nullptr;
        int& v = tcL[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)];
        return v < 0 ? nullptr : &v;
    };
    auto tcsetL = [&](int mbx, int mby, int lx, int ly, int v) {
        tcL[static_cast<size_t>(((mby * mbW + mbx) * 16) + ly * 4 + lx)] = v;
    };
    auto tcatC = [&](int p, int mbx, int mby, int lx, int ly) -> int* {
        if (mbx < 0 || mby < 0 || mbx >= mbW || mby >= mbH || lx < 0 || ly < 0 || lx > 1 || ly > 1)
            return nullptr;
        int& v = tcC[p][static_cast<size_t>(((mby * mbW + mbx) * 4) + ly * 2 + lx)];
        return v < 0 ? nullptr : &v;
    };
    auto tcsetC = [&](int p, int mbx, int mby, int lx, int ly, int v) {
        tcC[p][static_cast<size_t>(((mby * mbW + mbx) * 4) + ly * 2 + lx)] = v;
    };
    auto parseChr = [&](int mbx, int mby, int cbp_c) -> const char* {
        if (cbp_c) {
            if (!cavlc::residualBlock(br, -1, 4).ok || !cavlc::residualBlock(br, -1, 4).ok)
                return "chrDC";
        }
        if (cbp_c == 2) {
            for (int p = 0; p < 2; ++p) {
                for (int b = 0; b < 4; ++b) {
                    int lx = 0, ly = 0;
                    walk_detail::chrXY(b, lx, ly);
                    int* a = (lx > 0) ? tcatC(p, mbx, mby, lx - 1, ly) : tcatC(p, mbx - 1, mby, 1, ly);
                    int* bb = (ly > 0) ? tcatC(p, mbx, mby, lx, ly - 1) : tcatC(p, mbx, mby - 1, lx, 1);
                    auto r = cavlc::residualBlock(br, walk_detail::ncFrom(a, bb), 15);
                    if (!r.ok)
                        return "chrAC";
                    tcsetC(p, mbx, mby, lx, ly, r.total_coeff);
                }
            }
        } else {
            for (int p = 0; p < 2; ++p)
                for (int b = 0; b < 4; ++b) {
                    int lx = 0, ly = 0;
                    walk_detail::chrXY(b, lx, ly);
                    tcsetC(p, mbx, mby, lx, ly, 0);
                }
        }
        return nullptr;
    };

    for (int mby = 0; mby < mbH; ++mby) {
        for (int mbx = 0; mbx < mbW; ++mbx) {
            uint32_t mt = br.ue();
            if (mt > 25 || !br.ok)
                return {};
            out.mbType.push_back(static_cast<int>(mt));
            if (mt == 25) {
                out.chromaMode.push_back(-1);
                while (br.ok && (br.bit % 8) != 0)
                    br.u(1);
                for (int k = 0; k < 384 && br.ok; ++k)
                    br.u(8);
                for (int ly = 0; ly < 4; ++ly)
                    for (int lx = 0; lx < 4; ++lx)
                        tcsetL(mbx, mby, lx, ly, 16);
                for (int p = 0; p < 2; ++p)
                    for (int b = 0; b < 4; ++b) {
                        int lx = 0, ly = 0;
                        walk_detail::chrXY(b, lx, ly);
                        tcsetC(p, mbx, mby, lx, ly, 16);
                    }
                continue;
            }
            int cbp_l = 0, cbp_c = 0;
            if (mt == 0) {
                for (int k = 0; k < 16; ++k)
                    if (br.u(1) == 0)
                        br.u(3);
                int cm = static_cast<int>(br.ue());
                out.chromaMode.push_back(cm);
                uint32_t code = br.ue();
                if (code >= 48)
                    return {};
                int cbp = walk_detail::kMeIntra[code];
                cbp_l = cbp & 15;
                cbp_c = cbp >> 4;
                if (cbp)
                    br.se();
                for (int i8 = 0; i8 < 4; ++i8) {
                    if ((cbp_l >> i8) & 1) {
                        for (int i4 = 0; i4 < 4; ++i4) {
                            int lx = 0, ly = 0;
                            walk_detail::blkXY(i8, i4, lx, ly);
                            int* a = (lx > 0) ? tcatL(mbx, mby, lx - 1, ly) : tcatL(mbx - 1, mby, 3, ly);
                            int* b = (ly > 0) ? tcatL(mbx, mby, lx, ly - 1) : tcatL(mbx, mby - 1, lx, 3);
                            auto r = cavlc::residualBlock(br, walk_detail::ncFrom(a, b), 16);
                            if (!r.ok)
                                return {};
                            tcsetL(mbx, mby, lx, ly, r.total_coeff);
                        }
                    } else {
                        for (int i4 = 0; i4 < 4; ++i4) {
                            int lx = 0, ly = 0;
                            walk_detail::blkXY(i8, i4, lx, ly);
                            tcsetL(mbx, mby, lx, ly, 0);
                        }
                    }
                }
            } else {
                int x = static_cast<int>(mt) - 1;
                cbp_c = (x / 4) % 3;
                cbp_l = (x / 12) ? 15 : 0;
                int cm = static_cast<int>(br.ue());
                out.chromaMode.push_back(cm);
                br.se();
                int* a = tcatL(mbx - 1, mby, 3, 0);
                int* b = tcatL(mbx, mby - 1, 0, 3);
                auto r = cavlc::residualBlock(br, walk_detail::ncFrom(a, b), 16);
                if (!r.ok)
                    return {};
                if (cbp_l) {
                    for (int i8 = 0; i8 < 4; ++i8)
                        for (int i4 = 0; i4 < 4; ++i4) {
                            int lx = 0, ly = 0;
                            walk_detail::blkXY(i8, i4, lx, ly);
                            int* aa = (lx > 0) ? tcatL(mbx, mby, lx - 1, ly) : tcatL(mbx - 1, mby, 3, ly);
                            int* bb = (ly > 0) ? tcatL(mbx, mby, lx, ly - 1) : tcatL(mbx, mby - 1, lx, 3);
                            auto rr = cavlc::residualBlock(br, walk_detail::ncFrom(aa, bb), 15);
                            if (!rr.ok)
                                return {};
                            tcsetL(mbx, mby, lx, ly, rr.total_coeff);
                        }
                } else {
                    for (int ly = 0; ly < 4; ++ly)
                        for (int lx = 0; lx < 4; ++lx)
                            tcsetL(mbx, mby, lx, ly, 0);
                }
            }
            if (parseChr(mbx, mby, cbp_c))
                return {};
        }
    }
    return out;
}

bool checkMaeFixture(const misterplex::recon::ReconResult& rec) {
    std::ifstream in(kMaePath);
    if (!in) {
        std::cerr << "missing frame MAE fixture " << kMaePath << "\n";
        return false;
    }
    std::string line;
    int width = -1, height = -1, mbW = -1, mbH = -1, rows = 0;
    bool inRows = false;
    while (std::getline(in, line)) {
        if (line == "mb,mb_x,mb_y,sum_abs_y,pixels,mae_y,max_abs_y") {
            inRows = true;
            continue;
        }
        if (!inRows) {
            auto comma = line.find(',');
            if (comma == std::string::npos)
                continue;
            std::string k = line.substr(0, comma);
            int v = std::atoi(line.c_str() + comma + 1);
            if (k == "width") width = v;
            if (k == "height") height = v;
            if (k == "mb_width") mbW = v;
            if (k == "mb_height") mbH = v;
            continue;
        }
        if (line.empty())
            continue;
        std::stringstream ss(line);
        std::string tok;
        std::array<std::string, 7> cols{};
        for (size_t i = 0; i < cols.size() && std::getline(ss, tok, ','); ++i)
            cols[i] = tok;
        if (std::atoi(cols[3].c_str()) != 0 || std::atoi(cols[6].c_str()) != 0) {
            std::cerr << "frame_mae_v1.csv non-zero row: " << line << "\n";
            return false;
        }
        ++rows;
    }
    if (width != rec.width || height != rec.height || mbW != (rec.width + 15) / 16 ||
        mbH != (rec.height + 15) / 16 || rows != rec.mb_total) {
        std::cerr << "frame_mae_v1.csv geometry mismatch fixture=" << width << "x" << height
                  << " mb=" << mbW << "x" << mbH << " rows=" << rows << " rec="
                  << rec.width << "x" << rec.height << " mb_total=" << rec.mb_total << "\n";
        return false;
    }
    return true;
}

void setResidual(Vp3_intra_frame_tb& dut, const std::array<int16_t, 16>& residual, bool generic) {
    for (int i = 0; i < 16; ++i) {
        if (generic)
            dut.recon_residual[i] = signed18(residual[static_cast<size_t>(i)]);
        else
            dut.i4_residual[i] = signed18(residual[static_cast<size_t>(i)]);
    }
}

int expectedDc(const std::array<uint8_t, 8>& above, const std::array<uint8_t, 4>& left,
               bool hasA, bool hasL) {
    int sum = 0;
    if (hasA)
        for (int i = 0; i < 4; ++i)
            sum += above[static_cast<size_t>(i)];
    if (hasL)
        for (int i = 0; i < 4; ++i)
            sum += left[static_cast<size_t>(i)];
    if (hasA && hasL)
        return (sum + 4) >> 3;
    if (hasA || hasL)
        return (sum + 2) >> 2;
    return 128;
}

bool runFallbackProbe(Vp3_intra_frame_tb& dut) {
    bool ok = true;
    std::array<uint8_t, 8> above{{10, 20, 30, 40, 50, 60, 70, 80}};
    std::array<uint8_t, 4> left{{90, 100, 110, 120}};
    for (int i = 0; i < 8; ++i)
        dut.i4_above[i] = above[static_cast<size_t>(i)];
    for (int i = 0; i < 4; ++i)
        dut.i4_left[i] = left[static_cast<size_t>(i)];
    for (int i = 0; i < 16; ++i)
        dut.i4_residual[i] = 0;
    dut.i4_top_left = 77;

    struct Probe { int mode; bool ha; bool hl; } probes[] = {{0, false, true}, {1, true, false}, {4, true, false}, {5, false, true}, {6, true, false}, {7, false, true}, {8, true, false}};
    for (const auto& p : probes) {
        dut.i4_mode = static_cast<uint8_t>(p.mode);
        dut.i4_has_above = p.ha;
        dut.i4_has_left = p.hl;
        dut.eval();
        int exp = expectedDc(above, left, p.ha, p.hl);
        if (dut.i4_used_mode != 2 || dut.i4_pred[0] != exp) {
            std::cerr << "fallback probe failed mode=" << p.mode << " used=" << int(dut.i4_used_mode)
                      << " pred0=" << int(dut.i4_pred[0]) << " expected_dc=" << exp << "\n";
            ok = false;
        }
    }
    return ok;
}

void tickFrame(Vp3_intra_frame_tb& dut) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
}

// Run I16 prediction pipeline until valid
int runI16Pred(Vp3_intra_frame_tb& dut) {
    dut.i16_start = 1;
    tickFrame(dut);
    dut.i16_start = 0;
    int cycles = 1;
    while (!dut.i16_valid && cycles < 10) {
        tickFrame(dut);
        ++cycles;
    }
    return cycles;
}

// Run chroma prediction pipeline until valid
int runChromaPred(Vp3_intra_frame_tb& dut) {
    dut.chroma_start = 1;
    tickFrame(dut);
    dut.chroma_start = 0;
    int cycles = 1;
    while (!dut.chroma_valid && cycles < 10) {
        tickFrame(dut);
        ++cycles;
    }
    return cycles;
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const auto blob = readFile(kVectorPath);
    if (blob.empty()) {
        std::cerr << "missing vector " << kVectorPath << "\n";
        return 1;
    }
    auto rec = misterplex::recon::reconISlice(blob.data(), blob.size(), nullptr);
    if (rec.mb_decoded != rec.mb_total || rec.y.empty()) {
        std::cerr << "host recon failed mb=" << rec.mb_decoded << "/" << rec.mb_total
                  << " reason=" << (rec.fail_reason ? rec.fail_reason : "?") << "\n";
        return 1;
    }
    if (!checkMaeFixture(rec))
        return 1;
    SliceModes modes = parseSliceModes(blob);
    if (modes.mbType.size() != static_cast<size_t>(rec.mb_total) ||
        modes.chromaMode.size() != static_cast<size_t>(rec.mb_total)) {
        std::cerr << "mode parse failed: got mbType=" << modes.mbType.size()
                  << " chroma=" << modes.chromaMode.size() << " expected=" << rec.mb_total << "\n";
        return 1;
    }

    Vp3_intra_frame_tb dut;
    const int width = rec.width;
    const int height = rec.height;
    const int mbW = (width + 15) / 16;
    const int mbH = (height + 15) / 16;
    const int cw = (width + 1) / 2;
    const int ch = (height + 1) / 2;
    std::vector<uint8_t> rtlY(static_cast<size_t>(width * height), 128);

    std::array<int, 9> i4Pass{};
    std::array<int, 4> i16Pass{};
    std::array<int, 4> chromaMbPass{};
    std::array<int, 4> chromaPlaneEvalPass{};
    int i4Mbs = 0, i16Mbs = 0;
    int mbExact = 0;
    int noAboveBlocks = 0, noLeftBlocks = 0, topRightReplicatedBlocks = 0;
    int failures = 0;

    for (int mb = 0; mb < rec.mb_total; ++mb) {
        misterplex::recon::ReconTrace trace;
        trace.target_mb = mb;
        auto traced = misterplex::recon::reconISlice(blob.data(), blob.size(), &trace);
        if (traced.mb_decoded != traced.mb_total || !trace.mb.valid) {
            std::cerr << "trace recon failed mb=" << mb << " decoded=" << traced.mb_decoded
                      << "/" << traced.mb_total << "\n";
            return 1;
        }
        int mbx = mb % mbW;
        int mby = mb / mbW;
        int baseX = mbx * 16;
        int baseY = mby * 16;
        bool mbOk = true;

        if (trace.mb.mb_type == 0) {
            ++i4Mbs;
            for (const auto& blk : trace.mb.blocks) {
                int lx = blk.x / 4;
                int ly = blk.y / 4;
                int x0 = baseX + blk.x;
                int y0 = baseY + blk.y;
                bool ha = (y0 > 0) && lumaReady(x0, y0 - 1, mbx, mby, lx, ly, width, height, mbW);
                bool hl = (x0 > 0) && lumaReady(x0 - 1, y0, mbx, mby, lx, ly, width, height, mbW);
                if (!ha)
                    ++noAboveBlocks;
                if (!hl)
                    ++noLeftBlocks;
                bool trRep = false;
                dut.i4_mode = static_cast<uint8_t>(blk.pred_mode);
                dut.i4_has_above = ha;
                dut.i4_has_left = hl;
                for (int t = 0; t < 4; ++t)
                    dut.i4_above[t] = ha ? yAt(rtlY, width, height, x0 + t, y0 - 1) : 128;
                for (int t = 0; t < 4; ++t) {
                    int tx = x0 + 4 + t;
                    if (ha && lumaReady(tx, y0 - 1, mbx, mby, lx, ly, width, height, mbW))
                        dut.i4_above[4 + t] = yAt(rtlY, width, height, tx, y0 - 1);
                    else {
                        dut.i4_above[4 + t] = dut.i4_above[3];
                        trRep = true;
                    }
                }
                for (int t = 0; t < 4; ++t)
                    dut.i4_left[t] = hl ? yAt(rtlY, width, height, x0 - 1, y0 + t) : 128;
                if (ha && hl && lumaReady(x0 - 1, y0 - 1, mbx, mby, lx, ly, width, height, mbW))
                    dut.i4_top_left = yAt(rtlY, width, height, x0 - 1, y0 - 1);
                else if (ha)
                    dut.i4_top_left = dut.i4_above[0];
                else if (hl)
                    dut.i4_top_left = dut.i4_left[0];
                else
                    dut.i4_top_left = 128;
                if (trRep)
                    ++topRightReplicatedBlocks;
                setResidual(dut, blk.idct, false);
                dut.eval();
                if (dut.i4_used_mode != blk.pred_mode) {
                    std::cerr << "mb " << mb << " block " << blk.block << " used_mode got "
                              << int(dut.i4_used_mode) << " expected " << blk.pred_mode << "\n";
                    ++failures;
                    mbOk = false;
                }
                for (int i = 0; i < 16; ++i) {
                    if (dut.i4_pred[i] != blk.pred[static_cast<size_t>(i)] ||
                        dut.i4_recon[i] != blk.recon[static_cast<size_t>(i)]) {
                        std::cerr << "mb " << mb << " block " << blk.block << " sample " << i
                                  << " pred got/exp " << int(dut.i4_pred[i]) << "/" << int(blk.pred[static_cast<size_t>(i)])
                                  << " recon got/exp " << int(dut.i4_recon[i]) << "/" << int(blk.recon[static_cast<size_t>(i)]) << "\n";
                        ++failures;
                        mbOk = false;
                        break;
                    }
                }
                if (blk.pred_mode >= 0 && blk.pred_mode < 9)
                    ++i4Pass[static_cast<size_t>(blk.pred_mode)];
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        if (x0 + xx < width && y0 + yy < height)
                            rtlY[static_cast<size_t>((y0 + yy) * width + x0 + xx)] = dut.i4_recon[yy * 4 + xx];
            }
        } else {
            ++i16Mbs;
            int predMode = trace.mb.pred_mode;
            dut.i16_mode = static_cast<uint8_t>(predMode);
            dut.i16_has_above = mby > 0;
            dut.i16_has_left = mbx > 0;
            for (int t = 0; t < 16; ++t) {
                dut.i16_above[t] = (mby > 0) ? yAt(rtlY, width, height, baseX + t, baseY - 1) : 128;
                dut.i16_left[t] = (mbx > 0) ? yAt(rtlY, width, height, baseX - 1, baseY + t) : 128;
            }
            dut.i16_top_left = (mby > 0 && mbx > 0) ? yAt(rtlY, width, height, baseX - 1, baseY - 1) : 128;
            runI16Pred(dut);
            if (dut.i16_unsupported) {
                std::cerr << "unexpected I16 unsupported at mb " << mb << " mode " << predMode << "\n";
                ++failures;
                mbOk = false;
            }
            for (int i = 0; i < 256; ++i) {
                if (dut.i16_pred[i] != trace.mb.pred[static_cast<size_t>(i)]) {
                    std::cerr << "mb " << mb << " I16 pred[" << i << "] got " << int(dut.i16_pred[i])
                              << " expected " << int(trace.mb.pred[static_cast<size_t>(i)]) << "\n";
                    ++failures;
                    mbOk = false;
                    break;
                }
            }
            if (predMode >= 0 && predMode < 4)
                ++i16Pass[static_cast<size_t>(predMode)];
            for (const auto& blk : trace.mb.blocks) {
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        dut.recon_pred[yy * 4 + xx] = dut.i16_pred[(blk.y + yy) * 16 + blk.x + xx];
                setResidual(dut, blk.idct, true);
                dut.eval();
                int x0 = baseX + blk.x;
                int y0 = baseY + blk.y;
                for (int i = 0; i < 16; ++i) {
                    if (dut.recon_out[i] != blk.recon[static_cast<size_t>(i)]) {
                        std::cerr << "mb " << mb << " I16 block " << blk.block << " recon[" << i << "] got "
                                  << int(dut.recon_out[i]) << " expected " << int(blk.recon[static_cast<size_t>(i)]) << "\n";
                        ++failures;
                        mbOk = false;
                        break;
                    }
                }
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        if (x0 + xx < width && y0 + yy < height)
                            rtlY[static_cast<size_t>((y0 + yy) * width + x0 + xx)] = dut.recon_out[yy * 4 + xx];
            }
        }

        int cm = modes.chromaMode[static_cast<size_t>(mb)];
        if (cm >= 0 && cm < 4) {
            bool chromaOk = true;
            for (int plane = 0; plane < 2; ++plane) {
                const std::vector<uint8_t>& src = (plane == 0) ? rec.u : rec.v;
                uint8_t above[8], left[8], tl = 128, exp[64];
                int cx = mbx * 8;
                int cy = mby * 8;
                bool ha = mby > 0;
                bool hl = mbx > 0;
                for (int t = 0; t < 8; ++t) {
                    above[t] = ha ? src[static_cast<size_t>((cy - 1) * cw + cx + t)] : 128;
                    left[t] = hl ? src[static_cast<size_t>((cy + t) * cw + cx - 1)] : 128;
                    dut.chroma_above[t] = above[t];
                    dut.chroma_left[t] = left[t];
                }
                if (ha && hl)
                    tl = src[static_cast<size_t>((cy - 1) * cw + cx - 1)];
                dut.chroma_mode = static_cast<uint8_t>(cm);
                dut.chroma_has_above = ha;
                dut.chroma_has_left = hl;
                dut.chroma_top_left = tl;
                misterplex::recon::detail_r::predChroma8(cm, exp, 8, above, left, tl, ha, hl);
                runChromaPred(dut);
                for (int i = 0; i < 64; ++i) {
                    if (dut.chroma_pred[i] != exp[i]) {
                        std::cerr << "mb " << mb << " chroma plane " << plane << " mode " << cm
                                  << " pred[" << i << "] got " << int(dut.chroma_pred[i])
                                  << " expected " << int(exp[i]) << "\n";
                        ++failures;
                        chromaOk = false;
                        mbOk = false;
                        break;
                    }
                }
                if (chromaOk)
                    ++chromaPlaneEvalPass[static_cast<size_t>(cm)];
            }
            if (chromaOk)
                ++chromaMbPass[static_cast<size_t>(cm)];
        }

        for (int yy = 0; yy < 16; ++yy) {
            for (int xx = 0; xx < 16; ++xx) {
                int x = baseX + xx;
                int y = baseY + yy;
                if (x >= width || y >= height)
                    continue;
                if (rtlY[static_cast<size_t>(y * width + x)] != rec.y[static_cast<size_t>(y * width + x)])
                    mbOk = false;
            }
        }
        if (mbOk)
            ++mbExact;
    }

    if (!runFallbackProbe(dut))
        ++failures;

    const std::array<int, 9> expectedI4{{656, 298, 263, 19, 8, 14, 37, 22, 11}};
    const std::array<int, 4> expectedI16{{195, 13, 9, 0}};
    const std::array<int, 4> expectedChroma{{165, 33, 100, 2}};
    if (i4Pass != expectedI4) {
        std::cerr << "I4 mode coverage mismatch\n";
        ++failures;
    }
    if (i16Pass != expectedI16) {
        std::cerr << "I16 mode coverage mismatch\n";
        ++failures;
    }
    if (chromaMbPass != expectedChroma) {
        std::cerr << "chroma mode coverage mismatch\n";
        ++failures;
    }
    if (i4Pass[4] == 0 || i4Pass[8] == 0 || chromaMbPass[3] == 0) {
        std::cerr << "rare mode coverage missing: DDR=" << i4Pass[4] << " HU=" << i4Pass[8]
                  << " chromaPlane=" << chromaMbPass[3] << "\n";
        ++failures;
    }

    if (failures) {
        std::cerr << "P3 intra frame-wide Verilator exact check FAILED: failures=" << failures
                  << " mb_exact=" << mbExact << "/" << rec.mb_total << "\n";
        return 1;
    }

    std::cout << "P3 intra frame-wide Verilator exact check PASS: mb_exact=" << mbExact << "/"
              << rec.mb_total << " frame=" << width << "x" << height
              << " luma_pixels=" << rtlY.size() << " frame_mae_fixture=max_abs_y_zero\n";
    std::cout << "I4 pass counts: V=" << i4Pass[0] << " H=" << i4Pass[1]
              << " DC=" << i4Pass[2] << " DDL=" << i4Pass[3] << " DDR=" << i4Pass[4]
              << " VR=" << i4Pass[5] << " HD=" << i4Pass[6] << " VL=" << i4Pass[7]
              << " HU=" << i4Pass[8] << " I_NxN_MBs=" << i4Mbs << "\n";
    std::cout << "I16 pass counts: V=" << i16Pass[0] << " H=" << i16Pass[1]
              << " DC=" << i16Pass[2] << " PlaneUnsupportedSeen=" << i16Pass[3]
              << " I16_MBs=" << i16Mbs << "\n";
    std::cout << "Chroma pass counts (MBs): DC=" << chromaMbPass[0]
              << " H=" << chromaMbPass[1] << " V=" << chromaMbPass[2]
              << " Plane=" << chromaMbPass[3] << " plane_evals={" << chromaPlaneEvalPass[0]
              << "," << chromaPlaneEvalPass[1] << "," << chromaPlaneEvalPass[2]
              << "," << chromaPlaneEvalPass[3] << "}\n";
    std::cout << "Neighbour coverage: no_above_blocks=" << noAboveBlocks
              << " no_left_blocks=" << noLeftBlocks
              << " top_right_replicated_blocks=" << topRightReplicatedBlocks
              << " dc_fallback_probe=PASS\n";
    return 0;
}
