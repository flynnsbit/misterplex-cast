#pragma once
// Minimal MiSTer HPS→FPGA SPI bridge for ioctl file_tx (frame store ingest).
// Mirrors Main_MiSTer FIO_FILE_* protocol. Shares GPO/GPI with Main — short
// transfers only; optional flock reduces races.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/ddr_bitstream_ring.hpp"
#include "libmisterplex/audio_session.hpp"
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/input_mailbox.hpp"
#include "libmisterplex/source_aspect.hpp"
#include "libmisterplex/spi_ack_wait.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/f1_beat.hpp"
#include "libmisterplex/mast_reload.hpp"

namespace misterplex {

class FpgaSpi {
public:
    ~FpgaSpi();

    // Map FPGA manager regs via /dev/mem. Returns false if unavailable (host).
    bool open();
    void close();
    bool ok() const { return map_ != nullptr; }

    // --- Sharing the SPI bus with Main ----------------------------------------
    // The HPS<->FPGA "SPI" is not a bus with arbitration: it is a single 32-bit
    // GPO register in the FPGA manager (0xFF706010) plus a strobe/ACK handshake
    // on GPI. Main_MiSTer assumes it is the sole owner. Two facts make naive
    // sharing fatal:
    //
    //   1. Main's fpga_spi() spins on the ACK handshake with NO timeout — the
    //      only other way out is the FPGA leaving user mode (fpga_io.cpp). If we
    //      rewrite GPO while Main sits between "strobe=1" and "saw ACK", the core
    //      drops ACK, Main never sees it, and Main spins forever. That is a hard
    //      hang: no F12, no OSD, no /dev/MiSTer_cmd, and no way out but killing
    //      the process. This is what used to be blamed on "the Plex core crashing
    //      Main", and what healMainReloadPlex() existed to paper over.
    //   2. Main never re-reads GPO (`#define fpga_gpo_read() gpo_copy`), so any
    //      bits we leave behind are invisible to it.
    //
    // SIGSTOPing Main does not fix either problem — it freezes Main at an
    // arbitrary instruction, including inside the handshake, which is precisely
    // the dangerous case.
    //
    // What is actually safe: stop Main, WAIT until it has really stopped, then
    // read the hardware GPO and check Main's own transaction-enable bits
    // (FPGA_EN/OSD_EN/IO_EN) and the strobe. Main only ever calls fpga_spi()
    // between EnableXxx()/DisableXxx(), so all-clear proves it is between
    // transactions and, being stopped, cannot start one. We then run our
    // transaction, restore GPO byte-for-byte, and resume Main — which wakes up
    // with hardware exactly matching its shadow copy. If Main is mid-transaction
    // we resume it, back off, and retry; if it stays busy the SPI call fails
    // cleanly rather than corrupting anything. See SpiExclusive in fpga_spi.cpp.

    // --- Stranded-Main safety net ---------------------------------------------
    // The safe window above still SIGSTOPs Main for a few microseconds. If
    // misterplexd dies inside that window — crash, SIGKILL, OOM — nothing ever
    // sends SIGCONT and Main is left stopped forever.
    //
    // resumeStrandedMain(): SIGCONT any MiSTer left in state T while we hold no
    // window of our own. Safe to call from anywhere — it only reads /proc and
    // never touches SPI. Call at startup (repairs a previous death) and from a
    // slow watchdog (repairs a hang inside the critical section).
    static void resumeStrandedMain();

    // installCrashGuard(): resume Main from fatal-signal handlers, then re-raise
    // with the default disposition so the crash is still reported normally.
    // Covers everything except SIGKILL, which resumeStrandedMain() mops up on the
    // next start.
    static void installCrashGuard();

    // True while this process holds Main stopped for an SPI critical section.
    static bool mainPaused();

    // False when no MiSTer process exists at all.
    static bool mainAlive();

    // Push a complete raw buffer as an ioctl download (index = OSD F# entry).
    // F1 frame presentation is not accepted here: the product frame-store
    // contract is DDR YUV420p only.
    bool sendFileTx(const uint8_t* data, size_t len, uint8_t index = 1);

    // Convenience for non-F1 legacy/debug slots: RGB24 WxH → RGB565 LE then sendFileTx.
    bool sendRgb24Frame(const uint8_t* rgb, int w, int h, uint8_t index = 1);

    // Push packed RGB565 LE to non-F1 legacy/debug slots. len must be w*h*2.
    bool sendRgb565Frame(const uint16_t* rgb, int w, int h, uint8_t index = 1);
    bool sendRgb565Bytes(const uint8_t* rgb565le, size_t len, uint8_t index = 1);

