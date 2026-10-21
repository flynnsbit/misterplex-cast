#pragma once

#include <cstring>

// Display size from SPS frame_crop_*_offset, plus G2 letterbox into 480 lines.
// streamRasterPlacement480 is the playback decision: a 480-coded bank (visible
// 468 or 480) stays lb_top = lb_bottom = 0. A shorter coded picture uses
// spsCropLetterbox480. The 480p core does not consume lb_top; see
// docs/progress/C044-HDMI-LETTERBOX-235.md.

namespace misterplex {

constexpr int kSpsLetterboxRasterLines = 480;
constexpr int kSpsLetterboxRasterLines240 = 240;

struct SpsCropLetterbox {
    bool valid = false;
    int coded_width = 0;
    int coded_height = 0;
    int display_width = 0;
    int display_height = 0;
    int crop_left = 0;
    int crop_right = 0;
    int crop_top = 0;
    int crop_bottom = 0;
    int lb_top = 0;
    int lb_bottom = 0;
};

// Progressive 4:2:0, frame_mbs_only: CropUnitX = CropUnitY = 2 luma samples.
// crop*Offset are the SPS ue values, not the luma-sample crops the core publishes.
// rasterLines is the store (480 or 240). One helper for every NTSC raster.
inline SpsCropLetterbox spsCropLetterboxN(int rasterLines, int codedWidth, int codedHeight,
                                          int cropLeftOffset, int cropRightOffset,
                                          int cropTopOffset, int cropBottomOffset) {
    SpsCropLetterbox out;
    if (rasterLines <= 0 || codedWidth <= 0 || codedHeight <= 0 || (codedWidth & 15) ||
        (codedHeight & 15))
        return out;
    if (cropLeftOffset < 0 || cropRightOffset < 0 || cropTopOffset < 0 || cropBottomOffset < 0)
        return out;
    if (cropLeftOffset > 4096 || cropRightOffset > 4096 || cropTopOffset > 4096 ||
        cropBottomOffset > 4096)
        return out;

    constexpr int kCropUnit = 2;
    const int cropLeft = cropLeftOffset * kCropUnit;
    const int cropRight = cropRightOffset * kCropUnit;
    const int cropTop = cropTopOffset * kCropUnit;
    const int cropBottom = cropBottomOffset * kCropUnit;
    if (cropLeft + cropRight >= codedWidth || cropTop + cropBottom >= codedHeight)
        return out;

    const int displayWidth = codedWidth - cropLeft - cropRight;
    const int displayHeight = codedHeight - cropTop - cropBottom;
    if (displayWidth <= 0 || displayHeight <= 0 || (displayWidth & 1) || (displayHeight & 1) ||
        displayHeight > rasterLines)
        return out;

    const int lbTop = ((rasterLines - displayHeight) / 2) & ~1;
    const int lbBottom = rasterLines - displayHeight - lbTop;
    if (lbTop < 0 || lbBottom < 0 || (lbTop & 1) || (lbBottom & 1) ||
        lbTop + displayHeight + lbBottom != rasterLines)
        return out;

    out.valid = true;
    out.coded_width = codedWidth;
    out.coded_height = codedHeight;
    out.display_width = displayWidth;
    out.display_height = displayHeight;
    out.crop_left = cropLeft;
    out.crop_right = cropRight;
    out.crop_top = cropTop;
    out.crop_bottom = cropBottom;
    out.lb_top = lbTop;
    out.lb_bottom = lbBottom;
    return out;
}

inline SpsCropLetterbox spsCropLetterbox480(int codedWidth, int codedHeight,
                                            int cropLeftOffset, int cropRightOffset,
                                            int cropTopOffset, int cropBottomOffset) {
    return spsCropLetterboxN(kSpsLetterboxRasterLines, codedWidth, codedHeight, cropLeftOffset,
                             cropRightOffset, cropTopOffset, cropBottomOffset);
}

inline SpsCropLetterbox spsCropLetterbox240(int codedWidth, int codedHeight,
                                            int cropLeftOffset, int cropRightOffset,
                                            int cropTopOffset, int cropBottomOffset) {
    return spsCropLetterboxN(kSpsLetterboxRasterLines240, codedWidth, codedHeight, cropLeftOffset,
                             cropRightOffset, cropTopOffset, cropBottomOffset);
}

// Playback placement of one SPS picture on the 480-line raster.
// Crop arguments are luma samples (the core's crop_px), not SPS crop units.
//
// coded height >= 480, or a display height that already fills the raster,
// is a passthrough: letterbox is false and both bars are 0. That is the
// 624×480 visible-468 4:3 crop and a full 624×480 frame. Calling
// spsCropLetterbox480 on the 468 crop would center it with 6+6 bars; this
// function does not.
//
// A shorter coded picture (624×272, luma crop bottom 12, visible 260) returns
// the G2 bars from spsCropLetterbox480.
struct StreamRasterPlacement {
    bool valid = false;
    bool letterbox = false;
    int display_width = 0;
    int display_height = 0;
    int lb_top = 0;
    int lb_bottom = 0;
};

inline StreamRasterPlacement streamRasterPlacementN(int rasterLines, int codedWidth, int codedHeight,
                                                    int cropLeftLuma, int cropRightLuma,
                                                    int cropTopLuma, int cropBottomLuma) {
    StreamRasterPlacement out;
    if (rasterLines <= 0 || codedWidth <= 0 || codedHeight <= 0)
        return out;
    if (cropLeftLuma < 0 || cropRightLuma < 0 || cropTopLuma < 0 || cropBottomLuma < 0)
        return out;
    if ((cropLeftLuma | cropRightLuma | cropTopLuma | cropBottomLuma) & 1)
        return out;
    if (cropLeftLuma + cropRightLuma >= codedWidth || cropTopLuma + cropBottomLuma >= codedHeight)
        return out;
    const int displayW = codedWidth - cropLeftLuma - cropRightLuma;
    const int displayH = codedHeight - cropTopLuma - cropBottomLuma;
    if (displayW <= 0 || displayH <= 0 || (displayW & 1) || (displayH & 1))
        return out;

    if (codedHeight >= rasterLines || displayH >= rasterLines) {
        out.valid = true;
        out.letterbox = false;
        out.display_width = displayW;
        out.display_height = displayH;
        out.lb_top = 0;
        out.lb_bottom = 0;
        return out;
    }

    const SpsCropLetterbox g = spsCropLetterboxN(
        rasterLines, codedWidth, codedHeight, cropLeftLuma / 2, cropRightLuma / 2, cropTopLuma / 2,
        cropBottomLuma / 2);
    if (!g.valid || g.display_width != displayW || g.display_height != displayH)
        return out;
    out.valid = true;
    out.letterbox = true;
    out.display_width = g.display_width;
    out.display_height = g.display_height;
    out.lb_top = g.lb_top;
    out.lb_bottom = g.lb_bottom;
    return out;
}

inline StreamRasterPlacement streamRasterPlacement480(int codedWidth, int codedHeight,
                                                     int cropLeftLuma, int cropRightLuma,
                                                     int cropTopLuma, int cropBottomLuma) {
    return streamRasterPlacementN(kSpsLetterboxRasterLines, codedWidth, codedHeight, cropLeftLuma,
                                  cropRightLuma, cropTopLuma, cropBottomLuma);
}

inline StreamRasterPlacement streamRasterPlacement240(int codedWidth, int codedHeight,
                                                     int cropLeftLuma, int cropRightLuma,
                                                     int cropTopLuma, int cropBottomLuma) {
    return streamRasterPlacementN(kSpsLetterboxRasterLines240, codedWidth, codedHeight, cropLeftLuma,
                                  cropRightLuma, cropTopLuma, cropBottomLuma);
}

// streamRasterPlacement480 is 480-line-store only.
inline bool skipStreamRasterPlacement480(const char* raster) {
    return raster &&
           (std::strcmp(raster, "240p") == 0 || std::strcmp(raster, "480i") == 0);
}

template <typename BuildRow>
inline bool skipStreamRasterPlacement480(const BuildRow* row) {
    return row && skipStreamRasterPlacement480(row->raster);
}

} // namespace misterplex
