// T-SPS-GATE-FALLBACK code layer 2 (pre-reg v1.4.3 b57a87fb: sections 2, 3 (a), 6 and 8 T10): CompressedSyntax::inspect
// keyed on GateCaps for the H3 refs family, plus its typed verdict. One TU with arm/misterplexd/av_inproc_decode.cpp and
// the system libav, as test_l2_transcode_dar_demux builds it. Host unit: no daemon, no fake core.
//   - v1 anchor: the gatefix corpus c764a27d (tests/fixtures/gatefix_c764a27d) under the prod CAPS line reproduces the
//     gatefix daemon run of record: err + refused AU, 17/17 (expect.tsv b62a3ad9, RECORD.v2.MD5 aa19e8d0).
//   - H3: the same corpus under MAX_REF_FRAMES 2 and 3.
//   - H264 p1: a P slice under max_num_ref_frames 0 is MALFORMED{max_num_ref_frames, 0}.
//   - H264 p2: one tooth per refs-family field, combined violations, and the pinned (syntax) order per NAL.
//   - H264 p3 / review 03:04: an effective active count above max_num_ref_frames is refused, at the P slice only.
//     review 04:16 (n1): such a stream is legal H.264, so the kind is OUT{why 'active > max_num_ref_frames'}.
// Including the .cpp makes GCC treat it as a header, so its anonymous-namespace member types warn here only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#include "av_inproc_decode.cpp"
#pragma GCC diagnostic pop

#include "libmisterplex/fpga_build_ids.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace ring = misterplex::ddr_bitstream_ring;
namespace gate = misterplex::gate;
using gate::StreamOutcome;

int fails = 0;
int checks = 0;

void expect(bool cond, const std::string& msg) {
    ++checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL compressed syntax: %s\n", msg.c_str());
        ++fails;
    }
}

const char* const kSps = "unsupported SPS: requires 8-bit baseline/CAVLC progressive ref<=1 geometry";
const char* const kPps = "unsupported PPS: CABAC/FMO/weighted/multiple references";
const char* const kSlice = "unsupported slice: requires single complete I/P picture";
const char* const kOvr = "multiple active references are unsupported";
const char* const kRplm = "reference list modification is unsupported";
const char* const kActive = "active reference count exceeds max_num_ref_frames";
const char* const kAu = "access unit lacks one complete picture or changes SPS/PPS outside IDR";

// The prod 0x48005D52 CAPS line (tl-1718 live-prod-restore caps.txt 1f3116b4): MPC1 0x0000E1BF, MPC2 0x01E00280
// (640x480), MPC3 0x00020000, with MAX_REF_FRAMES = MPC1[18:16] (0 = absent = 1).
gate::GateCaps capsWithRefs(uint32_t refsField) {
    ring::VideoCapabilities c;
    c.abi_version = ::mailbox_abi::kFpgaVideoAbiVersion;
    c.layout_id = ::mailbox_abi::kFpgaVideoLayoutId;
    c.features = 0x0000E1BFu | (refsField << gate::kMaxRefFramesShift);
    c.max_width = 640;
    c.max_height = 480;
    c.max_au_bytes = 131072;
    c.build_id = 0x48005D52u;
    c.nonce = 0x7E8A252E8C9A0691ull;
    c.publication = 1;
    const auto v = gate::evaluateCaps(c);
    expect(v.outcome == gate::CapsOutcome::Ok, "caps refs field " + std::to_string(refsField) + ": Ok");
    return v.caps;
}

// media_player.cpp :3829-3859 on a 480p raster: expect = the CAPS ceiling, allowInter/allowDeblock from the low half
// (prototype "ip", filter "on"), the AU and VCL RBSP budgets = MPC3.
misterplex::AvInprocOpenOpts optsFor(const gate::GateCaps& g) {
    misterplex::AvInprocOpenOpts o;
    o.compressedVideo = true;
    o.expectW = g.maxWidth;
    o.expectH = g.maxHeight;
    o.maxAccessUnitBytes = g.maxAuBytes;
    o.maxVclRbspBytes = g.maxAuBytes;
    o.allowInter = g.inter;
    o.allowDeblock = g.deblock;
    o.gateCaps = g;
    return o;
}

struct Want {
    int au = -1;  // the refused AU; -1 = every AU IN
    const char* err = "";
    StreamOutcome kind = StreamOutcome::In;
    const char* field = "";
    int64_t value = 0;
    int64_t limit = 0;
    const char* why = "";
};

const Want kIn{};

std::string show(int au, StreamOutcome kind, const char* field, int64_t value, int64_t limit, const char* why) {
    if (au < 0)
        return "IN";
    gate::StreamVerdict v{kind, field, value, limit, why};
    return "AU" + std::to_string(au) + " " + gate::describe(v);
}

std::string show(const Want& w) { return show(w.au, w.kind, w.field, w.value, w.limit, w.why); }

struct Got {
    int au = -1;
    std::string err;
    gate::StreamVerdict v;
};

std::string show(const Got& g) { return show(g.au, g.v.outcome, g.v.field, g.v.value, g.v.limit, g.v.why); }

