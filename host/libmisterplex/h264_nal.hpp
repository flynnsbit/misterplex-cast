// Host-side H.264 PPS + slice header + first mb_type (Baseline) — 3.3d/3.3e.
#pragma once
#include "libmisterplex/h264_sps.hpp"

#include <cstdint>

namespace misterplex {

struct PpsInfo {
    bool valid = false;
    uint8_t pps_id = 0;
    uint8_t sps_id = 0;
    bool entropy_cabac = false;
    uint8_t num_ref_idx_l0_default = 0;
    int8_t pic_init_qp = 0; // 26 + pic_init_qp_minus26
    bool deblock_ctrl = false;
};

struct SliceHeader {
    bool valid = false;
    uint32_t first_mb_in_slice = 0;
    uint8_t slice_type = 0; // 2 or 7 = I
    uint8_t pps_id = 0;
    uint32_t frame_num = 0;
    uint32_t idr_pic_id = 0;
    bool is_idr = false;
    bool is_i_slice = false;
    int8_t slice_qp_delta = 0;
    int8_t slice_qp = 0; // clamped 0..51
    uint8_t disable_deblocking_idc = 0;
    uint8_t first_mb_type = 0;
    bool has_first_mb_type = false;
};

inline bool isISliceType(uint8_t t) { return t == 2 || t == 7; }

inline int32_t seFromUe(uint32_t c) {
    if ((c & 1u) == 0)
        return -static_cast<int32_t>(c / 2);
    return static_cast<int32_t>((c + 1) / 2);
}

inline PpsInfo parsePpsRbsp(const uint8_t* payload, size_t len) {
    PpsInfo out;
    if (!payload || len < 2)
        return out;
    auto rbsp = detail::removeEpb(payload, len);
    detail::BitReader br(rbsp.data(), rbsp.size());
    out.pps_id = static_cast<uint8_t>(br.ue());
    out.sps_id = static_cast<uint8_t>(br.ue());
    out.entropy_cabac = br.u(1) != 0;
    br.u(1); // bottom_field_pic_order_in_frame_present
    if (br.ue() > 0)
        return out; // FMO
    out.num_ref_idx_l0_default = static_cast<uint8_t>(br.ue());
    br.ue(); // l1
    br.u(1); // weighted_pred
    br.u(2); // weighted_bipred
    out.pic_init_qp = static_cast<int8_t>(26 + br.se());
    br.se(); // qs
    br.se(); // chroma_qp_index_offset
    out.deblock_ctrl = br.u(1) != 0;
    br.u(1); // constrained_intra
    br.u(1); // redundant_pic_cnt
    if (!br.ok || out.entropy_cabac)
        return out;
    out.valid = true;
    return out;
}

inline SliceHeader parseSliceHeaderRbsp(const uint8_t* payload, size_t len, uint8_t nal_type,
                                        uint8_t nal_ref_idc,
                                        uint8_t log2_max_frame_num, uint8_t /*poc_type*/,
                                        const PpsInfo& pps) {
    SliceHeader out;
    if (!payload || len < 1 || log2_max_frame_num == 0 || log2_max_frame_num > 16 || !pps.valid)
        return out;
    auto rbsp = detail::removeEpb(payload, len);
    detail::BitReader br(rbsp.data(), rbsp.size());
    out.first_mb_in_slice = br.ue();
    out.slice_type = static_cast<uint8_t>(br.ue());
    out.pps_id = static_cast<uint8_t>(br.ue());
    out.frame_num = br.u(log2_max_frame_num);
    out.is_idr = (nal_type == 5);
    if (out.is_idr)
        out.idr_pic_id = br.ue();
    // poc_type 0/1 not handled (Baseline uses poc_type 2 → no POC syntax)

    // num_ref_idx_active_override (P/B slices only, not I/SI)
    bool is_p_or_b = !isISliceType(out.slice_type) && (out.slice_type % 5) != 4;
    if (is_p_or_b) {
        uint32_t override = br.u(1);
        if (override) {
            br.ue(); // num_ref_idx_l0_active_minus1
            // B-slice would also read l1; baseline has no B-slices
        }
    }

    // ref_pic_list_modification (P/B slices only)
    if (is_p_or_b) {
        uint32_t rplm_flag = br.u(1);
        if (rplm_flag) {
            uint32_t idc;
            do {
                idc = br.ue();
                if (idc == 0 || idc == 1)
                    br.ue(); // abs_diff_pic_num_minus1
                else if (idc == 2)
                    br.ue(); // long_term_pic_num
            } while (idc != 3 && br.ok);
        }
    }

    // dec_ref_pic_marking
    if (nal_ref_idc != 0) {
        if (out.is_idr) {
            br.u(1); // no_output_of_prior_pics_flag
            br.u(1); // long_term_reference_flag
        } else {
            uint32_t adaptive = br.u(1);
            if (adaptive) {
                uint32_t mmco;
                do {
                    mmco = br.ue();
                    if (mmco == 1 || mmco == 3)
                        br.ue(); // difference_of_pic_nums_minus1
                    if (mmco == 2)
                        br.ue(); // long_term_pic_num
                    if (mmco == 3 || mmco == 6)
                        br.ue(); // long_term_frame_idx
                    if (mmco == 4)
                        br.ue(); // max_long_term_frame_idx_plus1
                } while (mmco != 0 && br.ok);
            }
        }
    }
    out.slice_qp_delta = static_cast<int8_t>(br.se());
    int qp = static_cast<int>(pps.pic_init_qp) + out.slice_qp_delta;
    if (qp < 0)
        qp = 0;
    if (qp > 51)
        qp = 51;
    out.slice_qp = static_cast<int8_t>(qp);
    if (pps.deblock_ctrl) {
        out.disable_deblocking_idc = static_cast<uint8_t>(br.ue());
        if (out.disable_deblocking_idc != 1) {
            br.se(); // alpha
            br.se(); // beta
        }
    }
    if (!br.ok)
        return out;
    // First macroblock type (I-slice residual path entry)
    out.first_mb_type = static_cast<uint8_t>(br.ue());
    out.has_first_mb_type = br.ok && out.first_mb_type <= 25;
    out.is_i_slice = isISliceType(out.slice_type);
    out.valid = br.ok || out.has_first_mb_type;
    // even if first_mb_type fails, header fields may be ok
    if (out.is_i_slice && out.slice_type != 0)
        out.valid = true;
    return out;
}

struct NalChainInfo {
    SpsInfo sps;
    PpsInfo pps;
    SliceHeader slice;
    uint8_t log2_max_frame_num = 0;
    uint8_t poc_type = 0;
};

inline NalChainInfo parseAnnexBChain(const uint8_t* data, size_t n) {
    NalChainInfo info;
    if (!data || n < 5)
        return info;
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
            uint8_t t = hdr & 0x1f;
            const uint8_t* pay = data + i + sc + 1;
            size_t plen = j - (i + sc + 1);
            if (t == 7) {
                info.sps = parseSpsRbsp(pay, plen);
                auto rb = detail::removeEpb(pay, plen);
                detail::BitReader br(rb.data(), rb.size());
                br.u(8);
                br.u(8);
                br.u(8);
                br.ue();
                info.log2_max_frame_num = static_cast<uint8_t>(br.ue() + 4);
                info.poc_type = static_cast<uint8_t>(br.ue());
            } else if (t == 8) {
                info.pps = parsePpsRbsp(pay, plen);
            } else if ((t == 1 || t == 5) && info.sps.valid && info.pps.valid &&
                       info.log2_max_frame_num) {
                uint8_t nri = (hdr >> 5) & 3;
                info.slice = parseSliceHeaderRbsp(pay, plen, t, nri, info.log2_max_frame_num,
                                                  info.poc_type, info.pps);
                if (info.slice.valid)
                    break;
            }
        }
        i = j;
    }
    return info;
}

} // namespace misterplex
