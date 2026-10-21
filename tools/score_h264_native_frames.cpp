// Native I420 frame scorer for checked-in Baseline/CAVLC fixtures.
// I/IDR frames are reconstructed through the host CAVLC/intra reference model
// and compared directly plane-vs-plane against FFmpeg YUV420p goldens.
// P frames are reported as expected-red until the inter/DPB RTL path is wired
// into a native plane producer.
#include "libmisterplex/h264_recon.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

std::string sha256Hex(const std::vector<uint8_t>& data) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<uint8_t> msg = data;
    const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            size_t p = off + static_cast<size_t>(i * 4);
            w[i] = (uint32_t(msg[p]) << 24) | (uint32_t(msg[p + 1]) << 16) |
                   (uint32_t(msg[p + 2]) << 8) | uint32_t(msg[p + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint32_t v : h)
        os << std::setw(8) << v;
    return os.str();
}

struct Nal {
    size_t start = 0, end = 0, payload = 0;
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
                       hdr + 1, static_cast<uint8_t>(b[hdr] & 0x1f)});
    }
    return out;
}

void appendNal(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, const Nal& n) {
    dst.insert(dst.end(), src.begin() + static_cast<std::ptrdiff_t>(n.start),
               src.begin() + static_cast<std::ptrdiff_t>(n.end));
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

struct PlaneStats {
    uint64_t exact = 0, total = 0, sumAbs = 0;
    int maxAbs = 0, firstX = -1, firstY = -1, firstGot = 0, firstRef = 0;
};

void scorePlane(const std::vector<uint8_t>& got, const std::vector<uint8_t>& ref,
                size_t refOff, int w, int h, PlaneStats& st) {
    st.total = static_cast<uint64_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = static_cast<size_t>(y * w + x);
            int g = got[i];
            int r = ref[refOff + i];
            int d = g > r ? g - r : r - g;
            st.exact += d == 0;
            st.sumAbs += static_cast<uint64_t>(d);
            if (d > st.maxAbs) st.maxAbs = d;
            if (d && st.firstX < 0) {
                st.firstX = x; st.firstY = y; st.firstGot = g; st.firstRef = r;
            }
        }
    }
}

int mbExactAllPlanes(const misterplex::recon::ReconResult& rec, const std::vector<uint8_t>& ref, size_t frameOff) {
    const int mbW = (rec.width + 15) / 16;
    const int mbH = (rec.height + 15) / 16;
    const int cw = rec.width / 2;
    const size_t yOff = frameOff;
    const size_t uOff = yOff + static_cast<size_t>(rec.width) * rec.height;
    const size_t vOff = uOff + static_cast<size_t>(cw) * (rec.height / 2);
    int exact = 0;
    for (int my = 0; my < mbH; ++my) {
        for (int mx = 0; mx < mbW; ++mx) {
            bool ok = true;
            for (int y = 0; y < 16 && ok; ++y) {
                for (int x = 0; x < 16; ++x) {
                    int px = mx * 16 + x, py = my * 16 + y;
                    if (px >= rec.width || py >= rec.height) continue;
                    size_t i = static_cast<size_t>(py * rec.width + px);
                    if (rec.y[i] != ref[yOff + i]) { ok = false; break; }
                }
            }
            for (int p = 0; p < 2 && ok; ++p) {
                const auto& plane = p == 0 ? rec.u : rec.v;
                size_t off = p == 0 ? uOff : vOff;
                for (int y = 0; y < 8 && ok; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        int px = mx * 8 + x, py = my * 8 + y;
                        if (px >= cw || py >= rec.height / 2) continue;
                        size_t i = static_cast<size_t>(py * cw + px);
                        if (plane[i] != ref[off + i]) { ok = false; break; }
                    }
                }
            }
            exact += ok;
        }
    }
    return exact;
}

} // namespace

