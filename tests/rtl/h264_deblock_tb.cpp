#include "Vh264_deblock_tb.h"
#include "verilated.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readText(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "FAIL deblock mb_golden: cannot open " << path << "\n";
        std::exit(1);
    }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int parseIntAfter(const std::string& text, const std::string& key, std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle, start);
    if (p == std::string::npos) {
        std::cerr << "FAIL deblock mb_golden: missing key " << key << "\n";
        std::exit(1);
    }
    p = text.find(':', p);
    if (p == std::string::npos) {
        std::cerr << "FAIL deblock mb_golden: malformed key " << key << "\n";
        std::exit(1);
    }
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    char* end = nullptr;
    long v = std::strtol(text.c_str() + p, &end, 10);
    if (end == text.c_str() + p) {
        std::cerr << "FAIL deblock mb_golden: invalid integer for " << key << "\n";
        std::exit(1);
    }
    return static_cast<int>(v);
}

std::vector<uint8_t> parseByteArrayAfter(const std::string& text, const std::string& key, std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle, start);
    if (p == std::string::npos) {
        std::cerr << "FAIL deblock mb_golden: missing array " << key << "\n";
        std::exit(1);
    }
    p = text.find('[', p);
    const std::size_t q = text.find(']', p);
    if (p == std::string::npos || q == std::string::npos) {
        std::cerr << "FAIL deblock mb_golden: malformed array " << key << "\n";
        std::exit(1);
    }
    std::vector<uint8_t> out;
    const char* cur = text.c_str() + p + 1;
    const char* end = text.c_str() + q;
    while (cur < end) {
        while (cur < end && (std::isspace(static_cast<unsigned char>(*cur)) || *cur == ',')) ++cur;
        if (cur >= end) break;
        char* next = nullptr;
        long v = std::strtol(cur, &next, 10);
        if (next == cur || v < 0 || v > 255) {
            std::cerr << "FAIL deblock mb_golden: invalid byte in " << key << "\n";
            std::exit(1);
        }
        out.push_back(static_cast<uint8_t>(v));
        cur = next;
    }
    return out;
}

struct EdgeIO {
    std::array<uint8_t, 4> p3{}, p2{}, p1{}, p0{}, q0{}, q1{}, q2{}, q3{};
};

struct EdgeOut {
    std::array<uint8_t, 4> p2{}, p1{}, p0{}, q0{}, q1{}, q2{};
};

int clip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int clip8(int v) { return clip(v, 0, 255); }
int absdiff(int a, int b) { return a >= b ? a - b : b - a; }

int alphaTable(int idx) {
    static constexpr int t[52] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        4,4,5,6,7,8,9,10,12,13,15,17,20,22,25,28,
        32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,203,226,255,255};
    return t[clip(idx, 0, 51)];
}

int betaTable(int idx) {
    static constexpr int t[52] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,
        9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18};
    return t[clip(idx, 0, 51)];
}

int tc0Table(int idx, int bs) {
    static constexpr int t[52][3] = {
        {-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},
        {-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},{-1,-1,-1},
        {0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},
        {1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},
        {1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},{3,4,6},
        {4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},{8,11,16},
        {9,12,18},{10,13,20},{11,15,23},{13,17,25}};
    if (bs < 1 || bs > 3) return 0;
    return t[clip(idx, 0, 51)][bs - 1];
}

