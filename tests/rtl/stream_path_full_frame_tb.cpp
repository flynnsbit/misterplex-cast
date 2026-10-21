#include "Vstream_path_full_frame_tb.h"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace {

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct Args {
    std::string annexb;
    std::string goldenPlanes;
    std::string goldenManifest;
    std::string candidateI420Out;
    std::string nativeCandidateI420Out;
    std::string interMetadataOut;
    std::string sequenceJson;
    std::string sourceSha256;
    std::string jsonOut;
    std::string traceJsonOut;
    int width = 0;
    int height = 0;
    bool expectRed = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[i];
        };
        if (k == "--annexb") a.annexb = need("--annexb");
        else if (k == "--golden-planes") a.goldenPlanes = need("--golden-planes");
        else if (k == "--golden-manifest") a.goldenManifest = need("--golden-manifest");
        else if (k == "--candidate-i420-out") a.candidateI420Out = need("--candidate-i420-out");
        else if (k == "--native-candidate-i420-out") a.nativeCandidateI420Out = need("--native-candidate-i420-out");
        else if (k == "--inter-metadata-out") a.interMetadataOut = need("--inter-metadata-out");
        else if (k == "--sequence") a.sequenceJson = need("--sequence");
        else if (k == "--source-sha256") a.sourceSha256 = need("--source-sha256");
        else if (k == "--json-out") a.jsonOut = need("--json-out");
        else if (k == "--trace-json-out") a.traceJsonOut = need("--trace-json-out");
        else if (k == "--width") a.width = std::stoi(need("--width"));
        else if (k == "--height") a.height = std::stoi(need("--height"));
        else if (k == "--expect-red") a.expectRed = true;
        else throw std::runtime_error("unknown argument: " + k);
    }
    if (a.annexb.empty() || a.goldenPlanes.empty() || a.goldenManifest.empty() ||
        a.sequenceJson.empty() || a.sourceSha256.empty() ||
        a.width <= 0 || a.height <= 0)
        throw std::runtime_error("usage: --annexb in.264 --golden-planes ref.yuv "
                                 "--golden-manifest ref.json --sequence sequence.json "
                                 "--source-sha256 SHA --width W --height H [--json-out out.json] [--expect-red]");
    return a;
}

int parseIntAfter(const std::string& text, const std::string& key, std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle, start);
    if (p == std::string::npos) throw std::runtime_error("missing sequence key: " + key);
    p = text.find(':', p);
    if (p == std::string::npos) throw std::runtime_error("malformed sequence key: " + key);
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    char* end = nullptr;
    long v = std::strtol(text.c_str() + p, &end, 10);
    if (end == text.c_str() + p) throw std::runtime_error("invalid integer sequence key: " + key);
    return static_cast<int>(v);
}

std::string parseStringAfter(const std::string& text, const std::string& key, std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle, start);
    if (p == std::string::npos) throw std::runtime_error("missing sequence key: " + key);
    p = text.find(':', p);
    p = text.find('"', p);
    if (p == std::string::npos) throw std::runtime_error("malformed sequence string key: " + key);
    const std::size_t q = text.find('"', p + 1);
    if (q == std::string::npos) throw std::runtime_error("unterminated sequence string key: " + key);
    return text.substr(p + 1, q - p - 1);
}

struct FrameMeta {
    int frameNum = -1;
    std::string sliceKind = "?";
};

struct SequenceMeta {
    int bytes = 0;
    int width = 0;
    int height = 0;
    int nalCount = 0;
    int vcl = 0;
    int idr = 0;
    int nonIdr = 0;
    int pSlices = 0;
    std::string sha256;
    std::vector<FrameMeta> frames;
};

SequenceMeta parseSequenceManifest(const std::string& text) {
    if (parseStringAfter(text, "format") != "misterplex.p3.nal_sequence.v1")
        throw std::runtime_error("sequence manifest is not misterplex.p3.nal_sequence.v1");
    if (text.find("\"ioctl_injection\"") == std::string::npos ||
        text.find("ioctl_download") == std::string::npos ||
        text.find("ioctl_wr") == std::string::npos)
        throw std::runtime_error("sequence manifest does not declare ioctl injection route");

    SequenceMeta m;
    m.bytes = parseIntAfter(text, "bytes");
    m.sha256 = parseStringAfter(text, "sha256");
    m.width = parseIntAfter(text, "coded_width");
    m.height = parseIntAfter(text, "coded_height");
    m.nalCount = parseIntAfter(text, "nal_count");
    m.vcl = parseIntAfter(text, "vcl");
    m.idr = parseIntAfter(text, "idr");
    m.nonIdr = parseIntAfter(text, "non_idr");
    m.pSlices = parseIntAfter(text, "p_slices");

    std::size_t p = 0;
    while ((p = text.find("\"vcl_index\"", p)) != std::string::npos) {
        FrameMeta fm;
        fm.frameNum = parseIntAfter(text, "frame_num", p);
        fm.sliceKind = parseStringAfter(text, "slice_kind", p);
        m.frames.push_back(fm);
        ++p;
    }
    return m;
}

struct NalSeg {
    std::size_t start = 0;
    std::size_t end = 0;
    uint8_t type = 0;
};

std::size_t startCodeLen(const std::vector<uint8_t>& b, std::size_t pos) {
    if (pos + 4 <= b.size() && b[pos] == 0 && b[pos + 1] == 0 && b[pos + 2] == 0 && b[pos + 3] == 1) return 4;
    if (pos + 3 <= b.size() && b[pos] == 0 && b[pos + 1] == 0 && b[pos + 2] == 1) return 3;
    return 0;
}

std::vector<NalSeg> findNals(const std::vector<uint8_t>& b) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 < b.size();) {
        const std::size_t len = startCodeLen(b, i);
        if (len) {
            starts.push_back(i);
            i += len;
        } else {
            ++i;
        }
    }
    std::vector<NalSeg> out;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::size_t hdr = starts[i] + startCodeLen(b, starts[i]);
        if (hdr >= b.size()) throw std::runtime_error("truncated NAL header");
        out.push_back({starts[i], (i + 1 < starts.size()) ? starts[i + 1] : b.size(),
                       static_cast<uint8_t>(b[hdr] & 0x1f)});
    }
    return out;
}