int main(int argc, char** argv) {
    std::string input, planes, manifest, output;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* n) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << n << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--input") input = need("--input");
        else if (a == "--planes") planes = need("--planes");
        else if (a == "--manifest") manifest = need("--manifest");
        else if (a == "--output") output = need("--output");
        else {
            std::cerr << "usage: score_h264_native_frames --input stream.264 --planes golden.yuv "
                         "--manifest frame_planes.json [--output out.json]\n";
            return 2;
        }
    }
    if (input.empty() || planes.empty() || manifest.empty()) {
        std::cerr << "usage: score_h264_native_frames --input stream.264 --planes golden.yuv "
                     "--manifest frame_planes.json [--output out.json]\n";
        return 2;
    }
    auto blob = readFile(input);
    auto golden = readFile(planes);
    auto manifestBlob = readFile(manifest);
    if (blob.empty() || golden.empty()) {
        std::cerr << "FAIL native score: missing input or planes\n";
        return 1;
    }
    if (manifestBlob.empty()) {
        std::cerr << "FAIL native score: missing frame-plane manifest\n";
        return 1;
    }
    const std::string manifestText(manifestBlob.begin(), manifestBlob.end());
    if (manifestText.find("\"format\": \"misterplex.p3.frame_planes_golden.v1\"") == std::string::npos) {
        std::cerr << "FAIL native score: unknown frame-plane manifest format\n";
        return 2;
    }
    if (manifestText.find("\"loop_filter\": \"skip_loop_filter=all\"") == std::string::npos ||
        manifestText.find("\"h264_loop_filter\": \"disabled\"") == std::string::npos) {
        std::cerr << "FAIL native score: current recon output is undeblocked; refusing manifest without "
                     "decoder.loop_filter=skip_loop_filter=all and provenance.h264_loop_filter=disabled\n";
        return 9;
    }
    auto nals = splitAnnexB(blob);
    if (nals.size() < 2) {
        std::cerr << "FAIL native score: fixture must contain >=2 NALs\n";
        return 1;
    }

    const Nal* sps = nullptr;
    const Nal* pps = nullptr;
    int frameIndex = 0, idr = 0, p = 0, iFrames = 0, pFrames = 0;
    int width = 0, height = 0, mbTotal = 0;
    int intraMbExact = 0, intraMbTotal = 0;
    bool firstBadValid = false;
    std::string firstBadText = "null";
    std::ostringstream frames;
    frames << "  \"frames\": [\n";
    bool firstFrameJson = true;

    for (const auto& n : nals) {
        if (n.type == 7) sps = &n;
        if (n.type == 8) pps = &n;
        if (n.type != 1 && n.type != 5) continue;
        if (!sps || !pps) {
            std::cerr << "FAIL native score: VCL before SPS/PPS\n";
            return 1;
        }
        std::vector<uint8_t> one;
        appendNal(one, blob, *sps);
        appendNal(one, blob, *pps);
        appendNal(one, blob, n);
        auto chain = misterplex::parseAnnexBChain(one.data(), one.size());
        if (!chain.sps.valid || !chain.pps.valid || !chain.slice.valid) {
            std::cerr << "FAIL native score: cannot parse VCL frame " << frameIndex << "\n";
            return 1;
        }
        if (!width) {
            width = chain.sps.width;
            height = chain.sps.height;
            mbTotal = ((width + 15) / 16) * ((height + 15) / 16);
            const size_t frameBytes = static_cast<size_t>(width) * height * 3 / 2;
            if (golden.size() % frameBytes != 0) {
                std::cerr << "FAIL native score: golden size is not whole I420 frames for SPS geometry\n";
                return 1;
            }
        }
        if (n.type == 5) ++idr;
        if (n.type == 1) ++p;
        const size_t frameBytes = static_cast<size_t>(width) * height * 3 / 2;
        const size_t frameOff = static_cast<size_t>(frameIndex) * frameBytes;
        if (frameOff + frameBytes > golden.size()) {
            std::cerr << "FAIL native score: more VCL frames than golden planes\n";
            return 1;
        }
        const bool isI = chain.slice.is_i_slice;
        if (isI) ++iFrames; else ++pFrames;
        if (!firstFrameJson) frames << ",\n";
        firstFrameJson = false;
        frames << "    {\"frame_index\": " << frameIndex << ", \"nal_type\": " << int(n.type)
               << ", \"slice_kind\": \"" << (isI ? "I" : "P") << "\"";
        if (!isI) {
            frames << ", \"implemented\": false, \"expected_red\": true"
                   << ", \"reason\": \"inter/DPB native plane producer not wired in this branch\""
                   << ", \"mb_exact\": 0, \"mb_total\": " << mbTotal << ", \"planes\": []}";
            ++frameIndex;
            continue;
        }
        auto rec = misterplex::recon::reconISlice(one.data(), one.size(), nullptr);
        if (rec.mb_decoded != rec.mb_total || rec.y.empty()) {
            std::cerr << "FAIL native score: I recon failed frame " << frameIndex
                      << " reason=" << (rec.fail_reason ? rec.fail_reason : "?") << "\n";
            return 1;
        }
        const int cw = width / 2;
        const size_t yOff = frameOff;
        const size_t uOff = yOff + static_cast<size_t>(width) * height;
        const size_t vOff = uOff + static_cast<size_t>(cw) * (height / 2);
        std::array<PlaneStats, 3> st{};
        scorePlane(rec.y, golden, yOff, width, height, st[0]);
        scorePlane(rec.u, golden, uOff, cw, height / 2, st[1]);
        scorePlane(rec.v, golden, vOff, cw, height / 2, st[2]);
        int mbExact = mbExactAllPlanes(rec, golden, frameOff);
        intraMbExact += mbExact;
        intraMbTotal += mbTotal;
        frames << ", \"implemented\": true, \"expected_red\": false, \"mb_exact\": " << mbExact
               << ", \"mb_total\": " << mbTotal << ", \"planes\": [";
        const char* names[3] = {"Y", "U", "V"};
        for (int pi = 0; pi < 3; ++pi) {
            if (pi) frames << ", ";
            double mae = st[pi].total ? double(st[pi].sumAbs) / double(st[pi].total) : 0.0;
            frames << "{\"plane\": \"" << names[pi] << "\", \"exact_pixels\": " << st[pi].exact
                   << ", \"total_pixels\": " << st[pi].total << ", \"mae\": "
                   << std::fixed << std::setprecision(6) << mae << ", \"max_abs\": "
                   << st[pi].maxAbs << ", \"first_bad\": ";
            if (st[pi].firstX >= 0) {
                frames << "{\"x\": " << st[pi].firstX << ", \"y\": " << st[pi].firstY
                       << ", \"got\": " << st[pi].firstGot << ", \"ref\": " << st[pi].firstRef
                       << ", \"abs\": " << (st[pi].firstGot > st[pi].firstRef
                             ? st[pi].firstGot - st[pi].firstRef : st[pi].firstRef - st[pi].firstGot)
                       << "}";
                if (!firstBadValid) {
                    firstBadValid = true;
                    std::ostringstream fb;
                    fb << "{\"frame_index\": " << frameIndex << ", \"plane\": \"" << names[pi]
                       << "\", \"x\": " << st[pi].firstX << ", \"y\": " << st[pi].firstY
                       << ", \"mb_x\": " << (st[pi].firstX / (pi == 0 ? 16 : 8))
                       << ", \"mb_y\": " << (st[pi].firstY / (pi == 0 ? 16 : 8))
                       << ", \"pixel_in_mb_x\": " << (st[pi].firstX % (pi == 0 ? 16 : 8))
                       << ", \"pixel_in_mb_y\": " << (st[pi].firstY % (pi == 0 ? 16 : 8))
                       << ", \"got\": " << st[pi].firstGot << ", \"ref\": " << st[pi].firstRef
                       << ", \"abs\": " << (st[pi].firstGot > st[pi].firstRef
                             ? st[pi].firstGot - st[pi].firstRef : st[pi].firstRef - st[pi].firstGot)
                       << "}";
                    firstBadText = fb.str();
                }
            } else {
                frames << "null";
            }
            frames << "}";
        }
        frames << "]}";
        ++frameIndex;
    }
    frames << "\n  ]\n";
    if (frameIndex < 2 || pFrames < 1 || iFrames < 1) {
        std::cerr << "FAIL native score: need multi-frame I+P fixture, got frames=" << frameIndex
                  << " i=" << iFrames << " p=" << pFrames << "\n";
        return 1;
    }

    std::ostringstream out;
    out << "{\n";
    out << "  \"format\": \"misterplex.p3.native_frame_score.v1\",\n";
    out << "  \"source\": {\"path\": \"" << jsonEscape(input) << "\", \"bytes\": " << blob.size()
        << ", \"sha256\": \"" << sha256Hex(blob) << "\"},\n";
    out << "  \"colorspace\": \"I420_NATIVE\",\n";
    out << "  \"loop_filter\": \"skip_loop_filter=all\",\n";
    out << "  \"h264_loop_filter\": \"disabled\",\n";
    out << "  \"golden_manifest\": \"" << jsonEscape(manifest) << "\",\n";
    out << "  \"mechanism\": \"Native score compares host CAVLC/intra reconstructed I420 planes directly to FFmpeg yuv420p goldens whose manifest declares decoder.loop_filter=skip_loop_filter=all and provenance.h264_loop_filter=disabled; no RGB/RGB565 round-trip, loop-filter mismatch, or presentation border is in the reference path.\",\n";
    out << "  \"geometry\": {\"width\": " << width << ", \"height\": " << height
        << ", \"mb_total_per_frame\": " << mbTotal << "},\n";
    out << "  \"summary\": {\"frames\": " << frameIndex << ", \"idr\": " << idr
        << ", \"p_nals\": " << p << ", \"i_frames\": " << iFrames << ", \"p_frames\": "
        << pFrames << ", \"intra_mb_exact\": " << intraMbExact << ", \"intra_mb_total\": "
        << intraMbTotal << ", \"inter_expected_red_frames\": " << pFrames
        << ", \"first_bad\": " << firstBadText << "},\n";
    out << frames.str();
    out << "}\n";
    if (!output.empty()) {
        std::ofstream f(output, std::ios::binary);
        if (!f) {
            std::cerr << "FAIL native score: cannot write " << output << "\n";
            return 1;
        }
        f << out.str();
    } else {
        std::cout << out.str();
    }
    std::cout << "NATIVE_FRAME_SCORE summary frames=" << frameIndex << " i=" << iFrames
              << " p=" << pFrames << " intra_mb_exact=" << intraMbExact << "/"
              << intraMbTotal << " inter_expected_red_frames=" << pFrames
              << " first_bad=" << (firstBadValid ? "yes" : "none") << "\n";
    return 0;
}
