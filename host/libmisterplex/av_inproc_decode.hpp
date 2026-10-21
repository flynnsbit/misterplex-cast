#pragma once
#include "fpga_av_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef MPX_HAVE_LIBAV
#include <atomic>
#include <optional>
#include <string>
#include <vector>
#include "ddr_bitstream_ring.hpp"
#include "gate_caps.hpp"
#include "source_aspect.hpp"
#endif

namespace misterplex {

// Default OFF. Pipe path is unchanged unless MPX_INPROC_DECODE is truthy.
// 0/n/f/o/empty → off. Play script may export MPX_INPROC_DECODE=1.
inline bool inprocDecodeWanted() {
    const char* e = std::getenv("MPX_INPROC_DECODE");
    if (!e || !e[0])
        return false;
    const char c = e[0];
    if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F' || c == 'o' ||
        c == 'O')
        return false;
    return true;
}

constexpr int kInproc720W = 1280;
constexpr int kInproc720H = 720;
constexpr int kInproc960W = 960;
constexpr int kInproc960H = 540;
constexpr size_t kInprocI420_1280 =
    static_cast<size_t>(kInproc720W) * static_cast<size_t>(kInproc720H) * 3 / 2;
constexpr size_t kInprocI420_960 =
    static_cast<size_t>(kInproc960W) * static_cast<size_t>(kInproc960H) * 3 / 2;

inline bool inprocDecodeSizeOk(int w, int h) {
    return (w == kInproc720W && h == kInproc720H) ||
           (w == kInproc960W && h == kInproc960H);
}

// Identity 1280×720 inproc must skip in-loop deblock (pipe path already does).
inline bool inprocSkipLoopFilter720p(int w, int h) {
    return w == kInproc720W && h == kInproc720H;
}

inline bool inprocScale1280to960(int srcW, int srcH, int dstW, int dstH) {
    return srcW == kInproc720W && srcH == kInproc720H && dstW == kInproc960W &&
           dstH == kInproc960H;
}

// Point-sample dest(x,y)=src(x*4/3,y*4/3). 1280/960=720/540=4/3. No swscale.
inline void pointSamplePlane4_3(const uint8_t* src, int srcStride, uint8_t* dst,
                                int dstW, int dstH) {
    for (int y = 0; y < dstH; ++y) {
        const uint8_t* srow =
            src + static_cast<size_t>(y * 4 / 3) * static_cast<size_t>(srcStride);
        uint8_t* drow = dst + static_cast<size_t>(y) * static_cast<size_t>(dstW);
        int x = 0;
        for (; x + 2 < dstW; x += 3) {
            const int sx = x * 4 / 3;
            drow[x] = srow[sx];
            drow[x + 1] = srow[sx + 1];
            drow[x + 2] = srow[sx + 2];
        }
        for (; x < dstW; ++x)
            drow[x] = srow[x * 4 / 3];
    }
}

inline bool downsampleI420_1280_to_960_planes(const uint8_t* y, int yStride,
                                              const uint8_t* u, int uStride,
                                              const uint8_t* v, int vStride,
                                              uint8_t* dst, size_t dstBytes) {
    if (!y || !u || !v || !dst || dstBytes != kInprocI420_960)
        return false;
    if (yStride < kInproc720W || uStride < (kInproc720W / 2) ||
        vStride < (kInproc720W / 2))
        return false;
    const size_t ysz =
        static_cast<size_t>(kInproc960W) * static_cast<size_t>(kInproc960H);
    const size_t csz = ysz / 4;
    pointSamplePlane4_3(y, yStride, dst, kInproc960W, kInproc960H);
    pointSamplePlane4_3(u, uStride, dst + ysz, kInproc960W / 2, kInproc960H / 2);
    pointSamplePlane4_3(v, vStride, dst + ysz + csz, kInproc960W / 2,
                        kInproc960H / 2);
    return true;
}

// Packed I420 (Y then U then V, no plane padding). 1382400 → 777600.
inline bool downsampleI420_1280_to_960(const uint8_t* src, size_t srcBytes,
                                       uint8_t* dst, size_t dstBytes) {
    if (!src || srcBytes != kInprocI420_1280)
        return false;
    const size_t ysz =
        static_cast<size_t>(kInproc720W) * static_cast<size_t>(kInproc720H);
    const size_t csz = ysz / 4;
    return downsampleI420_1280_to_960_planes(src, kInproc720W, src + ysz,
                                             kInproc720W / 2, src + ysz + csz,
                                             kInproc720W / 2, dst, dstBytes);
}

