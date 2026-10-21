// Recognised FPGA video builds (lead decision 2026-10-03 03:51; explicit
// status layout per row: release 05:04, V3-FIT-PRECHECK-1003).
//
// The core publishes its FPGA_VIDEO_BUILD_ID (qsf VERILOG_MACRO) in CAPS word
// MPC4 (ddr_bitstream_reader.sv TX_CAPS, VideoCapabilities::build_id).
// misterplexd plays only on the builds below, whose raster and status-word
// layout it has been checked against. Any other build is refused right after
// CAPS negotiation, before the first access unit. failFpgaSession stops the
// session, and the fpga-h264 backend never falls back to ARM present.
//
// Status layout, explicit in every row:
// - byte15Echoed / byte12Echoed: status_in byte 15 [127:120] / byte 12 [103:96]
//   echoes the user's OSD byte (OSD-MENU-SPEC-1002 section 2 splice) instead of
//   carrying telemetry (p3_recon_dbg / residual_dc).
// - confVersion: the CONF_STR "v,N" the core ships.
// Two layouts exist. v2 is 0/0/9: both bytes telemetry, AR [122:121] echoed.
// v3 is 1/1/10, the merged OSD layout (MPX_OSD_V10).
// The daemon decodes bytes 12/15 by the negotiated row (FpgaSpi::parseCoreStatus).
// A new RBF line needs its own ID and a row here, and a build whose layout
// changes must not reuse an ID. A retired ID is refused with its own reason
// (kRetiredFpgaBuilds).
#pragma once

#include "plx_page.hpp"
#include "status_telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace misterplex {

struct FpgaBuildInfo {
    uint32_t id;
    const char* raster;  // "480p", "240p" or "480i"
    // PLX mailbox page class: the present_core branch the row's qsf selects,
    // checked in the tree the lineage cites (plx_page.hpp).
    PlxPageClass plxPage;
    bool byte15Echoed;   // status_in byte 15 [127:120] echoes the OSD byte (else telemetry)
    bool byte12Echoed;   // status_in byte 12 [103:96] echoes the OSD byte (else telemetry)
    int confVersion;     // CONF_STR "v,N": 9 before the OSD menu, 10 with it
    const char* lineage; // qsf line and the RBFs known to report the ID
    // A/V sync (avsync45, release 19:22 10/3): avTrimMs is a signed trim added to the conf
    // AUDIO_DELAY_MS at play start (the effective delay is clamped at 0; + = audio later), so
    // the conf stays universal. avResync runs the A/V machinery of av_sync.hpp: the open-loop
    // drift compensation avDriftPpm (the audio content rate, ppm; - = audio slower, which
    // cancels an offset that falls over time) and the 40 ms excursion resync. Only the
    // 45/50 MHz N5 rows (and the 45 MHz R1 lab line) set them; every other row keeps
    // 0/false/0 and plays exactly as before.
    int avTrimMs = 0;
    bool avResync = false;
    double avDriftPpm = 0.0;
    // R1 host idle framebuffer (item 6 build kit 6c md5 bc95b8e1; DDR address map 9252a9a1, K1-K4
    // folded, accepted by review 16:39): while no decoded picture is live, the core
    // scans out the daemon's RGB24 idle buffers, selected by the PLXH word, so idle and browse
    // paint there instead of the I420 frame-store banks, with no YUV doorbell. A consumer must
    // also have the second WC window for the idle buffers (map K1: they sit outside the offset-0
    // 8 MiB map) or fall back to today's path. inputNav: PLEX_INPUT_NAV (item 6 MVP-b), mailbox
    // input codes 6..11. Both are false on every row but the R1 lab line 4800A1FB. The F1 240p row
    // (idleBeatPlxl below) also has hostIdleFb, without inputNav.
    bool hostIdleFb = false;
    bool inputNav = false;
    // F1 (PLEX_IDLE_FB_240P; F1-PREREG-1006 v2.1 910719de, v2.2 PLXL ba1ae917): the core turns its
    // MISTER_FB (the R1 idle buffers above, flipped by PLXH) on only while it sees the daemon's PLXL
    // beat, <PLX page> + 0x148 {hb32, 'PLXL'}. The beat thread writes PLXL only on a row with this
    // flag and only while the core gate is open on it (review 01:30 p2). Such a core is
    // publish-only: it never presents a host-painted bank, so idle paints only the R1 buffers.
    bool idleBeatPlxl = false;
    // Status block byte H (status byte 6 [55:48], OSD-MENU-SPEC-1002 section 9, STATUS-BYTE-H.md;
    // ported from the staged on-demand line 583885f5): the core echoes H[6:0] and drives H[7]
    // Running. The daemon writes its 2 s heartbeat (status_heartbeat.hpp) only while the core gate
    // is open on a row with this set. True on 480p Status-block refits and F1 240p 0x24003997
    // (Plex_240p.qsf MPX_STATUS_BLOCK). False on 480i 0x4801F40A until that fit adds the block
    // (writing H would stomp first_mb_type telemetry).
    bool statusBlock = false;
    // T-WEDGE-COUNTERS phase 1 (48FF65F2, dev/wedge-p1-1007): the build also carves a read-only
    // detect-only stall/wedge monitor page off the status telemetry (UIO WEDGE page, bytes 12/15),
    // so the daemon publishes it in addition to the 14-field row. Only the 0x48FF65F2 WEDGE-p1 row
    // carries this set; every other recognised row is false. `true` = the daemon may read the WEDGE page.
    bool wedgePage = false;
    // SPS relax for h264_nvenc Baseline (c950 analog v10 0x48C5385C, analog v11 0x48C51FF1): the
    // core's sps_parser takes a declared max_num_ref_frames 2, NAL/VCL HRD and pic_struct_present_flag 1,
    // and bitstream_restriction max_dec_frame_buffering <= 2. CAPS/MPC1 is unchanged (maxRefs 1, no grant
    // bits), so active references stay <= 1 (G18/G26) and RPLM/MMCO stay refused. True on 0x48C5385C,
    // 0x48C51FF1, 0x48CFEE31, the 480p-prod refit 0x48110015, and the 240p-prod refit 0x24A10026.
    // Prod analog v9 0x487A1BD1 and selected 240p 0x24F1A2C4 stay false.
    bool spsRelaxNvenc = false;
    // High/CABAC core (0x48A64AED only, Plex_high.qsf). The core accepts a High or
    // CABAC AU, so the daemon must not restart that AU as the baseline ladder.
    // False on every baseline row. CLOSED RBF s5 353e7c6d; kSelectedCoreId480p stays 0x487A1BD1.
    bool highCabac = false;
};

