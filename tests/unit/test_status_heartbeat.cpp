// Status block heartbeat (status byte 6 = H; OSD-MENU-SPEC-1002 section 9, the status-byte-H lab note).
// Ported from the staged on-demand line (1cb004e5) to the canon daemon line, where the core gate, not the
// negotiated play row plus a CORENAME stamp, decides when the status block is live. Covers:
// - the H bit layout, fps classes and clean-stop value (verbatim);
// - the heartbeat word (verbatim): H[6:0] from the daemon, H[7] from the live echo, the live OSD bytes
//   12/15 and [15:0] kept, pulse bits [0]/[10]/[11] cleared, nothing else moved;
// - the row rule (FpgaSpi::statusBlockRow):
//   - only rows with statusBlock pass, and those are exactly kStatusBlockRows;
//   - the lab force passes every recognised row; no row never passes;
//   - a synthetic statusBlock row passes;
// - the gate: with the core gate closed (a fresh process), writeStatusHeartbeat returns before touching SPI,
//   forced or not;
// - the gate open (through CoreGate's own probe path): on the statusBlock row the write passes the gate; a
//   reprogram that closes the gate (buildId kept) stops it; an allowlisted raster change re-pairs and opens
//   (status block follows the new row); a CAPS refusal stays Rejected and the status block follows
//   lastCapsId (active on F1 24003997, refused on 4801F40A); on every bundle row without statusBlock
//   (4800C88F, 48003ECB, 2400F40B, 4801F40A) the write is refused before SPI unless the lab force is set;
// - the MPX_STATUS_BLOCK_FORCE env parse (verbatim);
// - parseCoreStatus still reads byte 6 as first_mb_type (only a stat).
#include "fpga_spi.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/status_heartbeat.hpp"
#include "libmisterplex/status_telemetry.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using misterplex::FpgaSpi;
namespace hb = misterplex::status_heartbeat;
namespace st = misterplex::status_telemetry;

namespace {

// The build rows that carry statusBlock (fpga_build_ids.hpp): the 480p Status-block builds on the C88F line
// (Track B, 0x480058BC; T-DPP1-STATUS joint, 0x48018AE0), F1 240p 0x24003997, the High core 0x48A64AED
// (Plex_high.qsf MPX_STATUS_BLOCK), and the NVENC refits that keep their parent's statusBlock
// (0x48110015 from 0x487A1BD1, 0x24A10026 from 0x24F1A2C4). 480i 0x4801F40A stays off.
const std::vector<uint32_t> kStatusBlockRows = {0x480058BCu, 0x48018AE0u, 0x48FF65F2u, 0x487A1BD1u,
                                                0x48C5385Cu, 0x48C51FF1u, 0x48CFEE31u, 0x48A64AEDu, 0x48110015u,
                                                0x24003997u, 0x24F1A2C4u, 0x24A10026u, 0x48F14A0Bu};

int fails = 0;
void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL status heartbeat: %s\n", msg.c_str());
        ++fails;
    }
}

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