    // C3 DDR frame-store path: planar I420/YUV420p via HPS DDR
    // (/dev/mem @ 0x30000000) plus format-tagged doorbell. RGB565 is not a
    // valid F1 frame-store payload for this RTL.
    bool setDdrFrameLayout(const DdrFrameGeometry& geometry,
                           DdrFrameFormat format = DdrFrameFormat::Yuv420p);
    bool setDdrFrameLayout(int width, int height,
                           DdrFrameFormat format = DdrFrameFormat::Yuv420p);
    DdrFrameLayout ddrFrameLayout() const { return ddrLayout_; }
    bool sendYuv420pFrameDdr(const uint8_t* yuv420p, size_t len,
                             const DdrFrameGeometry& geometry, int bank = 0,
                             DdrBankWritePolicy policy = DdrBankWritePolicy::BestEffort,
                             uint32_t srcPhys = 0);
    void setFabricDirectPresent(bool) {}
    void setStickI420Present(bool) {}
    uint8_t* ddrBankVirt(int bank);
    bool lastStickI420Used() const { return false; }
    bool lastFabricDirectUsed() const { return ddrMapViaMplex_; }
    bool lastDdrMapViaMplex() const { return ddrMapViaMplex_; }
    const char* lastDdrPublishHow() const { return lastPublishHow_; }
    bool sendYuv420pFrameDdr(const uint8_t* yuv420p, size_t len, int width, int height,
                             int bank = 0,
                             DdrBankWritePolicy policy = DdrBankWritePolicy::BestEffort);
    static uint32_t resolveCachedSrcPhys(const void* virt, size_t len);
    // Zero-intermediate-buffer present: PLXD bank select + mapped write pointer.
    // Caller fills outLen bytes at the returned pointer, then commitDdrBankIngest.
    // Returns nullptr on failure (lastError set). outBank is the bank actually selected.
    uint8_t* beginDdrBankIngest(size_t expectLen, int preferredBank, int& outBank);
    // Fence/flush + doorbell after beginDdrBankIngest payload write. copy_us=0 (ingest
    // cost is attributed by the caller as pipe-read into the bank).
    bool commitDdrBankIngest(int bank, size_t len);
    // DDR frame mmap policy. Default true keeps the proven strongly-ordered/device
    // mapping; false is a lab knob for write-combine/cacheable /dev/mem tests.
    // If a lab proves the no-sync mapping is cacheable, enable flush so the FPGA
    // sees all stores before the DDR doorbell is signalled.
    void setDdrMemSync(bool on);
    void setDdrMemFlush(bool on) { ddrMemFlush_ = on; }
    // Next sendDdrFrame re-probes doorbell vs SPI. Leftover has_frame after a
    // prior swap must not lock doorbell-only mode (cast then stays on chevron).
    void reprobeDdrKick() {
        ddrKickMode_ = 0;
        ddrKickFailMs_ = -1.0;
    }
    // Core gate reopened (core_gate.hpp): a fresh Plex core instance is loaded.
    // Forget the old instance's doorbell/kick/mailbox state and negotiated row,
    // the same per-core state close() resets; the maps stay.
    void onCoreReturned();
    // A retained session epoch from before a core swap died with that core
    // instance: drop the ring/session/audio ownership (as close() does) so the
    // next begin starts clean instead of aborting an epoch nobody can ACK.
    void dropStaleSession();
    // Core gate v2 inputs. fabricState: FPGA-manager stat mode and GPO core-reset bit
    // from the register map (HPS-side reads, safe while the FPGA is reprogrammed).
    // mainIdentity: the oldest process named MiSTer, i.e. Main (a new one means a reload).
    FabricState fabricState() const { return readFabricState(map_); }
    static MainIdentity mainIdentity();
    // The CAPS half of beginFpgaVideoSession and nothing more: AU-ring reset, one
    // Probe record, the core's nonce-matched reply. No session starts. The core gate
    // uses it to learn the loaded build ID before it opens (AU ring only).
    bool probeVideoCapabilities(ddr_bitstream_ring::VideoCapabilities& capabilities,
                                int timeout_ms);
    // The probe the gate asks for (ProbeWanted) and its verdict, used by the daemon's
    // idle tick and the tests alike. Runs only while the gate is Verifying. A session
    // still marked active began on the core instance the gate closed on: its ownership
    // is dropped first (nothing can ACK its end; the probe's ring reset fences it).
    // Opens only on the play-time allowlist test, fpgaBuildRefusal(id) empty, and on the
    // raster the gate is paired with (CoreGate::probeResult).
    CoreGateEvent runCoreGateProbe(int timeout_ms, int64_t nowUs = coreGateNowUs());
    struct DdrTiming {
        int64_t prep_wait_us = 0;
        int64_t copy_us = 0;
        int64_t flush_us = 0;
        int64_t doorbell_us = 0;
        int64_t post_wait_us = 0;
        int64_t total_us = 0;
        int64_t bank_reuse_wait_us = 0;
        int64_t plxa_poll_us = 0;     // PLXA bank-release poll time (0 = fallback)
        int plxa_poll_iters = 0;      // number of PLXA poll iterations
        bool plxa_used = false;       // true if PLXA drove bank selection
        const char* publish_how = "memcpy"; // wc | memcpy | pl330_contig | pl330_pages
    };
    DdrTiming lastDdrTiming() const { return lastDdrTiming_; }
    struct DdrDoorbellStatus {
        uint32_t seq = 0;
        int bank = 0;
        DdrFrameFormat format = DdrFrameFormat::Yuv420p;
    };
    bool readDdrDoorbellStatus(DdrDoorbellStatus& status);
    bool readFrameStoreStatus(FrameStoreStatus& status);
    bool readBankRelease(BankReleaseStatus& status);
    // Overwrite the bank the core is scanning (no swap). Used for on-glass overlay.
    bool blitDisplayedBank(const uint8_t* payload, size_t len);
    bool readSourceAspectAck(SourceAspectAck& status);
    // Read-only: one 32-bit word at `off` (4-aligned, < 4096) of the current layout's doorbell
    // (PLX) page, e.g. the D52 logger's PLXF/PLXD/PLXJ halves. Locked like every access to that
    // page (one mapper, review 08:17/08:18); false if the core gate is closed, the page cannot be
    // mapped or `off` is out of range. No store path is reachable from here.
    bool plxPageRead32(uint32_t off, uint32_t& out);
    // D52-LOG's reader (review 08:17 (b)): plxPageRead32, only while `page` is still the layout's
    // doorbell page (checked under the same lock); *otherPage says that was the refusal.
    bool plxPageRead32(uint32_t page, uint32_t off, uint32_t& out, bool* otherPage);
    // Physical base used by core ddram_frame_rd (must match RTL PHYS_BASE).
    static constexpr uint32_t kDdrFrameBase = 0x30000000u;
    static constexpr uint32_t kDdrFrameStride = 0x40000u; // 256 KiB
    static constexpr uint32_t kDdrDoorbellMagic = mailbox_abi::kPlxkMagic;
    static constexpr size_t kDdrFrameBytes = 320 * 240 * 2;