// Repeat each src sample rx times. rx==2/4 use word stores (no per-pixel div).
inline void nearestRepeatRow(const uint8_t* s, uint8_t* d, int srcW, int rx) {
    if (rx == 4) {
        for (int sx = 0; sx < srcW; ++sx) {
            const uint32_t v = static_cast<uint32_t>(s[sx]) * 0x01010101u;
            std::memcpy(d + static_cast<size_t>(sx) * 4u, &v, 4);
        }
        return;
    }
    if (rx == 2) {
        int sx = 0;
        for (; sx + 3 < srcW; sx += 4) {
            const uint32_t a = static_cast<uint32_t>(s[sx]) * 0x0101u |
                               ((static_cast<uint32_t>(s[sx + 1]) * 0x0101u) << 16);
            const uint32_t b = static_cast<uint32_t>(s[sx + 2]) * 0x0101u |
                               ((static_cast<uint32_t>(s[sx + 3]) * 0x0101u) << 16);
            std::memcpy(d + static_cast<size_t>(sx) * 2u, &a, 4);
            std::memcpy(d + static_cast<size_t>(sx) * 2u + 4u, &b, 4);
        }
        for (; sx < srcW; ++sx) {
            const uint16_t v = static_cast<uint16_t>(
                static_cast<uint16_t>(s[sx]) * static_cast<uint16_t>(0x0101u));
            std::memcpy(d + static_cast<size_t>(sx) * 2u, &v, 2);
        }
        return;
    }
    for (int sx = 0; sx < srcW; ++sx) {
        const uint8_t p = s[sx];
        uint8_t* dp = d + static_cast<size_t>(sx * rx);
        for (int k = 0; k < rx; ++k)
            dp[k] = p;
    }
}

inline void nearestRepeatChroma(const uint8_t* su, const uint8_t* sv, uint8_t* du,
                                uint8_t* dv, int srcCw, int crx) {
    if (crx == 2 || crx == 4) {
        nearestRepeatRow(su, du, srcCw, crx);
        nearestRepeatRow(sv, dv, srcCw, crx);
        return;
    }
    for (int sx = 0; sx < srcCw; ++sx) {
        const uint8_t pu = su[sx];
        const uint8_t pv = sv[sx];
        uint8_t* dpu = du + static_cast<size_t>(sx * crx);
        uint8_t* dpv = dv + static_cast<size_t>(sx * crx);
        for (int k = 0; k < crx; ++k) {
            dpu[k] = pu;
            dpv[k] = pv;
        }
    }
}