void testLayout() {
    expect(hb::kByte == 6 && hb::kFirstBit == 48, "H is status byte 6 = bits [55:48]");
    expect(hb::kByte != st::kOsdPositionByte && hb::kByte != st::kOsdFlagsByte,
           "H is never byte 12 or 15 (OSD-owned under v,10)");
    expect(hb::kByte >= 2 && hb::kByte <= 11, "H is in telem[95:16]");
    expect(hb::compose(true, hb::Server::Unknown, false, 0) == 0x01, "H[0] toggle");
    expect(hb::compose(false, hb::Server::Unreachable, false, 0) == 0x02, "H[2:1] = 1 Unreachable");
    expect(hb::compose(false, hb::Server::Connected, false, 0) == 0x04, "H[2:1] = 2 Connected");
    expect(hb::compose(false, hb::Server::Pairing, false, 0) == 0x06, "H[2:1] = 3 Pairing");
    expect(hb::compose(false, hb::Server::Unknown, true, 0) == 0x08, "H[3] Casting");
    expect(hb::compose(false, hb::Server::Unknown, false, 7) == 0x70, "H[6:4] fps 7");
    expect(hb::compose(true, hb::Server::Connected, true, 4) == 0x4D, "Connected, Casting, 29.97");
    for (int t = 0; t < 2; ++t)
        for (int s = 0; s < 4; ++s)
            for (int c = 0; c < 2; ++c)
                for (int f = 0; f < 8; ++f)
                    expect((hb::compose(t, static_cast<hb::Server>(s), c, static_cast<uint8_t>(f)) &
                            hb::kRunning) == 0,
                           "the daemon never sets H[7] (core-driven Running)");
    struct { int n, d; uint8_t c; } fps[] = {
        {24000, 1001, 1}, {24, 1, 2}, {25, 1, 3}, {30000, 1001, 4}, {30, 1, 5}, {50, 1, 6},
        {60000, 1001, 7}, {48000, 2002, 1}, {0, 0, 0}, {60, 1, 0}, {48, 1, 0}, {-1, 1, 0}};
    for (const auto& f : fps)
        expect(hb::fpsClass(f.n, f.d) == f.c,
               "fps class " + std::to_string(f.n) + "/" + std::to_string(f.d));
    expect(hb::stopValue(0x4D) == 0x01 && hb::stopValue(0x4C) == 0x00,
           "clean stop: H[6:1] = 0, H[0] kept");
}

void testWord() {
    uint8_t shadow[16], live[16], word[16];
    for (int i = 0; i < 16; ++i) {
        shadow[i] = static_cast<uint8_t>(0x11 * i + 3);
        live[i] = static_cast<uint8_t>(0x5A ^ (0x07 * i));
    }
    shadow[0] |= 0x01;  // stale reset bit in the shadow
    shadow[1] |= 0x0C;  // stale flush pulses [10]/[11]
    for (uint8_t hrun : {uint8_t(0x00), uint8_t(0x80)}) {
        live[hb::kByte] = static_cast<uint8_t>(hrun | 0x2A);
        for (int h = 0; h < 256; ++h) {
            FpgaSpi::composeStatusHeartbeat(word, shadow, live, static_cast<uint8_t>(h));
            expect((word[hb::kByte] & hb::kDaemonBits) == (h & hb::kDaemonBits),
                   "word H[6:0] = the daemon's bits");
            expect((word[hb::kByte] & hb::kRunning) == hrun, "word H[7] = live Running");
            expect(word[st::kOsdPositionByte] == live[st::kOsdPositionByte] &&
                       word[st::kOsdFlagsByte] == live[st::kOsdFlagsByte],
                   "live OSD bytes 12/15 kept");
            expect(word[0] == (live[0] & ~0x01) && word[1] == (live[1] & ~0x0C),
                   "live [15:0], reset and flush pulses cleared");
            const bool pairing = ((h & hb::kServerMask) >> hb::kServerShift) ==
                                 static_cast<uint8_t>(hb::Server::Pairing);
            for (int i = 2; i < 16; ++i) {
                if (i == hb::kByte || i == st::kOsdPositionByte || i == st::kOsdFlagsByte)
                    continue;
                if (i >= 8 && i <= 11) {
                    if (pairing)
                        expect(word[i] == 0, "pairing bytes 8-11 default daemon_id 0");
                    else
                        expect(word[i] == live[i], "play bytes 8-11 live telem");
                    continue;
                }
                expect(word[i] == shadow[i], "byte " + std::to_string(i) + " is the shadow's");
            }
        }
    }
    // Without a live echo H[7] keeps the shadow's value (writeStatusHeartbeat never gets here: it skips the
    // tick when the live read fails).
    shadow[hb::kByte] = 0x80;
    FpgaSpi::composeStatusHeartbeat(word, shadow, nullptr, 0x15);
    expect(word[hb::kByte] == 0x95, "no live: H[7] from the shadow");
    expect(word[8] == shadow[8] && word[11] == shadow[11], "no live, not pairing: bytes 8-11 stay shadow");
    const uint32_t id = 0x48C5385Cu;
    const uint8_t hp = hb::compose(true, hb::Server::Pairing, false, 0);
    FpgaSpi::composeStatusHeartbeat(word, shadow, live, hp, id);
    expect((word[hb::kByte] & hb::kServerMask) == 0x06, "pairing H[2:1]=3");
    expect(word[8] == 0x5C && word[9] == 0x38 && word[10] == 0xC5 && word[11] == 0x48,
           "pairing LE daemon BUILD_ID in bytes 8-11");
    FpgaSpi::composeStatusHeartbeat(word, shadow, live, hb::compose(false, hb::Server::Connected, false, 0));
    expect(word[8] == live[8] && word[9] == live[9] && word[10] == live[10] && word[11] == live[11],
           "play copies live telem over a stale pairing id");
}