uint8_t expand5(uint16_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t expand6(uint16_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

std::array<uint8_t, 3> rgb565ToRgb(uint16_t px) {
    return {expand5((px >> 11) & 0x1f), expand6((px >> 5) & 0x3f), expand5(px & 0x1f)};
}

std::array<uint8_t, 3> rgb888ToRgb565Expanded(uint8_t r, uint8_t g, uint8_t b) {
    return rgb565ToRgb(static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
}

template <typename T>
int signExtend(T value, int bits) {
    const int mask = 1 << (bits - 1);
    const int full = 1 << bits;
    int v = static_cast<int>(value) & (full - 1);
    return (v & mask) ? (v - full) : v;
}

struct FrameStageCycles {
    uint64_t vcl_arrive = 0;     // cycle when stub_busy rises (VCL begins)
    uint64_t paint_start = 0;    // cycle when fs_wr_reset fires (parse done)
    uint64_t paint_end = 0;      // cycle when fs_swap fires (paint done)
    uint64_t parse_cycles() const { return paint_start - vcl_arrive; }
    uint64_t paint_cycles() const { return paint_end - paint_start; }
};

struct Mb0Trace {
    bool valid = false;
    uint64_t cycle = 0;
    int qp = 0;
    int totalCoeff = 0;
    int trailingOnes = 0;
    int dc = 0;
    int csum = 0;
    std::array<int, 16> coeff{};
    std::array<int, 16> dequant{};
    std::array<int, 16> idct{};
    std::array<int, 16> recon{};
};

struct InterMbCapture {
    int frame = 0;
    int mbX = 0;
    int mbY = 0;
    bool pSkip = false;
    int partMode = 0;
    std::array<uint8_t, 256> predY{};
    std::array<uint8_t, 64> predU{};
    std::array<uint8_t, 64> predV{};
};

class Sim {
public:
    explicit Sim(int w, int h) : width(w), height(h), framePixels(static_cast<std::size_t>(w) * h) {}

    Vstream_path_full_frame_tb top{};
    uint64_t cycles = 0;
    int width = 0;
    int height = 0;
    std::size_t framePixels = 0;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> cur;
    std::vector<uint8_t> nativeCandidate;
    std::vector<InterMbCapture> interCaptures;
    std::size_t nativeFrameCount = 0;
    std::size_t nativeFrameBytes = 0;
    std::size_t nativeI420DpbWrites = 0;
    int ignoredInterCaptures = 0;
    bool nativeInterValidQ = false;
    Mb0Trace mb0Trace;
    std::vector<FrameStageCycles> stageCycles;
    bool prevStubBusy = false;
    uint64_t injectionCycles = 0;
    uint64_t nonVclIdleCycles = 0;
    uint64_t resetCycles = 0;

    void initNativeCandidate(std::size_t frameCount) {
        nativeFrameCount = frameCount;
        nativeFrameBytes = static_cast<std::size_t>(width) * height * 3 / 2;
        nativeCandidate.assign(frameCount * nativeFrameBytes, 0);
    }

    void writeNativeInterMb(const InterMbCapture& cap) {
        if (nativeCandidate.empty()) return;
        const int mbW = width / 16;
        const int mbH = height / 16;
        if (cap.frame < 0 || cap.mbX < 0 || cap.mbY < 0 ||
            cap.mbX >= mbW || cap.mbY >= mbH)
            throw std::runtime_error("native inter MB capture outside coded frame");
        const std::size_t yBytes = static_cast<std::size_t>(width) * height;
        const int cw = width / 2;
        const int ch = height / 2;
        const std::size_t cBytes = static_cast<std::size_t>(cw) * ch;
        const std::size_t frameBytes = yBytes + cBytes * 2;
        const std::size_t base = static_cast<std::size_t>(cap.frame) * frameBytes;
        if (base + frameBytes > nativeCandidate.size())
            return;
        const std::size_t yBase = base;
        const std::size_t uBase = base + yBytes;
        const std::size_t vBase = uBase + cBytes;
        for (int yy = 0; yy < 16; ++yy) {
            for (int xx = 0; xx < 16; ++xx) {
                const int x = cap.mbX * 16 + xx;
                const int y = cap.mbY * 16 + yy;
                nativeCandidate[yBase + static_cast<std::size_t>(y) * width + x] =
                    cap.predY[static_cast<std::size_t>(yy * 16 + xx)];
            }
        }
        for (int yy = 0; yy < 8; ++yy) {
            for (int xx = 0; xx < 8; ++xx) {
                const int x = cap.mbX * 8 + xx;
                const int y = cap.mbY * 8 + yy;
                const std::size_t ci = static_cast<std::size_t>(y) * cw + x;
                const std::size_t bi = static_cast<std::size_t>(yy * 8 + xx);
                nativeCandidate[uBase + ci] = cap.predU[bi];
                nativeCandidate[vBase + ci] = cap.predV[bi];
            }
        }
    }

    void capturePosedge() {
        // Track decode_stub phase transitions for per-stage cycle accounting.
        if (top.stub_busy && !prevStubBusy) {
            FrameStageCycles fsc;
            fsc.vcl_arrive = cycles;
            stageCycles.push_back(fsc);
        }
        if (top.fs_wr_reset && !stageCycles.empty() && stageCycles.back().paint_start == 0) {
            stageCycles.back().paint_start = cycles;
        }
        if (top.fs_swap && !stageCycles.empty() && stageCycles.back().paint_end == 0) {
            stageCycles.back().paint_end = cycles;
        }
        prevStubBusy = top.stub_busy;

        if (top.recon_valid && !mb0Trace.valid) {
            mb0Trace.valid = true;
            mb0Trace.cycle = cycles;
            mb0Trace.qp = static_cast<int>(top.trace_slice_qp);
            mb0Trace.totalCoeff = static_cast<int>(top.trace_residual_tc);
            mb0Trace.trailingOnes = static_cast<int>(top.trace_residual_t1);
            mb0Trace.dc = signExtend(top.trace_residual_dc, 8);
            mb0Trace.csum = static_cast<int>(top.trace_residual_csum);
            for (int i = 0; i < 16; ++i) {
                mb0Trace.coeff[static_cast<std::size_t>(i)] = signExtend(top.trace_residual_coeff[i], 9);
                mb0Trace.dequant[static_cast<std::size_t>(i)] = signExtend(top.trace_idct_dequant[i], 29);
                mb0Trace.idct[static_cast<std::size_t>(i)] = signExtend(top.trace_idct_residual[i], 29);
                mb0Trace.recon[static_cast<std::size_t>(i)] = static_cast<int>(top.trace_recon_px[i]);
            }
        }
        // Native I420 DPB write tap: capture per-sample I420 data from the
        // reconstruction path (DPB fill for IDR, eventually inter writeback).
        if (top.native_i420_wr_en && !nativeCandidate.empty()) {
            const auto offset = static_cast<std::size_t>(top.native_i420_wr_offset);
            const int frame = static_cast<int>(top.native_i420_wr_frame);
            if (frame >= 0 && static_cast<std::size_t>(frame) < nativeFrameCount &&
                offset < nativeFrameBytes) {
                nativeCandidate[static_cast<std::size_t>(frame) * nativeFrameBytes + offset] =
                    static_cast<uint8_t>(top.native_i420_wr_data);
                ++nativeI420DpbWrites;
            }
        }
        if (top.native_inter_valid && !nativeInterValidQ) {
            InterMbCapture cap;
            cap.frame = static_cast<int>(top.native_inter_frame_idx);
            cap.mbX = static_cast<int>(top.native_inter_mb_x);
            cap.mbY = static_cast<int>(top.native_inter_mb_y);
            cap.pSkip = static_cast<bool>(top.native_inter_p_skip);
            cap.partMode = static_cast<int>(top.native_inter_part_mode);
            for (int i = 0; i < 256; ++i)
                cap.predY[static_cast<std::size_t>(i)] = static_cast<uint8_t>(top.native_inter_pred_y[i]);
            for (int i = 0; i < 64; ++i) {
                cap.predU[static_cast<std::size_t>(i)] = static_cast<uint8_t>(top.native_inter_pred_u[i]);
                cap.predV[static_cast<std::size_t>(i)] = static_cast<uint8_t>(top.native_inter_pred_v[i]);
            }
            if (cap.frame >= 0 && static_cast<std::size_t>(cap.frame) < nativeFrameCount) {
                writeNativeInterMb(cap);
                interCaptures.push_back(cap);
            } else {
                ++ignoredInterCaptures;
            }
        }
        nativeInterValidQ = static_cast<bool>(top.native_inter_valid);
        if (top.fs_wr_reset) cur.clear();
        if (!top.fs_wr_en) return;
        const auto rgb = rgb565ToRgb(static_cast<uint16_t>(top.fs_wr_pixel));
        cur.push_back(rgb[0]);
        cur.push_back(rgb[1]);
        cur.push_back(rgb[2]);
        if (cur.size() == framePixels * 3) {
            frames.push_back(cur);
            cur.clear();
        } else if (cur.size() > framePixels * 3) {
            throw std::runtime_error("captured more pixels than one frame before fs_wr_reset");
        }
    }

    void tick() {
        top.clk = 0;
        top.eval();
        top.clk = 1;
        top.eval();
        capturePosedge();
        top.clk = 0;
        top.eval();
        ++cycles;
    }

    void reset() {
        top.reset = 1;
        top.flush = 0;
        top.ioctl_download = 0;
        top.ioctl_wr = 0;
        top.ioctl_dout = 0;
        top.dpb_prefill_en = 0;
        top.dpb_prefill_addr = 0;
        top.dpb_prefill_data = 0;
        for (int i = 0; i < 8; ++i) tick();
        top.reset = 0;
        for (int i = 0; i < 4; ++i) tick();
        resetCycles = cycles;
    }

    // Pre-fill the DPB reference bank with real IDR frame data.
    // The h264_dpb_one_ref starts with current_base=BANK0, reference_base=BANK1.
    // After IDR fill + frame_done, banks swap: current=BANK1, reference=BANK0.
    // So for the FIRST P-slice, the reference is in BANK0.
    // We pre-fill BANK0 (addr 0..frameBytes-1) with the IDR frame.
    void prefillDpbReference(const std::vector<uint8_t>& goldenPlanes) {
        const std::size_t frameBytes = static_cast<std::size_t>(width) * height * 3 / 2;
        if (goldenPlanes.size() < frameBytes) {
            std::cerr << "WARNING: golden planes too small for DPB prefill ("
                      << goldenPlanes.size() << " < " << frameBytes << ")\n";
            return;
        }
        // Write frame 0 (IDR) into BANK0 of DPB SRAM
        for (std::size_t i = 0; i < frameBytes; ++i) {
            top.dpb_prefill_en = 1;
            top.dpb_prefill_addr = static_cast<uint32_t>(i);
            top.dpb_prefill_data = goldenPlanes[i];
            tick();
        }
        top.dpb_prefill_en = 0;
        dpbPrefilled = true;
        std::cout << "DPB_PREFILL wrote " << frameBytes
                  << " bytes of IDR reference into BANK0\n";
    }

    bool dpbPrefilled = false;

    void feedByte(uint8_t v) {
        top.ioctl_download = 1;
        top.ioctl_dout = v;
        top.ioctl_wr = 1;
        tick();
        top.ioctl_wr = 0;
        tick();
    }

    bool waitForBytes(uint32_t want, int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            if (top.bytes_seen >= want) return true;
            tick();
        }
        return top.bytes_seen >= want;
    }

    bool waitForFrames(uint16_t want, int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            if (top.stub_frames >= want && frames.size() >= want && !top.stub_busy) return true;
            tick();
        }
        return top.stub_frames >= want && frames.size() >= want && !top.stub_busy;
    }
};

void printArray(std::ostream& out, const std::array<int, 16>& a) {
    out << "[";
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i) out << ",";
        out << a[i];
    }
    out << "]";
}