bool same(const Want& w, const Got& g) {
    if (w.au != g.au)
        return false;
    if (w.au < 0)
        return g.v.outcome == StreamOutcome::In;
    return w.kind == g.v.outcome && std::strcmp(w.field, g.v.field) == 0 && w.value == g.v.value &&
           w.limit == g.v.limit && std::strcmp(w.why, g.v.why) == 0;
}

// Feeds the AUs in order to one session's inspect, stopping at the first refusal.
Got run(const std::vector<std::vector<uint8_t>>& aus, const misterplex::AvInprocOpenOpts& o, const std::string& tag) {
    misterplex::CompressedSyntax cs;
    Got g;
    for (size_t i = 0; i < aus.size(); ++i) {
        bool idr = false, changed = false;
        std::string err;
        if (!cs.inspect(aus[i].data(), aus[i].size(), o, idr, changed, err)) {
            g.au = static_cast<int>(i);
            g.err = err;
            g.v = cs.verdict;
            return g;
        }
        expect(cs.verdict.outcome == StreamOutcome::In && err.empty(), tag + ": AU" + std::to_string(i) + " accepted IN");
    }
    return g;
}

// An Annex-B file split at each access unit delimiter (NAL type 9), as the demuxer hands AUs to inspect.
std::vector<std::vector<uint8_t>> splitAud(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::vector<uint8_t>> aus;
    size_t start = std::string::npos;
    for (size_t i = 0; i + 3 < b.size(); ++i) {
        const bool sc4 = b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1;
        if (!sc4 || i + 4 >= b.size() || (b[i + 4] & 0x1f) != 9) continue;
        if (start != std::string::npos) aus.emplace_back(b.begin() + start, b.begin() + i);
        start = i;
    }
    if (start != std::string::npos) aus.emplace_back(b.begin() + start, b.end());
    return aus;
}

void check(const std::string& tag, const Want& w, const Got& g) {
    expect(same(w, g), tag + ": want " + show(w) + ", got " + show(g));
    expect(g.err == w.err, tag + ": err '" + g.err + "', want '" + w.err + "'");
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    expect(static_cast<bool>(f), "fixture " + path + " is readable");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- synthetic parameter sets (p2 order teeth) -------------------------------------------------------------------

struct BitWriter {
    std::vector<uint8_t> b;
    int n = 0;
    void bit(uint32_t v) {
        if (n == 0)
            b.push_back(0);
        if (v & 1u)
            b.back() = static_cast<uint8_t>(b.back() | (0x80u >> n));
        n = (n + 1) % 8;
    }
    void u(int bits, uint32_t v) {
        for (int i = bits - 1; i >= 0; --i)
            bit(v >> i);
    }
    void ue(uint32_t v) {
        const uint64_t x = uint64_t(v) + 1;
        int len = 0;
        while ((x >> len) > 1)
            ++len;
        for (int i = 0; i < len; ++i)
            bit(0);
        for (int i = len; i >= 0; --i)
            bit(static_cast<uint32_t>(x >> i));
    }
    void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * int64_t(v))); }
    std::vector<uint8_t> rbsp() {
        bit(1);
        while (n != 0)
            bit(0);
        return b;
    }
};

