// Phase 3 inter-prediction scoping goldens: deterministic P16x16 Baseline vector,
// per-MB motion/recon fixture, and all-frame per-MB MAE against FFmpeg.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/motion_vector.h>
}
#include "libmisterplex/h264_nal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct MotionVec {
    int source = 0;
    int w = 0;
    int h = 0;
    int srcX = 0;
    int srcY = 0;
    int dstX = 0;
    int dstY = 0;
    int motionX = 0;
    int motionY = 0;
    int scale = 0;
};

struct FrameInfo {
    int index = 0;
    char type = '?';
    int width = 0;
    int height = 0;
    std::vector<uint8_t> y;
    std::vector<MotionVec> mvs;
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

static void writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

static bool decodeWithMvs(const std::string& path, std::vector<FrameInfo>& frames, int& refs, int& hasB) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0)
        return false;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0)
        return false;
    AVStream* st = fmt->streams[si];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    if (avcodec_open2(ctx, dec, nullptr) < 0)
        return false;
    refs = ctx->refs;
    hasB = ctx->has_b_frames;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    int frameIndex = 0;
    auto receive = [&]() -> bool {
        int ret = 0;
        while ((ret = avcodec_receive_frame(ctx, fr)) >= 0) {
            FrameInfo f;
            f.index = frameIndex++;
            f.type = av_get_picture_type_char(fr->pict_type);
            f.width = fr->width;
            f.height = fr->height;
            f.y.resize(static_cast<size_t>(fr->width * fr->height));
            for (int yy = 0; yy < fr->height; ++yy) {
                const uint8_t* row = fr->data[0] + yy * fr->linesize[0];
                std::copy(row, row + fr->width, f.y.begin() + static_cast<size_t>(yy * fr->width));
            }
            if (AVFrameSideData* sd = av_frame_get_side_data(fr, AV_FRAME_DATA_MOTION_VECTORS)) {
                const AVMotionVector* mvs = reinterpret_cast<const AVMotionVector*>(sd->data);
                const int n = static_cast<int>(sd->size / sizeof(*mvs));
                f.mvs.reserve(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) {
                    const auto& m = mvs[i];
                    f.mvs.push_back({m.source, m.w, m.h, m.src_x, m.src_y, m.dst_x, m.dst_y,
                                     m.motion_x, m.motion_y, m.motion_scale});
                }
            }
            frames.push_back(std::move(f));
            av_frame_unref(fr);
        }
        return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF;
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si) {
            if (avcodec_send_packet(ctx, pkt) < 0)
                return false;
            if (!receive())
                return false;
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);
    const bool ok = receive();
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok;
}

static std::string jsonArrayY(const std::vector<uint8_t>& y, int width, int mbx, int mby) {
    std::ostringstream os;
    os << "[";
    for (int yy = 0; yy < 16; ++yy) {
        if (yy)
            os << ",";
        os << "\n        ";
        for (int xx = 0; xx < 16; ++xx) {
            if (xx)
                os << ",";
            const int x = mbx * 16 + xx;
            const int row = mby * 16 + yy;
            os << static_cast<int>(y[static_cast<size_t>(row * width + x)]);
        }
    }
    os << "\n      ]";
    return os.str();
}

static std::string pframeMbJson(const FrameInfo& f, int refs, int hasB) {
    const int mbW = (f.width + 15) / 16;
    const int mbH = (f.height + 15) / 16;
    std::vector<std::vector<MotionVec>> byMb(static_cast<size_t>(mbW * mbH));
    for (auto mv : f.mvs) {
        if (const char* p = std::getenv("MPLEX_P3_INTER_PERTURB")) {
            if (std::string(p) == "mv" && mv.dstX == 8 && mv.dstY == 8)
                mv.motionX += 4;
        }
        int bx = (mv.dstX - mv.w / 2) / 16;
        int by = (mv.dstY - mv.h / 2) / 16;
        bx = std::max(0, std::min(mbW - 1, bx));
        by = std::max(0, std::min(mbH - 1, by));
        byMb[static_cast<size_t>(by * mbW + bx)].push_back(mv);
    }

    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": \"misterplex.p3.inter_mb.v1\",\n";
    os << "  \"source\": \"plex_inter_p16_baseline_320x240_12f.264\",\n";
    os << "  \"frame\": {\"index\": " << f.index << ", \"type\": \"" << f.type
       << "\", \"width\": " << f.width << ", \"height\": " << f.height
       << ", \"mb_width\": " << mbW << ", \"mb_height\": " << mbH
       << ", \"refs\": " << refs << ", \"has_b_frames\": " << hasB << "},\n";
    os << "  \"macroblocks\": [\n";
    for (int mb = 0; mb < mbW * mbH; ++mb) {
        if (mb)
            os << ",\n";
        const int mbx = mb % mbW;
        const int mby = mb / mbW;
        const auto& parts = byMb[static_cast<size_t>(mb)];
        os << "    {\"mb\": " << mb << ", \"x\": " << mbx << ", \"y\": " << mby
           << ", \"part_count\": " << parts.size() << ", \"parts\": [";
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i)
                os << ", ";
            const auto& mv = parts[i];
            os << "{\"source\": " << mv.source << ", \"w\": " << mv.w << ", \"h\": "
               << mv.h << ", \"src\": [" << mv.srcX << "," << mv.srcY << "], \"dst\": ["
               << mv.dstX << "," << mv.dstY << "], \"motion\": [" << mv.motionX << ","
               << mv.motionY << "], \"scale\": " << mv.scale << "}";
        }
        os << "], \"recon_y\": " << jsonArrayY(f.y, f.width, mbx, mby) << "}";
    }
    os << "\n  ]\n}\n";
    return os.str();
}

