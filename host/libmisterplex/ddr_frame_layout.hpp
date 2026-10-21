#pragma once

#include <cstddef>
#include <cstdint>

namespace misterplex {

constexpr int kPlex480pCodedWidth = 624;
constexpr int kPlex480pCodedHeight = 480;
constexpr int kPlex480pDisplayWidth = 618;
constexpr int kPlex480pDisplayHeight = 480;
constexpr int kPlex480pPresentedWidth = 640;
constexpr int kPlex480pPresentedHeight = 480;
constexpr int kPlex480pCropLeft = 0;
constexpr int kPlex480pCropRight = 6;
constexpr int kPlex480pCropTop = 0;
constexpr int kPlex480pCropBottom = 0;
constexpr int kPlex480pPillarboxLeft = 11;
constexpr int kPlex480pPillarboxRight = 11;
constexpr uint32_t kDdrFramePhysBase = 0x30000000u;
constexpr uint32_t kDdrFrameStrideAlign = 0x40000u;
constexpr int kPlex480pRgb565LineQwords = 156;
constexpr int kPlex480pYuvLumaLineQwords = 78;
constexpr int kPlex480pYuvChromaLineQwords = 39;
constexpr int kPlex480pRgb565Bytes = 599040;
constexpr int kPlex480pYuv420pBytes = 449280;
constexpr int kPlex480pYPlaneOffset = 0;
constexpr int kPlex480pUPlaneOffset = 299520;
constexpr int kPlex480pVPlaneOffset = 374400;
constexpr int kPlex480pYStrideBytes = 624;
constexpr int kPlex480pChromaStrideBytes = 312;
constexpr uint32_t kPlex480pRgb565BankStride = 0x000C0000u;
constexpr uint32_t kPlex480pYuv420pBankStride = 0x00080000u;
constexpr uint32_t kPlex480pRgb565DoorbellPhys = 0x3017F000u;
constexpr uint32_t kPlex480pYuv420pDoorbellPhys = 0x300FF000u;
constexpr uint32_t kDdrFrameDoorbellMagic = 0x504C584Bu; // PLXK
constexpr uint32_t kDdrFrameDoorbellSeqMask = 0x1FFFFFFFu;
constexpr uint8_t kYuv420BlackY = 16;
constexpr uint8_t kYuv420BlackU = 128;
constexpr uint8_t kYuv420BlackV = 128;

static_assert(kPlex480pCodedWidth == 39 * 16, "480p coded width is 39 macroblocks");
static_assert(kPlex480pCodedHeight == 30 * 16, "480p coded height is 30 macroblocks");
static_assert(kPlex480pCodedWidth - kPlex480pCropLeft - kPlex480pCropRight ==
                  kPlex480pDisplayWidth,
              "480p SPS crop must expose 618 pixels");
static_assert(kPlex480pPillarboxLeft + kPlex480pDisplayWidth +
                      kPlex480pPillarboxRight ==
                  kPlex480pPresentedWidth,
              "480p pillars must fill the 640-pixel presentation");
static_assert(kPlex480pYPlaneOffset == 0, "I420 Y starts at bank offset zero");
static_assert(kPlex480pUPlaneOffset ==
                  kPlex480pCodedWidth * kPlex480pCodedHeight,
              "I420 U follows the complete coded Y plane");
static_assert(kPlex480pVPlaneOffset ==
                  kPlex480pUPlaneOffset +
                      (kPlex480pCodedWidth / 2) * (kPlex480pCodedHeight / 2),
              "I420 V follows the complete coded U plane");
static_assert(kPlex480pYuv420pBytes ==
                  kPlex480pCodedWidth * kPlex480pCodedHeight * 3 / 2,
              "I420 bank payload uses coded, not presented, geometry");
static_assert(kPlex480pYuv420pDoorbellPhys ==
                  kDdrFramePhysBase + 2u * kPlex480pYuv420pBankStride - 0x1000u,
              "480p doorbell occupies the final map page");

// ---- 240p letterbox/crop (480p width, 240 active lines) ----
constexpr int kPlex240pCodedWidth = 624;
constexpr int kPlex240pCodedHeight = 240;
constexpr int kPlex240pDisplayWidth = 618;
constexpr int kPlex240pDisplayHeight = 240;
constexpr int kPlex240pPresentedWidth = 640;
constexpr int kPlex240pPresentedHeight = 240;
constexpr int kPlex240pCropLeft = 0;
constexpr int kPlex240pCropRight = 6;
constexpr int kPlex240pCropTop = 0;
constexpr int kPlex240pCropBottom = 0;
constexpr int kPlex240pPillarboxLeft = 11;
constexpr int kPlex240pPillarboxRight = 11;
constexpr int kPlex240pRgb565Bytes = 299520; // 624*240*2
constexpr int kPlex240pYuv420pBytes = 224640; // 624*240*3/2
constexpr int kPlex240pYPlaneOffset = 0;
constexpr int kPlex240pUPlaneOffset = 149760;
constexpr int kPlex240pVPlaneOffset = 187200;
constexpr int kPlex240pYStrideBytes = 624;
constexpr int kPlex240pChromaStrideBytes = 312;
constexpr uint32_t kPlex240pRgb565BankStride = 0x00080000u;
constexpr uint32_t kPlex240pYuv420pBankStride = 0x00040000u;
constexpr uint32_t kPlex240pRgb565DoorbellPhys = 0x300FF000u;
constexpr uint32_t kPlex240pYuv420pDoorbellPhys = 0x3007F000u;
static_assert(kPlex240pCodedWidth == 39 * 16, "240p coded width is 39 macroblocks");
static_assert(kPlex240pCodedHeight == 15 * 16, "240p coded height is 15 macroblocks");
static_assert(kPlex240pCodedWidth - kPlex240pCropLeft - kPlex240pCropRight ==
                  kPlex240pDisplayWidth,
              "240p SPS crop must expose 618 pixels");
static_assert(kPlex240pPillarboxLeft + kPlex240pDisplayWidth +
                      kPlex240pPillarboxRight ==
                  kPlex240pPresentedWidth,
              "240p pillars must fill the 640-pixel presentation");
static_assert(kPlex240pYPlaneOffset == 0, "240p I420 Y starts at bank offset zero");
static_assert(kPlex240pUPlaneOffset ==
                  kPlex240pCodedWidth * kPlex240pCodedHeight,
              "240p I420 U follows the complete coded Y plane");
static_assert(kPlex240pVPlaneOffset ==
                  kPlex240pUPlaneOffset +
                      (kPlex240pCodedWidth / 2) * (kPlex240pCodedHeight / 2),
              "240p I420 V follows the complete coded U plane");
static_assert(kPlex240pYuv420pBytes ==
                  kPlex240pCodedWidth * kPlex240pCodedHeight * 3 / 2,
              "240p I420 bank payload uses coded, not presented, geometry");
static_assert(kPlex240pRgb565Bytes ==
                  kPlex240pCodedWidth * kPlex240pCodedHeight * 2,
              "240p RGB565 payload uses coded geometry");
static_assert(kPlex240pYuv420pDoorbellPhys ==
                  kDdrFramePhysBase + 2u * kPlex240pYuv420pBankStride - 0x1000u,
              "240p doorbell occupies the final map page");
static_assert(kPlex240pRgb565DoorbellPhys ==
                  kDdrFramePhysBase + 2u * kPlex240pRgb565BankStride - 0x1000u,
              "240p RGB565 doorbell occupies the final map page");

// ---- 480i present-only (R15 / PLEX_PRESENT_480I) ----
constexpr int kPlex480iCodedWidth = 720;
constexpr int kPlex480iCodedHeight = 480;
constexpr int kPlex480iDisplayWidth = 720;
constexpr int kPlex480iDisplayHeight = 480;
constexpr int kPlex480iYuv420pBytes = 518400; // 720*480*3/2
constexpr uint32_t kPlex480iYuv420pBankStride = 0x00100000u; // 1 MiB
constexpr uint32_t kPlex480iYuv420pDoorbellPhys = 0x301FF000u;
static_assert(kPlex480iYuv420pBytes == kPlex480iCodedWidth * kPlex480iCodedHeight * 3 / 2,
              "480i I420 payload");
static_assert(kPlex480iYuv420pDoorbellPhys ==
                  kDdrFramePhysBase + 2u * kPlex480iYuv420pBankStride - 0x1000u,
              "480i doorbell final map page");

// ---- 720p tier (present path land; opt-in RBF macros) ----
constexpr int kPlex720pCodedWidth = 1280;
constexpr int kPlex720pCodedHeight = 720;
constexpr int kPlex720pDisplayWidth = 1280;
constexpr int kPlex720pDisplayHeight = 720;
constexpr int kPlex720pPresentedWidth = 1280;
constexpr int kPlex720pPresentedHeight = 720;
constexpr int kPlex720pPillarboxLeft = 0;
constexpr int kPlex720pPillarboxRight = 0;
constexpr int kPlex720pYuv420pBytes = 1382400;
constexpr int kPlex720pYStrideBytes = 1280;
constexpr int kPlex720pChromaStrideBytes = 640;
constexpr uint32_t kPlex720pYuv420pBankStride = 0x00180000u;
constexpr uint32_t kPlex720pPhysBase = 0x30180000u;
// Alias used by PL330/Option-C ingest code (integ naming).
constexpr uint32_t kPlex720pDdrFramePhysBase = kPlex720pPhysBase;
constexpr uint32_t kPlex720pYuv420pDoorbellPhys = 0x3047F000u;
// L4 beam (w-clock): 24 MHz, H=1312, V=762 → 24.006 Hz with DE 1280×720.
constexpr int kPlex720p24BeamHTotal = 1312;
constexpr int kPlex720p24BeamVTotal = 762;
constexpr int kPlex720p24BeamHDe = 1280;
constexpr int kPlex720p24BeamVActive = 720;
// The L4 beam pixel clock (RTL DDR_FRAME_720P24_CLK_PIX_HZ). On L4 it is clk_sys under
// PLEX_CLK_SYS_24 (24 MHz); clk_sys has no host counterpart (T-RTLINV-STALE; was kPlex720p24ClkSysHz).
constexpr int kPlex720p24ClkPixHz = 24000000;
constexpr int kPlex960PresentedWidth = 960;
constexpr int kPlex960PresentedHeight = 540;

// Reserved HPS window (parent device: mem=511M memmap=513M$511M).
constexpr uint32_t kPlexDdrReservedWindowStart = 0x1FF00000u;
constexpr uint32_t kPlexDdrReservedWindowEnd = 0x40000000u;

// /dev/mplex_ddr write-combine chardev (kmod/mplex_ddr). Must match
// MPLEX_DDR_PHYS / MPLEX_DDR_SIZE. 4MiB clipped 720p bank 1
// (0x30300000+1382400 = 0x30451800 > 0x30400000).
constexpr uint32_t kMplexDdrPhys = 0x30000000u;
constexpr uint32_t kMplexDdrSize = 0x800000u; // 8MiB

inline constexpr bool mplexDdrCovers(uint32_t phys_base, uint32_t map_bytes) {
    if (phys_base < kMplexDdrPhys)
        return false;
    const uint64_t need =
        static_cast<uint64_t>(phys_base - kMplexDdrPhys) + static_cast<uint64_t>(map_bytes);
    return need <= static_cast<uint64_t>(kMplexDdrSize);
}

static_assert(kMplexDdrSize == 0x800000u, "WC window 8MiB");
static_assert(mplexDdrCovers(kPlex720pPhysBase, 2u * kPlex720pYuv420pBankStride),
              "8MiB WC must cover 720p dual-bank map (4MiB clipped bank1)");
static_assert(mplexDdrCovers(kDdrFramePhysBase, 2u * kPlex480pYuv420pBankStride),
              "8MiB WC must still cover 480p dual-bank map");

// Option-C triple end = first free byte after 3×720p banks (base+3*stride).
constexpr uint32_t kPlex720pMapBytes3Bank = 0x00480000u; // 3 * 0x180000
constexpr uint32_t kPlex720pOptionCTripleEndPhys =
    kPlex720pDdrFramePhysBase + kPlex720pMapBytes3Bank; // 0x30600000

// PL330 program scratch + contiguous staging (HPS DMA src). Collision fence only
// until product DMA path lands. Mirror intent of DDR_PL330_SCRATCH_PHYS = 0x3060_0000.
constexpr uint32_t kDdrPl330ScratchPhys = 0x30600000u;
constexpr uint32_t kPl330AbiRegionPhys = kPlex720pOptionCTripleEndPhys;
constexpr uint32_t kPl330ProgScratchPhys = kPl330AbiRegionPhys;
constexpr uint32_t kPl330ProgScratchBytes = 0x1000u;
constexpr uint32_t kPl330StagingPhys = kPl330ProgScratchPhys + kPl330ProgScratchBytes;
constexpr uint32_t kPl330StagingBytes = kPlex720pYuv420pBankStride; // 0x180000
constexpr uint32_t kPl330AbiRegionBytes = kPl330ProgScratchBytes + kPl330StagingBytes;
constexpr uint32_t kPl330AbiRegionEndPhys = kPl330AbiRegionPhys + kPl330AbiRegionBytes;

static_assert(kPl330AbiRegionPhys == 0x30600000u, "PL330 ABI base");
static_assert(kPl330AbiRegionPhys == kDdrPl330ScratchPhys, "PL330 ABI == scratch phys");
static_assert(kPl330AbiRegionPhys ==
                  kPlex720pDdrFramePhysBase + 3u * kPlex720pYuv420pBankStride,
              "PL330 sits after Option-C triple banks");
static_assert(kPlex720pYuv420pDoorbellPhys + 0x1000u <= kPl330AbiRegionPhys,
              "doorbell page must not reach PL330");
static_assert(kPl330AbiRegionPhys >= kPlexDdrReservedWindowStart &&
                  kPl330AbiRegionEndPhys <= kPlexDdrReservedWindowEnd,
              "PL330 ABI inside memmap reserved window");
static_assert(kDdrFramePhysBase + 2u * kPlex480pYuv420pBankStride <= kPlex720pDdrFramePhysBase,
              "480p map must not overlap Option-C base");

inline bool pl330PhysInProgScratch(uint32_t phys, uint32_t len = 1) {
    if (len == 0)
        return false;
    if (phys < kPl330ProgScratchPhys)
        return false;
    return phys + len <= kPl330ProgScratchPhys + kPl330ProgScratchBytes;
}

inline bool pl330PhysInStaging(uint32_t phys, uint32_t len = 1) {
    if (len == 0)
        return false;
    if (phys < kPl330StagingPhys)
        return false;
    return phys + len <= kPl330StagingPhys + kPl330StagingBytes;
}

inline bool pl330AbiOverlapsOptionCBanks(uint32_t phys, uint32_t len) {
    const uint32_t a0 = phys;
    const uint32_t a1 = phys + len;
    const uint32_t b0 = kPlex720pDdrFramePhysBase;
    const uint32_t b1 = kPlex720pOptionCTripleEndPhys;
    return a0 < b1 && b0 < a1;
}

enum class DdrFramePlacement {
    None,
    Pillarbox,
};

enum class DdrFramePixelRegion {
    Outside,
    Border,
    Content,
};

struct DdrFramePixelMapping {
    DdrFramePixelRegion region = DdrFramePixelRegion::Outside;
    int coded_x = -1;
    int coded_y = -1;
};

struct DdrFrameSampleOffsets {
    bool valid = false;
    uint32_t y = 0;
    uint32_t u = 0;
    uint32_t v = 0;
};

// HPS DDR frame-store contract shared by misterplexd and RTL:
// - Two banks start at phys_base and phys_base+bank_stride.
// - Doorbell is the final 4 KiB page of the mapped window.
// - Banks contain planar I420: Y at y_offset, U at u_offset,
//   V at v_offset. Luma stride is line_bytes; chroma stride is
//   chroma_line_bytes. The RTL reader schedules line_qwords for luma bursts and
//   chroma_line_qwords for U/V bursts.
// - Geometry separates coded pixels in memory from cropped display pixels and
//   the VGA presentation area. The measured 480p PMS stream is coded 624x480,
//   display-cropped to 618x480 (right crop = 6), then pillarboxed into 640x480
//   with 11 black pixels at each side. The stored payload is the coded frame;
//   the reader applies crop + pillarbox at scanout. Pillarbox pixels are not
//   stored in DDR; the RTL reader emits deterministic video black
//   (Y=16,U=128,V=128) for those columns. The ARM writer also clears cropped
//   padding inside the coded frame to the same black.
// - Doorbell high word is [31]=bank, [30:29]=format, [28:0]=sequence.
//   C3 RTL consumes format 1=YUV420p only; the ARM must never ring this doorbell
//   with an RGB565 payload.
enum class DdrFrameFormat {
    Yuv420p,
};

inline uint32_t ddrFrameFormatCode(DdrFrameFormat) {
    return 1;
}

struct DdrFrameGeometry {
    int coded_width = 0;
    int coded_height = 0;
    int display_width = 0;
    int display_height = 0;
    int presented_width = 0;
    int presented_height = 0;
    int crop_left = 0;
    int crop_right = 0;
    int crop_top = 0;
    int crop_bottom = 0;
    int present_x = 0;
    int present_y = 0;
    DdrFramePlacement placement = DdrFramePlacement::None;
};

struct DdrFrameLayout {
    uint32_t phys_base = 0;
    uint32_t bank_stride = 0;
    uint32_t doorbell_phys = 0;
    uint32_t map_bytes = 0;
    size_t frame_bytes = 0;
    int width = 0;
    int height = 0;
    int line_bytes = 0;
    int line_qwords = 0;
    int chroma_line_bytes = 0;
    int chroma_line_qwords = 0;
    uint32_t y_offset = 0;
    uint32_t u_offset = 0;
    uint32_t v_offset = 0;
    uint32_t doorbell_format = 0;
    int coded_width = 0;
    int coded_height = 0;
    int display_width = 0;
    int display_height = 0;
    int presented_width = 0;
    int presented_height = 0;
    int crop_left = 0;
    int crop_right = 0;
    int crop_top = 0;
    int crop_bottom = 0;
    int present_x = 0;
    int present_y = 0;
    DdrFramePlacement placement = DdrFramePlacement::None;
    DdrFrameFormat format = DdrFrameFormat::Yuv420p;
};

inline uint32_t alignUpU32(uint32_t v, uint32_t align) {
    return align == 0 ? v : static_cast<uint32_t>((v + align - 1u) & ~(align - 1u));
}

inline size_t yuv420pFrameBytes(int width, int height) {
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return 0;
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
}

inline bool ddrFrameGeometryValid(const DdrFrameGeometry& g) {
    if (g.coded_width <= 0 || g.coded_height <= 0 || g.display_width <= 0 ||
        g.display_height <= 0 || g.presented_width <= 0 || g.presented_height <= 0)
        return false;
    if (g.crop_left < 0 || g.crop_right < 0 || g.crop_top < 0 || g.crop_bottom < 0 ||
        g.present_x < 0 || g.present_y < 0)
        return false;
    if (g.display_width + g.crop_left + g.crop_right != g.coded_width ||
        g.display_height + g.crop_top + g.crop_bottom != g.coded_height)
        return false;
    if (g.present_x + g.display_width > g.presented_width ||
        g.present_y + g.display_height > g.presented_height)
        return false;
    if ((g.coded_width & 15) || (g.coded_height & 1))
        return false;
    if ((g.crop_left | g.crop_right | g.crop_top | g.crop_bottom) & 1)
        return false;
    return true;
}

inline DdrFrameGeometry makeDdrFrameGeometry(int codedWidth, int codedHeight,
                                             int displayWidth = 0, int displayHeight = 0,
                                             int presentedWidth = 0, int presentedHeight = 0,
                                             DdrFramePlacement placement =
                                                 DdrFramePlacement::None) {
    DdrFrameGeometry g{};
    g.coded_width = codedWidth;
    g.coded_height = codedHeight;
    g.display_width = displayWidth > 0 ? displayWidth : codedWidth;
    g.display_height = displayHeight > 0 ? displayHeight : codedHeight;
    g.presented_width = presentedWidth > 0 ? presentedWidth : g.display_width;
    g.presented_height = presentedHeight > 0 ? presentedHeight : g.display_height;
    g.crop_left = 0;
    g.crop_top = 0;
    g.crop_right = codedWidth - g.display_width;
    g.crop_bottom = codedHeight - g.display_height;
    g.placement = placement;
    if (placement == DdrFramePlacement::Pillarbox) {
        g.present_x = (g.presented_width - g.display_width) / 2;
        g.present_y = (g.presented_height - g.display_height) / 2;
    }
    return g;
}

inline DdrFrameGeometry plex480pDdrFrameGeometry() {
    DdrFrameGeometry g = makeDdrFrameGeometry(
        kPlex480pCodedWidth, kPlex480pCodedHeight, kPlex480pDisplayWidth,
        kPlex480pDisplayHeight, kPlex480pPresentedWidth, kPlex480pPresentedHeight,
        DdrFramePlacement::Pillarbox);
    g.crop_left = kPlex480pCropLeft;
    g.crop_right = kPlex480pCropRight;
    g.crop_top = kPlex480pCropTop;
    g.crop_bottom = kPlex480pCropBottom;
    g.present_x = kPlex480pPillarboxLeft;
    g.present_y = 0;
    return g;
}

inline DdrFrameGeometry plex480iDdrFrameGeometry() {
    return makeDdrFrameGeometry(
        kPlex480iCodedWidth, kPlex480iCodedHeight, kPlex480iDisplayWidth,
        kPlex480iDisplayHeight, kPlex480iDisplayWidth, kPlex480iDisplayHeight);
}

// Host-paint 480i bank is 720x480. A shorter title (Q006 720x404) is G2-letterboxed
// into that bank: crop_top/bottom are the even bars, coded size stays 720x480 so
// FFmpeg scale+pad in MediaPlayer::threadMain writes the bars into the coded frame.
inline DdrFrameGeometry plex480iLetterboxGeometry(int displayW, int displayH) {
    DdrFrameGeometry g = plex480iDdrFrameGeometry();
    if (displayW <= 0 || displayH <= 0)
        return g;
    displayW &= ~1;
    displayH &= ~1;
    if (displayW > kPlex480iCodedWidth)
        displayW = kPlex480iCodedWidth;
    if (displayH >= kPlex480iCodedHeight)
        return g;
    const int lbTop = ((kPlex480iCodedHeight - displayH) / 2) & ~1;
    const int lbBottom = kPlex480iCodedHeight - displayH - lbTop;
    if (lbTop < 0 || lbBottom < 0)
        return g;
    g.display_width = displayW;
    g.display_height = displayH;
    g.crop_left = 0;
    g.crop_right = kPlex480iCodedWidth - displayW;
    g.crop_top = lbTop;
    g.crop_bottom = lbBottom;
    return g;
}

inline bool isPlex480pDdrFrameGeometry(const DdrFrameGeometry& g) {
    return g.coded_width == kPlex480pCodedWidth &&
           g.coded_height == kPlex480pCodedHeight &&
           g.display_width == kPlex480pDisplayWidth &&
           g.display_height == kPlex480pDisplayHeight &&
           g.presented_width == kPlex480pPresentedWidth &&
           g.presented_height == kPlex480pPresentedHeight &&
           g.crop_left == kPlex480pCropLeft &&
           g.crop_right == kPlex480pCropRight &&
           g.crop_top == kPlex480pCropTop &&
           g.crop_bottom == kPlex480pCropBottom &&
           g.present_x == kPlex480pPillarboxLeft &&
           g.present_y == 0;
}

inline DdrFrameGeometry plex240pDdrFrameGeometry() {
    DdrFrameGeometry g = makeDdrFrameGeometry(
        kPlex240pCodedWidth, kPlex240pCodedHeight, kPlex240pDisplayWidth,
        kPlex240pDisplayHeight, kPlex240pPresentedWidth, kPlex240pPresentedHeight,
        DdrFramePlacement::Pillarbox);
    g.crop_left = kPlex240pCropLeft;
    g.crop_right = kPlex240pCropRight;
    g.crop_top = kPlex240pCropTop;
    g.crop_bottom = kPlex240pCropBottom;
    g.present_x = kPlex240pPillarboxLeft;
    g.present_y = 0;
    return g;
}

inline bool isPlex240pDdrFrameGeometry(const DdrFrameGeometry& g) {
    return g.coded_width == kPlex240pCodedWidth &&
           g.coded_height == kPlex240pCodedHeight &&
           g.display_width == kPlex240pDisplayWidth &&
           g.display_height == kPlex240pDisplayHeight &&
           g.presented_width == kPlex240pPresentedWidth &&
           g.presented_height == kPlex240pPresentedHeight &&
           g.crop_left == kPlex240pCropLeft &&
           g.crop_right == kPlex240pCropRight &&
           g.crop_top == kPlex240pCropTop &&
           g.crop_bottom == kPlex240pCropBottom &&
           g.present_x == kPlex240pPillarboxLeft &&
           g.present_y == 0;
}

inline bool isPlex720pDdrFrameGeometry(const DdrFrameGeometry& g) {
    return g.coded_width == kPlex720pCodedWidth &&
           g.coded_height == kPlex720pCodedHeight &&
           g.display_width == kPlex720pDisplayWidth &&
           g.display_height == kPlex720pDisplayHeight &&
           g.presented_width == kPlex720pPresentedWidth &&
           g.presented_height == kPlex720pPresentedHeight &&
           g.crop_left == 0 && g.crop_right == 0 &&
           g.crop_top == 0 && g.crop_bottom == 0 &&
           g.present_x == 0 && g.present_y == 0;
}

inline uint32_t ddrFramePhysBaseForGeometry(const DdrFrameGeometry& g) {
    return isPlex720pDdrFrameGeometry(g) ? kPlex720pDdrFramePhysBase
                                        : kDdrFramePhysBase;
}

inline DdrFrameGeometry ddrFrameGeometryForPresentedSize(int width, int height) {
    if (width == kPlex480pPresentedWidth && height == kPlex480pPresentedHeight)
        return plex480pDdrFrameGeometry();
    if (width == kPlex480iCodedWidth && height == kPlex480iCodedHeight)
        return plex480iDdrFrameGeometry();
    if (width == kPlex240pPresentedWidth && height == kPlex240pPresentedHeight)
        return plex240pDdrFrameGeometry();
    return makeDdrFrameGeometry(width, height);
}

inline DdrFramePixelMapping mapDdrFramePresentedPixel(const DdrFrameGeometry& g, int x, int y) {
    DdrFramePixelMapping out{};
    if (!ddrFrameGeometryValid(g) || x < 0 || y < 0 || x >= g.presented_width ||
        y >= g.presented_height)
        return out;
    if (x < g.present_x || x >= g.present_x + g.display_width || y < g.present_y ||
        y >= g.present_y + g.display_height) {
        out.region = DdrFramePixelRegion::Border;
        return out;
    }
    out.coded_x = x - g.present_x + g.crop_left;
    out.coded_y = y - g.present_y + g.crop_top;
    if (out.coded_x < 0 || out.coded_x >= g.coded_width || out.coded_y < 0 ||
        out.coded_y >= g.coded_height) {
        out.coded_x = -1;
        out.coded_y = -1;
        return out;
    }
    out.region = DdrFramePixelRegion::Content;
    return out;
}

inline bool ddrFrameGeometryMatchesDelivered(const DdrFrameGeometry& expected, int codedWidth,
                                             int codedHeight, int displayWidth,
                                             int displayHeight, int cropLeftPixels,
                                             int cropRightPixels, int cropTopPixels,
                                             int cropBottomPixels) {
    return ddrFrameGeometryValid(expected) && codedWidth == expected.coded_width &&
           codedHeight == expected.coded_height && displayWidth == expected.display_width &&
           displayHeight == expected.display_height && cropLeftPixels == expected.crop_left &&
           cropRightPixels == expected.crop_right && cropTopPixels == expected.crop_top &&
           cropBottomPixels == expected.crop_bottom;
}

inline DdrFrameLayout makeDdrFrameLayout(const DdrFrameGeometry& geom,
                                         uint32_t physBase = kDdrFramePhysBase,
                                         uint32_t strideAlign = kDdrFrameStrideAlign,
                                         DdrFrameFormat format = DdrFrameFormat::Yuv420p) {
    DdrFrameLayout out{};
    if (!ddrFrameGeometryValid(geom))
        return out;
    if (strideAlign != 0 && (strideAlign & (strideAlign - 1u)) != 0)
        return out;
    const uint64_t lineBytes = static_cast<uint64_t>(geom.coded_width);
    const uint64_t frameBytes = static_cast<uint64_t>(geom.coded_width) *
                                static_cast<uint64_t>(geom.coded_height) * 3u / 2u;
    const uint64_t chromaLineBytes = static_cast<uint64_t>(geom.coded_width / 2);
    const uint64_t minBankStride = frameBytes + 0x1000u;
    // R15 PLEX_PRESENT_480I freezes bank stride at 1 MiB / doorbell 0x301FF000.
    // Default 0x40000 align would pack 720×480 into 0x80000 and miss that ABI.
    // Key off coded+presented 720x480 so a G2 letterbox (display 720x404) keeps
    // the same map as the identity bank; display size is the active area only.
    const bool plex480iPresent =
        geom.coded_width == kPlex480iCodedWidth &&
        geom.coded_height == kPlex480iCodedHeight &&
        geom.presented_width == kPlex480iDisplayWidth &&
        geom.presented_height == kPlex480iDisplayHeight;
    const uint64_t bankStride =
        plex480iPresent
            ? static_cast<uint64_t>(kPlex480iYuv420pBankStride)
            : (strideAlign == 0
                   ? minBankStride
                   : (minBankStride + strideAlign - 1u) &
                         ~static_cast<uint64_t>(strideAlign - 1u));
    const uint64_t mapEnd = static_cast<uint64_t>(physBase) + bankStride * 2u;
    if (lineBytes > 0xFFFFFFFFull || frameBytes > 0xFFFFFFFFull ||
        bankStride > 0xFFFFFFFFull || bankStride * 2u > 0xFFFFFFFFull ||
        mapEnd > 0x100000000ull || bankStride < minBankStride)
        return out;

    out.phys_base = physBase;
    out.format = format;
    out.doorbell_format = ddrFrameFormatCode(format);
    out.width = geom.coded_width;
    out.height = geom.coded_height;
    out.coded_width = geom.coded_width;
    out.coded_height = geom.coded_height;
    out.display_width = geom.display_width;
    out.display_height = geom.display_height;
    out.presented_width = geom.presented_width;
    out.presented_height = geom.presented_height;
    out.crop_left = geom.crop_left;
    out.crop_right = geom.crop_right;
    out.crop_top = geom.crop_top;
    out.crop_bottom = geom.crop_bottom;
    out.present_x = geom.present_x;
    out.present_y = geom.present_y;
    out.placement = geom.placement;
    out.line_bytes = static_cast<int>(lineBytes);
    out.line_qwords = static_cast<int>(lineBytes / 8u);
    out.chroma_line_bytes = static_cast<int>(chromaLineBytes);
    out.chroma_line_qwords = static_cast<int>(chromaLineBytes / 8u);
    out.frame_bytes = static_cast<size_t>(frameBytes);
    const uint32_t yBytes = static_cast<uint32_t>(geom.coded_width * geom.coded_height);
    const uint32_t cBytes = yBytes / 4u;
    out.y_offset = 0;
    out.u_offset = yBytes;
    out.v_offset = yBytes + cBytes;
    out.bank_stride = static_cast<uint32_t>(bankStride);
    out.doorbell_phys = static_cast<uint32_t>(mapEnd - 0x1000u);
    out.map_bytes = static_cast<uint32_t>(bankStride * 2u);
    return out;
}

inline DdrFrameLayout makeDdrFrameLayout(int width, int height,
                                         uint32_t physBase = kDdrFramePhysBase,
                                         uint32_t strideAlign = kDdrFrameStrideAlign,
                                         DdrFrameFormat format = DdrFrameFormat::Yuv420p) {
    return makeDdrFrameLayout(makeDdrFrameGeometry(width, height), physBase, strideAlign, format);
}

inline bool ddrFrameLayoutValid(const DdrFrameLayout& l) {
    if (l.phys_base == 0 || l.width <= 0 || l.height <= 0 || l.frame_bytes == 0)
        return false;
    const DdrFrameGeometry g{l.coded_width, l.coded_height, l.display_width, l.display_height,
                             l.presented_width, l.presented_height, l.crop_left, l.crop_right,
                             l.crop_top, l.crop_bottom, l.present_x, l.present_y, l.placement};
    if (!ddrFrameGeometryValid(g) || l.coded_width != l.width || l.coded_height != l.height)
        return false;
    const uint64_t yBytes = static_cast<uint64_t>(l.coded_width) * l.coded_height;
    const uint64_t cBytes = yBytes / 4u;
    const uint64_t frameBytes = yBytes + 2u * cBytes;
    if (l.line_bytes != l.coded_width || l.chroma_line_bytes != l.coded_width / 2 ||
        l.line_qwords * 8 != l.line_bytes || l.chroma_line_qwords * 8 != l.chroma_line_bytes ||
        l.y_offset != 0 || l.u_offset != yBytes || l.v_offset != yBytes + cBytes ||
        l.frame_bytes != frameBytes || l.doorbell_format != ddrFrameFormatCode(l.format))
        return false;
    if (l.bank_stride < l.frame_bytes ||
        static_cast<uint64_t>(l.map_bytes) != static_cast<uint64_t>(l.bank_stride) * 2u)
        return false;
    const uint64_t mapEnd = static_cast<uint64_t>(l.phys_base) + l.map_bytes;
    if (mapEnd > 0x100000000ull ||
        static_cast<uint64_t>(l.doorbell_phys) + 0x1000u != mapEnd)
        return false;
    const uint64_t bank1 = static_cast<uint64_t>(l.phys_base) + l.bank_stride;
    const uint64_t bank0End = static_cast<uint64_t>(l.phys_base) + l.frame_bytes;
    const uint64_t bank1End = bank1 + l.frame_bytes;
    return bank0End <= bank1 && bank1End <= l.doorbell_phys;
}

inline DdrFrameSampleOffsets ddrFramePresentedSampleOffsets(const DdrFrameLayout& l, int x,
                                                            int y) {
    DdrFrameSampleOffsets out{};
    if (!ddrFrameLayoutValid(l))
        return out;
    const DdrFrameGeometry g{l.coded_width, l.coded_height, l.display_width, l.display_height,
                             l.presented_width, l.presented_height, l.crop_left, l.crop_right,
                             l.crop_top, l.crop_bottom, l.present_x, l.present_y, l.placement};
    const DdrFramePixelMapping map = mapDdrFramePresentedPixel(g, x, y);
    if (map.region != DdrFramePixelRegion::Content)
        return out;
    const uint64_t yOff =
        static_cast<uint64_t>(l.y_offset) + static_cast<uint64_t>(map.coded_y) * l.line_bytes +
        map.coded_x;
    const uint64_t cIndex = static_cast<uint64_t>(map.coded_y / 2) * l.chroma_line_bytes +
                            static_cast<uint64_t>(map.coded_x / 2);
    const uint64_t uOff = static_cast<uint64_t>(l.u_offset) + cIndex;
    const uint64_t vOff = static_cast<uint64_t>(l.v_offset) + cIndex;
    if (yOff >= l.u_offset || uOff >= l.v_offset || vOff >= l.frame_bytes)
        return out;
    out.valid = true;
    out.y = static_cast<uint32_t>(yOff);
    out.u = static_cast<uint32_t>(uOff);
    out.v = static_cast<uint32_t>(vOff);
    return out;
}

inline uint32_t ddrDoorbellHi(uint32_t seq, int bank, DdrFrameFormat format) {
    return (static_cast<uint32_t>(bank & 1) << 31) |
           ((ddrFrameFormatCode(format) & 0x3u) << 29) | (seq & kDdrFrameDoorbellSeqMask);
}

// Second doorbell qword (L4 DYN_BASE_EN=1): FPGA reads burst-2.
// [31]=valid, [28:0]=phys>>3. Live 03f1b95a ignores the extra qword.
inline uint32_t ddrDoorbellDynWord(uint32_t phys) {
    if (phys == 0 || (phys & 7u) != 0)
        return 0;
    return ((phys >> 3) & 0x1FFFFFFFu) | 0x80000000u;
}

inline bool ddrDoorbellDynValid(uint32_t word) { return (word & 0x80000000u) != 0; }

inline uint32_t ddrDoorbellDynPhys(uint32_t word) {
    if (!ddrDoorbellDynValid(word))
        return 0;
    return (word & 0x1FFFFFFFu) << 3;
}

inline bool decodeDdrDoorbell(uint32_t lo, uint32_t hi, DdrFrameFormat expectedFormat,
                              uint32_t& seq, int& bank) {
    if (lo != kDdrFrameDoorbellMagic)
        return false;
    const uint32_t format = (hi >> 29) & 0x3u;
    if (format != ddrFrameFormatCode(expectedFormat))
        return false;
    bank = static_cast<int>((hi >> 31) & 0x1u);
    seq = hi & kDdrFrameDoorbellSeqMask;
    return true;
}

} // namespace misterplex
