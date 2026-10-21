#include "Vh264_dpb_mc_tb.h"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int W = 624;
constexpr int H = 480;
constexpr int CW = W / 2;
constexpr int CH = H / 2;
constexpr int Y_BYTES = W * H;
constexpr int C_BYTES = CW * CH;
constexpr int FRAME_BYTES = Y_BYTES + 2 * C_BYTES;
constexpr int DPB_BYTES = 2 * FRAME_BYTES;

struct Sim {
    Vh264_dpb_mc_tb top;
    std::vector<uint8_t> mem = std::vector<uint8_t>(DPB_BYTES, 0xee);
    bool pendingRead = false;
    uint32_t pendingAddr = 0;
    uint64_t cycle = 0;

    void tick() {
        top.mem_rvalid = pendingRead;
        top.mem_rdata = pendingRead && pendingAddr < mem.size() ? mem[pendingAddr] : 0;
        top.clk = 0;
        top.eval();
        top.clk = 1;
        top.eval();
        if (top.mem_we) {
            if (top.mem_waddr >= mem.size()) throw std::runtime_error("write address out of DPB range");
            mem[top.mem_waddr] = static_cast<uint8_t>(top.mem_wdata);
        }
        pendingRead = top.mem_rd;
        pendingAddr = top.mem_raddr;
        top.clk = 0;
        top.eval();
        ++cycle;
    }
};

int countAnnexBNals(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open NAL fixture: " + path);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    int n = 0;
    for (std::size_t i = 2; i < b.size(); ++i) {
        if (b[i - 2] == 0 && b[i - 1] == 0 && b[i] == 1) ++n;
        else if (i >= 3 && b[i - 3] == 0 && b[i - 2] == 0 && b[i - 1] == 0 && b[i] == 1) ++n;
    }
    return n;
}

uint32_t i420Addr(uint32_t base, int plane, int x, int y) {
    if (plane == 0) return base + y * W + x;
    if (plane == 1) return base + Y_BYTES + y * CW + x;
    return base + Y_BYTES + C_BYTES + y * CW + x;
}

uint8_t yPattern(int x, int y) { return static_cast<uint8_t>((17 + x * 3 + y * 5) & 0xff); }
uint8_t uPattern(int x, int y) { return static_cast<uint8_t>((83 + x * 7 + y * 11) & 0xff); }
uint8_t vPattern(int x, int y) { return static_cast<uint8_t>((191 + x * 13 + y * 3) & 0xff); }

uint8_t planePattern(int plane, int x, int y) {
    if (plane == 0) return yPattern(x, y);
    if (plane == 1) return uPattern(x, y);
    return vPattern(x, y);
}

uint32_t mbSampleAddr(int mbx, int mby, int plane, int idx) {
    if (plane == 0) {
        int x = mbx * 16 + (idx & 15);
        int y = mby * 16 + (idx >> 4);
        return i420Addr(0, plane, x, y);
    }
    int x = mbx * 8 + (idx & 7);
    int y = mby * 8 + (idx >> 3);
    return i420Addr(0, plane, x, y);
}

void writeSample(Sim& s, int mbx, int mby, int plane, int idx) {
    int x = plane == 0 ? mbx * 16 + (idx & 15) : mbx * 8 + (idx & 7);
    int y = plane == 0 ? mby * 16 + (idx >> 4) : mby * 8 + (idx >> 3);
    s.top.filtered_sample_valid = 1;
    s.top.filtered_mb_x = mbx;
    s.top.filtered_mb_y = mby;
    s.top.filtered_plane = plane;
    s.top.filtered_sample_idx = idx;
    s.top.filtered_sample = planePattern(plane, x, y);
    s.tick();
    uint32_t want = mbSampleAddr(mbx, mby, plane, idx);
    if (!s.top.mem_we || s.top.mem_waddr != want || s.top.mem_wdata != planePattern(plane, x, y)) {
        std::cerr << "FAIL h264_dpb_mc RTL: filtered I420 writeback mismatch plane=" << plane
                  << " mb=(" << mbx << "," << mby << ") idx=" << idx
                  << " got we=" << int(s.top.mem_we) << " addr=" << s.top.mem_waddr
                  << " data=" << int(s.top.mem_wdata) << " want addr=" << want
                  << " data=" << int(planePattern(plane, x, y)) << "\n";
        throw std::runtime_error("filtered I420 writeback mismatch");
    }
}

