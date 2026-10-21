// Unit: CAVLC residual + I-slice walk + recon (Phase 3.3f/g/h).
// Against real Baseline: require full walk + bit-exact YUV vs FFmpeg -skip_loop_filter.
#include "libmisterplex/h264_cavlc.hpp"
#include "libmisterplex/h264_recon.hpp"
#include "libmisterplex/h264_slice_walk.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

static bool ffmpegGold(const char* h264, const char* yuvOut) {
    // No deblock: host recon does not implement loop filter yet.
    std::string cmd = std::string("ffmpeg -y -hide_banner -loglevel error ") +
                      "-skip_loop_filter all -i '" + h264 +
                      "' -pix_fmt yuv420p -vframes 1 '" + yuvOut + "'";
    return std::system(cmd.c_str()) == 0;
}

int main(int argc, char** argv) {


    const char* path = argc > 1 ? argv[1] : "build/plex_real_baseline.264";
    auto blob = readFile(path);
    if (blob.empty()) {
        if (std::system("python3 scripts/gen_test_annexb_real.py build/plex_real_baseline.264") !=


            0) {
            std::printf("FAIL: no bitstream\n");
            return 1;
        }


        blob = readFile("build/plex_real_baseline.264");
        path = "build/plex_real_baseline.264";


    }

    // First residual probe (I_NxN first coded 4x4 or I16 DC)
    auto r = misterplex::cavlc::probeFirstI16Dc(blob.data(), blob.size());
    if (!r.ok || r.total_coeff < 0 || r.total_coeff > 16) {
        std::printf("FAIL: residual probe ok=%d tc=%d\n", r.ok, r.total_coeff);
        return 1;
    }

    // Full I-slice residual walk (3.3h) — require complete slice
    auto w = misterplex::walkISliceResiduals(blob.data(), blob.size());
    if (w.mb_decoded != w.mb_total || w.mb_total <= 0) {
        std::printf("FAIL: walk mb_decoded=%d/%d fail_mb=%d reason=%s\n", w.mb_decoded, w.mb_total,
                    w.fail_mb, w.fail_reason ? w.fail_reason : "?");
        return 1;
    }

    // Reconstruct I-slice → YUV; RGB565 smoke
    auto rec = misterplex::recon::reconISlice(blob.data(), blob.size());
    if (rec.mb_decoded != rec.mb_total || rec.y.empty()) {
        std::printf("FAIL: recon mb=%d/%d reason=%s\n", rec.mb_decoded, rec.mb_total,
                    rec.fail_reason ? rec.fail_reason : "?");
        return 1;
    }
    std::vector<uint16_t> rgb;
    misterplex::recon::yuv420ToRgb565(rec.y.data(), rec.u.data(), rec.v.data(), rec.width,
                                      rec.height, rgb);
    if (rgb.size() != static_cast<size_t>(rec.width * rec.height)) {
        std::printf("FAIL: rgb565 size\n");
        return 1;
    }

    // Bit-exact vs FFmpeg (no loop filter) when ffmpeg is available.
    double maeY = -1.0, maeU = -1.0, maeV = -1.0;
    const char* goldPath = "build/misterplex_cavlc_gold.yuv";
    if (ffmpegGold(path, goldPath)) {
        auto gold = readFile(goldPath);
        const size_t ysz = rec.y.size();
        const size_t usz = rec.u.size();
        if (gold.size() < ysz + 2 * usz) {
            std::printf("FAIL: gold yuv too small %zu need %zu\n", gold.size(), ysz + 2 * usz);
            return 1;
        }
        maeY = misterplex::recon::maeY(rec, gold.data(), ysz);
        double su = 0, sv = 0;
        const uint8_t* gU = gold.data() + ysz;
        const uint8_t* gV = gU + usz;
        for (size_t i = 0; i < usz; ++i) {
            su += std::abs(static_cast<int>(rec.u[i]) - static_cast<int>(gU[i]));
            sv += std::abs(static_cast<int>(rec.v[i]) - static_cast<int>(gV[i]));
        }
        maeU = su / static_cast<double>(usz);
        maeV = sv / static_cast<double>(usz);
        // Real Baseline + synthetic clips must be exact without deblock.
        if (maeY > 1e-9 || maeU > 1e-9 || maeV > 1e-9) {
            std::printf("FAIL: recon vs FFmpeg maeY=%.6f maeU=%.6f maeV=%.6f (want 0)\n", maeY,
                        maeU, maeV);
            return 1;
        }
    }

    std::printf("test_cavlc_dc: OK probe_tc=%d t1=%d walk=%d/%d FULL recon=%dx%d rgb=%zu "
                "maeY=%.4f maeU=%.4f maeV=%.4f\n",
                r.total_coeff, r.trailing_ones, w.mb_decoded, w.mb_total, rec.width, rec.height,
                rgb.size(), maeY, maeU, maeV);
    return 0;
}
