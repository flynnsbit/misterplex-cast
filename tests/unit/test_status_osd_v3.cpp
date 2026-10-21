// OSD-MENU-SPEC-1002 status layout v3: daemon status writes must carry the
// live OSD bytes 12 ([103:96] H/V position) and 15 ([127:120] Bob, AR, field
// order, 16:9 sources, top-swap) so they never reset the user's settings.
#include "fpga_spi.hpp"
#include "libmisterplex/status_telemetry.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using misterplex::FpgaSpi;
namespace st = misterplex::status_telemetry;

namespace {

void expect(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL status OSD v3: %s\n", msg);
        std::exit(1);
    }
}

bool bitOf(const uint8_t w[16], int bit) { return (w[bit / 8] >> (bit % 8)) & 1u; }

// User: H +7, V +1, AR [ARC1], Field order Inverted, 16:9 Crop; daemon top-swap on.
constexpr uint8_t kLivePos = (7u << 4) | (1u << 1);                          // 0x72
constexpr uint8_t kLiveFlags = 0x02 | 0x08 | 0x10 | 0x40;                     // 0x5A

void liveWord(uint8_t live[16]) {
    std::memset(live, 0, 16);
    live[0] = 0x40; // [6] OSD bit, echoed in status_in [15:0]
    live[1] = 0x00;
    live[st::kOsdPositionByte] = kLivePos;
    live[st::kOsdFlagsByte] = kLiveFlags;
}

void testFlushKeepsOsd() {
    // Fresh daemon process: private shadow is zero. flushAudioFifo pulses [16].
    const uint8_t shadow[16]{};
    uint8_t live[16];
    liveWord(live);
    uint8_t w[16];
    FpgaSpi::composeStatusBit(w, shadow, live, 16, 1);
    expect(bitOf(w, 16), "flush on sets [16]");
    expect(w[st::kOsdPositionByte] == kLivePos, "flush on keeps H/V position");
    expect(w[st::kOsdFlagsByte] == kLiveFlags, "flush on keeps [127:120]");
    expect(w[0] == 0 && w[1] == 0, "bit>=16 keeps shadow [15:0] (pre-v3 rule)");
    // Second half of the pulse starts from the shadow setStatusWord adopted.
    uint8_t w2[16];
    FpgaSpi::composeStatusBit(w2, w, live, 16, 0);
    expect(!bitOf(w2, 16), "flush off clears [16]");
    expect(w2[st::kOsdPositionByte] == kLivePos, "flush off keeps H/V position");
    expect(w2[st::kOsdFlagsByte] == kLiveFlags, "flush off keeps [127:120]");
    // Same pulse with no live read keeps the pre-v3 behaviour (shadow only).
    uint8_t w3[16];
    FpgaSpi::composeStatusBit(w3, shadow, nullptr, 16, 1);
    expect(bitOf(w3, 16) && w3[st::kOsdPositionByte] == 0 && w3[st::kOsdFlagsByte] == 0,
           "no live read: shadow plus bit");
}

void testLowWordRules() {
    uint8_t shadow[16]{};
    shadow[1] = 0x20;
    uint8_t live[16];
    liveWord(live);
    live[0] |= 0x01; // reset echo must not be re-raised by an unrelated kick
    uint8_t w[16];
    FpgaSpi::composeStatusBit(w, shadow, live, 11, 1);
    expect(w[0] == 0x40, "bit<16 takes live [7:0] with [0] cleared");
    expect(w[1] == 0x08, "bit<16 takes live [15:8] and sets the bit");
    expect(w[st::kOsdPositionByte] == kLivePos && w[st::kOsdFlagsByte] == kLiveFlags,
           "bit<16 keeps OSD bytes");
}