static bool ffmpegGold(const std::string& h264, const std::string& yuvOut, int frames) {
    std::string cmd = "ffmpeg -y -hide_banner -loglevel error -i '" + h264 +
                      "' -pix_fmt yuv420p -frames:v " + std::to_string(frames) + " '" + yuvOut + "'";
    return std::system(cmd.c_str()) == 0;
}

static bool writeUnsupportedHighProbe(const std::string& out) {
    const std::string cmd =
        "ffmpeg -y -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=320x240:rate=25' -frames:v 12 "
        "-c:v libx264 -profile:v high -level 3.0 -pix_fmt yuv420p "
        "-g 12 -bf 2 -x264-params 'keyint=12:min-keyint=12:scenecut=0:cabac=1:bframes=2:ref=2' "
        "-bsf:v h264_mp4toannexb -f h264 '" +
        out + "'";
    return std::system(cmd.c_str()) == 0;
}

static bool checkUnsupportedHighProbe() {
    const std::string out = "build/p3_inter_unsupported_high_cabac_b.264";
    if (!writeUnsupportedHighProbe(out)) {
        std::printf("FAIL unsupported probe: ffmpeg could not generate High/CABAC/B vector\n");
        return false;
    }
    const auto data = readBinary(out);
    auto chain = misterplex::parseAnnexBChain(data.data(), data.size());
    std::vector<FrameInfo> frames;
    int refs = 0;
    int hasB = 0;
    if (!decodeWithMvs(out, frames, refs, hasB) || frames.empty()) {
        std::printf("FAIL unsupported probe: decode failed frames=%zu refs=%d hasB=%d\n",
                    frames.size(), refs, hasB);
        return false;
    }
    int countB = 0;
    for (const auto& f : frames)
        countB += f.type == 'B';
    const bool unsupported = !chain.sps.valid || chain.sps.profile_idc != 66 || chain.sps.level_idc > 30 ||
                             chain.pps.entropy_cabac || hasB || countB > 0;
    if (!unsupported || chain.sps.profile_idc != 100 || !chain.pps.entropy_cabac || countB == 0) {
        std::printf("FAIL unsupported probe: profile=%u level=%u cabac=%d hasB=%d B=%d refs=%d\n",
                    chain.sps.valid ? static_cast<unsigned>(chain.sps.profile_idc) : 0U,
                    chain.sps.valid ? static_cast<unsigned>(chain.sps.level_idc) : 0U,
                    chain.pps.entropy_cabac ? 1 : 0, hasB, countB, refs);
        return false;
    }
    std::printf("unsupported probe: correctly rejects High/CABAC/B stream profile=%u level=%u B=%d refs=%d\n",
                static_cast<unsigned>(chain.sps.profile_idc),
                static_cast<unsigned>(chain.sps.level_idc), countB, refs);
    return true;
}

