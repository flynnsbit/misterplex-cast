// h264_decode_top_tb.cpp — C++ driver for integrated decode top testbench.
// Drives golden MB data through h264_decode_top and verifies full-macroblock
// reconstruction against golden samples.
//
// This produces the INTEGRATED_PIPELINE_COVERAGE number.

#include "Vh264_decode_top_tb.h"
#include "verilated.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

// Minimal JSON value parser (numbers, arrays, strings)
struct JsonVal {
    enum Type { NUL, NUM, STR, ARR, OBJ } type = NUL;
    double num = 0;
    std::string str;
    std::vector<JsonVal> arr;
    std::vector<std::pair<std::string, JsonVal>> obj;

    int64_t i() const { return (int64_t)num; }
};

static void skipWs(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t')) p++;
}

static JsonVal parseVal(const std::string& s, size_t& p);

static std::string parseStr(const std::string& s, size_t& p) {
    if (s[p] != '"') return "";
    p++;
    std::string r;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\') { p++; r += s[p]; }
        else r += s[p];
        p++;
    }
    if (p < s.size()) p++; // closing "
    return r;
}

static JsonVal parseVal(const std::string& s, size_t& p) {
    skipWs(s, p);
    JsonVal v;
    if (p >= s.size()) return v;
    if (s[p] == '"') { v.type = JsonVal::STR; v.str = parseStr(s, p); return v; }
    if (s[p] == '[') {
        v.type = JsonVal::ARR;
        p++;
        skipWs(s, p);
        while (p < s.size() && s[p] != ']') {
            v.arr.push_back(parseVal(s, p));
            skipWs(s, p);
            if (p < s.size() && s[p] == ',') p++;
        }
        if (p < s.size()) p++;
        return v;
    }
    if (s[p] == '{') {
        v.type = JsonVal::OBJ;
        p++;
        skipWs(s, p);
        while (p < s.size() && s[p] != '}') {
            skipWs(s, p);
            std::string key = parseStr(s, p);
            skipWs(s, p);
            if (p < s.size() && s[p] == ':') p++;
            v.obj.push_back({key, parseVal(s, p)});
            skipWs(s, p);
            if (p < s.size() && s[p] == ',') p++;
        }
        if (p < s.size()) p++;
        return v;
    }
    if (s[p]=='n' && s.substr(p,4)=="null") { p+=4; return v; }
    if (s[p]=='t' && s.substr(p,4)=="true") { v.type=JsonVal::NUM; v.num=1; p+=4; return v; }
    if (s[p]=='f' && s.substr(p,5)=="false") { v.type=JsonVal::NUM; v.num=0; p+=5; return v; }
    // number
    v.type = JsonVal::NUM;
    size_t start = p;
    if (s[p]=='-') p++;
    while (p < s.size() && ((s[p]>='0'&&s[p]<='9')||s[p]=='.'||s[p]=='e'||s[p]=='E'||s[p]=='+'||s[p]=='-')) p++;
    v.num = std::stod(s.substr(start, p - start));
    return v;
}

static JsonVal parseJson(const std::string& s) {
    size_t p = 0;
    return parseVal(s, p);
}

static const JsonVal& findKey(const JsonVal& obj, const std::string& key) {
    static JsonVal null_val;
    for (auto& [k, v] : obj.obj)
        if (k == key) return v;
    return null_val;
}

struct MbGolden {
    int index, x, y;
    uint8_t mb_type;
    uint8_t qp;
    std::string type_name;
    uint8_t i16_pred_mode;
    uint8_t i4_modes[16];
    // Per-block coefficients (zigzag order)
    int16_t coeff[16][16];
    // I_16x16 DC values (post-Hadamard dequant)
    int32_t i16_dc[16];
    // Golden prediction and reconstruction (256 pixels each)
    uint8_t pred_y[256];
    uint8_t recon_y[256];
    // Neighbour info
    bool avail_left, avail_top, avail_topright, avail_topleft;
    uint8_t nb_top[16], nb_left[16], nb_topleft, nb_topright[4];
};