// Nearest I420 into a packed dest (Y then U then V, no padding).
// dest(x,y)=src(x*srcW/dstW, y*srcH/dstH). Even sizes only. No libswscale
// (ARM ffmpeg prefix has none). Identity is a stride-aware row copy.
// Fast: integer box (320×240→1280×720 is 4×3) and 2×/3/2 (640×480→1280×720).
inline bool scaleI420NearestPlanes(const uint8_t* y, int yStride,
                                   const uint8_t* u, int uStride,
                                   const uint8_t* v, int vStride,
                                   int srcW, int srcH,
                                   uint8_t* dst, int dstW, int dstH,
                                   size_t dstBytes) {
    if (!y || !u || !v || !dst)
        return false;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return false;
    if ((srcW & 1) || (srcH & 1) || (dstW & 1) || (dstH & 1))
        return false;
    const size_t need =
        static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 3 / 2;
    if (dstBytes != need)
        return false;
    const int srcCw = srcW / 2;
    const int srcCh = srcH / 2;
    const int dstCw = dstW / 2;
    const int dstCh = dstH / 2;
    if (yStride < srcW || uStride < srcCw || vStride < srcCw)
        return false;
    const size_t ysz = static_cast<size_t>(dstW) * static_cast<size_t>(dstH);
    uint8_t* dy = dst;
    uint8_t* du = dst + ysz;
    uint8_t* dv = du + ysz / 4;
    if (srcW == dstW && srcH == dstH) {
        for (int row = 0; row < dstH; ++row) {
            std::memcpy(dy + static_cast<size_t>(row) * static_cast<size_t>(dstW),
                        y + static_cast<size_t>(row) * static_cast<size_t>(yStride),
                        static_cast<size_t>(dstW));
        }
        for (int row = 0; row < dstCh; ++row) {
            std::memcpy(du + static_cast<size_t>(row) * static_cast<size_t>(dstCw),
                        u + static_cast<size_t>(row) * static_cast<size_t>(uStride),
                        static_cast<size_t>(dstCw));
            std::memcpy(dv + static_cast<size_t>(row) * static_cast<size_t>(dstCw),
                        v + static_cast<size_t>(row) * static_cast<size_t>(vStride),
                        static_cast<size_t>(dstCw));
        }
        return true;
    }
    // Integer box upsample (320×240→1280×720 is 4×3). dest(x,y)=src(x/rx,y/ry).
    if (dstW % srcW == 0 && dstH % srcH == 0) {
        const int rx = dstW / srcW;
        const int ry = dstH / srcH;
        for (int sy = 0; sy < srcH; ++sy) {
            const uint8_t* srow =
                y + static_cast<size_t>(sy) * static_cast<size_t>(yStride);
            uint8_t* drow = dy + static_cast<size_t>(sy * ry) *
                                     static_cast<size_t>(dstW);
            nearestRepeatRow(srow, drow, srcW, rx);
            for (int k = 1; k < ry; ++k)
                std::memcpy(drow + static_cast<size_t>(k) * static_cast<size_t>(dstW),
                            drow, static_cast<size_t>(dstW));
        }
        const int crx = dstCw / srcCw;
        const int cry = dstCh / srcCh;
        for (int sy = 0; sy < srcCh; ++sy) {
            const uint8_t* su =
                u + static_cast<size_t>(sy) * static_cast<size_t>(uStride);
            const uint8_t* sv =
                v + static_cast<size_t>(sy) * static_cast<size_t>(vStride);
            uint8_t* durow = du + static_cast<size_t>(sy * cry) *
                                      static_cast<size_t>(dstCw);
            uint8_t* dvrow = dv + static_cast<size_t>(sy * cry) *
                                      static_cast<size_t>(dstCw);
            nearestRepeatChroma(su, sv, durow, dvrow, srcCw, crx);
            for (int k = 1; k < cry; ++k) {
                std::memcpy(durow + static_cast<size_t>(k) * static_cast<size_t>(dstCw),
                            durow, static_cast<size_t>(dstCw));
                std::memcpy(dvrow + static_cast<size_t>(k) * static_cast<size_t>(dstCw),
                            dvrow, static_cast<size_t>(dstCw));
            }
        }
        return true;
    }
    // 640×480 → 1280×720: 2× width, 3/2 height. y*480/720 = y*2/3.
    // Dest groups of 3 rows sample src (2g, 2g, 2g+1). Same on chroma.
    if (srcW * 2 == dstW && srcH * 3 == dstH * 2) {
        const size_t dYb = static_cast<size_t>(dstW);
        const size_t dCb = static_cast<size_t>(dstCw);
        for (int g = 0; g < dstH / 3; ++g) {
            const int sy0 = g * 2;
            uint8_t* d0 = dy + static_cast<size_t>(g * 3) * dYb;
            nearestRepeatRow(y + static_cast<size_t>(sy0) * static_cast<size_t>(yStride),
                             d0, srcW, 2);
            std::memcpy(d0 + dYb, d0, dYb);
            nearestRepeatRow(y + static_cast<size_t>(sy0 + 1) * static_cast<size_t>(yStride),
                             d0 + 2 * dYb, srcW, 2);
        }
        for (int g = 0; g < dstCh / 3; ++g) {
            const int sy0 = g * 2;
            uint8_t* u0 = du + static_cast<size_t>(g * 3) * dCb;
            uint8_t* v0 = dv + static_cast<size_t>(g * 3) * dCb;
            nearestRepeatChroma(u + static_cast<size_t>(sy0) * static_cast<size_t>(uStride),
                                v + static_cast<size_t>(sy0) * static_cast<size_t>(vStride),
                                u0, v0, srcCw, 2);
            std::memcpy(u0 + dCb, u0, dCb);
            std::memcpy(v0 + dCb, v0, dCb);
            nearestRepeatChroma(
                u + static_cast<size_t>(sy0 + 1) * static_cast<size_t>(uStride),
                v + static_cast<size_t>(sy0 + 1) * static_cast<size_t>(vStride),
                u0 + 2 * dCb, v0 + 2 * dCb, srcCw, 2);
        }
        return true;
    }
    // Generic: one div per column (xmap), not per pixel.
    if (dstW <= 2048 && dstCw <= 1024) {
        int xmap[2048];
        int cxmap[1024];
        for (int x = 0; x < dstW; ++x)
            xmap[x] = x * srcW / dstW;
        for (int x = 0; x < dstCw; ++x)
            cxmap[x] = x * srcCw / dstCw;
        for (int row = 0; row < dstH; ++row) {
            const int sy = row * srcH / dstH;
            const uint8_t* srow =
                y + static_cast<size_t>(sy) * static_cast<size_t>(yStride);
            uint8_t* drow = dy + static_cast<size_t>(row) * static_cast<size_t>(dstW);
            for (int x = 0; x < dstW; ++x)
                drow[x] = srow[xmap[x]];
        }
        for (int row = 0; row < dstCh; ++row) {
            const int sy = row * srcCh / dstCh;
            const uint8_t* su =
                u + static_cast<size_t>(sy) * static_cast<size_t>(uStride);
            const uint8_t* sv =
                v + static_cast<size_t>(sy) * static_cast<size_t>(vStride);
            uint8_t* durow = du + static_cast<size_t>(row) * static_cast<size_t>(dstCw);
            uint8_t* dvrow = dv + static_cast<size_t>(row) * static_cast<size_t>(dstCw);
            for (int x = 0; x < dstCw; ++x) {
                const int sx = cxmap[x];
                durow[x] = su[sx];
                dvrow[x] = sv[sx];
            }
        }
        return true;
    }
    for (int row = 0; row < dstH; ++row) {
        const int sy = row * srcH / dstH;
        const uint8_t* srow =
            y + static_cast<size_t>(sy) * static_cast<size_t>(yStride);
        uint8_t* drow = dy + static_cast<size_t>(row) * static_cast<size_t>(dstW);
        for (int x = 0; x < dstW; ++x)
            drow[x] = srow[x * srcW / dstW];
    }
    for (int row = 0; row < dstCh; ++row) {
        const int sy = row * srcCh / dstCh;
        const uint8_t* su =
            u + static_cast<size_t>(sy) * static_cast<size_t>(uStride);
        const uint8_t* sv =
            v + static_cast<size_t>(sy) * static_cast<size_t>(vStride);
        uint8_t* durow = du + static_cast<size_t>(row) * static_cast<size_t>(dstCw);
        uint8_t* dvrow = dv + static_cast<size_t>(row) * static_cast<size_t>(dstCw);
        for (int x = 0; x < dstCw; ++x) {
            const int sx = x * srcCw / dstCw;
            durow[x] = su[sx];
            dvrow[x] = sv[sx];
        }
    }
    return true;
}