void testRowRule() {
    expect(!FpgaSpi::statusBlockRow(nullptr, false) && !FpgaSpi::statusBlockRow(nullptr, true),
           "no row (unknown build): off, forced or not");
    int rows = 0, on = 0;
    for (const misterplex::FpgaBuildInfo& b : misterplex::kRecognisedFpgaBuilds) {
        const bool want = std::find(kStatusBlockRows.begin(), kStatusBlockRows.end(), b.id) !=
                          kStatusBlockRows.end();
        expect(b.statusBlock == want, hex8(b.id) + ": table flag statusBlock=" + (want ? "1" : "0"));
        expect(FpgaSpi::statusBlockRow(&b, false) == b.statusBlock, hex8(b.id) + ": row rule = flag");
        expect(FpgaSpi::statusBlockRow(&b, true), hex8(b.id) + ": forced on");
        ++rows;
        on += b.statusBlock ? 1 : 0;
    }
    expect(on == static_cast<int>(kStatusBlockRows.size()), "every kStatusBlockRows id is a table row");
    misterplex::FpgaBuildInfo future = misterplex::kRecognisedFpgaBuilds[0];
    future.id = 0x00000003u;
    future.statusBlock = true;
    expect(FpgaSpi::statusBlockRow(&future, false), "synthetic statusBlock row: on");
    std::printf("row rule: %d rows, %d with statusBlock; forced passes all; no row never\n", rows, on);
}

void testGate() {
    // A fresh process: the core gate has verified no core yet (closed), so no write may touch SPI.
    FpgaSpi spi;  // never opened: any SPI access would report "not open"
    const bool gateOpen = misterplex::CoreGate::process().open();
    expect(!spi.statusBlockActive(), "gate closed / no row: off");
    expect(!spi.writeStatusHeartbeat(0x4D) && spi.lastError().find("status block:") != std::string::npos &&
               spi.lastError().find("SPI not open") == std::string::npos,
           "refused before SPI (" + spi.lastError() + ")");
    spi.setStatusBlockForce(true);
    expect(!spi.statusBlockActive() && !spi.writeStatusHeartbeat(0x4D) &&
               spi.lastError().find("SPI not open") == std::string::npos,
           "forced, gate closed / no row: still refused before SPI");
    spi.setStatusBlockForce(false);
    std::printf("gate: core gate %s at start, heartbeat refused before SPI (%s)\n",
                gateOpen ? "open" : "closed", spi.lastError().c_str());
}

// Opens the process core gate (the singleton FpgaSpi reads) on `build` through the gate's own path: first look,
// settle, CAPS probe verdict. The observation is test_core_gate.cpp's Sim (live fabric, names written after Main
// and boot).
bool openGateOn(uint32_t build) {
    misterplex::CoreGate& g = misterplex::CoreGate::process();
    g.resetForTest(false);  // managed, closed, no look yet
    misterplex::CoreObservation o;
    o.fabric = misterplex::FabricState{true, misterplex::kFpgaMgrModeUser, false};
    o.main = misterplex::MainIdentity{100, 100};
    o.mainStartWallNs = 500;
    o.bootWallNs = 200;
    o.coreName = misterplex::CoreFileStamp{true, "Plex", 1000};
    o.rbfName = misterplex::CoreFileStamp{true, "Plex_480p_r1", 1000};
    o.nowUs = 5000000000;
    g.observe(o);
    o.nowUs += misterplex::CoreGate::kSettleUs;
    if (g.observe(o) != misterplex::CoreGateEvent::ProbeWanted)
        return false;
    return g.probeResult(true, build, "", "480p", "row", o.nowUs) == misterplex::CoreGateEvent::Opened &&
           g.open() && g.buildId() == build;
}

