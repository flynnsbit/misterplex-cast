// Phase 3.3l-2 prep reference model.
// This is intentionally not an RTL simulator: it checks that the checked-in RTL
// building-block file is present and simulator-friendly, then grades a C++
// inverse-quant/IDCT/recon reference model against stable host goldens. A real
// Verilator/Icarus testbench should consume the same fixtures and model.
#include "libmisterplex/h264_cavlc.hpp"
#include "libmisterplex/h264_recon.hpp"
#include "libmisterplex/h264_residual_gold.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace gold = misterplex::residual_gold;

struct BlockFixture {
    int block = -1;
    int x = 0;
    int y = 0;
    int totalCoeff = 0;
    std::array<int, 16> pred{};
    std::array<int, 16> dequant{};
    std::array<int, 16> idct{};
    std::array<int, 16> recon{};
};

struct MaeRow {
    int mb = -1;
    int mbX = -1;
    int mbY = -1;
    int64_t sumAbs = -1;
    int pixels = -1;
    double mae = -1.0;
    int maxAbs = -1;
};

static std::vector<uint8_t> readBinary(const std::string& path) {
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

static bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

static bool sourceGuards() {
    const std::string rtl = readText("fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv");
    const std::string slp = readText("fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv");
    const std::string qip = readText("fpga/Plex_MiSTer/files.qip");
    if (rtl.empty() || slp.empty() || qip.empty()) {
        std::printf("FAIL source: missing h264_iq_idct_4x4.sv, slice_hdr_parser.sv, or files.qip\n");
        return false;
    }
    if (!contains(qip, "rtl/h264_iq_idct_4x4.sv")) {
        std::printf("FAIL source: files.qip does not include h264_iq_idct_4x4.sv\n");
        return false;
    }
    const char* modules[] = {"module h264_dequant4x4", "module h264_idct4x4", "module h264_recon4x4"};
    for (const char* m : modules) {
        if (!contains(rtl, m)) {
            std::printf("FAIL source: missing %s\n", m);
            return false;
        }
    }
    const char* banned[] = {"altsyncram", "lpm_", "cyclonev", "altera_", "dcfifo"};
    for (const char* b : banned) {
        if (contains(rtl, b)) {
            std::printf("FAIL source: h264_iq_idct_4x4.sv uses vendor/non-standalone token %s\n", b);
            return false;
        }
    }
    if (!contains(slp, "residual_coeff [0:15]") || !contains(slp, "residual_place_pulse")) {
        std::printf("FAIL source: slice_hdr_parser.sv no longer exposes full residual handoff\n");
        return false;
    }
    return true;
}

static bool extractNamedArray(const std::string& text, const std::string& key, std::vector<int>& out) {
    const std::string quoted = "\"" + key + "\"";
    size_t p = text.find(quoted);
    if (p == std::string::npos)
        return false;
    p = text.find('[', p);
    if (p == std::string::npos)
        return false;
    int depth = 0;
    size_t end = p;
    for (; end < text.size(); ++end) {
        if (text[end] == '[')
            ++depth;
        else if (text[end] == ']') {
            --depth;
            if (depth == 0)
                break;
        }
    }
    if (end >= text.size())
        return false;
    out.clear();
    const std::string body = text.substr(p + 1, end - p - 1);
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && !(body[i] == '-' || std::isdigit(static_cast<unsigned char>(body[i]))))
            ++i;
        if (i >= body.size())
            break;
        char* ep = nullptr;
        long v = std::strtol(body.c_str() + i, &ep, 10);
        out.push_back(static_cast<int>(v));
        i = static_cast<size_t>(ep - body.c_str());
    }
    return true;
}

static bool fill16(const std::string& text, const char* key, std::array<int, 16>& dst) {
    std::vector<int> tmp;
    if (!extractNamedArray(text, key, tmp) || tmp.size() != 16) {
        std::printf("FAIL fixture: %s size=%zu\n", key, tmp.size());
        return false;
    }
    for (size_t i = 0; i < dst.size(); ++i)
        dst[i] = tmp[i];
    return true;
}

