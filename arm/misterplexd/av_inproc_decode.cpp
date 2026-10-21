#ifndef MPX_HAVE_LIBAV
#error "av_inproc_decode.cpp requires -DMPX_HAVE_LIBAV"
#endif

#include "libmisterplex/av_inproc_decode.hpp"
#include "libmisterplex/pts_cadence.hpp"
#include "libmisterplex/h264_sps.hpp"
#include "libmisterplex/fpga_terminal.hpp"
#include "log_redact.hpp"
#include "av_open_opts.hpp"
#include "ca_bundle.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

namespace misterplex {
namespace {

std::string avErr(int ret) {
    char buf[128];
    if (av_strerror(ret, buf, sizeof(buf)) == 0)
        return std::string(buf);
    return "av error " + std::to_string(ret);
}

bool looksHttpUrl(const std::string& path) {
    size_t i = 0;
    while (i < path.size() &&
           (path[i] == ' ' || path[i] == '\t' || path[i] == '\r' || path[i] == '\n'))
        ++i;
    if (path.compare(i, 7, "http://") == 0 || path.compare(i, 7, "HTTP://") == 0)
        return true;
    if (path.compare(i, 8, "https://") == 0 || path.compare(i, 8, "HTTPS://") == 0)
        return true;
    return false;
}

bool looksNetworkUrl(const std::string& path) {
    const auto pos = path.find("://");
    if (pos == std::string::npos)
        return false;
    return path.compare(0, 5, "file:") != 0;
}

// True if annex-B payload contains an IDR VCL NAL (type 5).
bool annexbHasIdrNal(const uint8_t* data, size_t size) {
    if (!data || size < 4)
        return false;
    for (size_t i = 0; i + 4 < size;) {
        size_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        if (sc == 0) {
            ++i;
            continue;
        }
        const size_t nal = i + sc;
        if (nal >= size)
            break;
        const unsigned nt = data[nal] & 0x1fu;
        if (nt == 5u)
            return true;
        i = nal + 1;
    }
    return false;
}

// Drop SEI/AUD/filler so borderline seek IDRs fit the FPGA 8KiB AU ring.
// Keeps SPS/PPS/VCL. Returns false if no VCL remains.
bool annexbStripPaddingNals(std::vector<uint8_t>& out, const uint8_t* data, size_t size) {
    out.clear();
    if (!data || size < 4)
        return false;
    out.reserve(size);
    bool haveVcl = false;
    auto startCode = [&](size_t pos) -> size_t {
        if (pos + 3 < size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 &&
            data[pos + 3] == 1)
            return 4;
        if (pos + 2 < size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1)
            return 3;
        return 0;
    };
    size_t i = 0;
    while (i + 3 < size) {
        const size_t sc = startCode(i);
        if (!sc) {
            ++i;
            continue;
        }
        const size_t nal = i + sc;
        if (nal >= size)
            break;
        size_t end = nal + 1;
        while (end < size && !startCode(end))
            ++end;
        const unsigned nt = data[nal] & 0x1fu;
        // 6=SEI, 9=AUD, 12=filler — safe to drop for FPGA CAVLC path.
        if (nt != 6u && nt != 9u && nt != 12u) {
            out.insert(out.end(), data + i, data + end);
            if (nt == 1u || nt == 5u)
                haveVcl = true;
        }
        i = end;
    }
    return haveVcl && !out.empty();
}

std::string auCapacityError(size_t got, size_t limit) {
    return "access unit exceeds negotiated bounded ring capacity (" +
           std::to_string(got) + " > " + std::to_string(limit) + ")";
}

int64_t monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A container packet (including libav's raw/TS parser output) must be one whole
// picture. The initial product deliberately rejects multislice and B pictures.
struct CompressedSyntax {
    std::vector<uint8_t> sps, pps;
    uint32_t spsId = 0, ppsId = 0;
    uint32_t frameNumBits = 0, pocType = 0, pocLsbBits = 0;
    uint32_t maxReferences = 0;
    uint32_t l0DefaultActive = 1;  // PPS num_ref_idx_l0_default_active_minus1 + 1
    int initialQp = 26;
    bool deblockControl = false;
    SourceAspect aspect{};
    AvCompressedVideoGeometry geometry;
    int width = 0, height = 0;
    bool haveSps = false, havePps = false;
    unsigned spsProfile = 66;
    // 180 L3: the High SPS AU was accepted and host libx264 started, then the
    // next source AU (no SPS) still hit the baseline slice gate
    // (max_num_ref_frames=0) and the rewrite never emitted. Once a High SPS
    // has been accepted for host rewrite, later AUs in this session pass
    // through so the encoder can see them.
    bool hostRewriteArmed = false;
    // The typed verdict of the last inspect (pre-reg v1.4.3 section 3 (a)). Each refusal names the
    // first term that tripped, in its site's pinned order. The err strings are unchanged.
    gate::StreamVerdict verdict;

    bool inspect(const uint8_t* data, size_t size, const AvInprocOpenOpts& opts,
                 bool& idr, bool& changed, std::string& err) {
        idr = false;
        changed = false;
        verdict = {};
        int pictures = 0;
        // H3: the refs family reads the CAPS limit. MAX_REF_FRAMES 0 means absent = 1, so a
        // GateCaps below 1 is not a CAPS value and is floored to v1.
        const uint32_t capRefs =
            opts.gateCaps.maxRefs < 1 ? 1u : static_cast<uint32_t>(opts.gateCaps.maxRefs);
        // G9's declared-refs limit: the CAPS maxRefs, or 2 on the SPS-relax row (0x48C5385C, whose sps_parser
        // takes a declared max_num_ref_frames 2 while its slice path keeps one active reference). Only G9 and
        // its slice consistency check read it; the active-count checks (G18/G26) stay on capRefs.
        const uint32_t refsLimit = opts.spsRelaxNvenc && capRefs < 2 ? 2u : capRefs;
        const bool spsRelax = opts.spsRelaxNvenc;
        // Each refusal records its typed verdict next to the err string, which is unchanged.
        // Pinned order (H264 p2): within a NAL, syntax order. A field is checked as soon as it is
        // read, so syntax this gate does not parse (High-profile SPS fields, FMO, the interlace
        // flag) cannot misalign the reads that would name it, and a read past the RBSP or a ue
        // overflow is MALFORMED "truncated" at the field being read. All the terms of one d42a4460
        // OR chain share that chain's err string, so the order never changes an err.
        auto refuse = [&](gate::StreamOutcome kind, const char* field, int64_t value, int64_t limit,
                          const char* why, std::string msg) {
            verdict = {kind, field, value, limit, why};
            err = std::move(msg);
            return false;
        };
        auto out = [&](const char* field, int64_t value, int64_t limit, std::string msg) {
            return refuse(gate::StreamOutcome::Out, field, value, limit, "", std::move(msg));
        };
        auto malformed = [&](const char* field, int64_t value, int64_t limit, const char* why,
                             std::string msg) {
            return refuse(gate::StreamOutcome::Malformed, field, value, limit, why, std::move(msg));
        };
        auto start = [&](size_t i) -> size_t {
            if (i + 3 < size && !data[i] && !data[i + 1] && !data[i + 2] &&
                data[i + 3] == 1) return 4;
            if (i + 2 < size && !data[i] && !data[i + 1] && data[i + 2] == 1) return 3;
            return 0;
        };
        // 179 L6: the cabac=0 probe still returned profile_idc 100. Accept the
        // AU so the player can rewrite it. The baseline SPS gate stays when
        // acceptHighForHostRewrite is false.
        // 180 L3: after that first High SPS, a following P AU has no SPS and
        // was refused (max_num_ref_frames=0) before libx264 could emit.
        if (opts.acceptHighForHostRewrite && hostRewriteArmed && data && size >= 4) {
            idr = false;
            return true;
        }
        if (opts.acceptHighForHostRewrite && data && size >= 5) {
            bool highAu = false;
            for (size_t i = 0; i + 5 < size; ++i) {
                size_t sc = 0;
                if (!data[i] && !data[i + 1] && !data[i + 2] && data[i + 3] == 1)
                    sc = 4;
                else if (!data[i] && !data[i + 1] && data[i + 2] == 1)
                    sc = 3;
                if (!sc || i + sc + 1 >= size)
                    continue;
                if ((data[i + sc] & 31) == 7) {
                    const unsigned profile = data[i + sc + 1];
                    if (profile == 77 || profile == 100)
                        highAu = true;
                }
                i += sc;
            }
            if (highAu) {
                hostRewriteArmed = true;
                spsProfile = 100;
                haveSps = true;
                havePps = true;
                width = opts.expectW;
                height = opts.expectH;
                idr = true;
                return true;
            }
        }
        size_t pos = 0;
        while (pos < size && data[pos] == 0 && !start(pos)) ++pos;
        while (pos < size) {
            const size_t sc = start(pos);
            if (!sc || pos + sc >= size)
                return malformed("annexb_start_code", 0, 0, "start code",
                                 "malformed Annex-B access unit");
            const size_t hdr = pos + sc;
            size_t end = hdr + 1;
            while (end < size && !start(end)) ++end;
            const uint8_t type = data[hdr] & 31;
            if (type == 1 && opts.requireAllIdr)
                return out("nal_unit_type", 1, 5,
                           "selected all-IDR profile requires every picture to be IDR");
            if (data[hdr] & 128)
                return malformed("forbidden_zero_bit", 1, 0, "forbidden bit", "forbidden H.264 NAL bit");
            auto rbsp = detail::removeEpb(data + hdr + 1, end - hdr - 1);
            if ((type == 1 || type == 5) && rbsp.size() > opts.maxVclRbspBytes)
                return out("vcl_rbsp_bytes", static_cast<int64_t>(rbsp.size()),
                           static_cast<int64_t>(opts.maxVclRbspBytes),
                           "VCL RBSP exceeds the decoder frontend capacity");
            detail::BitReader br(rbsp.data(), rbsp.size());
            if (type == 7) {
                // 7.3.2.1.1; every term is the d42a4460 :211-216 chain (err :217).
                static const char* const kSpsErr =
                    "unsupported SPS: requires 8-bit baseline/CAVLC progressive ref<=1 geometry";
                auto cut = [&](const char* field) {
                    return malformed(field, 0, 0, "truncated", kSpsErr);
                };
                const unsigned profile = br.u(8);
                if (!br.ok) return cut("profile_idc");
                // High core takes Main (77). High (100) still fails here: its SPS
                // tail is a different parse, and the ladder asks for main.
                if (profile != 66 && !(opts.allowHighCabac && profile == 77))
                    return out("profile_idc", profile, 66, kSpsErr);
                spsProfile = profile;
                br.u(8);
                if (!br.ok) return cut("constraint_set_flags");
                const unsigned level = br.u(8);
                if (!br.ok) return cut("level_idc");
                if (level > 30) return out("level_idc", level, 30, kSpsErr);
                const uint32_t sid = br.ue();
                if (!br.ok) return cut("seq_parameter_set_id");
                if (sid > 31) return malformed("seq_parameter_set_id", sid, 31, "range", kSpsErr);
                const uint32_t frameBits = br.ue();
                if (!br.ok) return cut("log2_max_frame_num_minus4");
                if (frameBits > 12)
                    return malformed("log2_max_frame_num_minus4", frameBits, 12, "range", kSpsErr);
                const uint32_t poc = br.ue();
                if (!br.ok) return cut("pic_order_cnt_type");
                if (poc == 1) return out("pic_order_cnt_type", 1, -1, kSpsErr);
                if (poc > 2) return malformed("pic_order_cnt_type", poc, 2, "range", kSpsErr);
                uint32_t pocBits = 0;
                if (poc == 0) {
                    pocBits = br.ue();
                    if (!br.ok) return cut("log2_max_pic_order_cnt_lsb_minus4");
                    if (pocBits > 12)
                        return malformed("log2_max_pic_order_cnt_lsb_minus4", pocBits, 12, "range",
                                         kSpsErr);
                }
                const uint32_t refs = br.ue();
                if (!br.ok) return cut("max_num_ref_frames");
                // G9 (H3): max_num_ref_frames <= refsLimit (CAPS maxRefs, or 2 on spsRelaxNvenc).
                // Record-only (H264): level > 30 stays refused above. At L3.0, 720x480 (1350 MBs)
                // allows MaxDpbFrames 6, so maxRefs <= 3 is inside the level DPB; an L3.1/720p
                // grant must re-check A.3.1. Active refs (G18/G26) stay on capRefs.
                if (refs > refsLimit) return out("max_num_ref_frames", refs, refsLimit, kSpsErr);
                const bool gaps = br.u1();
                if (!br.ok) return cut("gaps_in_frame_num_value_allowed_flag");
                if (gaps) return out("gaps_in_frame_num_value_allowed_flag", 1, 0, kSpsErr);
                const uint32_t mbw = br.ue() + 1;
                if (!br.ok) return cut("pic_width_in_mbs_minus1");
                const uint32_t capMbw = static_cast<unsigned>(opts.expectW / 16);
                if (mbw > capMbw) return out("pic_width_in_mbs", mbw, capMbw, kSpsErr);
                const uint32_t mbh = br.ue() + 1;
                if (!br.ok) return cut("pic_height_in_map_units_minus1");
                const uint32_t capMbh = static_cast<unsigned>(opts.expectH / 16);
                if (mbh > capMbh) return out("pic_height_in_map_units", mbh, capMbh, kSpsErr);
                const bool progressive = br.u1();
                if (!br.ok) return cut("frame_mbs_only_flag");
                if (!progressive) return out("frame_mbs_only_flag", 0, 1, kSpsErr);
                br.u1(); // direct_8x8_inference_flag
                if (!br.ok) return cut("direct_8x8_inference_flag");
                uint32_t left = 0, right = 0, top = 0, bottom = 0;
                if (br.u1()) {
                    left = br.ue(); right = br.ue(); top = br.ue(); bottom = br.ue();
                }
                if (!br.ok) return cut("frame_cropping");
                if (left + right >= mbw * 8)
                    return malformed("frame_crop_left_right_offset", left + right,
                                     static_cast<int64_t>(mbw) * 8 - 1, "range", kSpsErr);
                if (top + bottom >= mbh * 8)
                    return malformed("frame_crop_top_bottom_offset", top + bottom,
                                     static_cast<int64_t>(mbh) * 8 - 1, "range", kSpsErr);
                const int w = static_cast<int>(mbw * 16 - 2 * (left + right));
                const int h = static_cast<int>(mbh * 16 - 2 * (top + bottom));
                AvCompressedVideoGeometry nextGeometry;
                nextGeometry.codedWidth = static_cast<int>(mbw * 16);
                nextGeometry.codedHeight = static_cast<int>(mbh * 16);
                nextGeometry.visibleWidth = w;
                nextGeometry.visibleHeight = h;
                nextGeometry.macroblockColumns = static_cast<int>(mbw);
                nextGeometry.macroblockRows = static_cast<int>(mbh);
                nextGeometry.cropLeft = static_cast<int>(left * 2);
                nextGeometry.cropRight = static_cast<int>(right * 2);
                nextGeometry.cropTop = static_cast<int>(top * 2);
                nextGeometry.cropBottom = static_cast<int>(bottom * 2);
                static const char* const kGeomErr = "unsupported mid-session geometry change";
                if (w > opts.expectW) return out("visible_width", w, opts.expectW, kGeomErr);
                if (h > opts.expectH) return out("visible_height", h, opts.expectH, kGeomErr);
                if (haveSps && !geometry.samePictureLayout(nextGeometry))
                    return out("picture_layout_change", 1, 0, kGeomErr);
                aspect = {};
                const bool hasVui = br.u1();
                if (hasVui && br.u1()) {
                    const unsigned aspectIdc = br.u(8);
                    static constexpr unsigned sar[][2] = {
                        {0,0}, {1,1}, {12,11}, {10,11}, {16,11}, {40,33},
                        {24,11}, {20,11}, {32,11}, {80,33}, {18,11}, {15,11},
                        {64,33}, {160,99}, {4,3}, {3,2}, {2,1}
                    };
                    unsigned num = 0, den = 0;
                    if (aspectIdc == 255) {
                        num = br.u(16); den = br.u(16);
                    } else if (aspectIdc < sizeof(sar) / sizeof(sar[0])) {
                        num = sar[aspectIdc][0]; den = sar[aspectIdc][1];
                    }
                    if (br.ok && num && den) {
                        // Bound to the 12-bit PLXA terms (source_aspect_ack.sv): av_reduce
                        // returns the closest ratio with both terms <= kSourceAspectMax.
                        int darNum = 0, darDen = 0;
                        av_reduce(&darNum, &darDen, int64_t(w) * num,
                                  int64_t(h) * den, kSourceAspectMax);
                        aspect = {static_cast<uint16_t>(darNum),
                                  static_cast<uint16_t>(darDen), darNum > 0 && darDen > 0};
                    }
                }
                if (hasVui) {
                    if (br.u1()) br.u1(); // overscan
                    nextGeometry.videoSignalPresent = br.u1();
                    if (nextGeometry.videoSignalPresent) {
                        br.u(3);
                        nextGeometry.fullRange = br.u1();
                        nextGeometry.colorDescriptionPresent = br.u1();
                        if (nextGeometry.colorDescriptionPresent) {
                            nextGeometry.colorPrimaries = br.u(8);
                            nextGeometry.transferCharacteristics = br.u(8);
                            nextGeometry.matrixCoefficients = br.u(8);
                        }
                    }
                }
                if (!br.ok)
                    return malformed("vui_parameters", 0, 0, "truncated",
                                     "truncated SPS VUI aspect/color description");
                const bool badMatrix = nextGeometry.matrixCoefficients != 2 &&
                                       nextGeometry.matrixCoefficients != 5 &&
                                       nextGeometry.matrixCoefficients != 6;
                if (nextGeometry.fullRange || badMatrix ||
                    (opts.requireLimitedBt601 && !nextGeometry.limitedBt601Signaled())) {
                    std::string msg =
                        "fixed limited-BT601 scanout color policy rejected SPS: "
                        "full_range=" + std::to_string(nextGeometry.fullRange) +
                        " matrix_coefficients=" + std::to_string(nextGeometry.matrixCoefficients) +
                        " description_present=" + std::to_string(nextGeometry.colorDescriptionPresent);
                    if (nextGeometry.fullRange)
                        return out("video_full_range_flag", 1, 0, std::move(msg));
                    if (badMatrix)
                        return out("matrix_coefficients", nextGeometry.matrixCoefficients, -1,
                                   std::move(msg));
                    return out("colour_description_present_flag", 0, 1, std::move(msg));
                }
                if (hasVui) {
                    // E.1.1 past the colour description, every core (sps_parser parses the whole VUI).
                    // NAL/VCL HRD and pic_struct_present_flag 1 are OUT unless the row has spsRelaxNvenc (older
                    // cores fail ST_NAL_HRD / ST_VCL_HRD / ST_PIC_STRUCT). bitstream_restriction: no reordering,
                    // max_dec_frame_buffering <= 1 (<= 2 with spsRelaxNvenc).
                    static const char* const kVuiErr = "truncated SPS VUI timing/HRD/bitstream_restriction";
                    auto vcut = [&](const char* field) {
                        return malformed(field, 0, 0, "truncated", kVuiErr);
                    };
                    // HRD bit_rate/cpb_size ue values span up to 32 bits (more leading zeros than BitReader::ue).
                    auto ue32 = [&](uint64_t& v) {
                        int z = 0;
                        while (br.ok && br.u1() == 0 && z <= 32)
                            ++z;
                        if (!br.ok || z > 31) {
                            br.ok = false;
                            return false;
                        }
                        v = (uint64_t(1) << z) - 1 + br.u(z);
                        return br.ok;
                    };
                    auto hrd = [&]() {
                        const uint32_t cpbCnt = br.ue();
                        if (!br.ok) return vcut("cpb_cnt_minus1");
                        if (cpbCnt > 31) return malformed("cpb_cnt_minus1", cpbCnt, 31, "range", kSpsErr);
                        br.u(4); // bit_rate_scale
                        br.u(4); // cpb_size_scale
                        if (!br.ok) return vcut("cpb_size_scale");
                        for (uint32_t i = 0; i <= cpbCnt; ++i) {
                            uint64_t v = 0;
                            if (!ue32(v)) return vcut("bit_rate_value_minus1");
                            if (!ue32(v)) return vcut("cpb_size_value_minus1");
                            br.u1(); // cbr_flag
                            if (!br.ok) return vcut("cbr_flag");
                        }
                        br.u(5); br.u(5); br.u(5); br.u(5); // delay lengths, time_offset_length
                        if (!br.ok) return vcut("time_offset_length");
                        return true;
                    };
                    if (br.u1()) { br.ue(); br.ue(); } // chroma_loc_info
                    if (!br.ok) return vcut("chroma_loc_info");
                    if (br.u1()) { br.u(32); br.u(32); br.u1(); } // timing_info
                    if (!br.ok) return vcut("timing_info");
                    const bool nalHrd = br.u1();
                    if (!br.ok) return vcut("nal_hrd_parameters_present_flag");
                    if (nalHrd && !spsRelax) return out("nal_hrd_parameters_present_flag", 1, 0, kSpsErr);
                    if (nalHrd && !hrd()) return false;
                    const bool vclHrd = br.u1();
                    if (!br.ok) return vcut("vcl_hrd_parameters_present_flag");
                    if (vclHrd && !spsRelax) return out("vcl_hrd_parameters_present_flag", 1, 0, kSpsErr);
                    if (vclHrd && !hrd()) return false;
                    if (nalHrd || vclHrd) br.u1(); // low_delay_hrd_flag
                    if (!br.ok) return vcut("low_delay_hrd_flag");
                    const bool picStruct = br.u1();
                    if (!br.ok) return vcut("pic_struct_present_flag");
                    if (picStruct && !spsRelax) return out("pic_struct_present_flag", 1, 0, kSpsErr);
                    const bool restriction = br.u1();
                    if (!br.ok) return vcut("bitstream_restriction_flag");
                    if (restriction) {
                        br.u1(); // motion_vectors_over_pic_boundaries_flag
                        br.ue(); br.ue(); br.ue(); br.ue(); // max_bytes/bits denoms, log2_max_mv_length h/v
                        const uint32_t reorder = br.ue();
                        if (!br.ok) return vcut("max_num_reorder_frames");
                        if (reorder) return out("max_num_reorder_frames", reorder, 0, kSpsErr);
                        const uint32_t decBuf = br.ue();
                        if (!br.ok) return vcut("max_dec_frame_buffering");
                        const uint32_t decBufLimit = spsRelax ? 2u : 1u;
                        if (decBuf > decBufLimit)
                            return out("max_dec_frame_buffering", decBuf, decBufLimit, kSpsErr);
                    }
                }
                nextGeometry.sourceAspect = aspect;
                geometry = nextGeometry;
                std::vector<uint8_t> next(data + hdr, data + end);
                changed |= !haveSps || next != sps;
                if (haveSps && spsId != sid)
                    havePps = false;
                sps = std::move(next);
                spsId = sid; width = w; height = h; haveSps = true;
                maxReferences = refs;
                frameNumBits = frameBits + 4;
                pocType = poc;
                pocLsbBits = pocBits + 4;
            } else if (type == 8) {
                // 7.3.2.2; every term is the d42a4460 :315-318 chain (err :319). That chain tested
                // redundant_pic_cnt_present_flag before the QP terms; syntax order reads it last.
                static const char* const kPpsErr =
                    "unsupported PPS: CABAC/FMO/weighted/multiple references";
                auto cut = [&](const char* field) {
                    return malformed(field, 0, 0, "truncated", kPpsErr);
                };
                const uint32_t pid = br.ue();
                if (!br.ok) return cut("pic_parameter_set_id");
                if (pid > 255) return malformed("pic_parameter_set_id", pid, 255, "range", kPpsErr);
                const uint32_t sid = br.ue();
                if (!br.ok) return cut("seq_parameter_set_id");
                if (!haveSps) return malformed("seq_parameter_set_id", sid, -1, "no SPS", kPpsErr);
                if (sid != spsId) return out("seq_parameter_set_id", sid, spsId, kPpsErr);
                const bool cabac = br.u1();
                if (!br.ok) return cut("entropy_coding_mode_flag");
                if (cabac && !opts.allowHighCabac)
                    return out("entropy_coding_mode_flag", 1, 0, kPpsErr);
                const bool bottomPoc = br.u1();
                if (!br.ok) return cut("bottom_field_pic_order_in_frame_present_flag");
                if (bottomPoc)
                    return out("bottom_field_pic_order_in_frame_present_flag", 1, 0, kPpsErr);
                const uint32_t groups = br.ue();
                if (!br.ok) return cut("num_slice_groups_minus1");
                if (groups) return out("num_slice_groups_minus1", groups, 0, kPpsErr);
                const uint32_t l0 = br.ue();
                if (!br.ok) return cut("num_ref_idx_l0_default_active_minus1");
                // G18 (H3): the default active count, minus1 + 1, <= the CAPS maxRefs (H264 p3).
                const uint64_t l0Active = uint64_t(l0) + 1;
                if (l0Active > capRefs)
                    return out("num_ref_idx_l0_default_active", static_cast<int64_t>(l0Active), capRefs,
                               kPpsErr);
                const uint32_t l1 = br.ue();
                if (!br.ok) return cut("num_ref_idx_l1_default_active_minus1");
                if (l1)
                    return out("num_ref_idx_l1_default_active", static_cast<int64_t>(l1) + 1, 1, kPpsErr);
                const bool weighted = br.u1();
                if (!br.ok) return cut("weighted_pred_flag");
                if (weighted) return out("weighted_pred_flag", 1, 0, kPpsErr);
                const uint32_t bipred = br.u(2);
                if (!br.ok) return cut("weighted_bipred_idc");
                if (bipred > 2) return malformed("weighted_bipred_idc", bipred, 2, "range", kPpsErr);
                if (bipred) return out("weighted_bipred_idc", bipred, 0, kPpsErr);
                const int qp = br.se();
                if (!br.ok) return cut("pic_init_qp_minus26");
                if (qp < -26 || qp > 25)
                    return malformed("pic_init_qp_minus26", qp, qp < -26 ? -26 : 25, "range", kPpsErr);
                const int qs = br.se();
                if (!br.ok) return cut("pic_init_qs_minus26");
                if (qs < -26 || qs > 25)
                    return malformed("pic_init_qs_minus26", qs, qs < -26 ? -26 : 25, "range", kPpsErr);
                const int chroma = br.se();
                if (!br.ok) return cut("chroma_qp_index_offset");
                if (chroma < -12 || chroma > 12)
                    return malformed("chroma_qp_index_offset", chroma, chroma < -12 ? -12 : 12, "range",
                                     kPpsErr);
                const bool deblock = br.u1();
                if (!br.ok) return cut("deblocking_filter_control_present_flag");
                br.u1(); // constrained_intra_pred_flag
                if (!br.ok) return cut("constrained_intra_pred_flag");
                const bool redundant = br.u1();
                if (!br.ok) return cut("redundant_pic_cnt_present_flag");
                if (redundant) return out("redundant_pic_cnt_present_flag", 1, 0, kPpsErr);
                // Baseline permits no transform/scaling-list extension. A Main PPS
                // writes transform_8x8, scaling, and second_chroma before the stop bit.
                if (br.bit < rbsp.size() * 8) {
                    auto moreThanTrailing = [&]() {
                        detail::BitReader peek = br;
                        if (peek.u1() == 0)
                            return true;
                        while (peek.ok && peek.bit < peek.n * 8) {
                            if (peek.u1() != 0)
                                return true;
                        }
                        return false;
                    };
                    if (opts.allowHighCabac && (spsProfile == 77 || spsProfile == 100) &&
                        moreThanTrailing()) {
                        const bool xform8 = br.u1();
                        if (!br.ok) return cut("transform_8x8_mode_flag");
                        if (xform8)
                            return out("transform_8x8_mode_flag", 1, 0, kPpsErr);
                        const bool scaling = br.u1();
                        if (!br.ok) return cut("pic_scaling_matrix_present_flag");
                        if (scaling)
                            return out("pic_scaling_matrix_present_flag", 1, 0, kPpsErr);
                        const int secondChroma = br.se();
                        if (!br.ok) return cut("second_chroma_qp_index_offset");
                        if (secondChroma < -12 || secondChroma > 12)
                            return malformed("second_chroma_qp_index_offset", secondChroma,
                                             secondChroma < -12 ? -12 : 12, "range", kPpsErr);
                    }
                    if (br.bit < rbsp.size() * 8) {
                        const bool stopBit = br.u1();
                        bool trailingZero = true;
                        while (br.bit < rbsp.size() * 8)
                            trailingZero &= br.u1() == 0;
                        if (!stopBit || !trailingZero)
                            return out("pps_rbsp_extension", 1, 0, "unsupported PPS extension");
                    }
                }
                std::vector<uint8_t> next(data + hdr, data + end);
                changed |= !havePps || next != pps;
                pps = std::move(next); ppsId = pid; havePps = true;
                l0DefaultActive = static_cast<uint32_t>(l0Active);
                initialQp = 26 + qp;
                deblockControl = deblock;
            } else if (type == 1 || type == 5) {
                // The NAL header, then 7.3.3 in syntax order; the slice_type rules that need the
                // active SPS/PPS run once pic_parameter_set_id is bound. Every term is the
                // d42a4460 :340-345 chain (err :346).
                static const char* const kSliceErr =
                    "unsupported slice: requires single complete I/P picture";
                auto cut = [&](const char* field) {
                    return malformed(field, 0, 0, "truncated", kSliceErr);
                };
                if (type == 5 && (data[hdr] & 0x60) == 0)
                    return malformed("nal_ref_idc", 0, 1, "IDR nal_ref_idc 0", kSliceErr);
                const uint32_t firstMb = br.ue();
                if (!br.ok) return cut("first_mb_in_slice");
                if (firstMb != 0) return out("first_mb_in_slice", firstMb, 0, kSliceErr);
                const uint32_t sliceType = br.ue();
                if (!br.ok) return cut("slice_type");
                if (sliceType > 9) return malformed("slice_type", sliceType, 9, "range", kSliceErr);
                const uint32_t kind = sliceType % 5;
                // An IDR is I, or SI (conforming, but SI is unsupported).
                if (type == 5 && kind == 4) return out("slice_type", sliceType, -1, kSliceErr);
                if (type == 5 && kind != 2)
                    return malformed("slice_type", sliceType, -1, "IDR not I", kSliceErr);
                const uint32_t pid = br.ue();
                if (!br.ok) return cut("pic_parameter_set_id");
                if (!havePps) return malformed("pic_parameter_set_id", pid, -1, "no PPS", kSliceErr);
                if (pid != ppsId) return out("pic_parameter_set_id", pid, ppsId, kSliceErr);
                // G24 (H264 p1): a P slice under max_num_ref_frames 0 has nothing to reference.
                if (kind == 0 && maxReferences == 0)
                    return malformed("max_num_ref_frames", 0, 1, "P without references", kSliceErr);
                // Consistency only: the SPS gate (G9) already refused max_num_ref_frames > refsLimit.
                if (kind == 0 && maxReferences > refsLimit)
                    return out("max_num_ref_frames", maxReferences, refsLimit, kSliceErr);
                if (kind != 2 && !(kind == 0 && opts.allowInter))
                    return out("slice_type", sliceType, -1, kSliceErr);
                if (++pictures != 1) return out("pictures_per_access_unit", pictures, 1, kSliceErr);
                // A read past the RBSP after :346 ends in this err in d42a4460: single-bit reads
                // return 0 there and the frame_num/POC values are never tested, so no check before
                // :389 fires on them. The one multi-bit value tested before :389, the override's ue,
                // is handled at its read. The verdict names the truncation where it happens.
                static const char* const kHeaderErr =
                    "unsupported/truncated slice header or unnegotiated deblocking";
                auto cutHeader = [&](const char* field) {
                    return malformed(field, 0, 0, "truncated", kHeaderErr);
                };
                br.u(frameNumBits);
                if (!br.ok) return cutHeader("frame_num");
                if (type == 5) br.ue();
                if (!br.ok) return cutHeader("idr_pic_id");
                if (pocType == 0) br.u(pocLsbBits);
                if (!br.ok) return cutHeader("pic_order_cnt_lsb");
                if (kind == 0) {
                    uint64_t active = l0DefaultActive;
                    const char* activeField = "num_ref_idx_l0_default_active";
                    const bool overrideFlag = br.u1();
                    if (!br.ok) return cutHeader("num_ref_idx_active_override_flag");
                    if (overrideFlag) {
                        const uint32_t minus1 = br.ue();
                        // A ue cut inside its suffix returns a nonzero partial value with br.ok
                        // false: d42a4460 :353 refused that with :354's err (a zero partial reached
                        // :389's). The err is kept; the verdict names the truncation.
                        if (!br.ok)
                            return malformed("num_ref_idx_l0_active_minus1", 0, 0, "truncated",
                                             minus1 != 0 ? "multiple active references are unsupported"
                                                         : kHeaderErr);
                        active = uint64_t(minus1) + 1;
                        // G26 (H3): the override's active count, minus1 + 1, <= the CAPS maxRefs.
                        activeField = "num_ref_idx_l0_active_override";
                        if (active > capRefs)
                            return out(activeField, static_cast<int64_t>(active), capRefs,
                                       "multiple active references are unsupported");
                    }
                    // review 03:04 (p3), re-labelled by review 04:16 (n1): an active count above
                    // max_num_ref_frames is legal H.264 (7.4.2.2, 7.4.3; the extra RefPicList0
                    // entries are "no reference picture", 8.2.4.2), but this decoder has only
                    // max_num_ref_frames DPB slots, so it is OUT for us, never MALFORMED. It is
                    // checked here, on the effective count, never at the PPS: an all-intra stream
                    // pairs max_num_ref_frames 0 with a default active count of 1 (x264 keyint=1).
                    if (active > maxReferences)
                        return refuse(gate::StreamOutcome::Out, activeField, static_cast<int64_t>(active),
                                      maxReferences, "active > max_num_ref_frames",
                                      "active reference count exceeds max_num_ref_frames");
                    const bool rplm = br.u1();
                    if (!br.ok) return cutHeader("ref_pic_list_modification_flag_l0");
                    if (rplm)
                        return out("ref_pic_list_modification_flag_l0", 1, 0,
                                   "reference list modification is unsupported");
                }
                if ((data[hdr] >> 5) & 3) {
                    if (type == 5) {
                        br.u1(); // no_output_of_prior_pics_flag
                        const bool longTerm = br.u1();
                        if (!br.ok) return cutHeader("long_term_reference_flag");
                        if (longTerm)
                            return out("long_term_reference_flag", 1, 0,
                                       "long-term IDR references are unsupported");
                    } else {
                        const bool adaptive = br.u1();
                        if (!br.ok) return cutHeader("adaptive_ref_pic_marking_mode_flag");
                        if (adaptive)
                            return out("adaptive_ref_pic_marking_mode_flag", 1, 0,
                                       "adaptive reference marking is unsupported");
                    }
                }
                const int qp = initialQp + br.se();
                if (!br.ok) return cutHeader("slice_qp_delta");
                if (qp < 0 || qp > 51) return malformed("slice_qp", qp, qp < 0 ? 0 : 51, "range", kHeaderErr);
                uint32_t disableFilter = 0;
                if (deblockControl) {
                    disableFilter = br.ue();
                    if (!br.ok) return cutHeader("disable_deblocking_filter_idc");
                    if (disableFilter > 2)
                        return malformed("disable_deblocking_filter_idc", disableFilter, 2, "range",
                                         kHeaderErr);
                    if (disableFilter != 1) {
                        const int alpha = br.se();
                        if (!br.ok) return cutHeader("slice_alpha_c0_offset_div2");
                        if (alpha < -6 || alpha > 6)
                            return malformed("slice_alpha_c0_offset_div2", alpha, alpha < 0 ? -6 : 6,
                                             "range", kHeaderErr);
                        const int beta = br.se();
                        if (!br.ok) return cutHeader("slice_beta_offset_div2");
                        if (beta < -6 || beta > 6)
                            return malformed("slice_beta_offset_div2", beta, beta < 0 ? -6 : 6, "range",
                                             kHeaderErr);
                    }
                }
                // filter=off cores do not run in-loop deblock. Prefer disable_idc=1
                // (PMS FPGA ladder), but also accept idc=0 from host libx264 lab proxy
                // (no-deblock encode still often emits idc=0). idc=2 remains rejected.
                if (!opts.allowDeblock && disableFilter == 2)
                    return out("disable_deblocking_filter_idc", 2, 1, kHeaderErr);
                idr |= type == 5;
            } else if (type != 6 && type != 9 && type != 10 && type != 11 && type != 12) {
                return out("nal_unit_type", type, -1, "unsupported H.264 NAL type " + std::to_string(type));
            }
            pos = end;
        }
        if (pictures != 1 || (changed && !idr)) {
            static const char* const kAuErr =
                "access unit lacks one complete picture or changes SPS/PPS outside IDR";
            if (pictures != 1)
                return malformed("pictures_per_access_unit", pictures, 1, "no picture", kAuErr);
            return out("parameter_set_change_outside_idr", 1, 0, kAuErr);
        }
        return true;
    }
};

bool copyPackedI420(const AVFrame* fr, uint8_t* dst, int w, int h, std::string& err) {
    if (!fr || !dst || !fr->data[0] || !fr->data[1] || !fr->data[2]) {
        err = "missing I420 plane";
        return false;
    }
    const int cw = w / 2;
    const int ch = h / 2;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1) ||
        fr->linesize[0] < w || fr->linesize[1] < cw || fr->linesize[2] < cw) {
        err = "I420 linesize/size invalid";
        return false;
    }
    const size_t ysz = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t csz = static_cast<size_t>(cw) * static_cast<size_t>(ch);
    if (fr->linesize[0] == w && fr->linesize[1] == cw && fr->linesize[2] == cw) {
        std::memcpy(dst, fr->data[0], ysz);
        std::memcpy(dst + ysz, fr->data[1], csz);
        std::memcpy(dst + ysz + csz, fr->data[2], csz);
        return true;
    }
    for (int row = 0; row < h; ++row)
        std::memcpy(dst + static_cast<size_t>(row) * static_cast<size_t>(w),
                    fr->data[0] + static_cast<size_t>(row) * static_cast<size_t>(fr->linesize[0]),
                    static_cast<size_t>(w));
    uint8_t* u = dst + ysz;
    uint8_t* v = u + csz;
    for (int row = 0; row < ch; ++row) {
        std::memcpy(u + static_cast<size_t>(row) * static_cast<size_t>(cw),
                    fr->data[1] + static_cast<size_t>(row) * static_cast<size_t>(fr->linesize[1]),
                    static_cast<size_t>(cw));
        std::memcpy(v + static_cast<size_t>(row) * static_cast<size_t>(cw),
                    fr->data[2] + static_cast<size_t>(row) * static_cast<size_t>(fr->linesize[2]),
                    static_cast<size_t>(cw));
    }
    return true;
}

} // namespace

