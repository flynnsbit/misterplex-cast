// T-SPS-GATE-FALLBACK pre-reg v1.4.3 (b57a87fb) section 1: unit teeth T11a-g + the v1 no-regression rows for
// gate::evaluateCaps (host/libmisterplex/gate_caps.hpp). Pure, no daemon, no fake core.
#include "libmisterplex/gate_caps.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

using namespace misterplex;
namespace ring = misterplex::ddr_bitstream_ring;
using gate::CapsOutcome;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL gate caps: %s\n", msg.c_str());
        ++fails;
    }
}

// The prod 0x48005D52 CAPS line (tl-1718 live-prod-restore caps.txt 1f3116b4): MPC1 0x0000E1BF, MPC2 0x01E00280
// (640x480), MPC3 0x00020000.
ring::VideoCapabilities prodCaps() {
    ring::VideoCapabilities c;
    c.abi_version = mailbox_abi::kFpgaVideoAbiVersion;
    c.layout_id = mailbox_abi::kFpgaVideoLayoutId;
    c.features = 0x0000E1BFu;
    c.max_width = 640;
    c.max_height = 480;
    c.max_au_bytes = 131072;
    c.build_id = 0x48005D52u;
    c.nonce = 0x7E8A252E8C9A0691ull;
    c.publication = 1;
    return c;
}

const char* name(CapsOutcome o) {
    return o == CapsOutcome::Ok ? "Ok" : o == CapsOutcome::FallbackOnFloor ? "FallbackOnFloor" : "NoPlay";
}

void expectV1(const gate::GateCaps& g, bool inter, bool deblock, const std::string& who) {
    expect(g.maxRefs == 1 && g.maxLevelIdc == 30 && !g.pPart && !g.pSub8x8 && !g.cabac && !g.bSlices &&
               !g.transform8x8 && !g.weightedPred && !g.interlace && !g.spsRelax,
           who + ": v1 limits (refs 1, L3.0, no grants)");
    expect(g.inter == inter, who + ": inter " + std::to_string(g.inter));
    expect(g.deblock == deblock, who + ": deblock " + std::to_string(g.deblock));
}

} // namespace