// N2 (dev-480i 10/4; review F1-F4, review-rel-batch-1428-1004.md section 4): the
// lab ladder proxy answers with Connection: close and no Content-Length, and FFmpeg's http
// protocol ends such a response with AVERROR(EIO), "Stream ends prematurely ... should be
// 18446744073709551615" (n8.0 http.c:1680-1691), not AVERROR_EOF. Every natural end of a
// proxied play then failed the session. The demux treats that EIO as the end of the stream only
// when all of this holds; anything else keeps the EIO, an error:
//  F1  the input has no known length (avio_size < 0); a Content-Length server closing short
//      keeps its EIO;
//  F3  the session has a declared duration (0 = unknown, never converted), the read was not
//      cancelled, and neither reader has failed;
//  F2  the input reached the declared end: its session position, the play's start offset plus
//      the longest stream span read (last pts + duration - first pts, per stream), is within
//      kInputCloseEndWindowMs of the PMS duration, or past it.
// Limit (review N-g): the span is max(end) - min(first) per stream, so a forward pts jump
// mid-stream would inflate it, and a server cut after such a jump could convert early. The
// ladder's ffmpeg re-encode is continuous. If it shows up on glass, cap the span at the summed
// packet durations or restart it at a discontinuity.
constexpr int64_t kInputCloseEndWindowMs = 2000;
constexpr int64_t kNoInputCloseAtEnd = INT64_MIN;  // no EIO was taken as the end

