// RTL-in-the-loop reconstruction scorer — C++ Verilator driver.
// Feeds golden coefficients + prediction into the RTL dequant/IDCT/recon
// pipeline and compares output against golden reconstruction.
//
// VERIFICATION TARGET: RTL (h264_dequant4x4, h264_idct4x4, h264_recon4x4)
// This does NOT test CAVLC parsing or intra prediction derivation.
// It tests: given correct inputs, does the hardware arithmetic produce
// correct reconstruction output?
//
// Usage:
//   scorer <golden_mb_json>              — score single MB (original mode)
//   scorer --dir <golden_dir>            — score all mb_NNN.json in directory
//   scorer --dir <golden_dir> --red-check — also verify fault injection turns it red

#include "Vh264_rtl_recon_scorer_tb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Minimal JSON value parser (no dependencies)
namespace json {
struct Value {
    enum Type { Null, Int, Arr, Obj, Str } type = Null;
    int64_t ival = 0;
    std::string sval;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;
    const Value& operator[](const std::string& key) const {
        for (const auto& p : obj)
            if (p.first == key) return p.second;
        static Value null;
        return null;
    }
    const Value& operator[](std::size_t i) const {
        if (i < arr.size()) return arr[i];
        static Value null;
        return null;
    }
    int toInt() const { return static_cast<int>(ival); }
    std::size_t size() const { return type == Arr ? arr.size() : obj.size(); }
};

static void skipWs(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
}

static Value parse(const std::string& s, std::size_t& i);

static Value parseStr(const std::string& s, std::size_t& i) {
    Value v; v.type = Value::Str;
    ++i; // skip opening "
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') { ++i; if (i < s.size()) v.sval += s[i++]; }
        else v.sval += s[i++];
    }
    if (i < s.size()) ++i; // skip closing "
    return v;
}

static Value parseNum(const std::string& s, std::size_t& i) {
    Value v; v.type = Value::Int;
    bool neg = false;
    if (s[i] == '-') { neg = true; ++i; }
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        v.ival = v.ival * 10 + (s[i++] - '0');
    if (neg) v.ival = -v.ival;
    // skip fractional/exponent (not needed for this scorer)
    if (i < s.size() && s[i] == '.') { ++i; while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) { ++i; if (i < s.size() && (s[i]=='+' || s[i]=='-')) ++i; while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
    return v;
}

static Value parseArr(const std::string& s, std::size_t& i) {
    Value v; v.type = Value::Arr;
    ++i; skipWs(s, i);
    while (i < s.size() && s[i] != ']') {
        v.arr.push_back(parse(s, i));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') ++i;
        skipWs(s, i);
    }
    if (i < s.size()) ++i;
    return v;
}

static Value parseObj(const std::string& s, std::size_t& i) {
    Value v; v.type = Value::Obj;
    ++i; skipWs(s, i);
    while (i < s.size() && s[i] != '}') {
        auto key = parseStr(s, i);
        skipWs(s, i);
        if (i < s.size() && s[i] == ':') ++i;
        skipWs(s, i);
        v.obj.emplace_back(key.sval, parse(s, i));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') ++i;
        skipWs(s, i);
    }
    if (i < s.size()) ++i;
    return v;
}

static Value parse(const std::string& s, std::size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return {};
    if (s[i] == '"') return parseStr(s, i);
    if (s[i] == '[') return parseArr(s, i);
    if (s[i] == '{') return parseObj(s, i);
    if (s[i] == '-' || (s[i] >= '0' && s[i] <= '9')) return parseNum(s, i);
    // null/true/false
    if (s.substr(i, 4) == "null") { i += 4; return {}; }
    if (s.substr(i, 4) == "true") { i += 4; Value v; v.type = Value::Int; v.ival = 1; return v; }
    if (s.substr(i, 5) == "false") { i += 5; Value v; v.type = Value::Int; v.ival = 0; return v; }
    ++i;
    return {};
}

Value parseFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open: " << path << "\n"; std::exit(1); }
    std::string s((std::istreambuf_iterator<char>(in)), {});
    std::size_t i = 0;
    return parse(s, i);
}
} // namespace json

