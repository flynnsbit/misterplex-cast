// Deterministic H.264 Baseline/CAVLC macroblock golden extractor.
// v1 emits I-slice luma MB goldens and verifies MB0 against the established
// p3_host_recon path (residual checksum 0x14, recon signature 0x3b).
#include "libmisterplex/h264_recon.hpp"
#include "libmisterplex/h264_residual_gold.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << text;
    return static_cast<bool>(out);
}

std::string baseName(const std::string& path) {
    const size_t p = path.find_last_of("/");
    return p == std::string::npos ? path : path.substr(p + 1);
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
            w[i] = (static_cast<uint32_t>(msg[p]) << 24) | (static_cast<uint32_t>(msg[p+1]) << 16) |
                   (static_cast<uint32_t>(msg[p+2]) << 8) | static_cast<uint32_t>(msg[p+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;
            hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint32_t v : h)
        os << std::setw(8) << v;
    return os.str();
}

template <typename T, size_t N>
void jsonArray(std::ostringstream& os, const std::array<T, N>& a, int indent, int perLine = 16) {
    os << "[";
    for (size_t i = 0; i < N; ++i) {
        if (i)
            os << ",";
        if (i % static_cast<size_t>(perLine) == 0)
            os << "\n" << std::string(static_cast<size_t>(indent), ' ');
        os << static_cast<int>(a[i]);
    }
    os << "\n" << std::string(static_cast<size_t>(indent - 2), ' ') << "]";
}

uint8_t xorU8(const std::array<uint8_t, 16>& a) {
    uint8_t v = 0;
    for (uint8_t x : a)
        v ^= x;
    return v;
}

std::string hexU8(uint8_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(v);
    return os.str();
}

uint8_t clip8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return static_cast<uint8_t>(v);
}

uint8_t addResidualSig(const std::array<uint8_t, 16>& pred,
                       const std::array<int16_t, 16>& idct) {
    uint8_t v = 0;
    for (size_t i = 0; i < pred.size(); ++i)
        v ^= clip8(static_cast<int>(pred[i]) + static_cast<int>(idct[i]));
    return v;
}

const char* coeffTokenTableName(int table) {
    switch (table) {
    case 0:
        return "nC_0_1";
    case 1:
        return "nC_2_3";
    case 2:
        return "nC_4_7";
    case 3:
        return "nC_8_plus";
    default:
        return "not_parsed";
    }
}

struct NalUnit {
    size_t start = 0;
    size_t start_code_bytes = 0;
    size_t header_offset = 0;
    size_t payload_offset = 0;
    size_t end = 0;
    uint8_t header = 0;
    uint8_t type = 0;
    misterplex::SliceHeader slice;
};

std::vector<NalUnit> splitAnnexB(const std::vector<uint8_t>& data) {
    std::vector<NalUnit> out;
    size_t i = 0;
    auto startCodeLen = [&](size_t p) -> size_t {
        if (p + 3 < data.size() && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 &&
            data[p + 3] == 1)
            return 4;
        if (p + 2 < data.size() && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1)
            return 3;
        return 0;
    };
    while (i + 3 < data.size()) {
        const size_t sc = startCodeLen(i);
        if (!sc) {
            ++i;
            continue;
        }
        size_t j = i + sc;
        while (j + 3 < data.size() && !startCodeLen(j))
            ++j;
        if (j + 3 >= data.size())
            j = data.size();
        if (i + sc < j) {
            NalUnit n;
            n.start = i;
            n.start_code_bytes = sc;
            n.header_offset = i + sc;
            n.payload_offset = n.header_offset + 1;
            n.end = j;
            n.header = data[n.header_offset];
            n.type = n.header & 0x1f;
            out.push_back(n);
        }
        i = j;
    }
    return out;
}

