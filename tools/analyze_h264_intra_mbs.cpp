#include "libmisterplex/h264_recon.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

struct Nal {
    size_t start = 0, end = 0;
    uint8_t type = 0;
};

size_t startCodeLen(const std::vector<uint8_t>& b, size_t p) {
    if (p + 4 <= b.size() && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 0 && b[p + 3] == 1) return 4;
    if (p + 3 <= b.size() && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 1) return 3;
    return 0;
}

std::vector<Nal> splitAnnexB(const std::vector<uint8_t>& b) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < b.size();) {
        size_t sc = startCodeLen(b, i);
        if (sc) {
            starts.push_back(i);
            i += sc;
        } else {
            ++i;
        }
    }
    std::vector<Nal> out;
    for (size_t i = 0; i < starts.size(); ++i) {
        size_t sc = startCodeLen(b, starts[i]);
        size_t hdr = starts[i] + sc;
        if (hdr >= b.size()) continue;
        out.push_back({starts[i], (i + 1 < starts.size()) ? starts[i + 1] : b.size(),
                       static_cast<uint8_t>(b[hdr] & 0x1f)});
    }
    return out;
}

void appendNal(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, const Nal& n) {
    dst.insert(dst.end(), src.begin() + static_cast<std::ptrdiff_t>(n.start),
               src.begin() + static_cast<std::ptrdiff_t>(n.end));
}

const char* i4Name(int mode) {
    static const char* k[] = {"I4_VERTICAL", "I4_HORIZONTAL", "I4_DC", "I4_DIAG_DOWN_LEFT",
                             "I4_DIAG_DOWN_RIGHT", "I4_VERTICAL_RIGHT", "I4_HORIZONTAL_DOWN",
                             "I4_VERTICAL_LEFT", "I4_HORIZONTAL_UP"};
    return (mode >= 0 && mode < 9) ? k[mode] : "I4_UNKNOWN";
}

const char* i16Name(int mode) {
    static const char* k[] = {"I16_VERTICAL", "I16_HORIZONTAL", "I16_DC", "I16_PLANE"};
    return (mode >= 0 && mode < 4) ? k[mode] : "I16_UNKNOWN";
}

const char* chromaName(int mode) {
    static const char* k[] = {"CHROMA_DC", "CHROMA_HORIZONTAL", "CHROMA_VERTICAL", "CHROMA_PLANE"};
    return (mode >= 0 && mode < 4) ? k[mode] : "CHROMA_UNKNOWN";
}

struct Count {
    uint64_t exact = 0, total = 0, exactPixels = 0, totalPixels = 0, sumAbs = 0;
    int maxAbs = 0;
};

struct Bad {
    bool valid = false;
    int mb = -1, mbx = 0, mby = 0, plane = 0, x = 0, y = 0, got = 0, ref = 0, abs = 0;
};

void addPixel(Count& c, int got, int ref) {
    int d = got > ref ? got - ref : ref - got;
    c.exactPixels += (d == 0);
    c.totalPixels++;
    c.sumAbs += static_cast<uint64_t>(d);
    if (d > c.maxAbs) c.maxAbs = d;
}

template <typename T>
void jsonArray(std::ostream& os, const T& a) {
    os << "[";
    for (size_t i = 0; i < a.size(); ++i) {
        if (i) os << ",";
        os << int(a[i]);
    }
    os << "]";
}

void jsonCount(std::ostream& os, const char* name, const Count& c) {
    const double mae = c.totalPixels ? double(c.sumAbs) / double(c.totalPixels) : 0.0;
    os << "{\"name\":\"" << name << "\",\"exact\":" << c.exact << ",\"total\":" << c.total
       << ",\"exact_pixels\":" << c.exactPixels << ",\"total_pixels\":" << c.totalPixels
       << ",\"mae\":" << std::fixed << std::setprecision(6) << mae
       << ",\"max_abs\":" << c.maxAbs << "}";
}

} // namespace

