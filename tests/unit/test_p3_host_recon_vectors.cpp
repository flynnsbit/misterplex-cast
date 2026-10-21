// Phase 3.3l host golden vectors:
// - MB0 luma prediction/dequant/IDCT/recon JSON for RTL testbenches.
// - Whole-frame per-MB Y MAE CSV vs FFmpeg -skip_loop_filter all.
#include "libmisterplex/h264_recon.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

static std::string readText(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

template <typename T, size_t N>
static void jsonArray(std::ostringstream& os, const std::array<T, N>& a, int indent) {
    os << "[";
    for (size_t i = 0; i < N; ++i) {
        if (i)
            os << ",";
        if (i % 16 == 0)
            os << "\n" << std::string(static_cast<size_t>(indent), ' ');
        os << static_cast<int>(a[i]);
    }
    os << "\n" << std::string(static_cast<size_t>(indent - 2), ' ') << "]";
}

static std::string mb0Json(const misterplex::recon::ReconResult& rec,
                           const misterplex::recon::ReconTrace& trace) {
    const auto& mb = trace.mb;
    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": \"misterplex.p3.luma_mb.v1\",\n";
    os << "  \"source\": \"plex_real_baseline_320x240_1f.264\",\n";
    os << "  \"plane\": \"Y\",\n";
    os << "  \"frame\": {\"width\": " << rec.width << ", \"height\": " << rec.height
       << ", \"mb_width\": " << ((rec.width + 15) / 16)
       << ", \"mb_height\": " << ((rec.height + 15) / 16) << "},\n";
    os << "  \"macroblock\": {\"index\": " << mb.mb << ", \"x\": " << mb.mb_x
       << ", \"y\": " << mb.mb_y << ", \"type\": " << mb.mb_type << ", \"qp\": " << mb.qp
       << ", \"pred_mode\": " << mb.pred_mode << "},\n";
    os << "  \"pred_y\": ";
    jsonArray(os, mb.pred, 4);
    os << ",\n  \"recon_y\": ";
    jsonArray(os, mb.recon, 4);
    os << ",\n  \"blocks\": [\n";
    for (size_t b = 0; b < mb.blocks.size(); ++b) {
        const auto& blk = mb.blocks[b];
        if (b)
            os << ",\n";
        os << "    {\n";
        os << "      \"block\": " << blk.block << ", \"x\": " << blk.x << ", \"y\": " << blk.y
           << ", \"pred_mode\": " << blk.pred_mode << ", \"total_coeff\": "
           << blk.total_coeff << ",\n";
        os << "      \"pred\": ";
        jsonArray(os, blk.pred, 8);
        os << ",\n      \"dequant\": ";
        jsonArray(os, blk.dequant, 8);
        os << ",\n      \"idct\": ";
        jsonArray(os, blk.idct, 8);
        os << ",\n      \"recon\": ";
        jsonArray(os, blk.recon, 8);
        os << "\n    }";
    }
    os << "\n  ]\n";
    os << "}\n";
    return os.str();
}

static bool ffmpegGold(const std::string& h264, const std::string& yuvOut) {
    std::string cmd = "ffmpeg -y -hide_banner -loglevel error -skip_loop_filter all -i '" + h264 +
                      "' -pix_fmt yuv420p -vframes 1 '" + yuvOut + "'";
    return std::system(cmd.c_str()) == 0;
}

static std::string frameMaeCsv(const misterplex::recon::ReconResult& rec,
                               const std::vector<uint8_t>& gold) {
    const int mbW = (rec.width + 15) / 16;
    const int mbH = (rec.height + 15) / 16;
    std::ostringstream os;
    os << "format,misterplex.p3.frame_mae.v1\n";
    os << "source,plex_real_baseline_320x240_1f.264\n";
    os << "plane,Y\n";
    os << "width," << rec.width << "\n";
    os << "height," << rec.height << "\n";
    os << "mb_width," << mbW << "\n";
    os << "mb_height," << mbH << "\n";
    os << "mb,mb_x,mb_y,sum_abs_y,pixels,mae_y,max_abs_y\n";
    os << std::fixed << std::setprecision(6);
    for (int mby = 0; mby < mbH; ++mby) {
        for (int mbx = 0; mbx < mbW; ++mbx) {
            int64_t sum = 0;
            int maxAbs = 0;
            int pixels = 0;
            for (int yy = 0; yy < 16; ++yy) {
                int y = mby * 16 + yy;
                if (y >= rec.height)
                    continue;
                for (int xx = 0; xx < 16; ++xx) {
                    int x = mbx * 16 + xx;
                    if (x >= rec.width)
                        continue;
                    size_t idx = static_cast<size_t>(y * rec.width + x);
                    int d = std::abs(static_cast<int>(rec.y[idx]) - static_cast<int>(gold[idx]));
                    sum += d;
                    maxAbs = std::max(maxAbs, d);
                    ++pixels;
                }
            }
            double mae = pixels ? static_cast<double>(sum) / static_cast<double>(pixels) : 0.0;
            os << (mby * mbW + mbx) << "," << mbx << "," << mby << "," << sum << ","
               << pixels << "," << mae << "," << maxAbs << "\n";
        }
    }
    return os.str();
}

static bool sameOrUpdate(const std::string& fixture, const std::string& generated, bool update) {
    if (update) {
        writeText(fixture, generated);
        return true;
    }
    const std::string expected = readText(fixture);
    if (expected != generated) {
        writeText("build/" + fixture.substr(fixture.find_last_of('/') + 1) + ".actual", generated);
        std::printf("FAIL: fixture mismatch %s (actual saved under build/)\n", fixture.c_str());
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string(argv[1]) == "--write-fixtures";
    const std::string h264 = "tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264";
    const std::string mbFixture = "tests/fixtures/p3_host_recon/mb0_luma_v1.json";
    const std::string maeFixture = "tests/fixtures/p3_host_recon/frame_mae_v1.csv";
    auto blob = readFile(h264);
    if (blob.empty()) {
        std::printf("FAIL: missing checked-in vector %s\n", h264.c_str());
        return 1;
    }
    if (blob.size() != 6739) {
        std::printf("FAIL: checked-in vector size=%zu expected 6739\n", blob.size());
        return 1;
    }

    misterplex::recon::ReconTrace trace;
    trace.target_mb = 0;
    auto rec = misterplex::recon::reconISlice(blob.data(), blob.size(), &trace);
    if (rec.mb_decoded != rec.mb_total || !trace.mb.valid || rec.y.empty()) {
        std::printf("FAIL: recon mb=%d/%d trace=%d reason=%s\n", rec.mb_decoded, rec.mb_total,
                    trace.mb.valid ? 1 : 0, rec.fail_reason ? rec.fail_reason : "?");
        return 1;
    }

    const std::string goldPath = "build/p3_host_recon_gold.yuv";
    if (!ffmpegGold(h264, goldPath)) {
        std::printf("FAIL: ffmpeg gold generation failed\n");
        return 1;
    }
    auto gold = readFile(goldPath);
    if (gold.size() < rec.y.size()) {
        std::printf("FAIL: gold too small %zu need %zu\n", gold.size(), rec.y.size());
        return 1;
    }

    const double maeY = misterplex::recon::maeY(rec, gold.data(), rec.y.size());
    if (maeY != 0.0) {
        std::printf("FAIL: frame maeY %.6f expected 0\n", maeY);
        return 1;
    }

    const std::string mbJson = mb0Json(rec, trace);
    const std::string maeCsv = frameMaeCsv(rec, gold);
    writeText("build/p3_host_recon_mb0_luma_v1.json", mbJson);
    writeText("build/p3_host_recon_frame_mae_v1.csv", maeCsv);

    if (!sameOrUpdate(mbFixture, mbJson, update) || !sameOrUpdate(maeFixture, maeCsv, update))
        return 1;

    std::printf("test_p3_host_recon_vectors: OK vector_bytes=%zu mb=%d/%d frame=%dx%d "
                "maeY=%.6f mb0_json=build/p3_host_recon_mb0_luma_v1.json "
                "frame_mae=build/p3_host_recon_frame_mae_v1.csv\n",
                blob.size(), rec.mb_decoded, rec.mb_total, rec.width, rec.height, maeY);
    return 0;
}