static bool loadMb0Blocks(std::vector<BlockFixture>& blocks) {
    const std::string json = readText("tests/fixtures/p3_host_recon/mb0_luma_v1.json");
    if (json.empty() || !contains(json, "\"format\": \"misterplex.p3.luma_mb.v1\"")) {
        std::printf("FAIL fixture: mb0_luma_v1.json missing or wrong format\n");
        return false;
    }
    blocks.clear();
    const size_t blocksPos = json.find("\"blocks\"");
    if (blocksPos == std::string::npos)
        return false;
    for (int b = 0; b < 16; ++b) {
        const std::string marker = "\"block\": " + std::to_string(b);
        const size_t cur = json.find(marker, blocksPos);
        const size_t next = b == 15 ? json.find("\n  ]", cur) : json.find("\"block\": " + std::to_string(b + 1), cur);
        if (cur == std::string::npos || next == std::string::npos || next <= cur) {
            std::printf("FAIL fixture: cannot isolate MB0 block %d\n", b);
            return false;
        }
        const std::string one = json.substr(cur, next - cur);
        BlockFixture f;
        f.block = b;
        if (!fill16(one, "pred", f.pred) || !fill16(one, "dequant", f.dequant) ||
            !fill16(one, "idct", f.idct) || !fill16(one, "recon", f.recon))
            return false;
        blocks.push_back(f);
    }
    return blocks.size() == 16;
}

static int normAdjust(int qp, int i, int j) {
    static const int kNorm[6][3] = {
        {10, 13, 16}, {11, 14, 18}, {13, 16, 20},
        {14, 18, 23}, {16, 20, 25}, {18, 23, 29},
    };
    const int mi = ((i & 1) + (j & 1)) == 0 ? 0 : (((i & 1) + (j & 1)) == 1 ? 1 : 2);
    return kNorm[qp % 6][mi];
}

static void refDequant4x4(const int16_t coeff[16], int qp, int maxCoeff, int deq[16]) {
    static const int zz[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
    for (int i = 0; i < 16; ++i)
        deq[i] = 0;
    for (int scan = 0; scan < maxCoeff; ++scan) {
        if (!coeff[scan])
            continue;
        const int pos = zz[scan];
        const int row = pos / 4;
        const int col = pos % 4;
        const int qmul = (normAdjust(qp, row, col) * 16) << (qp / 6 + 2);
        int c = coeff[scan];
        if (const char* p = std::getenv("MPLEX_P3_IDCT_REF_PERTURB")) {
            if (std::string(p) == "dequant_ac" && scan == 1)
                c = -c;
        }
        deq[pos] = (c * qmul + 32) >> 6;
    }
}

static void refIdct4x4(const int deq[16], int residual[16]) {
    int b[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            b[r][c] = deq[r * 4 + c];
    b[0][0] += 32;
    int t[4][4];
    for (int r = 0; r < 4; ++r) {
        const int z0 = b[r][0] + b[r][2];
        const int z1 = b[r][0] - b[r][2];
        const int z2 = (b[r][1] >> 1) - b[r][3];
        const int z3 = b[r][1] + (b[r][3] >> 1);
        t[r][0] = z0 + z3;
        t[r][1] = z1 + z2;
        t[r][2] = z1 - z2;
        t[r][3] = z0 - z3;
    }
    for (int c = 0; c < 4; ++c) {
        const int z0 = t[0][c] + t[2][c];
        const int z1 = t[0][c] - t[2][c];
        const int z2 = (t[1][c] >> 1) - t[3][c];
        const int z3 = t[1][c] + (t[3][c] >> 1);
        residual[0 * 4 + c] = (z0 + z3) >> 6;
        residual[1 * 4 + c] = (z1 + z2) >> 6;
        residual[2 * 4 + c] = (z1 - z2) >> 6;
        residual[3 * 4 + c] = (z0 - z3) >> 6;
    }
}

static int clip8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

static bool compare16(const char* label, const int got[16], const std::array<int, 16>& exp) {
    for (int i = 0; i < 16; ++i) {
        if (got[i] != exp[i]) {
            std::printf("FAIL %s[%d]: got=%d want=%d\n", label, i, got[i], exp[i]);
            std::printf("  got:");
            for (int j = 0; j < 16; ++j)
                std::printf(" %d", got[j]);
            std::printf("\n  exp:");
            for (int j = 0; j < 16; ++j)
                std::printf(" %d", exp[j]);
            std::printf("\n");
            return false;
        }
    }
    return true;
}

static bool checkMb0Reference(const std::vector<BlockFixture>& blocks, const int16_t coeff[16]) {
    int deq[16];
    int idct[16];
    int recon[16];
    refDequant4x4(coeff, 25, 16, deq);
    refIdct4x4(deq, idct);
    for (int i = 0; i < 16; ++i)
        recon[i] = clip8(blocks[0].pred[i] + idct[i]);

    if (!compare16("dequant", deq, blocks[0].dequant) || !compare16("idct", idct, blocks[0].idct) ||
        !compare16("recon", recon, blocks[0].recon))
        return false;

    for (const auto& b : blocks) {
        for (int i = 0; i < 16; ++i) {
            const int wantRecon = clip8(b.pred[i] + b.idct[i]);
            if (wantRecon != b.recon[i]) {
                std::printf("FAIL mb0 block%d recon[%d]: pred=%d idct=%d got=%d want=%d\n",
                            b.block, i, b.pred[i], b.idct[i], wantRecon, b.recon[i]);
                return false;
            }
        }
    }
    return true;
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep))
        out.push_back(cur);
    return out;
}

