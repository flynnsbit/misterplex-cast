// Unit: parse real Baseline SPS from ffmpeg annex-B (mirrors FPGA 3.3c).
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/h264_sps.hpp"
#include "libmisterplex/sps_crop_letterbox.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

// Synthetic SPS-like from gen_test_annexb.py is NOT valid — skip.

// Worked rows from docs/progress/Q006-SPS-CROP-LETTERBOX.md.
// Offsets are SPS frame_crop_*_offset (crop units). Expected bars are G2,
// not the truncating center (480-H)/2. For 640x350 that center is 65; G2 is 64.
struct CropExample {
    const char* name;
    int coded_w;
    int coded_h;
    int off_l, off_r, off_t, off_b;
    int display_w;
    int display_h;
    int crop_l, crop_r, crop_t, crop_b;
    int lb_top;
    int lb_bottom;
};

static int checkCropLetterbox() {
    const CropExample rows[] = {
        {"720x404", 720, 416, 0, 0, 0, 6, 720, 404, 0, 0, 0, 12, 38, 38},
        {"640x360", 640, 368, 0, 0, 0, 4, 640, 360, 0, 0, 0, 8, 60, 60},
        {"704x480", 704, 480, 0, 0, 0, 0, 704, 480, 0, 0, 0, 0, 0, 0},
        {"640x350", 640, 352, 0, 0, 0, 1, 640, 350, 0, 0, 0, 2, 64, 66},
    };
    for (const CropExample& row : rows) {
        const misterplex::SpsCropLetterbox g = misterplex::spsCropLetterbox480(
            row.coded_w, row.coded_h, row.off_l, row.off_r, row.off_t, row.off_b);
        if (!g.valid || g.display_width != row.display_w || g.display_height != row.display_h ||
            g.crop_left != row.crop_l || g.crop_right != row.crop_r || g.crop_top != row.crop_t ||
            g.crop_bottom != row.crop_b || g.lb_top != row.lb_top || g.lb_bottom != row.lb_bottom ||
            g.lb_top + g.display_height + g.lb_bottom != 480) {
            std::printf("FAIL: %s display=%dx%d crop=%d,%d,%d,%d lb=%d,%d valid=%d\n", row.name,
                        g.display_width, g.display_height, g.crop_left, g.crop_right, g.crop_top,
                        g.crop_bottom, g.lb_top, g.lb_bottom, g.valid ? 1 : 0);
            return 1;
        }
        std::printf("test_sps_parse: %s H=%d lb_top=%d lb_bottom=%d\n", row.name, g.display_height,
                    g.lb_top, g.lb_bottom);
    }
    const int oldCenter350 = (480 - 350) / 2;
    const misterplex::SpsCropLetterbox h350 =
        misterplex::spsCropLetterbox480(640, 352, 0, 0, 0, 1);
    if (oldCenter350 != 65 || h350.lb_top == oldCenter350) {
        std::printf("FAIL: 640x350 lb_top=%d still matches truncating center %d\n", h350.lb_top,
                    oldCenter350);
        return 1;
    }

    // 3:10 to Yuma glass 2026-10-10: coded 624x272, luma crop bottom 12, visible
    // 624x260, DAR 47:20. G2 bars are 110/110. A 624x480 visible-468 4:3 crop
    // and a full 624x480 frame must stay unletterboxed (the raw helper would
    // put 6+6 bars on the 468 crop).
    const misterplex::StreamRasterPlacement yuma =
        misterplex::streamRasterPlacement480(624, 272, 0, 0, 0, 12);
    if (!yuma.valid || !yuma.letterbox || yuma.display_width != 624 || yuma.display_height != 260 ||
        yuma.lb_top != 110 || yuma.lb_bottom != 110 ||
        yuma.lb_top + yuma.display_height + yuma.lb_bottom != 480) {
        std::printf("FAIL: 624x272/260 letterbox valid=%d lb=%d,%d H=%d\n", yuma.valid ? 1 : 0,
                    yuma.lb_top, yuma.lb_bottom, yuma.display_height);
        return 1;
    }
    std::printf("test_sps_parse: 624x272 H=%d lb_top=%d lb_bottom=%d\n", yuma.display_height,
                yuma.lb_top, yuma.lb_bottom);

    const misterplex::SpsCropLetterbox helper468 =
        misterplex::spsCropLetterbox480(624, 480, 0, 3, 0, 6);
    const misterplex::StreamRasterPlacement fourThree =
        misterplex::streamRasterPlacement480(624, 480, 0, 6, 0, 12);
    if (!helper468.valid || helper468.display_height != 468 || helper468.lb_top != 6 ||
        helper468.lb_bottom != 6) {
        std::printf("FAIL: helper 624x480/468 lb_top=%d (want the raw G2 6)\n", helper468.lb_top);
        return 1;
    }
    if (!fourThree.valid || fourThree.letterbox || fourThree.display_width != 618 ||
        fourThree.display_height != 468 || fourThree.lb_top != 0 || fourThree.lb_bottom != 0 ||
        fourThree.lb_top == helper468.lb_top) {
        std::printf("FAIL: 624x480/468 placement changed valid=%d letterbox=%d display=%dx%d "
                    "lb=%d,%d\n",
                    fourThree.valid ? 1 : 0, fourThree.letterbox ? 1 : 0, fourThree.display_width,
                    fourThree.display_height, fourThree.lb_top, fourThree.lb_bottom);
        return 1;
    }
    const misterplex::StreamRasterPlacement full480 =
        misterplex::streamRasterPlacement480(624, 480, 0, 0, 0, 0);
    if (!full480.valid || full480.letterbox || full480.display_width != 624 ||
        full480.display_height != 480 || full480.lb_top != 0 || full480.lb_bottom != 0) {
        std::printf("FAIL: 624x480 placement changed valid=%d letterbox=%d H=%d lb=%d\n",
                    full480.valid ? 1 : 0, full480.letterbox ? 1 : 0, full480.display_height,
                    full480.lb_top);
        return 1;
    }
    std::printf("test_sps_parse: 624x480/468 and 624x480 passthrough lb_top=0\n");

    const misterplex::StreamRasterPlacement p240_180 =
        misterplex::streamRasterPlacement240(320, 192, 0, 0, 0, 12);
    if (!p240_180.valid || !p240_180.letterbox || p240_180.display_height != 180 ||
        p240_180.lb_top != 30 || p240_180.lb_bottom != 30) {
        std::printf("FAIL: 320x192/180 240p letterbox valid=%d H=%d lb=%d,%d\n",
                    p240_180.valid ? 1 : 0, p240_180.display_height, p240_180.lb_top,
                    p240_180.lb_bottom);
        return 1;
    }
    const misterplex::StreamRasterPlacement p240_178 =
        misterplex::streamRasterPlacement240(320, 192, 0, 0, 0, 14);
    if (!p240_178.valid || !p240_178.letterbox || p240_178.display_height != 178 ||
        p240_178.lb_top != 30 || p240_178.lb_bottom != 32) {
        std::printf("FAIL: 320x192/178 240p odd-center valid=%d H=%d lb=%d,%d\n",
                    p240_178.valid ? 1 : 0, p240_178.display_height, p240_178.lb_top,
                    p240_178.lb_bottom);
        return 1;
    }
    const misterplex::StreamRasterPlacement p240_full =
        misterplex::streamRasterPlacement240(320, 240, 0, 0, 0, 0);
    if (!p240_full.valid || p240_full.letterbox || p240_full.display_height != 240 ||
        p240_full.lb_top != 0 || p240_full.lb_bottom != 0) {
        std::printf("FAIL: 320x240 240p passthrough valid=%d letterbox=%d H=%d\n",
                    p240_full.valid ? 1 : 0, p240_full.letterbox ? 1 : 0, p240_full.display_height);
        return 1;
    }
    const misterplex::StreamRasterPlacement p240_as_480 =
        misterplex::streamRasterPlacement480(320, 240, 0, 0, 0, 0);
    if (!p240_as_480.valid || !p240_as_480.letterbox || p240_as_480.lb_top != 120 ||
        p240_as_480.lb_bottom != 120) {
        std::printf("FAIL: 320x240 on 480 helper must letterbox 120/120, got lb=%d,%d\n",
                    p240_as_480.lb_top, p240_as_480.lb_bottom);
        return 1;
    }
    std::printf("test_sps_parse: 240p G2 180->30/30 178->30/32 240 passthrough\n");

    const misterplex::FpgaBuildInfo* lab240 = misterplex::recognisedFpgaBuild(0x24C07821u);
    const misterplex::FpgaBuildInfo* lab480i = misterplex::recognisedFpgaBuild(0x48C078E2u);
    const misterplex::FpgaBuildInfo* prod480i = misterplex::recognisedFpgaBuild(0x4801F40Au);
    const misterplex::FpgaBuildInfo* prod480p = misterplex::recognisedFpgaBuild(0x48FF65F2u);
    const bool skipLab240 = misterplex::skipStreamRasterPlacement480(lab240);
    const bool skipLab480i = misterplex::skipStreamRasterPlacement480(lab480i);
    const bool skipProd480i = misterplex::skipStreamRasterPlacement480(prod480i);
    const bool skipProd480p = misterplex::skipStreamRasterPlacement480(prod480p);
    const bool skipNull =
        misterplex::skipStreamRasterPlacement480(static_cast<const misterplex::FpgaBuildInfo*>(nullptr));
    if (!skipLab240 || !skipLab480i || !skipProd480i || skipProd480p || skipNull) {
        std::printf("FAIL: skipStreamRasterPlacement480 24C07821=%d 48C078E2=%d 4801F40A=%d "
                    "48FF65F2=%d null=%d\n",
                    skipLab240 ? 1 : 0, skipLab480i ? 1 : 0, skipProd480i ? 1 : 0,
                    skipProd480p ? 1 : 0, skipNull ? 1 : 0);
        return 1;
    }
    std::printf("test_sps_parse: skipStreamRasterPlacement480 24C07821/48C078E2/4801F40A=1 "
                "48FF65F2=0\n");
    return 0;
}