inline bool inputCloseIsEndOfStream(bool lengthUnknown, bool cancelled, bool readerFailed,
                                    int64_t expectedDurationMs, int64_t sessionStartMs,
                                    int64_t spanMs, int64_t windowMs = kInputCloseEndWindowMs) {
    if (!lengthUnknown || cancelled || readerFailed)
        return false;
    if (expectedDurationMs <= 0 || spanMs < 0 || sessionStartMs < 0 || windowMs < 0)
        return false;
    return sessionStartMs + spanMs >= expectedDurationMs - windowMs;
}

// In-session read stall bound: one blocked input read longer than this ends the session with
// AVERROR_EXIT ("Immediate exit requested"). LAN (lab PMS, ladder proxy) keeps the historic 5 s.
// A remote/shared PMS universal transcode (location=wan) can pause several seconds right after its
// first burst (a lab MiSTer 1011-cp-cutoff-playbar run-A: 155 KB / 3.6 s then >5 s silence, 4 of 4
// sessions; same signature on 198.51.100.153 F2/B1/J2 with mediaBufferSize set), so a WAN
// read waits up to the open's rw_timeout (20 s). MPX_REMOTE_READ_STALL_MS (2000..60000) overrides
// the WAN bound.
constexpr int kLanReadStallMs = 5000;
constexpr int kWanReadStallMs = 20000;
inline int compressedReadStallMs(const std::string& url) {
#ifdef MUT_WAN_READ_STALL_5S   // test-only mutant: the pre-fix bound everywhere
    (void)url;
    return kLanReadStallMs;
#else
    if (url.find("location=wan") == std::string::npos)
        return kLanReadStallMs;
    if (const char* e = std::getenv("MPX_REMOTE_READ_STALL_MS")) {
        char* end = nullptr;
        const long v = std::strtol(e, &end, 10);
        if (end && *end == '\0' && v >= 2000 && v <= 60000)
            return static_cast<int>(v);
    }
    return kWanReadStallMs;
#endif
}

#ifdef MPX_HAVE_LIBAV

struct AvInprocOpenOpts {
    // Software bank expectation; in compressed mode these are MAX coded bounds.
    int expectW = 1280;
    int expectH = 720;
    int threads = 2;
    int64_t startMs = 0;
    // 0 → same as expect (bank). Source may differ; readI420 nearest-scales.
    int outW = 0;
    int outH = 0;
    // FFmpeg-style header block (CRLF). PMS universal needs X-Plex-Token etc.
    std::string headers;
    // Live annex-B fifo: larger probe, no nobuffer (32k+nobuffer sent a
    // truncated NAL → avcodec_send_packet Invalid data).
    bool liveFifo = false;
    // Demux/parse compressed H.264 only. Never opens a video decoder.
    bool compressedVideo = false;
    bool decodeAudio = true;
    bool allowInter = false;
    bool allowDeblock = false;
    bool requireAllIdr = false;
    bool requireLimitedBt601 = false;
    // The stream gate's limits from the loaded core's CAPS (gate::evaluateCaps). The default is
    // the v1 rule (refs 1, L3.0, no grants), so an unset field keeps today's limits. inspect reads
    // the refs family (maxRefs) from it.
    gate::GateCaps gateCaps;
    // The negotiated build row's spsRelaxNvenc (fpga_build_ids.hpp; true on 0x48C5385C, 0x48C51FF1, 0x48CFEE31): G9 takes a
    // declared max_num_ref_frames up to 2, the VUI may carry NAL/VCL HRD and pic_struct_present_flag 1, and
    // bitstream_restriction max_dec_frame_buffering may be 2. Active refs (G18/G26) still key on gateCaps.
    bool spsRelaxNvenc = false;
    // High core (0x48A64AED, MPX_HIGH_NO_CAVLC): Main profile_idc 77 and a CABAC
    // PPS are legal. 8x8 transform and scaling lists stay refused. Baseline cores
    // leave this false and still require profile 66 / CAVLC.
    bool allowHighCabac = false;
    // Baseline core + host ffmpeg: a High/Main AU is accepted so the player can
    // rewrite it to Baseline CAVLC. The baseline SPS gate still runs when this
    // is false. 179 L6 stayed on profile_idc 100 after the cabac=0 probe.
    bool acceptHighForHostRewrite = false;
    size_t maxAccessUnitBytes = ddr_bitstream_ring::kMaxAccessUnitBytes;
    size_t maxVclRbspBytes = ddr_bitstream_ring::kMaxAccessUnitBytes;
    int expectedFpsNum = 0;
    int expectedFpsDen = 0;
    // N2 (inputCloseIsEndOfStream): the item's PMS duration in ms, 0 = unknown (an EIO at
    // the close is never taken as the end), and the play's start offset in the item, whether
    // the URL (offset_ms=, PMS offset=) or this demux's seek (startMs) applies it.
    int64_t expectedDurationMs = 0;
    int64_t sessionStartMs = 0;
    const std::atomic<bool>* cancelled = nullptr;
    const std::atomic<bool>* paused = nullptr;
    // compressedReadStallMs(url): the bound on one blocked in-session input read.
    int readStallMs = kLanReadStallMs;
};