static bool loadFrameMae(std::vector<MaeRow>& rows) {
    const std::string csv = readText("tests/fixtures/p3_host_recon/frame_mae_v1.csv");
    if (csv.empty() || !contains(csv, "format,misterplex.p3.frame_mae.v1")) {
        std::printf("FAIL frame_mae: missing or wrong format\n");
        return false;
    }
    rows.clear();
    std::istringstream is(csv);
    std::string line;
    bool inRows = false;
    while (std::getline(is, line)) {
        if (line == "mb,mb_x,mb_y,sum_abs_y,pixels,mae_y,max_abs_y") {
            inRows = true;
            continue;
        }
        if (!inRows || line.empty())
            continue;
        const auto f = split(line, ',');
        if (f.size() != 7) {
            std::printf("FAIL frame_mae: malformed row %s\n", line.c_str());
            return false;
        }
        MaeRow r;
        r.mb = std::stoi(f[0]);
        r.mbX = std::stoi(f[1]);
        r.mbY = std::stoi(f[2]);
        r.sumAbs = std::stoll(f[3]);
        r.pixels = std::stoi(f[4]);
        r.mae = std::stod(f[5]);
        r.maxAbs = std::stoi(f[6]);
        rows.push_back(r);
    }
    if (rows.size() != 300) {
        std::printf("FAIL frame_mae: rows=%zu want 300\n", rows.size());
        return false;
    }
    return true;
}

static bool ffmpegGold(const std::string& h264, const std::string& yuvOut) {
    const std::string cmd = "ffmpeg -y -hide_banner -loglevel error -skip_loop_filter all -i '" + h264 +
                            "' -pix_fmt yuv420p -vframes 1 '" + yuvOut + "'";
    return std::system(cmd.c_str()) == 0;
}