void printMb0Trace(const Mb0Trace& t) {
    if (!t.valid) {
        std::cout << "MB0_PIPELINE_TRACE missing\n";
        return;
    }
    std::cout << "MB0_PIPELINE_TRACE cycle=" << t.cycle
              << " qp=" << t.qp
              << " total_coeff=" << t.totalCoeff
              << " trailing_ones=" << t.trailingOnes
              << " residual_dc=" << t.dc
              << " residual_csum=0x" << std::hex << std::setw(2) << std::setfill('0') << t.csum
              << std::dec << std::setfill(' ')
              << " coeff=";
    printArray(std::cout, t.coeff);
    std::cout << " dequant=";
    printArray(std::cout, t.dequant);
    std::cout << " idct=";
    printArray(std::cout, t.idct);
    std::cout << " recon=";
    printArray(std::cout, t.recon);
    std::cout << "\n";
}

void writeTraceJson(const std::string& path, const Mb0Trace& t) {
    if (path.empty()) return;
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write MB0 trace JSON: " + path);
    out << "{\n";
    out << "  \"format\": \"misterplex.p3.mb0_pipeline_trace.v1\",\n";
    out << "  \"colorspace\": \"I420_NATIVE\",\n";
    out << "  \"valid\": " << (t.valid ? "true" : "false");
    if (t.valid) {
        out << ",\n  \"cycle\": " << t.cycle << ",\n";
        out << "  \"qp\": " << t.qp << ",\n";
        out << "  \"total_coeff\": " << t.totalCoeff << ",\n";
        out << "  \"trailing_ones\": " << t.trailingOnes << ",\n";
        out << "  \"residual_dc\": " << t.dc << ",\n";
        out << "  \"residual_csum\": " << t.csum << ",\n";
        out << "  \"coefficients_zigzag\": ";
        printArray(out, t.coeff);
        out << ",\n  \"dequant\": ";
        printArray(out, t.dequant);
        out << ",\n  \"idct\": ";
        printArray(out, t.idct);
        out << ",\n  \"recon\": ";
        printArray(out, t.recon);
    }
    out << "\n}\n";
}