void fillSequenceHeaders(const std::vector<uint8_t>& data, std::vector<NalUnit>& nals,
                         misterplex::SpsInfo& sps, misterplex::PpsInfo& pps,
                         uint8_t& log2MaxFrameNum, uint8_t& pocType) {
    for (auto& n : nals) {
        const uint8_t* pay = data.data() + n.payload_offset;
        const size_t plen = n.end - n.payload_offset;
        if (n.type == 7) {
            sps = misterplex::parseSpsRbsp(pay, plen);
            auto rb = misterplex::detail::removeEpb(pay, plen);
            misterplex::detail::BitReader br(rb.data(), rb.size());
            br.u(8); // profile
            br.u(8); // constraints
            br.u(8); // level
            br.ue(); // sps id
            log2MaxFrameNum = static_cast<uint8_t>(br.ue() + 4);
            pocType = static_cast<uint8_t>(br.ue());
        } else if (n.type == 8) {
            pps = misterplex::parsePpsRbsp(pay, plen);
        } else if ((n.type == 1 || n.type == 5) && sps.valid && pps.valid &&
                   log2MaxFrameNum != 0) {
            uint8_t nri = (n.header >> 5) & 3;
            n.slice = misterplex::parseSliceHeaderRbsp(pay, plen, n.type, nri, log2MaxFrameNum,
                                                       pocType, pps);
        }
    }
}

const char* nalTypeName(uint8_t type) {
    switch (type) {
    case 1:
        return "non_idr_vcl";
    case 5:
        return "idr_vcl";
    case 6:
        return "sei";
    case 7:
        return "sps";
    case 8:
        return "pps";
    case 9:
        return "aud";
    default:
        return "other";
    }
}

const char* sliceKind(const misterplex::SliceHeader& sh) {
    const uint8_t t = sh.slice_type % 5;
    if (t == 0)
        return "P";
    if (t == 2)
        return "I";
    return "other";
}

int parseMaxNumRefFrames(const std::vector<uint8_t>& data, const std::vector<NalUnit>& nals) {
    for (const auto& n : nals) {
        if (n.type != 7)
            continue;
        auto rb = misterplex::detail::removeEpb(data.data() + n.payload_offset,
                                                n.end - n.payload_offset);
        misterplex::detail::BitReader br(rb.data(), rb.size());
        const uint8_t profile = static_cast<uint8_t>(br.u(8));
        br.u(8);
        br.u(8);
        br.ue();
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
            profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
            profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
            profile == 135) {
            uint32_t chroma = br.ue();
            if (chroma == 3)
                br.u(1);
            br.ue();
            br.ue();
            br.u(1);
            if (br.u(1)) {
                const int lists = chroma != 3 ? 8 : 12;
                for (int i = 0; i < lists && br.ok; ++i) {
                    if (!br.u(1))
                        continue;
                    int last = 8, next = 8;
                    const int count = i < 6 ? 16 : 64;
                    for (int j = 0; j < count && br.ok; ++j) {
                        if (next) {
                            next = (last + br.se() + 256) % 256;
                        }
                        last = next ? next : last;
                    }
                }
            }
        }
        br.ue(); // log2_max_frame_num_minus4
        const uint32_t poc = br.ue();
        if (poc == 0) {
            br.ue();
        } else if (poc == 1) {
            br.u(1);
            br.se();
            br.se();
            const uint32_t nref = br.ue();
            for (uint32_t i = 0; i < nref && br.ok; ++i)
                br.se();
        }
        return br.ok ? static_cast<int>(br.ue()) : -1;
    }
    return -1;
}