void testSetBitsKeepsOsd() {
    // Daemon cadence top-swap write [126]=1 must keep [123]/[124] and H/V.
    uint8_t shadow[16]{};
    uint8_t live[16];
    liveWord(live);
    live[st::kOsdFlagsByte] = static_cast<uint8_t>(kLiveFlags & ~0x40);
    uint8_t w[16];
    bool pulse0 = true;
    const int swap_on[] = {126, 1};
    expect(FpgaSpi::composeStatusBits(w, shadow, live, swap_on, 1, pulse0), "[126] write ok");
    expect(!pulse0, "[126] write does not raise reset");
    expect(bitOf(w, 126) && bitOf(w, 123) && bitOf(w, 124), "[126] keeps [123]/[124]");
    expect(((w[st::kOsdFlagsByte] >> 1) & 3) == 1, "[126] keeps AR");
    expect(w[st::kOsdPositionByte] == kLivePos, "[126] keeps H/V position");
    // The media_player park write (bits 10/11 style pulses) keeps the OSD too.
    const int park[] = {5, 1, 4, 0};
    expect(FpgaSpi::composeStatusBits(w, shadow, live, park, 2, pulse0), "park write ok");
    expect(w[st::kOsdPositionByte] == kLivePos &&
               w[st::kOsdFlagsByte] == static_cast<uint8_t>(kLiveFlags & ~0x40),
           "park keeps OSD bytes");
    expect(!bitOf(w, 0) && !bitOf(w, 10) && !bitOf(w, 11), "sticky pulses cleared");
}

void testRequestedBitsWin() {
    uint8_t shadow[16]{};
    uint8_t live[16];
    liveWord(live);
    uint8_t w[16];
    bool pulse0 = false;
    const int pairs[] = {126, 0, 123, 0, 100, 0, 101, 0, 102, 0, 97, 1};
    expect(FpgaSpi::composeStatusBits(w, shadow, live, pairs, 6, pulse0), "override ok");
    expect(!bitOf(w, 126) && !bitOf(w, 123), "requested [126]/[123] clear wins over live");
    expect(bitOf(w, 124), "unrequested [124] still live");
    const auto o = st::decodeOsdStatusV3(w);
    expect(o.h_position == 0 && o.v_position == 1, "requested H bits win, V stays live");
    uint8_t w1[16];
    FpgaSpi::composeStatusBit(w1, shadow, live, 126, 0);
    expect(!bitOf(w1, 126) && bitOf(w1, 123), "setStatusBit request wins over live");
    const int bad[] = {128, 1};
    expect(!FpgaSpi::composeStatusBits(w, shadow, live, bad, 1, pulse0), "bit 128 rejected");
    const int rst[] = {0, 1};
    expect(FpgaSpi::composeStatusBits(w, shadow, live, rst, 1, pulse0) && pulse0 && bitOf(w, 0),
           "explicit reset pulse kept");
}

void testDecode() {
    uint8_t raw[16]{};
    raw[st::kOsdPositionByte] = kLivePos;
    raw[st::kOsdFlagsByte] = kLiveFlags | 0x01;
    auto o = st::decodeOsdStatusV3(raw);
    expect(o.h_position == 7 && o.v_position == 1, "decode H+7 V+1");
    expect(o.bob && o.aspect == 1 && o.field_inverted && o.crop_169 && o.top_swap,
           "decode [127:120] flags");
    raw[st::kOsdPositionByte] = 0x88; // H 1000 = -8, V 100 = -4
    raw[st::kOsdFlagsByte] = 0;
    o = st::decodeOsdStatusV3(raw);
    expect(o.h_position == -8 && o.v_position == -4, "decode H-8 V-4");
    expect(!o.bob && o.aspect == 0 && !o.field_inverted && !o.crop_169 && !o.top_swap,
           "decode zero flags");
    raw[st::kOsdPositionByte] = 0xF7; // H -1, V +3, [96] ignored
    o = st::decodeOsdStatusV3(raw);
    expect(o.h_position == -1 && o.v_position == 3, "decode H-1 V+3");
}

bool sameExceptBytes12And15(const FpgaSpi::CoreStatus& a, const FpgaSpi::CoreStatus& b) {
    return a.has_frame == b.has_frame && a.has_audio == b.has_audio &&
           a.has_stream == b.has_stream && a.audio_underrun == b.audio_underrun &&
           a.has_idr == b.has_idr && a.stub_busy == b.stub_busy && a.sps_valid == b.sps_valid &&
           a.pps_valid == b.pps_valid && a.last_nal_type == b.last_nal_type &&
           a.nalu_count == b.nalu_count && a.stream_fifo_level == b.stream_fifo_level &&
           a.idr_count == b.idr_count && a.slice_type == b.slice_type &&
           a.first_mb_type == b.first_mb_type && a.slice_qp == b.slice_qp &&
           a.residual_tc == b.residual_tc && a.residual_t1 == b.residual_t1 &&
           a.residual_ok == b.residual_ok && a.residual_csum == b.residual_csum &&
           a.recon_sig == b.recon_sig && a.ddr_busy == b.ddr_busy &&
           a.swap_pending == b.swap_pending && a.stub_frames == b.stub_frames &&
           a.sps_width == b.sps_width && a.sps_height == b.sps_height &&
           a.wr_count_lo == b.wr_count_lo && a.stream_bytes_seen == b.stream_bytes_seen &&
           a.stream_nalus == b.stream_nalus && a.stream_bytes_in == b.stream_bytes_in;
}