int main(int argc, char** argv) {
    if (checkCropLetterbox() != 0)
        return 1;



    const char* path = argc > 1 ? argv[1] : "build/plex_real_baseline.264";


    auto blob = readFile(path);
    if (blob.empty()) {
        // Generate via script if missing
        std::string cmd = std::string("python3 ") +
                          (argc > 2 ? argv[2] : "scripts/gen_test_annexb_real.py") + " " + path;
        int r = std::system(cmd.c_str());
        if (r != 0) {
            std::printf("FAIL: no bitstream and gen failed path=%s\n", path);
            return 1;
        }
        blob = readFile(path);
    }
    if (blob.empty()) {
        std::printf("FAIL: empty %s\n", path);
        return 1;
    }
    auto s = misterplex::parseFirstSpsAnnexB(blob.data(), blob.size());
    if (!s.valid) {
        std::printf("FAIL: SPS not valid (%zu bytes)\n", blob.size());
        return 1;
    }
    if (s.width != 320 || s.height != 240) {
        std::printf("FAIL: size %ux%u want 320x240 profile=%u level=%u\n", s.width, s.height,
                    s.profile_idc, s.level_idc);
        return 1;
    }
    if (s.profile_idc != 66) {
        std::printf("FAIL: profile_idc=%u want 66 (Baseline)\n", s.profile_idc);
        return 1;
    }
    std::printf("test_sps_parse: OK %ux%u profile=%u level=%u\n", s.width, s.height, s.profile_idc,
                s.level_idc);
    return 0;
}