int16_t s16sat(int v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return static_cast<int16_t>(v);
}

int avFrameChannels(const AVFrame* fr, const AVCodecContext* ctx) {
    if (fr && fr->ch_layout.nb_channels > 0)
        return fr->ch_layout.nb_channels;
    if (ctx && ctx->ch_layout.nb_channels > 0)
        return ctx->ch_layout.nb_channels;
    return 1;
}

struct AvInprocDecoder::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVCodecContext* acodec = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    AVBSFContext* bsf = nullptr;
    SwrContext* resampler = nullptr;
    int resampleRate = 0, resampleFormat = -1, resampleChannels = 0;
    std::deque<AVPacket*> queuedPackets;
    AVPacket* aheadPacket = nullptr;
    size_t queuedBytes = 0;
    std::mutex compressedMu;
    std::atomic<size_t> diagnosticPackets{0}, diagnosticBytes{0};
    std::atomic<int64_t> diagnosticNextPts{ddr_bitstream_ring::kNoTimestamp};
    std::atomic<AvDemuxBlocked> blocked{AvDemuxBlocked::Idle};
    std::atomic<bool> inputEof{false};
#if MPX_FPGA_AV_TRACE
    std::atomic<size_t> diagnosticPcmBytes{0};
    std::atomic<bool> diagnosticAudioEof{false}, diagnosticReservedVideo{false};