// parseCoreStatus decodes bytes 12/15 by the negotiated build row
// (fpga_build_ids.hpp, V3-FIT-PRECHECK-1003): the user's OSD bytes on the v3
// rows 2400F40B and 4801F40A, telemetry on v2 rows and before CAPS (no row).
void testParseByBuildRow() {
    uint8_t raw[16];
    for (int i = 0; i < 16; ++i)
        raw[i] = static_cast<uint8_t>(0x35 * i + 0x17);
    raw[st::kOsdPositionByte] = kLivePos;          // H +7, V +1
    raw[st::kOsdFlagsByte] = kLiveFlags | 0x01;    // Bob, AR 1, inverted, crop, top-swap
    const FpgaSpi::CoreStatus none = FpgaSpi::parseCoreStatus(raw);
    for (uint32_t id : {0x2400F36Au, 0x4800F36Au}) {
        const FpgaSpi::CoreStatus s = FpgaSpi::parseCoreStatus(raw, misterplex::recognisedFpgaBuild(id));
        expect(!s.osd_byte12 && !s.osd_byte15, "v2 row: bytes 12/15 are telemetry");
        expect(s.residual_dc == static_cast<int8_t>(kLivePos) && s.recon_dbg == (kLiveFlags | 0x01),
               "v2 row: residual_dc / recon_dbg from raw[12] / raw[15]");
        expect(sameExceptBytes12And15(s, none) && s.residual_dc == none.residual_dc &&
                   s.recon_dbg == none.recon_dbg,
               "v2 row parses exactly as no row (pre-CAPS)");
    }
    expect(!none.osd_byte12 && !none.osd_byte15, "no row: bytes 12/15 are telemetry");
    for (uint32_t id : {0x2400F40Bu, 0x4801F40Au}) {
        const misterplex::FpgaBuildInfo* b = misterplex::recognisedFpgaBuild(id);
        expect(b != nullptr, "v3 row is recognised");
        const FpgaSpi::CoreStatus s = FpgaSpi::parseCoreStatus(raw, b);
        expect(s.osd_byte12 && s.osd_byte15, "v3 row: bytes 12/15 are echoed OSD bytes");
        expect(s.residual_dc == 0 && s.recon_dbg == 0, "v3 row: no telemetry from bytes 12/15");
        expect(s.osd.h_position == 7 && s.osd.v_position == 1, "v3 row decodes H+7 V+1 from byte 12");
        expect(s.osd.bob && s.osd.aspect == 1 && s.osd.field_inverted && s.osd.crop_169 &&
                   s.osd.top_swap,
               "v3 row decodes the byte 15 flags");
        expect(sameExceptBytes12And15(s, none), "v3 row: every other field parses as v2");
    }
    // Each row field drives its own byte (synthetic rows; no core ships a mixed layout).
    const misterplex::FpgaBuildInfo only12{0x00000002u, "240p",
                                           misterplex::PlxPageClass::Present480pOrMetaDpb, false,
                                           true, 10, "synthetic"};
    const misterplex::FpgaBuildInfo only15{0x00000001u, "240p",
                                           misterplex::PlxPageClass::Present480pOrMetaDpb, true,
                                           false, 10, "synthetic"};
    FpgaSpi::CoreStatus m = FpgaSpi::parseCoreStatus(raw, &only12);
    expect(m.osd_byte12 && !m.osd_byte15 && m.residual_dc == 0 &&
               m.recon_dbg == (kLiveFlags | 0x01) && m.osd.h_position == 7 && !m.osd.bob,
           "byte12Echoed alone: byte 12 OSD, byte 15 telemetry");
    m = FpgaSpi::parseCoreStatus(raw, &only15);
    expect(!m.osd_byte12 && m.osd_byte15 && m.residual_dc == static_cast<int8_t>(kLivePos) &&
               m.recon_dbg == 0 && m.osd.h_position == 0 && m.osd.bob,
           "byte15Echoed alone: byte 15 OSD, byte 12 telemetry");
    std::printf("parseCoreStatus by build row: 2400F40B and 4801F40A decode bytes 12/15 as OSD "
                "(H+7 V+1, Bob, AR 1, field inverted, 16:9 crop, top-swap); 2400F36A, 4800F36A and "
                "no row decode them as telemetry; all other fields identical\n");
}