    // --- OSD status mailbox (core -> HPS, zero SPI) ----------------------------
    // ddram_frame_rd publishes the live OSD word here whenever it changes, plus a
    // slow heartbeat. Reading it costs one uncached 64-bit load and, unlike
    // UIO_GET_STATUS, never touches the GPO register Main_MiSTer owns — so it
    // cannot stall or hang Main no matter how often we poll.
    //   [31:0]  magic "PLXS"
    //   [47:32] status[15:0]
    //   [63:48] seq
    static constexpr uint32_t kDdrMailboxMagic = kDdrStatusMailboxMagic; // "PLXS"

    // Read the OSD word from the mailbox. Returns false until the mailbox is
    // proven LIVE, which is the caller's cue to fall back to getCoreStatus().
    // Liveness matters: DDR keeps its contents across a core reload, so a
    // pre-mailbox RBF would otherwise leave us reading the previous core's stale
    // magic forever. The core bumps seq on every publish (change + heartbeat),
    // so we trust the word only while seq keeps advancing.
    bool readOsdMailbox(uint16_t& osd);

    // Read one edge-detected playback command from the input mailbox. This is
    // DDR-only and never falls back to the HPS<->FPGA SPI bus.
    bool readInputMailbox(PlaybackCommand& command);

    // MPX_BROWSE_V2 (browse_v2.hpp): the same reads without presentMu_, so a paint, the overlay
    // or an SPI window holding presentMu_ never delays a remote press. They never touch the frame
    // window (ddrMap_), which the present path unmaps and remaps under presentMu_: they read the
    // doorbell page through the one mapper (review 08:17 (a)), its lock held only for the read.
    // The mailbox address and a reset epoch (frame-map release, core return) are atomics, so the
    // reader restarts its edge/heartbeat state exactly where readInputMailbox / readOsdMailbox
    // would. A closed gate reads nothing (coreGateAllowsQuiet: no setErr).
    // readInputMailboxDirect: the input poll thread only; maxCommand: kBrowseMaxInputCommand.
    // readOsdMailboxDirect: the OSD poll thread only.
    bool readInputMailboxDirect(PlaybackCommand& command, uint8_t maxCommand);
    bool readOsdMailboxDirect(uint16_t& osd);

    // Owenb ring control block at 0x3140_0000. lo32 of one 64-bit word.
    // PlexOB Y/C is word 87. A failed read returns false and leaves loOut unchanged.
    bool readOwenbRingCtrlWord(unsigned wordIndex, uint32_t& loOut);

    // Push raw s16le stereo PCM chunk to audio_fifo (F2 / index 2). Appends.
    bool sendPcmChunk(const uint8_t* pcm, size_t len, uint8_t index = 2);