void printStageCycles(const Sim& sim, int mbsPerFrame) {
    uint64_t totalParse = 0, totalPaint = 0;
    for (std::size_t f = 0; f < sim.stageCycles.size(); ++f) {
        const auto& sc = sim.stageCycles[f];
        const uint64_t parse = sc.parse_cycles();
        const uint64_t paint = sc.paint_cycles();
        totalParse += parse;
        totalPaint += paint;
        std::cout << "STAGE_CYCLES frame=" << f
                  << " parse_cycles=" << parse
                  << " paint_cycles=" << paint
                  << " parse_per_mb=" << std::fixed << std::setprecision(3)
                  << (static_cast<double>(parse) / mbsPerFrame)
                  << " paint_per_mb="
                  << (static_cast<double>(paint) / mbsPerFrame) << "\n";
    }
    const int totalMbs = mbsPerFrame * static_cast<int>(sim.stageCycles.size());
    std::cout << "STAGE_CYCLES_TOTAL"
              << " frames=" << sim.stageCycles.size()
              << " parse_total=" << totalParse
              << " paint_total=" << totalPaint
              << " injection_total=" << sim.injectionCycles
              << " nonvcl_idle_total=" << sim.nonVclIdleCycles
              << " reset_total=" << sim.resetCycles
              << " parse_per_mb=" << std::fixed << std::setprecision(3)
              << (totalMbs > 0 ? static_cast<double>(totalParse) / totalMbs : 0.0)
              << " paint_per_mb="
              << (totalMbs > 0 ? static_cast<double>(totalPaint) / totalMbs : 0.0)
              << " injection_per_mb="
              << (totalMbs > 0 ? static_cast<double>(sim.injectionCycles) / totalMbs : 0.0) << "\n";
}

const char* partModeName(bool pSkip, int mode) {
    if (pSkip) return "P_Skip";
    switch (mode) {
    case 0: return "P_L0_16x16";
    case 1: return "P_L0_16x8";
    case 2: return "P_L0_8x16";
    case 3: return "P_8x8";
    case 4: return "P_8x8ref0";
    default: return "P_UNKNOWN";
    }
}

template <std::size_t N>
void writeByteArray(std::ostream& out, const std::array<uint8_t, N>& a) {
    out << "[";
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i) out << ",";
        out << static_cast<int>(a[i]);
    }
    out << "]";
}

