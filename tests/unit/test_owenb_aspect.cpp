#include "libmisterplex/owenb_aspect.hpp"
#include "h264_sps_builder.hpp"

#include <cerrno>
#include <cstdio>

namespace {

int fails = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

} // namespace

int main() {
    const auto letter = misterplex::owenbAspectRequest(misterplex::OwenbAspectMode::Letterbox, true, 4, 3);
    check(letter.mask == (1u << H264RING_OPT_ANALOG_ASPECT), "0006: Analog Aspect only (no VIDEO_OUTPUT, no ASPECT)");
    check((letter.mask & (1u << H264RING_OPT_VIDEO_OUTPUT)) == 0 && letter.values[H264RING_OPT_VIDEO_OUTPUT] == 0,
          "0006: never a Video Output switch (the TV resyncs, h264ring.h:229)");
    check(letter.values[H264RING_OPT_ANALOG_ASPECT] == 2, "letterbox");

    const auto forced = misterplex::owenbAspectRequest(misterplex::OwenbAspectMode::Letterbox, false, 4, 3);
    check(forced.mask == ((1u << H264RING_OPT_ANALOG_ASPECT) | (1u << H264RING_OPT_ASPECT)) &&
              forced.values[H264RING_OPT_ASPECT] == 1,
          "VUI-less 4:3: ASPECT 4:3 asked");

    const auto wide = misterplex::owenbAspectRequest(misterplex::OwenbAspectMode::Auto, false, 852, 356);
    check((wide.mask & (1u << H264RING_OPT_ASPECT)) == 0, "wide DAR: the user's ASPECT untouched");
    check(wide.values[H264RING_OPT_ANALOG_ASPECT] == 0, "auto analog");

    // 0008: 4:3 within 3%; the stream decision from an in-band SPS + the container's display aspect
    check(misterplex::owenbIs43(4, 3) && misterplex::owenbIs43(720 * 10, 480 * 11) && misterplex::owenbIs43(704 * 10, 480 * 11),
          "4:3, a 720x480 DVD at SAR 10:11 (1.364), 704 active");
    check(!misterplex::owenbIs43(16, 9) && !misterplex::owenbIs43(3, 2) && !misterplex::owenbIs43(5, 4) &&
              !misterplex::owenbIs43(0, 3) && !misterplex::owenbIs43(4, 0),
          "not 4:3: 16:9, 3:2, 5:4, zero");
    using spsb::Vui;
    auto sa = [](const std::vector<uint8_t>& sps, bool hc = false, int64_t n = 0, int64_t d = 0) {
        std::vector<uint8_t> au = {0, 0, 0, 1, 0x09, 0xF0};
        au.insert(au.end(), sps.begin(), sps.end());
        const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
        au.insert(au.end(), idr, idr + sizeof idr);
        return misterplex::owenbStreamAspect(au.data(), au.size(), hc, n, d);
    };
    const auto a1 = sa(spsb::buildSps(640, 480, Vui::None));
    check(a1.spsFound && !a1.spsAspect && a1.darNum == 640 && a1.darDen == 480 && a1.force43,
          "640x480 no VUI: the picture's shape, 4:3 asked");
    check(sa(spsb::buildSps(640, 480, Vui::NoAspect)).force43, "a VUI without aspect_ratio_info: 4:3 asked");
    const auto a3 = sa(spsb::buildSps(640, 480, Vui::Aspect, 1));
    check(a3.spsFound && a3.spsAspect && !a3.force43, "an SPS aspect (square): the core's own");
    check(!sa(spsb::buildSps(640, 480, Vui::Aspect, 0)).spsAspect && sa(spsb::buildSps(640, 480, Vui::Aspect, 0)).force43,
          "aspect_ratio_idc 0 (Unspecified) counts as none");
    check(sa(spsb::buildSps(640, 480, Vui::Aspect, 255, 1, 1)).spsAspect &&
              !sa(spsb::buildSps(640, 480, Vui::Aspect, 255, 0, 0)).spsAspect,
          "Extended_SAR: 1:1 an aspect, 0:0 none");
    check(sa(spsb::buildSps(720, 480, Vui::None), true, 720 * 8, 480 * 9).force43 &&
              !sa(spsb::buildSps(720, 480, Vui::None)).force43,
          "720x480 no VUI: the container's 4:3 asks, the bare 3:2 does not");
    check(!sa(spsb::buildSps(640, 480, Vui::None), true, 16, 9).force43, "the container's 16:9 wins over the shape");
    check(sa(spsb::buildSps(960, 720, Vui::None)).square43 && sa(spsb::buildSps(960, 720, Vui::None)).force43,
          "BUG6: a VUI-less 960x720 picture is a square-sample 4:3 (force43 and square43)");
    check(!sa(spsb::buildSps(720, 480, Vui::None), true, 4, 3).square43 && sa(spsb::buildSps(720, 480, Vui::None), true, 4, 3).force43,
          "BUG6: 720x480 with a 4:3 container: force43 but not square43 (anamorphic)");
    check(!sa(spsb::buildSps(1280, 720, Vui::None)).square43, "BUG6: a 16:9 picture is not square43");
    const uint8_t noSps[] = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x65, 0x88};
    check(!misterplex::owenbStreamAspect(noSps, sizeof noSps, false, 0, 0).spsFound &&
              !misterplex::owenbStreamAspect(noSps, sizeof noSps, false, 0, 0).force43,
          "no SPS: unknown, nothing asked");
    const std::vector<uint8_t> ext = spsb::buildSps(1280, 720, Vui::Aspect, 255, 4, 3);
    const misterplex::SpsInfo si = misterplex::parseFirstSpsAnnexB(ext.data(), ext.size());
    check(si.valid && si.width == 1280 && si.height == 720 && si.aspectIdc == 255 && si.sarW == 4 && si.sarH == 3,
          "h264_sps: the VUI's Extended_SAR read");

    const misterplex::OwenbCrop crop{0, 0, 32, 32};
    const misterplex::OwenbCrop kept = misterplex::owenbKeepCrop(crop);
    check(kept.top == 32 && kept.bottom == 32 && kept.left == 0 && kept.right == 0,
          "720x416 crop kept");

    const int rc = h264ring_request_settings(nullptr, letter.mask, letter.values);
    check(rc == -EINVAL, "null handle");

    if (fails) {
        std::fprintf(stderr, "test_owenb_aspect: %d FAIL\n", fails);
        return 1;
    }
    std::puts("test_owenb_aspect: OK");
    return 0;
}