#endif
    std::string compressedError;
    AvInprocOpenOpts options;
    CompressedSyntax syntax;
    bool compressed = false;
    bool bsfFlushed = false;
    bool audioFlushed = false;
    std::string audioError;
    std::atomic<int64_t> audioPtsUs{ddr_bitstream_ring::kNoTimestamp};
    SourceAspect aspect{};
    int64_t previousPts = ddr_bitstream_ring::kNoTimestamp;
    int64_t firstPts = ddr_bitstream_ring::kNoTimestamp;
    uint64_t accessUnits = 0;
    std::optional<int> inputReadResult;
    bool inputReadCancelled = false;
    uint64_t inputVideoPackets = 0;
    int64_t lastInputPts = AV_NOPTS_VALUE, lastInputDuration = 0, lastAuDuration = 0;
    AVRational inputTimebase{0, 0};
    // N2 (F2): per used stream (0 video, 1 audio), the first pts and the furthest pts +
    // duration read, in AV_TIME_BASE units; the span is measured per stream.
    int64_t spanFirstUs[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    int64_t spanEndUs[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    bool spanPinned = false;  // after this demux's own seek: the start is the seek target
    std::atomic<int64_t> inputCloseShortMs{kNoInputCloseAtEnd};
    std::atomic<unsigned> inputCloseConversions{0};
    int64_t seekTargetPts = AV_NOPTS_VALUE;
    bool seekChecked = false;
    unsigned oversizeAuSkips = 0;
    int vidx = -1;
    int aidx = -1;
    int w = 0;
    int h = 0;
    int srcW = 0;
    int srcH = 0;
    int arate = 0;
    bool open = false;
    bool flushing = false;
    bool pending = false;
    bool audioEof = false;
    bool stopping = false;
    std::mutex pcmMu;
    std::condition_variable pcmCv;
    std::vector<uint8_t> pcm;

    void pushPcm48(const uint8_t* p, size_t n) {
        if (!p || n == 0)
            return;
        std::unique_lock<std::mutex> lk(pcmMu);
        constexpr size_t kCap = kCompressedPcmByteLimit;
        if (compressed) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!stopping && !audioEof && pcm.size() + n > kCap) {
                blocked.store(AvDemuxBlocked::PcmQueue);
                if (options.cancelled && options.cancelled->load()) return;
                if (options.paused && options.paused->load())
                    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                else if (std::chrono::steady_clock::now() >= deadline) {
                    audioError = "PCM backpressure timeout";
                    return;
                }
                pcmCv.wait_for(lk, std::chrono::milliseconds(20));
            }
            if (stopping || audioEof) return;
        }
        pcm.insert(pcm.end(), p, p + n);
        if (pcm.size() > kCap)
            pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(pcm.size() - kCap));
        MPX_AV_TRACE(diagnosticPcmBytes.store(pcm.size(), std::memory_order_relaxed);)
        pcmCv.notify_all();
    }

    void ingestAudioFrame() {
        if (!aframe)
            return;
        const int n = aframe->nb_samples;
        if (n <= 0)
            return;
        const int ch = avFrameChannels(aframe, acodec);
        int rate = aframe->sample_rate > 0 ? aframe->sample_rate : arate;
        if (rate <= 0)
            rate = 48000;
        if (compressed) {
            if (!resampler) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 48000,
                        &aframe->ch_layout, static_cast<AVSampleFormat>(aframe->format),
                        rate, 0, nullptr) < 0 || swr_init(resampler) < 0) {
                    audioError = "audio resampler initialization failed";
                    return;
                }
                resampleRate = rate;
                resampleFormat = aframe->format;
                resampleChannels = ch;
            }
            if (rate != resampleRate || aframe->format != resampleFormat ||
                ch != resampleChannels) {
                audioError = "unsupported mid-session audio format change";
                return;
            }
            if (audioPtsUs.load() == ddr_bitstream_ring::kNoTimestamp &&
                aframe->best_effort_timestamp != AV_NOPTS_VALUE)
                audioPtsUs.store(av_rescale_q(aframe->best_effort_timestamp,
                                              fmtTimebase(), AVRational{1, 1000000}));
            const int capacity = swr_get_out_samples(resampler, n);
            if (capacity <= 0 || capacity > 48000) {
                audioError = "audio resampler output bound";
                return;
            }
            std::vector<uint8_t> out(static_cast<size_t>(capacity) * 4);
            uint8_t* output = out.data();
            const int samples = swr_convert(resampler, &output, capacity,
                                            const_cast<const uint8_t**>(aframe->extended_data), n);
            if (samples < 0) { audioError = "audio resample failed"; return; }
            pushPcm48(out.data(), static_cast<size_t>(samples) * 4);
            return;
        }
        std::vector<float> stereo(static_cast<size_t>(n) * 2u, 0.f);
        const int fmt = aframe->format;
        auto sampleAt = [&](int c, int i) -> float {
            const int cc = c < ch ? c : 0;
            if (fmt == AV_SAMPLE_FMT_FLTP && aframe->data[cc])
                return reinterpret_cast<const float*>(aframe->data[cc])[i];
            if (fmt == AV_SAMPLE_FMT_FLT && aframe->data[0])
                return reinterpret_cast<const float*>(aframe->data[0])[i * ch + cc];
            if (fmt == AV_SAMPLE_FMT_S16P && aframe->data[cc])
                return static_cast<float>(
                           reinterpret_cast<const int16_t*>(aframe->data[cc])[i]) /
                       32768.f;
            if (fmt == AV_SAMPLE_FMT_S16 && aframe->data[0])
                return static_cast<float>(
                           reinterpret_cast<const int16_t*>(aframe->data[0])[i * ch + cc]) /
                       32768.f;
            return 0.f;
        };
        for (int i = 0; i < n; ++i) {
            stereo[static_cast<size_t>(i) * 2u] = sampleAt(0, i);
            stereo[static_cast<size_t>(i) * 2u + 1u] = ch > 1 ? sampleAt(1, i) : sampleAt(0, i);
        }
        const int n48 = rate == 48000 ? n : static_cast<int>((static_cast<int64_t>(n) * 48000) / rate);
        std::vector<uint8_t> out(static_cast<size_t>(n48) * 4u);
        for (int i = 0; i < n48; ++i) {
            int src = i;
            if (rate != 48000)
                src = static_cast<int>((static_cast<int64_t>(i) * n) / n48);
            if (src >= n)
                src = n - 1;
            const float l = stereo[static_cast<size_t>(src) * 2u];
            const float r = stereo[static_cast<size_t>(src) * 2u + 1u];
            const int16_t ls = s16sat(static_cast<int>(l * 32767.f));
            const int16_t rs = s16sat(static_cast<int>(r * 32767.f));
            out[static_cast<size_t>(i) * 4u] = static_cast<uint8_t>(ls & 0xff);
            out[static_cast<size_t>(i) * 4u + 1u] = static_cast<uint8_t>((ls >> 8) & 0xff);
            out[static_cast<size_t>(i) * 4u + 2u] = static_cast<uint8_t>(rs & 0xff);
            out[static_cast<size_t>(i) * 4u + 3u] = static_cast<uint8_t>((rs >> 8) & 0xff);
        }
        pushPcm48(out.data(), out.size());
    }

    void decodeAudioPacket() {
        if (!acodec || !aframe || !pkt)
            return;
        int ret = avcodec_send_packet(acodec, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return;
        for (;;) {
            ret = avcodec_receive_frame(acodec, aframe);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            ingestAudioFrame();
            av_frame_unref(aframe);
        }
    }

    AVRational fmtTimebase() const { return fmt->streams[aidx]->time_base; }

    int readInput(const std::atomic<int>& abort) {
        int result = av_read_frame(fmt, pkt);
        inputReadCancelled = abort.load() || (options.cancelled && options.cancelled->load());
        if (result >= 0)
            noteInputSpan();
        else if (result == AVERROR(EIO) && inputClosedAtEnd())
            result = AVERROR_EOF;
        inputReadResult = result;
        if (result >= 0 &&
            fmt->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++inputVideoPackets;
            lastInputPts = pkt->pts;
            lastInputDuration = pkt->duration;
            inputTimebase = fmt->streams[pkt->stream_index]->time_base;
        }
        return result;
    }

    void noteInputSpan() {
        const int slot = pkt->stream_index == vidx ? 0 : pkt->stream_index == aidx ? 1 : -1;
        if (slot < 0 || pkt->pts == AV_NOPTS_VALUE)
            return;
        const AVRational tb = fmt->streams[pkt->stream_index]->time_base;
        const int64_t startUs = av_rescale_q(pkt->pts, tb, AV_TIME_BASE_Q);
        const int64_t endUs =
            av_rescale_q(pkt->pts + std::max<int64_t>(pkt->duration, 0), tb, AV_TIME_BASE_Q);
        if (!spanPinned && (spanFirstUs[slot] == AV_NOPTS_VALUE || startUs < spanFirstUs[slot]))
            spanFirstUs[slot] = startUs;
        if (spanEndUs[slot] == AV_NOPTS_VALUE || endUs > spanEndUs[slot])
            spanEndUs[slot] = endUs;
    }

    // The longest per-stream span read, in ms; -1 before a packet with a pts.
    int64_t inputSpanMs() const {
        int64_t spanUs = -1;
        for (int slot = 0; slot < 2; ++slot)
            if (spanFirstUs[slot] != AV_NOPTS_VALUE && spanEndUs[slot] != AV_NOPTS_VALUE)
                spanUs = std::max(spanUs, spanEndUs[slot] - spanFirstUs[slot]);
        return spanUs < 0 ? -1 : spanUs / 1000;
    }

    // N2: whether this EIO is the server's close at the declared end (inputCloseIsEndOfStream).
    // At most one conversion per input; the reader that gets it sets inputEof, so the other
    // reader then sees EOF without reading again (F4).
    bool inputClosedAtEnd() {
        if (inputCloseConversions.load() > 0)
            return true;
        const bool lengthUnknown = fmt && fmt->pb && avio_size(fmt->pb) < 0;
        const bool readerFailed = !compressedError.empty() || !audioError.empty();
        const int64_t spanMs = inputSpanMs();
        if (!inputCloseIsEndOfStream(lengthUnknown, inputReadCancelled, readerFailed,
                                     options.expectedDurationMs, options.sessionStartMs, spanMs))
            return false;
        inputCloseShortMs.store(options.expectedDurationMs - (options.sessionStartMs + spanMs));
        inputCloseConversions.fetch_add(1);
        return true;
    }

    void publishQueueDepth() {
        MPX_AV_TRACE(diagnosticReservedVideo.store(aheadPacket != nullptr, std::memory_order_relaxed);)
        diagnosticPackets.store(queuedPackets.size() + (aheadPacket ? 1 : 0));
        diagnosticBytes.store(queuedBytes + (aheadPacket ? size_t(aheadPacket->size) : 0));
        const auto video = std::find_if(queuedPackets.begin(), queuedPackets.end(),
            [&](const AVPacket* packet) { return packet->stream_index == vidx; });
        diagnosticNextPts.store(video != queuedPackets.end() ? (*video)->pts :
            aheadPacket ? aheadPacket->pts : ddr_bitstream_ring::kNoTimestamp);
    }

    bool decodeCompressedAudio(AVPacket* packet, std::string& err) {
        auto* audio = fmt->streams[aidx];
        if (!acodec) {
            const AVCodec* decoder = avcodec_find_decoder(audio->codecpar->codec_id);
            if (!decoder || decoder->type != AVMEDIA_TYPE_AUDIO ||
                !(acodec = avcodec_alloc_context3(decoder))) {
                err = "audio decoder unavailable";
                return false;
            }
            acodec->pkt_timebase = audio->time_base;
            if (avcodec_parameters_to_context(acodec, audio->codecpar) < 0 ||
                avcodec_open2(acodec, decoder, nullptr) < 0 ||
                !(aframe = av_frame_alloc())) {
                err = "audio decoder unavailable";
                return false;
            }
            arate = acodec->sample_rate;
        }
        const int sent = avcodec_send_packet(acodec, packet);
        if (sent < 0 && sent != AVERROR_EOF) {
            err = "audio packet rejected: " + avErr(sent);
            return false;
        }
        for (;;) {
            const int got = avcodec_receive_frame(acodec, aframe);
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) break;
            if (got < 0) { err = "audio decode failed: " + avErr(got); return false; }
            ingestAudioFrame();
            av_frame_unref(aframe);
            if (!audioError.empty()) { err = audioError; return false; }
        }
        return true;
    }

    bool finishCompressedAudio(std::string& err) {
        if (audioFlushed) return true;
        if (acodec && !decodeCompressedAudio(nullptr, err)) return false;
        if (resampler) {
            uint8_t tail[4096];
            for (;;) {
                uint8_t* output = tail;
                const int samples = swr_convert(resampler, &output, sizeof(tail) / 4, nullptr, 0);
                if (samples < 0) { err = "audio resampler EOF drain failed"; return false; }
                if (!samples) break;
                pushPcm48(tail, size_t(samples) * 4);
                if (!audioError.empty()) { err = audioError; return false; }
            }
        }
        audioFlushed = true;
        std::lock_guard<std::mutex> lock(pcmMu);
        audioEof = true;
        MPX_AV_TRACE(diagnosticAudioEof.store(true, std::memory_order_relaxed);)
        pcmCv.notify_all();
        return true;
    }

    ~Impl() {
        if (resampler) swr_free(&resampler);
        if (aheadPacket) av_packet_free(&aheadPacket);
        for (auto* packet : queuedPackets) av_packet_free(&packet);
        if (bsf) av_bsf_free(&bsf);
        if (aframe)
            av_frame_free(&aframe);
        if (frame)
            av_frame_free(&frame);
        if (pkt)
            av_packet_free(&pkt);
        if (acodec)
            avcodec_free_context(&acodec);
        if (codec)
            avcodec_free_context(&codec);
        if (fmt)
            avformat_close_input(&fmt);
    }
};