void writeInterMetadataJson(const std::string& path, const std::vector<InterMbCapture>& caps,
                            int width, int height) {
    if (path.empty()) return;
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write inter metadata JSON: " + path);
    out << "{\n";
    out << "  \"format\": \"misterplex.p3.inter_mb_metadata.v1\",\n";
    out << "  \"producer\": \"stream_path_full_frame_tb.native_inter_candidate\",\n";
    out << "  \"candidate\": {\n";
    out << "    \"colorspace\": \"I420_NATIVE\",\n";
    out << "    \"h264_loop_filter\": \"disabled\",\n";
    out << "    \"reconstruction_stage\": \"mc_prediction_only_pre_deblock_no_residual_add\",\n";
    out << "    \"reference_picture_state\": \"diagnostic_filtered_reference_via_deblock_writeback_ctrl\",\n";
    out << "    \"reference_picture_source\": \"generated_i420_pattern_not_decoded_prior_frame\",\n";
    out << "    \"conformance_scope\": \"MC arithmetic and parser-to-DPB plumbing only; not end-to-end H.264 P reconstruction\"\n";
    out << "  },\n";
    out << "  \"geometry\": {\"width\": " << width << ", \"height\": " << height << "},\n";
    out << "  \"macroblocks\": [\n";
    for (std::size_t i = 0; i < caps.size(); ++i) {
        const auto& c = caps[i];
        const int mbIndex = c.mbY * (width / 16) + c.mbX;
        out << "    {\"frame_index\": " << c.frame
            << ", \"mb_index\": " << mbIndex
            << ", \"mb_x\": " << c.mbX
            << ", \"mb_y\": " << c.mbY
            << ", \"mb_type\": \"" << partModeName(c.pSkip, c.partMode) << "\""
            << ", \"part_mode\": " << c.partMode
            << ", \"ref_idx_l0\": 0"
            << ", \"mv_l0\": {\"x\": 0, \"y\": 0}"
            << ", \"pred_y\": ";
        writeByteArray(out, c.predY);
        out << ", \"pred_u\": ";
        writeByteArray(out, c.predU);
        out << ", \"pred_v\": ";
        writeByteArray(out, c.predV);
        out << "}";
        if (i + 1 != caps.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

struct PlaneStats {
    uint64_t exact = 0;
    uint64_t sumAbs = 0;
    uint64_t total = 0;
    uint8_t maxAbs = 0;
    double mae = 0.0;
};

struct BadPixel {
    bool valid = false;
    std::size_t frame = 0;
    int plane = 0;
    int x = 0;
    int y = 0;
    int got = 0;
    int ref = 0;
    int abs = 0;
};

struct FrameCompareStats {
    std::array<PlaneStats, 3> plane;
    std::array<BadPixel, 3> firstBad;
    int mbExact = 0;
    int mbTotal = 0;
};

struct CompareResult {
    bool exact = true;
    int firstBadFrame = -1;
    BadPixel firstBad;
    std::vector<FrameCompareStats> frames;
    std::vector<uint8_t> candidateI420;
};

uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

std::array<uint8_t, 3> rgbToYuv(uint8_t r, uint8_t g, uint8_t b) {
    return {
        clamp8((77 * r + 150 * g + 29 * b + 128) >> 8),
        clamp8((-43 * r - 85 * g + 128 * b + 32896) >> 8),
        clamp8((128 * r - 107 * g - 21 * b + 32896) >> 8),
    };
}

struct PlaneView {
    std::size_t offset = 0;
    std::size_t count = 0;
    int width = 0;
    int height = 0;
};

std::array<PlaneView, 3> framePlaneViews(std::size_t frame, int width, int height) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    const int uvW = width / 2;
    const int uvH = height / 2;
    const std::size_t uvPixels = static_cast<std::size_t>(uvW) * uvH;
    const std::size_t frameBytes = pixels + uvPixels * 2;
    const std::size_t base = frame * frameBytes;
    return {{
        {base, pixels, width, height},
        {base + pixels, uvPixels, uvW, uvH},
        {base + pixels + uvPixels, uvPixels, uvW, uvH},
    }};
}

bool mbExactAllPlanes(const std::vector<uint8_t>& got, const std::vector<uint8_t>& ref,
                      std::size_t frame, int width, int height, int mbX, int mbY) {
    const auto views = framePlaneViews(frame, width, height);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int px = mbX * 16 + x;
            const int py = mbY * 16 + y;
            if (px >= width || py >= height) continue;
            const std::size_t i = views[0].offset + static_cast<std::size_t>(py) * width + px;
            if (got[i] != ref[i]) return false;
        }
    }
    const int cw = width / 2;
    const int ch = height / 2;
    for (int p = 1; p < 3; ++p) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int px = mbX * 8 + x;
                const int py = mbY * 8 + y;
                if (px >= cw || py >= ch) continue;
                const std::size_t i = views[static_cast<std::size_t>(p)].offset +
                                      static_cast<std::size_t>(py) * cw + px;
                if (got[i] != ref[i]) return false;
            }
        }
    }
    return true;
}

void writeCandidateFrameI420(const std::vector<uint8_t>& rgb,
                             std::vector<uint8_t>& candidate,
                             std::size_t frame,
                             int width,
                             int height) {
    const std::size_t rgbBytes = static_cast<std::size_t>(width) * height * 3;
    if (rgb.size() != rgbBytes) throw std::runtime_error("captured frame has wrong byte count");
    const auto views = framePlaneViews(frame, width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t px = static_cast<std::size_t>(y) * width + x;
            const auto yuv = rgbToYuv(rgb[px * 3 + 0], rgb[px * 3 + 1], rgb[px * 3 + 2]);
            candidate[views[0].offset + px] = yuv[0];
        }
    }
    for (int cy = 0; cy < height / 2; ++cy) {
        for (int cx = 0; cx < width / 2; ++cx) {
            int sumU = 0;
            int sumV = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = cx * 2 + dx;
                    const int y = cy * 2 + dy;
                    const std::size_t px = static_cast<std::size_t>(y) * width + x;
                    const auto yuv = rgbToYuv(rgb[px * 3 + 0], rgb[px * 3 + 1], rgb[px * 3 + 2]);
                    sumU += yuv[1];
                    sumV += yuv[2];
                }
            }
            const std::size_t cpx = static_cast<std::size_t>(cy) * (width / 2) + cx;
            candidate[views[1].offset + cpx] = static_cast<uint8_t>((sumU + 2) / 4);
            candidate[views[2].offset + cpx] = static_cast<uint8_t>((sumV + 2) / 4);
        }
    }
}