int clip1(int v) { return std::max(0, std::min(255, v)); }
int avg2(int a, int b) { return (a + b + 1) >> 1; }

int wpix(const std::array<uint8_t, 441>& w, int r, int c) { return w.at(static_cast<size_t>(r * 21 + c)); }
int hraw(const std::array<uint8_t, 441>& w, int r, int c) {
    return wpix(w, r, c - 2) - 5 * wpix(w, r, c - 1) + 20 * wpix(w, r, c) +
           20 * wpix(w, r, c + 1) - 5 * wpix(w, r, c + 2) + wpix(w, r, c + 3);
}
int halfH(const std::array<uint8_t, 441>& w, int r, int c) { return clip1((hraw(w, r, c) + 16) >> 5); }
int halfV(const std::array<uint8_t, 441>& w, int r, int c) {
    return clip1((wpix(w, r - 2, c) - 5 * wpix(w, r - 1, c) + 20 * wpix(w, r, c) +
                  20 * wpix(w, r + 1, c) - 5 * wpix(w, r + 2, c) + wpix(w, r + 3, c) + 16) >> 5);
}
int halfC(const std::array<uint8_t, 441>& w, int r, int c) {
    int sum = hraw(w, r - 2, c) - 5 * hraw(w, r - 1, c) + 20 * hraw(w, r, c) +
              20 * hraw(w, r + 1, c) - 5 * hraw(w, r + 2, c) + hraw(w, r + 3, c);
    return clip1((sum + 512) >> 10);
}

uint8_t qpel(const std::array<uint8_t, 441>& w, int x, int y, int fx, int fy) {
    int r = y + 2;
    int c = x + 2;
    switch ((fy << 2) | fx) {
    case 0x0: return wpix(w, r, c);
    case 0x1: return avg2(wpix(w, r, c), halfH(w, r, c));
    case 0x2: return halfH(w, r, c);
    case 0x3: return avg2(halfH(w, r, c), wpix(w, r, c + 1));
    case 0x4: return avg2(wpix(w, r, c), halfV(w, r, c));
    case 0x5: return avg2(halfH(w, r, c), halfV(w, r, c));
    case 0x6: return avg2(halfH(w, r, c), halfC(w, r, c));
    case 0x7: return avg2(halfH(w, r, c), halfV(w, r, c + 1));
    case 0x8: return halfV(w, r, c);
    case 0x9: return avg2(halfV(w, r, c), halfC(w, r, c));
    case 0xa: return halfC(w, r, c);
    case 0xb: return avg2(halfC(w, r, c), halfV(w, r, c + 1));
    case 0xc: return avg2(halfV(w, r, c), wpix(w, r + 1, c));
    case 0xd: return avg2(halfH(w, r + 1, c), halfV(w, r, c));
    case 0xe: return avg2(halfC(w, r, c), halfH(w, r + 1, c));
    default: return avg2(halfH(w, r + 1, c), halfV(w, r, c + 1));
    }
}

uint8_t chroma(const std::array<uint8_t, 81>& w, int x, int y, int fx, int fy) {
    int p00 = w.at(static_cast<size_t>(y * 9 + x));
    int p10 = w.at(static_cast<size_t>(y * 9 + x + 1));
    int p01 = w.at(static_cast<size_t>((y + 1) * 9 + x));
    int p11 = w.at(static_cast<size_t>((y + 1) * 9 + x + 1));
    return static_cast<uint8_t>(((8 - fx) * (8 - fy) * p00 + fx * (8 - fy) * p10 +
                                 (8 - fx) * fy * p01 + fx * fy * p11 + 32) >> 6);
}