int AvInprocDecoder::interruptThunk(void* p) {
    auto* d = static_cast<AvInprocDecoder*>(p);
    return d && (d->abort_.load(std::memory_order_relaxed) ||
                 (d->cancelled_ && d->cancelled_->load()) ||
                 (d->ioDeadlineMs_.load() > 0 && monotonicMs() > d->ioDeadlineMs_.load()));
}

void AvInprocDecoder::requestStop() {
    abort_.store(1, std::memory_order_relaxed);
    if (!impl_)
        return;
    std::lock_guard<std::mutex> lk(impl_->pcmMu);
    impl_->stopping = true;
    impl_->blocked.store(AvDemuxBlocked::Cancelled);
    impl_->pcmCv.notify_all();
}

void AvInprocDecoder::close() {
    requestStop();
    delete impl_;
    impl_ = nullptr;
}

int AvInprocDecoder::width() const { return impl_ ? impl_->w : 0; }
int AvInprocDecoder::height() const { return impl_ ? impl_->h : 0; }
bool AvInprocDecoder::isOpen() const { return impl_ && impl_->open; }
bool AvInprocDecoder::hasAudio() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->compressedMu);
    return impl_->aidx >= 0 && impl_->acodec;
}
bool AvInprocDecoder::audioEof() const {
    if (!impl_)
        return true;
    std::lock_guard<std::mutex> lk(impl_->pcmMu);
    return impl_->audioEof && impl_->pcm.empty();
}