// Per-byte replay under each build ID (release 05:52 acceptance 3): every recognised
// row, status_in byte 12 [103:96] and byte 15 [127:120], all 256 values, through the
// real parseCoreStatus. A v3 row (v,10) decodes the byte as the user's OSD setting
// and never as telemetry; a v2 row (v,9) decodes it as telemetry and leaves the OSD
// view zero. Every other field parses exactly as with no row.
void testPerByteReplayByBuildRow() {
    uint8_t base[16];
    for (int i = 0; i < 16; ++i)
        base[i] = static_cast<uint8_t>(0x35 * i + 0x17);
    int osd = 0, telemetry = 0, v3_rows = 0, v2_rows = 0;
    for (const misterplex::FpgaBuildInfo& row : misterplex::kRecognisedFpgaBuilds) {
        const char* layout = misterplex::fpgaStatusLayoutName(row);
        const bool v3 = std::strcmp(layout, "v3") == 0;
        expect(v3 || std::strcmp(layout, "v2") == 0, "per-byte replay: row is v2 or v3");
        ++(v3 ? v3_rows : v2_rows);
        for (int byte : {st::kOsdPositionByte, st::kOsdFlagsByte}) {
            const bool echoed = misterplex::fpgaStatusByteIsOsdEcho(row, byte);
            expect(echoed == v3, "per-byte replay: v3 rows echo bytes 12 and 15, v2 rows neither");
            for (int v = 0; v < 256; ++v) {
                uint8_t raw[16];
                std::memcpy(raw, base, sizeof raw);
                raw[byte] = static_cast<uint8_t>(v);
                const FpgaSpi::CoreStatus none = FpgaSpi::parseCoreStatus(raw);
                const FpgaSpi::CoreStatus s = FpgaSpi::parseCoreStatus(raw, &row);
                const st::OsdStatusV3 ref = st::decodeOsdStatusV3(raw);
                expect(sameExceptBytes12And15(s, none),
                       "per-byte replay: every other field parses as with no row");
                if (byte == st::kOsdPositionByte && echoed)
                    expect(s.osd_byte12 && s.residual_dc == 0 &&
                               s.osd.h_position == ref.h_position &&
                               s.osd.v_position == ref.v_position,
                           "per-byte replay: v3 byte 12 is the OSD H/V position");
                else if (byte == st::kOsdPositionByte)
                    expect(!s.osd_byte12 && s.residual_dc == static_cast<int8_t>(v) &&
                               s.osd.h_position == 0 && s.osd.v_position == 0,
                           "per-byte replay: v2 byte 12 is residual_dc telemetry");
                else if (echoed)
                    expect(s.osd_byte15 && s.recon_dbg == 0 && s.osd.bob == ref.bob &&
                               s.osd.aspect == ref.aspect &&
                               s.osd.field_inverted == ref.field_inverted &&
                               s.osd.crop_169 == ref.crop_169 && s.osd.top_swap == ref.top_swap,
                           "per-byte replay: v3 byte 15 is the OSD flags byte");
                else
                    expect(!s.osd_byte15 && s.recon_dbg == v && !s.osd.bob && s.osd.aspect == 0 &&
                               !s.osd.field_inverted && !s.osd.crop_169 && !s.osd.top_swap,
                           "per-byte replay: v2 byte 15 is recon_dbg telemetry");
                ++(echoed ? osd : telemetry);
            }
        }
    }
    for (uint32_t id : {0x4801F40Au, 0x2400F40Bu}) {
        const misterplex::FpgaBuildInfo* b = misterplex::recognisedFpgaBuild(id);
        expect(b && b->byte12Echoed && b->byte15Echoed && b->confVersion == 10,
               "4801F40A / 2400F40B are v3 (1/1/10)");
    }
    const misterplex::FpgaBuildInfo* p240 = misterplex::recognisedFpgaBuild(0x2400F36Au);
    expect(p240 && !p240->byte12Echoed && !p240->byte15Echoed && p240->confVersion == 9,
           "2400F36A is v2 (0/0/9)");
    std::printf("per-byte replay by build row: %d rows (%d v3: 2400F40B 4801F40A; %d v2) x bytes "
                "12/15 x 256 values = %d OSD-echo + %d telemetry decodes; other fields identical "
                "to no row\n",
                v3_rows + v2_rows, v3_rows, v2_rows, osd, telemetry);
}