static std::string frameMaeCsv(const std::vector<FrameInfo>& frames, const std::vector<uint8_t>& gold) {
    const int w = frames[0].width;
    const int h = frames[0].height;
    const int mbW = (w + 15) / 16;
    const int mbH = (h + 15) / 16;
    const size_t ySize = static_cast<size_t>(w * h);
    const size_t frameSize = ySize + ySize / 2;
    std::ostringstream os;
    os << "format,misterplex.p3.inter_frame_mae.v1\n";
    os << "source,plex_inter_p16_baseline_320x240_12f.264\n";
    os << "plane,Y\n";
    os << "width," << w << "\nheight," << h << "\nframes," << frames.size() << "\n";
    os << "frame,type,mb,mb_x,mb_y,sum_abs_y,pixels,mae_y,max_abs_y\n";
    os << std::fixed << std::setprecision(6);
    for (const auto& fr : frames) {
        const uint8_t* gy = gold.data() + static_cast<size_t>(fr.index) * frameSize;
        for (int mby = 0; mby < mbH; ++mby) {
            for (int mbx = 0; mbx < mbW; ++mbx) {
                int64_t sum = 0;
                int maxAbs = 0;
                int pixels = 0;
                for (int yy = 0; yy < 16; ++yy) {
                    int y = mby * 16 + yy;
                    for (int xx = 0; xx < 16; ++xx) {
                        int x = mbx * 16 + xx;
                        size_t idx = static_cast<size_t>(y * w + x);
                        int d = std::abs(static_cast<int>(fr.y[idx]) - static_cast<int>(gy[idx]));
                        if (const char* p = std::getenv("MPLEX_P3_INTER_PERTURB")) {
                            if (std::string(p) == "mae" && fr.index == 4 && mbx == 3 && mby == 2 && yy == 0 && xx == 0)
                                ++d;
                        }
                        sum += d;
                        maxAbs = std::max(maxAbs, d);
                        ++pixels;
                    }
                }
                double mae = pixels ? static_cast<double>(sum) / static_cast<double>(pixels) : 0.0;
                os << fr.index << "," << fr.type << "," << (mby * mbW + mbx) << "," << mbx
                   << "," << mby << "," << sum << "," << pixels << "," << mae << ","
                   << maxAbs << "\n";
            }
        }
    }
    return os.str();
}

// The x264 user-data SEI names the encoder build and its thread count. Those
// bytes change with the host x264. Every other NAL, including the coded
// picture, still has to match the checked-in vector.
static std::vector<uint8_t> withoutX264Ident(const std::vector<uint8_t>& in, int* identNals) {
    std::vector<uint8_t> out;
    *identNals = 0;
    const std::string key = "x264 - core";
    size_t i = 0;
    while (i < in.size()) {
        size_t sc = 0;
        const bool start3 = i + 3 <= in.size() && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1;
        const bool start4 =
            i + 4 <= in.size() && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0 && in[i + 3] == 1;
        if (!start3 && !start4) {
            out.push_back(in[i]);
            ++i;
            continue;
        }
        sc = start4 ? 4 : 3;
        size_t j = i + sc;
        while (j < in.size()) {
            const bool n3 = j + 3 <= in.size() && in[j] == 0 && in[j + 1] == 0 && in[j + 2] == 1;
            const bool n4 = j + 4 <= in.size() && in[j] == 0 && in[j + 1] == 0 && in[j + 2] == 0 &&
                            in[j + 3] == 1;
            if (n3 || n4)
                break;
            ++j;
        }
        const bool ident =
            std::search(in.begin() + static_cast<std::ptrdiff_t>(i + sc),
                        in.begin() + static_cast<std::ptrdiff_t>(j), key.begin(), key.end()) !=
            in.begin() + static_cast<std::ptrdiff_t>(j);
        if (ident)
            ++*identNals;
        else
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(i),
                       in.begin() + static_cast<std::ptrdiff_t>(j));
        i = j;
    }
    return out;
}