int AvInprocDecoder::drainPcm(uint8_t* dst, size_t n, bool wait) {
    if (!impl_ || !dst || n == 0)
        return 0;
    std::unique_lock<std::mutex> lk(impl_->pcmMu);
    if (wait && impl_->pcm.empty() && !impl_->audioEof && !impl_->stopping && impl_->open)
        impl_->pcmCv.wait_for(lk, std::chrono::milliseconds(20),
                              [&] { return !impl_->pcm.empty() || impl_->audioEof ||
                                            impl_->stopping || !impl_->open; });
    const size_t take = std::min(n, impl_->pcm.size());
    if (take == 0)
        return 0;
    std::memcpy(dst, impl_->pcm.data(), take);
    impl_->pcm.erase(impl_->pcm.begin(),
                     impl_->pcm.begin() + static_cast<std::ptrdiff_t>(take));
    MPX_AV_TRACE(impl_->diagnosticPcmBytes.store(impl_->pcm.size(), std::memory_order_relaxed);)
    impl_->pcmCv.notify_all();
    return static_cast<int>(take);
}

const char* AvInprocDecoder::libavIdent() { return av_version_info(); }

SourceAspect AvInprocDecoder::sourceAspect() const {
    return impl_ ? impl_->aspect : SourceAspect{};
}

AvCompressedVideoGeometry AvInprocDecoder::videoGeometry() const {
    if (!impl_ || !impl_->compressed)
        return {};
    auto geometry = impl_->syntax.geometry;
    geometry.sourceAspect = impl_->aspect;
    return geometry;
}

int64_t AvInprocDecoder::firstAudioPtsUs() const {
    return impl_ ? impl_->audioPtsUs.load() : ddr_bitstream_ring::kNoTimestamp;
}