    // Push elementary bitstream (H.264 annex-B) to F3 bitstream_fifo. Appends.
    bool sendBitstreamChunk(const uint8_t* data, size_t len, uint8_t index = 3);
    // Publish the source display aspect to hidden ioctl index 4. The core uses
    // it for VIDEO_ARX/VIDEO_ARY when the OSD Aspect ratio is "Original".
    // Terms above kSourceAspectMax are folded (sourceAspectForSend): *sent gets
    // the value put on the wire (the PLXJ ACK must match it), *note the clamp text.
    bool sendSourceAspect(const SourceAspect& aspect, SourceAspect* sent = nullptr,
                          std::string* note = nullptr);
    // Product path: copy complete Annex-B NAL records into the HPS DDR ring.
    // pushBitstreamNal() copies before returning, so the caller may immediately
    // reuse/free Nal::annexb. Full is transient; Desync/Fatal require a session
    // reset. begin while active is rejected by contract: end the old session first.
    using BitstreamNal = ddr_bitstream_ring::Nal;
    using BitstreamStatus = ddr_bitstream_ring::Status;
    using BitstreamPushResult = ddr_bitstream_ring::PushResult;
    using BitstreamAccessUnit = ddr_bitstream_ring::AccessUnit;
    using VideoCapabilities = ddr_bitstream_ring::VideoCapabilities;
    using VideoPresentation = ddr_bitstream_ring::VideoPresentation;
    bool beginFpgaVideoSession(uint64_t session_id, VideoCapabilities& capabilities,
                               int timeout_ms = 1000);
    BitstreamPushResult pushBitstreamAccessUnit(const BitstreamAccessUnit& au,
                                               int timeout_ms = 250);
    bool drainBitstreamSession(uint64_t session_id, int timeout_ms = 2000);
    VideoCapabilities videoCapabilities() const;
    // Native MVPS: audio samples are frozen at this frame's display ACK.
    // Use readAudioSessionStatus separately for the current pacing/EOF clock.
    bool readVideoPresentation(uint64_t session_id, VideoPresentation& status);
    using AudioControl = audio_session::Control;
    using AudioSessionStatus = audio_session::Status;
    // Quiesce the HPS producer before Begin/Reset. Begin ACK leaves DMA paused
    // for priming; Resume starts the first queued sample without dropping it.
    // With no ACK bound to this session, the MrAudio lane is classified from fresh,
    // unfiltered MAST (classifyAudioLane): a Begin the consumer did not adopt leaves
    // audio unowned; a Reset of a session the consumer does not own succeeds (there is
    // nothing of this session's to reset). lastError() says which, and why.
    bool controlAudioSession(uint64_t session_id, AudioControl command,
                             int timeout_ms = 1000);
    bool readAudioSessionStatus(uint64_t session_id, AudioSessionStatus& status);
    // Recovery after a failed End/Flush. Never releases local ownership until
    // actual audio Reset and fresh decoder CTRL-reset fences have acknowledged.
    bool abortFpgaVideoSession(uint64_t session_id, int timeout_ms = 2000);
    // True while a Begin of this process may still own the MrAudio consumer.
    bool audioSessionOwned() const;
    // Hard-kill restart: when beginFpgaVideoSession reclaimed a MrAudio consumer left
    // active by a previous owner (a killed daemon), the one-line note to log; else "".
    std::string takeAudioRecoveryNote();
    // MAST-WEDGE (review 21:14): one liveness probe of the MrAudio lane's publisher, as at
    // core-gate open. MAST must decode (magic, ABI, commit) and its publication counter must
    // advance by the begin check's count within the begin check's window. A core whose gate
    // CAPS lacks the lane gives out.lane = false. Returns out.live (true when there is no lane).
    bool probeAudioLane(mast::Probe& out);
    // True when the last beginFpgaVideoSession failed only because the lane did not publish
    // (recoverOrphanedAudioConsumer's fresh check). No begin retry can move that FSM; only a
    // core reload can.
    bool lastBeginWedged() const;
    // R5f (review 01:19/01:25): withdraw any outstanding audio command, i.e. the control
    // commit word at +0x128 := 0 (classifyAudioLane's withdraw), so the DDR control page never
    // carries a valid command into a reload. Gated like every DDR write: with another core loaded
    // (closed gate) it writes nothing and returns false with the reason in why.
    bool withdrawAudioCommand(std::string& why);
    // The MrAudio lane as fresh, unfiltered MAST shows it after a command of this session
    // got no bound ACK (see classifyAudioLane).
    enum class AudioLane { Owned, NotOwned, Wedged };
    bool beginBitstreamSession(uint64_t session_id, int timeout_ms = 250);
    BitstreamPushResult pushBitstreamNal(const BitstreamNal& nal, int timeout_ms = 250);
    bool flushBitstreamSession(uint64_t session_id, int timeout_ms = 250);
    bool endBitstreamSession(uint64_t session_id, int timeout_ms = 250);
    bool pauseBitstreamSession(uint64_t session_id, int timeout_ms = 250);
    bool resumeBitstreamSession(uint64_t session_id, int timeout_ms = 250);
    bool readBitstreamStatus(BitstreamStatus& status);
    // Legacy byte-chunk entry point: wraps each chunk in a default-session NAL
    // record so older call sites keep working while source-demux moves to the
    // explicit record API above.
    bool sendBitstreamChunkDdr(const uint8_t* data, size_t len);
    bool flushBitstreamDdr();

    // Pulse status bit 16 to flush present-domain audio FIFO (not T[10]).
    bool flushAudioFifo();

    // Pulse status bit 11 to flush bitstream FIFO / NAL scanner.
    bool flushBitstreamFifo();

    // UIO_SET_STATUS2: set/clear a single status bit (0–127). Best-effort —
    // does not fully sync with Main's cur_status shadow. Base is the private
    // status_[] shadow (starts zero) plus the live echo read in the same SPI
    // window (composeStatusBit); prefer setStatusBits for multi-bit.
    bool setStatusBit(int bit, int value);

    // Write full 128-bit status word via UIO_SET_STATUS2 and update private shadow.
    // Writes the word as given: no live overlay (setStatusBit/setStatusBits add it).
    bool setStatusWord(const uint8_t word[16]);

    // Read UIO_GET_STATUS into private shadow, apply bit changes, write back.
    // bits: pairs (bit, value). Only OSD-safe if core status_in v2 preserves [15:0].
    bool setStatusBits(const int* bit_val_pairs, int n_pairs);

    // Word composition of setStatusBit() / setStatusBits(): RMW base is the
    // private shadow; `live` (nullptr if the read failed) is the core's echo.
    // Both carry the live OSD bytes 12/15 (status layout v3,
    // OSD-MENU-SPEC-1002) under the caller's own bits.
    static void composeStatusBit(uint8_t word[16], const uint8_t shadow[16], const uint8_t* live,
                                 int bit, int value);
    // false: a bit is out of range. want_pulse0: the pairs raise bit 0.
    static bool composeStatusBits(uint8_t word[16], const uint8_t shadow[16], const uint8_t* live,
                                  const int* bit_val_pairs, int n_pairs, bool& want_pulse0);
    // Base word of the SPI DDR kick (kickDdrSpi), before its kick bits: the
    // shadow, live [15:0] on the first (verify) kick, and the live OSD bytes
    // 12/15 on every kick.
    static void composeKickBase(uint8_t word[16], const uint8_t shadow[16], const uint8_t* live,
                                bool first_verify);
    // Status block heartbeat (status byte 6 = H, status_heartbeat.hpp; ported from 1cb004e5): the
    // setStatusBits base (shadow, live [15:0] and AR, live OSD bytes 12/15, pulse bits [0]/[10]/[11]
    // cleared), then H[6:0] = h and H[7] copied from the live echo (the core drives Running;
    // UIO_SET_STATUS2 cannot skip a bit).
    static void composeStatusHeartbeat(uint8_t word[16], const uint8_t shadow[16],
                                       const uint8_t* live, uint8_t h, uint32_t daemon_id = 0);
    // The row rule alone: a recognised row with statusBlock, or any recognised row under the lab
    // override MPX_STATUS_BLOCK_FORCE=1. No row: off.
    static bool statusBlockRow(const FpgaBuildInfo* row, bool force) {
        return row && (row->statusBlock || force);
    }
    // True while the core gate is open on a statusBlock row, or while idleErrorPaintAllowed
    // on a live CAPS id whose row passes statusBlockRow (Rejected Plex pairing). closeNow and
    // unrecognised CAPS stay off, so no beat reaches the next core or a telemetry-byte-6 layout.
    bool statusBlockActive() const;
    void setStatusBlockForce(bool on) { statusBlockForce_.store(on); }
    // One UIO_SET_STATUS2 word carrying H[6:0] = h, built by composeStatusHeartbeat on the live
    // echo read in the same SPI window. Only when statusBlockActive(); otherwise false with no SPI
    // access at all. A failed live read skips the beat (never a stale-word write).
    bool writeStatusHeartbeat(uint8_t h);
    void noteWedgeTrigger(const char* site);
    void serviceWedgeRead(const char* site, bool requirePending = false);