// v2 rows: Plex.sv CONF_STR "v,9;" (Plex.sv:144 at each cited commit).
// v3 rows: "v,10;" under MPX_OSD_V10 (Plex.sv:172 at 73de0211).
// plxPage: 480p rows set PLEX_PRESENT_480P, the 240p/480i rows PLEX_META_DPB, in
// the qsf of the commit each lineage cites (2400F36A: 96a5c60e, RBF 5554d013's
// source); every recognised row publishes PLX at 0x300FF000.
inline constexpr PlxPageClass kPlx300FF = PlxPageClass::Present480pOrMetaDpb;
inline constexpr FpgaBuildInfo kRecognisedFpgaBuilds[] = {
    {0x4800F36Au, "480p", kPlx300FF, false, false, 9,
     "Plex_480p.qsf F36A line: prod RBF 7cbae324 (82ef65c s2 map report), 9fc1f6ca qsf"},
    {0x48004D57u, "480p", kPlx300FF, false, false, 9, "L2 7d3e4b5b (RBF 7bccc0e0)"},
    {0x48004D40u, "480p", kPlx300FF, false, false, 9, "RC 47cb9fb4 (RBF 999800cf)"},
    {0x48005D40u, "480p", kPlx300FF, false, false, 9,
     "next RC 97584eaf: RC 47cb9fb4 + LANE7-N5 (refit pending)"},
    // 45/50 MHz A/V: soak A (RBF 534cb8e7 + daemon c5334354, AUDIO_DELAY_MS=50,
    // lab-runs/soak45-1003) measured HDMI +52.7 ms over the first 180 s (N5: +36.3) and a
    // smooth -0.874 ms/min drift (p50 +40.0, p95 66.0). Trim -45 ms puts the start near
    // +7.7 ms; -14.6 ppm of audio content rate (0.874 ms/min) cancels the drift. 50 MHz has no
    // soak yet and takes the 45 MHz numbers.
    {0x48005D45u, "480p", kPlx300FF, false, false, 9,
     "N5 2b625b7c + clk_dec 45 MHz: 4c210d99 (Plex_480p.qsf:178, RBF 534cb8e7)", -45, true,
     -14.6},
    {0x48005D50u, "480p", kPlx300FF, false, false, 9,
     "N5 2b625b7c + clk_dec 50 MHz: 2b770497 (Plex_480p.qsf:178, RBF 7fdb10b9)", -45, true,
     -14.6},
    // dec50 cut 4 (review; ID by release 00:47 10/4; row 02:14): its own ID, the 48005D50 row in
    // every field, including the trim, the drift compensation and the resync (as 48005D51 was).
    {0x48005D52u, "480p", kPlx300FF, false, false, 9,
     "dec50 cut 4: DPB_SPAN_CUT 9c86d9b2 on cut 3 62c6df94 (dev/dec50-cut4); "
     "RBF s2 d53bdd17 (glass), s9 b04cbb84 (backup)", -45, true,
     -14.6},
    // R1 + MVP-b lab line (release 16:26; re-ID 17:02 from 0x48005D47, 1 bit from 48005D45, to
    // 0x4800A1FB, >=5 bits from every known ID, before any RBF): the 48005D45 core (4c210d99,
    // clk_dec 45 MHz) plus PLEX_HOST_IDLE_FB and PLEX_INPUT_NAV (lab qsf Plex_480p_r1.qsf), with
    // its own ID (MVP-b M4). The clocks and the audio path are 48005D45's, so are the A/V numbers.
    // Its SDRAM_DIAG word is on the 480p PLX page (0x300FF120), not 0x3007F120.
    {0x4800A1FBu, "480p", kPlx300FF, false, false, 9,
     "R1 host idle FB + MVP-b input nav on 4c210d99 (dev/r1-idlefb-mvpb, Plex_480p_r1.qsf); "
     "RBF 2caa9483", -45, true, -14.6, true, true},
    // R1 50 MHz refit (release 08:58 ID 0x4800A152, review 09:11; wQ dev/r1-refit-dec50
    // 45ff6daf): dec50 cut 4 (48005D52, 9c86d9b2, RBF d53bdd17) plus PLEX_HOST_IDLE_FB and
    // PLEX_INPUT_NAV. Plex_480p_r1.qsf :193 the ID, :199/:202 the macros, :284/:289/:290 clk_dec
    // M16/DIV16 (the 5D52 clock); Plex.sv :158 "v,9"; the PLX page 0x300FF000
    // (ddr_frame_layout_params.svh :30/:34, PLXH/PLXO :110-111, DIAG :39); bytes 15 and 12 telemetry
    // (Plex.sv :2171-2175, as 9c86d9b2). The decode/present path and the A/V numbers are 48005D52's.
    {0x4800A152u, "480p", kPlx300FF, false, false, 9,
     "R1 host idle FB + MVP-b input nav, clk_dec 50 MHz, on dec50 cut 4 9c86d9b2 (RBF d53bdd17): "
     "dev/r1-refit-dec50 45ff6daf (Plex_480p_r1.qsf); RBF s2 daf64dc3", -45, true,
     -14.6, true, true},
    // Fixes refit (release 10:44 ID 0x48009E36, review 10:45; wQ ea4ca1e0, RBF s2 c74cf4dc, review 12:30):
    // 4800A152 + BANK2-ANALOG F-a (PLEX_FS_BANK3) + s11 fix (a) PLEX_S11_GATHER + analog idle blank (iii)
    // PLEX_ANALOG_IDLE_BLANK. Plex_480p_r1.qsf :199 the ID, :162 PLEX_PRESENT_480P, :205/:208 the R1
    // macros, :229 PLEX_480P_LEAN, :290/:295/:296 clk_dec M16/DIV16; Plex.sv :158 "v,9"; the PLX page
    // 0x300FF000 (ddr_frame_layout_params.svh :30/:34); bytes 15 and 12 telemetry (Plex.sv :2194-2199,
    // byte-identical to 45ff6daf :2171-2176). The daemon ABI is A152's (the PLXD [36]/[37] bits are not
    // decoded, review 10:06), so every field but the ID and the lineage is 4800A152's.
    {0x48009E36u, "480p", kPlx300FF, false, false, 9,
     "R1 + F-a bank-2 analog + s11 gather + analog idle blank, clk_dec 50 MHz: fixes head "
     "ea4ca1e0 (Plex_480p_r1.qsf); RBF s2 c74cf4dc", -45, true, -14.6, true, true},
    // P_PART port on 9E36 (release 10/05; ID 0x48002731 adopted by review 16:41 from w9's idsearch,
    // >= 7 bits from every known ID): 48009E36 + PLEX_P_PART_PRODUCT (P16x8/P8x16/P8x8, the
    // dev/480p-ppart-m10k 73d46ad7 RTL merged at ccea7d23). dev/ppart-9e36 9801dad2
    // Plex_480p_r1.qsf :207 the ID, :72 PLEX_P_PART_PRODUCT, :164 PLEX_PRESENT_480P, :237 PLEX_480P_LEAN.
    // fpga/ differs from ea4ca1e0 only in h264_mb_ctrl.sv, h264_inter_pred.sv and the qsf; the macro is
    // used under rtl/ only, so Plex.sv ("v,9", PLX page 0x300FF000, bytes 15/12 telemetry,
    // VIDEO_FUNCTIONAL_FEATURES :1054-1057) is 9E36's and every field but the ID and the lineage is 48009E36's.
    // SUPERSEDED by 0x48003ECB, never shipped (review 18:42, review 18:42): kept as an inert accept row at
    // the base of the canon stack. No RBF carries this ID, and no tree with it active may be fitted (release
    // gates fits). 48005D46 (5D45 + P_PART) stays must-refuse.
    {0x48002731u, "480p", kPlx300FF, false, false, 9,
     "R1 + F-a bank-2 analog + s11 gather + analog idle blank + P_PART (P16x8/P8x16/P8x8), clk_dec "
     "50 MHz: dev/ppart-9e36 9801dad2 (Plex_480p_r1.qsf); no RBF yet", -45, true, -14.6, true, true},
    // Deblock fit (lineage label "9E3x"; ID 0x4800C88F per review 16:41, from w9's 16:40 search: min
    // Hamming 7 vs every known ID, 9 from 48009E36, 13 from P_PART's 48002731): 48009E36 (ea4ca1e0) +
    // the lane7 in-loop deblock DEBLOCK_EN, dev/lane7-deblock-9e36 7cb897b7. Plex_480p_r1.qsf :204 the
    // ID, :218 DEBLOCK_EN; never with PLEX_P_PART_PRODUCT. CAPS adds Deblock (1u << 6;
    // VIDEO_FUNCTIONAL_FEATURES 0x0000e1ff), and deblocked streams need MPX_H264_FILTER=on. Every field
    // but the ID and the lineage is 48009E36's. Fit of record: 7cb897b7 seed 9 (sweep59), RBF 5bdd22da
    // (review 18:39 fit-record ACK; seeds 5 b592ff44 and r2 s2 f579994c are quarantined NEGSLACK).
    {0x4800C88Fu, "480p", kPlx300FF, false, false, 9,
     "9E36 + lane7 in-loop deblock (DEBLOCK_EN), clk_dec 50 MHz: dev/lane7-deblock-9e36 "
     "7cb897b7 (Plex_480p_r1.qsf); RBF s9 5bdd22da", -45, true, -14.6, true, true},
    // P_PART ship build (lineage label "9E3x"; ID 0x48003ECB per review 18:39, from w9's idsearch3 a607a103:
    // the only 0x4800xxxx id at min Hamming 7 vs the 65-id set, 9 from 48009E36 and 48002731, 8 from 4800C88F):
    // 48002731 + F-PPART-SUB8 path R (review 18:16): h264_mb_ctrl H_SUB refuses P sub_mb_type != 0 with
    // decode_error 22 (dev/ppart-r-sub8 ce74cb00; RTLINV tripwire cb676640; ID commit f32947c5,
    // Plex_480p_r1.qsf :212 the ID, :72 PLEX_P_PART_PRODUCT, :164 PLEX_PRESENT_480P, :242 PLEX_480P_LEAN).
    // The refusal is under rtl/ only, so every field but the ID and the lineage is 48009E36's (as 48002731's).
    // A sub-8x8 stream ends the session on the MVPS error (media_player :3483-3485); nothing partial is shown.
    {0x48003ECBu, "480p", kPlx300FF, false, false, 9,
     "R1 + F-a bank-2 analog + s11 gather + analog idle blank + P_PART (P16x8/P8x16/P8x8; sub-8x8 refused, "
     "decode_error 22), clk_dec 50 MHz: dev/ppart-r-sub8 f32947c5 (Plex_480p_r1.qsf); no RBF yet",
     -45, true, -14.6, true, true},
    // Track B (tonight 2026-10-06; ID 0x480058BC: release 17:28 with review 17:28; min Hamming 6 over the origin
    // and crew IDs, the most any 0x4800xxxx ID reaches; the local-only S1 SPS-crop lab row 0x480051AC, 3 bits away,
    // must never ship while this row is live): C88F 7cb897b7 + fix21 43f22ffd (store only) + the read-only Status
    // block (MPX_STATUS_BLOCK; 029A's 8cdd8cfa RTL verbatim), dev/c88f-029a-status-side cb1e5f8b,
    // Plex_480p_r1.qsf :207 the ID, :221 DEBLOCK_EN, :227/:228 MPX_STATUS_BLOCK + rtl/plex_status_block.sv.
    // Status byte 6 [55:48] is H (the daemon's heartbeat; H[7] the core's Running), no longer first_mb_type; the
    // CONF_STR stays "v,9" and bytes 15/12 stay telemetry. Every field but the ID, the lineage and statusBlock is
    // 4800C88F's: idleBeatPlxl (13th) false, statusBlock (14th) true.
    {0x480058BCu, "480p", kPlx300FF, false, false, 9,
     "C88F + fix21 + read-only Status block (MPX_STATUS_BLOCK), clk_dec 50 MHz: dev/c88f-029a-status-side "
     "cb1e5f8b (Plex_480p_r1.qsf); no RBF yet",
     -45, true, -14.6, true, true, false, true},
    // D-PP1 joint (release 20:28 ID 0x480094DD, review 20:28 CLEAR; wW 9bb4fdde, min Hamming 7 vs
    // recognised+retired). C88F 43f22ffd (in-loop deblock + fix21) + 3ECB P_PART (ea4ca1e0..f32947c5)
    // + P1 PLEX_MB_PLANE_ONLY + partitioned-bS that lifts the DEBLOCK_EN x PLEX_P_PART_PRODUCT guard.
    // Plex_480p_r1.qsf @01d2e473 :209 the ID, :72 PLEX_P_PART_PRODUCT, :223 DEBLOCK_EN, :215/:218 R1
    // macros, :244 PLEX_480P_LEAN, :305/:310/:311 clk_dec M16/DIV16. No MPX_STATUS_BLOCK. Every field
    // but the ID and the lineage is 4800C88F's (hostIdleFb/inputNav true, statusBlock false). RBF s3
    // 3f5213ab (review 00:13/00:14 fit CLEAR). g1 CAPS 0x0000E1FF / 640x480 / max_au 0x20000 (same features
    // word as C88F; P_PART is not a CAPS bit).
    {0x480094DDu, "480p", kPlx300FF, false, false, 9,
     "D-PP1 joint: C88F 43f22ffd + 3ECB P_PART + P1 + partitioned-bS, clk_dec 50 MHz: "
     "dev/dpp1-joint-1006 01d2e473 (Plex_480p_r1.qsf); RBF s3 3f5213ab",
     -45, true, -14.6, true, true},
    // T-DPP1-STATUS joint (tonight 2026-10-07; ID 0x48018AE0, min Hamming 8 over the fleet): the
    // D-PP1 94DD core (C88F 43f22ffd + 3ECB P_PART + P1 + partitioned-bS; 01d2e473, RBF 3f5213ab)
    // with the 029A read-only Status block on top (MPX_STATUS_BLOCK; the 8cdd8cfa RTL verbatim),
    // dev/dpp1-status-1007 e7a38f4a on the menu-clean 94DD base bdf3dec4e. Plex_480p_r1.qsf: the
    // ID; PLEX_P_PART_PRODUCT; DEBLOCK_EN; the R1 macros; PLEX_480P_LEAN; MPX_STATUS_BLOCK=1 and
    // rtl/plex_status_block.sv (no MPX_OSD / MPX_STATUS_SET_ARM). Status byte 6 [55:48] is H (the
    // daemon's heartbeat; H[7] the core's Running); the CONF_STR stays "v,9" and bytes 15/12 stay
    // telemetry. Every field but the ID, the lineage and statusBlock is 4800C88F's: idleBeatPlxl
    // (13th) false, statusBlock (14th) true.
    {0x48018AE0u, "480p", kPlx300FF, false, false, 9,
     "D-PP1 joint + read-only Status block (MPX_STATUS_BLOCK), clk_dec 50 MHz: dev/dpp1-status-1007 "
     "e7a38f4a (Plex_480p_r1.qsf); no RBF yet",
     -45, true, -14.6, true, true, false, true},
    // T-WEDGE-COUNTERS phase 1 (ID 0x48FF65F2, min Hamming 13 over the fleet): the 48018AE0 D-PP1/94DD
    // status-block core with the detect-only stall/wedge monitor on top (PLEX_WEDGE_MON=1, select
    // status[119], UIO WEDGE page bytes 12/15), dev/wedge-p1-1007 0a4c5f8fce0 on the wedge-p1 branch
    // from the tip of dev/dpp1-status-1007. Same Plex_480p_r1.qsf :214 BUILD_ID, same 480p v2 layout
    // (PLEX_480P_LEAN=1, no OSD echo, no bytes 15/12 echo). Every field is 48018AE0's except the new
    // wedgePage flag (15th) true: the counters are read-only side-outputs off the status telemetry (no
    // status-block byte rewrite), so the daemon owns the WEDGE page read/publish. FIT CLOSED s3 3324fcbc.
    {0x48FF65F2u, "480p", kPlx300FF, false, false, 9,
     "detect-only stall/wedge counters on the D-PP1/18AE0 status-block core: dev/wedge-p1-1007 "
     "0a4c5f8fce0 (Plex_480p_r1.qsf:214, PLEX_WEDGE_MON=1, select status[119], UIO WEDGE page bytes 12/15, "
     "read-only side-outputs); 480p v2 PLEX_480P_LEAN=1, no OSD echo; RBF s3 3324fcbc",
     -45, true, -14.6, true, true, false, true, true},
    // Prod 480p + analog VGA restored (ID 0x487A1BD1, min Hamming 9 over 201 0x48 IDs): the 48FF65F2 build with
    // PLEX_NO_ANALOG_VGA commented out (HD15 640x480p 31.47 kHz RGBHV), prod-480p/analog-vga-1011 853265774 (parent
    // 0a4c5f8fc), Plex_480p_r1.qsf, fit s7 CLOSED (+0.242 setup). Every field is 48FF65F2's but the ID and lineage.
    {0x487A1BD1u, "480p", kPlx300FF, false, false, 9,
     "48FF65F2 + analog VGA restored (PLEX_NO_ANALOG_VGA off): prod-480p/analog-vga-1011 853265774 "
     "(Plex_480p_r1.qsf, parent 0a4c5f8fc); 480p v2 PLEX_480P_LEAN=1, no OSD echo; RBF s7 89fd62e2",
     -45, true, -14.6, true, true, false, true, true},
    // Analog v10 SPS relax for h264_nvenc Baseline (ID 0x48C5385C, min Hamming 8 vs 0x48C5B317):
    // the 487A1BD1 analog-v9 build with the sps_parser relax (refs<=2 declared, NAL/VCL HRD,
    // pic_struct, decbuf<=2), c950-spsrelax-analog-v10-fit e1c118511 on 3f23f30dae, Plex_480p_r1.qsf
    // seed 9 CLOSED (+0.324/+0.113). Every field is 487A1BD1's but the ID, the lineage and
    // spsRelaxNvenc. RBF e6a011d0875444472c95cc1c4a243c18. Never reuse 0x48FF65F2 / 0x48FE178F.
    {0x48C5385Cu, "480p", kPlx300FF, false, false, 9,
     "487A1BD1 + sps_parser NVENC relax (refs 2 declared, HRD, pic_struct, decbuf<=2): "
     "c950-spsrelax-analog-v10-fit e1c118511 (Plex_480p_r1.qsf BUILD_ID 0x48C5385C, parent 3f23f30dae); "
     "480p v2 PLEX_480P_LEAN=1, analog VGA on; RBF s9 e6a011d0",
     -45, true, -14.6, true, true, false, true, true, true},
    // Analog v11 pairing overlay (ID 0x48C51FF1, Hamming 9 vs 0x48C5385C, 7 vs 0x48C534E2):
    // 48C5385C + plex_daemon_down_osd both BUILD_IDs (239388c6b / 83413fc30). Same v2 / A/V /
    // statusBlock / spsRelaxNvenc as v10. CLOSED seed 5: 36844/41910 (88%), setup 0.374,
    // hold 0.102, CDC 0, md5 6365a3d6. Never reuse 0x48C5385C. kSelectedCoreId480p stays
    // prod 0x487A1BD1 while owner tests.
    {0x48C51FF1u, "480p", kPlx300FF, false, false, 9,
     "48C5385C + pairing overlay both BUILD_IDs (dev/analog-v11-pair-osd-1014 83413fc30, parent 239388c6b, "
     "Plex_480p_r1.qsf 32'h48C51FF1); 480p v2 PLEX_480P_LEAN=1, analog VGA on; RBF s5 6365a3d6",
     -45, true, -14.6, true, true, false, true, true, true},
    // Analog v12 HDMI daemon-down (ID 0x48CFEE31, Hamming 9 vs 0x48C51FF1, 12 vs 0x48C5385C):
    // 48C51FF1 + FB_FORCE_BLANK gated by FB_EN so ascal does not zero HDMI when the
    // native plex_daemon_down_osd is on VGA_* (F017 480p.jpg max=2). Same v2 / A/V /
    // statusBlock / spsRelaxNvenc / hostIdleFb as v11. Never reuse 0x48C51FF1.
    // kSelectedCoreId480p stays prod 0x487A1BD1 while owner tests. CLOSED seed 5:
    // 36794/41910 (88%), setup 0.097, hold 0.056, CDC 0, md5 052990c6.
    {0x48CFEE31u, "480p", kPlx300FF, false, false, 9,
     "48C51FF1 + HDMI FB_FORCE_BLANK gated by FB_EN (dev/analog-v12-hdmi-osd-1014, "
     "Plex_480p_r1.qsf 32'h48CFEE31); 480p v2 PLEX_480P_LEAN=1, analog VGA on; RBF s5 052990c6",
     -45, true, -14.6, true, true, false, true, true, true},
    // High core F057 CLOSED (Plex_high.qsf BUILD_ID 0x48A64AED, dev/high-next-rbf-1014
    // bd2c4fbd4). QSF: MPX_STATUS_BLOCK, PLEX_WEDGE_MON, PLEX_INPUT_NAV, CONF_STR v,9.
    // PLEX_HOST_IDLE_FB is commented out. Min Hamming 9. Not selected.
    // kSelectedCoreId480p stays 0x487A1BD1. Seed 5 setup +0.152 hold +0.106 CDC 0, md5 353e7c6d.
    {0x48A64AEDu, "480p", kPlx300FF, false, false, 9,
     "Plex_high.qsf 32'h48A64AED (dev/high-next-rbf-1014 bd2c4fbd4); High/CABAC; "
     "status block, wedge, input nav; no host idle FB; RBF s5 353e7c6d; not selected",
     0, false, 0.0, false, true, false, true, true, false, true},
    // 480p-prod NVENC relax (ID 0x48110015, dev fit df08dbe1e). Same daemon fields as
    // prod 0x487A1BD1 except spsRelaxNvenc. kSelectedCoreId480p stays 0x487A1BD1.
    // CLOSED seed 5: setup 0.430, hold 0.176. RBF md5 532e5456. Not a High core.
    {0x48110015u, "480p", kPlx300FF, false, false, 9,
     "487A1BD1 + sps_parser NVENC relax (df08dbe1e, Plex_480p_r1.qsf BUILD_ID 0x48110015); "
     "480p v2 PLEX_480P_LEAN=1, analog VGA on; RBF s5 532e5456; not selected",
     -45, true, -14.6, true, true, false, true, true, true},
    // T-ASCAL-FOLD 48032F84 (dev/ascal-fold-1007, 6b3832ac3): the 480094DD row, the ID and lineage
    // only different: the ascal 4-cone fold (cones A/B/C-h/C) onto the D-PP1 core, same 480p r1 qsf
    // (BUILD_ID alone), same PLX page and A/V tracking row; no status block; no RBF yet (fit pending).
    {0x48032F84u, "480p", kPlx300FF, false, false, 9,
     "ascal 4-cone fold onto the D-PP1 94DD core (C88F 43f22ffd + 3ECB P_PART + P1 + partitioned-bS), "
     "clk_dec 50 MHz (M16/C16): dev/ascal-fold-1007 6b3832ac3 (Plex_480p_r1.qsf); no RBF yet",
     -45, true, -14.6, true, true},
    // T-ASCAL-FOLD-SYNC 48038D9A (dev/asc-sync-baaeff6-1007, 99fe93e1e): the 48032F84 row, the ID
    // and lineage only different: the same 4-cone fold (A + B + C-h + C) but onto the E-port
    // (baaeff6) ascal — the upstream o_state vsync-reset (e759585c902) folded under cone A, proven
    // == plain-fold (6b3832ac3) + the 16-line E-port delta and == bchc + cone A (tb/asc-fold-proof).
    // Same 480p r1 qsf (BUILD_ID alone), same PLX page and A/V tracking row; no status block;
    // no RBF yet (fit pending).
    {0x48038D9Au, "480p", kPlx300FF, false, false, 9,
     "ascal 4-cone fold (A+B+C-h+C) onto the 94DD core, E-port ascal (= bchc+coneA ="
     " plain-fold+E-port): dev/asc-sync-baaeff6-1007 99fe93e1e (Plex_480p_r1.qsf); no RBF yet",
     -45, true, -14.6, true, true},
    {0x4800F40Au, "480p", kPlx300FF, false, false, 9,
     "F40A line, clk_dec 40 MHz: 8c4c83f8 (RBF 9dcba0ba)"},
    {0x48006A7Eu, "480p", kPlx300FF, false, false, 9, "fstore scanout gate cb60f749 (RBF 6a29ef7d)"},
    {0x2400F36Au, "240p", kPlx300FF, false, false, 9,
     "Plex_240p.qsf raster on the 480p ABI, mpx240-integ 96a5c60e (RBF 5554d013)"},
    {0x2400F40Bu, "240p", kPlx300FF, true, true, 10,
     "240p OSD line, one fit: OSD menu + [127:120] echo fix c4188b00 + the later handshake; "
     "ID e6493329 (Plex_240p.qsf:194, MPX_OSD_V10 :219 at 73de0211; gate 1c537e0f); no RBF yet"},
    // F1 ship (A) (review 12:45): the 240p v3 line c4188b00 (the 2400F40B status layout v3: bytes 15/12
    // echoed, "v,10") plus PLEX_IDLE_FB_240P and MISTER_FB, no redraw. ID 24003997: release 12:46 (min
    // Hamming 9 vs 74 IDs). The lab F1 ID 24000EFD (the redraw line + F1) is retired below.
    {0x24003997u, "240p", kPlx300FF, true, true, 10,
     "F1 240p idle FB (A): 240p v3 c4188b00 + PLEX_IDLE_FB_240P (dev/f1-ship-240p-c4188b00, "
     "Plex_240p.qsf FPGA_VIDEO_BUILD_ID + MPX_STATUS_BLOCK); RBF s5 9df56ef3 (8add8de1)",
     0, false, 0.0, true, false, true, true},
    // 24F1A2C4 (dev/240p-ppart-osd-1014 57beec621): F1 24003997 + PLEX_P_PART_PRODUCT
    // (remote PMS epoch 20) + daemon-down/pairing OSD + plex_fb_en_hold + sequential MC_WIDE
    // + analog P1 plane-only + analog COLRAM-under-P_PART COL_MV4. Hamming 12 vs 0x48018AE0.
    // CLOSED seed 5: 37054/41910 (88%), setup 0.009, hold 0.118, CDC 0, md5 247b062f.
    {0x24F1A2C4u, "240p", kPlx300FF, true, true, 10,
     "F1 240p P_PART+OSD+FB-hold+MC_WIDE+P1+COLRAM: 24003997 + PLEX_P_PART_PRODUCT + plex_daemon_down_osd + "
     "plex_fb_en_hold (dev/240p-ppart-osd-1014 57beec621, Plex_240p.qsf 32'h24F1A2C4); RBF s5 247b062f",
     0, false, 0.0, true, false, true, true},
    // 240p-prod NVENC relax (ID 0x24A10026, dev fit caa273c52). Same daemon fields as
    // selected 0x24F1A2C4 except spsRelaxNvenc. kSelectedCoreId240p stays 0x24F1A2C4.
    // CLOSED seed 5: setup 0.444, hold 0.109. RBF md5 a39c2808. Not a High core.
    {0x24A10026u, "240p", kPlx300FF, true, true, 10,
     "24F1A2C4 + sps_parser NVENC relax (caa273c52, Plex_240p.qsf BUILD_ID 0x24A10026); "
     "RBF s5 a39c2808; not selected",
     0, false, 0.0, true, false, true, true, false, true},
    // 240p lab (c078/c135 Plex_240p_lab.qsf): FPGA_VIDEO_BUILD_ID 32'h24C07821, PLEX_WEDGE_MON,
    // PLEX_SPS_RELAX, no MPX_OSD_V10 so v2 0/0/9, no MPX_STATUS_BLOCK. Hamming 8 vs 0x2400F40B.
    // RBF_PATH=NONE / RBF_MD5=NONE: do not load this id until a seed-3 fit closes slack.
    {0x24C07821u, "240p", kPlx300FF, false, false, 9,
     "Plex_240p_lab.qsf FPGA_VIDEO_BUILD_ID 32'h24C07821 (c078/c135; PLEX_WEDGE_MON, PLEX_SPS_RELAX, "
     "no MPX_OSD_V10, no MPX_STATUS_BLOCK); no RBF yet"},
    {0x4801F40Au, "480i", kPlx300FF, true, true, 10,
     "525ef6c5 (b594453b = 1c537e0f on d2b3cb03), v,10 with [127:120] and [103:96]; "
     "ID Plex_480i.qsf:185 (FPGA_VIDEO_BUILD_ID), "
     "MPX_OSD_V10 :230 at 525ef6c5"},
    // 48F14A0B (dev/480i-ppart-osd-1014 2ad700e7a): 4801F40A + PLEX_P_PART_PRODUCT
    // (remote PMS epoch 20) + daemon-down/pairing OSD + MPX_STATUS_BLOCK + sequential MC_WIDE
    // + analog P1 plane-only + analog COLRAM-under-P_PART COL_MV4. Hamming 9 vs 0x4800C88F.
    // CLOSED seed 5: 39010/41910 (93%), setup 0.007, hold 0.091, CDC 0, md5 203158fe.
    {0x48F14A0Bu, "480i", kPlx300FF, true, true, 10,
     "480i P_PART+OSD+statusBlock+MC_WIDE+P1+COLRAM: 4801F40A + PLEX_P_PART_PRODUCT + plex_daemon_down_osd + "
     "MPX_STATUS_BLOCK (dev/480i-ppart-osd-1014 2ad700e7a, Plex_480i.qsf 32'h48F14A0B); RBF s5 203158fe",
     0, false, 0.0, false, false, false, true},
    // 480i lab (c078/c136 Plex_480i_lab.qsf): FPGA_VIDEO_BUILD_ID 32'h48C078E2, same macros as
    // the 240p lab, PLEX_DECODE_480P off. Hamming 8 vs 0x48003ECB. Product 480i stays 4801F40A
    // (v3). RBF_PATH=NONE / RBF_MD5=NONE.
    {0x48C078E2u, "480i", kPlx300FF, false, false, 9,
     "Plex_480i_lab.qsf FPGA_VIDEO_BUILD_ID 32'h48C078E2 (c078/c136; PLEX_WEDGE_MON, PLEX_SPS_RELAX, "
     "no MPX_OSD_V10, no MPX_STATUS_BLOCK, PLEX_DECODE_480P off); no RBF yet"},
};