bool AvInprocDecoder::openCompressed(const std::string& url,
                                     const AvInprocOpenOpts& opts, std::string& err) {
    err.clear();
    abort_.store(0);
    av_log_set_level(AV_LOG_QUIET); // URL-bearing libav diagnostics must not expose Plex tokens.
    impl_ = new Impl();
    impl_->compressed = true;
    impl_->options = opts;
    if (url.empty() || opts.expectW <= 0 || opts.expectH <= 0 ||
        opts.maxAccessUnitBytes == 0 ||
        opts.maxAccessUnitBytes > ddr_bitstream_ring::kMaxAccessUnitBytes ||
        opts.maxVclRbspBytes == 0) {
        err = "invalid compressed session options";
        return false;
    }
    if ((opts.expectedFpsNum != 0 || opts.expectedFpsDen != 0) &&
        !misterplex::expected_fps_supported(opts.expectedFpsNum, opts.expectedFpsDen)) {
        err = "unsupported expected source frame rate";
        return false;
    }
    impl_->fmt = avformat_alloc_context();
    if (!impl_->fmt) { err = "format allocation failed"; return false; }
    impl_->fmt->interrupt_callback = {interruptThunk, this};
    // Stream probing may open a video decoder; it is not used in this backend.
    // Demuxers and their H.264 packet parser discover stream parameters instead.
    AVDictionary* options = nullptr;
    if (!opts.headers.empty())
        av_dict_set(&options, "headers", opts.headers.c_str(), 0);
    // Remote shared PMS can be slow to emit the first transcode byte.
    av_dict_set(&options, "rw_timeout", "20000000", 0);
    av_dict_set(&options, "probesize", "262144", 0);
    av_dict_set(&options, "protocol_whitelist", misterplex::kAvProtocolWhitelist, 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    {
        const std::string ca = mpx::caBundlePath();
        if (!ca.empty())
            av_dict_set(&options, "ca_file", ca.c_str(), 0);
    }
    ioDeadlineMs_.store(monotonicMs() + 20000);
    const int opened = avformat_open_input(&impl_->fmt, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (opened < 0) { err = "compressed input open: " + avErr(opened); return false; }
    impl_->pkt = av_packet_alloc();
    if (!impl_->pkt) { err = "packet allocation failed"; return false; }
    auto findStreams = [&] {
        for (unsigned i = 0; i < impl_->fmt->nb_streams; ++i) {
            auto* par = impl_->fmt->streams[i]->codecpar;
            if (impl_->vidx < 0 && par->codec_type == AVMEDIA_TYPE_VIDEO)
                impl_->vidx = static_cast<int>(i);
            if (impl_->aidx < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO)
                impl_->aidx = static_cast<int>(i);
        }
    };
    findStreams();
    size_t probeBytes = 0;
    while (impl_->vidx < 0) {
        const int read = impl_->readInput(abort_);
        if (read < 0) { err = "no compressed video stream: " + avErr(read); return false; }
        probeBytes += static_cast<size_t>(impl_->pkt->size);
        if (probeBytes > kCompressedPacketByteLimit ||
            impl_->queuedPackets.size() >= kCompressedPacketCountLimit) {
            err = "compressed stream discovery limit";
            return false;
        }
        AVPacket* saved = av_packet_clone(impl_->pkt);
        if (!saved) { err = "packet clone failed"; return false; }
        impl_->queuedPackets.push_back(saved);
        impl_->queuedBytes += size_t(saved->size);
        impl_->publishQueueDepth();
        av_packet_unref(impl_->pkt);
        findStreams();
    }
    auto* video = impl_->fmt->streams[impl_->vidx];
    if (video->codecpar->codec_id != AV_CODEC_ID_H264 ||
        video->time_base.num <= 0 || video->time_base.den <= 0) {
        err = "FPGA backend requires H.264 with rational container timestamps";
        return false;
    }
    const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
    int ret = filter ? av_bsf_alloc(filter, &impl_->bsf) : AVERROR(ENOSYS);
    if (ret >= 0)
        ret = avcodec_parameters_copy(impl_->bsf->par_in, video->codecpar);
    if (ret >= 0) {
        impl_->bsf->time_base_in = video->time_base;
        ret = av_bsf_init(impl_->bsf);
    }
    if (ret < 0) { err = "Annex-B bitstream filter: " + avErr(ret); return false; }
    if (opts.startMs > 0) {
        int64_t origin = video->start_time;
        size_t discovered = 0;
        while (origin == AV_NOPTS_VALUE) {
            ret = impl_->readInput(abort_);
            if (ret < 0) { err = "seek origin unavailable: " + avErr(ret); return false; }
            discovered += static_cast<size_t>(impl_->pkt->size);
            if (impl_->pkt->stream_index == impl_->vidx)
                origin = impl_->pkt->pts;
            av_packet_unref(impl_->pkt);
            if (discovered > 2 * 1024 * 1024) {
                err = "bounded seek origin discovery exceeded";
                return false;
            }
        }
        const int64_t offset = av_rescale_q(opts.startMs, AVRational{1, 1000}, video->time_base);
        if (offset < 0 || origin > std::numeric_limits<int64_t>::max() - offset) {
            err = "compressed seek timestamp overflow";
            return false;
        }
        const int64_t ts = origin + offset;
        ret = av_seek_frame(impl_->fmt, impl_->vidx, ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) { err = "compressed seek: " + avErr(ret); return false; }
        impl_->seekTargetPts = ts;
        // N2 (F2; 90e335b5 review N-b): session time restarts at the seek target, so the origin
        // discovery above and the backward seek's preroll before the target never count.
        const int64_t seekTargetUs = av_rescale_q(ts, video->time_base, AV_TIME_BASE_Q);
        for (int slot = 0; slot < 2; ++slot) {
            impl_->spanFirstUs[slot] = seekTargetUs;
            impl_->spanEndUs[slot] = AV_NOPTS_VALUE;
        }
        impl_->spanPinned = true;
        for (auto* packet : impl_->queuedPackets) av_packet_free(&packet);
        impl_->queuedPackets.clear();
        impl_->queuedBytes = 0;
        impl_->publishQueueDepth();
    }
    impl_->w = 0;
    impl_->h = 0;
    impl_->open = true;
    ioDeadlineMs_.store(0);
    return true;
}

#if MPX_FPGA_AV_TRACE
AvCompressedPressure AvInprocDecoder::compressedPressure() const noexcept {
    AvCompressedPressure result;
    if (!impl_) return result;
    result.available = true;
    result.queuedPackets = impl_->diagnosticPackets.load(std::memory_order_relaxed);
    result.queuedBytes = impl_->diagnosticBytes.load(std::memory_order_relaxed);
    result.pcmBytes = impl_->diagnosticPcmBytes.load(std::memory_order_relaxed);
    result.inputEof = impl_->inputEof.load(std::memory_order_relaxed);
    result.audioEof = impl_->diagnosticAudioEof.load(std::memory_order_relaxed);
    result.reservedVideo = impl_->diagnosticReservedVideo.load(std::memory_order_relaxed);
    result.blocked = impl_->blocked.load(std::memory_order_relaxed);
    return result;
}
#endif

AvCompressedDiagnostics AvInprocDecoder::compressedDiagnostics() const {
    AvCompressedDiagnostics result;
    if (!impl_) return result;
    result.available = true;
    result.sourceOpen = impl_->open;
    result.queuedPackets = impl_->diagnosticPackets.load();
    result.queuedBytes = impl_->diagnosticBytes.load();
    result.nextQueuedPts = impl_->diagnosticNextPts.load();
    result.inputEof = impl_->inputEof.load();
    result.blocked = impl_->blocked.load();
    result.cancelled = abort_.load() || (cancelled_ && cancelled_->load());
    result.inputCloseShortMs = impl_->inputCloseShortMs.load();
    // Never wait behind a demux producer blocked on the PCM consumer.
    std::unique_lock<std::mutex> demux(impl_->compressedMu, std::try_to_lock);
    result.detailsAvailable = demux.owns_lock();
    if (result.detailsAvailable) {
        result.inputReadResult = impl_->inputReadResult;
        result.inputReadCancelled = impl_->inputReadCancelled;
        if (impl_->fmt) {
            if (impl_->fmt->duration != AV_NOPTS_VALUE)
                result.containerDurationUs = impl_->fmt->duration;
            if (impl_->fmt->pb) {
                result.ioError = impl_->fmt->pb->error;
                result.ioEof = impl_->fmt->pb->eof_reached != 0;
                result.ioBytesRead = impl_->fmt->pb->bytes_read;
            }
        }
        result.inputVideoPackets = impl_->inputVideoPackets;
        result.returnedAccessUnits = impl_->accessUnits;
        {
            const int64_t spanMs = impl_->inputSpanMs();
            result.inputSessionMs =
                spanMs < 0 ? -1 : std::max<int64_t>(0, impl_->options.sessionStartMs) + spanMs;
        }
        result.lastInputPts = impl_->lastInputPts;
        result.lastInputDuration = impl_->lastInputDuration;
        result.inputTimebaseNum = impl_->inputTimebase.num;
        result.inputTimebaseDen = impl_->inputTimebase.den;
        result.lastAuPts = impl_->previousPts;
        result.firstAuPts = impl_->firstPts;
        result.lastAuDuration = impl_->lastAuDuration;
        if (impl_->bsf) {
            result.auTimebaseNum = impl_->bsf->time_base_out.num;
            result.auTimebaseDen = impl_->bsf->time_base_out.den;
        }
        result.reservedVideo = impl_->aheadPacket != nullptr;
        for (const auto* packet : impl_->queuedPackets)
            if (packet->stream_index == impl_->vidx) ++result.queuedVideoPackets;
        result.error = impl_->compressedError;
    }
    std::lock_guard<std::mutex> lock(impl_->pcmMu);
    result.pcmBytes = impl_->pcm.size();
    result.audioEof = impl_->audioEof;
    return result;
}

int64_t AvInprocDecoder::inputClosedAtEndShortMs() const {
    return impl_ ? impl_->inputCloseShortMs.load() : kNoInputCloseAtEnd;
}

unsigned AvInprocDecoder::inputCloseConversions() const {
    return impl_ ? impl_->inputCloseConversions.load() : 0;
}

std::string formatCompressedDiagnostics(const AvCompressedDiagnostics& state) {
    if (!state.available) return "demux=unavailable";
    std::ostringstream out;
    out << "source_open=" << state.sourceOpen << " input_eof=" << state.inputEof
        << " audio_eof=" << state.audioEof << " source_cancelled=" << state.cancelled
        << " queued_packets=" << state.queuedPackets << " queued_bytes=" << state.queuedBytes
        << " pcm_bytes=" << state.pcmBytes << " demux_blocked=" << unsigned(state.blocked);
    auto field = [&](const char* name, auto value, bool known) {
        out << ' ' << name << '=';
        if (known) out << value;
        else out << "unavailable";
    };
    const bool known = state.detailsAvailable;
    field("input_read_code", state.inputReadResult.value_or(0),
          known && state.inputReadResult.has_value());
    field("input_read_cancelled", state.inputReadCancelled,
          known && state.inputReadResult.has_value());
    field("io_error", state.ioError.value_or(0), known && state.ioError.has_value());
    field("io_eof", state.ioEof.value_or(false), known && state.ioEof.has_value());
    field("io_bytes_read", state.ioBytesRead.value_or(0), known && state.ioBytesRead.has_value());
    field("container_duration_us", state.containerDurationUs.value_or(0),
          known && state.containerDurationUs.has_value());
    out << " input_read_kind=" << (!known || !state.inputReadResult ? "unavailable" :
        *state.inputReadResult >= 0 ? "packet" :
        *state.inputReadResult == AVERROR_EOF ? "eof" :
        state.inputReadCancelled ? "cancelled" : "error");
    out << " input_closed_at_end=" << (state.inputCloseShortMs != kNoInputCloseAtEnd);
    field("input_close_short_ms", state.inputCloseShortMs,
          state.inputCloseShortMs != kNoInputCloseAtEnd);
    field("input_session_ms", state.inputSessionMs, known && state.inputSessionMs >= 0);
    field("input_video_packets", state.inputVideoPackets, known);
    field("returned_aus", state.returnedAccessUnits, known);
    field("queued_next_video_pts", state.nextQueuedPts, state.nextQueuedPts != AV_NOPTS_VALUE);
    field("queued_video", state.queuedVideoPackets, known);
    field("reserved_video", state.reservedVideo, known);
    field("input_last_pts", state.lastInputPts,
          known && state.lastInputPts != AV_NOPTS_VALUE);
    field("input_last_duration", state.lastInputDuration, known && state.lastInputDuration > 0);
    field("input_tb_num", state.inputTimebaseNum, known && state.inputTimebaseNum > 0);
    field("input_tb_den", state.inputTimebaseDen, known && state.inputTimebaseDen > 0);
    field("au_last_pts", state.lastAuPts, known && state.lastAuPts != AV_NOPTS_VALUE);
    field("au_first_pts", state.firstAuPts, known && state.firstAuPts != AV_NOPTS_VALUE);
    field("au_last_duration", state.lastAuDuration, known && state.lastAuDuration > 0);
    field("au_tb_num", state.auTimebaseNum, known && state.auTimebaseNum > 0);
    field("au_tb_den", state.auTimebaseDen, known && state.auTimebaseDen > 0);
    field("demux_error_present", !state.error.empty(), known);
    out << " demux_error=" << (known ? fpgaTerminalText(redactSensitive(state.error)) :
                                       "unavailable");
    return out.str();
}

AvAudioProgress AvInprocDecoder::advanceCompressedAudio(std::string& err) {
    err.clear();
    // 182 L4 on 7b8d71a8: the host rewrite had the first AU, video had not
    // started, and this Error closed the session (had_presentation=0).
    // Not-open yet is Pending. A real demux failure still returns Error below.
    if (!impl_ || !impl_->open || !impl_->compressed || !impl_->options.decodeAudio)
        return AvAudioProgress::Pending;
    if (abort_.load() || (cancelled_ && cancelled_->load()))
        return AvAudioProgress::Cancelled;
    if (impl_->options.paused && impl_->options.paused->load())
        return AvAudioProgress::Paused;
    // A video reader can be blocked supplying PCM. Never wait for its demux
    // lock here: release this call so the existing PCM consumer can drain it.
    std::unique_lock<std::mutex> lock(impl_->compressedMu, std::try_to_lock);
    if (!lock.owns_lock()) return AvAudioProgress::Pending;
    auto fail = [&](const std::string& message) {
        err = message;
        impl_->compressedError = message;
        impl_->blocked.store(AvDemuxBlocked::Error);
        ioDeadlineMs_.store(0);
        return AvAudioProgress::Error;
    };
    if (!impl_->compressedError.empty()) return fail(impl_->compressedError);
    auto pcmState = [&] {
        std::lock_guard<std::mutex> pcm(impl_->pcmMu);
        return !impl_->pcm.empty() ? AvAudioProgress::Ready :
            impl_->audioEof ? AvAudioProgress::Eof : AvAudioProgress::Pending;
    };
    if (pcmState() != AvAudioProgress::Pending) return pcmState();
    auto full = [&] {
        impl_->blocked.store(AvDemuxBlocked::VideoQueue);
        return AvAudioProgress::Backpressure;
    };
    const size_t regularByteLimit =
        kCompressedPacketByteLimit - impl_->options.maxAccessUnitBytes;
    if (impl_->aheadPacket) {
        if (impl_->queuedPackets.size() >= kCompressedPacketCountLimit - 1 ||
            impl_->queuedBytes + size_t(impl_->aheadPacket->size) > regularByteLimit)
            return full();
        impl_->queuedPackets.push_back(impl_->aheadPacket);
        impl_->queuedBytes += size_t(impl_->aheadPacket->size);
        impl_->aheadPacket = nullptr;
        impl_->publishQueueDepth();
        impl_->blocked.store(AvDemuxBlocked::Idle);
        return AvAudioProgress::Pending;
    }
    // Reserve one packet so audio immediately after a full regular video queue
    // can still be decoded. A video packet in that slot is retained, never dropped.
    auto savedAudio = std::find_if(impl_->queuedPackets.begin(), impl_->queuedPackets.end(),
        [&](const AVPacket* packet) {
            return impl_->fmt->streams[packet->stream_index]->codecpar->codec_type ==
                   AVMEDIA_TYPE_AUDIO;
        });
    int read = 0;
    if (savedAudio != impl_->queuedPackets.end()) {
        AVPacket* packet = *savedAudio;
        impl_->queuedBytes -= size_t(packet->size);
        impl_->queuedPackets.erase(savedAudio);
        av_packet_move_ref(impl_->pkt, packet);
        av_packet_free(&packet);
        impl_->publishQueueDepth();
    } else if (impl_->inputEof.load()) {
        read = AVERROR_EOF;
    } else {
        if (impl_->queuedPackets.size() >= kCompressedPacketCountLimit ||
            impl_->queuedBytes > regularByteLimit)
            return full();
        impl_->blocked.store(AvDemuxBlocked::Input);
        ioDeadlineMs_.store(monotonicMs() + readStallMs_);
        read = impl_->readInput(abort_);
        ioDeadlineMs_.store(0);
    }
    if (abort_.load() || (cancelled_ && cancelled_->load()))
        return AvAudioProgress::Cancelled;
    if (read == AVERROR_EOF) {
        impl_->inputEof.store(true);
        if (!impl_->finishCompressedAudio(err)) return fail(err);
        impl_->blocked.store(AvDemuxBlocked::Eof);
        return pcmState();
    }
    if (read < 0) return fail("compressed audio demux: " + avErr(read));
    if (impl_->pkt->stream_index == impl_->vidx) {
        if (impl_->pkt->size <= 0 ||
            size_t(impl_->pkt->size) > impl_->options.maxAccessUnitBytes) {
            // Skip oversized non-IDR AUs (rare); hard-fail IDR/oversize that
            // the FPGA 8KiB RBSP cannot accept — seek path clamps bitrate.
            // (Annex-B strip of SEI/AUD happens later in readAccessUnit after BSF.)
            const size_t got = impl_->pkt->size <= 0 ? 0 : size_t(impl_->pkt->size);
            if (got > 0 && got <= impl_->options.maxAccessUnitBytes * 4 &&
                !annexbHasIdrNal(impl_->pkt->data, got) &&
                impl_->oversizeAuSkips < 64) {
                ++impl_->oversizeAuSkips;
                av_packet_unref(impl_->pkt);
                impl_->blocked.store(AvDemuxBlocked::Idle);
                return pcmState();
            }
            // Keep mildly-oversize packets for BSF/strip in readAccessUnit.
            if (got > impl_->options.maxAccessUnitBytes &&
                got <= impl_->options.maxAccessUnitBytes + 4096) {
                // fall through to queue
            } else {
                return fail(auCapacityError(got, impl_->options.maxAccessUnitBytes));
            }
        }
        AVPacket* packet = av_packet_clone(impl_->pkt);
        if (!packet) return fail("compressed read-ahead packet allocation failed");
        if (impl_->queuedPackets.size() < kCompressedPacketCountLimit - 1 &&
            impl_->queuedBytes + size_t(packet->size) <= regularByteLimit) {
            try {
                impl_->queuedPackets.push_back(packet);
            } catch (...) {
                av_packet_free(&packet);
                throw;
            }
            impl_->queuedBytes += size_t(packet->size);
        } else {
            impl_->aheadPacket = packet;
        }
        impl_->publishQueueDepth();
    } else {
        auto* stream = impl_->fmt->streams[impl_->pkt->stream_index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            (impl_->aidx < 0 || impl_->aidx == impl_->pkt->stream_index)) {
            impl_->aidx = impl_->pkt->stream_index;
            if (!impl_->decodeCompressedAudio(impl_->pkt, err)) return fail(err);
        }
    }
    av_packet_unref(impl_->pkt);
    impl_->blocked.store(AvDemuxBlocked::Idle);
    return pcmState();
}

int AvInprocDecoder::readAccessUnit(AvCompressedAccessUnit& au, std::string& err) {
    err.clear();
    au = {};
    gateRefusal_ = {};
    if (!impl_ || !impl_->open || !impl_->compressed || !impl_->bsf) {
        err = "compressed session is not open";
        return -1;
    }
    std::lock_guard<std::mutex> compressed(impl_->compressedMu);
    auto fail = [&](const std::string& message) {
        err = message;
        impl_->compressedError = message;
        impl_->blocked.store(AvDemuxBlocked::Error);
        ioDeadlineMs_.store(0);
        return -1;
    };
    if (!impl_->compressedError.empty()) return fail(impl_->compressedError);
    for (;;) {
        if (interruptThunk(this)) return fail("compressed session cancelled or timed out");
        int ret = av_bsf_receive_packet(impl_->bsf, impl_->pkt);
        if (ret >= 0) {
            const uint8_t* auData = impl_->pkt->data;
            size_t auSize = impl_->pkt->size <= 0 ? 0 : size_t(impl_->pkt->size);
            std::vector<uint8_t> stripped;
            if (auSize == 0 || auSize > impl_->options.maxAccessUnitBytes) {
                const size_t got = auSize;
                // Prefer stripping SEI/AUD/filler over failing — seek IDRs often
                // only barely exceed 8KiB because of SEI padding.
                if (got > impl_->options.maxAccessUnitBytes &&
                    annexbStripPaddingNals(stripped, impl_->pkt->data, got) &&
                    stripped.size() <= impl_->options.maxAccessUnitBytes &&
                    stripped.size() > 0) {
                    auData = stripped.data();
                    auSize = stripped.size();
                } else if (got > 0 && impl_->oversizeAuSkips < 128) {
                    // Skip oversize P and mid-GOP IDRs. First-picture IDR still
                    // fails below when seekChecked is false and packet is IDR.
                    const bool idr = annexbHasIdrNal(impl_->pkt->data, got);
                    if (!idr || impl_->seekChecked) {
                        ++impl_->oversizeAuSkips;
                        av_packet_unref(impl_->pkt);
                        continue;
                    }
                    return fail(auCapacityError(got, impl_->options.maxAccessUnitBytes));
                } else {
                    return fail(auCapacityError(got, impl_->options.maxAccessUnitBytes));
                }
            }
            if (impl_->pkt->pts == AV_NOPTS_VALUE)
                return fail("access unit has no original PTS (no fabricated frame-rate clock)");
            if (!impl_->seekChecked && impl_->seekTargetPts != AV_NOPTS_VALUE &&
                impl_->pkt->pts < impl_->seekTargetPts)
                return fail("non-IDR direct seek requires FPGA preroll; use the PMS offset transcode");
            impl_->seekChecked = true;
            bool key = false, changed = false;
            if (!impl_->syntax.inspect(auData, auSize, impl_->options, key, changed, err)) {
                // SPS-gate L3: keep the typed verdict and the refused AU's own clock for the decision owner.
                gateRefusal_.valid = true;
                gateRefusal_.verdict = impl_->syntax.verdict;
                gateRefusal_.err = err;
                gateRefusal_.pts = impl_->pkt->pts;
                if (impl_->bsf->time_base_out.num > 0 && impl_->bsf->time_base_out.den > 0) {
                    gateRefusal_.timebaseNum = static_cast<uint32_t>(impl_->bsf->time_base_out.num);
                    gateRefusal_.timebaseDen = static_cast<uint32_t>(impl_->bsf->time_base_out.den);
                }
                return fail(err);
            }
            impl_->w = impl_->syntax.geometry.codedWidth;
            impl_->h = impl_->syntax.geometry.codedHeight;
            au.annexb.assign(auData, auData + auSize);
            au.pts = impl_->pkt->pts;
            au.duration = std::max<int64_t>(0, impl_->pkt->duration);
            au.timebaseNum = impl_->bsf->time_base_out.num;
            au.timebaseDen = impl_->bsf->time_base_out.den;
            if (impl_->bsf->time_base_out.num <= 0 || impl_->bsf->time_base_out.den <= 0)
                return fail("invalid original stream timebase");
            const long double ptsUs = static_cast<long double>(au.pts) *
                                      au.timebaseNum * 1000000 / au.timebaseDen;
            if (std::fabs(ptsUs) > std::numeric_limits<int64_t>::max() / 4)
                return fail("original timestamp is outside the bounded session clock");
            au.keyframe = key;
            au.parameterSetsChanged = changed;
            // af1c1a94 L4: the join let the demux retry return, then this check
            // closed the session (had_presentation=0). The AU is a High source
            // the player is about to rewrite; its PTS is not the baseline cadence.
            if (impl_->previousPts != ddr_bitstream_ring::kNoTimestamp &&
                !impl_->syntax.hostRewriteArmed) {
                const long double delta = static_cast<long double>(au.pts) - impl_->previousPts;
                const long double ticks = delta * au.timebaseNum;
                const bool oneFrame = misterplex::pts_delta_is_one_frame(
                    ticks, au.timebaseNum, au.timebaseDen);
                // After dropping oversize AUs, PTS may jump by N film frames.
                // Accept exact 1-frame cadence, or integer N-frame jumps (N<=48)
                // when at least one oversize skip has occurred this session.
                bool multiFrameOk = false;
                if (impl_->oversizeAuSkips > 0 && delta > 0)
                    multiFrameOk = misterplex::pts_delta_is_n_frames(
                        ticks, au.timebaseNum, au.timebaseDen, 48);
                if (delta <= 0 || (!oneFrame && !multiFrameOk))
                    return fail("unsupported PTS cadence/discontinuity: requires actual 24, 24000/1001, 30, or 30000/1001");
            }
            if (impl_->firstPts == ddr_bitstream_ring::kNoTimestamp)
                impl_->firstPts = au.pts;
            // When oversize AUs were skipped, wall-clock PTS no longer tracks
            // delivered picture count 1:1 — skip the strict rate identity check.
            if (impl_->options.expectedFpsNum > 0 && impl_->accessUnits > 0 &&
                impl_->oversizeAuSkips == 0 && !impl_->syntax.hostRewriteArmed) {
                if (!misterplex::pts_long_run_rate_ok(
                        static_cast<long double>(au.pts) - impl_->firstPts, impl_->accessUnits,
                        au.timebaseNum, au.timebaseDen, impl_->options.expectedFpsNum,
                        impl_->options.expectedFpsDen))
                    return fail("delivered PTS do not match the requested source frame rate");
            }
            if (impl_->accessUnits == std::numeric_limits<uint64_t>::max())
                return fail("access-unit clock counter exhausted");
            ++impl_->accessUnits;
            impl_->previousPts = au.pts;
            impl_->lastAuDuration = au.duration;
            auto* stream = impl_->fmt->streams[impl_->vidx];
            AVRational sar = stream->sample_aspect_ratio;
            if (sar.num <= 0 || sar.den <= 0) sar = stream->codecpar->sample_aspect_ratio;
            if (impl_->syntax.aspect.valid) {
                impl_->aspect = impl_->syntax.aspect;
            } else if (sar.num > 0 && sar.den > 0) {
                int num = 0, den = 0;
                av_reduce(&num, &den, int64_t(impl_->syntax.width) * sar.num,
                          int64_t(impl_->syntax.height) * sar.den, kSourceAspectMax);
                impl_->aspect = {static_cast<uint16_t>(num), static_cast<uint16_t>(den),
                                  num > 0 && den > 0};
            } else {
                impl_->aspect = {};
            }
            au.geometry = videoGeometry();
            av_packet_unref(impl_->pkt);
            ioDeadlineMs_.store(0);
            impl_->blocked.store(AvDemuxBlocked::Idle);
            return 1;
        }
        if (ret == AVERROR_EOF) {
            if (!impl_->finishCompressedAudio(err)) return fail(err);
            ioDeadlineMs_.store(0);
            impl_->blocked.store(AvDemuxBlocked::Eof);
            return 0;
        }
        if (ret != AVERROR(EAGAIN)) return fail("Annex-B receive: " + avErr(ret));
        if (!impl_->queuedPackets.empty()) {
            AVPacket* packet = impl_->queuedPackets.front();
            impl_->queuedPackets.pop_front();
            impl_->queuedBytes -= size_t(packet->size);
            av_packet_move_ref(impl_->pkt, packet);
            av_packet_free(&packet);
            impl_->publishQueueDepth();
            ret = 0;
        } else if (impl_->aheadPacket) {
            av_packet_move_ref(impl_->pkt, impl_->aheadPacket);
            av_packet_free(&impl_->aheadPacket);
            impl_->publishQueueDepth();
            ret = 0;
        } else if (impl_->inputEof.load()) {
            ret = AVERROR_EOF;
        } else {
            impl_->blocked.store(AvDemuxBlocked::Input);
            ioDeadlineMs_.store(monotonicMs() + readStallMs_);
            ret = impl_->readInput(abort_);
            ioDeadlineMs_.store(0);
        }
        if (ret == AVERROR_EOF) {
            impl_->inputEof.store(true);
            if (impl_->bsfFlushed) return fail("bitstream filter stalled after EOF");
            impl_->bsfFlushed = true;
            ret = av_bsf_send_packet(impl_->bsf, nullptr);
            if (ret < 0) return fail("Annex-B EOF drain: " + avErr(ret));
            continue;
        }
        if (ret < 0) return fail("compressed demux: " + avErr(ret));
        if (impl_->pkt->stream_index == impl_->vidx) {
            ret = av_bsf_send_packet(impl_->bsf, impl_->pkt);
            if (ret < 0) return fail("Annex-B packet rejected: " + avErr(ret));
        } else {
            auto* stream = impl_->fmt->streams[impl_->pkt->stream_index];
            if (impl_->options.decodeAudio &&
                stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                (impl_->aidx < 0 || impl_->aidx == impl_->pkt->stream_index)) {
                impl_->aidx = impl_->pkt->stream_index;
                if (!impl_->decodeCompressedAudio(impl_->pkt, err)) return fail(err);
            }
            av_packet_unref(impl_->pkt);
        }
    }
}

bool AvInprocDecoder::open(const std::string& pathOrUrl, const AvInprocOpenOpts& o,
                           std::string& err) {
    close();
    gateRefusal_ = {};
    cancelled_ = o.cancelled;
    readStallMs_ = o.readStallMs > 0 ? o.readStallMs : kLanReadStallMs;
    ioDeadlineMs_.store(0);
    if (o.compressedVideo)
        return openCompressed(pathOrUrl, o, err);
    err.clear();
    if (pathOrUrl.empty()) {
        err = "empty path";
        return false;
    }
    // Static ARM binary cannot call getaddrinfo (glibc NSS abort). Remote
    // HTTP is remuxed to a fifo. Numeric loopback does not need NSS.
    const bool loopbackHttp =
        pathOrUrl.compare(0, 16, "http://127.0.0.1") == 0 ||
        pathOrUrl.compare(0, 16, "HTTP://127.0.0.1") == 0;
    if (looksNetworkUrl(pathOrUrl) && !loopbackHttp) {
        err = "network protocols disabled prefix=" + pathOrUrl.substr(0, 24);
        return false;
    }
    if (o.expectW <= 0 || o.expectH <= 0 || (o.expectW & 1) || (o.expectH & 1)) {
        err = "expect WxH must be positive even";
        return false;
    }
    const int outW = o.outW > 0 ? o.outW : o.expectW;
    const int outH = o.outH > 0 ? o.outH : o.expectH;
    if (outW <= 0 || outH <= 0 || (outW & 1) || (outH & 1)) {
        err = "out WxH must be positive even";
        return false;
    }

    abort_.store(0, std::memory_order_relaxed);
    impl_ = new Impl();
    av_log_set_level(AV_LOG_ERROR);

    AVDictionary* opts = nullptr;
    if (!o.headers.empty()) {
        std::string h = o.headers;
        if (h.size() < 2 || h.back() != '\n')
            h += "\r\n";
        av_dict_set(&opts, "headers", h.c_str(), 0);
    }
    const AVInputFormat* ifmt = nullptr;
    if (o.liveFifo) {
        ifmt = av_find_input_format("h264");
        // 2 MiB probe on a slow live HEVC remux filled the 1 MiB PCM fifo
        // (nobody pumping yet) and deadlocked annex-B. 128 KiB is one IDR.
        av_dict_set(&opts, "probesize", "131072", 0);
        av_dict_set(&opts, "analyzeduration", "0", 0);
    } else {
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "probesize", "32768", 0);
        av_dict_set(&opts, "analyzeduration", "0", 0);
    }
    av_dict_set(&opts, "protocol_whitelist", misterplex::kAvProtocolWhitelist, 0);
    av_dict_set(&opts, "tls_verify", "1", 0);
    {
        const std::string ca = mpx::caBundlePath();
        if (!ca.empty())
            av_dict_set(&opts, "ca_file", ca.c_str(), 0);
    }
    impl_->fmt = avformat_alloc_context();
    if (!impl_->fmt) {
        av_dict_free(&opts);
        err = "avformat_alloc_context failed";
        close();
        return false;
    }
    impl_->fmt->interrupt_callback.callback = interruptThunk;
    impl_->fmt->interrupt_callback.opaque = this;
    int ret = avformat_open_input(&impl_->fmt, pathOrUrl.c_str(), ifmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        err = "avformat_open_input: " + avErr(ret);
        close();
        return false;
    }
    ret = avformat_find_stream_info(impl_->fmt, nullptr);
    if (ret < 0) {
        err = "avformat_find_stream_info: " + avErr(ret);
        close();
        return false;
    }

    impl_->vidx = -1;
    impl_->aidx = -1;
    for (unsigned i = 0; i < impl_->fmt->nb_streams; ++i) {
        const AVCodecParameters* par = impl_->fmt->streams[i]->codecpar;
        if (!par)
            continue;
        if (impl_->vidx < 0 && par->codec_type == AVMEDIA_TYPE_VIDEO)
            impl_->vidx = static_cast<int>(i);
        if (impl_->aidx < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO)
            impl_->aidx = static_cast<int>(i);
    }
    if (impl_->vidx < 0) {
        err = "no video stream";
        close();
        return false;
    }

    AVStream* st = impl_->fmt->streams[impl_->vidx];
    const AVCodecParameters* par = st->codecpar;
    const int sw = par->width;
    const int sh = par->height;
    // Bank is out WxH. Source may be 240/480 while the live L4 bank is 1280×720.
    if (sw != 0 || sh != 0) {
        if (sw <= 0 || sh <= 0 || (sw & 1) || (sh & 1)) {
            err = "source WxH must be positive even";
            close();
            return false;
        }
    }
    if (par->format != AV_PIX_FMT_NONE && par->format != AV_PIX_FMT_YUV420P) {
        err = "pixfmt not YUV420P";
        close();
        return false;
    }

    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        err = "no decoder for codec_id";
        close();
        return false;
    }
    impl_->codec = avcodec_alloc_context3(dec);
    if (!impl_->codec) {
        err = "avcodec_alloc_context3 failed";
        close();
        return false;
    }
    ret = avcodec_parameters_to_context(impl_->codec, par);
    if (ret < 0) {
        err = "avcodec_parameters_to_context: " + avErr(ret);
        close();
        return false;
    }
    impl_->codec->thread_count = o.threads > 0 ? o.threads : 2;
#ifdef FF_THREAD_FRAME
    impl_->codec->thread_type = FF_THREAD_FRAME;
#endif
    // Match ffmpeg -skip_loop_filter all on the identity 720p pipe (2–4 ms/f
    // deblock was the 21.8 unique lock). Inproc had no equivalent.
#ifdef AVDISCARD_ALL
    if (inprocSkipLoopFilter720p(outW, outH))
        impl_->codec->skip_loop_filter = AVDISCARD_ALL;
#endif
    ret = avcodec_open2(impl_->codec, dec, nullptr);
    if (ret < 0) {
        err = "avcodec_open2: " + avErr(ret);
        close();
        return false;
    }
    if (impl_->codec->pix_fmt != AV_PIX_FMT_NONE &&
        impl_->codec->pix_fmt != AV_PIX_FMT_YUV420P) {
        err = "decoder pixfmt not YUV420P";
        close();
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) {
        err = "av_frame/av_packet alloc failed";
        close();
        return false;
    }
    if (impl_->aidx >= 0) {
        AVStream* ast = impl_->fmt->streams[impl_->aidx];
        const AVCodecParameters* apar = ast->codecpar;
        const AVCodec* adec = avcodec_find_decoder(apar->codec_id);
        if (adec) {
            impl_->acodec = avcodec_alloc_context3(adec);
            if (impl_->acodec && avcodec_parameters_to_context(impl_->acodec, apar) >= 0 &&
                avcodec_open2(impl_->acodec, adec, nullptr) >= 0) {
                impl_->aframe = av_frame_alloc();
                impl_->arate = impl_->acodec->sample_rate;
                if (impl_->arate <= 0)
                    impl_->arate = 48000;
            } else {
                if (impl_->acodec)
                    avcodec_free_context(&impl_->acodec);
                impl_->aidx = -1;
            }
        } else {
            impl_->aidx = -1;
        }
        if (!impl_->aframe)
            impl_->aidx = -1;
    }

    if (o.startMs > 0) {
        struct stat stbuf {};
        const bool fifo =
            ::stat(pathOrUrl.c_str(), &stbuf) == 0 && S_ISFIFO(stbuf.st_mode);
        // HTTP universal already has offset=; a second av_seek_frame on
        // loopback mpegts added seconds of A/V hole.
        if (!fifo && !loopbackHttp) {
            const int64_t ts =
                av_rescale_q(o.startMs, AVRational{1, 1000}, st->time_base);
            ret = av_seek_frame(impl_->fmt, impl_->vidx, ts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                err = "av_seek_frame: " + avErr(ret);
                close();
                return false;
            }
            avcodec_flush_buffers(impl_->codec);
        }
    }

    int srcW = 0;
    int srcH = 0;
    if (sw > 0 && sh > 0) {
        srcW = sw;
        srcH = sh;
    } else {
        const int dw = impl_->codec->width;
        const int dh = impl_->codec->height;
        if (dw > 0 && dh > 0) {
            if ((dw & 1) || (dh & 1)) {
                err = "source WxH must be positive even";
                close();
                return false;
            }
            srcW = dw;
            srcH = dh;
        }
    }
    impl_->srcW = srcW;
    impl_->srcH = srcH;
    impl_->w = outW;
    impl_->h = outH;
    impl_->open = true;
    impl_->flushing = false;
    impl_->pending = false;

    // Fail-closed: first frame must be YUV420P. Bank is out WxH; source may scale.
    std::string rerr;
    const int first = readI420(nullptr, 0, rerr);
    if (first != 1) {
        err = first == 0 ? "no video frames" : rerr;
        close();
        return false;
    }
    return true;
}