void testKickCarriesOsd() {
    // A later SPI kick: the shadow holds the previous kick's OSD copy, and the
    // user has since moved H/V and toggled field order in Main's OSD.
    uint8_t shadow[16]{};
    shadow[1] = 0x20;
    shadow[st::kOsdPositionByte] = 0x10;
    shadow[st::kOsdFlagsByte] = 0x02;
    uint8_t live[16];
    liveWord(live);
    uint8_t w[16];
    FpgaSpi::composeKickBase(w, shadow, live, false);
    expect(w[st::kOsdPositionByte] == kLivePos && w[st::kOsdFlagsByte] == kLiveFlags,
           "later kick carries the live OSD bytes, not the previous kick's");
    expect(w[0] == 0 && w[1] == 0x20, "later kick keeps shadow [15:0] (pre-v3 rule)");
    FpgaSpi::composeKickBase(w, shadow, live, true);
    expect(w[0] == 0x40 && w[1] == 0x00, "first kick takes live [15:0]");
    expect(w[st::kOsdPositionByte] == kLivePos && w[st::kOsdFlagsByte] == kLiveFlags,
           "first kick carries the live OSD bytes");
    FpgaSpi::composeKickBase(w, shadow, nullptr, false);
    expect(std::memcmp(w, shadow, 16) == 0, "kick with no live read: the shadow");
}

// The writers' word composition at f6526d96 (prod 8d8faaf1), unchanged through
// dd0bcb08: kickDdrSpi fpga_spi.cpp:1113-1122, setStatusBit :1357-1383,
// setStatusBits :1385-1440 (before the usleep).
void setBit(uint8_t w[16], int bit, int value) {
    if (value)
        w[bit / 8] = static_cast<uint8_t>(w[bit / 8] | (1u << (bit % 8)));
    else
        w[bit / 8] = static_cast<uint8_t>(w[bit / 8] & ~(1u << (bit % 8)));
}

void v2StatusBit(uint8_t w[16], const uint8_t shadow[16], const uint8_t* live, int bit,
                 int value) {
    std::memcpy(w, shadow, 16);
    if (bit < 16 && live) {
        w[0] = live[0];
        w[1] = live[1];
        if (bit != 0)
            w[0] = static_cast<uint8_t>(w[0] & ~0x01);
    }
    setBit(w, bit, value);
}

bool v2StatusBits(uint8_t w[16], const uint8_t shadow[16], const uint8_t* live, const int* pairs,
                  int n, bool& want_pulse0) {
    std::memcpy(w, shadow, 16);
    if (live) {
        if ((live[0] | (live[1] << 8)) != 0) {
            w[0] = live[0];
            w[1] = live[1];
        }
        if (live[15] & 0x06)
            w[15] = static_cast<uint8_t>((w[15] & ~0x06) | (live[15] & 0x06));
    }
    want_pulse0 = false;
    bool want10 = false, want11 = false;
    for (int i = 0; i < n; ++i) {
        const int bit = pairs[i * 2], value = pairs[i * 2 + 1] ? 1 : 0;
        if (bit < 0 || bit > 127)
            return false;
        want_pulse0 |= bit == 0 && value;
        want10 |= bit == 10 && value;
        want11 |= bit == 11 && value;
        setBit(w, bit, value);
    }
    if (!want_pulse0)
        w[0] = static_cast<uint8_t>(w[0] & ~0x01);
    if (!want10)
        w[1] = static_cast<uint8_t>(w[1] & ~0x04);
    if (!want11)
        w[1] = static_cast<uint8_t>(w[1] & ~0x08);
    return true;
}

void v2KickBase(uint8_t w[16], const uint8_t shadow[16], const uint8_t* live, bool first_verify) {
    std::memcpy(w, shadow, 16);
    if (first_verify && live) {
        w[0] = live[0];
        w[1] = live[1];
    }
}