void reset(Sim& s) {
    s.top.reset = 1;
    s.top.idr_start = 0;
    s.top.frame_done = 0;
    s.top.filtered_sample_valid = 0;
    s.top.filtered_mb_valid = 0;
    s.top.filtered_mb_addr = 0;
    s.top.filtered_mb_is_ref = 0;
    s.top.filtered_frame_done = 0;
    s.top.frame_slot_i = 0;
    s.top.frame_boundary = 0;
    s.top.fetch_start = 0;
    s.tick();
    s.tick();
    s.top.reset = 0;
    s.tick();
}

void commitFilteredMb(Sim& s, int mbAddr, bool terminal) {
    s.top.filtered_sample_valid = 0;
    s.top.filtered_mb_valid = 1;
    s.top.filtered_mb_addr = mbAddr;
    s.top.filtered_mb_is_ref = 1;
    s.top.filtered_frame_done = terminal;
    s.top.frame_slot_i = 0;
    s.tick();
    s.top.filtered_mb_valid = 0;
    s.top.filtered_frame_done = 0;
}

int runDeblockDpbSeam(const std::string& nalFixture) {
    int nals = countAnnexBNals(nalFixture);
    if (nals < 2) {
        std::cerr << "FAIL h264_dpb_mc RTL: deblock-DPB seam bench requires >=2 NAL units, got " << nals << "\n";
        return 1;
    }

    Sim s;
    reset(s);
    s.top.filtered_mb_valid = 1;
    s.top.filtered_mb_addr = 0;
    s.top.filtered_mb_is_ref = 1;
    s.top.filtered_frame_done = 0;
    s.tick();
    if (s.top.deblock_wb_valid || !s.top.deblock_commit_order_error || s.top.ref_ready) {
        std::cerr << "FAIL h264_dpb_mc RTL: deblock-DPB seam MB commit before all filtered samples"
                  << " wb=" << int(s.top.deblock_wb_valid)
                  << " order_error=" << int(s.top.deblock_commit_order_error)
                  << " ref_ready=" << int(s.top.ref_ready) << "\n";
        return 1;
    }
    s.top.filtered_mb_valid = 0;
    s.tick();

    for (int i = 0; i < 256; ++i) writeSample(s, 0, 0, 0, i);
    for (int i = 0; i < 64; ++i) writeSample(s, 0, 0, 1, i);
    for (int i = 0; i < 64; ++i) writeSample(s, 0, 0, 2, i);
    commitFilteredMb(s, 0, false);
    if (!s.top.deblock_wb_valid || s.top.deblock_wb_mb_addr != 0 || s.top.ref_ready) {
        std::cerr << "FAIL h264_dpb_mc RTL: deblock-DPB seam nonterminal commit mismatch"
                  << " wb=" << int(s.top.deblock_wb_valid)
                  << " addr=" << int(s.top.deblock_wb_mb_addr)
                  << " ref_ready=" << int(s.top.ref_ready) << "\n";
        return 1;
    }
    s.tick();

    for (int i = 0; i < 256; ++i) writeSample(s, 38, 29, 0, i);
    for (int i = 0; i < 64; ++i) writeSample(s, 38, 29, 1, i);
    for (int i = 0; i < 64; ++i) writeSample(s, 38, 29, 2, i);
    commitFilteredMb(s, 1169, true);
    if (!s.top.deblock_wb_valid || s.top.deblock_wb_mb_addr != 1169 || s.top.ref_ready) {
        std::cerr << "FAIL h264_dpb_mc RTL: deblock-DPB seam terminal commit/ref_ready order"
                  << " wb=" << int(s.top.deblock_wb_valid)
                  << " addr=" << int(s.top.deblock_wb_mb_addr)
                  << " ref_ready=" << int(s.top.ref_ready) << "\n";
        return 1;
    }
    s.top.frame_boundary = 1;
    s.tick();
    s.top.frame_boundary = 0;
    if (!s.top.deblock_ref_ready_pulse || s.top.ref_ready) {
        std::cerr << "FAIL h264_dpb_mc RTL: deblock-DPB seam terminal commit/ref_ready order phase"
                  << " pulse=" << int(s.top.deblock_ref_ready_pulse)
                  << " ref_ready=" << int(s.top.ref_ready) << "\n";
        return 1;
    }
    s.tick();
    if (!s.top.ref_ready || s.top.reference_base != 0 || s.top.current_base != FRAME_BYTES) {
        std::cerr << "FAIL h264_dpb_mc RTL: deblock-DPB seam frame_done before terminal filtered MB commit"
                  << " ref_ready=" << int(s.top.ref_ready)
                  << " ref_base=" << s.top.reference_base
                  << " cur_base=" << s.top.current_base << "\n";
        return 1;
    }

    std::cout << "OK h264_dpb_mc deblock-DPB seam: filtered samples precede wb_valid; "
              << "terminal wb_valid precedes frame_done/ref_ready"
              << " nals=" << nals << " fixture=" << nalFixture << "\n";
    return 0;
}