    // Core→HPS status via UIO_GET_STATUS (0x29). Fills 16 bytes (128 bits).
    // Layout: see CoreStatus helpers below / docs/phase3-decode.md.
    bool getCoreStatus(uint8_t out[16]);

    // UIO_GET_STRING — the core's CONF_STR exactly as Main_MiSTer sees it.
    bool getConfigString(std::string& out);

    struct CoreStatus {
        bool has_frame = false;
        bool has_audio = false;
        bool has_stream = false;
        bool audio_underrun = false;
        bool has_idr = false;
        bool stub_busy = false;
        bool sps_valid = false;
        bool pps_valid = false;
        uint8_t last_nal_type = 0;
        uint16_t nalu_count = 0;
        uint16_t stream_fifo_level = 0;
        uint8_t idr_count = 0;
        uint8_t slice_type = 0;
        uint8_t first_mb_type = 0;
        uint8_t slice_qp = 0;
        uint8_t residual_tc = 0;
        uint8_t residual_t1 = 0;
        bool residual_ok = false;
        // 3.3k: scan-order coeff[0] after levels+runs; raw[12]/status[103:96]
        // telemetry only on a v2 build row (0 when osd_byte12).
        int8_t residual_dc = 0;
        // 3.3l-1: residualCsum8 = XOR satS8(coeff[i]); raw[13]/status[111:104].
        // Golden Baseline first residual = 0x14 (20). Pre-3.3l-1 RBF: stream_bytes[7:0].
        uint8_t residual_csum = 0;
        // 3.3l-2: reconstructed-pixel signature for MB0 block0 after inv_quant+IDCT.
        // raw[14]/status[119:112] = XOR of 16 reconstructed Y samples; golden 0x3B.
        uint8_t recon_sig = 0;
        // raw[15]/status[127:120] = P3-3l2 silicon RCA flags on a v2 build row
        // (0 when osd_byte15). Bits [2:1] may be altered by the Aspect Ratio OSD
        // splice; use the other bits only.
        uint8_t recon_dbg = 0;
        // The negotiated build row (fpga_build_ids.hpp) echoes raw[12] / raw[15]
        // as the user's OSD bytes (layout v3, CONF_STR v,10): decoded into `osd`
        // instead of residual_dc / recon_dbg. No row yet (before CAPS) or a v2
        // row: both bytes are telemetry.
        bool osd_byte12 = false; // osd.h_position / v_position valid
        bool osd_byte15 = false; // osd.bob / aspect / field_inverted / crop_169 / top_swap valid
        status_telemetry::OsdStatusV3 osd{};
        bool ddr_busy = false;      // status_in[79] (v2) — DDR→BRAM copy in flight
        bool swap_pending = false;  // status_in[78] — display bank flip waiting for vsync
        uint8_t stub_frames = 0; // legacy alias
        uint16_t sps_width = 0;
        uint16_t sps_height = 0;
        // Legacy alias: high/low of previous wr_count field (now idr|stub)
        uint16_t wr_count_lo = 0;
        uint32_t stream_bytes_seen = 0; // not in status anymore; kept for API compat (=0)
        uint32_t stream_nalus = 0; // status liveness mirror of nalu_count, not bytes
        uint32_t stream_bytes_in = 0; // deprecated alias of stream_nalus for API compat
    };
    // Parse getCoreStatus raw bytes into fields. `build` is the negotiated row:
    // it decides whether bytes 12/15 are telemetry or echoed OSD bytes. nullptr
    // (no CAPS yet) decodes them as telemetry, as v2.
    static CoreStatus parseCoreStatus(const uint8_t raw[16],
                                      const FpgaBuildInfo* build = nullptr);
    // readCoreStatus and the internal status polls parse with this row.
    bool readCoreStatus(CoreStatus& out);