CompareResult compareFrames(const std::vector<std::vector<uint8_t>>& got,
                            const std::vector<uint8_t>& ref,
                            int width, int height) {
    if ((width & 1) || (height & 1)) throw std::runtime_error("I420 comparison requires even geometry");
    const std::size_t lumaPixels = static_cast<std::size_t>(width) * height;
    const std::size_t chromaPixels = static_cast<std::size_t>(width / 2) * (height / 2);
    const std::size_t frameBytes = lumaPixels + chromaPixels * 2;
    const std::size_t refFrames = ref.size() / frameBytes;
    if (ref.empty() || (ref.size() % frameBytes) != 0)
        throw std::runtime_error("golden I420 size is not a whole number of frames");
    CompareResult result;
    const char* names = "YUV";
    result.candidateI420.assign(got.size() * frameBytes, 0);
    for (std::size_t f = 0; f < got.size(); ++f) {
        writeCandidateFrameI420(got[f], result.candidateI420, f, width, height);
        FrameCompareStats frameStats;
        const auto views = framePlaneViews(f, width, height);
        for (int p = 0; p < 3; ++p) {
            const PlaneView& v = views[static_cast<std::size_t>(p)];
            for (std::size_t i = 0; i < v.count; ++i) {
                PlaneStats& st = frameStats.plane[static_cast<std::size_t>(p)];
                const uint8_t g = result.candidateI420[v.offset + i];
                const uint8_t r = ref[v.offset + i];
                const uint8_t d = static_cast<uint8_t>(g > r ? g - r : r - g);
                st.exact += (d == 0);
                st.sumAbs += d;
                st.total += 1;
                st.maxAbs = std::max(st.maxAbs, d);
                if (d != 0 && !frameStats.firstBad[static_cast<std::size_t>(p)].valid) {
                    const int x = static_cast<int>(i % static_cast<std::size_t>(v.width));
                    const int y = static_cast<int>(i / static_cast<std::size_t>(v.width));
                    frameStats.firstBad[static_cast<std::size_t>(p)] =
                        {true, f, p, x, y, static_cast<int>(g), static_cast<int>(r), static_cast<int>(d)};
                }
                if (d != 0 && !result.firstBad.valid) {
                    const int x = static_cast<int>(i % static_cast<std::size_t>(v.width));
                    const int y = static_cast<int>(i / static_cast<std::size_t>(v.width));
                    result.firstBad = {true, f, p, x, y, static_cast<int>(g), static_cast<int>(r), static_cast<int>(d)};
                    result.firstBadFrame = static_cast<int>(f);
                }
            }
        }
        for (int p = 0; p < 3; ++p) {
            PlaneStats& st = frameStats.plane[static_cast<std::size_t>(p)];
            st.mae = static_cast<double>(st.sumAbs) / static_cast<double>(st.total);
            const BadPixel& first = frameStats.firstBad[static_cast<std::size_t>(p)];
            std::cout << "FULL_FRAME_COMPARE raw frame=" << f
                      << " colorspace=I420_FROM_RGB565"
                      << " plane=" << names[p]
                      << " exact=" << st.exact
                      << " pixels=" << st.total
                      << " mae=" << std::fixed << std::setprecision(6) << st.mae
                      << " max_abs=" << static_cast<int>(st.maxAbs) << "\n";
            if (first.valid) {
                const int mbDiv = (p == 0) ? 16 : 8;
                std::cout << "FULL_FRAME_COMPARE first_bad frame=" << f
                          << " colorspace=I420_FROM_RGB565"
                          << " plane=" << names[p]
                          << " x=" << first.x
                          << " y=" << first.y
                          << " mb_x=" << (first.x / mbDiv)
                          << " mb_y=" << (first.y / mbDiv)
                          << " pixel_in_mb_x=" << (first.x % mbDiv)
                          << " pixel_in_mb_y=" << (first.y % mbDiv)
                          << " got=" << first.got
                          << " ref=" << first.ref
                          << " abs=" << first.abs << "\n";
            }
        }
        frameStats.mbTotal = ((width + 15) / 16) * ((height + 15) / 16);
        for (int my = 0; my < (height + 15) / 16; ++my) {
            for (int mx = 0; mx < (width + 15) / 16; ++mx) {
                frameStats.mbExact += mbExactAllPlanes(result.candidateI420, ref, f, width, height, mx, my) ? 1 : 0;
            }
        }
        std::cout << "FULL_FRAME_COMPARE mb frame=" << f
                  << " exact=" << frameStats.mbExact
                  << " total=" << frameStats.mbTotal << "\n";
        result.frames.push_back(frameStats);
    }
    result.exact = (result.firstBadFrame < 0) && (got.size() == refFrames);
    return result;
}