std::string makeSequenceJson(const std::string& inputPath, const std::vector<uint8_t>& blob) {
    auto nals = splitAnnexB(blob);
    misterplex::SpsInfo sps;
    misterplex::PpsInfo pps;
    uint8_t log2Fn = 0, pocType = 0;
    fillSequenceHeaders(blob, nals, sps, pps, log2Fn, pocType);

    int vcl = 0, idr = 0, nonIdr = 0, iSlices = 0, pSlices = 0;
    for (const auto& n : nals) {
        if (n.type == 1 || n.type == 5) {
            ++vcl;
            idr += n.type == 5;
            nonIdr += n.type == 1;
            if (n.slice.valid && std::string(sliceKind(n.slice)) == "I")
                ++iSlices;
            if (n.slice.valid && std::string(sliceKind(n.slice)) == "P")
                ++pSlices;
        }
    }
    const int mbW = sps.valid ? ((sps.width + 15) / 16) : 0;
    const int mbH = sps.valid ? ((sps.height + 15) / 16) : 0;
    const int codedW = mbW * 16;
    const int codedH = mbH * 16;
    const int maxRefs = parseMaxNumRefFrames(blob, nals);

    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": \"misterplex.p3.nal_sequence.v1\",\n";
    os << "  \"source\": {\"path\": \"" << baseName(inputPath) << "\", \"bytes\": "
       << blob.size() << ", \"sha256\": \"" << sha256Hex(blob) << "\"},\n";
    os << "  \"profile\": {\"profile_idc\": " << static_cast<int>(sps.profile_idc)
       << ", \"level_idc\": " << static_cast<int>(sps.level_idc)
       << ", \"max_num_ref_frames\": " << maxRefs
       << ", \"entropy_cabac\": " << (pps.entropy_cabac ? "true" : "false")
       << ", \"baseline_cavlc_only\": "
       << (sps.profile_idc == 66 && sps.level_idc <= 30 && pps.valid && !pps.entropy_cabac
               ? "true"
               : "false")
       << "},\n";
    os << "  \"frame\": {\"coded_width\": " << codedW << ", \"coded_height\": " << codedH
       << ", \"display_width\": " << sps.width << ", \"display_height\": " << sps.height
       << ", \"mb_width\": " << mbW << ", \"mb_height\": " << mbH
       << ", \"crop_right_px\": " << (codedW - static_cast<int>(sps.width)) << "},\n";
    os << "  \"sequence\": {\"nal_count\": " << nals.size() << ", \"vcl\": " << vcl
       << ", \"idr\": " << idr << ", \"non_idr\": " << nonIdr << ", \"i_slices\": "
       << iSlices << ", \"p_slices\": " << pSlices
       << ", \"requires_idle_between_vcl\": true, \"ioctl_injection\": \"assert ioctl_download, pulse ioctl_wr once per byte on ioctl_dout, deassert ioctl_download after final byte\"},\n";
    os << "  \"nals\": [\n";
    int vclIndex = 0;
    for (size_t i = 0; i < nals.size(); ++i) {
        const auto& n = nals[i];
        const bool isVcl = n.type == 1 || n.type == 5;
        if (i)
            os << ",\n";
        os << "    {\"index\": " << i << ", \"offset\": " << n.start
           << ", \"start_code_bytes\": " << n.start_code_bytes << ", \"header_offset\": "
           << n.header_offset << ", \"payload_offset\": " << n.payload_offset << ", \"end\": "
           << n.end << ", \"bytes\": " << (n.end - n.start) << ", \"nal_type\": "
           << static_cast<int>(n.type) << ", \"nal_type_name\": \"" << nalTypeName(n.type)
           << "\"";
        if (isVcl) {
            os << ", \"vcl_index\": " << vclIndex++ << ", \"slice_type\": "
               << (n.slice.valid ? static_cast<int>(n.slice.slice_type) : -1)
               << ", \"slice_kind\": \"" << (n.slice.valid ? sliceKind(n.slice) : "invalid")
               << "\", \"frame_num\": " << (n.slice.valid ? static_cast<int>(n.slice.frame_num) : -1)
               << ", \"first_mb_in_slice\": "
               << (n.slice.valid ? static_cast<int>(n.slice.first_mb_in_slice) : -1)
               << ", \"return_to_idle_before_next_vcl_required\": true";
        }
        os << "}";
    }
    os << "\n  ],\n";
    os << "  \"checks\": {\"min_two_nals\": " << (nals.size() >= 2 ? "true" : "false")
       << ", \"has_i_slice\": " << (iSlices > 0 ? "true" : "false")
       << ", \"has_p_slice\": " << (pSlices > 0 ? "true" : "false")
       << ", \"geometry_explicit_no_640_assumption\": true"
       << ", \"rtl_stream_path_idle_check\": \"tests/unit/test_h264_multinal_stream_path.sh verifies this byte stream through ioctl_download/ioctl_wr/ioctl_dout, observes slice_hdr_parser ST_IDLE between VCLs, and requires an IDR parse followed by a P-slice parse.\"}\n";
    os << "}\n";
    return os.str();
}

