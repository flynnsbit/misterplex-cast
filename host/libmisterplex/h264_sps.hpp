// Host-side H.264 SPS RBSP parser (Baseline-first) for unit tests mirroring FPGA 3.3c.
#pragma once
#include <cstdint>
#include <vector>

namespace misterplex {

struct SpsInfo {
    bool valid = false;
    uint8_t profile_idc = 0;
    uint8_t level_idc = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    // 0008: VUI aspect (H.264 E.1.1). vuiParsed: the bits up to aspect_ratio_idc were read.
    bool vuiParsed = false;
    bool vuiPresent = false;
    bool aspectInfo = false;   // aspect_ratio_info_present_flag
    uint8_t aspectIdc = 0;     // 0 Unspecified, 1 square, ... 255 Extended_SAR
    uint16_t sarW = 0, sarH = 0;
    // the SPS states a sample aspect (anything but Unspecified)
    bool hasAspect() const { return vuiPresent && aspectInfo && aspectIdc != 0 && (aspectIdc != 255 || (sarW && sarH)); }
};

namespace detail {

struct BitReader {
    const uint8_t* d = nullptr;
    size_t n = 0;
    size_t bit = 0;
    bool ok = true;

    explicit BitReader(const uint8_t* p, size_t len) : d(p), n(len) {}

    int u1() {
        if (!ok || (bit / 8) >= n) {
            ok = false;
            return 0;
        }
        const size_t by = bit / 8;
        const int bi = 7 - static_cast<int>(bit % 8);
        const int v = (d[by] >> bi) & 1;
        ++bit;
        return v;
    }
    uint32_t u(int nbits) {
        uint32_t v = 0;
        for (int i = 0; i < nbits; ++i)
            v = (v << 1) | static_cast<uint32_t>(u1());
        return v;
    }
    uint32_t ue() {
        int z = 0;
        while (ok && u1() == 0)
            ++z;
        if (!ok)
            return 0;
        if (z == 0)
            return 0;
        if (z > 24) {
            ok = false;
            return 0;
        }
        return (1u << z) - 1u + u(z);
    }
    int32_t se() {
        uint32_t c = ue();
        if ((c & 1u) == 0)
            return -static_cast<int32_t>(c / 2);
        return static_cast<int32_t>((c + 1) / 2);
    }
};

inline std::vector<uint8_t> removeEpb(const uint8_t* p, size_t n) {
    std::vector<uint8_t> o;
    o.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3) {
            o.push_back(0);
            o.push_back(0);
            i += 2; // skip 00 00; loop +1 skips 03
            continue;
        }
        o.push_back(p[i]);
    }
    return o;
}

} // namespace detail

// Parse SPS RBSP (payload after NAL header; EPB may still be present).
inline SpsInfo parseSpsRbsp(const uint8_t* payload, size_t len) {
    SpsInfo out;
    if (!payload || len < 4)
        return out;
    auto rbsp = detail::removeEpb(payload, len);
    detail::BitReader br(rbsp.data(), rbsp.size());

    out.profile_idc = static_cast<uint8_t>(br.u(8));
    br.u(8); // constraint flags
    out.level_idc = static_cast<uint8_t>(br.u(8));
    br.ue(); // seq_parameter_set_id

    const uint8_t p = out.profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 ||
        p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        uint32_t chroma = br.ue();
        if (chroma == 3)
            br.u(1);
        br.ue();
        br.ue();
        br.u(1);
        if (br.u(1)) {
            // scaling_matrix_present — skip lists (best-effort)
            int n = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < n && br.ok; ++i) {
                if (br.u(1)) {
                    // skip scaling list roughly: up to 16 se deltas
                    int last = 8, next = 8;
                    int size = (i < 6) ? 16 : 64;
                    for (int j = 0; j < size && br.ok; ++j) {
                        if (next != 0) {
                            int32_t delta = br.se();
                            next = (last + delta + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }

    br.ue(); // log2_max_frame_num_minus4
    uint32_t poc = br.ue();
    if (poc == 0) {
        br.ue();
    } else if (poc == 1) {
        br.u(1);
        br.se();
        br.se();
        uint32_t n = br.ue();
        for (uint32_t i = 0; i < n && br.ok; ++i)
            br.se();
    }
    br.ue(); // max_num_ref_frames
    br.u(1); // gaps_in_frame_num_value_allowed_flag
    uint32_t w_mbs = br.ue() + 1;
    uint32_t h_map = br.ue() + 1;
    uint32_t frame_mbs_only = br.u(1);
    if (!frame_mbs_only)
        br.u(1);
    br.u(1); // direct_8x8_inference_flag
    uint32_t crop = br.u(1);
    uint32_t w = w_mbs * 16;
    uint32_t h = h_map * 16 * (frame_mbs_only ? 1u : 2u);
    if (crop) {
        uint32_t l = br.ue(), r = br.ue(), t = br.ue(), b = br.ue();
        // chroma 4:2:0 crop units
        w -= (l + r) * 2;
        h -= (t + b) * 2;
    }
    if (!br.ok || w == 0 || h == 0 || w > 4096 || h > 2160)
        return out;
    out.width = static_cast<uint16_t>(w);
    out.height = static_cast<uint16_t>(h);
    out.valid = true;
    out.vuiPresent = br.u(1) != 0;
    if (out.vuiPresent) {
        out.aspectInfo = br.u(1) != 0;
        if (out.aspectInfo) {
            out.aspectIdc = static_cast<uint8_t>(br.u(8));
            if (out.aspectIdc == 255) {
                out.sarW = static_cast<uint16_t>(br.u(16));
                out.sarH = static_cast<uint16_t>(br.u(16));
            }
        }
    }
    out.vuiParsed = br.ok;
    return out;
}

// Scan annex-B for first SPS NAL and parse it.
inline SpsInfo parseFirstSpsAnnexB(const uint8_t* data, size_t n) {
    SpsInfo none;
    if (!data || n < 5)
        return none;
    size_t i = 0;
    while (i + 3 < n) {
        size_t sc = 0;
        if (i + 3 < n && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        else {
            ++i;
            continue;
        }
        size_t j = i + sc;
        while (j + 3 < n) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (j + 3 < n && data[j + 2] == 0 && data[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 3 >= n)
            j = n;
        if (i + sc < j) {
            uint8_t hdr = data[i + sc];
            if ((hdr & 0x1f) == 7 && (i + sc + 1) < j) {
                return parseSpsRbsp(data + i + sc + 1, j - (i + sc + 1));
            }
        }
        i = j;
    }
    return none;
}

} // namespace misterplex