// Annex-B NAL with emulation prevention.
std::vector<uint8_t> nal(uint8_t header, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> o{0, 0, 0, 1, header};
    int zeros = 0;
    for (uint8_t c : rbsp) {
        if (zeros >= 2 && c <= 3) {
            o.push_back(3);
            zeros = 0;
        }
        o.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return o;
}

// 7.3.2.1.1, Baseline 640x480 refs 1 by default; profile 100 writes the High fields.
struct SpsF {
    uint32_t profile = 66, constraint = 0xC0, level = 30, id = 0, frameBits = 0, poc = 2, pocBits = 0, refs = 1;
    uint32_t gaps = 0, mbwM1 = 39, mbhM1 = 29, frameMbsOnly = 1, crop = 0, cl = 0, cr = 0, ct = 0, cb = 0;
    // VUI (E.1.1), written only when vui = 1: limited-range BT.601 colour, then the fields past colour.
    uint32_t vui = 0, timing = 0, nalHrd = 0, vclHrd = 0, cpbCnt = 0, picStruct = 0, restriction = 0;
    uint32_t reorder = 0, decBuf = 1;
    uint64_t bitRateM1 = 66624, cpbSizeM1 = 66624;
    size_t cutTo = 0;  // truncate the RBSP to this many bytes (0 = whole)
};

std::vector<uint8_t> spsNal(const SpsF& f) {
    BitWriter w;
    w.u(8, f.profile);
    w.u(8, f.constraint);
    w.u(8, f.level);
    w.ue(f.id);
    if (f.profile == 100) {
        w.ue(1);    // chroma_format_idc 4:2:0
        w.ue(0);    // bit_depth_luma_minus8
        w.ue(0);    // bit_depth_chroma_minus8
        w.u(1, 0);  // qpprime_y_zero_transform_bypass_flag
        w.u(1, 0);  // seq_scaling_matrix_present_flag
    }
    w.ue(f.frameBits);
    w.ue(f.poc);
    if (f.poc == 0) {
        w.ue(f.pocBits);
    } else if (f.poc == 1) {
        w.u(1, 1);  // delta_pic_order_always_zero_flag
        w.se(0);    // offset_for_non_ref_pic
        w.se(0);    // offset_for_top_to_bottom_field
        w.ue(0);    // num_ref_frames_in_pic_order_cnt_cycle
    }
    w.ue(f.refs);
    w.u(1, f.gaps);
    w.ue(f.mbwM1);
    w.ue(f.mbhM1);
    w.u(1, f.frameMbsOnly);
    if (!f.frameMbsOnly)
        w.u(1, 0);  // mb_adaptive_frame_field_flag
    w.u(1, 1);      // direct_8x8_inference_flag
    w.u(1, f.crop);
    if (f.crop) {
        w.ue(f.cl);
        w.ue(f.cr);
        w.ue(f.ct);
        w.ue(f.cb);
    }
    w.u(1, f.vui);  // vui_parameters_present_flag
    if (f.vui) {
        auto ue64 = [&](uint64_t v) {
            const uint64_t x = v + 1;
            int len = 0;
            while ((x >> len) > 1)
                ++len;
            for (int i = 0; i < len; ++i)
                w.bit(0);
            for (int i = len; i >= 0; --i)
                w.bit(static_cast<uint32_t>(x >> i));
        };
        auto hrd = [&]() {
            w.ue(f.cpbCnt);
            w.u(4, 0);
            w.u(4, 0);
            for (uint32_t i = 0; i <= f.cpbCnt; ++i) {
                ue64(f.bitRateM1);
                ue64(f.cpbSizeM1);
                w.u(1, 0);
            }
            w.u(5, 23); w.u(5, 23); w.u(5, 23); w.u(5, 24);
        };
        w.u(1, 0);  // aspect_ratio_info_present_flag
        w.u(1, 0);  // overscan_info_present_flag
        w.u(1, 1);  // video_signal_type_present_flag
        w.u(3, 5); w.u(1, 0); w.u(1, 1);  // format, limited range, colour description
        w.u(8, 6); w.u(8, 6); w.u(8, 6);  // BT.601
        w.u(1, 0);  // chroma_loc_info_present_flag
        w.u(1, f.timing);
        if (f.timing) { w.u(32, 1001); w.u(32, 60000); w.u(1, 1); }
        w.u(1, f.nalHrd);
        if (f.nalHrd) hrd();
        w.u(1, f.vclHrd);
        if (f.vclHrd) hrd();
        if (f.nalHrd || f.vclHrd) w.u(1, 0);  // low_delay_hrd_flag
        w.u(1, f.picStruct);
        w.u(1, f.restriction);
        if (f.restriction) {
            w.u(1, 1); w.ue(0); w.ue(0); w.ue(16); w.ue(16);
            w.ue(f.reorder);
            w.ue(f.decBuf);
        }
    }
    auto r = w.rbsp();
    if (f.cutTo)
        r.resize(f.cutTo);
    return nal(0x67, r);
}

// 7.3.2.2 (Baseline: no extension).
struct PpsF {
    uint32_t id = 0, spsId = 0, cabac = 0, bottomPoc = 0, groups = 0, l0 = 0, l1 = 0, weighted = 0, bipred = 0;
    int32_t qp = 0, qs = 0, chroma = 0;
    uint32_t deblock = 1, constrained = 0, redundant = 0;
    // Main PPS tail (7.3.2.2 more_rbsp_data). 0 keeps the Baseline writer.
    uint32_t ext = 0, xform8 = 0, scaling = 0;
    int32_t secondChroma = 0;
    size_t cutTo = 0;
};

std::vector<uint8_t> ppsNal(const PpsF& f) {
    BitWriter w;
    w.ue(f.id);
    w.ue(f.spsId);
    w.u(1, f.cabac);
    w.u(1, f.bottomPoc);
    w.ue(f.groups);
    if (f.groups) {
        w.ue(0);  // slice_group_map_type 0 (interleaved)
        for (uint32_t i = 0; i <= f.groups; ++i)
            w.ue(0);  // run_length_minus1
    }
    w.ue(f.l0);
    w.ue(f.l1);
    w.u(1, f.weighted);
    w.u(2, f.bipred);
    w.se(f.qp);
    w.se(f.qs);
    w.se(f.chroma);
    w.u(1, f.deblock);
    w.u(1, f.constrained);
    w.u(1, f.redundant);
    if (f.ext) {
        w.u(1, f.xform8);
        w.u(1, f.scaling);
        w.se(f.secondChroma);
    }
    auto r = w.rbsp();
    if (f.cutTo)
        r.resize(f.cutTo);
    return nal(0x68, r);
}

std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> o;
    for (const auto& p : parts)
        o.insert(o.end(), p.begin(), p.end());
    return o;
}

Want out(int au, const char* err, const char* field, int64_t value, int64_t limit) {
    return Want{au, err, StreamOutcome::Out, field, value, limit, ""};
}

Want outWhy(int au, const char* err, const char* field, int64_t value, int64_t limit, const char* why) {
    return Want{au, err, StreamOutcome::Out, field, value, limit, why};
}