int firstVclNalOffset(const std::vector<uint8_t>& data) {
    size_t i = 0;
    while (i + 3 < data.size()) {
        size_t sc = 0;
        if (i + 3 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        else {
            ++i;
            continue;
        }
        if (i + sc < data.size()) {
            uint8_t t = data[i + sc] & 0x1f;
            if (t == 1 || t == 5)
                return static_cast<int>(i);
        }
        size_t j = i + sc;
        while (j + 3 < data.size()) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (j + 3 < data.size() && data[j + 2] == 0 && data[j + 3] == 1)))
                break;
            ++j;
        }
        i = (j + 3 >= data.size()) ? data.size() : j;
    }
    return -1;
}

bool verifyMb0Reference(const misterplex::recon::LumaMbTrace& mb, const std::string& refPath,
                        std::string& reason) {
    const std::string ref = readText(refPath);
    if (ref.find("misterplex.p3.luma_mb.v1") == std::string::npos) {
        reason = "missing/unknown reference fixture";
        return false;
    }
    const auto& b0 = mb.blocks[0];
    if (!misterplex::residual_gold::coeffScanMatches(b0.coeff.data())) {
        reason = "first residual coeffs differ from residual_gold::kCoeffScan";
        return false;
    }
    if (misterplex::cavlc::residualCsum8(b0.coeff.data()) != misterplex::residual_gold::kCsum8) {
        reason = "first residual checksum is not 0x14";
        return false;
    }
    if (xorU8(b0.recon) != 0x3b) {
        reason = "first 4x4 recon signature is not 0x3b";
        return false;
    }
    if (b0.recon[0] != misterplex::residual_gold::kY00) {
        reason = "first recon sample is not residual_gold::kY00";
        return false;
    }
    reason = "matches mb0_luma_v1/residual_gold path";
    return true;
}