uint32_t rng = 0x1002u;
uint32_t rnd() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// Every byte but 12 and 15 equals the f6526d96 word; 12 and 15 are the live
// echo under the caller's own bits (or the f6526d96 word when the read failed).
// changed[i] counts the cases where byte i differs from f6526d96.
void checkAgainstV2(const uint8_t v3[16], const uint8_t v2[16], const uint8_t* live,
                    const uint8_t own[16], unsigned changed[16], const char* writer) {
    for (int i = 0; i < 16; ++i) {
        uint8_t want = v2[i];
        if (live && (i == st::kOsdPositionByte || i == st::kOsdFlagsByte))
            want = static_cast<uint8_t>((live[i] & ~own[i]) | (v2[i] & own[i]));
        if (v3[i] != want) {
            std::fprintf(stderr, "FAIL status OSD v3: %s byte %d = 0x%02x, want 0x%02x\n", writer,
                         i, v3[i], want);
            std::exit(1);
        }
        if (v3[i] != v2[i])
            ++changed[i];
    }
}

void testOnlyOsdBytesChangeVsV2() {
    constexpr int kCases = 200000;
    unsigned changed[3][16]{};
    const char* names[3] = {"setStatusBit", "setStatusBits", "kickDdrSpi"};
    for (int c = 0; c < kCases; ++c) {
        uint8_t shadow[16], liveBuf[16], own[16]{}, v2[16], v3[16];
        for (int i = 0; i < 16; ++i) {
            shadow[i] = static_cast<uint8_t>(rnd());
            liveBuf[i] = static_cast<uint8_t>(rnd());
        }
        if (rnd() % 4 == 0)
            liveBuf[0] = liveBuf[1] = 0; // setStatusBits keeps the shadow [15:0]
        if (rnd() % 4 == 0)
            liveBuf[15] = static_cast<uint8_t>(liveBuf[15] & ~0x06); // AR 0
        const uint8_t* live = (rnd() % 8 == 0) ? nullptr : liveBuf; // failed read
        switch (c % 3) {
        case 0: {
            const int bit = static_cast<int>(rnd() % 128), value = static_cast<int>(rnd() & 1);
            v2StatusBit(v2, shadow, live, bit, value);
            FpgaSpi::composeStatusBit(v3, shadow, live, bit, value);
            setBit(own, bit, 1);
            break;
        }
        case 1: {
            int pairs[12];
            const int n = 1 + static_cast<int>(rnd() % 6);
            for (int i = 0; i < n; ++i) {
                // Half the bits land in the OSD bytes, so own bits are exercised.
                pairs[i * 2] = (rnd() & 1) ? static_cast<int>(96 + rnd() % 32)
                                            : static_cast<int>(rnd() % 128);
                pairs[i * 2 + 1] = static_cast<int>(rnd() & 1);
                setBit(own, pairs[i * 2], 1);
            }
            bool p2 = false, p3 = false;
            expect(v2StatusBits(v2, shadow, live, pairs, n, p2) &&
                       FpgaSpi::composeStatusBits(v3, shadow, live, pairs, n, p3) && p2 == p3,
                   "setStatusBits compose");
            break;
        }
        default: {
            const bool first = rnd() & 1;
            v2KickBase(v2, shadow, live, first);
            FpgaSpi::composeKickBase(v3, shadow, live, first);
            break;
        }
        }
        checkAgainstV2(v3, v2, live, own, changed[c % 3], names[c % 3]);
    }
    for (int k = 0; k < 3; ++k) {
        std::printf("status bytes changed vs f6526d96 (%s, %d cases):", names[k],
                    (kCases + 2 - k) / 3);
        for (int i = 0; i < 16; ++i) {
            if (changed[k][i])
                std::printf(" byte%d=%u", i, changed[k][i]);
            expect(!changed[k][i] || i == st::kOsdPositionByte || i == st::kOsdFlagsByte,
                   "a byte other than 12/15 changed vs f6526d96");
        }
        std::printf("\n");
    }
}

} // namespace

int main() {
    testFlushKeepsOsd();
    testLowWordRules();
    testSetBitsKeepsOsd();
    testRequestedBitsWin();
    testDecode();
    testParseByBuildRow();
    testPerByteReplayByBuildRow();
    testKickCarriesOsd();
    testOnlyOsdBytesChangeVsV2();
    std::printf("PASS status OSD v3 (live bytes 12/15 survive daemon writes)\n");
    return 0;
}
