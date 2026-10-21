// Recognised FPGA builds (fpga_build_ids.hpp): misterplexd plays only on the
// tabled CAPS build IDs and refuses every other one with a clear error and no
// ARM-present fallback (media_player.cpp fpgaThreadMain, failFpgaSession).
#include "libmisterplex/cadence.hpp"
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/plexob_yc.hpp"
#include "libmisterplex/status_telemetry.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>

using namespace misterplex;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL fpga build ids: %s\n", msg.c_str());
        ++fails;
    }
}

std::string hex(uint32_t id) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", id);
    return b;
}

// A recognised row with its explicit status layout: byte 15 / byte 12 echoed
// (true) or telemetry (false), and the CONF_STR "v,N" (V3-FIT-PRECHECK-1003).
void expectKnown(uint32_t id, const char* raster, bool byte15Echoed, bool byte12Echoed,
                 int confVersion, const char* layout) {
    const FpgaBuildInfo* b = recognisedFpgaBuild(id);
    expect(b != nullptr, hex(id) + " must be recognised");
    if (!b)
        return;
    expect(std::strcmp(b->raster, raster) == 0, hex(id) + " raster " + b->raster);
    expect(b->byte15Echoed == byte15Echoed && b->byte12Echoed == byte12Echoed &&
               b->confVersion == confVersion,
           hex(id) + " row " + std::to_string(b->byte15Echoed) + "/" +
               std::to_string(b->byte12Echoed) + "/" + std::to_string(b->confVersion) + ", want " +
               std::to_string(byte15Echoed) + "/" + std::to_string(byte12Echoed) + "/" +
               std::to_string(confVersion));
    expect(std::strcmp(fpgaStatusLayoutName(*b), layout) == 0,
           hex(id) + " layout " + fpgaStatusLayoutName(*b));
    expect(fpgaBuildRefusal(id).empty(), hex(id) + " must not be refused");
    const std::string label = fpgaBuildLabel(*b);
    expect(label.find(hex(id)) != std::string::npos && label.find(raster) != std::string::npos,
           hex(id) + " label: " + label);
}

void expectRefused(uint32_t id) {
    expect(recognisedFpgaBuild(id) == nullptr, hex(id) + " must not be recognised");
    const std::string r = fpgaBuildRefusal(id);
    expect(!r.empty(), hex(id) + " must be refused");
    expect(r.find("unrecognised FPGA build " + hex(id)) != std::string::npos,
           hex(id) + " refusal names the build: " + r);
    expect(r.find("refusing to play") != std::string::npos &&
               r.find("no ARM-present fallback") != std::string::npos,
           hex(id) + " refusal says no play and no ARM-present fallback");
}

void expectRetired(uint32_t id) {
    expect(recognisedFpgaBuild(id) == nullptr && retiredFpgaBuild(id) != nullptr,
           hex(id) + " must be retired, not recognised");
    const std::string r = fpgaBuildRefusal(id);
    expect(r.find("retired FPGA build " + hex(id)) != std::string::npos,
           hex(id) + " refusal says retired: " + r);
    expect(r.find("refusing to play, no ARM-present fallback") != std::string::npos,
           hex(id) + " retired refusal says no play and no ARM-present fallback");
}

// Merged OSD layout v3 (CONF_STR "v,10"): status_in byte `byte` = bits
// [bitHi:bitLo] is an echoed OSD byte on build `id`, so the daemon's status
// writes carry its live value (overlayLiveOsdBytes, fpga_spi.cpp kick and
// setStatusBit paths) and it is never read as telemetry.
void expectOsdEchoByte(uint32_t id, int byte, int bitHi, int bitLo) {
    const std::string what = hex(id) + " byte " + std::to_string(byte);
    const FpgaBuildInfo* b = recognisedFpgaBuild(id);
    expect(b != nullptr && std::strcmp(fpgaStatusLayoutName(*b), "v3") == 0, what + ": not a v3 build");
    if (!b)
        return;
    expect(byte * 8 == bitLo && bitHi == bitLo + 7, what + " is not bits [" +
           std::to_string(bitHi) + ":" + std::to_string(bitLo) + "]");
    expect(fpgaStatusByteIsOsdEcho(*b, byte), what + " must be an echoed OSD byte");
    uint8_t shadow[16], live[16];
    for (int i = 0; i < 16; ++i) {
        shadow[i] = static_cast<uint8_t>(0x10 + i);
        live[i] = static_cast<uint8_t>(0xA0 + i);
    }
    uint8_t word[16];
    std::memcpy(word, shadow, sizeof(word));
    status_telemetry::overlayLiveOsdBytes(word, live);
    expect(word[byte] == live[byte], what + ": the daemon's status write drops the live OSD value");
    for (int i = 0; i < 16; ++i) {
        if (!fpgaStatusByteIsOsdEcho(*b, i))
            expect(word[i] == shadow[i], what + ": overlay touched non-OSD byte " + std::to_string(i));
    }
}

void expectTelemetryBytes(uint32_t id) {
    const FpgaBuildInfo* b = recognisedFpgaBuild(id);
    expect(b != nullptr && std::strcmp(fpgaStatusLayoutName(*b), "v2") == 0, hex(id) + ": not a v2 build");
    if (!b)
        return;
    for (int byte : {15, 12})
        expect(!fpgaStatusByteIsOsdEcho(*b, byte),
               hex(id) + " byte " + std::to_string(byte) + " must stay telemetry (v2)");
}

} // namespace