static bool checkFrameMae(const std::string& h264, const std::vector<uint8_t>& blob, size_t& rowsChecked,
                          double& frameMaeY) {
    std::vector<MaeRow> expected;
    if (!loadFrameMae(expected))
        return false;
    auto rec = misterplex::recon::reconISlice(blob.data(), blob.size());
    if (rec.mb_decoded != rec.mb_total || rec.width != 320 || rec.height != 240 || rec.y.empty()) {
        std::printf("FAIL frame_mae: recon mb=%d/%d frame=%dx%d reason=%s\n", rec.mb_decoded,
                    rec.mb_total, rec.width, rec.height, rec.fail_reason ? rec.fail_reason : "?");
        return false;
    }
    const std::string goldPath = "build/p3_idct_reference_model_gold.yuv";
    if (!ffmpegGold(h264, goldPath)) {
        std::printf("FAIL frame_mae: ffmpeg gold generation failed\n");
        return false;
    }
    const auto yuv = readBinary(goldPath);
    if (yuv.size() < rec.y.size()) {
        std::printf("FAIL frame_mae: gold size=%zu need %zu\n", yuv.size(), rec.y.size());
        return false;
    }
    frameMaeY = misterplex::recon::maeY(rec, yuv.data(), rec.y.size());
    const int mbW = (rec.width + 15) / 16;
    const int mbH = (rec.height + 15) / 16;
    if (mbW * mbH != static_cast<int>(expected.size())) {
        std::printf("FAIL frame_mae: grid=%d rows=%zu\n", mbW * mbH, expected.size());
        return false;
    }
    for (const auto& row : expected) {
        int64_t sum = 0;
        int maxAbs = 0;
        int pixels = 0;
        for (int yy = 0; yy < 16; ++yy) {
            const int y = row.mbY * 16 + yy;
            for (int xx = 0; xx < 16; ++xx) {
                const int x = row.mbX * 16 + xx;
                const size_t idx = static_cast<size_t>(y * rec.width + x);
                int d = std::abs(static_cast<int>(rec.y[idx]) - static_cast<int>(yuv[idx]));
                if (const char* p = std::getenv("MPLEX_P3_IDCT_REF_PERTURB")) {
                    if (std::string(p) == "frame_mae" && row.mb == 17 && yy == 0 && xx == 0)
                        ++d;
                }
                sum += d;
                maxAbs = std::max(maxAbs, d);
                ++pixels;
            }
        }
        const double mae = pixels ? static_cast<double>(sum) / static_cast<double>(pixels) : 0.0;
        if (row.mb != row.mbY * mbW + row.mbX || row.pixels != pixels || row.sumAbs != sum ||
            row.maxAbs != maxAbs || std::fabs(row.mae - mae) > 0.0000005) {
            std::printf("FAIL frame_mae mb=%d: got sum=%lld pixels=%d mae=%.6f max=%d; "
                        "want sum=%lld pixels=%d mae=%.6f max=%d\n",
                        row.mb, static_cast<long long>(sum), pixels, mae, maxAbs,
                        static_cast<long long>(row.sumAbs), row.pixels, row.mae, row.maxAbs);
            return false;
        }
    }
    rowsChecked = expected.size();
    return true;
}

int main() {
    if (!sourceGuards())
        return 1;

    std::vector<BlockFixture> mb0Blocks;
    if (!loadMb0Blocks(mb0Blocks))
        return 1;

    const std::string vecPath = "tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264";
    const auto annexb = readBinary(vecPath);
    if (annexb.size() != 6739) {
        std::printf("FAIL vector: size=%zu want 6739 (%s)\n", annexb.size(), vecPath.c_str());
        return 1;
    }

    auto r = misterplex::cavlc::probeFirstI16Dc(annexb.data(), annexb.size());
    if (!r.ok || r.total_coeff != gold::kTc || r.trailing_ones != gold::kT1 ||
        !gold::coeffScanMatches(r.coeff)) {
        std::printf("FAIL slice residual: ok=%d tc=%d t1=%d coeff0=%d csum=0x%02x\n",
                    r.ok ? 1 : 0, r.total_coeff, r.trailing_ones, r.ok ? r.coeff[0] : 0,
                    r.ok ? static_cast<unsigned>(gold::coeffCsum8(r.coeff)) : 0U);
        return 1;
    }
    if (!checkMb0Reference(mb0Blocks, r.coeff))
        return 1;

    size_t maeRows = 0;
    double frameMaeY = -1.0;
    if (!checkFrameMae(vecPath, annexb, maeRows, frameMaeY))
        return 1;

    int sum = 0;
    for (int v : mb0Blocks[0].recon)
        sum += v;
    const int mean = (sum + 8) / 16;
    std::printf("test_p3_idct_reference_model: OK vector_bytes=%zu coeff_csum=0x%02x tc=%d t1=%d "
                "mb0_blocks=%zu dequant16/idct16/recon16 match block0 frame_mae_rows=%zu "
                "maeY=%.6f y00=%d mean=%d\n",
                annexb.size(), static_cast<unsigned>(gold::coeffCsum8(r.coeff)), r.total_coeff,
                r.trailing_ones, mb0Blocks.size(), maeRows, frameMaeY, mb0Blocks[0].recon[0], mean);
    return 0;
}