// PlexOB stamps are not legacy CAPS/PLX rows. A row here would make the daemon
// treat the core as a Plex.sv build (PLX page, display latch). The play gate is
// owenb_core_identity.hpp. This is the matching BUILD_ID registry.
struct PlexObBuildRow {
    uint32_t id;
    const char* label;
    const char* lineage;
};

inline constexpr PlexObBuildRow kPlexObBuildRows[] = {
    {0x0B59C11Au,
     "PlexOB Y/C word 87",
     "fpga/owenb-combo-s9/PlexOB_480p/vendor/H264.qsf MPX_PLEXOB_BUILD_ID 32'h0B59C11A "
     "(Y/C: O[21:20] analog, O[23:22] NTSC color; O[24] reserved for 240p; "
     "ring word 87 [3:0]=status[23:20], [9:4] tied 0); ring_in restored; "
     "dead fit 0x0B59C119 is not this id; shipped PlexOB RBF md5 c8b34604"},
};

inline const PlexObBuildRow* recognisedPlexObBuildRow(uint32_t id) {
    for (const PlexObBuildRow& b : kPlexObBuildRows) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

// Retired IDs: refused with a reason, never matched as an unknown build.
struct FpgaBuildRetired {
    uint32_t id;
    const char* reason;
};

inline constexpr FpgaBuildRetired kRetiredFpgaBuilds[] = {
    {0x4801F36Au,
     "480i ID shared by two status layouts: 42da8d23 (RBFs ad047c7a, 5b5403ff; v2) and 94cdeef9 "
     "s2 cc7b6e66 (v3 RTL behind a v2 ID), so the layout cannot be known; 480i plays as 4801F40A "
     "(94cdeef9 successor, merged OSD layout), and cc7b6e66 glasses on daemon dd0bcb08 only"},
    {0x24000EFDu,
     "F1 lab ID: the 2400F40C redraw line 35cfd26e + PLEX_IDLE_FB_240P (RBF ca0afbe4, lab only, review "
     "09:16); the ship build is 24003997 (240p v3 c4188b00 + F1, review 12:45)"},
};

inline const FpgaBuildInfo* recognisedFpgaBuild(uint32_t id) {
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

// The row that chooses the PMS ladder. The session row is set only after CAPS,
// which is after resolve, so an open core gate's build id is the one that counts
// before the first AU. A baseline row, a closed gate, or an unknown id is not High.
inline bool highCabacForPlay(const FpgaBuildInfo* negotiated, bool gateOpen, uint32_t gateBuild) {
    const FpgaBuildInfo* row = negotiated;
    if (!row && gateOpen)
        row = recognisedFpgaBuild(gateBuild);
    return row && row->highCabac;
}

// The PLX mailbox page of a recognised build, or 0 with no row: an unknown,
// retired or unnegotiated build gets no PLX reads.
inline constexpr uint32_t fpgaPlxPage(const FpgaBuildInfo* b) {
    return b ? plxPageForClass(b->plxPage) : 0u;
}

inline uint32_t fpgaPlxPage(uint32_t id) { return fpgaPlxPage(recognisedFpgaBuild(id)); }

// The FPGA audio start delay for a play (avsync45): the conf AUDIO_DELAY_MS plus the row's
// signed trim, clamped at 0. Rows without a trim (every row but 48005D45/4800A1FB/48005D50/
// 48005D52) give the conf value unchanged.
inline int fpgaAudioDelayMs(int confDelayMs, const FpgaBuildInfo* b) {
    const int d = confDelayMs + (b ? b->avTrimMs : 0);
    return d < 0 ? 0 : d;
}

inline const FpgaBuildRetired* retiredFpgaBuild(uint32_t id) {
    for (const FpgaBuildRetired& r : kRetiredFpgaBuilds) {
        if (r.id == id)
            return &r;
    }
    return nullptr;
}

// "v3" (1/1, v,10), "v2" (0/0, v,9), or "mixed" (no core ships one; the table test rejects it).
inline const char* fpgaStatusLayoutName(const FpgaBuildInfo& b) {
    if (b.byte15Echoed && b.byte12Echoed && b.confVersion == 10)
        return "v3";
    if (!b.byte15Echoed && !b.byte12Echoed && b.confVersion == 9)
        return "v2";
    return "mixed";
}

// True when the build's status_in byte (0-15, byte n = bits [8n+7:8n]) is an
// echoed OSD byte rather than telemetry.
inline bool fpgaStatusByteIsOsdEcho(const FpgaBuildInfo& b, int byte) {
    return (byte == status_telemetry::kOsdFlagsByte && b.byte15Echoed) ||
           (byte == status_telemetry::kOsdPositionByte && b.byte12Echoed);
}

// The "v,N;" entry of a CONF_STR (UIO_GET_STRING): N, or -1 when there is none.
inline int confStrVersion(const std::string& conf) {
    for (size_t pos = 0; pos < conf.size();) {
        size_t end = conf.find(';', pos);
        if (end == std::string::npos)
            end = conf.size();
        if (end - pos > 2 && conf.compare(pos, 2, "v,") == 0 && end - pos <= 6) {
            int v = 0;
            size_t i = pos + 2;
            for (; i < end && conf[i] >= '0' && conf[i] <= '9'; ++i)
                v = v * 10 + (conf[i] - '0');
            if (i == end)
                return v;
        }
        pos = end + 1;
    }
    return -1;
}

// The core's own CONF_STR against the row: empty when its "v,N" is the row's
// confVersion, otherwise the refusal handed to failFpgaSession.
inline std::string fpgaConfVersionMismatch(const FpgaBuildInfo& b, const std::string& conf) {
    const int v = confStrVersion(conf);
    if (v == b.confVersion)
        return {};
    char found[24];
    if (v < 0)
        std::snprintf(found, sizeof(found), "no v,N entry");
    else
        std::snprintf(found, sizeof(found), "v,%d", v);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "FPGA build 0x%08X: the core's CONF_STR has %s where the build row says v,%d "
                  "(status layout %s), so its status bytes 12/15 cannot be trusted; refusing to "
                  "play, no ARM-present fallback.",
                  b.id, found, b.confVersion, fpgaStatusLayoutName(b));
    return buf;
}

inline std::string fpgaBuildLabel(const FpgaBuildInfo& b) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "FPGA build 0x%08X: %s, status layout %s (byte 15 %s, byte 12 %s, CONF_STR "
                  "v,%d) (",
                  b.id, b.raster, fpgaStatusLayoutName(b),
                  b.byte15Echoed ? "OSD echo" : "telemetry",
                  b.byte12Echoed ? "OSD echo" : "telemetry", b.confVersion);
    return buf + std::string(b.lineage) + ")";
}

