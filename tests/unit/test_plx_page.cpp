// PLX mailbox page per build row (plx_page.hpp, fpga_build_ids.hpp) and the
// live-glass probe on the same table (present_bank.hpp probeLiveGlassWith).
// review 00:55 / release 01:01: a row per page class plus an unknown-ID row
// (no PLX reads); L1: a stale L4 PLXJ plus a live 480P core is not L4.
// --print-true480-pages / --print-row-pages print the probe pages / the ID:page rows for
// the shell twins (scripts/misterplex_named_rbf.sh T480_PAGES,
// scripts/probe_bitstream_ring.sh PLX_ROW_PAGES; tests/unit/test_plx_page_scripts.sh).
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/mailbox_abi_spec.hpp"
#include "libmisterplex/plx_page.hpp"
#include "libmisterplex/present_bank.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace misterplex;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL plx page: %s\n", msg.c_str());
        ++fails;
    }
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

constexpr uint32_t kL4Page = 0x3047F000u;
constexpr uint32_t k480pPage = 0x300FF000u;
constexpr uint32_t k480iPage = 0x301FF000u;
constexpr uint32_t kNoMacroPage = 0x3007F000u;

// HPS DRAM for the probe: a word reads `before` until the probe pauses, then
// `after`. Every read address is recorded.
struct FakeDram {
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> words;
    std::vector<uint32_t> reads;
    bool paused = false;

    void set(uint32_t addr, uint32_t v) { words[addr] = {v, v}; }
    void move(uint32_t addr, uint32_t before, uint32_t after) { words[addr] = {before, after}; }
    uint32_t read(uint32_t addr) {
        reads.push_back(addr);
        const auto it = words.find(addr);
        if (it == words.end())
            return 0;
        return paused ? it->second.second : it->second.first;
    }
    LiveGlass probe() {
        return probeLiveGlassWith([this](uint32_t a) { return read(a); },
                                  [this] { paused = true; });
    }
};

void staleL4Plxj(FakeDram& d) { d.set(kL4Page + mailbox_abi::kPlxjOff, mailbox_abi::kPlxjMagic); }

// A live core's PLXS: magic static, heartbeat seq in the high word moving.
void livePlxsHeartbeat(FakeDram& d, uint32_t page) {
    d.set(page + mailbox_abi::kPlxsOff, mailbox_abi::kPlxsMagic);
    d.move(page + mailbox_abi::kPlxsOff + 4u, 0x00010000u, 0x00070000u);
}

void staticPlxsPlxi(FakeDram& d, uint32_t page) {
    d.set(page + mailbox_abi::kPlxsOff, mailbox_abi::kPlxsMagic);
    d.set(page + mailbox_abi::kPlxsOff + 4u, 0x00420000u);
    d.set(page + mailbox_abi::kPlxiOff, mailbox_abi::kPlxiMagic);
}

void pageClassRows() {
    struct Row {
        PlxPageClass cls;
        uint32_t page;
        int layoutW, layoutH;
    };
    const Row rows[] = {
        {PlxPageClass::Present480pOrMetaDpb, k480pPage, 640, 480},
        {PlxPageClass::Present480i, k480iPage, 720, 480},
        {PlxPageClass::Present720pL4, kL4Page, 1280, 720},
        {PlxPageClass::NoPresentMacro, kNoMacroPage, 320, 240},
    };
    for (const Row& r : rows) {
        const std::string name = plxPageClassName(r.cls);
        expect(plxPageForClass(r.cls) == r.page,
               "class " + name + " page " + hex(plxPageForClass(r.cls)) + " want " + hex(r.page));
        // The daemon's resolved layout of the matching geometry has the same page.
        expect(presentBankDoorbell(r.layoutW, r.layoutH) == r.page,
               "class " + name + " page != layout doorbell " + std::to_string(r.layoutW) + "x" +
                   std::to_string(r.layoutH) + " " + hex(presentBankDoorbell(r.layoutW, r.layoutH)));
        expect(plxWordAddr(r.page, mailbox_abi::kPlxsOff) == r.page + 0x100u &&
                   plxWordAddr(r.page, mailbox_abi::kPlxiOff) == r.page + 0x108u &&
                   plxWordAddr(r.page, mailbox_abi::kPlxdOff, true) == r.page + 0x12Cu &&
                   plxWordAddr(r.page, mailbox_abi::kPlxjOff) == r.page + 0x130u,
               "class " + name + " mailbox words are page + spec offsets");
    }
    std::printf("PASS PLX page per class (480P|META_DPB 0x300FF000, 480I 0x301FF000, "
                "720P_L4 0x3047F000, no macro 0x3007F000) == the layout doorbells\n");
}

