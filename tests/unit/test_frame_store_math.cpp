// Sanity checks for the C3 YUV420 DDR frame-store sizing / ABI.
#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/pixel_format.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int fails = 0;
#define CHECK(c)                                                                                 \
    do {                                                                                         \
        if (!(c)) {                                                                              \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                     \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void checkLayout(const misterplex::DdrFrameGeometry& g, size_t bytes, uint32_t stride,
                        uint32_t doorbell, int lineQwords,
                        misterplex::DdrFrameFormat fmt = misterplex::DdrFrameFormat::Yuv420p,
                        int chromaLineQwords = 0) {
    const auto l = misterplex::makeDdrFrameLayout(g, 0x30000000u, 0x40000u, fmt);
    CHECK(misterplex::ddrFrameLayoutValid(l));
    CHECK(l.frame_bytes == bytes);
    CHECK(l.width == g.coded_width);
    CHECK(l.height == g.coded_height);
    CHECK(l.coded_width == g.coded_width);
    CHECK(l.display_width == g.display_width);
    CHECK(l.presented_width == g.presented_width);
    CHECK(l.crop_right == g.crop_right);
    CHECK(l.present_x == g.present_x);
    CHECK(l.line_bytes == g.coded_width);
    CHECK(l.line_qwords == lineQwords);
    CHECK(l.chroma_line_qwords == chromaLineQwords);
    CHECK(l.y_offset == 0);
    CHECK(l.u_offset == static_cast<uint32_t>(g.coded_width * g.coded_height));
    CHECK(l.v_offset == static_cast<uint32_t>(g.coded_width * g.coded_height +
                                              (g.coded_width / 2) * (g.coded_height / 2)));
    CHECK(l.bank_stride == stride);
    CHECK(l.phys_base + l.bank_stride >= l.phys_base + l.frame_bytes);
    CHECK(l.phys_base + l.bank_stride + l.frame_bytes <= l.doorbell_phys);
    CHECK(l.doorbell_phys == doorbell);
    CHECK(l.map_bytes == l.bank_stride * 2u);
    CHECK(l.doorbell_format == misterplex::ddrFrameFormatCode(fmt));
}

static void checkLayout(int w, int h, size_t bytes, uint32_t stride, uint32_t doorbell,
                        int lineQwords,
                        misterplex::DdrFrameFormat fmt = misterplex::DdrFrameFormat::Yuv420p,
                        int chromaLineQwords = 0) {
    checkLayout(misterplex::makeDdrFrameGeometry(w, h), bytes, stride, doorbell, lineQwords, fmt,
                chromaLineQwords);
}

static void checkConversion(int w, int h) {
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> rgb(pixels * 3);
    for (size_t i = 0; i < pixels; ++i) {
        rgb[i * 3 + 0] = static_cast<uint8_t>(i);
        rgb[i * 3 + 1] = static_cast<uint8_t>(i >> 3);
        rgb[i * 3 + 2] = static_cast<uint8_t>(255 - i);
    }
    std::vector<uint8_t> out(pixels * 2);
    misterplex::pixel::rgb24ToRgb565Le(rgb.data(), out.data(), pixels);
    const size_t probes[] = {0, pixels / 2, pixels - 1};
    for (size_t i : probes) {
        const uint16_t got = misterplex::pixel::loadLe16(out.data() + i * 2);
        const uint16_t want = misterplex::pixel::packRgb565(rgb[i * 3], rgb[i * 3 + 1],
                                                            rgb[i * 3 + 2]);
        CHECK(got == want);
    }
}

int main() {
    constexpr int W = 320, H = 240;
    constexpr int PIXELS = W * H;
    constexpr int BYTES = PIXELS * 3 / 2;
    CHECK(PIXELS == 76800);
    CHECK(BYTES == 115200);
    checkLayout(320, 240, 115200, 0x40000, 0x3007F000, 40,
                misterplex::DdrFrameFormat::Yuv420p, 20);
    checkLayout(640, 480, 460800, 0x80000, 0x300FF000, 80,
                misterplex::DdrFrameFormat::Yuv420p, 40);
    const auto p480 = misterplex::plex480pDdrFrameGeometry();
    CHECK(p480.coded_width == 624);
    CHECK(p480.display_width == 618);
    CHECK(p480.presented_width == 640);
    CHECK(p480.crop_right == 6);
    CHECK(p480.present_x == 11);
    CHECK(p480.placement == misterplex::DdrFramePlacement::Pillarbox);
    const auto p240 = misterplex::plex240pDdrFrameGeometry();
    CHECK(p240.coded_width == 624);
    CHECK(p240.coded_height == 240);
    CHECK(p240.display_width == 618);
    CHECK(p240.display_height == 240);
    CHECK(p240.presented_width == 640);
    CHECK(p240.presented_height == 240);
    CHECK(p240.crop_right == 6);
    CHECK(p240.present_x == 11);
    CHECK(p240.placement == misterplex::DdrFramePlacement::Pillarbox);
    const auto g240 = misterplex::ddrFrameGeometryForPresentedSize(640, 240);
    CHECK(g240.coded_width == 624);
    CHECK(g240.coded_height == 240);
    CHECK(g240.crop_right == 6);
    CHECK(g240.present_x == 11);
    CHECK(misterplex::kPlex240pVPlaneOffset == 187200);
    CHECK(misterplex::kPlex240pYuv420pDoorbellPhys == 0x3007F000u);
    checkLayout(p480, 449280, 0x80000, 0x300FF000, 78,
                misterplex::DdrFrameFormat::Yuv420p, 39);
    const auto presented480p =
        misterplex::ddrFrameGeometryForPresentedSize(640, 480);
    CHECK(presented480p.coded_width == 624);
    CHECK(presented480p.crop_right == 6);
    CHECK(presented480p.present_x == 11);
    const auto i480 = misterplex::plex480iDdrFrameGeometry();
    CHECK(i480.coded_width == 720);
    CHECK(i480.coded_height == 480);
    CHECK(i480.display_width == 720);
    CHECK(i480.display_height == 480);
    CHECK(i480.crop_top == 0);
    CHECK(i480.crop_bottom == 0);
    CHECK(misterplex::ddrFrameGeometryValid(i480));
    const auto presented480i =
        misterplex::ddrFrameGeometryForPresentedSize(720, 480);
    CHECK(presented480i.coded_width == 720);
    CHECK(presented480i.coded_height == 480);
    const auto lb404 = misterplex::plex480iLetterboxGeometry(720, 404);
    CHECK(lb404.coded_width == 720);
    CHECK(lb404.coded_height == 480);
    CHECK(lb404.display_width == 720);
    CHECK(lb404.display_height == 404);
    CHECK(lb404.crop_top == 38);
    CHECK(lb404.crop_bottom == 38);
    CHECK(lb404.crop_left == 0);
    CHECK(lb404.crop_right == 0);
    CHECK(misterplex::ddrFrameGeometryValid(lb404));
    const auto lb404Layout =
        misterplex::makeDdrFrameLayout(lb404, 0x30000000u, 0x40000u,
                                       misterplex::DdrFrameFormat::Yuv420p);
    CHECK(lb404Layout.bank_stride == misterplex::kPlex480iYuv420pBankStride);
    CHECK(lb404Layout.doorbell_phys == misterplex::kPlex480iYuv420pDoorbellPhys);
    CHECK(misterplex::ddrFrameLayoutValid(lb404Layout));
    const auto lb480 = misterplex::plex480iLetterboxGeometry(720, 480);
    CHECK(lb480.crop_top == 0);
    CHECK(lb480.crop_bottom == 0);
    CHECK(lb480.display_height == 480);
    const int oddCenter350 = (480 - 350) / 2;
    const auto lb350 = misterplex::plex480iLetterboxGeometry(640, 350);
    CHECK(oddCenter350 == 65);
    CHECK(lb350.crop_top == 64);
    CHECK(lb350.crop_bottom == 66);
    CHECK(lb350.crop_top != oddCenter350);
    const auto yuv480 =
        misterplex::makeDdrFrameLayout(p480, 0x30000000u, 0x40000u,
                                       misterplex::DdrFrameFormat::Yuv420p);
    CHECK(yuv480.y_offset == misterplex::kPlex480pYPlaneOffset);
    CHECK(yuv480.u_offset == misterplex::kPlex480pUPlaneOffset);
    CHECK(yuv480.v_offset == misterplex::kPlex480pVPlaneOffset);
    CHECK(yuv480.line_bytes == misterplex::kPlex480pYStrideBytes);
    CHECK(yuv480.chroma_line_bytes == misterplex::kPlex480pChromaStrideBytes);
    CHECK(misterplex::kYuv420BlackY == 16);
    CHECK(misterplex::kYuv420BlackU == 128);
    CHECK(misterplex::kYuv420BlackV == 128);
    CHECK(misterplex::ddrFrameFormatCode(misterplex::DdrFrameFormat::Yuv420p) == 1);
    CHECK(misterplex::ddrDoorbellDynWord(0) == 0);
    CHECK(misterplex::ddrDoorbellDynWord(1) == 0); // not 8-byte aligned
    CHECK(misterplex::ddrDoorbellDynWord(0x30180000u) == (0x80000000u | (0x30180000u >> 3)));
    CHECK(misterplex::ddrDoorbellDynWord(0x30300000u) == (0x80000000u | (0x30300000u >> 3)));
    CHECK(misterplex::ddrDoorbellDynValid(misterplex::ddrDoorbellDynWord(0x30180000u)));
    CHECK(misterplex::ddrDoorbellDynPhys(misterplex::ddrDoorbellDynWord(0x30180000u)) ==
          0x30180000u);
    CHECK(!misterplex::ddrDoorbellDynValid(0));
    CHECK(misterplex::ddrDoorbellHi(0x1234, 0, misterplex::DdrFrameFormat::Yuv420p) ==
          0x20001234u);
    CHECK(misterplex::ddrDoorbellHi(0x1234, 1, misterplex::DdrFrameFormat::Yuv420p) ==
          0xA0001234u);
    CHECK(misterplex::ddrDoorbellHi(0x3FFFFFFFu, 1, misterplex::DdrFrameFormat::Yuv420p) ==
          0xBFFFFFFFu);
    uint32_t decodedSeq = 0;
    int decodedBank = -1;
    CHECK(misterplex::decodeDdrDoorbell(misterplex::kDdrFrameDoorbellMagic, 0xA0000005u,
                                        misterplex::DdrFrameFormat::Yuv420p, decodedSeq,
                                        decodedBank));
    CHECK(decodedSeq == 5u);
    CHECK(decodedBank == 1);
    CHECK(misterplex::ddrDoorbellHi((decodedSeq + 1u) & misterplex::kDdrFrameDoorbellSeqMask,
                                    decodedBank, misterplex::DdrFrameFormat::Yuv420p) !=
          0xA0000005u);
    CHECK(!misterplex::decodeDdrDoorbell(0, 0xA0000005u,
                                         misterplex::DdrFrameFormat::Yuv420p, decodedSeq,
                                         decodedBank));

    // Hardware nondeterminism bbox from reload captures. Presentation x includes
    // the 11px pillarbox, so map it back to coded 624-wide YUV420 offsets.
    constexpr int kBboxX0 = 192, kBboxX1 = 360, kBboxY0 = 37, kBboxY1 = 325;
    constexpr int kSrcX0 = kBboxX0 - misterplex::kPlex480pPillarboxLeft;
    constexpr int kSrcX1 = kBboxX1 - misterplex::kPlex480pPillarboxLeft;
    CHECK(kSrcX0 == 181);
    CHECK(kSrcX1 == 349);
    CHECK(kSrcX0 / 8 == 22);
    CHECK(kSrcX1 / 8 == 43);
    CHECK(kSrcX0 / 16 == 11);
    CHECK(kSrcX1 / 16 == 21);
    CHECK(kBboxY0 * misterplex::kPlex480pYStrideBytes + kSrcX0 == 23269);
    CHECK(kBboxY1 * misterplex::kPlex480pYStrideBytes + kSrcX1 == 203149);
    CHECK(misterplex::kPlex480pUPlaneOffset + (kBboxY0 / 2) *
              misterplex::kPlex480pChromaStrideBytes + (kSrcX0 / 2) ==
          305226u);
    CHECK(misterplex::kPlex480pVPlaneOffset + (kBboxY1 / 2) *
              misterplex::kPlex480pChromaStrideBytes + (kSrcX1 / 2) ==
          425118u);
    CHECK((kSrcX0 / 8) / misterplex::kPlex480pYuvLumaLineQwords == 0);
    CHECK((kSrcX1 / 8) / misterplex::kPlex480pYuvLumaLineQwords == 0);
    CHECK((kSrcX0 / 16) / misterplex::kPlex480pYuvChromaLineQwords == 0);
    CHECK((kSrcX1 / 16) / misterplex::kPlex480pYuvChromaLineQwords == 0);
    checkConversion(320, 240);
    checkConversion(640, 480);

    // LE pack as frame_ingest: lo then hi
    uint16_t p = rgb565(255, 0, 0); // red-ish
    uint8_t lo = static_cast<uint8_t>(p & 0xFF);
    uint8_t hi = static_cast<uint8_t>(p >> 8);
    uint16_t recon = static_cast<uint16_t>((hi << 8) | lo);
    CHECK(recon == p);

    // Expand back rough red dominance
    unsigned r8 = (p >> 11) << 3;
    CHECK(r8 >= 240);

    if (fails) {
        std::fprintf(stderr, "test_frame_store_math: %d fails\n", fails);
        return 1;
    }
    std::printf("test_frame_store_math: OK\n");
    return 0;
}