// Pair the process gate with 480p, then CAPS-probe `caps` as `raster`.
// An empty refusal re-pairs and opens (probeResult). A non-empty refusal Rejects
// with idleErrorPaintAllowed and lastCapsId = caps (pairing-error idle).
bool probePaired(uint32_t caps, const char* raster, const char* refusal, misterplex::CoreGateEvent want) {
    misterplex::CoreGate& g = misterplex::CoreGate::process();
    g.resetForTest(false);
    g.setPairedRaster("480p", "test pair 480p");
    misterplex::CoreObservation o;
    o.fabric = misterplex::FabricState{true, misterplex::kFpgaMgrModeUser, false};
    o.main = misterplex::MainIdentity{100, 100};
    o.mainStartWallNs = 500;
    o.bootWallNs = 200;
    o.coreName = misterplex::CoreFileStamp{true, "Plex", 1000};
    o.rbfName = misterplex::CoreFileStamp{true, "Plex_480p_r1", 1000};
    o.nowUs = 5000000000;
    g.observe(o);
    o.nowUs += misterplex::CoreGate::kSettleUs;
    if (g.observe(o) != misterplex::CoreGateEvent::ProbeWanted)
        return false;
    if (g.probeResult(true, caps, refusal, raster, "row", o.nowUs) != want)
        return false;
    if (want == misterplex::CoreGateEvent::Opened)
        return g.open() && g.buildId() == caps && g.pairedRaster() == raster && g.takeRePair();
    return g.idleErrorPaintAllowed() && g.lastCapsId() == caps && !g.open();
}