// Play-time decision on the negotiated CAPS build ID: empty when the build is
// recognised, otherwise the refusal handed to failFpgaSession.
inline std::string fpgaBuildRefusal(uint32_t id) {
    if (recognisedFpgaBuild(id))
        return {};
    char buf[96];
    if (const FpgaBuildRetired* r = retiredFpgaBuild(id)) {
        std::snprintf(buf, sizeof(buf), "retired FPGA build 0x%08X (CAPS MPC4)", id);
        return std::string(buf) + ": " + r->reason +
               "; refusing to play, no ARM-present fallback.";
    }
    std::snprintf(buf, sizeof(buf), "unrecognised FPGA build 0x%08X (CAPS MPC4)", id);
    std::string msg = buf;
    msg += "; refusing to play, no ARM-present fallback. Known builds:";
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        std::snprintf(buf, sizeof(buf), " 0x%08X %s/%s", b.id, b.raster, fpgaStatusLayoutName(b));
        msg += buf;
    }
    msg += ". Add the RBF's FPGA_VIDEO_BUILD_ID to host/libmisterplex/fpga_build_ids.hpp only "
           "after checking its raster and status layout.";
    return msg;
}

// Lab 240p beam (c258 Plex_240p_lab.qsf, the 0x24C07821 row). That qsf does not
// set PLEX_PRESENT_480P or PLEX_ANALOG_480I, so scanout is colorbars.sv:
// H_LAST 637 → 638 clocks/line; NTSC, scandouble off, vc wraps at 261 → 262
// lines; ce_pix toggles. This tree's pll_0002 stays 20 MHz (PLEX_CLK_SYS_437
// is not a frequency select there), so the pixel rate is 10 MHz. Line rate
// 15.673981 kHz, frame rate 59.824355 Hz. The daemon cadence tick is still
// 60000/1001: 29.97 is 2:2, 23.976 is 3:2. 30/1 is not 29.97. 25 and 50 stay
// off that tick. Do not retune video_mode or display_hz.
inline constexpr uint32_t kLab240pBuildId = 0x24C07821u;
inline constexpr int kLab240pHContent = 320;
inline constexpr int kLab240pVActive = 240;
inline constexpr int kLab240pClocksPerLine = 638;
inline constexpr int kLab240pLinesPerFrame = 262;
inline constexpr int kLab240pSysHz = 20000000;
inline constexpr int kLab240pCeDiv = 2;