struct AvCompressedVideoGeometry {
    int codedWidth = 0, codedHeight = 0;
    int visibleWidth = 0, visibleHeight = 0;
    int macroblockColumns = 0, macroblockRows = 0;
    int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    SourceAspect sourceAspect{};
    bool videoSignalPresent = false;
    bool fullRange = false;
    bool colorDescriptionPresent = false;
    unsigned colorPrimaries = 2;
    unsigned transferCharacteristics = 2;
    unsigned matrixCoefficients = 2;

    bool limitedBt601Signaled() const {
        return !fullRange && colorDescriptionPresent &&
               (matrixCoefficients == 5 || matrixCoefficients == 6);
    }

    bool samePictureLayout(const AvCompressedVideoGeometry& other) const {
        return codedWidth == other.codedWidth && codedHeight == other.codedHeight &&
               cropLeft == other.cropLeft && cropRight == other.cropRight &&
               cropTop == other.cropTop && cropBottom == other.cropBottom;
    }
};

struct AvCompressedAccessUnit {
    std::vector<uint8_t> annexb;
    int64_t pts = ddr_bitstream_ring::kNoTimestamp;
    int64_t duration = 0;
    uint32_t timebaseNum = 0;
    uint32_t timebaseDen = 0;
    bool keyframe = false;
    bool parameterSetsChanged = false;
    AvCompressedVideoGeometry geometry;
};

// SPS-gate layer 3: the stream gate's refusal of the last readAccessUnit (CompressedSyntax::inspect said OUT or
// MALFORMED for that AU). valid is false after a successful read, a non-gate failure, and every open. err is the
// refusal's unchanged err string. pts/timebase are the refused AU's own (for L4's flip offset).
struct AvGateRefusal {
    bool valid = false;
    gate::StreamVerdict verdict;
    std::string err;
    int64_t pts = ddr_bitstream_ring::kNoTimestamp;
    uint32_t timebaseNum = 0;
    uint32_t timebaseDen = 0;
};

constexpr size_t kCompressedPacketCountLimit = 32;
constexpr size_t kCompressedPacketByteLimit = 2 * 1024 * 1024;
constexpr size_t kCompressedPcmByteLimit = 48000 * 4 * 4;

enum class AvAudioProgress { Ready, Pending, Backpressure, Paused, Eof, Cancelled, Error };
enum class AvDemuxBlocked { Idle, Input, VideoQueue, PcmQueue, Cancelled, Eof, Error };

#if MPX_FPGA_AV_TRACE
// Independent atomic observations, not a coherent demux/PCM snapshot.
struct AvCompressedPressure {
    bool available = false, inputEof = false, audioEof = false, reservedVideo = false;
    size_t queuedPackets = 0, queuedBytes = 0, pcmBytes = 0;
    AvDemuxBlocked blocked = AvDemuxBlocked::Idle;
};
#endif

