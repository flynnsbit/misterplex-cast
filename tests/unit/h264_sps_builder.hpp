#pragma once
// OWENB-P1B 0008 (tests): a minimal Baseline SPS writer (H.264 7.3.2.1.1 + E.1.1), Annex-B framed, so
// tests can feed the aspect sniffer streams with and without a VUI aspect.
#include <cstdint>
#include <vector>

namespace spsb {

struct BitW {
    std::vector<uint8_t> b;
    int n = 0;
    void u(unsigned v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if (n % 8 == 0)
                b.push_back(0);
            if ((v >> i) & 1u)
                b.back() = uint8_t(b.back() | (0x80u >> (n % 8)));
            ++n;
        }
    }
    void ue(unsigned v) {
        const unsigned x = v + 1;
        int len = 0;
        while ((x >> len) > 1)
            ++len;
        u(0, len);
        u(x, len + 1);
    }
};

enum class Vui { None, NoAspect, Aspect };

// w, h: multiples of 16. idc: aspect_ratio_idc when vui == Aspect (255: sarW:sarH).
inline std::vector<uint8_t> buildSps(unsigned w, unsigned h, Vui vui, unsigned idc = 1, unsigned sarW = 0,
                                     unsigned sarH = 0) {
    BitW s;
    s.u(66, 8);     // profile_idc Baseline
    s.u(0xC0, 8);   // constraint flags
    s.u(30, 8);     // level 3.0
    s.ue(0);        // seq_parameter_set_id
    s.ue(0);        // log2_max_frame_num_minus4
    s.ue(2);        // pic_order_cnt_type
    s.ue(1);        // max_num_ref_frames
    s.u(0, 1);      // gaps_in_frame_num_value_allowed_flag
    s.ue(w / 16 - 1);
    s.ue(h / 16 - 1);
    s.u(1, 1);      // frame_mbs_only_flag
    s.u(1, 1);      // direct_8x8_inference_flag
    s.u(0, 1);      // frame_cropping_flag
    s.u(vui != Vui::None, 1);
    if (vui != Vui::None) {
        s.u(vui == Vui::Aspect, 1);
        if (vui == Vui::Aspect) {
            s.u(idc, 8);
            if (idc == 255) {
                s.u(sarW, 16);
                s.u(sarH, 16);
            }
        }
        s.u(0, 8);   // overscan, video signal, chroma loc, timing, nal/vcl hrd, pic_struct, bitstream restriction
    }
    s.u(1, 1);   // rbsp_stop_one_bit
    while (s.n % 8)
        s.u(0, 1);
    std::vector<uint8_t> out = {0, 0, 0, 1, 0x67};
    int zeros = 0;
    for (uint8_t c : s.b) {   // emulation prevention
        if (zeros >= 2 && c <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return out;
}

} // namespace spsb