int AvInprocDecoder::readI420(uint8_t* dst, size_t frameBytes, std::string& err) {
    err.clear();
    if (!impl_ || !impl_->open || !impl_->codec || !impl_->fmt) {
        err = "decoder not open";
        return -1;
    }
    const int w = impl_->w;
    const int h = impl_->h;
    const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3 / 2;
    // dst==null / frameBytes==0 is the open() probe: decode+stash, no copy.
    const bool probe = (dst == nullptr && frameBytes == 0);
    if (!probe && (!dst || frameBytes != need)) {
        err = "frameBytes want " + std::to_string(need);
        return -1;
    }

    auto accept = [&]() -> int {
        if (impl_->frame->format != AV_PIX_FMT_YUV420P) {
            err = "frame pixfmt not YUV420P";
            av_frame_unref(impl_->frame);
            impl_->pending = false;
            return -1;
        }
        if (impl_->srcW == 0 || impl_->srcH == 0) {
            const int fw = impl_->frame->width;
            const int fh = impl_->frame->height;
            if (fw <= 0 || fh <= 0 || (fw & 1) || (fh & 1)) {
                err = "frame size must be positive even";
                av_frame_unref(impl_->frame);
                impl_->pending = false;
                return -1;
            }
            impl_->srcW = fw;
            impl_->srcH = fh;
        }
        const int srcW = impl_->srcW;
        const int srcH = impl_->srcH;
        if (impl_->frame->width != srcW || impl_->frame->height != srcH) {
            err = "frame size " + std::to_string(impl_->frame->width) + "x" +
                  std::to_string(impl_->frame->height) + " != src " +
                  std::to_string(srcW) + "x" + std::to_string(srcH);
            av_frame_unref(impl_->frame);
            impl_->pending = false;
            return -1;
        }
        if (probe) {
            impl_->pending = true;
            return 1;
        }
        bool packed = false;
        if (w == srcW && h == srcH) {
            packed = copyPackedI420(impl_->frame, dst, w, h, err);
        } else if (inprocScale1280to960(srcW, srcH, w, h)) {
            packed = downsampleI420_1280_to_960_planes(
                impl_->frame->data[0], impl_->frame->linesize[0],
                impl_->frame->data[1], impl_->frame->linesize[1],
                impl_->frame->data[2], impl_->frame->linesize[2], dst, need);
            if (!packed)
                err = "I420 4/3 downsample failed";
        } else {
            packed = scaleI420NearestPlanes(
                impl_->frame->data[0], impl_->frame->linesize[0],
                impl_->frame->data[1], impl_->frame->linesize[1],
                impl_->frame->data[2], impl_->frame->linesize[2], srcW, srcH,
                dst, w, h, need);
            if (!packed)
                err = "I420 nearest scale failed";
        }
        if (!packed) {
            av_frame_unref(impl_->frame);
            impl_->pending = false;
            return -1;
        }
        av_frame_unref(impl_->frame);
        impl_->pending = false;
        return 1;
    };

    if (impl_->pending) {
        return accept();
    }

    int invalidSkip = 0;
    for (;;) {
        int ret = avcodec_receive_frame(impl_->codec, impl_->frame);
        if (ret == 0)
            return accept();
        if (ret == AVERROR_EOF)
            return 0;
        if (ret != AVERROR(EAGAIN)) {
            err = "avcodec_receive_frame: " + avErr(ret);
            return -1;
        }
        if (impl_->flushing)
            return 0;

        ret = av_read_frame(impl_->fmt, impl_->pkt);
        if (ret == AVERROR_EOF) {
            impl_->flushing = true;
            if (impl_->acodec && impl_->aframe) {
                (void)avcodec_send_packet(impl_->acodec, nullptr);
                for (;;) {
                    const int ar = avcodec_receive_frame(impl_->acodec, impl_->aframe);
                    if (ar < 0)
                        break;
                    impl_->ingestAudioFrame();
                    av_frame_unref(impl_->aframe);
                }
            }
            {
                std::lock_guard<std::mutex> lk(impl_->pcmMu);
                impl_->audioEof = true;
                MPX_AV_TRACE(impl_->diagnosticAudioEof.store(true, std::memory_order_relaxed);)
                impl_->pcmCv.notify_all();
            }
            ret = avcodec_send_packet(impl_->codec, nullptr);
            if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                err = "avcodec_send_packet(flush): " + avErr(ret);
                return -1;
            }
            continue;
        }
        if (ret < 0) {
            err = "av_read_frame: " + avErr(ret);
            return -1;
        }
        if (impl_->aidx >= 0 && impl_->pkt->stream_index == impl_->aidx) {
            impl_->decodeAudioPacket();
            av_packet_unref(impl_->pkt);
            continue;
        }
        if (impl_->pkt->stream_index != impl_->vidx) {
            av_packet_unref(impl_->pkt);
            continue;
        }
        ret = avcodec_send_packet(impl_->codec, impl_->pkt);
        av_packet_unref(impl_->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            if (probe && ret == AVERROR_INVALIDDATA && invalidSkip < 24) {
                ++invalidSkip;
                continue;
            }
            err = "avcodec_send_packet: " + avErr(ret);
            return -1;
        }
    }
}

} // namespace misterplex
