// mailbox_abi_spec.hpp — SINGLE SOURCE OF TRUTH for MiSTerPlex DDR mailbox ABI.
//
// Every mailbox in the DDR address map is defined here ONCE: absolute
// mailboxes by physical address, frame-store mailboxes by their offset on the
// build's page (the page itself: plx_page.hpp). Both the RTL (SystemVerilog
// parameters/localparams) and the ARM C++ (constexpr constants) must be checked
// against these values. The invariant gate (tests/unit/test_rtl_invariants.py)
// statically asserts both sides match, per present_core branch.
//
// To add a new mailbox:
//   1. Add it to kPlxPageMailboxes or kAbsoluteMailboxes below.
//   2. If it has a magic header, register it in kAllMagics.
//   3. Run `python3 tests/unit/test_rtl_invariants.py` — it MUST pass.
//   4. If the gate fails, one side drifted. Fix it before committing.
//
// ====== NEVER hand-maintain magic or address constants in two places. ======
// If the RTL and C++ disagree, this file is authoritative.

#pragma once
#include <cstddef>
#include <cstdint>
#include <array>

namespace mailbox_abi {

// --- Mailbox address table (physical DDR addresses) -------------------------
// Each entry: {name, phys_addr, magic, size_bytes, direction, has_magic}

struct MailboxEntry {
    const char* name;
    uint32_t phys_addr;
    uint32_t magic;       // 0 if no magic header
    unsigned size_bytes;   // 8 = one 64-bit qword
    const char* direction; // "fpga_to_arm", "arm_to_fpga", "bidirectional"
    bool has_magic;
};

// ---- Core frame-store mailboxes (ddr_frame_store via present_core) ----
//
// These live on a PER-BUILD page, so only their OFFSETS are ABI. present_core.sv
// places PLXS..PLXJ at page + offset on every branch:
//   PLEX_PRESENT_480P     page = FS_DOORBELL 0x300FF000 (every recognised 480p row)
//   default branch        no mailbox override: the ddr_frame_store module
//                         defaults, page 0x3007F000 (240p and 480i rows)
//   PLEX_PRESENT_480I     page = FS_DOORBELL 0x301FF000 (lab R15)
//   PLEX_PRESENT_720P_L4  page = FS_DOORBELL 0x3047F000
// The daemon takes the page from its resolved layout (FpgaSpi
// ddrLayout_.doorbell_phys); code without a layout uses plx_page.hpp. Never add
// an absolute kPlx*Addr here: tests/unit/test_rtl_invariants.py fails on one.

// PLXK — Doorbell (ARM→FPGA). ARM writes bank|format|seq to trigger a frame swap.
// It sits at FS_DOORBELL, the layout's doorbell_phys: the mailbox page on every
// branch but the default one (doorbell 0x300FF000, publish-only on
// FPGA_VIDEO_320 builds).
constexpr uint32_t kPlxkMagic = 0x504C584Bu; // "PLXK"

// PLXS — Status (FPGA→ARM). [47:32] OSD word, [63:48] seq. ddr_frame_store
// republishes it at least every 2^18 clk_ddr, so a live core's high word moves.
constexpr uint32_t kPlxsOff   = 0x100u;
constexpr uint32_t kPlxsMagic = 0x504C5853u; // "PLXS"

// PLXI — Input (FPGA→ARM). Playback commands (pause/resume/seek). Word: [31:0] magic,
// [39:32] command (input_mailbox.hpp PlaybackCommand: 1-5 today; 6 Up, 7 Down, 8 PageUp,
// 9 PageDown, 10 Back, 11 Info for browse MVP-b, read only with MPX_BROWSE_V2=1 and
// MPX_BROWSE_MVPB=1), [47:40] command sequence, [63:48] heartbeat sequence.
constexpr uint32_t kPlxiOff   = 0x108u;
constexpr uint32_t kPlxiMagic = 0x504C5849u; // "PLXI"

// PLXM — SDRAM bring-up (FPGA→ARM). Memory test state.
constexpr uint32_t kPlxmOff   = 0x110u;
constexpr uint32_t kPlxmMagic = 0x504C584Du; // "PLXM"

// PLXF — Frame-store status (FPGA→ARM). Underrun count + debug state.
constexpr uint32_t kPlxfOff   = 0x118u;
constexpr uint32_t kPlxfMagic = 0x504C5846u; // "PLXF"

// SDRAM diagnostic (FPGA→ARM). ABSOLUTE on every build: present_core never
// overrides ddr_frame_store DIAG_MAILBOX_PHYS (written only under
// COMMIT_PERF_CNT) or the legacy ddram_frame_rd MEMTEST_DIAG_MAILBOX_PHYS.
constexpr uint32_t kSdramDiagAddr = 0x3007F120u;
// No magic — raw diagnostic word.

// PLXD — Bank-release ACK (FPGA→ARM). Tells ARM which bank is safe to write.
// Layout (64-bit, little-endian):
//   [31:0]   magic 0x504C5844 "PLXD"
//   [33:32]  free_bank_mask[1:0] — bit i=1 means bank i is safe to overwrite
//   [34]     disp_bank           — currently displayed bank (0 or 1)
//   [35]     swap_pending        — doorbell received, vsync flip pending
//   [47:36]  reserved (0)
//   [63:48]  frames_done[15:0]   — monotonic bank-swap counter (wraps at 65535)
//
// Semantics:
//   !swap_pending → free_bank_mask = disp_bank ? 0b01 : 0b10
//   swap_pending  → free_bank_mask = 0b00 (both banks in use)
//   On vsync swap: old disp_bank becomes free, frames_done++
constexpr uint32_t kPlxdOff   = 0x128u;
constexpr uint32_t kPlxdMagic = 0x504C5844u; // "PLXD"
// Bit-field positions (in the upper 32 bits, i.e. offset from bit 32):
constexpr unsigned kPlxdFreeBankMaskBit = 0;  // bits [33:32] → [1:0] of upper word
constexpr unsigned kPlxdFreeBankMaskWidth = 2;
constexpr unsigned kPlxdDispBankBit = 2;      // bit [34] → bit 2 of upper word
constexpr unsigned kPlxdSwapPendingBit = 3;   // bit [35] → bit 3 of upper word
constexpr unsigned kPlxdFramesDoneBit = 16;   // bits [63:48] → [31:16] of upper word
constexpr unsigned kPlxdFramesDoneWidth = 16;

// PLXJ — Source-aspect commit acknowledgement (FPGA→ARM).
// Layout (64-bit, little-endian):
//   [31:0]   magic 0x504C584A "PLXJ"
//   [43:32]  committed DAR X
//   [55:44]  committed DAR Y
//   [63:56]  packet token echoed from PLXA
constexpr uint32_t kPlxjOff   = 0x130u;
constexpr uint32_t kPlxjMagic = 0x504C584Au; // "PLXJ"

// Every frame-store mailbox fits in the page.
constexpr uint32_t kPlxPageBytes = 0x1000u;
static_assert(kPlxjOff + 8u <= kPlxPageBytes, "frame-store mailboxes fit one page");

// ---- Bitstream ring mailboxes (ddr_bitstream_reader) ----

// PLXB — Ring CTRL (ARM→FPGA). Bitstream ring control word.
constexpr uint32_t kPlxbAddr  = 0x30140000u;
constexpr uint32_t kPlxbMagic = 0x504C5842u; // "PLXB"

// FPGA-video capabilities are published only in response to a fresh Probe
// record. The nonce and final publication word bind the reply to this boot.
constexpr uint16_t kFpgaVideoAbiVersion = 2;
constexpr uint16_t kFpgaVideoLayoutId = 1;
constexpr uint32_t kVideoCapsAddr = 0x30140050u;
constexpr uint32_t kVideoCapsMagic = 0x4D504330u; // "MPC0"
constexpr uint32_t kVideoFeaturesAddr = 0x30140058u;
constexpr uint32_t kVideoFeaturesMagic = 0x4D504331u;
constexpr uint32_t kVideoDimensionsAddr = 0x30140060u;
constexpr uint32_t kVideoDimensionsMagic = 0x4D504332u;
constexpr uint32_t kVideoAuLimitAddr = 0x30140068u;
constexpr uint32_t kVideoAuLimitMagic = 0x4D504333u;
constexpr uint32_t kVideoBuildAddr = 0x30140070u;
constexpr uint32_t kVideoBuildMagic = 0x4D504334u;
constexpr uint32_t kVideoNonceLowAddr = 0x30140078u;
constexpr uint32_t kVideoNonceLowMagic = 0x4D504335u;
constexpr uint32_t kVideoNonceHighAddr = 0x30140080u;
constexpr uint32_t kVideoNonceHighMagic = 0x4D504336u;
constexpr uint32_t kVideoCapsCommitAddr = 0x30140088u;
constexpr uint32_t kVideoCapsCommitMagic = 0x4D504337u;
constexpr uint32_t kVideoPresentationAddr = 0x30140090u;
constexpr uint32_t kVideoPresentationMagic = 0x4D565053u; // "MVPS"
constexpr uint32_t kVideoPresentationCommitAddr = 0x301400D0u;
constexpr uint32_t kVideoPresentationCommitMagic = 0x4D565043u; // "MVPC"
constexpr unsigned kVideoPresentationBytes = 72;
static_assert(kVideoPresentationCommitAddr + 8 ==
                  kVideoPresentationAddr + kVideoPresentationBytes,
              "presentation commit is the final qword");
static_assert(kVideoPresentationAddr >= kVideoCapsCommitAddr + 8,
              "presentation feedback cannot overlap capabilities");

// Independent DMA-audio control remains reachable when video ingress is paused.
// Request: header(version16/opcode8/reserved8), epoch, Probe nonce, token,
// expected producer byte pointer (low32, high32 zero), publication32/magic32.
// The pointer comes from the quiesced kernel producer; MrAudio only publishes
// SPI metadata on write(), so Begin/Reset cannot demand a later SPI update.
// Publish invalid commit first, valid last.
constexpr uint16_t kAudioSessionAbiVersion = 2;
constexpr uint32_t kAudioControlAddr = 0x30140100u;
constexpr uint32_t kAudioControlMagic = 0x4D414354u; // "MACT"
constexpr uint32_t kAudioControlCommitAddr = 0x30140128u;
constexpr uint32_t kAudioControlCommitMagic = 0x4D414343u; // "MACC"
constexpr unsigned kAudioControlBytes = 48;
// Status: header(error8/opcode8/flags8/version8), current epoch, current nonce,
// ACK token, consumed stereo pairs, ACK epoch, ACK nonce, publication/commit.
constexpr uint32_t kAudioStatusAddr = 0x30140140u;
constexpr uint32_t kAudioStatusMagic = 0x4D415354u; // "MAST"
constexpr uint32_t kAudioStatusCommitAddr = 0x30140178u;
constexpr uint32_t kAudioStatusCommitMagic = 0x4D415343u; // "MASC"
constexpr unsigned kAudioStatusBytes = 64;
static_assert(kAudioControlAddr >= kVideoPresentationAddr + kVideoPresentationBytes);
static_assert(kAudioControlCommitAddr + 8 == kAudioControlAddr + kAudioControlBytes);
static_assert(kAudioStatusAddr >= kAudioControlAddr + kAudioControlBytes);
static_assert(kAudioStatusCommitAddr + 8 == kAudioStatusAddr + kAudioStatusBytes);

// ---- All magics (for collision detection) ----
// Every PLX-prefixed magic in the system. Gate rejects duplicates.
struct MagicEntry {
    const char* name;
    uint32_t magic;
};

constexpr std::array<MagicEntry, 22> kAllMagics = {{
    {"PLXK", kPlxkMagic},
    {"PLXS", kPlxsMagic},
    {"PLXI", kPlxiMagic},
    {"PLXM", kPlxmMagic},
    {"PLXF", kPlxfMagic},
    {"PLXD", kPlxdMagic},
    {"PLXJ", kPlxjMagic},
    {"PLXB", kPlxbMagic},
    {"MPC0", kVideoCapsMagic},
    {"MPC1", kVideoFeaturesMagic},
    {"MPC2", kVideoDimensionsMagic},
    {"MPC3", kVideoAuLimitMagic},
    {"MPC4", kVideoBuildMagic},
    {"MPC5", kVideoNonceLowMagic},
    {"MPC6", kVideoNonceHighMagic},
    {"MPC7", kVideoCapsCommitMagic},
    {"MVPS", kVideoPresentationMagic},
    {"MVPC", kVideoPresentationCommitMagic},
    {"MACT", kAudioControlMagic},
    {"MACC", kAudioControlCommitMagic},
    {"MAST", kAudioStatusMagic},
    {"MASC", kAudioStatusCommitMagic},
}};

// ---- Frame-store page mailboxes (offset on the build's page) ----
// PLXK is absent: it sits at FS_DOORBELL, which is not the mailbox page on the
// default branch.
struct PageMailboxEntry {
    const char* name;
    uint32_t offset;       // from the build's frame-store mailbox page
    uint32_t magic;
    unsigned size_bytes;
    const char* direction;
    bool has_magic;
};

constexpr std::array<PageMailboxEntry, 6> kPlxPageMailboxes = {{
    {"PLXS", kPlxsOff, kPlxsMagic, 8, "fpga_to_arm", true},
    {"PLXI", kPlxiOff, kPlxiMagic, 8, "fpga_to_arm", true},
    {"PLXM", kPlxmOff, kPlxmMagic, 8, "fpga_to_arm", true},
    {"PLXF", kPlxfOff, kPlxfMagic, 8, "fpga_to_arm", true},
    {"PLXD", kPlxdOff, kPlxdMagic, 8, "fpga_to_arm", true},
    {"PLXJ", kPlxjOff, kPlxjMagic, 8, "fpga_to_arm", true},
}};

// ---- Absolute mailboxes (one physical address on every build) ----
// Gate rejects overlapping slots.
constexpr std::array<MailboxEntry, 13> kAbsoluteMailboxes = {{
    {"DIAG", kSdramDiagAddr, 0,             8, "fpga_to_arm",  false},
    {"PLXB", kPlxbAddr,     kPlxbMagic,     8, "arm_to_fpga",  true},
    {"MPC0", kVideoCapsAddr, kVideoCapsMagic, 8, "fpga_to_arm", true},
    {"MPC1", kVideoFeaturesAddr, kVideoFeaturesMagic, 8, "fpga_to_arm", true},
    {"MPC2", kVideoDimensionsAddr, kVideoDimensionsMagic, 8, "fpga_to_arm", true},
    {"MPC3", kVideoAuLimitAddr, kVideoAuLimitMagic, 8, "fpga_to_arm", true},
    {"MPC4", kVideoBuildAddr, kVideoBuildMagic, 8, "fpga_to_arm", true},
    {"MPC5", kVideoNonceLowAddr, kVideoNonceLowMagic, 8, "fpga_to_arm", true},
    {"MPC6", kVideoNonceHighAddr, kVideoNonceHighMagic, 8, "fpga_to_arm", true},
    {"MPC7", kVideoCapsCommitAddr, kVideoCapsCommitMagic, 8, "fpga_to_arm", true},
    {"MVPS", kVideoPresentationAddr, kVideoPresentationMagic,
     kVideoPresentationBytes, "fpga_to_arm", true},
    {"MACT", kAudioControlAddr, kAudioControlMagic,
     kAudioControlBytes, "arm_to_fpga", true},
    {"MAST", kAudioStatusAddr, kAudioStatusMagic,
     kAudioStatusBytes, "fpga_to_arm", true},
}};

constexpr bool slotsOverlap(uint32_t a, unsigned aBytes, uint32_t b, unsigned bBytes) {
    return a < b + bBytes && b < a + aBytes;
}

constexpr bool plxPageMailboxesDisjoint() {
    for (size_t i = 0; i < kPlxPageMailboxes.size(); ++i) {
        const PageMailboxEntry& a = kPlxPageMailboxes[i];
        if (a.offset % 8u != 0 || a.offset + a.size_bytes > kPlxPageBytes)
            return false;
        for (size_t j = i + 1; j < kPlxPageMailboxes.size(); ++j) {
            const PageMailboxEntry& b = kPlxPageMailboxes[j];
            if (slotsOverlap(a.offset, a.size_bytes, b.offset, b.size_bytes))
                return false;
        }
    }
    return true;
}

constexpr bool absoluteMailboxesDisjoint() {
    for (size_t i = 0; i < kAbsoluteMailboxes.size(); ++i)
        for (size_t j = i + 1; j < kAbsoluteMailboxes.size(); ++j)
            if (slotsOverlap(kAbsoluteMailboxes[i].phys_addr, kAbsoluteMailboxes[i].size_bytes,
                             kAbsoluteMailboxes[j].phys_addr, kAbsoluteMailboxes[j].size_bytes))
                return false;
    return true;
}

static_assert(plxPageMailboxesDisjoint(), "frame-store page mailboxes overlap or leave the page");
static_assert(absoluteMailboxesDisjoint(), "absolute mailboxes overlap");

// ---- Bitstream ring additional magics (not address-mapped; in-band) ----
// These are record/status magics used within the bitstream ring protocol,
// not standalone mailboxes. Listed here for magic-collision detection only.
constexpr std::array<MagicEntry, 9> kBitstreamRingMagics = {{
    {"PLXR", 0x504C5852u},  // read pointer
    {"PLXE", 0x504C5845u},  // error
    {"PLXN", 0x504C584Eu},  // record header (NAL)
    {"PLXT", 0x504C5854u},  // ring level stat
    {"PLXU", 0x504C5855u},  // consumer seq stat
    {"PLXV", 0x504C5856u},  // last bad seq stat
    {"PLXW", 0x504C5857u},  // session low stat
    {"PLXY", 0x504C5859u},  // session high stat
    {"PLXZ", 0x504C585Au},  // underrun/overrun stat
}};

// Additional in-band magic (not in ring stats array above):
constexpr MagicEntry kPlxqMagic_entry = {"PLXQ", 0x504C5851u}; // desync/state flags

} // namespace mailbox_abi