struct AvCompressedDiagnostics {
    bool available = false, detailsAvailable = false, sourceOpen = false;
    size_t queuedPackets = 0, queuedBytes = 0, pcmBytes = 0;
    int64_t nextQueuedPts = ddr_bitstream_ring::kNoTimestamp;
    bool inputEof = false, audioEof = false, cancelled = false;
    AvDemuxBlocked blocked = AvDemuxBlocked::Idle;
    std::optional<int> inputReadResult;
    bool inputReadCancelled = false;
    std::optional<int> ioError;
    std::optional<bool> ioEof;
    std::optional<int64_t> ioBytesRead, containerDurationUs;
    uint64_t inputVideoPackets = 0, returnedAccessUnits = 0;
    int64_t lastInputPts = ddr_bitstream_ring::kNoTimestamp;
    int64_t lastInputDuration = 0, lastAuPts = ddr_bitstream_ring::kNoTimestamp;
    int64_t firstAuPts = ddr_bitstream_ring::kNoTimestamp;
    int64_t lastAuDuration = 0;
    int inputTimebaseNum = 0, inputTimebaseDen = 0;
    int auTimebaseNum = 0, auTimebaseDen = 0;
    size_t queuedVideoPackets = 0;
    bool reservedVideo = false;
    // N2: the declared duration minus the session position when an EIO at the server's close
    // was taken as the end (negative: the input ran past the declared duration).
    int64_t inputCloseShortMs = kNoInputCloseAtEnd;
    // N2: the session position read so far (the play's start offset plus the longest stream
    // span since the start, or since this demux's own seek target); -1 before any packet.
    int64_t inputSessionMs = -1;
    std::string error;
};

std::string formatCompressedDiagnostics(const AvCompressedDiagnostics& state);

class AvInprocDecoder {
public:
    bool open(const std::string& pathOrUrl, const AvInprocOpenOpts& o, std::string& err);
    // 1=frame, 0=EOF, -1=error. Packed I420 (Y then U then V, no padding between planes).
    int readI420(uint8_t* dst, size_t frameBytes, std::string& err);
    // 1=complete AU, 0=drained EOF, -1=error. Original container rational PTS.
    int readAccessUnit(AvCompressedAccessUnit& au, std::string& err);
    // The stream gate's refusal behind the last readAccessUnit's -1, if that was a gate refusal (SPS-gate L3).
    const AvGateRefusal& lastGateRefusal() const { return gateRefusal_; }
    // The existing PCM consumer may advance the shared demux while video is paced.
    // One packet per call; video packets remain bounded and in original order.
    AvAudioProgress advanceCompressedAudio(std::string& err);
    AvCompressedDiagnostics compressedDiagnostics() const;
    // N2: kNoInputCloseAtEnd, or the declared duration minus the session position when the
    // input's EIO at the server's close was taken as the end (at most once per open input).
    int64_t inputClosedAtEndShortMs() const;
    unsigned inputCloseConversions() const;
#if MPX_FPGA_AV_TRACE
    // Same open/close lifetime contract as the other reader methods; no locks.
    AvCompressedPressure compressedPressure() const noexcept;
#endif
    SourceAspect sourceAspect() const;
    AvCompressedVideoGeometry videoGeometry() const;
    int64_t firstAudioPtsUs() const;
    // PCM from the same demux as readI420. 48 kHz s16le stereo. 0=empty/EOF.
    int drainPcm(uint8_t* dst, size_t n, bool wait = true);
    bool hasAudio() const;
    bool audioEof() const;
    // Unblock PCM/demux waiters. Join readers/audio before close or reopening a seek.
    void requestStop();
    void close();
    ~AvInprocDecoder() { close(); }
    // Compressed mode: actual SPS coded size, never the configured maximum.
    int width() const;
    int height() const;
    bool isOpen() const;
    static const char* libavIdent();

    AvInprocDecoder() = default;
    AvInprocDecoder(const AvInprocDecoder&) = delete;
    AvInprocDecoder& operator=(const AvInprocDecoder&) = delete;

private:
    bool openCompressed(const std::string&, const AvInprocOpenOpts&, std::string&);
    static int interruptThunk(void* p);
    struct Impl;
    Impl* impl_ = nullptr;
    AvGateRefusal gateRefusal_;
    std::atomic<int> abort_{0};
    const std::atomic<bool>* cancelled_ = nullptr;
    std::atomic<int64_t> ioDeadlineMs_{0};
    int readStallMs_ = kLanReadStallMs;
};

#endif // MPX_HAVE_LIBAV

} // namespace misterplex