inline constexpr int lab240pPixelHz() { return kLab240pSysHz / kLab240pCeDiv; }

// 15.673981 kHz as millihertz.
inline constexpr int lab240pLineMilliHz() {
    return static_cast<int>(static_cast<int64_t>(lab240pPixelHz()) * 1000 / kLab240pClocksPerLine);
}

// 59.824355 Hz as truncated microhertz.
inline constexpr int64_t lab240pFrameMicroHz() {
    return static_cast<int64_t>(lab240pPixelHz()) * 1000000 /
           (static_cast<int64_t>(kLab240pClocksPerLine) * kLab240pLinesPerFrame);
}

// Constant picture-repeat on the NTSC tick (60000/1001). 30000/1001 returns 2
// (2:2). 24000/1001, 30/1, 25/1 and 50/1 return 0: 23.976 is 3:2, and the
// others are not this tick. The colorbars frame (59.824355 Hz) is not this tick.
inline constexpr int ntscConstantFieldRepeat(int contentNum, int contentDen) {
    constexpr int fieldNum = 60000;
    constexpr int fieldDen = 1001;
    if (contentNum <= 0 || contentDen <= 0)
        return 0;
    const int64_t num = static_cast<int64_t>(fieldNum) * contentDen;
    const int64_t den = static_cast<int64_t>(fieldDen) * contentNum;
    if (den == 0 || num % den != 0)
        return 0;
    const int64_t q = num / den;
    if (q < 1 || q > 4)
        return 0;
    return static_cast<int>(q);
}

} // namespace misterplex