int main() {
    // v1 = today's inspect limits on every core of record (a1: MPC1[31:16] = 0).
    {
        const auto v = gate::evaluateCaps(prodCaps());
        expect(v.outcome == CapsOutcome::Ok, std::string("prod 0xE1BF: Ok, got ") + name(v.outcome));
        expectV1(v.caps, true, false, "prod 0xE1BF");
        expect(v.caps.maxWidth == 640 && v.caps.maxHeight == 480 && v.caps.maxAuBytes == 131072, "prod limits");
    }
    for (uint32_t f : {0x0000E1BFu /* 9E36 */, 0x0000E1FFu /* C88F */, 0x0000E19Fu /* static IDR */}) {
        auto c = prodCaps();
        c.features = f;
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::Ok, "v1 word " + gate::detail::hex32(f) + " is Ok");
        expectV1(v.caps, (f & ring::Inter) != 0, (f & ring::Deblock) != 0, "v1 word " + gate::detail::hex32(f));
    }
    // MAX_REF_FRAMES [18:16]: 0 = absent = 1; 3 with Inter = 3.
    {
        auto c = prodCaps();
        c.features |= 3u << gate::kMaxRefFramesShift;
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::Ok && v.caps.maxRefs == 3, "refs field 3 with Inter -> maxRefs 3");
    }
    // T11a: an unknown bit in [31:16] (reserved bit 30) -> hard error -> fallback; the floor trusts MPC1[15:0].
    {
        auto c = prodCaps();
        c.features |= 1u << 30;
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::FallbackOnFloor, std::string("T11a: fallback, got ") + name(v.outcome));
        expect(v.unknownMask == 0x40000000u, "T11a: mask names bit 30, got " + gate::detail::hex32(v.unknownMask));
        expect(v.detail.find("0x40000000") != std::string::npos, "T11a: detail names the mask: " + v.detail);
        expectV1(v.caps, true, false, "T11a floor");
    }
    // T11b: an unlisted maxLevel code (5) -> fallback with the code in the detail.
    {
        auto c = prodCaps();
        c.features |= 5u << gate::kMaxLevelShift;
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::FallbackOnFloor, std::string("T11b: fallback, got ") + name(v.outcome));
        expect(v.detail.find("unlisted maxLevel 5") != std::string::npos, "T11b: detail: " + v.detail);
        for (uint32_t code = 0; code <= 4; ++code) {
            auto k = prodCaps();
            k.features |= code << gate::kMaxLevelShift;
            const auto w = gate::evaluateCaps(k);
            expect(w.outcome == CapsOutcome::Ok && w.caps.maxLevelIdc == gate::maxLevelIdcForCode(code),
                   "listed maxLevel code " + std::to_string(code) + " is Ok");
        }
    }
    // T11c/d: an unknown bit in [15:0] (driven through the knownLowMask seam: every 16 low bits are defined at this
    // tree) -> fallback on the hard-coded constants: no Inter, no Deblock (all-IDR helper only).
    {
        auto c = prodCaps();
        c.features |= ring::Deblock;  // a low bit the narrowed mask does not know
        const uint32_t narrowed = gate::kEnumKnownLowMask & ~ring::Deblock;
        const auto v = gate::evaluateCaps(c, narrowed);
        expect(v.outcome == CapsOutcome::FallbackOnFloor, std::string("T11c: fallback, got ") + name(v.outcome));
        expect(v.unknownMask == ring::Deblock, "T11c: mask is the unknown low bit");
        expectV1(v.caps, false, false, "T11c/d constants floor");
        expect(gate::kEnumKnownLowMask == 0xFFFFu, "every VideoFeature bit 0-15 is known at this tree");
    }
    // T11e: MPC2 passes supportsVideo but fails (i) -> NoPlay, and the detail names (i), not supportsVideo.
    for (auto wh : {std::pair<uint16_t, uint16_t>{4096, 480}, std::pair<uint16_t, uint16_t>{640, 1088}}) {
        auto c = prodCaps();
        c.max_width = wh.first;
        c.max_height = wh.second;
        expect(c.supportsVideo(), "T11e precondition: supportsVideo true for " + std::to_string(wh.first) + "x" +
                                      std::to_string(wh.second));
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::NoPlay, std::string("T11e: NoPlay, got ") + name(v.outcome));
        expect(v.detail.find("(i): MPC2") != std::string::npos, "T11e: detail names (i): " + v.detail);
    }
    // T11f: supportsVideo false (a required feature bit missing) -> NoPlay, no fallback.
    {
        auto c = prodCaps();
        c.features &= ~ring::PresentationFeedback;
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::NoPlay && v.detail.find("supportsVideo false") != std::string::npos,
               std::string("T11f: NoPlay on supportsVideo, got ") + name(v.outcome) + " " + v.detail);
    }
    // T11g (a2): a grant without its prerequisite -> (iv) fallback; the floor keeps Inter clear.
    {
        auto c = prodCaps();
        c.features = 0x0000E19Fu | gate::kPPart;  // no Inter, PPart granted
        const auto v = gate::evaluateCaps(c);
        expect(v.outcome == CapsOutcome::FallbackOnFloor, std::string("T11g: fallback, got ") + name(v.outcome));
        expect(v.unknownMask == gate::kPPart, "T11g: mask names PPart");
        expect(!v.caps.inter && !v.caps.pPart, "T11g: the floor never enables Inter or PPart");
        auto r = prodCaps();
        r.features = 0x0000E19Fu | (2u << gate::kMaxRefFramesShift);  // refs 2 without Inter
        expect(gate::evaluateCaps(r).outcome == CapsOutcome::FallbackOnFloor, "T11g: refs > 1 without Inter -> (iv)");
    }
    if (fails) {
        std::fprintf(stderr, "test_gate_caps: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_gate_caps: OK (v1 x4, refs, T11a-g)\n");
    return 0;
}