int main(int argc, char** argv) {
    std::string input, planes, manifest, output;
    int wantFrame = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* n) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + n);
            return argv[++i];
        };
        if (a == "--input") input = need("--input");
        else if (a == "--planes") planes = need("--planes");
        else if (a == "--manifest") manifest = need("--manifest");
        else if (a == "--output") output = need("--output");
        else if (a == "--frame-index") wantFrame = std::stoi(need("--frame-index"));
        else {
            std::cerr << "usage: analyze_h264_intra_mbs --input stream.264 --planes ref.yuv "
                         "--manifest frame_planes.json [--frame-index N] [--output out.json]\n";
            return 2;
        }
    }
    try {
        if (input.empty() || planes.empty() || manifest.empty())
            throw std::runtime_error("missing --input, --planes, or --manifest");
        const auto manifestBlob = readFile(manifest);
        const std::string manifestText(manifestBlob.begin(), manifestBlob.end());
        if (manifestText.find("\"format\": \"misterplex.p3.frame_planes_golden.v1\"") == std::string::npos) {
            std::cerr << "FAIL intra MB analysis: unknown frame-plane manifest format\n";
            return 2;
        }
        if (manifestText.find("\"loop_filter\": \"skip_loop_filter=all\"") == std::string::npos ||
            manifestText.find("\"h264_loop_filter\": \"disabled\"") == std::string::npos) {
            std::cerr << "FAIL intra MB analysis: current recon output is undeblocked; refusing manifest without "
                         "decoder.loop_filter=skip_loop_filter=all and provenance.h264_loop_filter=disabled\n";
            return 9;
        }
        auto blob = readFile(input);
        auto ref = readFile(planes);
        auto nals = splitAnnexB(blob);
        if (nals.size() < 2)
            throw std::runtime_error("fixture must contain >=2 NAL units");
        const Nal* sps = nullptr;
        const Nal* pps = nullptr;
        std::vector<uint8_t> au;
        int vclIndex = 0;
        for (const auto& n : nals) {
            if (n.type == 7) sps = &n;
            if (n.type == 8) pps = &n;
            if (n.type != 1 && n.type != 5) continue;
            if (vclIndex == wantFrame) {
                if (!sps || !pps) throw std::runtime_error("VCL before SPS/PPS");
                appendNal(au, blob, *sps);
                appendNal(au, blob, *pps);
                appendNal(au, blob, n);
                break;
            }
            ++vclIndex;
        }
        if (au.empty()) throw std::runtime_error("requested frame not found");
        auto chain = misterplex::parseAnnexBChain(au.data(), au.size());
        if (!chain.sps.valid || !chain.pps.valid || !chain.slice.valid || !chain.slice.is_i_slice)
            throw std::runtime_error("requested frame is not a parsed Baseline/CAVLC I slice");
        auto rec = misterplex::recon::reconISlice(au.data(), au.size(), nullptr);
        if (rec.mb_decoded != rec.mb_total || rec.y.empty())
            throw std::runtime_error(std::string("recon failed: ") + (rec.fail_reason ? rec.fail_reason : "?"));
        const int w = rec.width, h = rec.height, cw = w / 2, ch = h / 2;
        const int mbW = (w + 15) / 16, mbH = (h + 15) / 16;
        const size_t yBytes = static_cast<size_t>(w) * h;
        const size_t cBytes = static_cast<size_t>(cw) * ch;
        const size_t frameBytes = yBytes + cBytes * 2;
        const size_t frameOff = static_cast<size_t>(wantFrame) * frameBytes;
        if (frameOff + frameBytes > ref.size())
            throw std::runtime_error("reference planes do not contain requested frame");
        const size_t yOff = frameOff, uOff = yOff + yBytes, vOff = uOff + cBytes;

        Count mbAll, mbLuma, mbChroma;
        std::array<Count, 9> i4{};
        std::array<Count, 4> i16{};
        std::array<Count, 4> chr{};
        Bad first;
        std::vector<char> grid(static_cast<size_t>(rec.mb_total), '?');
        std::vector<misterplex::recon::LumaMbTrace> traces(static_cast<size_t>(rec.mb_total));

        auto refY = [&](int x, int y) { return int(ref[yOff + static_cast<size_t>(y) * w + x]); };
        auto refU = [&](int x, int y) { return int(ref[uOff + static_cast<size_t>(y) * cw + x]); };
        auto refV = [&](int x, int y) { return int(ref[vOff + static_cast<size_t>(y) * cw + x]); };
        auto recY = [&](int x, int y) { return int(rec.y[static_cast<size_t>(y) * w + x]); };
        auto recU = [&](int x, int y) { return int(rec.u[static_cast<size_t>(y) * cw + x]); };
        auto recV = [&](int x, int y) { return int(rec.v[static_cast<size_t>(y) * cw + x]); };

        for (int mb = 0; mb < rec.mb_total; ++mb) {
            misterplex::recon::ReconTrace tr;
            tr.target_mb = mb;
            auto traced = misterplex::recon::reconISlice(au.data(), au.size(), &tr);
            if (traced.mb_decoded != traced.mb_total || !tr.mb.valid)
                throw std::runtime_error("trace failed at mb " + std::to_string(mb));
            traces[static_cast<size_t>(mb)] = tr.mb;
            const int mbx = mb % mbW, mby = mb / mbW;
            const int x0 = mbx * 16, y0 = mby * 16;
            bool lumaOk = true, chromaOk = true;
            for (int yy = 0; yy < 16; ++yy) {
                for (int xx = 0; xx < 16; ++xx) {
                    if (x0 + xx >= w || y0 + yy >= h) continue;
                    int g = recY(x0 + xx, y0 + yy), r = refY(x0 + xx, y0 + yy);
                    if (g != r) {
                        lumaOk = false;
                        if (!first.valid) first = {true, mb, mbx, mby, 0, x0 + xx, y0 + yy, g, r, std::abs(g - r)};
                    }
                }
            }
            for (int yy = 0; yy < 8; ++yy) {
                for (int xx = 0; xx < 8; ++xx) {
                    int cx = mbx * 8 + xx, cy = mby * 8 + yy;
                    if (cx >= cw || cy >= ch) continue;
                    int gu = recU(cx, cy), ru = refU(cx, cy);
                    int gv = recV(cx, cy), rv = refV(cx, cy);
                    if (gu != ru || gv != rv) {
                        chromaOk = false;
                        if (!first.valid && gu != ru) first = {true, mb, mbx, mby, 1, cx, cy, gu, ru, std::abs(gu - ru)};
                        if (!first.valid && gv != rv) first = {true, mb, mbx, mby, 2, cx, cy, gv, rv, std::abs(gv - rv)};
                    }
                }
            }
            mbAll.total++;
            mbAll.exact += (lumaOk && chromaOk);
            mbLuma.total++;
            mbLuma.exact += lumaOk;
            mbChroma.total++;
            mbChroma.exact += chromaOk;
            grid[static_cast<size_t>(mb)] = (lumaOk && chromaOk) ? '.' : (!lumaOk && chromaOk ? 'Y' : (lumaOk ? 'C' : 'B'));

            if (tr.mb.mb_type == 0) {
                for (const auto& b : tr.mb.blocks) {
                    if (b.pred_mode < 0 || b.pred_mode >= 9) continue;
                    Count& c = i4[static_cast<size_t>(b.pred_mode)];
                    c.total++;
                    bool blockOk = true;
                    for (int yy = 0; yy < 4; ++yy) {
                        for (int xx = 0; xx < 4; ++xx) {
                            int px = x0 + b.x + xx, py = y0 + b.y + yy;
                            if (px >= w || py >= h) continue;
                            int g = recY(px, py), r = refY(px, py);
                            addPixel(c, g, r);
                            if (g != r) blockOk = false;
                        }
                    }
                    c.exact += blockOk;
                }
            } else if (tr.mb.pred_mode >= 0 && tr.mb.pred_mode < 4) {
                Count& c = i16[static_cast<size_t>(tr.mb.pred_mode)];
                c.total++;
                c.exact += lumaOk;
                for (int yy = 0; yy < 16; ++yy) {
                    for (int xx = 0; xx < 16; ++xx) {
                        if (x0 + xx >= w || y0 + yy >= h) continue;
                        addPixel(c, recY(x0 + xx, y0 + yy), refY(x0 + xx, y0 + yy));
                    }
                }
            }
            if (tr.mb.chroma_mode >= 0 && tr.mb.chroma_mode < 4) {
                Count& c = chr[static_cast<size_t>(tr.mb.chroma_mode)];
                c.total++;
                c.exact += chromaOk;
                for (int yy = 0; yy < 8; ++yy) {
                    for (int xx = 0; xx < 8; ++xx) {
                        int cx = mbx * 8 + xx, cy = mby * 8 + yy;
                        if (cx >= cw || cy >= ch) continue;
                        addPixel(c, recU(cx, cy), refU(cx, cy));
                        addPixel(c, recV(cx, cy), refV(cx, cy));
                    }
                }
            }
        }

        std::ostringstream os;
        os << "{\n  \"format\":\"misterplex.p3.intra_mb_analysis.v1\",\n";
        os << "  \"source\":{\"path\":\"" << input << "\"},\n";
        os << "  \"reference\":{\"path\":\"" << planes << "\"},\n";
        os << "  \"golden_manifest\":{\"path\":\"" << manifest
           << "\",\"decoder_loop_filter\":\"skip_loop_filter=all\",\"h264_loop_filter\":\"disabled\"},\n";
        os << "  \"frame_index\":" << wantFrame << ",\"geometry\":{\"width\":" << w
           << ",\"height\":" << h << ",\"mb_width\":" << mbW << ",\"mb_height\":" << mbH << "},\n";
        os << "  \"summary\":{\"mb_exact\":" << mbAll.exact << ",\"mb_total\":" << mbAll.total
           << ",\"luma_mb_exact\":" << mbLuma.exact << ",\"chroma_mb_exact\":" << mbChroma.exact
           << ",\"first_wrong\":{\"mb\":" << first.mb << ",\"mb_x\":" << first.mbx
           << ",\"mb_y\":" << first.mby << ",\"plane\":\"" << (first.plane == 0 ? "Y" : (first.plane == 1 ? "U" : "V"))
           << "\",\"x\":" << first.x << ",\"y\":" << first.y << ",\"got\":" << first.got
           << ",\"ref\":" << first.ref << ",\"abs\":" << first.abs << "}},\n";
        os << "  \"spatial\":[\n";
        for (int y = 0; y < mbH; ++y) {
            os << "    \"";
            for (int x = 0; x < mbW; ++x) os << grid[static_cast<size_t>(y * mbW + x)];
            os << "\"" << (y + 1 == mbH ? "\n" : ",\n");
        }
        os << "  ],\n";
        os << "  \"luma4x4_modes\":[";
        for (int i = 0; i < 9; ++i) {
            if (i) os << ",";
            jsonCount(os, i4Name(i), i4[static_cast<size_t>(i)]);
        }
        os << "],\n  \"luma16x16_modes\":[";
        for (int i = 0; i < 4; ++i) {
            if (i) os << ",";
            jsonCount(os, i16Name(i), i16[static_cast<size_t>(i)]);
        }
        os << "],\n  \"chroma_modes\":[";
        for (int i = 0; i < 4; ++i) {
            if (i) os << ",";
            jsonCount(os, chromaName(i), chr[static_cast<size_t>(i)]);
        }
        os << "]";

        if (first.valid && first.plane == 0) {
            const auto& tr = traces[static_cast<size_t>(first.mb)];
            const int localX = first.x - first.mbx * 16;
            const int localY = first.y - first.mby * 16;
            const int bx = localX / 4;
            const int by = localY / 4;
            const misterplex::recon::Luma4x4Trace* block = nullptr;
            for (const auto& b : tr.blocks) {
                if (b.x == bx * 4 && b.y == by * 4) {
                    block = &b;
                    break;
                }
            }
            if (block) {
                os << ",\n  \"first_wrong_decomposition\":{\"mb_type\":" << tr.mb_type
                   << ",\"luma16x16_mode\":" << tr.pred_mode
                   << ",\"chroma_mode\":" << tr.chroma_mode
                   << ",\"qp\":" << tr.qp << ",\"cbp_luma\":" << tr.cbp_luma
                   << ",\"cbp_chroma\":" << tr.cbp_chroma
                   << ",\"block\":" << block->block << ",\"block_x\":" << block->x
                   << ",\"block_y\":" << block->y << ",\"pred_mode\":" << block->pred_mode
                   << ",\"total_coeff\":" << block->total_coeff << ",\"pred\":";
                jsonArray(os, block->pred);
                os << ",\"coefficients_zigzag\":";
                jsonArray(os, block->coeff);
                os << ",\"dequant\":";
                jsonArray(os, block->dequant);
                os << ",\"idct\":";
                jsonArray(os, block->idct);
                os << ",\"recon\":";
                jsonArray(os, block->recon);
                os << ",\"reference\":";
                std::array<int, 16> rb{};
                for (int yy = 0; yy < 4; ++yy)
                    for (int xx = 0; xx < 4; ++xx)
                        rb[static_cast<size_t>(yy * 4 + xx)] = refY(first.mbx * 16 + block->x + xx,
                                                                    first.mby * 16 + block->y + yy);
                jsonArray(os, rb);
                os << "}";
            }
        }
        os << "\n}\n";
        if (!output.empty()) {
            std::ofstream out(output);
            if (!out) throw std::runtime_error("cannot write " + output);
            out << os.str();
        } else {
            std::cout << os.str();
        }
        std::cout << "INTRA_MB_ANALYSIS frame=" << wantFrame << " size=" << w << "x" << h
                  << " mb_exact=" << mbAll.exact << "/" << mbAll.total
                  << " luma_mb_exact=" << mbLuma.exact << "/" << mbLuma.total
                  << " chroma_mb_exact=" << mbChroma.exact << "/" << mbChroma.total
                  << " first_wrong_mb=" << first.mb << "(" << first.mbx << "," << first.mby << ")"
                  << " plane=" << (first.plane == 0 ? "Y" : (first.plane == 1 ? "U" : "V"))
                  << " x=" << first.x << " y=" << first.y
                  << " got=" << first.got << " ref=" << first.ref << " abs=" << first.abs << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL intra MB analysis: " << e.what() << "\n";
        return 1;
    }
}