void recognisedRows() {
    // Each row's class was checked in the tree its lineage cites (qsf macros and
    // present_core fstore overrides): PRESENT_480P or META_DPB, page 0x300FF000.
    const std::map<uint32_t, uint32_t> verified = {
        {0x4800F36Au, k480pPage}, {0x48004D57u, k480pPage}, {0x48004D40u, k480pPage},
        {0x48005D40u, k480pPage}, {0x48005D45u, k480pPage}, {0x48005D50u, k480pPage},
        {0x4800F40Au, k480pPage}, {0x48006A7Eu, k480pPage}, {0x2400F36Au, k480pPage},
        {0x2400F40Bu, k480pPage},
        {0x24C07821u, k480pPage},  // 240p lab Plex_240p_lab.qsf (c078/c135); META_DPB; no RBF yet
        {0x4801F40Au, k480pPage},
        {0x48C078E2u, k480pPage},  // 480i lab Plex_480i_lab.qsf (c078/c136); META_DPB; no RBF yet
        // dec50 cut 4 at 9c86d9b2: Plex_480p.qsf:162 PLEX_PRESENT_480P=1, :184 ID 48005D52;
        // ddr_frame_layout_params.svh:30/:34 doorbell 0x300F_F000 (review 08:34 (d)).
        {0x48005D52u, k480pPage},
        // R1 A1FB at 8ffb2335 (RBF 2caa9483): Plex_480p_r1.qsf:162 PLEX_PRESENT_480P=1, :187 ID 4800A1FB;
        // ddr_frame_layout_params.svh:30/:34 doorbell 0x300F_F000.
        {0x4800A1FBu, k480pPage},
        // R1 50 MHz refit A152 at 45ff6daf: Plex_480p_r1.qsf:162 PLEX_PRESENT_480P=1, :193 ID 4800A152;
        // ddr_frame_layout_params.svh:30/:34 doorbell 0x300F_F000.
        {0x4800A152u, k480pPage},
        // Fixes refit 9E36 at ea4ca1e0: Plex_480p_r1.qsf:162 PLEX_PRESENT_480P=1, :199 ID 48009E36;
        // ddr_frame_layout_params.svh:30/:34 doorbell 0x300F_F000.
        {0x48009E36u, k480pPage},
        // P_PART port 2731 at 9801dad2: Plex_480p_r1.qsf:164 PLEX_PRESENT_480P=1, :207 ID 48002731;
        // ddr_frame_layout_params.svh:30/:34 doorbell 0x300F_F000 (unchanged from ea4ca1e0).
        {0x48002731u, k480pPage},
        // Deblock fit C88F at 7cb897b7 (dev/lane7-deblock-9e36, 9E36 + DEBLOCK_EN): the same r1 qsf
        // (:204 ID 4800C88F) and the same PLX page 0x300F_F000.
        {0x4800C88Fu, k480pPage},
        // P_PART ship build 3ECB at f32947c5 (dev/ppart-r-sub8: 2731 + the path R sub-8x8 refusal):
        // Plex_480p_r1.qsf:164 PLEX_PRESENT_480P=1, :212 ID 48003ECB; the same PLX page 0x300F_F000.
        {0x48003ECBu, k480pPage},
        // Track B 58BC at cb1e5f8b (dev/c88f-029a-status-side: C88F + fix21 + the Status block): the same r1
        // qsf (:207 ID 480058BC, PLEX_PRESENT_480P unchanged) and the same PLX page 0x300F_F000.
        {0x480058BCu, k480pPage},
        // D-PP1 94DD at 01d2e473 (dev/dpp1-joint-1006: C88F + P_PART + partitioned-bS): the same r1
        // qsf (:209 ID 480094DD, :167 PLEX_PRESENT_480P) and the same PLX page 0x300F_F000.
        {0x480094DDu, k480pPage},
        // T-DPP1-STATUS 48018AE0 at e7a38f4a (dev/dpp1-status-1007: the 94DD core + the 029A Status
        // block): the same r1 qsf (:213 ID 0x48018AE0, PLEX_PRESENT_480P unchanged) and the same PLX
        // page 0x300F_F000.
        {0x48018AE0u, k480pPage},
        // T-ASCAL-FOLD 48032F84 at 6b3832ac3 (dev/ascal-fold-1007: the 94DD core + the 4-cone ascal
        // fold): the same r1 qsf (:220 ID 0x48032F84, PLEX_PRESENT_480P unchanged) and the same PLX
        // page 0x300F_F000.
        {0x48032F84u, k480pPage},
        // T-ASCAL-FOLD-SYNC 48038D9A at 99fe93e1e (dev/asc-sync-baaeff6-1007: the same fold on the
        // E-port ascal): the same r1 qsf (BUILD_ID line only) and the same PLX page 0x300F_F000.
        {0x48038D9Au, k480pPage},
        // T-WEDGE-COUNTERS phase 1 48FF65F2 at 0a4c5f8fce0 (dev/wedge-p1-1007: the 18AE0 status-block core +
        // the detect-only wedge monitor): the same r1 qsf (BUILD_ID line only) and the same PLX page 0x300F_F000.
        {0x48FF65F2u, k480pPage},
        {0x487A1BD1u, k480pPage}, // 48FF65F2 + analog VGA restored (prod-480p/analog-vga-1011 853265774)
        {0x48C5385Cu, k480pPage}, // 487A1BD1 + sps_parser NVENC relax (c950 analog v10)
        {0x48C51FF1u, k480pPage}, // 48C5385C + pairing overlay both BUILD_IDs (analog v11)
        {0x48CFEE31u, k480pPage}, // 48C51FF1 + HDMI FB_FORCE_BLANK gate (analog v12)
        // 480p-prod NVENC relax 48110015 (df08dbe1e): Plex_480p_r1.qsf BUILD_ID; META_DPB doorbell 0x300F_F000.
        {0x48110015u, k480pPage},
        // High core 48A64AED: Plex_high.qsf, same 480p doorbell 0x300FF000. Fit in progress.
        {0x48A64AEDu, k480pPage},
        // F1 ship 24003997 (dev/f1-ship-240p-c4188b00: 240p v3 c4188b00 + PLEX_IDLE_FB_240P): Plex_240p.qsf
        // FPGA_VIDEO_BUILD_ID, PLEX_META_DPB=1; ddr_frame_layout_params.svh:30/:34 doorbell 0x300F_F000.
        {0x24003997u, k480pPage},
        {0x24F1A2C4u, k480pPage}, // 24003997 + P_PART + OSD + FB-hold (dev/240p-ppart-osd-1014)
        // 240p-prod NVENC relax 24A10026 (caa273c52): Plex_240p.qsf BUILD_ID; META_DPB doorbell 0x300F_F000.
        {0x24A10026u, k480pPage},
        {0x48F14A0Bu, k480pPage}, // 4801F40A + P_PART + OSD + statusBlock (dev/480i-ppart-osd-1014)
    };
    size_t rows = 0;
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        ++rows;
        const auto it = verified.find(b.id);
        expect(it != verified.end(),
               hex(b.id) + " has no verified PLX page: check its qsf in the cited tree first");
        if (it != verified.end())
            expect(fpgaPlxPage(b.id) == it->second && fpgaPlxPage(&b) == it->second,
                   hex(b.id) + " PLX page " + hex(fpgaPlxPage(b.id)) + " want " + hex(it->second));
    }
    expect(rows == verified.size(), "every verified row is still in the table");
    std::printf("PASS %zu recognised rows: PLX page 0x300FF000 (PRESENT_480P or META_DPB)\n", rows);
}