int main() {
    // The table: unique IDs, known rasters and layouts, nothing empty.
    std::set<uint32_t> ids;
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        expect(ids.insert(b.id).second, "duplicate " + hex(b.id));
        expect(b.id != 0, "build ID 0 (CAPS publishes nonzero only)");
        const std::string r = b.raster, l = fpgaStatusLayoutName(b);
        expect(r == "480p" || r == "240p" || r == "480i", hex(b.id) + " raster " + r);
        expect(l == "v2" || l == "v3", hex(b.id) + " layout " + l + " (explicit fields mixed)");
        expect(b.lineage && *b.lineage, hex(b.id) + " lineage");
    }
    // PlexOB Y/C is a registry row, not a legacy CAPS/PLX row (same rule as T10).
    expect(recognisedFpgaBuild(0x0B59C11Au) == nullptr, "0B59C11A is not a legacy CAPS row");
    expect(recognisedPlexObBuildRow(0x0B59C119u) == nullptr, "dead 0B59C119 is not the Y/C row");
    {
        const PlexObBuildRow* y = recognisedPlexObBuildRow(0x0B59C11Au);
        expect(y && y->id == misterplex::plexob::kPlexObYcBuildId &&
                   std::strstr(y->lineage, "0B59C11A") != nullptr &&
                   std::strstr(y->lineage, "word 87") != nullptr &&
                   std::strstr(y->lineage, "O[21:20]") != nullptr &&
                   std::strstr(y->lineage, "O[24]") != nullptr &&
                   std::strstr(y->lineage, "c8b34604") != nullptr &&
                   std::strstr(y->lineage, "not fitted yet") == nullptr,
               "0B59C11A PlexOB registry row names the Y/C map and the shipped RBF");
    }

    // Prod 480p (RBF 7cbae324, 82ef65c s2: VIDEO_BUILD_ID in its map report).
    expectKnown(0x4800F36Au, "480p", false, false, 9, "v2");
    // 480p lab and RC lines that glass has played, and the N5 next RC.
    expectKnown(0x48004D57u, "480p", false, false, 9, "v2"); // L2 7bccc0e0
    expectKnown(0x48004D40u, "480p", false, false, 9, "v2"); // RC 47cb9fb4 999800cf
    expectKnown(0x48005D40u, "480p", false, false, 9, "v2"); // N5 next RC 97584eaf
    {
        const FpgaBuildInfo& n5 = *recognisedFpgaBuild(0x48005D40u);
        const FpgaBuildInfo& rc = *recognisedFpgaBuild(0x48004D40u);
        expect(n5.byte15Echoed == rc.byte15Echoed && n5.byte12Echoed == rc.byte12Echoed &&
                   n5.confVersion == rc.confVersion,
               "N5 48005D40 has the RC 48004D40 layout");
    }
    // N5 clk_dec speed steps (480p speed glass): 45 MHz 4c210d99 (RBF 534cb8e7) and
    // 50 MHz 2b770497 (RBF 7fdb10b9) play exactly like 48005D40: same raster, status layout
    // and CONF_STR.
    expectKnown(0x48005D45u, "480p", false, false, 9, "v2");
    expectKnown(0x48005D50u, "480p", false, false, 9, "v2");
    // R1 + MVP-b lab line 4800A1FB (48005D45 core + PLEX_HOST_IDLE_FB + PLEX_INPUT_NAV): the
    // MVP-b J1 names do not bump CONF_STR "v,9", so it plays like 48005D45.
    expectKnown(0x4800A1FBu, "480p", false, false, 9, "v2");
    for (uint32_t id : {0x48005D45u, 0x4800A1FBu, 0x48005D50u}) {
        const FpgaBuildInfo* s = recognisedFpgaBuild(id);
        const FpgaBuildInfo& n5 = *recognisedFpgaBuild(0x48005D40u);
        expect(s && std::strcmp(s->raster, n5.raster) == 0 &&
                   s->byte15Echoed == n5.byte15Echoed && s->byte12Echoed == n5.byte12Echoed &&
                   s->confVersion == n5.confVersion &&
                   std::strcmp(fpgaStatusLayoutName(*s), fpgaStatusLayoutName(n5)) == 0,
               hex(id) + " has the 48005D40 row (raster, layout, CONF_STR)");
        if (s) {
            expectTelemetryBytes(id);
            expect(fpgaConfVersionMismatch(*s, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*s, "Plex;;v,10;").empty(),
                   hex(id) + " CONF_STR check as 48005D40 (v,9 plays, v,10 refused)");
        }
    }
    // Nearby N5 clock IDs without an RBF stay refused (exact match only). 48005D47 was the R1 lab
    // line's ID before its 17:02 re-ID to 4800A1FB (no RBF ever carried it).
    for (uint32_t id : {0x48005D41u, 0x48005D44u, 0x48005D46u, 0x48005D47u,
                        0x48005D51u, 0x48005D53u, 0x48005D55u, 0x48005D60u})
        expectRefused(id);
    // dec50 cut 4 (f6abe0fe = e7b081ab's row; the D52-LOG soak rbf d53bdd17 answers it): 48005D52 is the
    // 48005D50 row in every field but the ID and the lineage, including the A/V trim, resync and drift.
    expectKnown(0x48005D52u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* c4 = recognisedFpgaBuild(0x48005D52u);
        const FpgaBuildInfo* d50 = recognisedFpgaBuild(0x48005D50u);
        expect(c4 && d50 && std::strcmp(c4->raster, d50->raster) == 0 &&
                   c4->byte15Echoed == d50->byte15Echoed && c4->byte12Echoed == d50->byte12Echoed &&
                   c4->confVersion == d50->confVersion && c4->avTrimMs == d50->avTrimMs &&
                   c4->avResync == d50->avResync && c4->avDriftPpm == d50->avDriftPpm &&
                   std::strcmp(fpgaStatusLayoutName(*c4), fpgaStatusLayoutName(*d50)) == 0,
               "48005D52 is the 48005D50 row (raster, layout, CONF_STR, trim, resync, drift)");
        if (c4 && d50) {
            expectTelemetryBytes(0x48005D52u);
            expect(fpgaAudioDelayMs(50, c4) == fpgaAudioDelayMs(50, d50),
                   "48005D52 audio start delay as 48005D50");
        }
    }
    // The host idle FB and input nav capabilities: the R1 lines only (45 MHz 4800A1FB, 50 MHz 4800A152,
    // the fixes refit 48009E36 on A152, and its P_PART port 48002731, and the deblock fit 4800C88F on 9E36,
    // the P_PART ship build 48003ECB, Track B 480058BC (C88F + fix21 + the Status block), D-PP1
    // 480094DD (C88F + P_PART + partitioned-bS), T-DPP1-STATUS 0x48018AE0 (the 94DD core + the 029A
    // Status block), T-ASCAL-FOLD 0x48032F84 (the 94DD core + the 4-cone ascal fold, 6b3832ac3), and
    // T-ASCAL-FOLD-SYNC 0x48038D9A (the same fold on the E-port (baaeff6) ascal, 99fe93e1e).
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        const bool r1 = b.id == 0x4800A1FBu || b.id == 0x4800A152u || b.id == 0x48009E36u || b.id == 0x48002731u || b.id == 0x4800C88Fu ||
                        b.id == 0x48003ECBu || b.id == 0x480058BCu || b.id == 0x480094DDu || b.id == 0x48018AE0u ||
                        b.id == 0x48032F84u || b.id == 0x48038D9Au ||
                        b.id == 0x48FF65F2u || b.id == 0x487A1BD1u || b.id == 0x48C5385Cu ||
                        b.id == 0x48C51FF1u || b.id == 0x48CFEE31u || b.id == 0x48110015u;
        // F1 (F1-PREREG v2.2): the 240p idle FB row has hostIdleFb (the R1 painter) without inputNav,
        // and is the only idleBeatPlxl row (the PLXL beat writes only there, review 01:30 p2).
        const bool f1 = b.id == 0x24003997u || b.id == 0x24F1A2C4u || b.id == 0x24A10026u;
        const bool high = b.id == 0x48A64AEDu;
        expect(b.hostIdleFb == (r1 || f1) && b.inputNav == (r1 || high),
               hex(b.id) + (r1 ? " must have hostIdleFb and inputNav"
                               : f1 ? " must have hostIdleFb and lack inputNav"
                                    : high ? " must lack hostIdleFb and have inputNav"
                                           : " must lack hostIdleFb and inputNav"));
        expect(b.idleBeatPlxl == f1, hex(b.id) + (f1 ? " must have" : " must lack") + " idleBeatPlxl");
        // Track B (review 17:24): statusBlock by NAME on every row; 480p Status-block refits plus
        // F1 240p 0x24003997. 480i 0x4801F40A stays false. A positional slip fails here.
        const bool sb = b.id == 0x480058BCu || b.id == 0x48018AE0u || b.id == 0x48FF65F2u || b.id == 0x487A1BD1u ||
                        b.id == 0x48C5385Cu || b.id == 0x48C51FF1u || b.id == 0x48CFEE31u || b.id == 0x48110015u ||
                        b.id == 0x48A64AEDu ||
                        b.id == 0x24003997u ||
                        b.id == 0x24F1A2C4u || b.id == 0x24A10026u || b.id == 0x48F14A0Bu;
        expect(b.statusBlock == sb, hex(b.id) + (sb ? " must have" : " must lack") + " statusBlock");
        // WEDGE page: only the 480p WEDGE-p1 row and its analog-VGA sibling. Lab 240p/480i
        // (24C07821 / 48C078E2) keep wedgePage false: those QSFs have PLEX_WEDGE_MON but no
        // MPX_STATUS_BLOCK / plex_cdc_lib.sv, so the daemon must not read a WEDGE page.
        const bool wp = b.id == 0x48FF65F2u || b.id == 0x487A1BD1u || b.id == 0x48C5385Cu ||
                        b.id == 0x48C51FF1u || b.id == 0x48CFEE31u || b.id == 0x48A64AEDu ||
                        b.id == 0x48110015u;
        expect(b.wedgePage == wp, hex(b.id) + (wp ? " must have" : " must lack") + " wedgePage");
    }
    // F1 ship 24003997 (release 12:46): the 2400F40B status layout v3 (c4188b00: bytes 15/12 echoed, "v,10"), the
    // 0x300FF000 PLX page (PLXL 0x300FF148), trim/resync/drift 0, and at least 7 bits from every other
    // recognised or retired ID (release: 9 over 74 IDs; the Plex_480p_r1.qsf rule is 7). The lab F1 ID 24000EFD
    // is retired: the ship daemon refuses the lab RBF ca0afbe4 (review 12:45 (2)).
    expectRetired(0x24000EFDu);
    expectKnown(0x24003997u, "240p", true, true, 10, "v3");
    {
        const FpgaBuildInfo* f = recognisedFpgaBuild(0x24003997u);
        expect(f && fpgaPlxPage(f) == 0x300FF000u && f->avTrimMs == 0 && !f->avResync && f->avDriftPpm == 0.0 &&
                   f->idleBeatPlxl && f->statusBlock && !f->wedgePage && !f->spsRelaxNvenc,
               "24003997: PLX page 0x300FF000, no A/V trim, idleBeatPlxl+statusBlock");
        int minBits = 32;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            if (b.id != 0x24003997u)
                minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x24003997u));
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x24003997u));
        expect(minBits >= 7, "24003997 is only " + std::to_string(minBits) + " bits from another ID (want >= 7)");
        std::printf("F1 24003997: min Hamming %d to the %zu other recognised + retired IDs\n", minBits,
                    std::size(kRecognisedFpgaBuilds) - 1 + std::size(kRetiredFpgaBuilds));
    }
    expectKnown(0x24F1A2C4u, "240p", true, true, 10, "v3");
    {
        const FpgaBuildInfo* f = recognisedFpgaBuild(0x24F1A2C4u);
        expect(f && fpgaPlxPage(f) == 0x300FF000u && f->avTrimMs == 0 && !f->avResync && f->avDriftPpm == 0.0 &&
                   f->idleBeatPlxl && f->statusBlock && !f->wedgePage && !f->spsRelaxNvenc && f->hostIdleFb &&
                   !f->inputNav,
               "24F1A2C4: PLX page 0x300FF000, no A/V trim, idleBeatPlxl+statusBlock+hostIdleFb");
        int minBits = 32;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            if (b.id != 0x24F1A2C4u)
                minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x24F1A2C4u));
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x24F1A2C4u));
        expect(minBits >= 7, "24F1A2C4 is only " + std::to_string(minBits) + " bits from another ID (want >= 7)");
        std::printf("F1 24F1A2C4: min Hamming %d to the %zu other recognised + retired IDs\n", minBits,
                    std::size(kRecognisedFpgaBuilds) - 1 + std::size(kRetiredFpgaBuilds));
    }
    expectKnown(0x24A10026u, "240p", true, true, 10, "v3");
    {
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x24A10026u);
        const FpgaBuildInfo* s = recognisedFpgaBuild(0x24F1A2C4u);
        expect(n && s && n != s && n->spsRelaxNvenc && !s->spsRelaxNvenc && !n->highCabac &&
                   n->hostIdleFb == s->hostIdleFb && n->inputNav == s->inputNav &&
                   n->idleBeatPlxl == s->idleBeatPlxl && n->statusBlock == s->statusBlock &&
                   n->wedgePage == s->wedgePage && n->avTrimMs == s->avTrimMs,
               "24A10026 is the 24F1A2C4 row plus spsRelaxNvenc");
        int minBits = 32;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            if (b.id != 0x24A10026u)
                minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x24A10026u));
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x24A10026u));
        expect(minBits >= 7, "24A10026 is only " + std::to_string(minBits) + " bits from another ID (want >= 7)");
    }
    // R1 50 MHz refit 4800A152 (release 08:58, review 09:11): the 48005D52 row in every field but the
    // ID, the lineage and the two R1 capabilities; unique, resolved, and at least 4 bits from every
    // other recognised or retired ID (4 from 4800A1FB).
    expectKnown(0x4800A152u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x4800A152u);
        const FpgaBuildInfo* c4 = recognisedFpgaBuild(0x48005D52u);
        expect(r && c4 && std::strcmp(r->raster, c4->raster) == 0 && r->plxPage == c4->plxPage &&
                   r->byte15Echoed == c4->byte15Echoed && r->byte12Echoed == c4->byte12Echoed &&
                   r->confVersion == c4->confVersion && r->avTrimMs == c4->avTrimMs &&
                   r->avResync == c4->avResync && r->avDriftPpm == c4->avDriftPpm &&
                   std::strcmp(fpgaStatusLayoutName(*r), fpgaStatusLayoutName(*c4)) == 0,
               "4800A152 is the 48005D52 row (raster, PLX page, layout, CONF_STR, trim, resync, drift)");
        if (r && c4) {
            expectTelemetryBytes(0x4800A152u);
            expect(fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "4800A152 CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, c4), "4800A152 audio start delay as 48005D52");
            expect(r->hostIdleFb && r->inputNav, "4800A152 has hostIdleFb and inputNav");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x4800A152u)
                return;
            const int bits = __builtin_popcount(id ^ 0x4800A152u);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 4, "4800A152 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 4 from every recognised or retired ID)");
        std::printf("4800A152: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // Fixes refit 48009E36 (release 10:44, review 10:45/12:30; ea4ca1e0, RBF s2 c74cf4dc): the 4800A152
    // row in every field but the ID and the lineage (the daemon ABI is A152's); unique, resolved, and at
    // least 4 bits from every other recognised or retired ID (7 when it was chosen).
    expectKnown(0x48009E36u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48009E36u);
        const FpgaBuildInfo* a = recognisedFpgaBuild(0x4800A152u);
        expect(r && a && std::strcmp(r->raster, a->raster) == 0 && r->plxPage == a->plxPage &&
                   r->byte15Echoed == a->byte15Echoed && r->byte12Echoed == a->byte12Echoed &&
                   r->confVersion == a->confVersion && r->avTrimMs == a->avTrimMs &&
                   r->avResync == a->avResync && r->avDriftPpm == a->avDriftPpm &&
                   r->hostIdleFb == a->hostIdleFb && r->inputNav == a->inputNav &&
                   std::strcmp(fpgaStatusLayoutName(*r), fpgaStatusLayoutName(*a)) == 0,
               "48009E36 is the 4800A152 row (raster, PLX page, layout, CONF_STR, trim, resync, drift, R1 caps)");
        if (r && a) {
            expectTelemetryBytes(0x48009E36u);
            expect(fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "48009E36 CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, a), "48009E36 audio start delay as 4800A152");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x48009E36u)
                return;
            const int bits = __builtin_popcount(id ^ 0x48009E36u);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 4, "48009E36 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 4 from every recognised or retired ID)");
        std::printf("48009E36: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // P_PART port 48002731 (release 10/05, review 16:41; dev/ppart-9e36 9801dad2, no RBF yet): the
    // 48009E36 row in every field but the ID and the lineage (PLEX_P_PART_PRODUCT is used under rtl/ only);
    // unique, resolved, and at least 7 bits from every other recognised or retired ID (the rule it was
    // chosen under). 48005D46 (5D45 + P_PART) stays refused.
    expectKnown(0x48002731u, "480p", false, false, 9, "v2");
    expectRefused(0x48005D46u);
    {
        const FpgaBuildInfo* p = recognisedFpgaBuild(0x48002731u);
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48009E36u);
        expect(p && r && std::strcmp(p->raster, r->raster) == 0 && p->plxPage == r->plxPage &&
                   p->byte15Echoed == r->byte15Echoed && p->byte12Echoed == r->byte12Echoed &&
                   p->confVersion == r->confVersion && p->avTrimMs == r->avTrimMs &&
                   p->avResync == r->avResync && p->avDriftPpm == r->avDriftPpm &&
                   p->hostIdleFb == r->hostIdleFb && p->inputNav == r->inputNav &&
                   std::strcmp(fpgaStatusLayoutName(*p), fpgaStatusLayoutName(*r)) == 0,
               "48002731 is the 48009E36 row (raster, PLX page, layout, CONF_STR, trim, resync, drift, R1 caps)");
        if (p && r) {
            expectTelemetryBytes(0x48002731u);
            expect(fpgaConfVersionMismatch(*p, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*p, "Plex;;v,10;").empty(),
                   "48002731 CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, p) == fpgaAudioDelayMs(50, r), "48002731 audio start delay as 48009E36");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x48002731u)
                return;
            const int bits = __builtin_popcount(id ^ 0x48002731u);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 7, "48002731 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 7 from every recognised or retired ID)");
        std::printf("48002731: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // Deblock fit 4800C88F (review 16:41 ID ruling; dev/lane7-deblock-9e36 7cb897b7): the 48009E36
    // row in every field but the ID and the lineage (the CAPS Deblock bit is read at runtime, not from
    // the table); unique, resolved, and at least 7 bits from every other recognised or retired ID (the
    // ruling's bar; 9 from 48009E36).
    expectKnown(0x4800C88Fu, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x4800C88Fu);
        const FpgaBuildInfo* f = recognisedFpgaBuild(0x48009E36u);
        expect(r && f && std::strcmp(r->raster, f->raster) == 0 && r->plxPage == f->plxPage &&
                   r->byte15Echoed == f->byte15Echoed && r->byte12Echoed == f->byte12Echoed &&
                   r->confVersion == f->confVersion && r->avTrimMs == f->avTrimMs &&
                   r->avResync == f->avResync && r->avDriftPpm == f->avDriftPpm &&
                   r->hostIdleFb == f->hostIdleFb && r->inputNav == f->inputNav &&
                   std::strcmp(fpgaStatusLayoutName(*r), fpgaStatusLayoutName(*f)) == 0,
               "4800C88F is the 48009E36 row (raster, PLX page, layout, CONF_STR, trim, resync, drift, R1 caps)");
        if (r && f) {
            expectTelemetryBytes(0x4800C88Fu);
            expect(fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "4800C88F CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, f), "4800C88F audio start delay as 48009E36");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            // Track B 480058BC sits 6 bits from 4800C88F, which release 17:28 + review 17:28 accepted (6 is the
            // most any 0x4800xxxx ID reaches): pinned exactly below, so the >= 7 bar still covers every other ID.
            if (id == 0x4800C88Fu || id == 0x480058BCu)
                return;
            const int bits = __builtin_popcount(id ^ 0x4800C88Fu);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 7, "4800C88F is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 7 from every recognised or retired ID but 480058BC)");
        expect(__builtin_popcount(0x4800C88Fu ^ 0x480058BCu) == 6, "4800C88F vs Track B 480058BC: exactly 6 bits");
        std::printf("4800C88F: min Hamming %d bits (nearest %s; 480058BC at 6, accepted)\n", minBits,
                    hex(nearest).c_str());
    }
    // P_PART ship build 48003ECB (review 18:39 ID ruling; dev/ppart-r-sub8 f32947c5 = 48002731 + the
    // F-PPART-SUB8 path R refusal of P sub_mb_type != 0, decode_error 22): the 48009E36 row in every field but
    // the ID and the lineage; unique, resolved, and at least 7 bits from every other recognised or retired ID
    // (the ruling's bar; 9 from 48009E36 and 48002731, 8 from 4800C88F).
    expectKnown(0x48003ECBu, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48003ECBu);
        const FpgaBuildInfo* f = recognisedFpgaBuild(0x48009E36u);
        expect(r && f && std::strcmp(r->raster, f->raster) == 0 && r->plxPage == f->plxPage &&
                   r->byte15Echoed == f->byte15Echoed && r->byte12Echoed == f->byte12Echoed &&
                   r->confVersion == f->confVersion && r->avTrimMs == f->avTrimMs &&
                   r->avResync == f->avResync && r->avDriftPpm == f->avDriftPpm &&
                   r->hostIdleFb == f->hostIdleFb && r->inputNav == f->inputNav &&
                   std::strcmp(fpgaStatusLayoutName(*r), fpgaStatusLayoutName(*f)) == 0,
               "48003ECB is the 48009E36 row (raster, PLX page, layout, CONF_STR, trim, resync, drift, R1 caps)");
        if (r && f) {
            expectTelemetryBytes(0x48003ECBu);
            expect(fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "48003ECB CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, f), "48003ECB audio start delay as 48009E36");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x48003ECBu)
                return;
            const int bits = __builtin_popcount(id ^ 0x48003ECBu);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 7, "48003ECB is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 7 from every recognised or retired ID)");
        std::printf("48003ECB: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // Track B 480058BC (tonight; release 17:28 with review 17:28; cb1e5f8b = C88F 7cb897b7 + fix21 + the Status
    // block): the 4800C88F row with statusBlock on. Every field is read BY NAME, so a positional slip
    // (statusBlock in the idleBeatPlxl slot) fails here, not on glass (review 17:24). Unique, resolved, and at
    // least 6 bits from every other recognised or retired ID (6 is the most any 0x4800xxxx ID reaches over the
    // origin and crew IDs; the local-only S1 crop lab row 480051AC must never ship while this row is live).
    expectKnown(0x480058BCu, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* b = recognisedFpgaBuild(0x480058BCu);
        const FpgaBuildInfo* c = recognisedFpgaBuild(0x4800C88Fu);
        const FpgaBuildInfo* f = recognisedFpgaBuild(0x24003997u);
        expect(b && c && std::strcmp(b->raster, c->raster) == 0 && b->plxPage == c->plxPage &&
                   b->byte15Echoed == c->byte15Echoed && b->byte12Echoed == c->byte12Echoed &&
                   b->confVersion == c->confVersion && b->avTrimMs == c->avTrimMs && b->avResync == c->avResync &&
                   b->avDriftPpm == c->avDriftPpm && b->hostIdleFb == c->hostIdleFb && b->inputNav == c->inputNav &&
                   b->idleBeatPlxl == c->idleBeatPlxl &&
                   std::strcmp(fpgaStatusLayoutName(*b), fpgaStatusLayoutName(*c)) == 0,
               "480058BC is the 4800C88F row in every field but the ID, the lineage and statusBlock (raster, PLX page, "
               "bytes 15/12, CONF_STR, trim, resync, drift, hostIdleFb, inputNav, idleBeatPlxl, layout)");
        expect(b && b->statusBlock && !b->idleBeatPlxl, "480058BC by name: statusBlock true, idleBeatPlxl false");
        expect(c && !c->statusBlock && !c->idleBeatPlxl, "4800C88F by name: statusBlock false, idleBeatPlxl false");
        expect(f && f->idleBeatPlxl && f->statusBlock, "24003997 by name: idleBeatPlxl true, statusBlock true");
        if (b && c) {
            expectTelemetryBytes(0x480058BCu);
            expect(fpgaConfVersionMismatch(*b, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*b, "Plex;;v,10;").empty(),
                   "480058BC CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, b) == fpgaAudioDelayMs(50, c), "480058BC audio start delay as 4800C88F");
            expect(fpgaPlxPage(b) == 0x300FF000u, "480058BC PLX page 0x300FF000");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x480058BCu)
                return;
            const int bits = __builtin_popcount(id ^ 0x480058BCu);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& r : kRecognisedFpgaBuilds)
            consider(r.id);
        for (const FpgaBuildRetired& r : kRetiredFpgaBuilds)
            consider(r.id);
        expect(minBits >= 6, "480058BC is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 6 from every recognised or retired ID)");
        std::printf("480058BC: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // D-PP1 480094DD (review 20:28 ID CLEAR; dev/dpp1-joint-1006 01d2e473, RBF 3f5213ab): the
    // 4800C88F row in every field but the ID and the lineage. Unique, resolved, and at least 7 bits
    // from every other recognised or retired ID (wW audit; nearest 3ECB/C88F/58BC/5D45/A1FB at 7).
    expectKnown(0x480094DDu, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x480094DDu);
        const FpgaBuildInfo* c = recognisedFpgaBuild(0x4800C88Fu);
        expect(r && c && std::strcmp(r->raster, c->raster) == 0 && r->plxPage == c->plxPage &&
                   r->byte15Echoed == c->byte15Echoed && r->byte12Echoed == c->byte12Echoed &&
                   r->confVersion == c->confVersion && r->avTrimMs == c->avTrimMs &&
                   r->avResync == c->avResync && r->avDriftPpm == c->avDriftPpm &&
                   r->hostIdleFb == c->hostIdleFb && r->inputNav == c->inputNav &&
                   r->idleBeatPlxl == c->idleBeatPlxl && r->statusBlock == c->statusBlock &&
                   std::strcmp(fpgaStatusLayoutName(*r), fpgaStatusLayoutName(*c)) == 0,
               "480094DD is the 4800C88F row in every field but the ID and the lineage");
        expect(r && r->hostIdleFb && r->inputNav && !r->statusBlock && !r->idleBeatPlxl,
               "480094DD by name: R1 caps, statusBlock false, idleBeatPlxl false");
        if (r && c) {
            expectTelemetryBytes(0x480094DDu);
            expect(fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "480094DD CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, c), "480094DD audio start delay as 4800C88F");
            expect(fpgaPlxPage(r) == 0x300FF000u, "480094DD PLX page 0x300FF000");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x480094DDu)
                return;
            const int bits = __builtin_popcount(id ^ 0x480094DDu);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 7, "480094DD is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 7 from every recognised or retired ID)");
        expect(__builtin_popcount(0x480094DDu ^ 0x4800C88Fu) == 7, "480094DD vs 4800C88F: exactly 7 bits");
        expect(__builtin_popcount(0x480094DDu ^ 0x480058BCu) == 7, "480094DD vs 480058BC: exactly 7 bits");
        expect(__builtin_popcount(0x480094DDu ^ 0x48003ECBu) == 7, "480094DD vs 48003ECB: exactly 7 bits");
        std::printf("480094DD: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // T-DPP1-STATUS 48018AE0 (tonight 2026-10-07, min Hamming 8): the 4800C88F row with statusBlock on,
    // the 94DD D-PP1 decoder plus the 029A read-only Status block (MPX_STATUS_BLOCK; the 8cdd8cfa RTL
    // verbatim), dev/dpp1-status-1007 e7a38f4a on the menu-clean 94DD base bdf3dec4e. Every field is
    // read BY NAME, so a positional slip (statusBlock in the idleBeatPlxl slot) fails here, not on
    // glass. Unique, resolved, and at least 7 bits from every other recognised or retired ID
    // (min 8; the nearest are 48004D40, 48009E36 and 4801F36A at 8).
    expectKnown(0x48018AE0u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48018AE0u);
        const FpgaBuildInfo* c = recognisedFpgaBuild(0x4800C88Fu);
        expect(r && c && std::strcmp(r->raster, c->raster) == 0 && r->plxPage == c->plxPage &&
                   r->byte15Echoed == c->byte15Echoed && r->byte12Echoed == c->byte12Echoed &&
                   r->confVersion == c->confVersion && r->avTrimMs == c->avTrimMs &&
                   r->avResync == c->avResync && r->avDriftPpm == c->avDriftPpm &&
                   r->hostIdleFb == c->hostIdleFb && r->inputNav == c->inputNav &&
                   r->idleBeatPlxl == c->idleBeatPlxl &&
                   std::strcmp(fpgaStatusLayoutName(*r), fpgaStatusLayoutName(*c)) == 0,
               "48018AE0 is the 4800C88F row in every field but the ID, the lineage and statusBlock");
        expect(r && r->hostIdleFb && r->inputNav && r->statusBlock && !r->idleBeatPlxl,
               "48018AE0 by name: R1 caps, statusBlock true, idleBeatPlxl false");
        if (r && c) {
            expectTelemetryBytes(0x48018AE0u);
            expect(fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "48018AE0 CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, c), "48018AE0 audio start delay as 4800C88F");
            expect(fpgaPlxPage(r) == 0x300FF000u, "48018AE0 PLX page 0x300FF000");
        }
        int minBits = 32;
        uint32_t nearest = 0;
        auto consider = [&](uint32_t id) {
            if (id == 0x48018AE0u)
                return;
            const int bits = __builtin_popcount(id ^ 0x48018AE0u);
            if (bits < minBits) {
                minBits = bits;
                nearest = id;
            }
        };
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            consider(b.id);
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            consider(b.id);
        expect(minBits >= 7, "48018AE0 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                 " (want >= 7 from every recognised or retired ID)");
        expect(__builtin_popcount(0x48018AE0u ^ 0x4801F36Au) == 8, "48018AE0 vs 4801F36A: exactly 8 bits");
        expect(__builtin_popcount(0x48018AE0u ^ 0x48004D40u) == 8, "48018AE0 vs 48004D40: exactly 8 bits");
        expect(__builtin_popcount(0x48018AE0u ^ 0x480058BCu) == 9, "48018AE0 vs 480058BC: exactly 9 bits");
        expect(__builtin_popcount(0x48018AE0u ^ 0x480094DDu) == 10, "48018AE0 vs 480094DD: exactly 10 bits");
        std::printf("48018AE0: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
    }
    // T-ASCAL-FOLD 48032F84 (tonight 2026-10-08, min Hamming 8): the 480094DD row in every field
    // but the ID and the lineage: the ascal 4-cone fold onto the 94DD core, same 480p r1 qsf
    // (BUILD_ID alone), no status block, no RBF yet.
    expectKnown(0x48032F84u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48032F84u);
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x480094DDu);
        if (r && n)
            expect((r->raster == n->raster) && (r->plxPage == n->plxPage) &&
                       (r->byte15Echoed == n->byte15Echoed) && (r->byte12Echoed == n->byte12Echoed) &&
                       (r->confVersion == n->confVersion) && (r->avTrimMs == n->avTrimMs) &&
                       (r->avResync == n->avResync) && (r->avDriftPpm == n->avDriftPpm) &&
                       (r->hostIdleFb == n->hostIdleFb) && (r->inputNav == n->inputNav) &&
                       (r->idleBeatPlxl == n->idleBeatPlxl) && (r->statusBlock == n->statusBlock),
                   "48032F84 is the 480094DD row in every field but the ID and the lineage");
        expect(r && r->hostIdleFb && r->inputNav && !r->statusBlock && !r->idleBeatPlxl &&
                   !r->byte15Echoed && !r->byte12Echoed && r->confVersion == 9 &&
                   r->avTrimMs == -45 && r->avResync && r->avDriftPpm == -14.6,
                "48032F84 by name: R1 caps, no status block, 94DD A/V row");
        if (r) {
            expectTelemetryBytes(0x48032F84u);
            expect(std::strcmp(fpgaStatusLayoutName(*r), "v2") == 0 &&
                       fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "48032F84 CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, n), "48032F84 audio start delay as 480094DD");
            expect(fpgaPlxPage(r) == 0x300FF000u, "48032F84 PLX page 0x300FF000");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48032F84u) {
                    const int bits = __builtin_popcount(id.id ^ 0x48032F84u);
                    if (bits < minBits) {
                        minBits = bits;
                        nearest = id.id;
                    }
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds) {
                if (id.id == 0x48032F84u)
                    continue;
                const int bits = __builtin_popcount(id.id ^ 0x48032F84u);
                if (bits < minBits) {
                    minBits = bits;
                    nearest = id.id;
                }
            }
            expect(minBits >= 7, "48032F84 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                               ", want 7 or more");
            expect(__builtin_popcount(0x48032F84u ^ 0x48002731u) == 8, "48032F84 vs 48002731: exactly 8 bits");
            expect(__builtin_popcount(0x48032F84u ^ 0x48004D40u) == 8, "48032F84 vs 48004D40: exactly 8 bits");
            expect(__builtin_popcount(0x48032F84u ^ 0x48018AE0u) == 8, "48032F84 vs 48018AE0: exactly 8 bits");
            expect(__builtin_popcount(0x48032F84u ^ 0x480094DDu) == 12, "48032F84 vs 480094DD: exactly 12 bits");
            std::printf("48032F84: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    // T-ASCAL-FOLD-SYNC 48038D9A (tonight 2026-10-08, min Hamming 7): the 480094DD row in every
    // field but the ID and the lineage: the ascal 4-cone fold (A+B+C-h+C) onto the 94DD core with
    // the E-port (baaeff6) ascal — proven == plain-fold (6b3832ac3) + the 16-line E-port delta and
    // == bchc + cone A (wt-asc-sync-baaeff6 tb/asc-fold-proof) — same 480p r1 qsf (BUILD_ID
    // alone), no status block, no RBF yet.
    expectKnown(0x48038D9Au, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48038D9Au);
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x480094DDu);
        if (r && n)
            expect((r->raster == n->raster) && (r->plxPage == n->plxPage) &&
                       (r->byte15Echoed == n->byte15Echoed) && (r->byte12Echoed == n->byte12Echoed) &&
                       (r->confVersion == n->confVersion) && (r->avTrimMs == n->avTrimMs) &&
                       (r->avResync == n->avResync) && (r->avDriftPpm == n->avDriftPpm) &&
                       (r->hostIdleFb == n->hostIdleFb) && (r->inputNav == n->inputNav) &&
                       (r->idleBeatPlxl == n->idleBeatPlxl) && (r->statusBlock == n->statusBlock),
                   "48038D9A is the 480094DD row in every field but the ID and the lineage");
        expect(r && r->hostIdleFb && r->inputNav && !r->statusBlock && !r->idleBeatPlxl &&
                   !r->byte15Echoed && !r->byte12Echoed && r->confVersion == 9 &&
                   r->avTrimMs == -45 && r->avResync && r->avDriftPpm == -14.6,
                "48038D9A by name: R1 caps, no status block, 94DD A/V row");
        if (r) {
            expectTelemetryBytes(0x48038D9Au);
            expect(std::strcmp(fpgaStatusLayoutName(*r), "v2") == 0 &&
                       fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "48038D9A CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, n), "48038D9A audio start delay as 480094DD");
            expect(fpgaPlxPage(r) == 0x300FF000u, "48038D9A PLX page 0x300FF000");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48038D9Au) {
                    const int bits = __builtin_popcount(id.id ^ 0x48038D9Au);
                    if (bits < minBits) {
                        minBits = bits;
                        nearest = id.id;
                    }
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds) {
                if (id.id == 0x48038D9Au)
                    continue;
                const int bits = __builtin_popcount(id.id ^ 0x48038D9Au);
                if (bits < minBits) {
                    minBits = bits;
                    nearest = id.id;
                }
            }
            expect(minBits >= 7, "48038D9A is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                               ", want 7 or more");
            expect(__builtin_popcount(0x48038D9Au ^ 0x48032F84u) == 7, "48038D9A vs 48032F84: exactly 7 bits");
            expect(__builtin_popcount(0x48038D9Au ^ 0x480094DDu) == 9, "48038D9A vs 480094DD: exactly 9 bits");
            expect(__builtin_popcount(0x48038D9Au ^ 0x48018AE0u) == 9, "48038D9A vs 48018AE0: exactly 9 bits");
            expect(__builtin_popcount(0x48038D9Au ^ 0x4800029Au) == 7, "48038D9A vs 4800029A: exactly 7 bits");
            std::printf("48038D9A: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    // T-WEDGE-COUNTERS phase 1 48FF65F2 (tonight 2026-10-08, min Hamming 13): the 48018AE0 row in
    // every field but the ID, the lineage and the new wedgePage (on). The detect-only counters are
    // read-only side-outputs off the status telemetry, so every other field (including statusBlock)
    // is 48018AE0's. Read BY NAME, so a positional slip (wedgePage in the statusBlock slot) fails here.
    // Unique, resolved, and at least 7 bits from every other recognised or retired ID
    // (min 13; the nearest are 48005D52, 4800A152 and 4800A1FB at 13). dev/wedge-p1-1007 0a4c5f8fce0.
    expectKnown(0x48FF65F2u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48FF65F2u);
        const FpgaBuildInfo* c = recognisedFpgaBuild(0x48018AE0u);
        if (r && c)
            expect((r->raster == c->raster) && (r->plxPage == c->plxPage) &&
                       (r->byte15Echoed == c->byte15Echoed) && (r->byte12Echoed == c->byte12Echoed) &&
                       (r->confVersion == c->confVersion) && (r->avTrimMs == c->avTrimMs) &&
                       (r->avResync == c->avResync) && (r->avDriftPpm == c->avDriftPpm) &&
                       (r->hostIdleFb == c->hostIdleFb) && (r->inputNav == c->inputNav) &&
                       (r->idleBeatPlxl == c->idleBeatPlxl) && (r->statusBlock == c->statusBlock) &&
                       (r->wedgePage != c->wedgePage),
                   "48FF65F2 is the 48018AE0 row in every field but the ID, the lineage and wedgePage");
        expect(r && r->hostIdleFb && r->inputNav && r->statusBlock && r->wedgePage && !r->idleBeatPlxl &&
                   !r->byte15Echoed && !r->byte12Echoed && r->confVersion == 9 &&
                   r->avTrimMs == -45 && r->avResync && r->avDriftPpm == -14.6,
               "48FF65F2 by name: R1 caps, statusBlock true, wedgePage true, 94DD A/V row");
        expect(c && !c->wedgePage, "48018AE0 must lack wedgePage (only the WIDGE-p1 row carries it)");
        if (r) {
            expectTelemetryBytes(0x48FF65F2u);
            expect(std::strcmp(fpgaStatusLayoutName(*r), "v2") == 0 &&
                       fpgaConfVersionMismatch(*r, "Plex;;v,9;").empty() &&
                       !fpgaConfVersionMismatch(*r, "Plex;;v,10;").empty(),
                   "48FF65F2 CONF_STR check (v,9 plays, v,10 refused)");
            expect(fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, c), "48FF65F2 audio start delay as 48018AE0");
            expect(fpgaPlxPage(r) == 0x300FF000u, "48FF65F2 PLX page 0x300FF000");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48FF65F2u) {
                    const int bits = __builtin_popcount(id.id ^ 0x48FF65F2u);
                    if (bits < minBits) {
                        minBits = bits;
                        nearest = id.id;
                    }
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds) {
                if (id.id == 0x48FF65F2u)
                    continue;
                const int bits = __builtin_popcount(id.id ^ 0x48FF65F2u);
                if (bits < minBits) {
                    minBits = bits;
                    nearest = id.id;
                }
            }
            expect(minBits >= 7, "48FF65F2 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                               ", want 7 or more");
            expect(__builtin_popcount(0x48FF65F2u ^ 0x48005D52u) == 13, "48FF65F2 vs 48005D52: exactly 13 bits");
            expect(__builtin_popcount(0x48FF65F2u ^ 0x4800A152u) == 13, "48FF65F2 vs 4800A152: exactly 13 bits");
            expect(__builtin_popcount(0x48FF65F2u ^ 0x4800A1FBu) == 13, "48FF65F2 vs 4800A1FB: exactly 13 bits");
            std::printf("48FF65F2: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    // Prod 480p + analog VGA 487A1BD1 (prod-480p/analog-vga-1011 853265774): the 48FF65F2 row in every field but the
    // ID and the lineage, read by name; unique and at least 7 bits from every recognised or retired ID (min 9).
    expectKnown(0x487A1BD1u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x487A1BD1u);
        const FpgaBuildInfo* c = recognisedFpgaBuild(0x48FF65F2u);
        expect(r && c && r != c && (r->raster == c->raster) && (r->plxPage == c->plxPage) &&
                   (r->byte15Echoed == c->byte15Echoed) && (r->byte12Echoed == c->byte12Echoed) &&
                   (r->confVersion == c->confVersion) && (r->avTrimMs == c->avTrimMs) &&
                   (r->avResync == c->avResync) && (r->avDriftPpm == c->avDriftPpm) &&
                   (r->hostIdleFb == c->hostIdleFb) && (r->inputNav == c->inputNav) &&
                   (r->idleBeatPlxl == c->idleBeatPlxl) && (r->statusBlock == c->statusBlock) &&
                   (r->wedgePage == c->wedgePage),
               "487A1BD1 is the 48FF65F2 row in every field but the ID and the lineage");
        if (r) {
            expectTelemetryBytes(0x487A1BD1u);
            expect(fpgaPlxPage(r) == 0x300FF000u && fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, c),
                   "487A1BD1 PLX page and audio start delay as 48FF65F2");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x487A1BD1u && __builtin_popcount(id.id ^ 0x487A1BD1u) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x487A1BD1u);
                    nearest = id.id;
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds)
                if (id.id != 0x487A1BD1u && __builtin_popcount(id.id ^ 0x487A1BD1u) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x487A1BD1u);
                    nearest = id.id;
                }
            expect(minBits >= 7, "487A1BD1 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                     ", want 7 or more");
            std::printf("487A1BD1: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    // Analog v10 SPS relax 48C5385C (c950-spsrelax-analog-v10-fit e1c118511): the 487A1BD1 row in every
    // field but the ID, the lineage and spsRelaxNvenc (on); analog v11 48C51FF1 is the same flag.
    // Unique and at least 7 bits from every recognised or retired ID.
    expectKnown(0x48C5385Cu, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* r = recognisedFpgaBuild(0x48C5385Cu);
        const FpgaBuildInfo* c = recognisedFpgaBuild(0x487A1BD1u);
        expect(r && c && r != c && (r->raster == c->raster) && (r->plxPage == c->plxPage) &&
                   (r->byte15Echoed == c->byte15Echoed) && (r->byte12Echoed == c->byte12Echoed) &&
                   (r->confVersion == c->confVersion) && (r->avTrimMs == c->avTrimMs) &&
                   (r->avResync == c->avResync) && (r->avDriftPpm == c->avDriftPpm) &&
                   (r->hostIdleFb == c->hostIdleFb) && (r->inputNav == c->inputNav) &&
                   (r->idleBeatPlxl == c->idleBeatPlxl) && (r->statusBlock == c->statusBlock) &&
                   (r->wedgePage == c->wedgePage) && r->spsRelaxNvenc && !c->spsRelaxNvenc,
               "48C5385C is the 487A1BD1 row in every field but the ID, the lineage and spsRelaxNvenc");
        int relaxRows = 0;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
            if (b.spsRelaxNvenc) {
                ++relaxRows;
                expect(b.id == 0x48C5385Cu || b.id == 0x48C51FF1u || b.id == 0x48CFEE31u ||
                           b.id == 0x48110015u || b.id == 0x24A10026u,
                       hex(b.id) +
                           " must lack spsRelaxNvenc (0x48C5385C / 0x48C51FF1 / 0x48CFEE31 / "
                           "0x48110015 / 0x24A10026 only)");
            }
        }
        expect(relaxRows == 5, "exactly five spsRelaxNvenc rows, got " + std::to_string(relaxRows));
        if (r) {
            expectTelemetryBytes(0x48C5385Cu);
            expect(fpgaPlxPage(r) == 0x300FF000u && fpgaAudioDelayMs(50, r) == fpgaAudioDelayMs(50, c),
                   "48C5385C PLX page and audio start delay as 487A1BD1");
            expect(std::strstr(r->lineage, "e6a011d0") != nullptr, "48C5385C lineage names RBF e6a011d0");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48C5385Cu && __builtin_popcount(id.id ^ 0x48C5385Cu) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x48C5385Cu);
                    nearest = id.id;
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds)
                if (id.id != 0x48C5385Cu && __builtin_popcount(id.id ^ 0x48C5385Cu) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x48C5385Cu);
                    nearest = id.id;
                }
            expect(minBits >= 7, "48C5385C is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                     ", want 7 or more");
            expect(__builtin_popcount(0x48C5385Cu ^ 0x487A1BD1u) == 14, "48C5385C vs 487A1BD1: exactly 14 bits");
            expect(__builtin_popcount(0x48C5385Cu ^ 0x48FF65F2u) == 14, "48C5385C vs 48FF65F2: exactly 14 bits");
            std::printf("48C5385C: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    expectKnown(0x48C51FF1u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x48C51FF1u);
        const FpgaBuildInfo* v10 = recognisedFpgaBuild(0x48C5385Cu);
        expect(n && v10 && n != v10 && (n->raster == v10->raster) && (n->plxPage == v10->plxPage) &&
                   (n->byte15Echoed == v10->byte15Echoed) && (n->byte12Echoed == v10->byte12Echoed) &&
                   (n->confVersion == v10->confVersion) && (n->avTrimMs == v10->avTrimMs) &&
                   (n->avResync == v10->avResync) && (n->avDriftPpm == v10->avDriftPpm) &&
                   (n->hostIdleFb == v10->hostIdleFb) && (n->inputNav == v10->inputNav) &&
                   (n->idleBeatPlxl == v10->idleBeatPlxl) && (n->statusBlock == v10->statusBlock) &&
                   (n->wedgePage == v10->wedgePage) && n->spsRelaxNvenc && v10->spsRelaxNvenc,
               "48C51FF1 is the 48C5385C row in every field but the ID and the lineage");
        if (n) {
            expectTelemetryBytes(0x48C51FF1u);
            expect(fpgaPlxPage(n) == 0x300FF000u && fpgaAudioDelayMs(50, n) == fpgaAudioDelayMs(50, v10),
                   "48C51FF1 PLX page and audio start delay as 48C5385C");
            expect(std::strstr(n->lineage, "48C51FF1") != nullptr, "48C51FF1 lineage names its BUILD_ID");
            expect(std::strstr(n->lineage, "6365a3d6") != nullptr, "48C51FF1 lineage names RBF 6365a3d6");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48C51FF1u && __builtin_popcount(id.id ^ 0x48C51FF1u) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x48C51FF1u);
                    nearest = id.id;
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds)
                if (id.id != 0x48C51FF1u && __builtin_popcount(id.id ^ 0x48C51FF1u) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x48C51FF1u);
                    nearest = id.id;
                }
            expect(minBits >= 7, "48C51FF1 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                     ", want 7 or more");
            expect(__builtin_popcount(0x48C51FF1u ^ 0x48C5385Cu) == 9, "48C51FF1 vs 48C5385C: exactly 9 bits");
            std::printf("48C51FF1: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    expectKnown(0x48CFEE31u, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x48CFEE31u);
        const FpgaBuildInfo* v11 = recognisedFpgaBuild(0x48C51FF1u);
        expect(n && v11 && n != v11 && (n->raster == v11->raster) && (n->plxPage == v11->plxPage) &&
                   (n->byte15Echoed == v11->byte15Echoed) && (n->byte12Echoed == v11->byte12Echoed) &&
                   (n->confVersion == v11->confVersion) && (n->avTrimMs == v11->avTrimMs) &&
                   (n->avResync == v11->avResync) && (n->avDriftPpm == v11->avDriftPpm) &&
                   (n->hostIdleFb == v11->hostIdleFb) && (n->inputNav == v11->inputNav) &&
                   (n->idleBeatPlxl == v11->idleBeatPlxl) && (n->statusBlock == v11->statusBlock) &&
                   (n->wedgePage == v11->wedgePage) && n->spsRelaxNvenc && v11->spsRelaxNvenc,
               "48CFEE31 is the 48C51FF1 row in every field but the ID and the lineage");
        if (n) {
            expectTelemetryBytes(0x48CFEE31u);
            expect(fpgaPlxPage(n) == 0x300FF000u && fpgaAudioDelayMs(50, n) == fpgaAudioDelayMs(50, v11),
                   "48CFEE31 PLX page and audio start delay as 48C51FF1");
            expect(std::strstr(n->lineage, "48CFEE31") != nullptr, "48CFEE31 lineage names its BUILD_ID");
            expect(std::strstr(n->lineage, "analog-v12") != nullptr, "48CFEE31 lineage names analog-v12");
            expect(std::strstr(n->lineage, "052990c6") != nullptr, "48CFEE31 lineage names RBF 052990c6");
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48CFEE31u && __builtin_popcount(id.id ^ 0x48CFEE31u) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x48CFEE31u);
                    nearest = id.id;
                }
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds)
                if (id.id != 0x48CFEE31u && __builtin_popcount(id.id ^ 0x48CFEE31u) < minBits) {
                    minBits = __builtin_popcount(id.id ^ 0x48CFEE31u);
                    nearest = id.id;
                }
            expect(minBits >= 7, "48CFEE31 is " + std::to_string(minBits) + " bit(s) from " + hex(nearest) +
                                     ", want 7 or more");
            expect(__builtin_popcount(0x48CFEE31u ^ 0x48C51FF1u) == 9, "48CFEE31 vs 48C51FF1: exactly 9 bits");
            std::printf("48CFEE31: min Hamming %d bits (nearest %s)\n", minBits, hex(nearest).c_str());
        }
    }
    expectKnown(0x48A64AEDu, "480p", false, false, 9, "v2");
    {
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x48A64AEDu);
        expect(n && !n->hostIdleFb && n->inputNav && n->statusBlock && n->wedgePage &&
                   !n->spsRelaxNvenc && !n->idleBeatPlxl && n->highCabac,
               "48A64AED matches Plex_high.qsf: High/CABAC, status, wedge, input nav, no idle FB");
        int highRows = 0;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            if (b.highCabac)
                ++highRows;
        expect(highRows == 1, "exactly one highCabac row, got " + std::to_string(highRows));
        expect(recognisedFpgaBuild(0x48C51FF1u) && !recognisedFpgaBuild(0x48C51FF1u)->highCabac,
               "baseline 48C51FF1 must still restart High/CABAC");
        expect(highCabacForPlay(nullptr, true, 0x48A64AEDu),
               "open gate 48A64AED selects the CABAC ladder before CAPS");
        expect(!highCabacForPlay(nullptr, false, 0x48A64AEDu),
               "a closed gate does not select the CABAC ladder");
        expect(!highCabacForPlay(nullptr, true, 0x48C51FF1u),
               "open baseline gate stays on the CAVLC ladder");
        expect(!highCabacForPlay(recognisedFpgaBuild(0x48C51FF1u), true, 0x48A64AEDu),
               "a negotiated baseline row is not overridden by the gate");
        expect(!recognisedFpgaBuild(0x48C0A6ACu), "placeholder 48C0A6AC is not a fitted core");
        if (n) {
            expect(std::strstr(n->lineage, "48A64AED") != nullptr, "48A64AED lineage names its BUILD_ID");
            expect(std::strstr(n->lineage, "353e7c6d") != nullptr, "48A64AED lineage cites CLOSED RBF 353e7c6d");
            int minBits = 32;
            for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds)
                if (id.id != 0x48A64AEDu)
                    minBits = std::min(minBits, __builtin_popcount(id.id ^ 0x48A64AEDu));
            for (const FpgaBuildRetired& id : kRetiredFpgaBuilds)
                minBits = std::min(minBits, __builtin_popcount(id.id ^ 0x48A64AEDu));
            expect(minBits >= 7, "48A64AED is only " + std::to_string(minBits) + " bits from another ID");
        }
    }
    expectKnown(0x4800F40Au, "480p", false, false, 9, "v2"); // F40A clk_dec 40 MHz
    expectKnown(0x48006A7Eu, "480p", false, false, 9, "v2"); // fstore scanout gate
    // 240p raster: pre-OSD 2400F36A (RBF 5554d013) stays v2 with both bytes telemetry.
    expectKnown(0x2400F36Au, "240p", false, false, 9, "v2");
    expectTelemetryBytes(0x2400F36Au);
    expect(std::strstr(recognisedFpgaBuild(0x2400F36Au)->lineage, "5554d013") != nullptr,
           "2400F36A lineage names RBF 5554d013");
    // 240p OSD line 2400F40B (OSD + echo fix + later handshake, one fit): merged layout v3.
    expectKnown(0x2400F40Bu, "240p", true, true, 10, "v3");
    // True 480i product: 4801F40A (94cdeef9 successor), layout v3. Lab 480i 48C078E2 is v2
    // (no MPX_OSD_V10) and is the second 480i entry; 4801F40B stays refused.
    expectKnown(0x4801F40Au, "480i", true, true, 10, "v3");
    {
        const FpgaBuildInfo* i480 = recognisedFpgaBuild(0x4801F40Au);
        expect(i480 && !i480->statusBlock && !i480->idleBeatPlxl,
               "4801F40A lacks statusBlock until MPX_STATUS_BLOCK fit (H would stomp first_mb_type)");
    }
    expectKnown(0x48F14A0Bu, "480i", true, true, 10, "v3");
    {
        const FpgaBuildInfo* n = recognisedFpgaBuild(0x48F14A0Bu);
        expect(n && fpgaPlxPage(n) == 0x300FF000u && n->statusBlock && !n->idleBeatPlxl &&
                   !n->hostIdleFb && !n->inputNav && !n->wedgePage && !n->spsRelaxNvenc,
               "48F14A0B: PLX page 0x300FF000, statusBlock, no idle FB/PLXL");
        int minBits = 32;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
            if (b.id != 0x48F14A0Bu)
                minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x48F14A0Bu));
        for (const FpgaBuildRetired& b : kRetiredFpgaBuilds)
            minBits = std::min(minBits, __builtin_popcount(b.id ^ 0x48F14A0Bu));
        expect(minBits >= 7, "48F14A0B is only " + std::to_string(minBits) + " bits from another ID (want >= 7)");
        std::printf("480i 48F14A0B: min Hamming %d to the %zu other recognised + retired IDs\n", minBits,
                    std::size(kRecognisedFpgaBuilds) - 1 + std::size(kRetiredFpgaBuilds));
    }
    // 240p/480i lab rows on the prod table (c465/c485): v2 0/0/9, kPlx300FF, A/V zeros,
    // no hostIdleFb / idleBeatPlxl / statusBlock / wedgePage. Lineage names the lab QSF
    // and "no RBF yet". Hamming already measured 8 vs nearest product ids.
    expectKnown(0x24C07821u, "240p", false, false, 9, "v2");
    expectKnown(0x48C078E2u, "480i", false, false, 9, "v2");
    {
        const FpgaBuildInfo* p240 = recognisedFpgaBuild(0x24C07821u);
        const FpgaBuildInfo* p480 = recognisedFpgaBuild(0x48C078E2u);
        for (const FpgaBuildInfo* b : {p240, p480}) {
            expect(b && b->plxPage == kPlx300FF && fpgaPlxPage(b) == 0x300FF000u &&
                       b->avTrimMs == 0 && !b->avResync && b->avDriftPpm == 0.0 &&
                       !b->hostIdleFb && !b->inputNav && !b->idleBeatPlxl && !b->statusBlock &&
                       !b->wedgePage,
                   hex(b ? b->id : 0) + " lab row: v2 defaults, PLX 0x300FF000, A/V 0/false/0, "
                                        "no idle/WEDGE/status");
            if (b) {
                expectTelemetryBytes(b->id);
                expect(std::strstr(b->lineage, "no RBF yet") != nullptr,
                       hex(b->id) + " lineage says no RBF yet: " + b->lineage);
                expect(fpgaConfVersionMismatch(*b, "Plex;;v,9;").empty() &&
                           !fpgaConfVersionMismatch(*b, "Plex;;v,10;").empty(),
                       hex(b->id) + " CONF_STR v,9 plays, v,10 refused");
                expect(fpgaAudioDelayMs(50, b) == 50, hex(b->id) + " conf 50 stays 50 (no trim)");
            }
        }
        expect(p240 && std::strstr(p240->lineage, "Plex_240p_lab.qsf") != nullptr &&
                   std::strstr(p240->lineage, "24C07821") != nullptr,
               "24C07821 lineage names Plex_240p_lab.qsf and the id");
        expect(p480 && std::strstr(p480->lineage, "Plex_480i_lab.qsf") != nullptr &&
                   std::strstr(p480->lineage, "48C078E2") != nullptr,
               "48C078E2 lineage names Plex_480i_lab.qsf and the id");
        auto minHamming = [](uint32_t id) {
            int minBits = 32;
            uint32_t nearest = 0;
            for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds)
                if (b.id != id) {
                    const int bits = __builtin_popcount(b.id ^ id);
                    if (bits < minBits) {
                        minBits = bits;
                        nearest = b.id;
                    }
                }
            for (const FpgaBuildRetired& b : kRetiredFpgaBuilds) {
                const int bits = __builtin_popcount(b.id ^ id);
                if (bits < minBits) {
                    minBits = bits;
                    nearest = b.id;
                }
            }
            return std::pair<int, uint32_t>{minBits, nearest};
        };
        const auto h240 = minHamming(0x24C07821u);
        const auto h480 = minHamming(0x48C078E2u);
        expect(h240.first == 8 && h240.second == 0x2400F40Bu,
               "24C07821 min Hamming " + std::to_string(h240.first) + " nearest " + hex(h240.second) +
                   ", want 8 vs 2400F40B");
        expect(h480.first == 8,
               "48C078E2 min Hamming " + std::to_string(h480.first) + " nearest " + hex(h480.second) +
                   ", want 8");
        std::printf("24C07821: min Hamming %d bits (nearest %s)\n", h240.first, hex(h240.second).c_str());
        std::printf("48C078E2: min Hamming %d bits (nearest %s)\n", h480.first, hex(h480.second).c_str());
        expect(kLab240pBuildId == 0x24C07821u && kLab240pHContent == 320 && kLab240pVActive == 240 &&
                   kLab240pClocksPerLine == 638 && kLab240pLinesPerFrame == 262 &&
                   lab240pPixelHz() == 10000000 && lab240pLineMilliHz() == 15673981 &&
                   lab240pFrameMicroHz() == 59824355,
               "lab 240p beam: 638 clocks/line, 262 lines, 10 MHz, 15.673981 kHz, 59.824355 Hz");
        expect(ntscConstantFieldRepeat(30000, 1001) == 2, "29.97 is 2:2 on the 59.94 tick");
        expect(ntscConstantFieldRepeat(30, 1) == 0, "30/1 is not 29.97");
        expect(ntscConstantFieldRepeat(24000, 1001) == 0, "23.976 is not a fixed repeat");
        expect(ntscConstantFieldRepeat(25, 1) == 0 && ntscConstantFieldRepeat(50, 1) == 0,
               "25 and 50 stay off the NTSC tick");
        expect(cadenceMailboxFps(24000, 1001) == 24 && cadenceMailboxFps(30000, 1001) == 30,
               "NTSC clocks program integer 24/30 into the 8-bit FPGA mailbox");
        bool pulldown32 = true;
        const bool want32[] = {true, false, false, true, false, true, false, false, true, false};
        for (int n = 0; n < 10; ++n)
            pulldown32 = pulldown32 &&
                         (should_advance_unique(static_cast<uint32_t>(n),
                                                cadenceMailboxFps(24000, 1001), 60) == want32[n]);
        bool weave22 = true;
        const bool want22[] = {true, false, true, false};
        for (int n = 0; n < 4; ++n)
            weave22 = weave22 &&
                      (should_advance_unique(static_cast<uint32_t>(n),
                                             cadenceMailboxFps(30000, 1001), 60) == want22[n]);
        expect(pulldown32 && weave22,
               "FPGA mailbox 24@60 is 3:2 and 30@60 is 2:2 (AV clock stays 24000/1001, 30000/1001)");
    }
    // NTSC rows never take the 480p 45/50 MHz A/V trim.
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        if (std::string(b.raster) == "240p" || std::string(b.raster) == "480i")
            expect(b.avTrimMs == 0 && !b.avResync && b.avDriftPpm == 0.0,
                   hex(b.id) + " NTSC row must keep A/V 0/false/0");
    }
    // One check per byte per ID: the merged OSD layout echoes both bytes on both lab rasters.
    expectOsdEchoByte(0x4801F40Au, 15, 127, 120);
    expectOsdEchoByte(0x4801F40Au, 12, 103, 96);
    expectOsdEchoByte(0x2400F40Bu, 15, 127, 120);
    expectOsdEchoByte(0x2400F40Bu, 12, 103, 96);
    // F1 ship 24003997: the 2400F40B status layout v3 (c4188b00), the same bytes as 2400F40B.
    expectOsdEchoByte(0x24003997u, 15, 127, 120);
    expectOsdEchoByte(0x24003997u, 12, 103, 96);
    expectOsdEchoByte(0x24F1A2C4u, 15, 127, 120);
    expectOsdEchoByte(0x24F1A2C4u, 12, 103, 96);
    expectOsdEchoByte(0x24A10026u, 15, 127, 120);
    expectOsdEchoByte(0x24A10026u, 12, 103, 96);
    expectOsdEchoByte(0x48F14A0Bu, 15, 127, 120);
    expectOsdEchoByte(0x48F14A0Bu, 12, 103, 96);
    // Byte roles across the table: exactly bytes 12 and 15 on v3, none on v2.
    unsigned osdBytes = 0;
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        const bool v3 = std::string(fpgaStatusLayoutName(b)) == "v3";
        for (int byte = 0; byte < 16; ++byte) {
            const bool osd = fpgaStatusByteIsOsdEcho(b, byte);
            osdBytes += osd;
            expect(osd == (v3 && (byte == 12 || byte == 15)),
                   hex(b.id) + " byte " + std::to_string(byte) + " role");
        }
    }
    // Six v3 rows: 4801F40A, 2400F40B, F1 24003997, 24F1A2C4, 24A10026, 48F14A0B (bytes 12 and 15 each).
    expect(osdBytes == 12, "OSD echo bytes in the table: " + std::to_string(osdBytes) + ", want 12");
    // Each field drives its own byte (synthetic rows; no core ships a mixed layout).
    const FpgaBuildInfo only15{0x00000001u, "240p", PlxPageClass::Present480pOrMetaDpb, true,
                               false, 10, "synthetic"};
    const FpgaBuildInfo only12{0x00000002u, "240p", PlxPageClass::Present480pOrMetaDpb, false,
                               true, 10, "synthetic"};
    expect(fpgaStatusByteIsOsdEcho(only15, 15) && !fpgaStatusByteIsOsdEcho(only15, 12) &&
               std::string(fpgaStatusLayoutName(only15)) == "mixed",
           "byte15Echoed alone: byte 15 OSD, byte 12 telemetry, layout mixed");
    expect(!fpgaStatusByteIsOsdEcho(only12, 15) && fpgaStatusByteIsOsdEcho(only12, 12) &&
               std::string(fpgaStatusLayoutName(only12)) == "mixed",
           "byte12Echoed alone: byte 12 OSD, byte 15 telemetry, layout mixed");
    // 4801F40B (OSD branch 480i) is on hold: one 480i ID, refused until the Leads add it.
    expectRefused(0x4801F40Bu);
    // CONF_STR cross-check (v,10 rows, media_player after the gate): "v,N;" parse and the
    // refusal when the core's CONF_STR disagrees with the row.
    expect(confStrVersion("Plex;;O[122:121],Aspect ratio,Original,Full Screen;v,10;V,v261003;") ==
               10, "CONF_STR v,10");
    expect(confStrVersion("v,9;Plex;;") == 9, "CONF_STR v,9 as the first entry");
    expect(confStrVersion("Plex;;V,v261003;") == -1, "CONF_STR without v,N (V,v is the date)");
    expect(confStrVersion("Plex;;v,1a;v,;") == -1, "CONF_STR malformed v entries");
    for (uint32_t id : {0x2400F40Bu, 0x4801F40Au}) {
        const FpgaBuildInfo& b = *recognisedFpgaBuild(id);
        expect(fpgaConfVersionMismatch(b, "Plex;;J1,Play;v,10;V,v1;").empty(),
               hex(id) + " accepts its own v,10");
        for (const char* conf : {"Plex;;J1,Play;v,9;V,v1;", "Plex;;J1,Play;V,v1;"}) {
            const std::string r = fpgaConfVersionMismatch(b, conf);
            expect(r.find(hex(id)) != std::string::npos && r.find("v,10") != std::string::npos &&
                       r.find("refusing to play, no ARM-present fallback") != std::string::npos,
                   hex(id) + " refuses CONF_STR '" + conf + "': " + r);
        }
    }
    expect(fpgaConfVersionMismatch(*recognisedFpgaBuild(0x4800F36Au), "Plex;;v,9;").empty(),
           "4800F36A matches v,9");
    unsigned i480 = 0;
    const FpgaBuildInfo* first480i = nullptr;
    const FpgaBuildInfo* second480i = nullptr;
    const FpgaBuildInfo* third480i = nullptr;
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        if (std::string(b.raster) == "480i") {
            ++i480;
            if (i480 == 1)
                first480i = &b;
            else if (i480 == 2)
                second480i = &b;
            else if (i480 == 3)
                third480i = &b;
        }
    }
    expect(i480 == 3, "exactly three 480i entries (4801F40A, 48F14A0B, lab 48C078E2)");
    expect(first480i && first480i->id == 0x4801F40Au &&
               std::strcmp(fpgaStatusLayoutName(*first480i), "v3") == 0 &&
               first480i->byte15Echoed && first480i->byte12Echoed && first480i->confVersion == 10,
           "first 480i entry is product 0x4801F40A v3 1/1/10");
    expect(second480i && second480i->id == 0x48F14A0Bu &&
               std::strcmp(fpgaStatusLayoutName(*second480i), "v3") == 0 &&
               second480i->statusBlock,
           "second 480i entry is 0x48F14A0B v3 statusBlock");
    expect(third480i && third480i->id == 0x48C078E2u &&
               std::strcmp(fpgaStatusLayoutName(*third480i), "v2") == 0 &&
               !third480i->byte15Echoed && !third480i->byte12Echoed &&
               third480i->confVersion == 9,
           "third 480i entry is lab 0x48C078E2 v2 0/0/9");
    // 4801F36A is retired: refused with the reason naming the RBFs behind it.
    expectRetired(0x4801F36Au);
    const std::string retired = fpgaBuildRefusal(0x4801F36Au);
    for (const char* name : {"42da8d23", "ad047c7a", "cc7b6e66", "4801F40A", "dd0bcb08"})
        expect(retired.find(name) != std::string::npos,
               std::string("4801F36A refusal names ") + name + ": " + retired);
    for (const FpgaBuildRetired& r : kRetiredFpgaBuilds)
        expect(!ids.count(r.id), hex(r.id) + " is both retired and recognised");

    // Unknown builds are refused: garbage, the Plex.qsf default, retired lines,
    // and every one-bit neighbour of a recognised ID (no fuzzy matching).
    for (uint32_t id : {0x00000000u, 0xDEADBEEFu, 0xFFFFFFFFu, 0xE41B0437u, 0x48015C03u,
                        0x4800FBA1u, 0x4801F320u, 0xE41BF320u, 0x4802F40Au, 0x2401F36Au})
        expectRefused(id);
    unsigned neighbours = 0;
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        for (int bit = 0; bit < 32; ++bit) {
            const uint32_t n = b.id ^ (1u << bit);
            if (ids.count(n))
                continue;
            if (retiredFpgaBuild(n))
                expectRetired(n);
            else
                expectRefused(n);
            ++neighbours;
        }
    }

    if (fails) {
        std::fprintf(stderr, "test_fpga_build_ids: %d failures\n", fails);
        return 1;
    }
    std::printf("test_fpga_build_ids: OK (%zu recognised builds, explicit rows: 2400F40B and "
                "4801F40A 1/1/10 (bytes 15+12 OSD echo, v,10), %zu v2 rows 0/0/9 incl. 2400F36A "
                "24C07821 48C078E2; CONF_STR v,N check; 4801F40A product 480i v3, 48C078E2 lab "
                "480i v2, 4801F40B refused; 4801F36A retired; "
                "%u one-bit neighbours and 10 other IDs refused)\n",
                ids.size(), ids.size() - 3, neighbours);
    std::printf("retired example: %s\n", fpgaBuildRefusal(0x4801F36Au).c_str());
    std::printf("refusal example: %s\n", fpgaBuildRefusal(0xDEADBEEFu).c_str());
    return 0;
}
