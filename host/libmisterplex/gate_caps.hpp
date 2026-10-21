#pragma once
// T-SPS-GATE-FALLBACK (pre-reg v1.4.3 b57a87fb, section 1): the stream gate's view of the LOADED core's CAPS
// readback. Pure: VideoCapabilities in, a verdict out. No daemon state, no I/O.
//
// DCAP grant bits live in MPC1[31:16] (r1, review 02:02: no new CAPS word, no layout bump). Every core of record
// publishes [31:16] = 0 (a1), which is the v1 rule: Baseline/CAVLC, refs <= 1, P16x16 only, no B, progressive,
// level <= 3.0, i.e. exactly the limits CompressedSyntax::inspect hard-codes today. So v1 is behaviour-identical.
#include "libmisterplex/ddr_bitstream_ring.hpp"
#include "libmisterplex/ddr_frame_layout.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace misterplex::gate {

namespace ring = ddr_bitstream_ring;

// MPC1[31:16] layout.
constexpr uint32_t kMaxRefFramesShift = 16;  // [18:16]: 0 = absent = 1 (the H3 field)
constexpr uint32_t kMaxRefFramesMask = 0x7u << kMaxRefFramesShift;
constexpr uint32_t kPPart = 1u << 19;        // P16x8/P8x16/P8x8
constexpr uint32_t kPSub8x8 = 1u << 20;
constexpr uint32_t kCabac = 1u << 21;
constexpr uint32_t kBSlices = 1u << 22;
constexpr uint32_t kTransform8x8 = 1u << 23;
constexpr uint32_t kWeightedPred = 1u << 24;
constexpr uint32_t kInterlace = 1u << 25;
constexpr uint32_t kSpsRelax = 1u << 26;     // NAL/VCL HRD, pic_struct, max_dec_frame_buffering > 1
constexpr uint32_t kMaxLevelShift = 27;      // [29:27], table below
constexpr uint32_t kMaxLevelMask = 0x7u << kMaxLevelShift;
constexpr uint32_t kReservedMask = 0x3u << 30;  // [31:30] must be 0
constexpr uint32_t kKnownGrantMask = kMaxRefFramesMask | kPPart | kPSub8x8 | kCabac | kBSlices | kTransform8x8 |
                                     kWeightedPred | kInterlace | kSpsRelax | kMaxLevelMask;

// The low half this daemon understands: every VideoFeature bit (all 16 at this tree). The gate takes the mask as a
// parameter so a narrower future enum (or a test) can exercise the unknown-low-bit floor (s7b).
constexpr uint32_t kEnumKnownLowMask =
    ring::AccessUnits | ring::PictureCommit | ring::H264Decode | ring::Color420 | ring::Intra | ring::Inter |
    ring::Deblock | ring::ConsumedAudioClock | ring::SeparateOverlay | ring::Native240 | ring::Native480i |
    ring::Progressive480 | ring::Hd720 | ring::PresentationFeedback | ring::AudioSessionControl | ring::FencedReset;

// maxLevel code table (s7, review 01:46). Code 0 is v1. Any unlisted code is a hard error.
constexpr int maxLevelIdcForCode(uint32_t code) {
    switch (code) {
    case 0: return 30;
    case 1: return 31;
    case 2: return 32;
    case 3: return 40;
    case 4: return 41;
    default: return -1;
    }
}

struct GateCaps {
    int maxRefs = 1;
    bool inter = false;    // MPC1 Inter: P slices allowed at all
    bool deblock = false;  // MPC1 Deblock: deblocking_filter_idc 0/2 allowed (the session's filter choice still applies)
    bool pPart = false;
    bool pSub8x8 = false;
    bool cabac = false;
    bool bSlices = false;
    bool transform8x8 = false;
    bool weightedPred = false;
    bool interlace = false;
    bool spsRelax = false;
    int maxLevelIdc = 30;
    int maxWidth = 0;
    int maxHeight = 0;
    uint32_t maxAuBytes = 0;
};

enum class CapsOutcome {
    Ok,                 // play direct, gated on `caps`
    FallbackOnFloor,    // section 1 (ii)/(iv)/(v): never trust the word; ladder fallback, helper re-gated on `caps`
    NoPlay,             // section 1 (i): no trustworthy core; error, no play, no fallback
};

struct CapsVerdict {
    CapsOutcome outcome = CapsOutcome::NoPlay;
    GateCaps caps;
    uint32_t unknownMask = 0;
    std::string detail;
};

namespace detail {

inline std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// The v1 Baseline floor. lowTrusted: MPC1[15:0] parsed cleanly, so Inter/Deblock come from it (s7a). Otherwise the
// hard-coded constants: no Inter, no Deblock, so an all-IDR helper only (s7b).
inline GateCaps v1Floor(const ring::VideoCapabilities& c, bool lowTrusted) {
    GateCaps g;
    g.maxRefs = 1;
    g.inter = lowTrusted && (c.features & ring::Inter) != 0;
    g.deblock = lowTrusted && (c.features & ring::Deblock) != 0;
    g.maxLevelIdc = 30;
    g.maxWidth = c.max_width;
    g.maxHeight = c.max_height;
    g.maxAuBytes = c.max_au_bytes;
    return g;
}

} // namespace detail