void unknownRows() {
    const uint32_t unknown[] = {0xDEADBEEFu, 0x00000000u, 0x4801F36Au /* retired */,
                                0x48006145u /* G1 lab, not tabled here */, 0x48005D44u};
    for (uint32_t id : unknown)
        expect(fpgaPlxPage(id) == 0u, hex(id) + " has no row: PLX page must be 0 (no reads)");
    expect(fpgaPlxPage(static_cast<const FpgaBuildInfo*>(nullptr)) == 0u,
           "no negotiated row: PLX page must be 0");
    std::printf("PASS unknown / retired / unnegotiated build: no PLX page, no PLX reads\n");
}

void probePages() {
    expect(kTrue480PlxPages.count == 1 && kTrue480PlxPages.page[0] == k480pPage,
           "probe true480 pages are the rows' pages: {0x300FF000}");
    std::printf("PASS probe true480 pages from the rows: {0x300FF000}\n");
}

void probeRows() {
    {   // L1 (review 00:36): stale L4 PLXJ + live 480P (idle: only PLXS moves).
        FakeDram d;
        staleL4Plxj(d);
        livePlxsHeartbeat(d, k480pPage);
        d.set(k480pPage + mailbox_abi::kPlxiOff, mailbox_abi::kPlxiMagic);
        expect(d.probe() == LiveGlass::True480, "stale L4 PLXJ + live idle 480P must be true480");
    }
    {   // Stale L4 PLXJ + 480P swapping banks (PLXD frames_done moves).
        FakeDram d;
        staleL4Plxj(d);
        d.move(k480pPage + mailbox_abi::kPlxdOff + 4u, 0x00050001u, 0x00060002u);
        expect(d.probe() == LiveGlass::True480, "stale L4 PLXJ + 480P bank swaps must be true480");
    }
    {   // Stale L4 PLXJ and nothing live: the leftover rule still gives L4.
        FakeDram d;
        staleL4Plxj(d);
        staticPlxsPlxi(d, k480pPage);
        expect(d.probe() == LiveGlass::L4, "static L4 PLXJ, nothing live: L4");
    }
    {   // Live L4 (PLXD swaps) over a leftover static 480P PLXS/PLXI.
        FakeDram d;
        staleL4Plxj(d);
        d.move(kL4Page + mailbox_abi::kPlxdOff + 4u, 0x00010001u, 0x00020002u);
        staticPlxsPlxi(d, k480pPage);
        expect(d.probe() == LiveGlass::L4, "live L4 over leftover 480P words: L4");
    }
    {   // A moving L4 word wins even when a true480 word moves too.
        FakeDram d;
        d.move(kL4Page + mailbox_abi::kPlxdOff + 4u, 0x00010001u, 0x00020002u);
        livePlxsHeartbeat(d, k480pPage);
        expect(d.probe() == LiveGlass::L4, "l4Changed is checked before t480Changed");
    }
    {   // Nothing anywhere.
        FakeDram d;
        expect(d.probe() == LiveGlass::True480, "empty DRAM: true480 default");
    }
    {   // N2 (review 01:51): idle L4 before its first PLXJ commit; only its PLXS heartbeat
        // moves, over leftover static 480P PLXS/PLXI magics.
        FakeDram d;
        livePlxsHeartbeat(d, kL4Page);
        staticPlxsPlxi(d, k480pPage);
        expect(d.probe() == LiveGlass::L4, "idle L4, no PLXJ yet, heartbeat only: L4");
    }
    {   // A leftover static L4 PLXS heartbeat word does not count as live.
        FakeDram d;
        d.set(kL4Page + mailbox_abi::kPlxsOff, mailbox_abi::kPlxsMagic);
        d.set(kL4Page + mailbox_abi::kPlxsOff + 4u, 0x00330000u);
        livePlxsHeartbeat(d, k480pPage);
        expect(d.probe() == LiveGlass::True480, "static L4 PLXS + live 480P: true480");
    }
    {   // Pages no row claims are not read: a core live at 0x3007F/0x301FF does not count.
        FakeDram d;
        staleL4Plxj(d);
        livePlxsHeartbeat(d, kNoMacroPage);
        livePlxsHeartbeat(d, k480iPage);
        expect(d.probe() == LiveGlass::L4, "0x3007F/0x301FF are not row pages: not probed");
    }
    {   // Exact read set: L4 PLXJ lo + PLXD hi + PLXS hi, then 480P PLXI lo, PLXD hi,
        // PLXS hi; twice.
        FakeDram d;
        (void)d.probe();
        const std::vector<uint32_t> want = {
            kL4Page + 0x130u, kL4Page + 0x12Cu, kL4Page + 0x104u, k480pPage + 0x108u,
            k480pPage + 0x12Cu, k480pPage + 0x104u,
        };
        std::vector<uint32_t> twice = want;
        twice.insert(twice.end(), want.begin(), want.end());
        std::string got;
        for (uint32_t a : d.reads)
            got += " " + hex(a);
        expect(d.reads == twice, "probe read set:" + got);
    }
    std::printf("PASS live-glass probe on the row table (L1: stale L4 PLXJ + live 480P -> "
                "true480; N2: idle L4 heartbeat -> L4; read set {0x3047F130,0x3047F12C,"
                "0x3047F104,0x300FF108,0x300FF12C,0x300FF104})\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--print-true480-pages") == 0) {
        for (size_t i = 0; i < kTrue480PlxPages.count; ++i)
            std::printf("%s0x%08X", i ? " " : "", kTrue480PlxPages.page[i]);
        std::printf("\n");
        return 0;
    }
    // ID:page of every recognised row, for the shell twin in probe_bitstream_ring.sh.
    if (argc == 2 && std::strcmp(argv[1], "--print-row-pages") == 0) {
        bool first = true;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
            std::printf("%s0x%08X:0x%08X", first ? "" : " ", b.id, fpgaPlxPage(&b));
            first = false;
        }
        std::printf("\n");
        return 0;
    }
    pageClassRows();
    recognisedRows();
    unknownRows();
    probePages();
    probeRows();
    if (fails) {
        std::fprintf(stderr, "test_plx_page: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_plx_page: PASS\n");
    return 0;
}