Want bad(int au, const char* err, const char* field, int64_t value, int64_t limit, const char* why) {
    return Want{au, err, StreamOutcome::Malformed, field, value, limit, why};
}

} // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string fx = root + "/tests/fixtures/gatefix_c764a27d/";
    const gate::GateCaps v1 = capsWithRefs(0), r2 = capsWithRefs(2), r3 = capsWithRefs(3);
    expect(v1.maxRefs == 1 && r2.maxRefs == 2 && r3.maxRefs == 3 && v1.inter && !v1.deblock,
           "caps: maxRefs 1/2/3, Inter on, Deblock off (0xE1BF)");
    // The default GateCaps is the v1 rule, so an opts that never sets it keeps today's limits.
    expect(misterplex::AvInprocOpenOpts{}.gateCaps.maxRefs == 1, "AvInprocOpenOpts default gateCaps.maxRefs 1");

    // ---- T10 corpus: v1 = the gatefix daemon run of record; H3 at MAX_REF_FRAMES 2 and 3 ----
    const char* mnrf = "max_num_ref_frames";
    const char* l0d = "num_ref_idx_l0_default_active";
    const char* l0o = "num_ref_idx_l0_active_override";
    const char* rplm = "ref_pic_list_modification_flag_l0";
    const char* p3 = "active > max_num_ref_frames";
    struct Row {
        const char* fixture;
        Want v1, r2, r3;
    };
    const Row rows[] = {
        {"B0_base", kIn, kIn, kIn},
        {"G9_acc_refs2", out(0, kSps, mnrf, 2, 1), kIn, kIn},
        {"G9_acc_refs3", out(0, kSps, mnrf, 3, 1), out(0, kSps, mnrf, 3, 2), kIn},
        {"G18_ref_l0d1", out(0, kPps, l0d, 2, 1), outWhy(1, kActive, l0d, 2, 1, p3),
         outWhy(1, kActive, l0d, 2, 1, p3)},
        {"G24_ref_P_refs0", bad(1, kSlice, mnrf, 0, 1, "P without references"),
         bad(1, kSlice, mnrf, 0, 1, "P without references"), bad(1, kSlice, mnrf, 0, 1, "P without references")},
        {"G26_ctl_ovr_val0", kIn, kIn, kIn},
        {"G26_ref_ovr1", out(1, kOvr, l0o, 2, 1), outWhy(1, kActive, l0o, 2, 1, p3),
         outWhy(1, kActive, l0o, 2, 1, p3)},
        {"G27_ref_rplm", out(1, kRplm, rplm, 1, 0), out(1, kRplm, rplm, 1, 0), out(1, kRplm, rplm, 1, 0)},
        {"G18_acc_refs2", out(0, kSps, mnrf, 2, 1), kIn, kIn},
        {"G24_acc_refs2", out(0, kSps, mnrf, 2, 1), kIn, kIn},
        {"G26_acc_refs2", out(0, kSps, mnrf, 2, 1), kIn, kIn},
        {"G27_acc_refs2", out(0, kSps, mnrf, 2, 1), out(1, kRplm, rplm, 1, 0), out(1, kRplm, rplm, 1, 0)},
        {"N_gaps", out(0, kSps, "gaps_in_frame_num_value_allowed_flag", 1, 0),
         out(0, kSps, "gaps_in_frame_num_value_allowed_flag", 1, 0),
         out(0, kSps, "gaps_in_frame_num_value_allowed_flag", 1, 0)},
        {"N_interlace", out(0, kSps, "frame_mbs_only_flag", 0, 1), out(0, kSps, "frame_mbs_only_flag", 0, 1),
         out(0, kSps, "frame_mbs_only_flag", 0, 1)},
        {"N_l1d1", out(0, kPps, "num_ref_idx_l1_default_active", 2, 1),
         out(0, kPps, "num_ref_idx_l1_default_active", 2, 1), out(0, kPps, "num_ref_idx_l1_default_active", 2, 1)},
        {"N_weighted", out(0, kPps, "weighted_pred_flag", 1, 0), out(0, kPps, "weighted_pred_flag", 1, 0),
         out(0, kPps, "weighted_pred_flag", 1, 0)},
        {"N_bipred", out(0, kPps, "weighted_bipred_idc", 1, 0), out(0, kPps, "weighted_bipred_idc", 1, 0),
         out(0, kPps, "weighted_bipred_idc", 1, 0)},
    };
    int corpus = 0;
    for (const Row& row : rows) {
        const std::vector<std::vector<uint8_t>> aus{readFile(fx + row.fixture + ".au0.264"),
                                                    readFile(fx + row.fixture + ".au1.264")};
        const std::pair<const char*, std::pair<const gate::GateCaps*, const Want*>> legs[] = {
            {"v1", {&v1, &row.v1}}, {"refs2", {&r2, &row.r2}}, {"refs3", {&r3, &row.r3}}};
        for (const auto& leg : legs) {
            const std::string tag = std::string(row.fixture) + " " + leg.first;
            check(tag, *leg.second.second, run(aus, optsFor(*leg.second.first), tag));
        }
        ++corpus;
    }
    expect(corpus == 17, "corpus rows 17, got " + std::to_string(corpus));

    // ---- p2: the pinned order, one field at a time, then combined (synthetic SPS/PPS, AU = the parameter sets) ----
    const auto o1 = optsFor(v1), o2 = optsFor(r2), o3 = optsFor(r3);
    const std::vector<uint8_t> goodSps = spsNal(SpsF{});
    // Control: the default SPS + PPS pass their NAL checks, so the AU is refused only for having no picture.
    check("control SPS+PPS", bad(0, kAu, "pictures_per_access_unit", 0, 1, "no picture"),
          run({cat({goodSps, ppsNal(PpsF{})})}, o1, "control SPS+PPS"));
    struct SpsCase {
        const char* name;
        std::function<void(SpsF&)> edit;
        Want want;
    };
    const SpsCase spsCases[] = {
        {"profile 77", [](SpsF& f) { f.profile = 77; }, out(0, kSps, "profile_idc", 77, 66)},
        {"profile 100 (High fields)", [](SpsF& f) { f.profile = 100; }, out(0, kSps, "profile_idc", 100, 66)},
        {"level 31", [](SpsF& f) { f.level = 31; }, out(0, kSps, "level_idc", 31, 30)},
        {"sps id 32", [](SpsF& f) { f.id = 32; }, bad(0, kSps, "seq_parameter_set_id", 32, 31, "range")},
        {"frame_num bits 13", [](SpsF& f) { f.frameBits = 13; },
         bad(0, kSps, "log2_max_frame_num_minus4", 13, 12, "range")},
        {"poc type 1", [](SpsF& f) { f.poc = 1; }, out(0, kSps, "pic_order_cnt_type", 1, -1)},
        {"poc type 3", [](SpsF& f) { f.poc = 3; }, bad(0, kSps, "pic_order_cnt_type", 3, 2, "range")},
        {"poc lsb bits 13", [](SpsF& f) { f.poc = 0; f.pocBits = 13; },
         bad(0, kSps, "log2_max_pic_order_cnt_lsb_minus4", 13, 12, "range")},
        {"refs 2", [](SpsF& f) { f.refs = 2; }, out(0, kSps, mnrf, 2, 1)},
        {"gaps", [](SpsF& f) { f.gaps = 1; }, out(0, kSps, "gaps_in_frame_num_value_allowed_flag", 1, 0)},
        {"width 41 MBs", [](SpsF& f) { f.mbwM1 = 40; }, out(0, kSps, "pic_width_in_mbs", 41, 40)},
        {"height 31 MBs", [](SpsF& f) { f.mbhM1 = 30; }, out(0, kSps, "pic_height_in_map_units", 31, 30)},
        {"interlace", [](SpsF& f) { f.frameMbsOnly = 0; }, out(0, kSps, "frame_mbs_only_flag", 0, 1)},
        {"crop left+right 320", [](SpsF& f) { f.crop = 1; f.cl = 160; f.cr = 160; },
         bad(0, kSps, "frame_crop_left_right_offset", 320, 319, "range")},
        {"crop top+bottom 240", [](SpsF& f) { f.crop = 1; f.ct = 120; f.cb = 120; },
         bad(0, kSps, "frame_crop_top_bottom_offset", 240, 239, "range")},
        {"truncated after max_num_ref_frames", [](SpsF& f) { f.cutTo = 4; },
         bad(0, kSps, "gaps_in_frame_num_value_allowed_flag", 0, 0, "truncated")},
    };
    for (const auto& c : spsCases) {
        SpsF f;
        c.edit(f);
        const std::string tag = std::string("SPS ") + c.name;
        check(tag, c.want, run({spsNal(f)}, o1, tag));
    }
    struct PpsCase {
        const char* name;
        std::function<void(PpsF&)> edit;
        Want want;
    };
    const PpsCase ppsCases[] = {
        {"pps id 256", [](PpsF& f) { f.id = 256; }, bad(0, kPps, "pic_parameter_set_id", 256, 255, "range")},
        {"sps id 1", [](PpsF& f) { f.spsId = 1; }, out(0, kPps, "seq_parameter_set_id", 1, 0)},
        {"cabac", [](PpsF& f) { f.cabac = 1; }, out(0, kPps, "entropy_coding_mode_flag", 1, 0)},
        {"bottom field poc", [](PpsF& f) { f.bottomPoc = 1; },
         out(0, kPps, "bottom_field_pic_order_in_frame_present_flag", 1, 0)},
        {"FMO 2 groups", [](PpsF& f) { f.groups = 1; }, out(0, kPps, "num_slice_groups_minus1", 1, 0)},
        {"l0 default 2", [](PpsF& f) { f.l0 = 1; }, out(0, kPps, l0d, 2, 1)},
        {"l1 default 2", [](PpsF& f) { f.l1 = 1; }, out(0, kPps, "num_ref_idx_l1_default_active", 2, 1)},
        {"weighted", [](PpsF& f) { f.weighted = 1; }, out(0, kPps, "weighted_pred_flag", 1, 0)},
        {"bipred 1", [](PpsF& f) { f.bipred = 1; }, out(0, kPps, "weighted_bipred_idc", 1, 0)},
        {"bipred 3", [](PpsF& f) { f.bipred = 3; }, bad(0, kPps, "weighted_bipred_idc", 3, 2, "range")},
        {"qp -27", [](PpsF& f) { f.qp = -27; }, bad(0, kPps, "pic_init_qp_minus26", -27, -26, "range")},
        {"qs 26", [](PpsF& f) { f.qs = 26; }, bad(0, kPps, "pic_init_qs_minus26", 26, 25, "range")},
        {"chroma 13", [](PpsF& f) { f.chroma = 13; }, bad(0, kPps, "chroma_qp_index_offset", 13, 12, "range")},
        {"redundant", [](PpsF& f) { f.redundant = 1; }, out(0, kPps, "redundant_pic_cnt_present_flag", 1, 0)},
        {"truncated after weighted_pred_flag", [](PpsF& f) { f.cutTo = 1; },
         bad(0, kPps, "weighted_bipred_idc", 0, 0, "truncated")},
    };
    for (const auto& c : ppsCases) {
        PpsF f;
        c.edit(f);
        const std::string tag = std::string("PPS ") + c.name;
        check(tag, c.want, run({cat({goodSps, ppsNal(f)})}, o1, tag));
    }
    check("PPS before any SPS", bad(0, kPps, "seq_parameter_set_id", 0, -1, "no SPS"),
          run({ppsNal(PpsF{})}, o1, "PPS before any SPS"));

    // Combined violations name the first field in the pinned order; the CAPS decide whether a refs field trips.
    SpsF refsGaps;
    refsGaps.refs = 3;
    refsGaps.gaps = 1;
    check("SPS refs 3 + gaps, v1", out(0, kSps, mnrf, 3, 1), run({spsNal(refsGaps)}, o1, "SPS refs 3 + gaps, v1"));
    check("SPS refs 3 + gaps, refs3", out(0, kSps, "gaps_in_frame_num_value_allowed_flag", 1, 0),
          run({spsNal(refsGaps)}, o3, "SPS refs 3 + gaps, refs3"));
    SpsF profRefs;
    profRefs.profile = 77;
    profRefs.refs = 3;
    check("SPS profile 77 + refs 3", out(0, kSps, "profile_idc", 77, 66),
          run({spsNal(profRefs)}, o1, "SPS profile 77 + refs 3"));
    SpsF refsInterlace;
    refsInterlace.refs = 2;
    refsInterlace.frameMbsOnly = 0;
    check("SPS refs 2 + interlace, v1", out(0, kSps, mnrf, 2, 1),
          run({spsNal(refsInterlace)}, o1, "SPS refs 2 + interlace, v1"));
    PpsF l0Weighted;
    l0Weighted.l0 = 2;
    l0Weighted.weighted = 1;
    check("PPS l0 default 3 + weighted, v1", out(0, kPps, l0d, 3, 1),
          run({cat({goodSps, ppsNal(l0Weighted)})}, o1, "PPS l0 default 3 + weighted, v1"));
    PpsF cabacL0;
    cabacL0.cabac = 1;
    cabacL0.l0 = 1;
    check("PPS cabac + l0 default 2", out(0, kPps, "entropy_coding_mode_flag", 1, 0),
          run({cat({goodSps, ppsNal(cabacL0)})}, o1, "PPS cabac + l0 default 2"));
    {
        auto oHigh = o1;
        oHigh.allowHighCabac = true;
        SpsF mainSps;
        mainSps.profile = 77;
        PpsF mainPps;
        mainPps.cabac = 1;
        mainPps.ext = 1;
        check("High Main+CABAC PPS admitted", bad(0, kAu, "pictures_per_access_unit", 0, 1, "no picture"),
              run({cat({spsNal(mainSps), ppsNal(mainPps)})}, oHigh, "High Main+CABAC PPS admitted"));
        mainPps.xform8 = 1;
        check("High 8x8 still refused", out(0, kPps, "transform_8x8_mode_flag", 1, 0),
              run({cat({spsNal(mainSps), ppsNal(mainPps)})}, oHigh, "High 8x8 still refused"));
        SpsF highSps;
        highSps.profile = 100;
        check("High profile 100 still refused", out(0, kSps, "profile_idc", 100, 66),
              run({spsNal(highSps)}, oHigh, "High profile 100 still refused"));
        check("baseline opts still refuse Main", out(0, kSps, "profile_idc", 77, 66),
              run({spsNal(mainSps)}, o1, "baseline opts still refuse Main"));
    }
    PpsF l0l1;
    l0l1.l0 = 1;
    l0l1.l1 = 1;
    SpsF refs2Sps;
    refs2Sps.refs = 2;
    check("PPS l0 default 2 + l1 default 2, refs2", out(0, kPps, "num_ref_idx_l1_default_active", 2, 1),
          run({cat({spsNal(refs2Sps), ppsNal(l0l1)})}, o2, "PPS l0 default 2 + l1 default 2, refs2"));
    PpsF fmoL0;
    fmoL0.groups = 1;
    fmoL0.l0 = 1;
    check("PPS FMO + l0 default 2", out(0, kPps, "num_slice_groups_minus1", 1, 0),
          run({cat({goodSps, ppsNal(fmoL0)})}, o1, "PPS FMO + l0 default 2"));

    // p3 lives at the P slice only: an all-intra x264 keyint=1 stream pairs max_num_ref_frames 0 with a default
    // active count of 1, and its SPS + PPS must pass (AU refused only for having no picture).
    SpsF intraOnly;
    intraOnly.refs = 0;
    check("SPS refs 0 + PPS l0 default 1 (all-intra)", bad(0, kAu, "pictures_per_access_unit", 0, 1, "no picture"),
          run({cat({spsNal(intraOnly), ppsNal(PpsF{})})}, o1, "SPS refs 0 + PPS l0 default 1"));

    // The override's ue cut inside its suffix: BitReader::ue returns a nonzero partial value with br.ok false, and
    // d42a4460 :353 refused that with :354's err (cut inside the leading zeros, it returns 0 and :389's err follows).
    // The err is kept; the verdict is MALFORMED truncated. log2_max_frame_num 6 puts the ue on a byte boundary:
    // first_mb '1', slice_type 5 '00110', pps '1', frame_num 6 bits, override flag '1' = 14 bits, then the ue.
    SpsF fn6;
    fn6.frameBits = 2;
    auto pSliceCut = [](uint32_t minus1) {
        BitWriter w;
        w.ue(0);
        w.ue(5);
        w.ue(0);
        w.u(6, 1);
        w.u(1, 1);
        w.ue(minus1);
        w.u(8, 0xFF);
        std::vector<uint8_t> r = w.b;
        r.resize(2);
        return nal(0x41, r);
    };
    const char* kHeader = "unsupported/truncated slice header or unnegotiated deblocking";
    check("P override ue cut in its suffix", bad(0, kOvr, "num_ref_idx_l0_active_minus1", 0, 0, "truncated"),
          run({cat({spsNal(fn6), ppsNal(PpsF{}), pSliceCut(1)})}, o1, "P override ue cut in its suffix"));
    check("P override ue cut in its zeros", bad(0, kHeader, "num_ref_idx_l0_active_minus1", 0, 0, "truncated"),
          run({cat({spsNal(fn6), ppsNal(PpsF{}), pSliceCut(3)})}, o1, "P override ue cut in its zeros"));

    // ---- NVENC refs (NO-REQUIRED-PROFILE 1011): the prod one-reference core's sps_parser.sv refuses
    // max_num_ref_frames > 1 (ST_REFS), NAL HRD and pic_struct_present (ST_NAL_HRD / ST_PIC_STRUCT), so a declared
    // second reference is NOT accepted by the daemon on a MAX_REF_FRAMES 1 core (a relaxed gate would hand the core an
    // SPS it fails on: reader fatal in the prod-core sim). Real home-PMS h264_nvenc captures (tests/fixtures/nvenc_refs).
    {
        const char* mnrf_nv = "max_num_ref_frames";
        const auto h2 = splitAud(root + "/tests/fixtures/nvenc_refs/h2_nvenc_refs2_10f.264");
        const auto h1 = splitAud(root + "/tests/fixtures/nvenc_refs/h1_nvenc_refs3_3f.264");
        expect(h2.size() == 10 && h1.size() == 3, "NV0 NVENC captures split into 10 and 3 AUs");
        check("NV1 H2 NVENC max_num_ref_frames 2 on a one-reference core: OUT", out(0, kSps, mnrf_nv, 2, 1),
              run(h2, optsFor(v1), "NV1"));
        check("NV3 H1 NVENC max_num_ref_frames 3, v1: OUT", out(0, kSps, mnrf_nv, 3, 1), run(h1, optsFor(v1), "NV3"));
        // VUI is parsed past colour on every core; NAL HRD / pic_struct are OUT without spsRelaxNvenc,
        // so a MAX_REF_FRAMES 2/3 CAPS grant alone no longer takes the NVENC SPS.
        const char* nhrd = "nal_hrd_parameters_present_flag";
        check("NV4 H2 under MAX_REF_FRAMES 2, no relax: OUT at NAL HRD", out(0, kSps, nhrd, 1, 0),
              run(h2, optsFor(r2), "NV4"));
        check("NV5 H1 under MAX_REF_FRAMES 3, no relax: OUT at NAL HRD", out(0, kSps, nhrd, 1, 0),
              run(h1, optsFor(r3), "NV5"));
    }

    // ---- SPS relax row 0x48C5385C (spsRelaxNvenc; c950 analog v10). CAPS is the v1 line (maxRefs
    // 1, no grants): G9 takes a declared max_num_ref_frames <= 2, the VUI may carry NAL/VCL HRD and pic_struct, and
    // max_dec_frame_buffering <= 2. Active refs stay <= 1 (G18/G26) and RPLM/MMCO stay refused.
    {
        auto relaxOpts = [&](const gate::GateCaps& g) {
            auto o = optsFor(g);
            o.spsRelaxNvenc = true;
            return o;
        };
        const auto ox = relaxOpts(v1);
        const auto o1 = optsFor(v1);
        expect(!misterplex::AvInprocOpenOpts{}.spsRelaxNvenc, "AvInprocOpenOpts default spsRelaxNvenc false");
        const char* mnrf = "max_num_ref_frames";
        const auto h2 = splitAud(root + "/tests/fixtures/nvenc_refs/h2_nvenc_refs2_10f.264");
        const auto h1 = splitAud(root + "/tests/fixtures/nvenc_refs/h1_nvenc_refs3_3f.264");
        const Want spsOk = bad(0, kAu, "pictures_per_access_unit", 0, 1, "no picture");
        check("RX5 H2 10 AUs, relax: IN", kIn, run(h2, ox, "RX5"));
        check("RX7 H1 3 AUs, relax: OUT refs 3/2", out(0, kSps, mnrf, 3, 2), run(h1, ox, "RX7"));
        check("RX9 H2 10 AUs, v1: OUT refs 2/1", out(0, kSps, mnrf, 2, 1), run(h2, o1, "RX9"));
        int rowsSeen = 0, h2Rows = 0;
        for (const misterplex::FpgaBuildInfo& b : misterplex::kRecognisedFpgaBuilds) {
            auto o = optsFor(v1);
            o.spsRelaxNvenc = b.spsRelaxNvenc;
            const std::string id = std::to_string(b.id);
            const Got g2 = run(h2, o, "RX10 H2 row " + id);
            const Got g1 = run(h1, o, "RX10 H1 row " + id);
            const bool h2In = g2.au < 0;
            h2Rows += h2In;
            const bool relaxRow = b.spsRelaxNvenc;
            expect(h2In == relaxRow, "RX10 H2 on row " + id + ": " + show(g2));
            expect(g1.au == 0 && g1.v.outcome == StreamOutcome::Out && std::strcmp(g1.v.field, mnrf) == 0,
                   "RX10 H1 on row " + id + " refused at max_num_ref_frames: " + show(g1));
            ++rowsSeen;
        }
        expect(rowsSeen > 20 && h2Rows == 5,
               "RX10 H2 IN on the five spsRelaxNvenc rows, got " + std::to_string(h2Rows));

        auto spsAu = [&](const std::function<void(SpsF&)>& edit) {
            SpsF f;
            f.refs = 2;
            f.vui = 1;
            f.timing = 1;
            f.nalHrd = 1;
            f.picStruct = 1;
            f.restriction = 1;
            f.decBuf = 2;
            edit(f);
            return std::vector<std::vector<uint8_t>>{cat({spsNal(f), ppsNal(PpsF{})})};
        };
        auto none = [](SpsF&) {};
        check("RX11 synth H2-shape, relax: pass", spsOk, run(spsAu(none), ox, "RX11"));
        check("RX12 synth H2-shape refs 1, v1: OUT NAL HRD", out(0, kSps, "nal_hrd_parameters_present_flag", 1, 0),
              run(spsAu([](SpsF& f) { f.refs = 1; }), o1, "RX12"));
        check("RX13 VCL HRD, v1: OUT", out(0, kSps, "vcl_hrd_parameters_present_flag", 1, 0),
              run(spsAu([](SpsF& f) { f.refs = 1; f.nalHrd = 0; f.vclHrd = 1; }), o1, "RX13"));
        check("RX14 VCL HRD, relax: pass", spsOk, run(spsAu([](SpsF& f) { f.vclHrd = 1; }), ox, "RX14"));
        check("RX15 pic_struct, v1: OUT", out(0, kSps, "pic_struct_present_flag", 1, 0),
              run(spsAu([](SpsF& f) { f.refs = 1; f.nalHrd = 0; f.decBuf = 1; }), o1, "RX15"));
        check("RX16 decbuf 2, v1: OUT 2/1", out(0, kSps, "max_dec_frame_buffering", 2, 1),
              run(spsAu([](SpsF& f) { f.refs = 1; f.nalHrd = 0; f.picStruct = 0; }), o1, "RX16"));
        check("RX17 decbuf 1, v1: pass", spsOk,
              run(spsAu([](SpsF& f) { f.refs = 1; f.nalHrd = 0; f.picStruct = 0; f.decBuf = 1; }), o1, "RX17"));
        check("RX18 decbuf 3, relax: OUT 3/2", out(0, kSps, "max_dec_frame_buffering", 3, 2),
              run(spsAu([](SpsF& f) { f.decBuf = 3; }), ox, "RX18"));
        check("RX19 reorder 1, relax: OUT", out(0, kSps, "max_num_reorder_frames", 1, 0),
              run(spsAu([](SpsF& f) { f.reorder = 1; }), ox, "RX19"));
        check("RX26 refs 3, relax: OUT 3/2", out(0, kSps, mnrf, 3, 2),
              run(spsAu([](SpsF& f) { f.refs = 3; f.decBuf = 2; }), ox, "RX26"));
    }

    std::printf("test_compressed_syntax: %s (%d checks, %d failed; corpus %d rows x 3 caps)\n",
                fails ? "FAIL" : "OK", checks, fails, corpus);
    return fails ? 1 : 0;
}
