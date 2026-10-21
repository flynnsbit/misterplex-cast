// Host unit: misterplex::spsRejectHigh on real NAL bytes (review-274 r0 / c636).
#include "companion.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

int main() {
    const std::vector<uint8_t> baseline{0x67, 66, 0xC0, 30, 0};
    if (misterplex::spsRejectHigh(baseline)) {
        std::fprintf(stderr, "FAIL: Baseline SPS rejected\n");
        return 1;
    }

    const std::vector<uint8_t> high{0x67, 100, 0, 30, 0};
    if (!misterplex::spsRejectHigh(high)) {
        std::fprintf(stderr, "FAIL: High SPS not rejected\n");
        return 1;
    }

    const std::vector<uint8_t> annexb{0, 0, 0, 1, 0x67, 100, 0, 30, 0};
    if (!misterplex::spsRejectHigh(annexb)) {
        std::fprintf(stderr, "FAIL: Annex-B High SPS not rejected\n");
        return 1;
    }

    const std::vector<uint8_t> annexb3{0, 0, 1, 0x67, 100, 0, 30, 0};
    if (!misterplex::spsRejectHigh(annexb3)) {
        std::fprintf(stderr, "FAIL: 3-byte Annex-B High SPS not rejected\n");
        return 1;
    }

    const std::vector<uint8_t> main_sps{0x67, 77, 0, 30, 0};
    if (!misterplex::spsRejectHigh(main_sps)) {
        std::fprintf(stderr, "FAIL: Main SPS not rejected\n");
        return 1;
    }

    // PPS NAL 0x68; RBSP 0xE0 starts pps_id=0, sps_id=0, entropy_coding_mode_flag=1 (CABAC).
    const std::vector<uint8_t> cabac_pps{0x68, 0xE0, 0x00};
    if (!misterplex::spsRejectHigh(cabac_pps)) {
        std::fprintf(stderr, "FAIL: CABAC PPS not rejected\n");
        return 1;
    }

    // Same layout with entropy_coding_mode_flag=0 (CAVLC).
    const std::vector<uint8_t> cavlc_pps{0x68, 0xC0, 0x00};
    if (misterplex::spsRejectHigh(cavlc_pps)) {
        std::fprintf(stderr, "FAIL: CAVLC PPS rejected\n");
        return 1;
    }

    const std::vector<uint8_t> highAu{0, 0, 0, 1, 0x67, 100, 0, 30, 0, 0, 0, 0, 1, 0x68, 0xC0, 0x00};
    if (!misterplex::annexbNeedsBaselineTranscode(highAu.data(), highAu.size())) {
        std::fprintf(stderr, "FAIL: High Annex-B AU did not request the baseline ladder\n");
        return 1;
    }
    const std::vector<uint8_t> baseAu{0, 0, 0, 1, 0x67, 66, 0xC0, 30, 0, 0, 0, 0, 1, 0x68, 0xC0, 0x00};
    if (misterplex::annexbNeedsBaselineTranscode(baseAu.data(), baseAu.size())) {
        std::fprintf(stderr, "FAIL: Baseline Annex-B AU requested a ladder\n");
        return 1;
    }
    const std::vector<uint8_t> mainAu{0, 0, 0, 1, 0x67, 77, 0, 30, 0};
    if (misterplex::annexbNeedsBaselineTranscode(mainAu.data(), mainAu.size())) {
        std::fprintf(stderr, "FAIL: Main SPS is not the High/CABAC probe\n");
        return 1;
    }
    const std::vector<uint8_t> cabacAu{0, 0, 0, 1, 0x67, 66, 0xC0, 30, 0, 0, 0, 0, 1, 0x68, 0xE0, 0x00};
    if (!misterplex::annexbNeedsBaselineTranscode(cabacAu.data(), cabacAu.size())) {
        std::fprintf(stderr, "FAIL: CABAC PPS AU did not request the baseline ladder\n");
        return 1;
    }
    if (!misterplex::highLibavTranscodeEnabled()) {
        std::fprintf(stderr, "FAIL: High libav transcode defaults off\n");
        return 1;
    }
    if (!misterplex::highHostCabacTranscodeEnabled()) {
        std::fprintf(stderr, "FAIL: host CABAC transcode defaults off\n");
        return 1;
    }
    const auto args = misterplex::highCabacFfmpegArgs("/media/fat/mistercast/bin/ffmpeg");
    auto joined = [&]() {
        std::string s;
        for (const auto& a : args) {
            if (!s.empty()) s += " ";
            s += a;
        }
        return s;
    }();
    if (joined.find("-c:v libx264") == std::string::npos ||
        joined.find("-profile:v main") == std::string::npos ||
        joined.find("cabac=1") == std::string::npos ||
        joined.find("8x8dct=0") == std::string::npos ||
        joined.find("-bf 0") == std::string::npos) {
        std::fprintf(stderr, "FAIL: host CABAC ffmpeg args missing Main contract: %s\n",
                     joined.c_str());
        return 1;
    }
    std::vector<uint8_t> grow{
        0, 0, 0, 1, 0x67, 66, 0, 0, 0, 0, 1, 0x65, 0x88, 0x00,
        0, 0, 0, 1, 0x41, 0x88};
    std::vector<std::vector<uint8_t>> aus;
    if (misterplex::extractCompleteAnnexbAus(grow, aus) != 1 || aus.size() != 1 ||
        aus[0].size() != 14) {
        std::fprintf(stderr, "FAIL: Annex-B AU split got %zu aus remain %zu\n",
                     aus.size(), grow.size());
        return 1;
    }
    if (!misterplex::annexbContainsIdr(aus[0].data(), aus[0].size())) {
        std::fprintf(stderr, "FAIL: split AU lost the IDR\n");
        return 1;
    }
    const char* ff = std::getenv("FFMPEG");
    if (!ff || !ff[0])
        ff = "/usr/bin/ffmpeg";
    const char* cavlcPath = "/tmp/mpx-f301-cavlc.264";
    const std::string gen =
        std::string(ff) + " -y -hide_banner -loglevel error -f lavfi -i "
        "color=c=black:s=64x64:r=24:d=1 -c:v libx264 -profile:v baseline "
        "-pix_fmt yuv420p -refs 1 -bf 0 -x264-params cabac=0:bframes=0:ref=1 "
        "-f h264 " + cavlcPath;
    if (std::system(gen.c_str()) != 0) {
        std::fprintf(stderr, "FAIL: could not build a CAVLC sample\n");
        return 1;
    }
    std::ifstream in(cavlcPath, std::ios::binary);
    std::vector<uint8_t> cavlc((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    std::remove(cavlcPath);
    if (cavlc.size() < 16 ||
        misterplex::annexbNeedsBaselineTranscode(cavlc.data(), cavlc.size())) {
        std::fprintf(stderr, "FAIL: sample is not CAVLC (%zu bytes)\n", cavlc.size());
        return 1;
    }
    misterplex::HighCabacHostTranscode host;
    if (!host.start(ff)) {
        std::fprintf(stderr, "FAIL: host transcode did not start %s\n", ff);
        return 1;
    }
    if (!host.push(cavlc.data(), cavlc.size())) {
        std::fprintf(stderr, "FAIL: host transcode push\n");
        return 1;
    }
    host.endInput();
    std::vector<uint8_t> cabac;
    bool got = false;
    for (int i = 0; i < 50; ++i) {
        const auto pop = host.pop(cabac, 100);
        if (pop == misterplex::HighCabacHostTranscode::Pop::Got) {
            got = true;
            break;
        }
        if (pop == misterplex::HighCabacHostTranscode::Pop::Failed)
            break;
    }
    host.stop();
    if (!got || !misterplex::annexbNeedsBaselineTranscode(cabac.data(), cabac.size())) {
        std::fprintf(stderr, "FAIL: host transcode did not emit CABAC (got=%d bytes=%zu)\n",
                     got ? 1 : 0, cabac.size());
        return 1;
    }
    // 107: stdin stays open. The first frame must come out without an EOF.
    misterplex::HighCabacHostTranscode live;
    if (!live.start(ff)) {
        std::fprintf(stderr, "FAIL: live transcode did not start\n");
        return 1;
    }
    for (int n = 0; n < 8; ++n) {
        if (!live.push(cavlc.data(), cavlc.size())) {
            std::fprintf(stderr, "FAIL: live transcode push\n");
            return 1;
        }
    }
    std::vector<uint8_t> liveAu;
    bool liveGot = false;
    for (int i = 0; i < 40; ++i) {
        const auto pop = live.pop(liveAu, 100);
        if (pop == misterplex::HighCabacHostTranscode::Pop::Got) {
            liveGot = true;
            break;
        }
        if (pop == misterplex::HighCabacHostTranscode::Pop::Failed)
            break;
    }
    const size_t liveOut = live.bytesOut();
    const std::string liveErr = live.stderrText();
    live.stop();
    if (!liveGot || !misterplex::annexbNeedsBaselineTranscode(liveAu.data(), liveAu.size())) {
        std::fprintf(stderr, "FAIL: live pipe emitted no CABAC (got=%d out=%zu err=%s)\n",
                     liveGot ? 1 : 0, liveOut, liveErr.c_str());
        return 1;
    }

    std::fprintf(stdout, "test_sps_reject_high: OK\n");
    return 0;
}