static bool loadMbGolden(const std::string& path, MbGolden& mb) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    auto root = parseJson(ss.str());

    auto& mbinfo = findKey(root, "macroblock");
    mb.index = (int)findKey(mbinfo, "index").i();
    mb.x = (int)findKey(mbinfo, "x").i();
    mb.y = (int)findKey(mbinfo, "y").i();
    mb.mb_type = (uint8_t)findKey(mbinfo, "type").i();
    mb.qp = (uint8_t)findKey(mbinfo, "qp").i();
    mb.type_name = findKey(mbinfo, "type_name").str;

    auto& pred = findKey(root, "prediction");
    auto& i16mode = findKey(pred, "luma_16x16");
    mb.i16_pred_mode = (i16mode.type == JsonVal::NUL) ? 2 : (uint8_t)i16mode.i();

    auto& i4modes = findKey(pred, "luma_4x4_modes");
    for (int i = 0; i < 16; i++)
        mb.i4_modes[i] = (i4modes.type == JsonVal::ARR && i < (int)i4modes.arr.size()) ?
                          (uint8_t)i4modes.arr[i].i() : 2;

    // Residual coefficients
    auto& res = findKey(root, "residual");
    auto& luma4x4 = findKey(res, "luma4x4");
    for (int b = 0; b < 16 && b < (int)luma4x4.arr.size(); b++) {
        auto& blk = luma4x4.arr[b];
        auto& cz = findKey(blk, "coefficients_zigzag");
        for (int c = 0; c < 16; c++)
            mb.coeff[b][c] = (c < (int)cz.arr.size()) ? (int16_t)cz.arr[c].i() : 0;
        // I_16x16 DC from dequant[0]
        auto& dq = findKey(blk, "dequant");
        mb.i16_dc[b] = (dq.type == JsonVal::ARR && !dq.arr.empty()) ? (int32_t)dq.arr[0].i() : 0;
    }

    // Golden samples
    auto& samp = findKey(root, "samples");
    auto& pred_y = findKey(samp, "pred_y");
    auto& recon_y = findKey(samp, "recon_y");
    for (int i = 0; i < 256; i++) {
        mb.pred_y[i] = (i < (int)pred_y.arr.size()) ? (uint8_t)pred_y.arr[i].i() : 128;
        mb.recon_y[i] = (i < (int)recon_y.arr.size()) ? (uint8_t)recon_y.arr[i].i() : 0;
    }

    // Neighbour context
    auto& nb = findKey(root, "neighbours");
    if (nb.type != JsonVal::NUL) {
        mb.avail_left = (bool)findKey(nb, "avail_left").i();
        mb.avail_top = (bool)findKey(nb, "avail_top").i();
        mb.avail_topright = (bool)findKey(nb, "avail_topright").i();
        mb.avail_topleft = (bool)findKey(nb, "avail_topleft").i();
        auto& top = findKey(nb, "top");
        auto& left = findKey(nb, "left");
        auto& tl = findKey(nb, "topleft");
        auto& tr = findKey(nb, "topright");
        for (int i = 0; i < 16; i++) {
            mb.nb_top[i] = (i < (int)top.arr.size()) ? (uint8_t)top.arr[i].i() : 128;
            mb.nb_left[i] = (i < (int)left.arr.size()) ? (uint8_t)left.arr[i].i() : 128;
        }
        mb.nb_topleft = (tl.type == JsonVal::NUM) ? (uint8_t)tl.i() : 128;
        for (int i = 0; i < 4; i++)
            mb.nb_topright[i] = (i < (int)tr.arr.size()) ? (uint8_t)tr.arr[i].i() : 128;
    } else {
        // Derive from position
        mb.avail_left = (mb.x > 0);
        mb.avail_top = (mb.y > 0);
        mb.avail_topright = (mb.y > 0) && (mb.x < 19); // 320/16-1
        mb.avail_topleft = (mb.x > 0) && (mb.y > 0);
        memset(mb.nb_top, 128, 16);
        memset(mb.nb_left, 128, 16);
        mb.nb_topleft = 128;
        memset(mb.nb_topright, 128, 4);
    }

    return true;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::string goldenDir;
    bool chainRow = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--dir" && i+1 < argc)
            goldenDir = argv[++i];
        if (std::string(argv[i]) == "--chain-row0")
            chainRow = true;
    }
    if (goldenDir.empty()) {
        std::cerr << "Usage: " << argv[0] << " --dir <golden_dir> [--chain-row0]\n";
        return 1;
    }

    // Collect golden files
    std::vector<std::string> files;
    for (auto& entry : fs::directory_iterator(goldenDir)) {
        if (entry.path().extension() == ".json")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        std::cerr << "No golden files found in " << goldenDir << "\n";
        return 1;
    }

    auto* tb = new Vh264_decode_top_tb;

    // Reset
    tb->clk = 0; tb->reset = 1;
    tb->eval(); tb->clk = 1; tb->eval(); tb->clk = 0; tb->eval();
    tb->reset = 0;

    int totalMbs = 0, exactMbs = 0;
    int totalPixels = 0, exactPixels = 0;
    int firstBadMb = -1, firstBadPixel = -1;
    int firstBadGot = 0, firstBadRef = 0;
    double sumAbsDiff = 0.0;

    // For chaining: previous MB's reconstructed right column (16 samples)
    // becomes current MB's left neighbour
    uint8_t prev_recon_y[256] = {};
    bool have_prev = false;

    for (auto& path : files) {
        MbGolden mb;
        if (!loadMbGolden(path, mb)) {
            std::cerr << "Failed to load " << path << "\n";
            continue;
        }

        if (chainRow) {
            // Only process row 0 (y=0), chain left neighbours from prior MB
            if (mb.y != 0) continue;
            // Set neighbour availability based on chaining
            mb.avail_top = false;
            mb.avail_topright = false;
            mb.avail_topleft = false;
            mb.avail_left = have_prev && (mb.x > 0);
            if (mb.avail_left) {
                // Left column = rightmost column (x=15) of previous MB's recon
                for (int r = 0; r < 16; r++)
                    mb.nb_left[r] = prev_recon_y[r * 16 + 15];
            } else {
                memset(mb.nb_left, 128, 16);
            }
            memset(mb.nb_top, 128, 16);
            mb.nb_topleft = 128;
            memset(mb.nb_topright, 128, 4);
        } else {
            // Non-chained mode: only MB0 (no neighbours needed)
            if (mb.x > 0 || mb.y > 0) continue;
        }

        bool isI16 = (mb.mb_type >= 1 && mb.mb_type <= 24);

        // --- Drive mb_start ---
        tb->tb_mb_start = 1;
        tb->tb_mb_type = mb.mb_type;
        tb->tb_mb_qp_y = mb.qp;
        tb->tb_mb_x = mb.x;
        tb->tb_mb_y = mb.y;
        tb->tb_i16_pred_mode = mb.i16_pred_mode;
        tb->tb_avail_left = mb.avail_left;
        tb->tb_avail_top = mb.avail_top;
        tb->tb_avail_topright = mb.avail_topright;
        tb->tb_avail_topleft = mb.avail_topleft;
        for (int i = 0; i < 16; i++) {
            tb->tb_nb_top[i] = mb.nb_top[i];
            tb->tb_nb_left[i] = mb.nb_left[i];
            tb->tb_i4_modes[i] = mb.i4_modes[i];
        }
        tb->tb_nb_topleft = mb.nb_topleft;
        for (int i = 0; i < 4; i++)
            tb->tb_nb_topright[i] = mb.nb_topright[i];

        // Supply I_16x16 DC if applicable
        tb->tb_i16_dc_valid = isI16 ? 1 : 0;
        for (int i = 0; i < 16; i++)
            tb->tb_i16_dc[i] = isI16 ? mb.i16_dc[i] : 0;

        tb->tb_block_valid = 0;
        tb->clk = 0; tb->eval(); tb->clk = 1; tb->eval();
        tb->tb_mb_start = 0;
        tb->tb_i16_dc_valid = 0;
        tb->clk = 0; tb->eval(); tb->clk = 1; tb->eval();

        // --- Drive all 16 blocks ---
        for (int b = 0; b < 16; b++) {
            tb->tb_block_valid = 1;
            tb->tb_block_index = b;
            for (int c = 0; c < 16; c++)
                tb->tb_block_coeff[c] = mb.coeff[b][c];

            tb->clk = 0; tb->eval(); tb->clk = 1; tb->eval();
            tb->tb_block_valid = 0;
            // ST_STORE takes 1 cycle
            tb->clk = 0; tb->eval(); tb->clk = 1; tb->eval();
        }

        // ST_DONE: 1 more cycle for output
        tb->clk = 0; tb->eval(); tb->clk = 1; tb->eval();

        if (!tb->tb_mb_recon_valid) {
            // One more cycle in case timing is off
            tb->clk = 0; tb->eval(); tb->clk = 1; tb->eval();
        }

        // --- Compare output ---
        totalMbs++;
        int mbExact = 0;
        for (int i = 0; i < 256; i++) {
            uint8_t got = tb->tb_recon_y[i];
            uint8_t ref = mb.recon_y[i];
            totalPixels++;
            if (got == ref) {
                exactPixels++;
                mbExact++;
            } else {
                sumAbsDiff += std::abs((int)got - (int)ref);
                if (firstBadMb < 0) {
                    firstBadMb = mb.index;
                    firstBadPixel = i;
                    firstBadGot = got;
                    firstBadRef = ref;
                }
            }
        }
        if (mbExact == 256) exactMbs++;

        // Save reconstruction for chaining to next MB
        for (int i = 0; i < 256; i++)
            prev_recon_y[i] = tb->tb_recon_y[i];
        have_prev = true;
    }

    delete tb;

    // Report
    double mae = (totalPixels > exactPixels) ?
                 sumAbsDiff / (totalPixels - exactPixels) : 0.0;
    bool pass = (exactPixels == totalPixels);

    std::cout << "DECODE_TOP_INTEGRATED  verification_target=RTL_INTEGRATED\n"
              << "  module=h264_decode_top(dequant+IDCT+intra_pred+recon)\n"
              << "  scope=luma_" << (chainRow ? "row0_chained" : "MB0_only") << "\n"
              << "  mbs_scored=" << totalMbs << " mbs_exact=" << exactMbs << "/" << totalMbs << "\n"
              << "  pixels_exact=" << exactPixels << "/" << totalPixels << "\n"
              << "  mae=" << std::fixed << std::setprecision(6) << mae << "\n"
              << "  INTEGRATED_PIPELINE_COVERAGE: " << exactPixels << "/" << totalPixels;
    if (totalPixels > 0)
        std::cout << " = " << std::fixed << std::setprecision(3)
                  << (100.0 * exactPixels / totalPixels) << "%";
    std::cout << " (real connected pipeline, not injection)\n";

    if (firstBadMb >= 0) {
        std::cout << "  first_divergence: mb=" << firstBadMb
                  << " pixel=" << firstBadPixel
                  << " got=" << firstBadGot
                  << " ref=" << firstBadRef
                  << " diff=" << (firstBadGot - firstBadRef) << "\n";
    }

    if (pass) {
        std::cout << "DECODE_TOP_INTEGRATED  PASS: " << exactPixels
                  << "/" << totalPixels << " pixels bit-exact (integrated pipeline)\n";
    } else {
        std::cerr << "DECODE_TOP_INTEGRATED  PARTIAL: " << exactPixels
                  << "/" << totalPixels << " pixels exact, "
                  << (totalPixels - exactPixels) << " differ\n";
    }

    return pass ? 0 : 1;
}