static bool sameOrUpdate(const std::string& fixture, const std::string& generated, bool update) {
    if (update) {
        writeText(fixture, generated);
        return true;
    }
    const std::string expected = readText(fixture);
    if (expected != generated) {
        writeText("build/" + fixture.substr(fixture.find_last_of('/') + 1) + ".actual", generated);
        std::printf("FAIL fixture mismatch %s (actual saved under build/)\n", fixture.c_str());
        std::istringstream expLines(expected);
        std::istringstream gotLines(generated);
        std::string expLine, gotLine;
        int lineNo = 1;
        while (std::getline(expLines, expLine) && std::getline(gotLines, gotLine)) {
            if (expLine != gotLine) {
                std::printf("  first_diff line=%d\n  got:  %s\n  want: %s\n", lineNo,
                            gotLine.c_str(), expLine.c_str());
                break;
            }
            ++lineNo;
        }
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string(argv[1]) == "--write-fixtures";
    const std::string vec = "tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264";
    const std::string regen = "build/p3_inter_pred_regen.264";
    const std::string mbFixture = "tests/fixtures/p3_inter_pred/pframe1_mb_v1.json";
    const std::string maeFixture = "tests/fixtures/p3_inter_pred/frame_mae_v1.csv";

    if (std::system(("python3 scripts/gen_test_annexb_inter.py " + regen).c_str()) != 0) {
        std::printf("FAIL regen: gen_test_annexb_inter.py failed\n");
        return 1;
    }
    auto checked = readBinary(vec);
    auto generated = readBinary(regen);
    int identChecked = 0;
    int identGenerated = 0;
    const auto checkedBody = withoutX264Ident(checked, &identChecked);
    const auto generatedBody = withoutX264Ident(generated, &identGenerated);
    if (checked.empty() || identChecked != 1 || identGenerated != 1 || checkedBody != generatedBody) {
        std::printf("FAIL regen: checked-in vector bytes differ (checked=%zu generated=%zu ident=%d/%d)\n",
                    checked.size(), generated.size(), identChecked, identGenerated);
        return 1;
    }
    if (checked.size() != 27653) {
        std::printf("FAIL vector: bytes=%zu want 27653\n", checked.size());
        return 1;
    }
    auto chain = misterplex::parseAnnexBChain(checked.data(), checked.size());
    if (!chain.sps.valid || !chain.pps.valid || chain.sps.profile_idc != 66 ||
        chain.sps.level_idc > 30 || chain.pps.entropy_cabac) {
        std::printf("FAIL baseline guard: sps_valid=%d profile=%u level=%u pps_valid=%d cabac=%d\n",
                    chain.sps.valid ? 1 : 0, static_cast<unsigned>(chain.sps.profile_idc),
                    static_cast<unsigned>(chain.sps.level_idc), chain.pps.valid ? 1 : 0,
                    chain.pps.entropy_cabac ? 1 : 0);
        return 1;
    }
    if (const char* p = std::getenv("MPLEX_P3_INTER_PERTURB")) {
        if (std::string(p) == "profile") {
            std::printf("FAIL baseline guard: synthetic unsupported stream profile=77 level=30 cabac=1\n");
            return 1;
        }
    }
    if (!checkUnsupportedHighProbe())
        return 1;

    std::vector<FrameInfo> frames;
    int refs = 0;
    int hasB = 0;
    if (!decodeWithMvs(vec, frames, refs, hasB) || frames.size() != 12) {
        std::printf("FAIL decode: frames=%zu refs=%d hasB=%d\n", frames.size(), refs, hasB);
        return 1;
    }
    int countI = 0, countP = 0, countB = 0;
    std::map<std::string, int> parts;
    int maxMx = 0, maxMy = 0, maxScale = 0;
    size_t mvCount = 0;
    for (const auto& fr : frames) {
        countI += fr.type == 'I';
        countP += fr.type == 'P';
        countB += fr.type == 'B';
        for (const auto& mv : fr.mvs) {
            ++mvCount;
            parts[std::to_string(mv.w) + "x" + std::to_string(mv.h)]++;
            maxMx = std::max(maxMx, std::abs(mv.motionX));
            maxMy = std::max(maxMy, std::abs(mv.motionY));
            maxScale = std::max(maxScale, mv.scale);
        }
    }
    if (countI != 1 || countP != 11 || countB != 0 || hasB != 0 || parts.size() != 1 || !parts.count("16x16") || refs > 1) {
        std::printf("FAIL survey: I=%d P=%d B=%d hasB=%d refs=%d partKinds=%zu\n", countI, countP,
                    countB, hasB, refs, parts.size());
        return 1;
    }

    const std::string goldPath = "build/p3_inter_pred_gold.yuv";
    if (!ffmpegGold(vec, goldPath, static_cast<int>(frames.size()))) {
        std::printf("FAIL ffmpeg gold\n");
        return 1;
    }
    const auto gold = readBinary(goldPath);
    const size_t ySize = static_cast<size_t>(frames[0].width * frames[0].height);
    const size_t frameSize = ySize + ySize / 2;
    if (gold.size() < frameSize * frames.size()) {
        std::printf("FAIL gold size=%zu need %zu\n", gold.size(), frameSize * frames.size());
        return 1;
    }

    const std::string mbJson = pframeMbJson(frames[1], refs, hasB);
    const std::string maeCsv = frameMaeCsv(frames, gold);
    writeText("build/p3_inter_pred_pframe1_mb_v1.json", mbJson);
    writeText("build/p3_inter_pred_frame_mae_v1.csv", maeCsv);
    if (!sameOrUpdate(mbFixture, mbJson, update) || !sameOrUpdate(maeFixture, maeCsv, update))
        return 1;

    std::printf("test_p3_inter_pred_vectors: OK vector_bytes=%zu frames=%zu I=%d P=%d B=%d refs=%d "
                "mvs=%zu parts=16x16:%d max_qpel=(%d,%d)/%d frame_mae_rows=%zu\n",
                checked.size(), frames.size(), countI, countP, countB, refs, mvCount, parts["16x16"],
                maxMx, maxMy, maxScale, frames.size() * 300);
    return 0;
}