    // The recognised build row of the loaded core, set after CAPS negotiation
    // (media_player fpgaThreadMain); nullptr when the core was refused.
    void setNegotiatedBuild(const FpgaBuildInfo* build);
    const FpgaBuildInfo* negotiatedBuild() const { return negotiatedBuild_.load(); }
    // R1 PLX slots (PLXH 0x300FF138 host -> FPGA, PLXO 0x300FF140 FPGA -> host) behind the same
    // core gate as every DDR mailbox access (r1_plxh.hpp PlxPort). review 06:57 D1: they use
    // their own uncached /dev/mem page (0x300FF000, proven to hold both slots), never the frame
    // layout's mailbox page: the start-up 320x240 layout maps only up to 0x3007FFFF.
    bool plxStore32(uint32_t phys, uint32_t value);
    bool plxStore64(uint32_t phys, uint64_t value);   // one 64-bit store (K2 PLXO zero)
    bool plxLoad64(uint32_t phys, uint64_t* value);   // a stable 64-bit read (two equal reads)
    // F1 PLXL beat (f1_beat.hpp BeatPort; F1-PREREG v2.2 ba1ae917, review 01:39 k1, H264 x1/x2): the
    // beat thread's only call into FpgaSpi. Its only lock is a try_lock of ddrPageMu_ (held:
    // Contended). Under it the gate must still be open with gateSnap and a live fabric (else
    // GateMoved, read lock-free), and the PLX page must already be mapped (else Unmapped: it never
    // maps). It takes no other lock and never calls setErr, coreGateAllows or the mapper. Then hb
    // (seeded from the word when !*seeded), a barrier, and the magic: two 32-bit stores.
    f1::BeatStore plxlBeatStore(uint64_t gateSnap, uint32_t plxlPhys, uint32_t* hb, bool* seeded);
    // R1 idle-entry stream clear (review 20:18, STATE trigger; r1_idle_paint.hpp): one bitstream
    // ring epoch reset, so the reader's out_flush raises generation_clear and drops fb_live. DDR
    // only (no SPI), ACKed by the reader's reset_epoch. Refused while a session, a ring reset or
    // an abort is active or pending.
    bool idleEntryStreamClear(int timeout_ms);

    // Thread-safe copy of last error (F1/F2/F3 share this object).
    std::string lastError() const;
    // Last successful frame push duration (ms)
    double lastPushMs() const { return lastPushMs_; }

    // MPX_OVERLAY_DECOUPLE=1 (overlay_publish.hpp): clearErr() and the hot DDR-path
    // errors (presentation read, ring full, paused push) update the error text only
    // when the SPI lock is free, never waiting for another thread's SPI window
    // (spiMutex guards err_). Process-wide; off by default. Skipped updates are counted.
    static void setHotPathErrorsNonBlocking(bool on);
    static bool hotPathErrorsNonBlocking();
    static uint64_t hotPathErrorsSkipped();
    // Tests: fn runs while this thread holds the SPI lock (as another thread's SPI
    // window would). clearErrorForTest is clearErr().
    static void withSpiLockForTest(const std::function<void()>& fn);
    void clearErrorForTest() { clearErr(); }

private:
    void setErr(std::string msg);
    void clearErr();
    // setErr, but non-blocking under setHotPathErrorsNonBlocking(true).
    void setErrHot(std::string msg);
    void gpoWrite(uint32_t v);
    uint32_t gpoRead() const;
    int gpiRead() const;
    void spiEn(uint32_t mask, int en);
    uint16_t spiWord(uint16_t word);
    void spiByte(uint8_t b);
    void spiWriteBytes(const uint8_t* p, size_t n);
    void enableFpga(int on);
    void enableIo(int on);
    void setIndex(uint8_t index);
    void setDownload(int enable);
    // Caller holds SpiExclusive + user mode.
    bool writeStatusWordRaw(const uint8_t word[16]);
    bool readStatusRaw(uint8_t out[16]);
    bool readStatusRawEx(uint8_t out[16], uint32_t* opword);
    struct WedgePage {
        uint8_t raw[16]{};
        uint32_t opword = 0;
        uint16_t stflg = 0;
        bool valid = false;
        bool dbr = false;
        bool amux = false;
        bool mux3 = false;
        bool arb = false;
        bool portbusy = false;
        bool guard = false;
        bool pll = false;
        bool rdec = false;
        bool decAlive = false;
        bool ddrAlive = false;
        uint8_t owed = 0;
        uint8_t busState = 0;
        bool killed = false;
        uint8_t orphan = 0;
        uint8_t gwin = 0;
        uint8_t resets = 0;
        const char* classification = "VOID(tag)";
    };
    // Negotiated row if one is set; otherwise the open gate's CAPS build, so a
    // cold start and the resume after onCoreReturned still arm a wedge page.
    bool wedgePageArmed() const;
    bool readWedgePage(WedgePage& out);
    static void parseWedgePage(WedgePage& out);
    static const char* classifyWedge(const WedgePage& page);
    static void logWedgePage(const char* site, int rd, const WedgePage& page);
    bool sendDdrFrame(const uint8_t* payload, size_t len, int bank,
                      DdrBankWritePolicy policy);