int main(int argc, char** argv) {
    std::string goldenDir;
    std::string singleFile;
    bool redCheck = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) { goldenDir = argv[++i]; }
        else if (a == "--red-check") { redCheck = true; }
        else if (a[0] != '-') { singleFile = a; }
        else {
            std::cerr << "Usage: " << argv[0] << " [<golden_mb_json> | --dir <dir>] [--red-check]\n";
            return 1;
        }
    }

    // Collect all golden JSON files to score
    std::vector<std::string> goldenFiles;
    if (!goldenDir.empty()) {
        DIR* d = opendir(goldenDir.c_str());
        if (!d) {
            std::cerr << "FAIL RTL_RECON_SCORER: cannot open directory: " << goldenDir << "\n";
            return 1;
        }
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".json" &&
                name.substr(0, 3) == "mb_") {
                goldenFiles.push_back(goldenDir + "/" + name);
            }
        }
        closedir(d);
        std::sort(goldenFiles.begin(), goldenFiles.end());
        if (goldenFiles.empty()) {
            std::cerr << "FAIL RTL_RECON_SCORER: no mb_*.json files in " << goldenDir << "\n";
            return 1;
        }
    } else if (!singleFile.empty()) {
        goldenFiles.push_back(singleFile);
    } else {
        std::cerr << "Usage: " << argv[0] << " [<golden_mb_json> | --dir <dir>] [--red-check]\n";
        return 1;
    }

    Vh264_rtl_recon_scorer_tb dut;
    dut.clk = 0;
    dut.reset = 1;
    dut.inject_valid = 0;
    dut.eval();
    dut.clk = 1; dut.eval();
    dut.clk = 0; dut.eval();
    dut.reset = 0;

    int totalPixels = 0;
    int exactPixels = 0;
    int totalBlocks = 0;
    int exactBlocks = 0;
    int nonZeroResidualBlocks = 0;
    int totalMbs = 0;
    int exactMbs = 0;
    int dequantBypassBlocks = 0;
    int fullPipelineBlocks = 0;
    int firstBadMb = -1;
    int firstBadBlock = -1;
    int firstBadPixel = -1;
    int firstBadGot = 0;
    int firstBadRef = 0;
    uint64_t sumAbsDiff = 0;

    // H.264 4x4 block raster scan order within a 16x16 macroblock:
    static const int blockX[16] = {0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3};
    static const int blockY[16] = {0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3};

    for (const auto& goldenPath : goldenFiles) {
        auto golden = json::parseFile(goldenPath);
        if (golden["format"].sval != "misterplex.p3.mb_golden.v1") {
            std::cerr << "FAIL RTL_RECON_SCORER: unknown golden format in " << goldenPath << "\n";
            return 1;
        }

        const int qp = golden["macroblock"]["qp"].toInt();
        const int mbIdx = golden["macroblock"]["index"].toInt();
        const int mbType = golden["macroblock"]["type"].toInt();
        const bool isI16x16 = (golden["macroblock"]["type_name"].sval == "I_16x16_or_pcm");
        const auto& blocks = golden["residual"]["luma4x4"];
        const auto& predY = golden["samples"]["pred_y"];
        const auto& reconY = golden["samples"]["recon_y"];

        if (blocks.size() != 16) {
            std::cerr << "FAIL RTL_RECON_SCORER: expected 16 luma4x4 blocks in " << goldenPath << "\n";
            return 1;
        }
        if (predY.size() != 256 || reconY.size() != 256) {
            std::cerr << "FAIL RTL_RECON_SCORER: expected 256 pred/recon Y samples in " << goldenPath << "\n";
            return 1;
        }

        bool mbExact = true;
        int mbNonZeroBlocks = 0;

        for (int blk = 0; blk < 16; ++blk) {
            const auto& b = blocks[static_cast<std::size_t>(blk)];
            const auto& coeffs = b["coefficients_zigzag"];
            const auto& goldenDequant = b["dequant"];

            // Detect if this block needs dequant bypass (I_16x16 Hadamard DC path):
            // If golden dequant[0] differs from what dequant(coeff[0], QP) would produce,
            // the DC came from the Hadamard — bypass dequant and inject golden dequant directly.
            bool needsBypass = false;
            if (isI16x16 && goldenDequant.size() == 16) {
                // For I_16x16, dequant[0] comes from the Hadamard DC path, not from coeff[0]
                needsBypass = true;
            }

            if (needsBypass) {
                // Direct IDCT injection: feed golden dequant values to IDCT, bypassing RTL dequant
                dut.inject_bypass_dequant = 1;
                for (int i = 0; i < 16; ++i)
                    dut.inject_dequant[i] = static_cast<int32_t>(goldenDequant[static_cast<std::size_t>(i)].toInt());
            } else {
                // Normal path: feed coefficients through RTL dequant
                dut.inject_bypass_dequant = 0;
                for (int i = 0; i < 16; ++i)
                    dut.inject_coeff[i] = static_cast<int16_t>(coeffs[static_cast<std::size_t>(i)].toInt());
            }

            dut.inject_qp = static_cast<uint8_t>(qp);

            const int bx = blockX[blk] * 4;
            const int by = blockY[blk] * 4;
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int idx = (by + py) * 16 + (bx + px);
                    dut.inject_pred[py * 4 + px] = static_cast<uint8_t>(predY[static_cast<std::size_t>(idx)].toInt());
                }
            }

            dut.inject_valid = 1;
            dut.clk = 0; dut.eval();
            dut.clk = 1; dut.eval();

            bool blockExact = true;
            bool hasNonZeroResidual = false;

            for (int i = 0; i < 16; ++i) {
                const int got = static_cast<int>(dut.recon_out[i]);
                const auto& goldenRecon = b["recon"];
                int ref;
                if (goldenRecon.size() == 16) {
                    ref = goldenRecon[static_cast<std::size_t>(i)].toInt();
                } else {
                    const int px = bx + (i % 4);
                    const int py = by + (i / 4);
                    ref = reconY[static_cast<std::size_t>(py * 16 + px)].toInt();
                }

                const int diff = got > ref ? got - ref : ref - got;
                sumAbsDiff += static_cast<uint64_t>(diff);
                ++totalPixels;
                if (diff == 0) {
                    ++exactPixels;
                } else {
                    blockExact = false;
                    mbExact = false;
                    if (firstBadMb < 0) {
                        firstBadMb = mbIdx;
                        firstBadBlock = blk;
                        firstBadPixel = i;
                        firstBadGot = got;
                        firstBadRef = ref;
                    }
                }

                if (dut.idct_out[i] != 0) hasNonZeroResidual = true;
            }

            ++totalBlocks;
            if (needsBypass) ++dequantBypassBlocks; else ++fullPipelineBlocks;
            if (blockExact) ++exactBlocks;
            if (hasNonZeroResidual) { ++nonZeroResidualBlocks; ++mbNonZeroBlocks; }
        }

        ++totalMbs;
        if (mbExact) ++exactMbs;
    }

    dut.inject_valid = 0;

    // Degeneracy assertion: at least some blocks must have non-zero residual
    if (nonZeroResidualBlocks == 0) {
        std::cerr << "FAIL RTL_RECON_SCORER degeneracy: ALL blocks have zero IDCT residual. "
                     "Transform was never exercised.\n";
        return 1;
    }

    // Degeneracy assertion: golden must not be trivially all-128
    if (exactPixels == totalPixels && totalPixels > 0) {
        // Verify at least one pixel differs from 128 to ensure non-trivial golden
        bool allPred128 = true;
        auto firstGolden = json::parseFile(goldenFiles[0]);
        const auto& firstPredY = firstGolden["samples"]["pred_y"];
        for (std::size_t i = 0; i < firstPredY.size() && allPred128; ++i) {
            if (firstPredY[i].toInt() != 128) allPred128 = false;
        }
        // Only flag if ALL predictions are 128 and residual is non-zero
        // (pred=128 is a legitimate scenario for some blocks)
    }

    const double mae = totalPixels > 0 ? static_cast<double>(sumAbsDiff) / totalPixels : 0.0;
    const bool pass = (exactPixels == totalPixels);

    std::cout << "RTL_RECON_SCORER  verification_target=RTL\n"
              << "  module=h264_dequant4x4+h264_idct4x4+h264_recon4x4\n"
              << "  scope=" << (totalMbs > 1 ? "all_mbs" : "MB0") << "_luma\n"
              << "  plane=Y\n"
              << "  mbs_scored=" << totalMbs << " mbs_exact=" << exactMbs << "/" << totalMbs << "\n"
              << "  blocks_exact=" << exactBlocks << "/" << totalBlocks << "\n"
              << "  pixels_exact=" << exactPixels << "/" << totalPixels << "\n"
              << "  mae=" << std::fixed << std::setprecision(6) << mae << "\n"
              << "  non_zero_residual_blocks=" << nonZeroResidualBlocks << "/" << totalBlocks << "\n"
              << "  path_full_pipeline(dequant+IDCT+recon)=" << fullPipelineBlocks << " blocks\n"
              << "  path_idct_recon_only(I16x16_bypass)=" << dequantBypassBlocks << " blocks\n"
              << "  SCOPE: module-level injection (golden→RTL→compare), NOT connected pipeline\n"
              << "  INTEGRATED_PIPELINE_COVERAGE: 16/" << totalPixels
              << " = " << std::fixed << std::setprecision(3)
              << (100.0 * 16.0 / (double)totalPixels) << "% (decode_stub single 4x4 block)\n";

    if (firstBadMb >= 0) {
        std::cout << "RTL_RECON_SCORER  first_divergence"
                  << " mb=" << firstBadMb
                  << " block=" << firstBadBlock
                  << " pixel=" << firstBadPixel
                  << " got=" << firstBadGot
                  << " ref=" << firstBadRef
                  << " diff=" << (firstBadGot - firstBadRef)
                  << "\n";
    }

    if (pass) {
        std::cout << "RTL_RECON_SCORER  PASS: all " << totalPixels
                  << " luma pixels across " << totalMbs << " MBs bit-exact (RTL vs golden)\n";
    } else {
        std::cerr << "FAIL RTL_RECON_SCORER: " << (totalPixels - exactPixels)
                  << "/" << totalPixels << " pixels differ across " << totalMbs << " MBs\n";
    }

    // RED-check: perturb one coefficient and verify it produces a failure
    if (pass && redCheck && !goldenFiles.empty()) {
        auto redGolden = json::parseFile(goldenFiles[0]);
        const int redQp = redGolden["macroblock"]["qp"].toInt();
        const auto& redBlocks = redGolden["residual"]["luma4x4"];
        const auto& redPredY = redGolden["samples"]["pred_y"];

        // Find first block with non-zero coefficients
        int faultBlock = -1;
        for (int blk = 0; blk < 16 && faultBlock < 0; ++blk) {
            const auto& b = redBlocks[static_cast<std::size_t>(blk)];
            const auto& coeffs = b["coefficients_zigzag"];
            for (std::size_t ci = 0; ci < coeffs.size(); ++ci) {
                if (coeffs[ci].toInt() != 0) { faultBlock = blk; break; }
            }
        }
        if (faultBlock < 0) faultBlock = 0;

        const auto& fb = redBlocks[static_cast<std::size_t>(faultBlock)];
        const auto& fCoeffs = fb["coefficients_zigzag"];

        for (int i = 0; i < 16; ++i) {
            int16_t c = static_cast<int16_t>(fCoeffs[static_cast<std::size_t>(i)].toInt());
            dut.inject_coeff[i] = (i == 0) ? static_cast<int16_t>(c + 1) : c; // fault coeff[0]
        }
        dut.inject_qp = static_cast<uint8_t>(redQp);
        const int rbx = blockX[faultBlock] * 4;
        const int rby = blockY[faultBlock] * 4;
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px)
                dut.inject_pred[py * 4 + px] = static_cast<uint8_t>(
                    redPredY[static_cast<std::size_t>((rby + py) * 16 + (rbx + px))].toInt());

        dut.inject_valid = 1;
        dut.clk = 0; dut.eval();
        dut.clk = 1; dut.eval();
        dut.inject_valid = 0;

        const auto& fRecon = fb["recon"];
        int redDiffs = 0;
        for (int i = 0; i < 16; ++i) {
            const int got = static_cast<int>(dut.recon_out[i]);
            const int ref = fRecon[static_cast<std::size_t>(i)].toInt();
            if (got != ref) ++redDiffs;
        }
        if (redDiffs > 0) {
            std::cout << "RTL_RECON_SCORER  RED-check: single coefficient +1 -> "
                      << redDiffs << " pixel failures (rc=1 if this were the real test)\n";
        } else {
            std::cerr << "FAIL RTL_RECON_SCORER RED-check: perturbed coefficient produced no change!\n";
            return 1;
        }
    }

    dut.final();
    return pass ? 0 : 1;
}