void writeJsonReport(const std::string& path, const Args& args, const SequenceMeta& seq,
                     const CompareResult& cr, int nals, int idr, int p, std::size_t bytes,
                     uint64_t cycles, const Sim& sim) {
    if (path.empty()) return;
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write compare JSON: " + path);
    const char* names[3] = {"Y", "U", "V"};
    out << "{\n";
    out << "  \"format\": \"misterplex.p3.frame_planes_compare.v1\",\n";
    out << "  \"source\": {\"path\": \"" << args.annexb << "\", \"bytes\": " << bytes
        << ", \"sha256\": \"" << args.sourceSha256 << "\"},\n";
    out << "  \"sequence_manifest\": \"" << args.sequenceJson << "\",\n";
    out << "  \"golden_manifest\": \"" << args.goldenManifest << "\",\n";
    out << "  \"golden_planes\": \"" << args.goldenPlanes << "\",\n";
    out << "  \"geometry\": {\"width\": " << args.width << ", \"height\": " << args.height
        << ", \"colorspace\": \"I420_FROM_RGB565\", \"planes\": ["
        << "{\"plane\": \"Y\", \"width\": " << args.width << ", \"height\": " << args.height << "}, "
        << "{\"plane\": \"U\", \"width\": " << (args.width / 2) << ", \"height\": " << (args.height / 2) << "}, "
        << "{\"plane\": \"V\", \"width\": " << (args.width / 2) << ", \"height\": " << (args.height / 2) << "}]},\n";
    out << "  \"summary\": {\"nals\": " << nals << ", \"idr\": " << idr << ", \"p\": " << p
        << ", \"cycles\": " << cycles << ", \"first_bad_frame\": " << cr.firstBadFrame
        << ", \"first_bad\": ";
    if (cr.firstBad.valid) {
        const int mbDiv = (cr.firstBad.plane == 0) ? 16 : 8;
        out << "{\"frame_index\": " << cr.firstBad.frame << ", \"plane\": \"" << names[cr.firstBad.plane]
            << "\", \"x\": " << cr.firstBad.x << ", \"y\": " << cr.firstBad.y
            << ", \"mb_x\": " << (cr.firstBad.x / mbDiv) << ", \"mb_y\": " << (cr.firstBad.y / mbDiv)
            << ", \"pixel_in_mb_x\": " << (cr.firstBad.x % mbDiv)
            << ", \"pixel_in_mb_y\": " << (cr.firstBad.y % mbDiv)
            << ", \"got\": " << cr.firstBad.got << ", \"ref\": " << cr.firstBad.ref
            << ", \"abs\": " << cr.firstBad.abs << "}";
    } else {
        out << "null";
    }
    out << ", \"strict_pass\": " << (cr.exact ? "true" : "false")
        << ", \"expectation\": \"" << (args.expectRed ? "red" : "strict") << "\"},\n";
    out << "  \"frames\": [\n";
    for (std::size_t f = 0; f < cr.frames.size(); ++f) {
        const int frameNum = f < seq.frames.size() ? seq.frames[f].frameNum : static_cast<int>(f);
        const std::string kind = f < seq.frames.size() ? seq.frames[f].sliceKind : "?";
        out << "    {\"frame_index\": " << f << ", \"frame_num\": " << frameNum
            << ", \"slice_kind\": \"" << kind << "\", \"mb_exact\": " << cr.frames[f].mbExact
            << ", \"mb_total\": " << cr.frames[f].mbTotal << ", \"planes\": [";
        for (int pi = 0; pi < 3; ++pi) {
            const auto& st = cr.frames[f].plane[static_cast<std::size_t>(pi)];
            if (pi) out << ", ";
            const BadPixel& first = cr.frames[f].firstBad[static_cast<std::size_t>(pi)];
            const int pw = (pi == 0) ? args.width : args.width / 2;
            const int ph = (pi == 0) ? args.height : args.height / 2;
            out << "{\"plane\": \"" << names[pi] << "\", \"width\": " << pw
                << ", \"height\": " << ph << ", \"exact_pixels\": " << st.exact
                << ", \"total_pixels\": " << st.total << ", \"mae\": "
                << std::fixed << std::setprecision(6) << st.mae
                << ", \"max_abs\": " << static_cast<int>(st.maxAbs) << ", \"first_bad\": ";
            if (first.valid) {
                const int mbDiv = (pi == 0) ? 16 : 8;
                out << "{\"x\": " << first.x << ", \"y\": " << first.y
                    << ", \"mb_x\": " << (first.x / mbDiv) << ", \"mb_y\": " << (first.y / mbDiv)
                    << ", \"pixel_in_mb_x\": " << (first.x % mbDiv)
                    << ", \"pixel_in_mb_y\": " << (first.y % mbDiv)
                    << ", \"got\": " << first.got << ", \"ref\": " << first.ref
                    << ", \"abs\": " << first.abs << "}";
            } else {
                out << "null";
            }
            out << "}";
        }
        out << "]}";
        if (f + 1 != cr.frames.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";
    // Per-stage cycle accounting
    uint64_t totalParse = 0, totalPaint = 0;
    out << "  \"stage_cycles\": {\n";
    out << "    \"method\": \"Per-frame phase tracking via stub_busy/fs_wr_reset/fs_swap transitions in Verilator sim\",\n";
    out << "    \"injection_cycles\": " << sim.injectionCycles << ",\n";
    out << "    \"nonvcl_idle_cycles\": " << sim.nonVclIdleCycles << ",\n";
    out << "    \"reset_cycles\": " << sim.resetCycles << ",\n";
    out << "    \"frames\": [\n";
    for (std::size_t f = 0; f < sim.stageCycles.size(); ++f) {
        const auto& sc = sim.stageCycles[f];
        const uint64_t parse = sc.parse_cycles();
        const uint64_t paint = sc.paint_cycles();
        totalParse += parse;
        totalPaint += paint;
        out << "      {\"frame\": " << f
            << ", \"parse_cycles\": " << parse
            << ", \"paint_cycles\": " << paint << "}";
        if (f + 1 != sim.stageCycles.size()) out << ",";
        out << "\n";
    }
    out << "    ],\n";
    out << "    \"parse_total\": " << totalParse << ",\n";
    out << "    \"paint_total\": " << totalPaint << "\n";
    out << "  }\n";
    out << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        const Args args = parseArgs(argc, argv);
        const auto annexb = readBytes(args.annexb);
        const auto golden = readBytes(args.goldenPlanes);
        (void)readBytes(args.goldenManifest);
        const auto sequenceText = readBytes(args.sequenceJson);
        const SequenceMeta seq = parseSequenceManifest(std::string(sequenceText.begin(), sequenceText.end()));
        if ((args.width & 1) || (args.height & 1))
            throw std::runtime_error("I420 comparison requires even geometry");
        const std::size_t yBytes = static_cast<std::size_t>(args.width) * args.height;
        const std::size_t uvBytes = static_cast<std::size_t>(args.width / 2) * (args.height / 2);
        const std::size_t frameBytes = yBytes + uvBytes * 2;
        if (golden.empty() || (golden.size() % frameBytes) != 0)
            throw std::runtime_error("golden I420 size is not a whole number of frames");
        const std::size_t refFrames = golden.size() / frameBytes;
        const auto nals = findNals(annexb);
        if (nals.size() < 2) throw std::runtime_error("fixture must contain >=2 NALs");

        int idr = 0, p = 0, vcl = 0;
        for (const auto& n : nals) {
            if (n.type == 5) { ++idr; ++vcl; }
            if (n.type == 1) { ++p; ++vcl; }
        }
        if (vcl < 2 || idr < 1 || p < 1)
            throw std::runtime_error("full-frame gate requires IDR plus P-frame VCL NALs");
        if (static_cast<std::size_t>(vcl) != refFrames)
            throw std::runtime_error("reference frame count does not match VCL count");
        if (seq.sha256 != args.sourceSha256 || seq.bytes != static_cast<int>(annexb.size()))
            throw std::runtime_error("sequence manifest source hash/size does not match bitstream");
        if (seq.width != args.width || seq.height != args.height)
            throw std::runtime_error("sequence manifest geometry does not match ffprobe geometry");
        if (seq.nalCount != static_cast<int>(nals.size()) || seq.vcl != vcl ||
            seq.idr != idr || seq.nonIdr != p || seq.pSlices != p)
            throw std::runtime_error("sequence manifest NAL/VCL counts do not match bitstream");
        if (seq.frames.size() != refFrames)
            throw std::runtime_error("sequence manifest VCL metadata count does not match reference frame count");

        Sim sim(args.width, args.height);
        sim.initNativeCandidate(refFrames);
        sim.reset();

        // Pre-fill DPB reference bank with real IDR frame data.
        // This gives MC an honest reference for P-slice prediction.
        sim.prefillDpbReference(golden);
        // Prefill ticks are setup cost, not decode; reset cycle counter.
        sim.cycles = 0;

        uint32_t fed = 0;
        uint16_t expectedFrames = 0;
        for (const auto& n : nals) {
            const uint64_t injectStart = sim.cycles;
            for (std::size_t i = n.start; i < n.end; ++i) sim.feedByte(annexb[i]);
            sim.injectionCycles += (sim.cycles - injectStart);
            fed += static_cast<uint32_t>(n.end - n.start);
            sim.top.ioctl_download = 0;
            sim.tick();
            if (!sim.waitForBytes(fed, 20000))
                throw std::runtime_error("scanner did not drain bytes after NAL type " + std::to_string(n.type));
            if (n.type == 5 || n.type == 1) {
                ++expectedFrames;
                const int frameWaitCycles = std::max(200000, args.width * args.height * 3);
                if (!sim.waitForFrames(expectedFrames, frameWaitCycles))
                    throw std::runtime_error("stream_path did not emit frame " + std::to_string(expectedFrames));
            } else {
                const uint64_t idleStart = sim.cycles;
                for (int i = 0; i < 256; ++i) sim.tick();
                sim.nonVclIdleCycles += (sim.cycles - idleStart);
            }
        }

        if (sim.frames.size() != refFrames)
            throw std::runtime_error("captured frame count does not match reference");
        if (sim.top.sps_width != args.width || sim.top.sps_height != args.height)
            throw std::runtime_error("SPS geometry does not match ffprobe geometry");

        printMb0Trace(sim.mb0Trace);
        writeTraceJson(args.traceJsonOut, sim.mb0Trace);

        // Degeneracy assertion (parent directive #18): MB0 IDCT must produce
        // non-zero residuals. If all 16 IDCT outputs are 0, the transform
        // never ran (all-zero coefficients) and recon == pred trivially.
        if (sim.mb0Trace.valid) {
            bool allIdctZero = true;
            for (int i = 0; i < 16; ++i) {
                if (sim.mb0Trace.idct[static_cast<std::size_t>(i)] != 0) {
                    allIdctZero = false;
                    break;
                }
            }
            if (allIdctZero) {
                std::cerr << "FAIL degeneracy: MB0 IDCT residual is all-zero — "
                             "transform was never exercised. Comparison is vacuous.\n";
                return 1;
            }
            // Also: recon must differ from prediction (128). If recon==pred for
            // all 16 pixels, the residual add had no effect.
            bool allRecon128 = true;
            for (int i = 0; i < 16; ++i) {
                if (sim.mb0Trace.recon[static_cast<std::size_t>(i)] != 128) {
                    allRecon128 = false;
                    break;
                }
            }
            if (allRecon128) {
                std::cerr << "FAIL degeneracy: MB0 recon is all-128 — "
                             "reconstruction did not change prediction.\n";
                return 1;
            }
        }

        const CompareResult cr = compareFrames(sim.frames, golden, args.width, args.height);
        if (!args.candidateI420Out.empty()) {
            std::ofstream cand(args.candidateI420Out, std::ios::binary);
            if (!cand) throw std::runtime_error("cannot write candidate I420: " + args.candidateI420Out);
            cand.write(reinterpret_cast<const char*>(cr.candidateI420.data()),
                       static_cast<std::streamsize>(cr.candidateI420.size()));
            if (!cand) throw std::runtime_error("short write candidate I420: " + args.candidateI420Out);
        }
        if (!args.nativeCandidateI420Out.empty()) {
            std::ofstream cand(args.nativeCandidateI420Out, std::ios::binary);
            if (!cand) throw std::runtime_error("cannot write native candidate I420: " + args.nativeCandidateI420Out);
            cand.write(reinterpret_cast<const char*>(sim.nativeCandidate.data()),
                       static_cast<std::streamsize>(sim.nativeCandidate.size()));
            if (!cand) throw std::runtime_error("short write native candidate I420: " + args.nativeCandidateI420Out);
        }
        writeInterMetadataJson(args.interMetadataOut, sim.interCaptures, args.width, args.height);

        // Degeneracy assertion (parent directive #18): DPB fetch must return
        // non-trivial data. If all 256 Y pixels in a prediction are identical,
        // the DPB fetch returned a constant fill (uninitialised/zero/stuck) and
        // any comparison against it is meaningless — it would pass trivially.
        int degenerateCaptures = 0;
        for (const auto& cap : sim.interCaptures) {
            bool allSame = true;
            const uint8_t first = cap.predY[0];
            for (int i = 1; i < 256; ++i) {
                if (cap.predY[static_cast<std::size_t>(i)] != first) {
                    allSame = false;
                    break;
                }
            }
            if (allSame) ++degenerateCaptures;
        }
        if (!sim.interCaptures.empty() && degenerateCaptures == static_cast<int>(sim.interCaptures.size())) {
            std::cerr << "FAIL degeneracy: ALL " << sim.interCaptures.size()
                      << " inter MB captures have constant Y prediction (value="
                      << static_cast<int>(sim.interCaptures[0].predY[0])
                      << "). DPB fetch returned no real data.\n";
            return 1;
        }
        if (degenerateCaptures > 0) {
            std::cout << "DEGENERACY_WARNING inter_captures_constant_y="
                      << degenerateCaptures << "/" << sim.interCaptures.size()
                      << " (partial DPB corruption)\n";
        }

        writeJsonReport(args.jsonOut, args, seq, cr, static_cast<int>(nals.size()), idr, p,
                        annexb.size(), sim.cycles, sim);
        const int mbsPerFrame = (args.width / 16) * (args.height / 16);
        printStageCycles(sim, mbsPerFrame);
        std::cout << "FULL_FRAME_COMPARE summary width=" << args.width
                  << " height=" << args.height
                  << " frames=" << sim.frames.size()
                  << " nals=" << nals.size()
                  << " idr=" << idr
                  << " p=" << p
                  << " bytes=" << annexb.size()
                  << " native_inter_mb_captures=" << sim.interCaptures.size()
                  << " native_inter_ignored=" << sim.ignoredInterCaptures
                  << " native_i420_dpb_writes=" << sim.nativeI420DpbWrites
                  << " cycles=" << sim.cycles
                  << " first_bad_frame=" << cr.firstBadFrame
                  << " strict_pass=" << (cr.exact ? 1 : 0)
                  << " mode=" << (args.expectRed ? "expect-red" : "strict") << "\n";

        if (args.expectRed) {
            if (cr.exact) {
                std::cerr << "FAIL full-frame expected-red: stream_path unexpectedly matched reference decoder\n";
                return 1;
            }
            std::cout << "OK full-frame expected-red: strict reference comparison diverged at frame "
                      << cr.firstBadFrame << "\n";
            return 0;
        }
        if (!cr.exact) {
            std::cerr << "FAIL full-frame strict: stream_path pixels differ from reference decoder at frame "
                      << cr.firstBadFrame << "\n";
            return 1;
        }
        std::cout << "OK full-frame strict: all I420 planes matched reference decoder\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL full-frame compare: " << e.what() << "\n";
        return 1;
    }
}