bool wantLumaValid(int idx, int w, int h) {
    return (idx % 16) < w && (idx / 16) < h;
}

bool wantChromaValid(int idx, int w, int h) {
    return (idx % 8) < (w / 2) && (idx / 8) < (h / 2);
}

void checkPartMc(Sim& s, const std::array<uint8_t, 441>& luma, const std::array<uint8_t, 81>& u,
                 const std::array<uint8_t, 81>& v, int w, int h, int fx, int fy) {
    s.top.fetch_part_w = w;
    s.top.fetch_part_h = h;
    s.top.eval();
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            int idx = y * 16 + x;
            bool valid = wantLumaValid(idx, w, h);
            uint8_t want = valid ? qpel(luma, x, y, fx, fy) : 0;
            if (static_cast<bool>(s.top.pred_y_valid[idx]) != valid || s.top.pred_y[idx] != want) {
                std::cerr << "FAIL h264_dpb_mc RTL: part luma prediction/mask mismatch"
                          << " part=" << w << "x" << h << " idx=" << idx
                          << " valid=" << int(s.top.pred_y_valid[idx]) << " want_valid=" << valid
                          << " got=" << int(s.top.pred_y[idx]) << " want=" << int(want) << "\n";
                throw std::runtime_error("part luma prediction/mask mismatch");
            }
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            int idx = y * 8 + x;
            bool valid = wantChromaValid(idx, w, h);
            uint8_t wantU = valid ? chroma(u, x, y, fx & 7, fy & 7) : 0;
            uint8_t wantV = valid ? chroma(v, x, y, fx & 7, fy & 7) : 0;
            if (static_cast<bool>(s.top.pred_u_valid[idx]) != valid ||
                static_cast<bool>(s.top.pred_v_valid[idx]) != valid ||
                s.top.pred_u[idx] != wantU || s.top.pred_v[idx] != wantV) {
                std::cerr << "FAIL h264_dpb_mc RTL: part chroma prediction/mask mismatch"
                          << " part=" << w << "x" << h << " idx=" << idx
                          << " u_valid=" << int(s.top.pred_u_valid[idx])
                          << " v_valid=" << int(s.top.pred_v_valid[idx])
                          << " want_valid=" << valid
                          << " got=(" << int(s.top.pred_u[idx]) << "," << int(s.top.pred_v[idx])
                          << ") want=(" << int(wantU) << "," << int(wantV) << ")\n";
                throw std::runtime_error("part chroma prediction/mask mismatch");
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        bool seamOnly = false;
        std::string nalFixture = "tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_624x480_12f.264";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--deblock-dpb-seam")
                seamOnly = true;
            else
                nalFixture = arg;
        }
        if (seamOnly)
            return runDeblockDpbSeam(nalFixture);
        int nals = countAnnexBNals(nalFixture);
        if (nals < 2) {
            std::cerr << "FAIL h264_dpb_mc RTL: bench requires >=2 NAL units, got " << nals << "\n";
            return 1;
        }

        Sim s;
        reset(s);
        if (s.top.ref_ready) {
            std::cerr << "FAIL h264_dpb_mc RTL: early reference publication before frame boundary\n";
            return 1;
        }

        for (int mby = 0; mby < 2; ++mby) {
            for (int mbx = 0; mbx < 2; ++mbx) {
                for (int i = 0; i < 256; ++i) writeSample(s, mbx, mby, 0, i);
                for (int i = 0; i < 64; ++i) writeSample(s, mbx, mby, 1, i);
                for (int i = 0; i < 64; ++i) writeSample(s, mbx, mby, 2, i);
            }
        }
        s.top.filtered_sample_valid = 0;
        s.tick();
        if (s.top.ref_ready) {
            std::cerr << "FAIL h264_dpb_mc RTL: reference became ready before frame_done\n";
            return 1;
        }

        s.top.frame_done = 1;
        s.tick();
        s.top.frame_done = 0;
        s.tick();
        if (!s.top.ref_ready || s.top.reference_base != 0 || s.top.current_base != FRAME_BYTES) {
            std::cerr << "FAIL h264_dpb_mc RTL: frame-boundary promotion mismatch ref_ready="
                      << int(s.top.ref_ready) << " ref_base=" << s.top.reference_base
                      << " cur_base=" << s.top.current_base << "\n";
            return 1;
        }

        s.top.fetch_mb_x = 0;
        s.top.fetch_mb_y = 0;
        s.top.fetch_part_mode = 0;
        s.top.fetch_part_idx = 0;
        s.top.fetch_part_w = 16;
        s.top.fetch_part_h = 16;
        s.top.fetch_mv_x_qpel = static_cast<int16_t>(-5);
        s.top.fetch_mv_y_qpel = static_cast<int16_t>(-7);
        s.top.fetch_start = 1;
        s.tick();
        s.top.fetch_start = 0;

        std::array<uint8_t, 441> luma{};
        std::array<uint8_t, 81> u{};
        std::array<uint8_t, 81> v{};
        std::array<bool, 441> lSeen{};
        std::array<bool, 81> uSeen{};
        std::array<bool, 81> vSeen{};
        int lc = 0, uc = 0, vc = 0;
        bool sawDone = false;
        for (int guard = 0; guard < 800 && (lc < 441 || uc < 81 || vc < 81 || !sawDone); ++guard) {
            s.tick();
            if (s.top.luma_window_valid) {
                int idx = s.top.luma_window_idx;
                // H.264 8.4.2.1 / 6-tap support: sample at origin + win - 2,
                // with out-of-picture coordinates clamped to the edge sample
                // (not wrapped). origin already includes mb*16 + mv>>2.
                const int ox = static_cast<int16_t>(s.top.luma_origin_x);
                const int oy = static_cast<int16_t>(s.top.luma_origin_y);
                int sx = std::clamp(ox + (idx % 21) - 2, 0, W - 1);
                int sy = std::clamp(oy + (idx / 21) - 2, 0, H - 1);
                uint8_t want = s.mem[i420Addr(0, 0, sx, sy)];
                if (s.top.luma_window_sample != want) {
                    std::cerr << "FAIL h264_dpb_mc RTL: luma window clamp mismatch idx=" << idx
                              << " got=" << int(s.top.luma_window_sample) << " want=" << int(want)
                              << " src=(" << sx << "," << sy << ")\n";
                    return 1;
                }
                if (!lSeen[idx]) { lSeen[idx] = true; ++lc; }
                luma[idx] = s.top.luma_window_sample;
            }
            if (s.top.chroma_u_window_valid || s.top.chroma_v_window_valid) {
                int idx = s.top.chroma_window_idx;
                const int cx0 = static_cast<int16_t>(s.top.chroma_origin_x);
                const int cy0 = static_cast<int16_t>(s.top.chroma_origin_y);
                int sx = std::clamp(cx0 + (idx % 9), 0, CW - 1);
                int sy = std::clamp(cy0 + (idx / 9), 0, CH - 1);
                int plane = s.top.chroma_u_window_valid ? 1 : 2;
                uint8_t want = s.mem[i420Addr(0, plane, sx, sy)];
                if (s.top.chroma_window_sample != want) {
                    std::cerr << "FAIL h264_dpb_mc RTL: chroma window clamp mismatch plane=" << plane
                              << " idx=" << idx << " got=" << int(s.top.chroma_window_sample)
                              << " want=" << int(want) << "\n";
                    return 1;
                }
                if (plane == 1) {
                    if (!uSeen[idx]) { uSeen[idx] = true; ++uc; }
                    u[idx] = s.top.chroma_window_sample;
                } else {
                    if (!vSeen[idx]) { vSeen[idx] = true; ++vc; }
                    v[idx] = s.top.chroma_window_sample;
                }
            }
            sawDone = sawDone || s.top.fetch_done;
        }
        if (lc != 441 || uc != 81 || vc != 81 || !sawDone) {
            std::cerr << "FAIL h264_dpb_mc RTL: incomplete DPB fetch luma=" << lc
                      << " u=" << uc << " v=" << vc << " done=" << sawDone << "\n";
            return 1;
        }
        if (static_cast<int16_t>(s.top.luma_origin_x) != -2 || static_cast<int16_t>(s.top.luma_origin_y) != -2 ||
            static_cast<int16_t>(s.top.chroma_origin_x) != -1 || static_cast<int16_t>(s.top.chroma_origin_y) != -1 ||
            s.top.luma_frac_x != 3 || s.top.luma_frac_y != 1 ||
            s.top.chroma_frac_x != 3 || s.top.chroma_frac_y != 1) {
            std::cerr << "FAIL h264_dpb_mc RTL: MV origin/frac mismatch\n";
            return 1;
        }

        for (int i = 0; i < 441; ++i) s.top.luma_ref_win[i] = luma[i];
        for (int i = 0; i < 81; ++i) {
            s.top.chroma_u_ref_win[i] = u[i];
            s.top.chroma_v_ref_win[i] = v[i];
        }
        s.top.eval();
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                int idx = y * 16 + x;
                uint8_t want = qpel(luma, x, y, 3, 1);
                if (s.top.pred_y[idx] != want) {
                    std::cerr << "FAIL h264_dpb_mc RTL: MC luma prediction mismatch idx=" << idx
                              << " got=" << int(s.top.pred_y[idx]) << " want=" << int(want) << "\n";
                    return 1;
                }
            }
        }
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int idx = y * 8 + x;
                uint8_t wantU = chroma(u, x, y, 3, 1);
                uint8_t wantV = chroma(v, x, y, 3, 1);
                if (s.top.pred_u[idx] != wantU || s.top.pred_v[idx] != wantV) {
                    std::cerr << "FAIL h264_dpb_mc RTL: MC chroma prediction mismatch idx=" << idx
                              << " got=(" << int(s.top.pred_u[idx]) << "," << int(s.top.pred_v[idx])
                              << ") want=(" << int(wantU) << "," << int(wantV) << ")\n";
                    return 1;
                }
            }
        }
        checkPartMc(s, luma, u, v, 16, 8, 3, 1);
        checkPartMc(s, luma, u, v, 8, 16, 3, 1);
        checkPartMc(s, luma, u, v, 8, 8, 3, 1);
        checkPartMc(s, luma, u, v, 8, 4, 3, 1);
        checkPartMc(s, luma, u, v, 4, 8, 3, 1);
        checkPartMc(s, luma, u, v, 4, 4, 3, 1);

        s.top.idr_start = 1;
        s.tick();
        s.top.idr_start = 0;
        s.tick();
        if (s.top.ref_ready) {
            std::cerr << "FAIL h264_dpb_mc RTL: IDR did not invalidate prior reference\n";
            return 1;
        }
        s.top.fetch_start = 1;
        s.tick();
        s.top.fetch_start = 0;
        if (!s.top.fetch_error_no_ref) {
            std::cerr << "FAIL h264_dpb_mc RTL: fetch without reference did not raise error\n";
            return 1;
        }

        std::cout << "OK real RTL sim: h264_dpb_mc product RTL"
                  << " nals=" << nals
                  << " i420_writes=" << (4 * (256 + 64 + 64))
                  << " luma_window=" << lc
                  << " chroma_windows=" << uc << "/" << vc
                  << " mc_pixels=256/64/64"
                  << " part_modes=16x8/8x16/8x8/8x4/4x8/4x4"
                  << " fixture=" << nalFixture << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL h264_dpb_mc RTL: " << e.what() << "\n";
        return 1;
    }
}