// The gate open on a row:
// - open on the statusBlock row 480058BC: active, so the write passes the gate and stops only at the
//   (never opened) SPI;
// - closed again by a reprogram: closeNow keeps buildId() = 480058BC, so only the open() check stops the beat
//   reaching the next core;
// - open on every bundle row without statusBlock (prod 4800C88F, P1 48003ECB, 240p 2400F40B): off
//   with a fresh FpgaSpi's default force and refused before SPI (zero SPI writes), on only while forced.
void testGateOpen() {
    misterplex::CoreGate& g = misterplex::CoreGate::process();
    FpgaSpi spi;
    expect(openGateOn(0x480058BCu), "core gate opens on 480058BC (probe path)");
    expect(spi.statusBlockActive(), "gate open on 480058BC (statusBlock row): active");
    expect(!spi.writeStatusHeartbeat(0x4D) && spi.lastError().find("SPI not open") != std::string::npos,
           "gate open on 480058BC: the write passes the gate, stops at the closed SPI (" + spi.lastError() + ")");
    g.closeNow("test: FPGA manager reprogramming", 5000000000 + 2 * misterplex::CoreGate::kSettleUs);
    expect(!g.open() && g.buildId() == 0x480058BCu, "a closed gate still reports the last build 480058BC");
    expect(!spi.statusBlockActive(), "gate closed by a reprogram (last build 480058BC): off");
    expect(!spi.writeStatusHeartbeat(0x4D) && spi.lastError().find("core gate closed") != std::string::npos,
           "gate closed by a reprogram: refused before SPI (" + spi.lastError() + ")");
    {
        FpgaSpi f1;
        expect(openGateOn(0x24003997u), "core gate opens on 24003997 (probe path)");
        expect(f1.statusBlockActive(), "gate open on 24003997 (F1 statusBlock row): active");
        expect(!f1.writeStatusHeartbeat(0x4D) && f1.lastError().find("SPI not open") != std::string::npos,
               "gate open on 24003997: the write passes the gate, stops at the closed SPI (" + f1.lastError() + ")");
    }
    {
        expect(probePaired(0x24003997u, "240p", "", misterplex::CoreGateEvent::Opened),
               "raster change re-pairs onto 24003997");
        FpgaSpi f1re;
        expect(f1re.statusBlockActive(), "re-paired open on 24003997: status block active");
        expect(probePaired(0x4801F40Au, "480i", "", misterplex::CoreGateEvent::Opened),
               "raster change re-pairs onto 4801F40A");
        FpgaSpi i480re;
        expect(!i480re.statusBlockActive(), "re-paired open on 4801F40A (no statusBlock): off");
        FpgaSpi f1rej;
        expect(probePaired(0x24003997u, "240p", "build refused", misterplex::CoreGateEvent::Rejected),
               "CAPS refusal rejects 24003997");
        expect(f1rej.statusBlockActive(), "pairing-error idle on 24003997 (F1 statusBlock CAPS): active");
        expect(!f1rej.writeStatusHeartbeat(0x4D) && f1rej.lastError().find("SPI not open") != std::string::npos,
               "pairing-error idle on 24003997: the write passes the gate, stops at the closed SPI (" +
                   f1rej.lastError() + ")");
        FpgaSpi i480rej;
        expect(probePaired(0x4801F40Au, "480i", "build refused", misterplex::CoreGateEvent::Rejected),
               "CAPS refusal rejects 4801F40A");
        expect(!i480rej.statusBlockActive(), "pairing-error idle on 4801F40A (no statusBlock): off");
        expect(!i480rej.writeStatusHeartbeat(0x4D) &&
                   i480rej.lastError().find("not on this build row") != std::string::npos,
               "pairing-error idle on 4801F40A: refused before SPI (" + i480rej.lastError() + ")");
    }
    for (uint32_t id : {0x4800C88Fu, 0x48003ECBu, 0x2400F40Bu, 0x4801F40Au}) {
        char hx[16];
        std::snprintf(hx, sizeof hx, "%08X", id);
        FpgaSpi fresh;
        expect(openGateOn(id), std::string("core gate opens on ") + hx + " (probe path)");
        expect(!fresh.statusBlockActive(), std::string("gate open on ") + hx + " (no statusBlock), default force: off");
        expect(!fresh.writeStatusHeartbeat(0x4D) &&
                   fresh.lastError().find("not on this build row") != std::string::npos,
               std::string(hx) + ": refused before SPI, not a status block row (" + fresh.lastError() + ")");
        fresh.setStatusBlockForce(true);
        expect(fresh.statusBlockActive(), std::string("gate open on ") + hx + ", lab force: active");
        fresh.setStatusBlockForce(false);
        expect(!fresh.statusBlockActive(), std::string(hx) + ": force cleared, off again");
    }
    g.resetForTest(false);  // leave the process gate as a fresh daemon has it: closed
    std::printf("gate open: 480058BC 24003997 active; re-pair 24003997 active, 4801F40A off; "
                "CAPS-refusal idle 24003997 active, 4801F40A refused; "
                "closed by a reprogram (buildId kept) off; 4800C88F 48003ECB 2400F40B 4801F40A refused before SPI "
                "unless forced\n");
}

void testForceEnv() {
    expect(hb::statusBlockForceEnv("1"), "env 1: on");
    for (const char* v : {static_cast<const char*>(nullptr), "", "0", "yes", "11", "1 ", "true"})
        expect(!hb::statusBlockForceEnv(v), std::string("env '") + (v ? v : "unset") + "': off");
}

void testStatUnchanged() {
    uint8_t raw[16]{};
    raw[6] = 0x5C;
    raw[7] = 0x05;
    for (const misterplex::FpgaBuildInfo& b : misterplex::kRecognisedFpgaBuilds) {
        const FpgaSpi::CoreStatus s = FpgaSpi::parseCoreStatus(raw, &b);
        expect(s.first_mb_type == 0x5C && s.wr_count_lo == 0x055C && s.slice_type == 0x05,
               "byte 6 still parses as the first_mb_type stat on every row");
    }
}

}  // namespace

int main() {
    testLayout();
    testWord();
    testRowRule();
    testGate();
    testGateOpen();
    testForceEnv();
    testStatUnchanged();
    if (fails) {
        std::fprintf(stderr, "test_status_heartbeat: %d failures\n", fails);
        return 1;
    }
    std::printf("PASS status heartbeat (H = status byte 6 [55:48], H[7] core-driven, writes while the gate is open "
                "or pairing-error idle on a statusBlock CAPS row)\n");
    return 0;
}