    int fd_ = -1;
    volatile uint32_t* map_ = nullptr;
    uint32_t gpo_copy_ = 0;
    uint8_t status_[16]{};
    std::atomic<const FpgaBuildInfo*> negotiatedBuild_{nullptr};
    std::atomic<bool> wedgePending_{false};
    std::atomic<bool> wedgeBusy_{false};
    std::atomic<bool> statusBlockForce_{false};
    double lastPushMs_ = 0;
    std::string err_;
    SpiAckWaitStats lastAckSet_{};
    SpiAckWaitStats lastAckClr_{};
    int lastStatusWriteFailedAt_ = -1;
    // Persistent HPS DDR frame window: both banks. It stops below the doorbell page, which only
    // ensureDdrMailboxMap maps (review 08:17 (a): one mapper of the PLX page).
    int ddrMemFd_ = -1;
    uint8_t* ddrMap_ = nullptr;
    size_t ddrMapLen_ = 0;
    uint8_t* ddrMapBase_ = nullptr;
    size_t ddrMapBaseLen_ = 0;
    bool ddrMapViaMplex_ = false;
    const char* lastPublishHow_ = "memcpy";
    // The doorbell (PLX) pages, uncached /dev/mem, keyed by page: ensureDdrMailboxMap is their
    // one mapper and ddrPageMu_ (lockDdrPage) their one lock. Every access through a page runs
    // under the lock, and the pages are unmapped under it (releaseDdrMap, close), so no access
    // is in flight at an munmap (review 07:30 C2, 08:17 (a), 08:18).
    struct DdrPageMap {
        uint32_t phys = 0;
        int fd = -1;
        uint8_t* map = nullptr;
    };
    std::mutex ddrPageMu_;
    std::array<DdrPageMap, 2> ddrPages_{};
    unsigned ddrPageEvict_ = 0;
    bool ddrMemSync_ = true;
    bool ddrMemFlush_ = false;
    DdrTiming lastDdrTiming_{};
    DdrFrameLayout ddrLayout_ = makeDdrFrameLayout(320, 240);
    // The layout's doorbell page, for plxPageRead32 on other threads (setDdrFrameLayout stores it).
    std::atomic<uint32_t> ddrDoorbellPage_{ddrLayout_.doorbell_phys & ~0xFFFu};
    uint32_t doorbellSeq_ = 0;
    uint32_t doorbellDynPhys_ = 0;
    uint8_t sourceAspectToken_ = 0;
    double lastDdrBankDoorbellMs_[2] = {-1.0, -1.0};
    DdrStrictReleaseState strictDdrRelease_{};
    bool mboxInit_ = false;
    bool mboxAlive_ = false;
    uint16_t mboxSeq_ = 0;
    double mboxSeqMs_ = 0.0;
    InputMailboxEdgeDetector inputMboxEdge_;
    // MPX_BROWSE_V2 mailbox reads (readInputMailboxDirect / readOsdMailboxDirect): no presentMu_, no
    // frame window; the doorbell page through the one mapper, its lock held only for the read.
    // The doorbell page the mailboxes sit in follows the frame layout; published by
    // setDdrFrameLayout. The epoch moves wherever the locked readers restart their state.
    std::atomic<uint32_t> mailboxDoorbellPhys_{ddrLayout_.doorbell_phys};
    std::atomic<uint32_t> mailboxEpoch_{0};
    uint32_t inputPageEpoch_ = 0;
    InputMailboxEdgeDetector inputEdgeDirect_;
    uint32_t osdPageEpoch_ = 0;
    bool osdDirectInit_ = false;
    bool osdDirectAlive_ = false;
    uint16_t osdDirectSeq_ = 0;
    double osdDirectSeqMs_ = 0.0;
    bool coreGateAllowsQuiet();
    volatile uint32_t* mailboxWordDirect(uint32_t phys);   // caller holds lockDdrPage()
    int ddrKickMode_ = 0; // 0=unknown, 1=doorbell, 2=SPI kick, -1=fail
    double ddrKickFailMs_ = -1.0; // steady_clock timestamp of last ddrKickMode_ = -1
    // beginDdrBankIngest / commitDdrBankIngest pairing state
    std::chrono::steady_clock::time_point ingestT0_{};
    int ingestBank_ = -1;
    // Core gate check at every choke point (open, or AU ring during the probe) plus
    // the fabric re-read right before the access; closes the gate if not live.
    bool coreGateAllows(const char* what, bool ringProbe = false);
    bool ensureDdrMap();
    // The doorbell-page lock; held by the caller of ensureDdrMailboxMap / ddrMailboxWord for as
    // long as it uses the page.
    std::unique_lock<std::mutex> lockDdrPage();
    // The one mapper of a doorbell page (core gate checked on every call): `map` = the page
    // `pagePhys` (4 KiB aligned), mapped once and shared by every user of that page.
    bool ensureDdrMailboxMap(uint32_t pagePhys, uint8_t*& map);
    volatile uint32_t* ddrMailboxWord(uint32_t phys);   // in the layout's doorbell page
    void releaseDdrPagesLocked();
    friend struct DdrPageMapperTest;   // tests/unit/test_ddr_page_mapper.cpp
    friend struct F1BeatTest;          // tests/unit/test_f1_plxl_beat.cpp (holds the page lock)
    friend struct WedgeReadTest;       // tests/unit/test_wedge_read.cpp (W-TRIG: resetBitstreamRing, wedgePending_)
    // R1 PLXH/PLXO (page 0x300FF000) through the one mapper; caller holds lockDdrPage().
    volatile uint32_t* plxWord(uint32_t phys);
    bool waitPlxdHeardKick(const BankReleaseStatus& pre, int timeoutUs);
    bool waitPlxfHeardKick(const FrameStoreStatus& pre, int timeoutUs);
    void releaseDdrMap();
    bool ensureBitstreamDdrMap();
    void releaseBitstreamDdrMap();
    bool readBitstreamFpgaCount(uint32_t& readCount);
    bool waitBitstreamReadCount(uint32_t target, int timeout_ms);
    bool waitBitstreamControl(ddr_bitstream_ring::Event event, uint64_t session_id,
                              uint32_t target, int timeout_ms);
    bool requireBitstreamSession(uint64_t session_id, const char* operation, bool hot = false);
    bool resetBitstreamRing(int timeout_ms);
    bool readVideoCapabilities(uint64_t nonce, VideoCapabilities& capabilities);
    bool readBitstreamSnapshot(uint32_t phys, uint64_t* words, size_t count);
    BitstreamPushResult writeBitstreamRecord(ddr_bitstream_ring::Event event,
                                             uint64_t session_id,
                                             uint32_t seq,
                                             uint8_t nal_type,
                                             const uint8_t* payload,
                                             size_t len,
                                             int timeout_ms,
                                             const uint8_t* prefix = nullptr,
                                             size_t prefix_len = 0);
    void publishBitstreamCtrl();
    bool waitCoreFlag(bool wantBusy, bool wantPending, int maxUs);
    bool kickDdrSpi(int bank, bool first_verify, bool& saw_busy, bool& saw_kick, bool& saw_frame);
    bool kickDdrDoorbell(int bank);
    uint32_t ddrStatusMailboxPhys() const {
        return ddrLayout_.doorbell_phys + kDdrStatusMailboxOff;
    }
    uint32_t ddrInputMailboxPhys() const {
        return ddrLayout_.doorbell_phys + kInputMailboxOff;
    }
    static constexpr uint32_t kMgrBase = 0xFF706000;
    static constexpr uint32_t kMapBase = 0xFF000000;
    static constexpr size_t kMapSize = 0x01000000;
    static constexpr uint32_t SSPI_FPGA_EN = (1u << 18);
    static constexpr uint32_t SSPI_IO_EN = (1u << 20);
    static constexpr uint32_t SSPI_STROBE = (1u << 17);