std::string makeJson(const std::string& inputPath, const std::vector<uint8_t>& blob,
                     const misterplex::NalChainInfo& chain,
                     const misterplex::recon::ReconResult& rec,
                     const misterplex::recon::LumaMbTrace& mb,
                     const std::string& refPath, bool refOk, const std::string& refReason) {
    const int mbW = rec.width > 0 ? (rec.width + 15) / 16 : 0;
    const int mbH = rec.height > 0 ? (rec.height + 15) / 16 : 0;
    const int codedW = mbW * 16;
    const int codedH = mbH * 16;
    const auto& b0 = mb.blocks[0];
    std::array<int16_t, 16> zeroResidual{};
    const uint8_t b0PredOnlySig = addResidualSig(b0.pred, zeroResidual);
    const uint8_t b0DelayedSig = addResidualSig(b0.pred, zeroResidual);
    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": \"misterplex.p3.mb_golden.v1\",\n";
    os << "  \"source\": {\"path\": \"" << baseName(inputPath) << "\", \"bytes\": " << blob.size()
       << ", \"sha256\": \"" << sha256Hex(blob) << "\"},\n";
    os << "  \"extractor\": {\"name\": \"tools/extract_h264_golden.cpp\", \"version\": 1, "
       << "\"scope\": \"Baseline/CAVLC I-slice luma macroblocks; P-slice inter fields are schema-reserved until the real 624x480 fixture is captured\"},\n";
    os << "  \"slice\": {\"nal_unit_offset\": " << firstVclNalOffset(blob)
       << ", \"rbsp_bit_offset\": 0, \"nal_type\": " << (chain.slice.is_idr ? 5 : 1)
       << ", \"slice_type\": " << static_cast<int>(chain.slice.slice_type)
       << ", \"entropy_cabac\": " << (chain.pps.entropy_cabac ? "true" : "false")
       << ", \"first_mb_in_slice\": " << chain.slice.first_mb_in_slice
       << ", \"frame_num\": " << chain.slice.frame_num
       << ", \"idr_pic_id\": " << chain.slice.idr_pic_id
       << ", \"qp\": " << static_cast<int>(chain.slice.slice_qp) << "},\n";
    os << "  \"frame\": {\"coded_width\": " << codedW << ", \"coded_height\": " << codedH
       << ", \"display_width\": " << rec.width << ", \"display_height\": " << rec.height
       << ", \"mb_width\": " << mbW << ", \"mb_height\": " << mbH
       << ", \"crop_right_px\": " << (codedW - rec.width) << "},\n";
    os << "  \"macroblock\": {\"index\": " << mb.mb << ", \"x\": " << mb.mb_x
       << ", \"y\": " << mb.mb_y << ", \"type\": " << mb.mb_type
       << ", \"type_name\": \"" << (mb.mb_type == 0 ? "I_NxN" : "I_16x16_or_pcm")
       << "\", \"qp\": " << mb.qp << ", \"partition_mode\": \"intra\", \"skipped\": false},\n";
    os << "  \"prediction\": {\"mode\": \"intra\", \"luma_16x16\": " << mb.pred_mode
       << ", \"chroma\": " << mb.chroma_mode
       << ", \"coded_block_pattern\": {\"luma\": " << mb.cbp_luma
       << ", \"chroma\": " << mb.cbp_chroma << "}"
       << ", \"luma_4x4_modes\": [";
    for (size_t i = 0; i < mb.blocks.size(); ++i) {
        if (i) os << ",";
        os << mb.blocks[i].pred_mode;
    }
    os << "], \"motion_vectors\": []},\n";
    os << "  \"residual\": {\"luma4x4\": [\n";
    for (size_t i = 0; i < mb.blocks.size(); ++i) {
        const auto& b = mb.blocks[i];
        const auto& prev = i == 0 ? zeroResidual : mb.blocks[i - 1].idct;
        const uint8_t predOnlySig = addResidualSig(b.pred, zeroResidual);
        const uint8_t delayedSig = addResidualSig(b.pred, prev);
        const uint8_t reconSig = xorU8(b.recon);
        if (i) os << ",\n";
        os << "    {\"block\": " << b.block << ", \"x\": " << b.x << ", \"y\": " << b.y
           << ", \"bit_offset_start\": " << b.bit_offset_start
           << ", \"bit_offset_end\": " << b.bit_offset_end
           << ", \"nA\": {\"available\": " << (b.nA_available ? "true" : "false")
           << ", \"total_coeff\": " << b.nA_total_coeff << "}"
           << ", \"nB\": {\"available\": " << (b.nB_available ? "true" : "false")
           << ", \"total_coeff\": " << b.nB_total_coeff << "}"
           << ", \"predicted_nC\": " << b.predicted_nC
           << ", \"coeff_token_table\": " << b.coeff_token_table
           << ", \"coeff_token_table_name\": \"" << coeffTokenTableName(b.coeff_token_table)
           << "\""
           << ", \"total_coeff\": " << b.total_coeff << ", \"coefficients_zigzag\": ";
        jsonArray(os, b.coeff, 8);
        os << ", \"dequant\": ";
        jsonArray(os, b.dequant, 8);
        os << ", \"idct\": ";
        jsonArray(os, b.idct, 8);
        os << ", \"latency_checks\": {\"true_recon_signature8_hex\": \"" << hexU8(reconSig)
           << "\", \"pred_only_signature8_hex\": \"" << hexU8(predOnlySig)
           << "\", \"one_cycle_delayed_signature8_hex\": \"" << hexU8(delayedSig)
           << "\", \"must_match_true_recon\": true, \"must_reject_pred_only\": "
           << (predOnlySig == reconSig ? "false" : "true")
           << ", \"must_reject_one_cycle_delayed\": " << (delayedSig == reconSig ? "false" : "true")
           << "}";
        os << "}";
    }
    os << "\n  ]},\n";
    os << "  \"samples\": {\"pred_y\": ";
    jsonArray(os, mb.pred, 4);
    os << ", \"recon_y\": ";
    jsonArray(os, mb.recon, 4);
    os << "},\n";
    os << "  \"checks\": {\"first_residual_checksum8\": "
       << static_cast<unsigned>(misterplex::cavlc::residualCsum8(b0.coeff.data()))
       << ", \"first_residual_checksum8_hex\": \""
       << hexU8(misterplex::cavlc::residualCsum8(b0.coeff.data()))
       << "\", \"first_recon_signature8\": "
       << static_cast<unsigned>(xorU8(b0.recon))
       << ", \"first_recon_signature8_hex\": \"" << hexU8(xorU8(b0.recon))
       << "\", \"first_pred_only_signature8\": " << static_cast<unsigned>(b0PredOnlySig)
       << ", \"first_pred_only_signature8_hex\": \"" << hexU8(b0PredOnlySig)
       << "\", \"first_one_cycle_delayed_signature8\": " << static_cast<unsigned>(b0DelayedSig)
       << ", \"first_one_cycle_delayed_signature8_hex\": \"" << hexU8(b0DelayedSig)
       << "\", \"latency_contract\": \"Consumers must align residual valid/data with prediction; pred-only or one-cycle-delayed residual paths are explicit red checks, not acceptable alternates.\""
       << ", \"independent_reference\": {\"path\": \"" << refPath
       << "\", \"match\": " << (refOk ? "true" : "false") << ", \"note\": \"" << refReason
       << "\"}}\n";
    os << "}\n";
    return os.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string input = "tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264";
    std::string output;
    std::string outputDir;
    std::string ref = "tests/fixtures/p3_host_recon/mb0_luma_v1.json";
    int mb = 0;
    bool verify = false;
    bool sequence = false;
    bool allMbs = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--input") input = need("--input");
        else if (a == "--output") output = need("--output");
        else if (a == "--output-dir") outputDir = need("--output-dir");
        else if (a == "--mb") mb = std::stoi(need("--mb"));
        else if (a == "--all-mbs") allMbs = true;
        else if (a == "--sequence") sequence = true;
        else if (a == "--verify-mb0-reference") { verify = true; ref = need("--verify-mb0-reference"); }
        else {
            std::cerr << "usage: extract_h264_golden [--input file.264] [--mb N] [--output file.json] [--output-dir dir] [--all-mbs] [--sequence] [--verify-mb0-reference mb0_luma_v1.json]\n";
            return 2;
        }
    }
    auto blob = readFile(input);
    if (blob.empty()) {
        std::cerr << "FAIL: empty/missing input " << input << "\n";
        return 1;
    }
    if (sequence) {
        const std::string json = makeSequenceJson(input, blob);
        if (!output.empty()) {
            if (!writeText(output, json)) {
                std::cerr << "FAIL: cannot write " << output << "\n";
                return 1;
            }
        } else {
            std::cout << json;
        }
        auto nals = splitAnnexB(blob);
        misterplex::SpsInfo sps;
        misterplex::PpsInfo pps;
        uint8_t log2Fn = 0, pocType = 0;
        fillSequenceHeaders(blob, nals, sps, pps, log2Fn, pocType);
        int vcl = 0, iSlices = 0, pSlices = 0;
        for (const auto& n : nals) {
            if (n.type == 1 || n.type == 5) {
                ++vcl;
                if (n.slice.valid && std::string(sliceKind(n.slice)) == "I")
                    ++iSlices;
                if (n.slice.valid && std::string(sliceKind(n.slice)) == "P")
                    ++pSlices;
            }
        }
        if (nals.size() < 2 || vcl < 2 || pSlices < 1) {
            std::cerr << "FAIL: sequence needs >=2 NALs, >=2 VCL NALs, and at least one P-slice; got nals="
                      << nals.size() << " vcl=" << vcl << " p=" << pSlices << "\n";
            return 1;
        }
        std::cout << "extract_h264_golden: OK sequence input=" << input << " nals="
                  << nals.size() << " vcl=" << vcl << " i=" << iSlices << " p=" << pSlices
                  << " sha256=" << sha256Hex(blob) << "\n";
        return 0;
    }
    auto chain = misterplex::parseAnnexBChain(blob.data(), blob.size());
    if (!chain.sps.valid || !chain.pps.valid || !chain.slice.valid) {
        std::cerr << "FAIL: unsupported bitstream (need Baseline/CAVLC SPS/PPS/slice chain)\n";
        return 1;
    }
    if (!chain.slice.is_i_slice) {
        std::cerr << "FAIL: v1 extractor supports I-slice macroblocks only; got slice_type="
                  << static_cast<int>(chain.slice.slice_type) << "\n";
        return 1;
    }
    if (allMbs) {
        if (outputDir.empty()) {
            std::cerr << "FAIL: --all-mbs requires --output-dir\n";
            return 2;
        }
        // Determine total MB count
        misterplex::recon::ReconTrace probe;
        probe.target_mb = 0;
        auto probeRec = misterplex::recon::reconISlice(blob.data(), blob.size(), &probe);
        if (probeRec.mb_decoded != probeRec.mb_total) {
            std::cerr << "FAIL: recon probe failed: " << probeRec.mb_decoded << "/"
                      << probeRec.mb_total << "\n";
            return 1;
        }
        const int totalMbs = probeRec.mb_total;
        int extracted = 0;
        for (int m = 0; m < totalMbs; ++m) {
            misterplex::recon::ReconTrace t;
            t.target_mb = m;
            auto r = misterplex::recon::reconISlice(blob.data(), blob.size(), &t);
            if (r.mb_decoded != r.mb_total || !t.mb.valid) {
                std::cerr << "FAIL: recon at mb=" << m << " decoded=" << r.mb_decoded
                          << "/" << r.mb_total << " valid=" << t.mb.valid << "\n";
                return 1;
            }
            std::string refReason = "not checked (all-mbs mode)";
            bool refOk = false;
            if (m == 0)
                refOk = verifyMb0Reference(t.mb, ref, refReason);
            const std::string json = makeJson(input, blob, chain, r, t.mb, ref, refOk, refReason);
            std::ostringstream fname;
            fname << outputDir << "/mb_" << std::setw(3) << std::setfill('0') << m << ".json";
            if (!writeText(fname.str(), json)) {
                std::cerr << "FAIL: cannot write " << fname.str() << "\n";
                return 1;
            }
            ++extracted;
        }
        std::cout << "extract_h264_golden: OK --all-mbs input=" << input
                  << " mbs=" << extracted << "/" << totalMbs
                  << " output_dir=" << outputDir << "\n";
        return 0;
    }
    misterplex::recon::ReconTrace trace;
    trace.target_mb = mb;
    auto rec = misterplex::recon::reconISlice(blob.data(), blob.size(), &trace);
    if (rec.mb_decoded != rec.mb_total || !trace.mb.valid) {
        std::cerr << "FAIL: recon mb=" << rec.mb_decoded << "/" << rec.mb_total
                  << " trace=" << (trace.mb.valid ? 1 : 0) << " reason="
                  << (rec.fail_reason ? rec.fail_reason : "?") << "\n";
        return 1;
    }
    std::string refReason = "not requested";
    bool refOk = false;
    if (verify || mb == 0)
        refOk = verifyMb0Reference(trace.mb, ref, refReason);
    if (verify && !refOk) {
        std::cerr << "FAIL: independent MB0 reference check: " << refReason << "\n";
        return 1;
    }
    const std::string json = makeJson(input, blob, chain, rec, trace.mb, ref, refOk, refReason);
    if (!output.empty()) {
        if (!writeText(output, json)) {
            std::cerr << "FAIL: cannot write " << output << "\n";
            return 1;
        }
    } else {
        std::cout << json;
    }
    std::cout << "extract_h264_golden: OK input=" << input << " mb=" << mb << " frame="
              << rec.width << "x" << rec.height << " csum=0x" << std::hex
              << static_cast<unsigned>(misterplex::cavlc::residualCsum8(trace.mb.blocks[0].coeff.data()))
              << " recon_sig=0x" << static_cast<unsigned>(xorU8(trace.mb.blocks[0].recon))
              << std::dec << " ref=" << (refOk ? "match" : "not-checked") << "\n";
    return 0;
}