EdgeOut refEdge(const EdgeIO& in, bool chroma, int bs, int qp, int alphaOff, int betaOff) {
    EdgeOut out{in.p2, in.p1, in.p0, in.q0, in.q1, in.q2};
    const int indexA = clip(qp + alphaOff, 0, 51);
    const int indexB = clip(qp + betaOff, 0, 51);
    const int alpha = alphaTable(indexA);
    const int beta = betaTable(indexB);
    const int tc0 = tc0Table(indexA, bs);
    for (int i = 0; i < 4; ++i) {
        const int p3 = in.p3[i], p2 = in.p2[i], p1 = in.p1[i], p0 = in.p0[i];
        const int q0 = in.q0[i], q1 = in.q1[i], q2 = in.q2[i], q3 = in.q3[i];
        const bool ok = bs != 0 && absdiff(p0, q0) < alpha && absdiff(p1, p0) < beta && absdiff(q1, q0) < beta;
        const bool ap = absdiff(p2, p0) < beta;
        const bool aq = absdiff(q2, q0) < beta;
        const bool strongExtra = absdiff(p0, q0) < ((alpha >> 2) + 2);
        if (!ok) continue;
        if (bs == 4) {
            if (chroma) {
                out.p0[i] = clip8((2 * p1 + p0 + q1 + 2) >> 2);
                out.q0[i] = clip8((2 * q1 + q0 + p1 + 2) >> 2);
            } else if (strongExtra) {
                if (ap) {
                    out.p0[i] = clip8((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                    out.p1[i] = clip8((p2 + p1 + p0 + q0 + 2) >> 2);
                    out.p2[i] = clip8((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
                } else {
                    out.p0[i] = clip8((2 * p1 + p0 + q1 + 2) >> 2);
                }
                if (aq) {
                    out.q0[i] = clip8((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                    out.q1[i] = clip8((p0 + q0 + q1 + q2 + 2) >> 2);
                    out.q2[i] = clip8((p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3);
                } else {
                    out.q0[i] = clip8((2 * q1 + q0 + p1 + 2) >> 2);
                }
            } else {
                out.p0[i] = clip8((2 * p1 + p0 + q1 + 2) >> 2);
                out.q0[i] = clip8((2 * q1 + q0 + p1 + 2) >> 2);
            }
        } else {
            const int tc = chroma ? tc0 + 1 : tc0 + (ap ? 1 : 0) + (aq ? 1 : 0);
            const int delta = clip((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc, tc);
            out.p0[i] = clip8(p0 + delta);
            out.q0[i] = clip8(q0 - delta);
            if (!chroma && ap) {
                const int adj = clip((p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1, -tc0, tc0);
                out.p1[i] = clip8(p1 + adj);
            }
            if (!chroma && aq) {
                const int adj = clip((q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1, -tc0, tc0);
                out.q1[i] = clip8(q1 + adj);
            }
        }
    }
    return out;
}

EdgeOut dutEdge(Vh264_deblock_tb& dut, const EdgeIO& in, bool chroma, int bs, int qp, int alphaOff, int betaOff) {
    dut.is_chroma = chroma;
    dut.bs_in = bs;
    dut.qp_avg = qp;
    dut.alpha_off = alphaOff;
    dut.beta_off = betaOff;
    for (int i = 0; i < 4; ++i) {
        dut.p3_in[i] = in.p3[i]; dut.p2_in[i] = in.p2[i]; dut.p1_in[i] = in.p1[i]; dut.p0_in[i] = in.p0[i];
        dut.q0_in[i] = in.q0[i]; dut.q1_in[i] = in.q1[i]; dut.q2_in[i] = in.q2[i]; dut.q3_in[i] = in.q3[i];
    }
    dut.eval();
    EdgeOut out{};
    for (int i = 0; i < 4; ++i) {
        out.p2[i] = dut.p2_out[i]; out.p1[i] = dut.p1_out[i]; out.p0[i] = dut.p0_out[i];
        out.q0[i] = dut.q0_out[i]; out.q1[i] = dut.q1_out[i]; out.q2[i] = dut.q2_out[i];
    }
    return out;
}

std::string edgeString(const EdgeOut& e) {
    std::ostringstream os;
    os << "p2/p1/p0/q0/q1/q2=";
    for (const auto* arr : {&e.p2, &e.p1, &e.p0, &e.q0, &e.q1, &e.q2}) {
        os << '[';
        for (int i = 0; i < 4; ++i) os << (i ? "," : "") << int((*arr)[i]);
        os << ']';
    }
    return os.str();
}

bool same(const EdgeOut& a, const EdgeOut& b) {
    return a.p2 == b.p2 && a.p1 == b.p1 && a.p0 == b.p0 && a.q0 == b.q0 && a.q1 == b.q1 && a.q2 == b.q2;
}

void requireEdge(Vh264_deblock_tb& dut, const std::string& name, const EdgeIO& in, bool chroma, int bs, int qp, int alphaOff, int betaOff) {
    const EdgeOut want = refEdge(in, chroma, bs, qp, alphaOff, betaOff);
    const EdgeOut got = dutEdge(dut, in, chroma, bs, qp, alphaOff, betaOff);
    if (!same(want, got)) {
        std::cerr << "FAIL " << name << " got " << edgeString(got) << " want " << edgeString(want) << "\n";
        std::exit(1);
    }
}


EdgeOut pipeOut(const Vh264_deblock_tb& dut) {
    EdgeOut out{};
    for (int i = 0; i < 4; ++i) {
        out.p2[i] = dut.pipe_p2_out[i]; out.p1[i] = dut.pipe_p1_out[i]; out.p0[i] = dut.pipe_p0_out[i];
        out.q0[i] = dut.pipe_q0_out[i]; out.q1[i] = dut.pipe_q1_out[i]; out.q2[i] = dut.pipe_q2_out[i];
    }
    return out;
}

void tick(Vh264_deblock_tb& dut) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
}

void testPipeLatency(Vh264_deblock_tb& dut, const EdgeIO& in) {
    dut.reset = 1;
    dut.pipe_valid_i = 0;
    tick(dut);
    if (dut.pipe_valid_o) {
        std::cerr << "FAIL deblock pipe: valid_o high during reset\n";
        std::exit(1);
    }
    dut.reset = 0;
    const EdgeOut want = dutEdge(dut, in, false, 2, 32, 0, 0);
    dut.pipe_valid_i = 1;
    tick(dut);
    if (dut.pipe_valid_o) {
        std::cerr << "FAIL deblock pipe: valid_o asserted without registered edge latency\n";
        std::exit(1);
    }
    EdgeIO poison{};
    (void)dutEdge(dut, poison, false, 0, 0, 0, 0);
    dut.pipe_valid_i = 0;
    tick(dut);
    const EdgeOut got = pipeOut(dut);
    if (!dut.pipe_valid_o || !same(got, want)) {
        std::cerr << "FAIL deblock pipe latency got_valid=" << int(dut.pipe_valid_o)
                  << " got " << edgeString(got) << " want " << edgeString(want) << "\n";
        std::exit(1);
    }
    tick(dut);
    if (dut.pipe_valid_o) {
        std::cerr << "FAIL deblock pipe: valid_o did not drain after one result\n";
        std::exit(1);
    }
}

void testBs(Vh264_deblock_tb& dut) {
    auto eval = [&]() { dut.eval(); return int(dut.bs_derived); };
    dut.disable_all = 0; dut.slice_boundary_blocked = 0; dut.mb_boundary = 1;
    dut.p_intra = 1; dut.q_intra = 0; dut.p_nonzero = 0; dut.q_nonzero = 0;
    dut.p_ref = 0; dut.q_ref = 0; dut.p_mvx = 0; dut.p_mvy = 0; dut.q_mvx = 0; dut.q_mvy = 0;
    if (eval() != 4) { std::cerr << "FAIL bS intra MB boundary\n"; std::exit(1); }
    dut.mb_boundary = 0;
    if (eval() != 3) { std::cerr << "FAIL bS intra internal\n"; std::exit(1); }
    dut.p_intra = 0; dut.q_nonzero = 1;
    if (eval() != 2) { std::cerr << "FAIL bS residual\n"; std::exit(1); }
    dut.q_nonzero = 0; dut.p_nonzero = 1;
    if (eval() != 2) { std::cerr << "FAIL bS residual p-side\n"; std::exit(1); }
    dut.p_nonzero = 0;
    dut.q_nonzero = 0; dut.q_mvx = 4;
    if (eval() != 1) { std::cerr << "FAIL bS mv threshold\n"; std::exit(1); }
    dut.q_mvx = 3;
    if (eval() != 0) { std::cerr << "FAIL bS mv below threshold\n"; std::exit(1); }
    dut.q_mvx = -4;
    if (eval() != 1) { std::cerr << "FAIL bS negative mvx threshold\n"; std::exit(1); }
    dut.q_mvx = 0; dut.q_mvy = 4;
    if (eval() != 1) { std::cerr << "FAIL bS mvy threshold\n"; std::exit(1); }
    dut.q_mvy = -4;
    if (eval() != 1) { std::cerr << "FAIL bS negative mvy threshold\n"; std::exit(1); }
    dut.q_mvy = 0; dut.p_mvx = 4;
    if (eval() != 1) { std::cerr << "FAIL bS p-side mvx threshold\n"; std::exit(1); }
    dut.p_mvx = 0; dut.p_mvy = -4;
    if (eval() != 1) { std::cerr << "FAIL bS p-side negative mvy threshold\n"; std::exit(1); }
    dut.p_mvy = 0;
    dut.q_ref = 1;
    if (eval() != 1) { std::cerr << "FAIL bS ref diff\n"; std::exit(1); }
    dut.q_ref = 0; dut.p_ref = 1;
    if (eval() != 1) { std::cerr << "FAIL bS p-side ref diff\n"; std::exit(1); }
    dut.p_ref = 0;
    dut.slice_boundary_blocked = 1;
    if (eval() != 0) { std::cerr << "FAIL bS disable idc2 boundary\n"; std::exit(1); }
    dut.slice_boundary_blocked = 0; dut.disable_all = 1;
    if (eval() != 0) { std::cerr << "FAIL bS disable all\n"; std::exit(1); }
    dut.disable_all = 0; dut.p_ref = 2;
    dut.eval();
    if (!dut.unsupported_ref) { std::cerr << "FAIL bS unsupported ref loud flag\n"; std::exit(1); }
}

void testThresholds(Vh264_deblock_tb& dut) {
    EdgeIO e{};
    dutEdge(dut, e, false, 3, 40, 0, 0);
    if (dut.alpha_dbg != 80 || dut.beta_dbg != 13 || dut.tc0_dbg != 7) {
        std::cerr << "FAIL thresholds qp40 bS3 got alpha=" << int(dut.alpha_dbg)
                  << " beta=" << int(dut.beta_dbg) << " tc0=" << int(dut.tc0_dbg) << "\n";
        std::exit(1);
    }
    dutEdge(dut, e, false, 2, 28, 6, -6);
    if (dut.alpha_dbg != 40 || dut.beta_dbg != 3 || dut.tc0_dbg != 2) {
        std::cerr << "FAIL thresholds offset clip/index got alpha=" << int(dut.alpha_dbg)
                  << " beta=" << int(dut.beta_dbg) << " tc0=" << int(dut.tc0_dbg) << "\n";
        std::exit(1);
    }
    dutEdge(dut, e, false, 1, 51, 12, 12);
    if (dut.alpha_dbg != 255 || dut.beta_dbg != 18 || dut.tc0_dbg != 13) {
        std::cerr << "FAIL thresholds high clip\n";
        std::exit(1);
    }
    dutEdge(dut, e, false, 3, 4, -12, -12);
    if (dut.alpha_dbg != 0 || dut.beta_dbg != 0 || dut.tc0_dbg != 0) {
        std::cerr << "FAIL thresholds low clip\n";
        std::exit(1);
    }
}

using Frame = std::vector<uint8_t>;

EdgeIO gatherVertical(const Frame& f, int w, int x, int y) {
    EdgeIO e{};
    for (int r = 0; r < 4; ++r) {
        const int yy = y + r;
        e.p3[r] = f[yy * w + x - 4]; e.p2[r] = f[yy * w + x - 3]; e.p1[r] = f[yy * w + x - 2]; e.p0[r] = f[yy * w + x - 1];
        e.q0[r] = f[yy * w + x + 0]; e.q1[r] = f[yy * w + x + 1]; e.q2[r] = f[yy * w + x + 2]; e.q3[r] = f[yy * w + x + 3];
    }
    return e;
}

void scatterVertical(Frame& f, int w, int x, int y, const EdgeOut& o) {
    for (int r = 0; r < 4; ++r) {
        const int yy = y + r;
        f[yy * w + x - 3] = o.p2[r]; f[yy * w + x - 2] = o.p1[r]; f[yy * w + x - 1] = o.p0[r];
        f[yy * w + x + 0] = o.q0[r]; f[yy * w + x + 1] = o.q1[r]; f[yy * w + x + 2] = o.q2[r];
    }
}

EdgeIO gatherHorizontal(const Frame& f, int w, int x, int y) {
    EdgeIO e{};
    for (int c = 0; c < 4; ++c) {
        const int xx = x + c;
        e.p3[c] = f[(y - 4) * w + xx]; e.p2[c] = f[(y - 3) * w + xx]; e.p1[c] = f[(y - 2) * w + xx]; e.p0[c] = f[(y - 1) * w + xx];
        e.q0[c] = f[(y + 0) * w + xx]; e.q1[c] = f[(y + 1) * w + xx]; e.q2[c] = f[(y + 2) * w + xx]; e.q3[c] = f[(y + 3) * w + xx];
    }
    return e;
}

void scatterHorizontal(Frame& f, int w, int x, int y, const EdgeOut& o) {
    for (int c = 0; c < 4; ++c) {
        const int xx = x + c;
        f[(y - 3) * w + xx] = o.p2[c]; f[(y - 2) * w + xx] = o.p1[c]; f[(y - 1) * w + xx] = o.p0[c];
        f[(y + 0) * w + xx] = o.q0[c]; f[(y + 1) * w + xx] = o.q1[c]; f[(y + 2) * w + xx] = o.q2[c];
    }
}

void filterFrameRef(Frame& f, int w, int h, bool horizontalFirst) {
    auto applyV = [&]() {
        for (int x : {4, 8, 12, 16, 20, 24, 28})
            for (int y = 0; y < h; y += 4) {
                const int bs = (x == 16) ? 4 : (((x + y) & 8) ? 2 : 1);
                scatterVertical(f, w, x, y, refEdge(gatherVertical(f, w, x, y), false, bs, 32, 0, 0));
            }
    };
    auto applyH = [&]() {
        for (int y : {4, 8, 12, 16, 20, 24, 28})
            for (int x = 0; x < w; x += 4) {
                const int bs = (y == 16) ? 4 : (((x + y) & 8) ? 2 : 1);
                scatterHorizontal(f, w, x, y, refEdge(gatherHorizontal(f, w, x, y), false, bs, 32, 0, 0));
            }
    };
    if (horizontalFirst) { applyH(); applyV(); } else { applyV(); applyH(); }
}

void filterFrameDut(Vh264_deblock_tb& dut, Frame& f, int w, int h, bool horizontalFirst) {
    auto applyV = [&]() {
        for (int x : {4, 8, 12, 16, 20, 24, 28})
            for (int y = 0; y < h; y += 4) {
                const int bs = (x == 16) ? 4 : (((x + y) & 8) ? 2 : 1);
                scatterVertical(f, w, x, y, dutEdge(dut, gatherVertical(f, w, x, y), false, bs, 32, 0, 0));
            }
    };
    auto applyH = [&]() {
        for (int y : {4, 8, 12, 16, 20, 24, 28})
            for (int x = 0; x < w; x += 4) {
                const int bs = (y == 16) ? 4 : (((x + y) & 8) ? 2 : 1);
                scatterHorizontal(f, w, x, y, dutEdge(dut, gatherHorizontal(f, w, x, y), false, bs, 32, 0, 0));
            }
    };
    if (horizontalFirst) { applyH(); applyV(); } else { applyV(); applyH(); }
}

uint32_t fnv1a(const Frame& f) {
    uint32_t h = 2166136261u;
    for (uint8_t b : f) { h ^= b; h *= 16777619u; }
    return h;
}


void runMbGolden(Vh264_deblock_tb& dut, const std::string& path) {
    const std::string json = readText(path);
    if (json.find("\"format\": \"misterplex.p3.mb_golden.v1\"") == std::string::npos) {
        std::cerr << "FAIL deblock mb_golden: wrong or missing format marker\n";
        std::exit(1);
    }
    if (json.find("\"first_recon_signature8_hex\": \"0x3b\"") == std::string::npos ||
        json.find("\"first_pred_only_signature8_hex\": \"0x00\"") == std::string::npos ||
        json.find("\"first_one_cycle_delayed_signature8_hex\": \"0x00\"") == std::string::npos) {
        std::cerr << "FAIL deblock mb_golden: latency signatures must be true=0x3b pred-only=0x00 delayed=0x00\n";
        std::exit(1);
    }
    const std::size_t mbPos = json.find("\"macroblock\"");
    const int qp = parseIntAfter(json, "qp", mbPos == std::string::npos ? 0 : mbPos);
    const std::size_t samplesPos = json.find("\"samples\"");
    const auto recon = parseByteArrayAfter(json, "recon_y", samplesPos == std::string::npos ? 0 : samplesPos);
    if (recon.size() != 256) {
        std::cerr << "FAIL deblock mb_golden: recon_y size " << recon.size() << " expected 256\n";
        std::exit(1);
    }
    Frame ref = recon;
    Frame got = recon;
    constexpr int W = 16, H = 16;
    for (int x : {4, 8, 12}) {
        for (int y = 0; y < H; y += 4) {
            scatterVertical(ref, W, x, y, refEdge(gatherVertical(ref, W, x, y), false, 3, qp, 0, 0));
            scatterVertical(got, W, x, y, dutEdge(dut, gatherVertical(got, W, x, y), false, 3, qp, 0, 0));
        }
    }
    for (int y : {4, 8, 12}) {
        for (int x = 0; x < W; x += 4) {
            scatterHorizontal(ref, W, x, y, refEdge(gatherHorizontal(ref, W, x, y), false, 3, qp, 0, 0));
            scatterHorizontal(got, W, x, y, dutEdge(dut, gatherHorizontal(got, W, x, y), false, 3, qp, 0, 0));
        }
    }
    if (ref != got) {
        for (std::size_t i = 0; i < ref.size(); ++i) {
            if (ref[i] != got[i]) {
                std::cerr << "FAIL deblock mb_golden first_mismatch=" << i << " got=" << int(got[i])
                          << " want=" << int(ref[i]) << " got_fnv=0x" << std::hex << fnv1a(got)
                          << " want_fnv=0x" << fnv1a(ref) << std::dec << "\n";
                std::exit(1);
            }
        }
    }
    std::cout << "OK deblock mb_golden.v1 MB0 latency-reject true=0x3b pred-only=0x00 delayed=0x00 intra-edge pass qp=" << qp
              << " fnv=0x" << std::hex << fnv1a(got) << std::dec << "\n";
}

void runNalSequenceContract(const std::string& path, const std::string& mbGoldenPath) {
    const std::string json = readText(path);
    const std::string golden = readText(mbGoldenPath);
    if (json.find("\"format\": \"misterplex.p3.nal_sequence.v1\"") == std::string::npos) {
        std::cerr << "FAIL deblock nal_sequence: wrong or missing format marker\n";
        std::exit(1);
    }
    const std::size_t seqPos = json.find("\"sequence\"");
    const int nalCount = parseIntAfter(json, "nal_count", seqPos == std::string::npos ? 0 : seqPos);
    const int vclCount = parseIntAfter(json, "vcl", seqPos == std::string::npos ? 0 : seqPos);
    const int idrCount = parseIntAfter(json, "idr", seqPos == std::string::npos ? 0 : seqPos);
    const int pSlices = parseIntAfter(json, "p_slices", seqPos == std::string::npos ? 0 : seqPos);
    if (nalCount < 2 || vclCount < 2 || idrCount < 1 || pSlices < 1 ||
        json.find("\"requires_idle_between_vcl\": true") == std::string::npos ||
        json.find("\"return_to_idle_before_next_vcl_required\": true") == std::string::npos) {
        std::cerr << "FAIL deblock nal_sequence: expected multi-NAL IDR+P fixture with idle-between-VCL requirement"
                  << " nals=" << nalCount << " vcl=" << vclCount << " idr=" << idrCount
                  << " p_slices=" << pSlices << "\n";
        std::exit(1);
    }
    const std::size_t srcPos = json.find("\"path\": \"");
    if (srcPos == std::string::npos) {
        std::cerr << "FAIL deblock nal_sequence: missing source path\n";
        std::exit(1);
    }
    const std::size_t srcStart = srcPos + 9;
    const std::size_t srcEnd = json.find('"', srcStart);
    const std::string srcName = json.substr(srcStart, srcEnd - srcStart);
    if (golden.find("\"path\": \"" + srcName + "\"") == std::string::npos) {
        std::cerr << "FAIL deblock nal_sequence: mb_golden source does not match sequence source " << srcName << "\n";
        std::exit(1);
    }
    std::cout << "OK deblock nal_sequence multi-NAL contract: nals=" << nalCount
              << " vcl=" << vclCount << " idr=" << idrCount << " p_slices=" << pSlices
              << " source=" << srcName << "\n";
}

void runDrift(Vh264_deblock_tb& dut, bool faultHorizontalFirst) {
    constexpr int W = 32, H = 32;
    Frame ref(W * H), got(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            ref[y * W + x] = got[y * W + x] = static_cast<uint8_t>(clip8(96 + x + y + ((x >= 16) ? 9 : 0) + ((y >= 16) ? 7 : 0)));
    for (int frame = 0; frame < 5; ++frame) {
        for (int i = 0; i < W * H; ++i) {
            const int residual = ((i * 7 + frame * 11) % 5) - 2;
            ref[i] = clip8(ref[i] + residual);
            got[i] = clip8(got[i] + residual);
        }
        filterFrameRef(ref, W, H, false);
        filterFrameDut(dut, got, W, H, faultHorizontalFirst);
    }
    if (ref != got) {
        for (std::size_t i = 0; i < ref.size(); ++i) {
            if (ref[i] != got[i]) {
                std::cerr << "FAIL deblock multi-frame drift first_mismatch=" << i
                          << " got=" << int(got[i]) << " want=" << int(ref[i])
                          << " got_fnv=0x" << std::hex << fnv1a(got)
                          << " want_fnv=0x" << fnv1a(ref) << std::dec << "\n";
                return std::exit(1);
            }
        }
    }
    std::cout << "OK deblock multi-frame drift fnv=0x" << std::hex << fnv1a(got) << std::dec << "\n";
}

void testWritebackContract(Vh264_deblock_tb& dut) {
    dut.reset = 1;
    dut.idr_frame_start = 0;
    dut.filtered_sample_valid = 0;
    dut.filtered_mb_valid = 0;
    dut.filtered_mb_addr = 0;
    dut.filtered_mb_is_ref = 0;
    dut.filtered_frame_done = 0;
    dut.frame_slot_i = 0;
    dut.frame_boundary = 0;
    tick(dut);
    dut.reset = 0;

    dut.idr_frame_start = 1;
    tick(dut);
    if (!dut.dpb_invalidate_refs || dut.ref_ready_pulse) {
        std::cerr << "FAIL deblock writeback: IDR invalidate/ref_ready got invalidate="
                  << int(dut.dpb_invalidate_refs) << " ready=" << int(dut.ref_ready_pulse) << "\n";
        std::exit(1);
    }
    dut.idr_frame_start = 0;

    dut.filtered_mb_valid = 1;
    dut.filtered_mb_addr = 3;
    dut.filtered_mb_is_ref = 1;
    dut.filtered_frame_done = 0;
    dut.frame_slot_i = 2;
    tick(dut);
    if (dut.wb_valid || !dut.commit_order_error) {
        std::cerr << "FAIL deblock writeback: MB commit before all filtered samples"
                  << " wb=" << int(dut.wb_valid)
                  << " order_error=" << int(dut.commit_order_error) << "\n";
        std::exit(1);
    }
    dut.filtered_mb_valid = 0;
    tick(dut);

    for (int i = 0; i < 384; ++i) {
        dut.filtered_sample_valid = 1;
        tick(dut);
        if (dut.wb_valid || dut.ref_ready_pulse) {
            std::cerr << "FAIL deblock writeback: sample write asserted commit/ref_ready"
                      << " sample_i=" << i << " wb=" << int(dut.wb_valid)
                      << " ready=" << int(dut.ref_ready_pulse) << "\n";
            std::exit(1);
        }
    }
    dut.filtered_sample_valid = 0;
    dut.filtered_mb_valid = 1;
    dut.filtered_mb_addr = 17;
    dut.filtered_mb_is_ref = 1;
    dut.filtered_frame_done = 0;
    dut.frame_slot_i = 2;
    tick(dut);
    if (!dut.wb_valid || dut.wb_mb_addr != 17 || !dut.wb_is_ref || dut.ref_ready_pulse) {
        std::cerr << "FAIL deblock writeback: nonterminal writeback/ready wb=" << int(dut.wb_valid)
                  << " addr=" << int(dut.wb_mb_addr) << " ref=" << int(dut.wb_is_ref)
                  << " ready=" << int(dut.ref_ready_pulse) << "\n";
        std::exit(1);
    }
    dut.filtered_mb_valid = 0;
    tick(dut);

    for (int i = 0; i < 384; ++i) {
        dut.filtered_sample_valid = 1;
        tick(dut);
        if (dut.wb_valid || dut.ref_ready_pulse) {
            std::cerr << "FAIL deblock writeback: terminal sample write asserted commit/ref_ready"
                      << " sample_i=" << i << " wb=" << int(dut.wb_valid)
                      << " ready=" << int(dut.ref_ready_pulse) << "\n";
            std::exit(1);
        }
    }
    dut.filtered_sample_valid = 0;
    dut.filtered_mb_valid = 1;
    dut.filtered_mb_addr = 1169;
    dut.filtered_frame_done = 1;
    tick(dut);
    if (!dut.wb_valid || dut.wb_mb_addr != 1169 || dut.ref_ready_pulse) {
        std::cerr << "FAIL deblock writeback: DPB ref ready before frame boundary"
                  << " wb=" << int(dut.wb_valid) << " addr=" << int(dut.wb_mb_addr)
                  << " ready=" << int(dut.ref_ready_pulse) << "\n";
        std::exit(1);
    }

    dut.filtered_mb_valid = 0;
    dut.filtered_frame_done = 0;
    dut.frame_boundary = 0;
    tick(dut);
    if (dut.ref_ready_pulse) {
        std::cerr << "FAIL deblock writeback: ref ready without frame boundary\n";
        std::exit(1);
    }

    dut.frame_boundary = 1;
    tick(dut);
    if (!dut.ref_ready_pulse || dut.ref_ready_slot != 2 || dut.wb_valid) {
        std::cerr << "FAIL deblock writeback: frame-boundary ref promotion got ready="
                  << int(dut.ref_ready_pulse) << " slot=" << int(dut.ref_ready_slot)
                  << " wb=" << int(dut.wb_valid) << "\n";
        std::exit(1);
    }
    dut.frame_boundary = 0;
    tick(dut);
    if (dut.ref_ready_pulse) {
        std::cerr << "FAIL deblock writeback: ref ready pulse did not clear\n";
        std::exit(1);
    }

    std::cout << "OK deblock writeback contract: filtered samples precede MB commit; writeback precedes frame-boundary DPB ref_ready; IDR invalidates refs\n";
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    bool faultHorizontalFirst = false;
    std::string mbGoldenPath;
    std::string nalSequencePath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fault-horizontal-first") faultHorizontalFirst = true;
        else if (arg == "--mb-golden" && i + 1 < argc) mbGoldenPath = argv[++i];
        else if (arg == "--nal-sequence" && i + 1 < argc) nalSequencePath = argv[++i];
        else { std::cerr << "usage: " << argv[0] << " [--mb-golden path] [--nal-sequence path] [--fault-horizontal-first]\n"; return 2; }
    }
    Vh264_deblock_tb dut;
    dut.clk = 0;
    dut.reset = 0;
    dut.pipe_valid_i = 0;
    dut.idr_frame_start = 0;
    dut.filtered_sample_valid = 0;
    dut.filtered_mb_valid = 0;
    dut.filtered_mb_addr = 0;
    dut.filtered_mb_is_ref = 0;
    dut.filtered_frame_done = 0;
    dut.frame_slot_i = 0;
    dut.frame_boundary = 0;

    testBs(dut);
    testThresholds(dut);
    testWritebackContract(dut);

    const EdgeIO lumaNormal{{116,118,120,122},{118,120,122,124},{120,122,124,126},{126,127,128,129},
                            {132,133,134,135},{138,139,140,141},{140,141,142,143},{142,143,144,145}};
    requireEdge(dut, "luma bS2 normal", lumaNormal, false, 2, 32, 0, 0);
    testPipeLatency(dut, lumaNormal);

    const EdgeIO lumaStrong{{110,111,112,113},{112,113,114,115},{114,115,116,117},{120,121,122,123},
                            {124,125,126,127},{128,129,130,131},{130,131,132,133},{132,133,134,135}};
    requireEdge(dut, "luma bS4 strong", lumaStrong, false, 4, 40, 0, 0);

    const EdgeIO chromaNormal{{118,119,120,121},{120,121,122,123},{122,123,124,125},{125,126,127,128},
                              {131,132,133,134},{136,137,138,139},{138,139,140,141},{140,141,142,143}};
    requireEdge(dut, "chroma bS2", chromaNormal, true, 2, 32, 0, 0);
    requireEdge(dut, "chroma bS4", chromaNormal, true, 4, 40, 0, 0);

    if (!mbGoldenPath.empty()) runMbGolden(dut, mbGoldenPath);
    if (!nalSequencePath.empty()) {
        if (mbGoldenPath.empty()) {
            std::cerr << "FAIL deblock nal_sequence: --mb-golden is required with --nal-sequence\n";
            return 2;
        }
        runNalSequenceContract(nalSequencePath, mbGoldenPath);
    }
    runDrift(dut, faultHorizontalFirst);

    std::cout << "OK h264_deblock RTL sim: bS, threshold, luma, chroma, pipe-latency, writeback-DPB contract, mb_golden, edge-order drift\n";
    return 0;
}