    int bitstreamMemFd_ = -1;
    uint8_t* bitstreamMap_ = nullptr;
    size_t bitstreamMapLen_ = 0;
    uint32_t bitstreamWriteCount_ = 0;
    uint64_t bitstreamLegacySessionId_ = 1;
    uint32_t bitstreamLegacySeq_ = 0;
    bool bitstreamLegacyActive_ = false;
    bool bitstreamResetEpoch_ = false;
    uint16_t bitstreamProtocolVersion_ = 0;
    bool bitstreamSessionActive_ = false;
    uint64_t bitstreamSessionId_ = 0;
    uint32_t bitstreamNextSeq_ = 0;
    VideoCapabilities videoCapabilities_{};
    // MrAudio session lane helpers (controlAudioSession and the hard-kill recovery).
    bool readMrAudioProducerPosition(uint32_t& position);
    uint64_t publishAudioControl(uint64_t session_id, uint64_t nonce, AudioControl command,
                                 uint32_t producerPosition);
    // The consumer's MAST as published, with no session/nonce filter.
    bool readAudioConsumerStatus(AudioSessionStatus& status);
    // Fresh MAST: take the first coherent publication as the baseline, then wait until the
    // mailbox has published `fresh` more times (its poll republishes MAST every few tens
    // of microseconds), so a publication left in DDR by an earlier core instance or an
    // earlier poll is never taken for the consumer's state. False: none came in time.
    bool readFreshAudioConsumerStatus(unsigned fresh, int timeout_ms,
                                      AudioSessionStatus& status);
    // After a command of this session got no bound ACK: withdraw it from the control
    // slot (a later poll must not dispatch it), then read fresh MAST. Owned: the
    // consumer runs under this session's identity. NotOwned: it runs under another one
    // (or none), so this session's command was rejected or never forwarded. Wedged: no
    // fresh publication (the mailbox waits on a consumer ACK, or its DDR lane stalls).
    AudioLane classifyAudioLane(uint64_t session_id, uint64_t nonce, uint64_t token,
                                AudioControl command, std::string& why,
                                AudioSessionStatus& status);
    // A killed daemon leaves the MrAudio consumer active under its own epoch/nonce;
    // the consumer then rejects every other identity's Begin and Reset (sys/alsa.sv).
    // Once the AU ring reset has made video inactive, Reset with the consumer's own
    // reported epoch/nonce is the RTL's recovery path (audio_session_mailbox.sv
    // reset_recovery_identity). No-op when the consumer is inactive.
    bool recoverOrphanedAudioConsumer(const VideoCapabilities& capabilities, int timeout_ms);
    // Tokens start at CLOCK_BOOTTIME ns, so they exceed every token an earlier
    // process on this boot sent (the consumer requires token > last_token when the
    // nonce is unchanged, as in the recovery Reset).
    static uint64_t initialAudioCommandToken();
    uint64_t audioCommandToken_ = initialAudioCommandToken();
    uint32_t audioPublication_ = 0;
    bool audioSessionOwned_ = false;
    std::string audioRecoveryNote_;
    bool beginWedged_ = false;
    // The capability reply of the last core-gate probe (its features name the MrAudio lane).
    VideoCapabilities gateCapabilities_{};
    bool bitstreamResetPending_ = false;
    bool bitstreamAbortPending_ = false;
};

} // namespace misterplex

// Test seam (T-CG-FIXTURE-HOOK): runs inside the SPI window once every Main task is
// stopped, before GPO is sampled (fpga_spi.cpp:304). Only test_core_gate builds define
// MPX_TEST_MAINSAFE_HOOK_ON. Call sites carry no ';', so release expands to no tokens.
#ifdef MPX_TEST_MAINSAFE_HOOK_ON
namespace misterplex { void testMainSafeHook(); }
#define MPX_TEST_MAINSAFE_HOOK() ::misterplex::testMainSafeHook();
#else
#define MPX_TEST_MAINSAFE_HOOK()
#endif
#if defined(MPX_TEST_MAINSAFE_HOOK_ON) && (defined(__arm__) || defined(__aarch64__))
#error "MPX_TEST_MAINSAFE_HOOK_ON is a host test seam (test_core_gate); it must never reach an ARM build"
#endif
