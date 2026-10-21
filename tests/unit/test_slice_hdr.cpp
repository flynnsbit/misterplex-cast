// Unit: PPS + full I-slice header + first mb_type from real Baseline annex-B (3.3e/h).
// Correct IDR path includes dec_ref_pic_marking (2 flags) before slice_qp_delta.
#include "libmisterplex/h264_nal.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

int main(int argc, char** argv) {


    const char* path = argc > 1 ? argv[1] : "build/plex_real_baseline.264";
    auto blob = readFile(path);
    if (blob.empty()) {
        if (std::system("python3 scripts/gen_test_annexb_real.py build/plex_real_baseline.264") != 0) {
            std::printf("FAIL: no bitstream\n");
            return 1;
        }
        blob = readFile("build/plex_real_baseline.264");


    }
    auto c = misterplex::parseAnnexBChain(blob.data(), blob.size());
    if (!c.sps.valid || c.sps.width != 320 || c.sps.height != 240) {
        std::printf("FAIL SPS %u x %u\n", c.sps.width, c.sps.height);
        return 1;
    }
    if (!c.pps.valid || c.pps.entropy_cabac || !c.pps.deblock_ctrl) {
        std::printf("FAIL PPS valid=%d cabac=%d deblock=%d\n", c.pps.valid, c.pps.entropy_cabac,
                    c.pps.deblock_ctrl);
        return 1;
    }
    if (!c.slice.valid || !c.slice.is_idr || !c.slice.is_i_slice || c.slice.slice_type != 7) {
        std::printf("FAIL slice valid=%d idr=%d i=%d type=%u\n", c.slice.valid, c.slice.is_idr,
                    c.slice.is_i_slice, c.slice.slice_type);
        return 1;
    }
    // Real baseline (ffmpeg/x264): first mb_type=0 (I_NxN); slice_qp = pic_init(23) + delta(2) = 25
    if (!c.slice.has_first_mb_type || c.slice.first_mb_type > 25) {
        std::printf("FAIL mb0 type=%u has=%d\n", c.slice.first_mb_type, c.slice.has_first_mb_type);
        return 1;
    }
    if (c.slice.slice_qp_delta != 2 || c.slice.slice_qp != 25) {
        std::printf("FAIL slice_qp=%d delta=%d want qp=25 delta=2 (init=%d)\n", c.slice.slice_qp,
                    c.slice.slice_qp_delta, c.pps.pic_init_qp);
        return 1;
    }
    std::printf("test_slice_hdr: OK sps=%ux%u pps deblock slice_type=7 mb0=%u qp=%d delta=%d\n",
                c.sps.width, c.sps.height, c.slice.first_mb_type, c.slice.slice_qp,
                c.slice.slice_qp_delta);
    return 0;
}