// Section 1 of the pre-reg, in order: (i) no play; (ii) unknown bits / unlisted level and (iv)/(v) prerequisites,
// fallback on a floor; else the granted caps.
inline CapsVerdict evaluateCaps(const ring::VideoCapabilities& c, uint32_t knownLowMask = kEnumKnownLowMask) {
    CapsVerdict v;
    if (!c.supportsVideo()) {
        v.outcome = CapsOutcome::NoPlay;
        v.detail = "gate caps (i): supportsVideo false";
        return v;
    }
    if (c.max_width > kPlex720pCodedWidth || c.max_height > kPlex720pCodedHeight) {
        v.outcome = CapsOutcome::NoPlay;
        v.detail = "gate caps (i): MPC2 " + std::to_string(c.max_width) + "x" + std::to_string(c.max_height) +
                   " exceeds the largest frame layout " + std::to_string(kPlex720pCodedWidth) + "x" +
                   std::to_string(kPlex720pCodedHeight);
        return v;
    }
    const uint32_t f = c.features;
    const uint32_t unknownLow = (f & 0xFFFFu) & ~knownLowMask;
    const uint32_t unknownHigh = (f & 0xFFFF0000u) & ~kKnownGrantMask;
    if (unknownLow || unknownHigh) {
        v.outcome = CapsOutcome::FallbackOnFloor;
        v.unknownMask = unknownLow | unknownHigh;
        v.caps = detail::v1Floor(c, unknownLow == 0);
        v.detail = "gate caps: unknown DCAP bits " + detail::hex32(v.unknownMask);
        return v;
    }
    const uint32_t levelCode = (f & kMaxLevelMask) >> kMaxLevelShift;
    const int levelIdc = maxLevelIdcForCode(levelCode);
    if (levelIdc < 0) {
        v.outcome = CapsOutcome::FallbackOnFloor;
        v.caps = detail::v1Floor(c, true);
        v.detail = "gate caps: unlisted maxLevel " + std::to_string(levelCode);
        return v;
    }
    const uint32_t refsField = (f & kMaxRefFramesMask) >> kMaxRefFramesShift;
    const bool inter = (f & ring::Inter) != 0;
    // a2: a grant never enables a feature whose low-half prerequisite is clear; such a word is inconsistent (iv).
    const uint32_t needsInter = kPPart | kPSub8x8 | kBSlices | kWeightedPred;
    const uint32_t needsDecode = kTransform8x8 | kCabac;
    uint32_t orphan = 0;
    if (!inter)
        orphan |= (f & needsInter) | (refsField > 1 ? (f & kMaxRefFramesMask) : 0u);
    if ((f & ring::H264Decode) == 0)
        orphan |= f & needsDecode;
    if (orphan) {
        v.outcome = CapsOutcome::FallbackOnFloor;
        v.unknownMask = orphan;
        v.caps = detail::v1Floor(c, true);
        v.detail = "gate caps (iv): grant without its prerequisite " + detail::hex32(orphan);
        return v;
    }
    GateCaps& g = v.caps;
    g.maxRefs = refsField == 0 ? 1 : static_cast<int>(refsField);
    g.inter = inter;
    g.deblock = (f & ring::Deblock) != 0;
    g.pPart = (f & kPPart) != 0;
    g.pSub8x8 = (f & kPSub8x8) != 0;
    g.cabac = (f & kCabac) != 0;
    g.bSlices = (f & kBSlices) != 0;
    g.transform8x8 = (f & kTransform8x8) != 0;
    g.weightedPred = (f & kWeightedPred) != 0;
    g.interlace = (f & kInterlace) != 0;
    g.spsRelax = (f & kSpsRelax) != 0;
    g.maxLevelIdc = levelIdc;
    g.maxWidth = c.max_width;
    g.maxHeight = c.max_height;
    g.maxAuBytes = c.max_au_bytes;
    v.outcome = CapsOutcome::Ok;
    return v;
}

// Stream verdict (pre-reg v1.4.3 section 3 (a)): what CompressedSyntax::inspect decided for one AU.
// Kind follows the stream: legal H.264 that this core or decoder cannot take (a CAPS/product bound, or more active
// references than max_num_ref_frames DPB slots, review 04:16 n1) is OUT; non-conforming H.264 (truncation, ue
// overflow, a spec range, a missing parameter set, a P slice under max_num_ref_frames 0) is MALFORMED. Only MALFORMED
// may hard-error; every OUT takes the transcode fallback (review 04:16, standing bar for the wiring layer).
enum class StreamOutcome { In, Out, Malformed };

struct StreamVerdict {
    StreamOutcome outcome = StreamOutcome::In;
    const char* field = "";  // the first term that tripped, in the site's pinned order
    int64_t value = 0;
    int64_t limit = 0;       // the bound the value broke: the max for a <= rule, the required value for a == rule,
                             // -1 for a set-membership rule
    const char* why = "";    // MALFORMED: truncated, range, no SPS, ...; OUT: empty, or a decoder-limit reason
};

inline const char* streamOutcomeName(StreamOutcome o) {
    return o == StreamOutcome::In ? "IN" : o == StreamOutcome::Out ? "OUT" : "MALFORMED";
}

inline std::string describe(const StreamVerdict& v) {
    if (v.outcome == StreamOutcome::In)
        return "IN";
    std::string s = std::string(streamOutcomeName(v.outcome)) + " field=" + v.field +
                    " value=" + std::to_string(v.value) + " limit=" + std::to_string(v.limit);
    if (v.why[0] != '\0')
        s += std::string(" why=") + v.why;
    return s;
}

} // namespace misterplex::gate
