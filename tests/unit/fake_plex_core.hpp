#pragma once
// A fake Plex core for the core-gate tests, on the same sparse fake /dev/mem file that
// fpga_spi.cpp maps (MPX_DEVMEM_PATH). It plays only the FPGA side the gate tests need:
//   - SPI: GPI bit 17 follows the GPO strobe, GPI bit 31 clear (user mode). Every strobe
//     rising edge is counted (SPI words on the wire).
//   - The AU ring control plane of ddr_bitstream_reader.sv:
//       * baseline CTRL at start;
//       * reset-epoch ACK (READ count 0, ERR epoch bit);
//       * records consumed in order: Probe -> CAPS reply MPC0..7 with the probe nonce
//         and this build's ID; Begin/End/Pause/Resume -> session/active/paused in ERR
//         and STAT3/4/6; Nal/Flush/Drain consumed.
//   - Presentation feedback (MVPS at 0x30140090): each AccessUnit of the active session
//     counts as presented the moment it is consumed (its pts and timebase).
//   - The 480p PLX page (optional: constructed with a map of kPlex480pPlxPage), as
//     ddr_frame_store.sv keeps it: PLXF and PLXD rewritten on a ~3 ms heartbeat, frames_done
//     = the frames presented by this instance. PLXJ (written only on a PLXA ACK) is not modelled.
// buildId 0 is a core that never answers a probe (a non-Plex core, or one still being
// reprogrammed). reprogram() is a new core instance: its state starts again from the
// CTRL then in DDR, and the old CAPS words stay in DDR, as on real hardware.
// The FPGA-manager stat and GPO core-reset bit belong to Main and the test: set them
// with setFabric().
//   - The MrAudio session lane (4c210d99: rtl/audio_session_mailbox.sv + sys/alsa.sv
//     g_session), every ~100 us like the mailbox poll: read the control slot, forward a
//     valid request (live_identity: this core's video session and probe nonce, active or
//     a Reset; or reset_recovery_identity: no active video, a Reset of the consumer's last
//     coherent owner), one dispatch per distinct command; the consumer applies
//     command_valid (Begin only while inactive or under the same identity; Pause/Resume/
//     Reset only under its own identity; token > last_token for an unchanged nonce); then
//     MAST is republished with a new publication. Injections model what the glass runs
//     saw: a consumer reset window (reject), an identity lane that is not live
//     (ignore), a previous owner's session left running (orphan), a mailbox stuck
//     waiting on a consumer ACK (wedge), a consumer reset mid-play (creset). A new
//     instance starts with an idle consumer.
#include "libmisterplex/audio_session.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/ddr_bitstream_ring.hpp"
#include "libmisterplex/mailbox_abi_spec.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fake_core {

namespace ring = misterplex::ddr_bitstream_ring;
namespace abi = ::mailbox_abi;

constexpr uint32_t kRegBase = 0xFF000000u;
constexpr uint32_t kRingWindowBase = ring::kDataPhys;              // 0x30100000
constexpr size_t kRingWindowLen = ring::kRingBytes + 0x1000u;      // ring + ctrl/stat/caps page
// The 480p Plex builds' PLX page: PLEX_PRESENT_480P (Plex_480p.qsf:162) selects DDR_FRAME_480P_FPGA_* =
// 0x300FF000 + 0x118 (PLXF) / 0x128 (PLXD) / 0x130 (PLXJ): present_core.sv:905-926 and
// ddr_frame_layout_params.svh:30-41, the same in 4c210d99 (0x48005D45) and 9c86d9b2 (0x48005D52).
constexpr uint32_t kPlex480pPlxPage = 0x300FF000u;

class FakePlexCore {
public:
    // The prod 45D core's CAPS features word, 0x0000E1BF: Plex.sv:994-997 in the 4c210d99
    // fit tree (0xe19f | Inter, no Deblock on 480p, no resolution-tier bits).
    static constexpr uint32_t kDefaultFeatures = 0x0000E1BFu;
    static_assert((kDefaultFeatures & ring::kRequiredVideoFeatures) == ring::kRequiredVideoFeatures,
                  "the fake core default must offer the features every session requires");

    struct CapsConfig {
        uint32_t features = kDefaultFeatures;
        uint16_t max_width = 640;
        uint16_t max_height = 480;
        uint32_t au_limit = 131072;
    };

    // regs: map of kRegBase (16 MiB). ringWindow: map of kRingWindowBase (kRingWindowLen).
    // plxPage (optional): map of kPlex480pPlxPage (4 KiB); nullptr = the page is not kept.
    FakePlexCore(volatile uint8_t* regs, volatile uint8_t* ringWindow,
                 volatile uint8_t* plxPage = nullptr)
        : regs_(regs), win_(ringWindow), plx_(plxPage) {}

    void setCapsConfig(const CapsConfig& caps) { caps_ = caps; }
    CapsConfig capsConfig() const { return caps_; }

    void setFabric(uint32_t mode, bool coreReset) {
        *reg(misterplex::kFpgaMgrStatPhys) = mode;
        uint32_t gpo = *reg(misterplex::kFpgaMgrGpoPhys);
        gpo = coreReset ? (gpo | misterplex::kGpoCoreResetBit) : (gpo & ~misterplex::kGpoCoreResetBit);
        *reg(misterplex::kFpgaMgrGpoPhys) = gpo;
    }
    // A new core instance (0 = one that never answers CAPS).
    void reprogram(uint32_t buildId) {
        buildId_.store(buildId);
        logCaps(buildId);
        restart_.store(true);
    }
    uint32_t buildId() const { return buildId_.load(); }
    uint64_t strobes() const { return strobes_.load(); }
    uint64_t probesAnswered() const { return probes_.load(); }

    // ---- MrAudio lane injections, on the running instance (any thread) ----
    // The consumer is held in reset for the next n commands that reach it: each is ACKed
    // with error 1 and leaves it inactive (alsa.sv: commands crossing reset are rejected).
    void audioRejectNext(int n) { audioRejectNext_.store(n); }
    // The mailbox forwards none of the next n distinct requests (its identity lane is not
    // live for them: session_active low); each stays unforwarded while it sits in the slot.
    void audioIgnoreNext(int n) { audioIgnoreNext_.store(n); }
    // The consumer runs a previous owner's session (a killed daemon, no core reload).
    void audioOrphan(uint64_t epoch, uint64_t nonce, uint64_t lastToken, bool playing) {
        orphanEpoch_.store(epoch);
        orphanNonce_.store(nonce);
        orphanToken_.store(lastToken);
        orphanPlaying_.store(playing);
        orphanPending_.store(true);
    }
    // The mailbox waits on a consumer ACK that never comes: nothing is read or published.
    void audioWedge(bool on) { audioWedged_.store(on); }
    // The same wedge, taking effect when the next CAPS probe is answered (a begin's probe):
    // MAST-WEDGE rows wedge the lane between a live gate-open probe and a later begin.
    void audioWedgeAtNextProbe() { wedgeAtProbe_.store(true); }
    // R5d (a lab MiSTer G2 2026-10-05): this instance is configured but its CTRL/CAPS lane never
    // answers: no CTRL baseline, no epoch ACK, no CAPS, no MAST, nothing written. A later
    // load (a new instance) answers again.
    void laneSilent() { silent_.store(true); }
    // MPX_AUDIO_RESET pulses mid-play (status[0], the OSD reset, buttons[1]): the consumer's
    // registers return to reset values (no identity, inactive); the mailbox (clk_dec) keeps
    // its state.
    void audioConsumerReset() { consumerResetPending_.store(true); }
    // Consumer cap (n2 e2e; off by default for the other e2e tests): mirror sys/alsa.sv, so a
    // pair is consumed only once it was written. The reader starts at the Start/Reset command's
    // producer cursor; it advances by the 48 kHz wall clock only up to the producer position
    // fake_mraudio_wrap.cpp publishes at kProducerSlotAddr. Short of it: silence (a gap,
    // counted), never consumed past written. Without a published producer it falls back to the
    // wall clock.
    void audioConsumerCap(bool on) { consumerCap_.store(on); }
    bool audioConsumerCapped() const { return consumerCap_.load(); }
    uint64_t audioGapPairs() const { return audioGapPairs_.load(); }
    uint64_t audioDispatched() const { return audioDispatched_.load(); }
    uint64_t audioMailboxRefused() const { return audioMailboxRefused_.load(); }
    uint64_t audioConsumerRejected() const { return audioConsumerRejected_.load(); }
    uint64_t audioPublications() const { return audioPublications_.load(); }
    bool audioConsumerActive() const { return audioActive_.load(); }
    uint64_t audioConsumerEpoch() const { return audioEpoch_.load(); }
    uint64_t audioConsumedLastActive() const { return audioConsumedLastActive_.load(); }

    // One pass of the FPGA side; call it in a tight loop.
    void step() {
        spiStep();
        if (restart_.exchange(false)) { // a new instance starts from reset
            haveCtrl_ = false;
            lastPublishedEpoch_ = false;
            session_ = 0;
            active_ = paused_ = false;
            telemSeq_ = 0;
            publication_ = 0;
            nonce_ = 0;
            silent_.store(false);
            framesDone_ = 0;
            plxfSeq_ = 0;
            plxPublished_ = false;
            restartAudioLane();
        }
        if (buildId_.load() == 0)
            return; // nothing configured that speaks the ring protocol
        if (silent_.load())
            return;
        ringStep();
        audioStep();
        plxStep();
    }

private:
    volatile uint32_t* reg(uint32_t phys) {
        return reinterpret_cast<volatile uint32_t*>(regs_ + (phys - kRegBase));
    }
    volatile uint64_t* word(uint32_t phys) {
        return reinterpret_cast<volatile uint64_t*>(win_ + (phys - kRingWindowBase));
    }

    void spiStep() {
        const uint32_t gpo = *reg(misterplex::kFpgaMgrGpoPhys);
        constexpr uint32_t kStrobe = 1u << 17;
        constexpr uint32_t kIoEn = 1u << 20;
        if ((gpo & kStrobe) && !(lastGpo_ & kStrobe)) {
            strobes_.fetch_add(1);
            spiResponse_ = spiResponseFor(static_cast<uint16_t>(gpo & 0xFFFFu),
                                          (gpo & kIoEn) != 0);
        }
        lastGpo_ = gpo;
        volatile uint32_t* gpi = reg(0xFF706014u);
        const uint32_t next = (gpo & kStrobe) | spiResponse_;
        if (*gpi != next)
            *gpi = next; // bit 31 clear: user mode
    }

    const char* configString() const {
        switch (buildId_.load()) {
        case 0x2400F40Bu:
        case 0x4801F40Au:
            return "Plex;v,10;";
        default:
            return "Plex;v,9;";
        }
    }

    uint16_t spiResponseFor(uint16_t word, bool ioSelected) {
        constexpr uint8_t UIO_GET_STRING = 0x14;
        constexpr uint8_t UIO_GET_STATUS = 0x29;
        if (!ioSelected) {
            spiIoCommand_ = 0;
            confStrPos_ = 0;
            return 0;
        }
        if (word == UIO_GET_STRING) {
            spiIoCommand_ = UIO_GET_STRING;
            confStrPos_ = 0;
            return 0;
        }
        if (word == UIO_GET_STATUS) {
            spiIoCommand_ = UIO_GET_STATUS;
            return 0;
        }
        if (spiIoCommand_ == UIO_GET_STRING) {
            const char* s = configString();
            const uint8_t c = static_cast<uint8_t>(s[confStrPos_]);
            if (c != 0)
                ++confStrPos_;
            return c;
        }
        return 0;
    }

    void publishStatus() {
        *word(ring::kReadPhys) = (static_cast<uint64_t>(consumer_ & ring::kCountMask) << 32) |
                                 ring::kReadMagic;
        *word(ring::kErrPhys) = (static_cast<uint64_t>(active_ ? 1 : 0) << ring::kErrActiveBit) |
                                (static_cast<uint64_t>(epoch_ ? 1 : 0) << ring::kErrResetEpochBit) |
                                (static_cast<uint64_t>(++telemSeq_ & 0xff)
                                 << ring::kErrTelemetrySeqShift) |
                                ring::kErrMagic;
        *word(ring::kStat0Phys) = (static_cast<uint64_t>(
                                       ring::countDistance(producer_, consumer_)) << 32) |
                                  ring::kStat0Magic;
        *word(ring::kStat3Phys) = (static_cast<uint64_t>(static_cast<uint32_t>(session_)) << 32) |
                                  ring::kStat3Magic;
        *word(ring::kStat4Phys) = (session_ & 0xffffffff00000000ull) | ring::kStat4Magic;
        const uint32_t flags = (active_ ? (1u << ring::kStat6ActiveFlagBit) : 0u) |
                               (paused_ ? (1u << ring::kStat6PausedFlagBit) : 0u);
        *word(ring::kStat6Phys) = (static_cast<uint64_t>(flags) << ring::kStat6StateFlagsShift) |
                                  ring::kStat6Magic;
    }

    void ringStep() {
        const uint64_t ctrl = *word(ring::kCtrlPhys);
        if (static_cast<uint32_t>(ctrl) != ring::kCtrlMagic) {
            haveCtrl_ = false; // wiped (a test fill): the next valid CTRL is a new baseline
            return;
        }
        const bool epoch = (ctrl >> 63) & 1u;
        const uint32_t producer = static_cast<uint32_t>(ctrl >> 32) & ring::kCountMask;
        if (!haveCtrl_) { // a new instance: CTRL in DDR is a baseline, not work
            haveCtrl_ = true;
            epoch_ = epoch;
            producer_ = consumer_ = producer;
            session_ = 0;
            active_ = paused_ = false;
            publishStatus();
            return;
        }
        if (epoch != epoch_) { // reset fence
            epoch_ = epoch;
            consumer_ = 0;
            session_ = 0;
            active_ = paused_ = false;
            nonce_ = 0; // ST_EPOCH clears committed_nonce until the next probe's CAPS
        }
        producer_ = producer;
        bool changed = epoch != lastPublishedEpoch_;
        while (consumer_ != producer_) {
            uint8_t h[ring::kRecordHeaderBytes];
            for (size_t i = 0; i < sizeof h; ++i)
                h[i] = win_[(consumer_ + i) & (ring::kRingBytes - 1u)];
            uint32_t magic, seq, len;
            uint64_t sid;
            std::memcpy(&magic, h, 4);
            std::memcpy(&sid, h + 8, 8);
            std::memcpy(&seq, h + 16, 4);
            std::memcpy(&len, h + 20, 4);
            if (magic != ring::kRecordMagic)
                break; // a half-written record: next pass
            const auto event = static_cast<ring::Event>(h[4]);
            if (event == ring::Event::Probe)
                answerProbe(sid);
            else if (event == ring::Event::Begin) {
                session_ = sid;
                active_ = true;
                paused_ = false;
                presented_ = 0;
                frameSeq_ = 0;
                framePts_ = 0;
                frameTbNum_ = frameTbDen_ = 0;
                publishPresentation();
            } else if (event == ring::Event::End) {
                active_ = false;
                publishPresentation();
            } else if (event == ring::Event::Pause) {
                paused_ = true;
                publishPresentation();
            } else if (event == ring::Event::Resume) {
                paused_ = false;
                publishPresentation();
            } else if (event == ring::Event::AccessUnit && active_ && sid == session_ &&
                       len >= ring::kAccessUnitMetadataBytes) {
                // Presented as soon as consumed: pts and timebase from the AU metadata.
                uint8_t meta[16];
                for (size_t i = 0; i < sizeof meta; ++i)
                    meta[i] = win_[(consumer_ + ring::kRecordHeaderBytes + i) &
                                   (ring::kRingBytes - 1u)];
                std::memcpy(&framePts_, meta, 8);
                std::memcpy(&frameTbNum_, meta + 8, 4);
                std::memcpy(&frameTbDen_, meta + 12, 4);
                frameSeq_ = seq;
                ++presented_;
                ++framesDone_;
                publishPresentation();
                presentedTotal_.fetch_add(1);
            }
            consumer_ = (consumer_ + ring::kRecordHeaderBytes + len) & ring::kCountMask;
            changed = true;
        }
        if (changed) {
            lastPublishedEpoch_ = epoch_;
            publishStatus();
        }
    }

    void answerProbe(uint64_t nonce) {
        const uint32_t pub = ++publication_;
        volatile uint64_t* caps = word(abi::kVideoCapsAddr);
        const CapsConfig cfg = caps_;
        caps[7] = abi::kVideoCapsCommitMagic; // invalidate first, as the RTL does
        caps[0] = (static_cast<uint64_t>(abi::kFpgaVideoLayoutId) << 48) |
                  (static_cast<uint64_t>(abi::kFpgaVideoAbiVersion) << 32) | abi::kVideoCapsMagic;
        caps[1] = (static_cast<uint64_t>(cfg.features) << 32) | abi::kVideoFeaturesMagic;
        caps[2] = (static_cast<uint64_t>(cfg.max_height) << 48) |
                  (static_cast<uint64_t>(cfg.max_width) << 32) |
                  abi::kVideoDimensionsMagic;
        caps[3] = (static_cast<uint64_t>(cfg.au_limit) << 32) | abi::kVideoAuLimitMagic;
        caps[4] = (static_cast<uint64_t>(buildId_.load()) << 32) | abi::kVideoBuildMagic;
        caps[5] = ((nonce & 0xffffffffull) << 32) | abi::kVideoNonceLowMagic;
        caps[6] = (nonce & 0xffffffff00000000ull) | abi::kVideoNonceHighMagic;
        caps[7] = (static_cast<uint64_t>(pub) << 32) | abi::kVideoCapsCommitMagic;
        nonce_ = nonce;
        probes_.fetch_add(1);
        if (wedgeAtProbe_.exchange(false))
            audioWedged_.store(true);
    }

    void logCaps(uint32_t buildId) const {
        const CapsConfig cfg = caps_;
        const uint64_t mpc1 = (static_cast<uint64_t>(cfg.features) << 32) |
                              abi::kVideoFeaturesMagic;
        const uint64_t mpc2 = (static_cast<uint64_t>(cfg.max_height) << 48) |
                              (static_cast<uint64_t>(cfg.max_width) << 32) |
                              abi::kVideoDimensionsMagic;
        const uint64_t mpc3 = (static_cast<uint64_t>(cfg.au_limit) << 32) |
                              abi::kVideoAuLimitMagic;
        std::fprintf(stderr,
                     "fake Plex core: reprogram build=0x%08X CAPS MPC1=0x%016llX "
                     "MPC2=0x%016llX MPC3=0x%016llX features=0x%08X max=%ux%u au_limit=%u\n",
                     buildId,
                     static_cast<unsigned long long>(mpc1),
                     static_cast<unsigned long long>(mpc2),
                     static_cast<unsigned long long>(mpc3),
                     cfg.features, cfg.max_width, cfg.max_height, cfg.au_limit);
    }

    // MVPS presentation feedback for the session (decodeVideoPresentation): MVPC with
    // publication 0 first, then the body, then MVPC with the new publication.
    void publishPresentation() {
        volatile uint64_t* p = word(abi::kVideoPresentationAddr);
        const uint32_t flags = (active_ ? ring::kVideoActive : 0u) |
                               (presented_ ? ring::kVideoHasFrame : 0u) |
                               (paused_ ? ring::kVideoPaused : 0u);
        p[8] = abi::kVideoPresentationCommitMagic;
        p[0] = (static_cast<uint64_t>(flags) << 32) | abi::kVideoPresentationMagic;
        p[1] = session_;
        p[2] = static_cast<uint64_t>(framePts_);
        p[3] = (static_cast<uint64_t>(frameTbDen_) << 32) | frameTbNum_;
        p[4] = (static_cast<uint64_t>(presented_) << 32) | frameSeq_;
        p[5] = 0;
        p[6] = nonce_;
        p[7] = 0;
        p[8] = (static_cast<uint64_t>(++presentationPub_) << 32) |
               abi::kVideoPresentationCommitMagic;
    }

public:
    uint64_t presentedTotal() const { return presentedTotal_.load(); }
    void answerProbeForTest(uint64_t nonce) { answerProbe(nonce); }

private:
    // ddr_frame_store.sv (4c210d99): PLXF (:1844-1846) and PLXD (:1865-1867) are rewritten every
    // 2^18 clk_ddr (~3 ms), PLXD also on each vsync. PLXF = {underrun, debug, seq + 1, PLXF}
    // (:2011); PLXD = {frames_done (the swap count), swap_pending, disp_bank, free mask, PLXD}
    // (:2027-2033). Underrun and debug stay 0; nothing is ever pending.
    void plxStep() {
        if (!plx_)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (plxPublished_ && now < nextPlx_)
            return;
        plxPublished_ = true;
        nextPlx_ = now + std::chrono::milliseconds(3);
        ++plxfSeq_;
        *plxWord(0x118u) = (static_cast<uint64_t>(plxfSeq_) << 32) | kPlxfMagic;
        const uint64_t bank = framesDone_ & 1u;
        *plxWord(0x128u) = (static_cast<uint64_t>(framesDone_) << 48) | (bank << 34) |
                           ((bank ? 1ull : 2ull) << 32) | kPlxdMagic;
    }
    volatile uint64_t* plxWord(uint32_t off) {
        return reinterpret_cast<volatile uint64_t*>(plx_ + off);
    }

private:
    // ---- MrAudio lane: audio_session_mailbox.sv + sys/alsa.sv g_session ----
    void restartAudioLane() { // configuration: registers at reset values, DDR as it was
        mrAudio_ = AudioConsumer{};
        prefetched_ = false;
        ackEpoch_ = ackNonce_ = ackToken_ = 0;
        ackOp_ = ackError_ = 0;
        haveCommand_ = false;
        lastCommand_ = {};
        ignoredKey_ = {};
        identityValid_ = false;
        clockEpoch_ = clockNonce_ = 0;
        audioPub_ = 0;
        audioActive_.store(false);
        audioEpoch_.store(0);
        nextAudioPoll_ = {};
        lastAudioClock_ = std::chrono::steady_clock::now();
    }

    // sys/alsa.sv command_valid and its Begin/Pause/Resume/Reset effects.
    void consumerApply(uint8_t op, uint64_t e, uint64_t n, uint64_t t, uint32_t cursorPairs = 0) {
        ackEpoch_ = e;
        ackNonce_ = n;
        ackToken_ = t;
        ackOp_ = op;
        int reject = audioRejectNext_.load();
        if (reject > 0 && audioRejectNext_.compare_exchange_strong(reject, reject - 1)) {
            // Held in reset: the command is ACKed with error 1; active <= 0, paused <= 1.
            ackError_ = 1;
            mrAudio_.active = false;
            mrAudio_.paused = true;
            audioConsumerRejected_.fetch_add(1);
            return;
        }
        const bool valid = e != 0 && n != 0 && t != 0 && op >= 1 && op <= 4 &&
            ((op == 1 && (!mrAudio_.active || (e == mrAudio_.epoch && n == mrAudio_.nonce))) ||
             (op != 1 && (op == 4 || mrAudio_.active) && e == mrAudio_.epoch &&
              n == mrAudio_.nonce)) &&
            (n != mrAudio_.nonce || t > mrAudio_.lastToken);
        if (!valid) {
            ackError_ = 1;
            audioConsumerRejected_.fetch_add(1);
            return;
        }
        ackError_ = 0;
        mrAudio_.lastToken = t;
        if (op == 1 || op == 4) {
            mrAudio_.epoch = e;
            mrAudio_.nonce = n;
            mrAudio_.active = op == 1;
            mrAudio_.paused = true;
            mrAudio_.consumed = 0;
            prefetched_ = false;
            // sys/alsa.sv:296-302: the reader starts at the command's producer cursor.
            readerPairs_ = cursorPairs & kPairMask;
            if (op == 1)
                audioGapPairs_.store(0);
        } else {
            mrAudio_.paused = op == 2;
        }
    }

    // SNAPSHOT -> INVALIDATE -> WRITE_BODY -> COMMIT; the mailbox then holds the snapshot's
    // owner as the last coherently observed identity (reset_recovery_identity).
    void publishMast() {
        namespace audio = misterplex::audio_session;
        const uint8_t flags = audio::kSupported | (prefetched_ ? audio::kPrefetched : 0) |
                              (mrAudio_.paused ? audio::kPaused : 0) |
                              (mrAudio_.active ? audio::kActive : 0);
        volatile uint64_t* s = word(abi::kAudioStatusAddr);
        s[7] = 0;
        s[0] = (static_cast<uint64_t>(ackError_) << 56) | (static_cast<uint64_t>(ackOp_) << 48) |
               (static_cast<uint64_t>(flags) << 40) |
               (static_cast<uint64_t>(abi::kAudioSessionAbiVersion) << 32) |
               abi::kAudioStatusMagic;
        s[1] = mrAudio_.epoch;
        s[2] = mrAudio_.nonce;
        s[3] = ackToken_;
        s[4] = mrAudio_.consumed;
        s[5] = ackEpoch_;
        s[6] = ackNonce_;
        audioPub_ = audioPub_ == 0xffffffffu ? 1u : audioPub_ + 1u;
        s[7] = (static_cast<uint64_t>(audioPub_) << 32) | abi::kAudioStatusCommitMagic;
        identityValid_ = mrAudio_.epoch != 0 && mrAudio_.nonce != 0;
        clockEpoch_ = mrAudio_.epoch;
        clockNonce_ = mrAudio_.nonce;
        audioPublications_.fetch_add(1);
        audioActive_.store(mrAudio_.active);
        audioEpoch_.store(mrAudio_.epoch);
        // Diagnostics only (n2 e2e retry lines): the consumed clock of the last publication
        // while the consumer was active, kept across the session's Reset.
        if (mrAudio_.active)
            audioConsumedLastActive_.store(mrAudio_.consumed);
    }

    void audioStep() {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextAudioPoll_)
            return;
        nextAudioPoll_ = now + std::chrono::microseconds(100);
        if (consumerResetPending_.exchange(false)) {
            mrAudio_ = AudioConsumer{};
            prefetched_ = false;
            ackEpoch_ = ackNonce_ = ackToken_ = 0;
            ackOp_ = ackError_ = 0;
        }
        if (orphanPending_.exchange(false)) {
            mrAudio_.epoch = orphanEpoch_.load();
            mrAudio_.nonce = orphanNonce_.load();
            mrAudio_.lastToken = orphanToken_.load();
            mrAudio_.active = true;
            mrAudio_.paused = !orphanPlaying_.load();
            mrAudio_.consumed = 4800;
            prefetched_ = !mrAudio_.paused;
        }
        // The consumer's clock: 48 kHz stereo pairs while active and running.
        if (mrAudio_.active && !mrAudio_.paused) {
            const uint64_t due = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - lastAudioClock_)
                    .count()) * 48 / 1000;
            uint32_t producer = 0;
            if (consumerCap_.load() && producerPairs(producer)) {
                // sys/alsa.sv:311-323: a pair is consumed only after it was read from the ring,
                // and a ring read issues only while the reader is behind the producer; short of
                // it the output is silence and consumed holds (:316-319).
                const uint32_t avail = (producer - readerPairs_) & kPairMask;
                const uint64_t take = std::min<uint64_t>(due, avail);
                mrAudio_.consumed += take;
                readerPairs_ = (readerPairs_ + static_cast<uint32_t>(take)) & kPairMask;
                if (take < due)
                    audioGapPairs_.fetch_add(due - take);
            } else {
                mrAudio_.consumed += due;
            }
            prefetched_ = true;
        }
        lastAudioClock_ = now;
        if (audioWedged_.load())
            return; // WAIT_ACK forever: no slot read, no publication
        const uint64_t first = *word(abi::kAudioControlCommitAddr);
        if (static_cast<uint32_t>(first) == abi::kAudioControlCommitMagic && (first >> 32) != 0) {
            std::array<uint64_t, 6> r{};
            for (size_t i = 0; i < r.size(); ++i)
                r[i] = *word(abi::kAudioControlAddr + static_cast<uint32_t>(i * 8));
            const uint8_t op = static_cast<uint8_t>(r[0] >> 48);
            const bool sessionActive = active_;
            const bool live = r[1] == session_ && r[2] == nonce_ && (sessionActive || op == 4);
            const bool recovery = !sessionActive && identityValid_ && op == 4 &&
                                  r[1] == clockEpoch_ && r[2] == clockNonce_;
            const bool requestValid = r[5] == first &&
                static_cast<uint32_t>(r[0]) == abi::kAudioControlMagic &&
                static_cast<uint16_t>(r[0] >> 32) == abi::kAudioSessionAbiVersion &&
                static_cast<uint8_t>(r[0] >> 56) == 0 && op >= 1 && op <= 4 && r[1] != 0 &&
                r[2] != 0 && r[3] != 0 && (r[4] >> 19) == 0 && (r[4] & 3) == 0 &&
                (live || recovery);
            const std::array<uint64_t, 5> key{{r[1], r[2], r[3], op, r[4] >> 2}};
            const bool distinct = !haveCommand_ || key != lastCommand_;
            int ignore = audioIgnoreNext_.load();
            if (requestValid && distinct && key != ignoredKey_ && ignore > 0 &&
                audioIgnoreNext_.compare_exchange_strong(ignore, ignore - 1))
                ignoredKey_ = key; // this request's identity lane is not live
            if (requestValid && distinct && key != ignoredKey_) {
                identityValid_ = false;
                lastCommand_ = key;
                haveCommand_ = true;
                audioDispatched_.fetch_add(1);
                consumerApply(op, r[1], r[2], r[3], static_cast<uint32_t>(r[4] >> 2) & kPairMask);
            } else if (distinct && key != lastRefusedKey_) {
                lastRefusedKey_ = key;
                audioMailboxRefused_.fetch_add(1);
            }
        }
        publishMast();
    }

    struct AudioConsumer { // sys/alsa.sv registers that MAST shows
        uint64_t epoch = 0, nonce = 0, lastToken = 0, consumed = 0;
        bool active = false, paused = true;
    };
    AudioConsumer mrAudio_;
    bool prefetched_ = false;
    uint64_t ackEpoch_ = 0, ackNonce_ = 0, ackToken_ = 0;
    uint8_t ackOp_ = 0, ackError_ = 0;
    bool haveCommand_ = false, identityValid_ = false;
    std::array<uint64_t, 5> lastCommand_{}, ignoredKey_{}, lastRefusedKey_{};
    uint64_t clockEpoch_ = 0, clockNonce_ = 0;
    uint32_t audioPub_ = 0;
    std::chrono::steady_clock::time_point nextAudioPoll_{}, lastAudioClock_{};
    std::atomic<int> audioRejectNext_{0}, audioIgnoreNext_{0};
    std::atomic<bool> silent_{false};
    std::atomic<bool> audioWedged_{false}, wedgeAtProbe_{false}, orphanPending_{false}, orphanPlaying_{false},
        consumerResetPending_{false};
    std::atomic<uint64_t> orphanEpoch_{0}, orphanNonce_{0}, orphanToken_{0};
    std::atomic<uint64_t> audioDispatched_{0}, audioMailboxRefused_{0},
        audioConsumerRejected_{0}, audioPublications_{0};
    std::atomic<bool> audioActive_{false};
    std::atomic<uint64_t> audioEpoch_{0};
    std::atomic<uint64_t> audioConsumedLastActive_{0};
    // Consumer cap: the fake-only producer slot fake_mraudio_wrap.cpp writes (the last qword of
    // the 0x30140000 page; magic "MSPI" low, the MrAudio ring write pointer in bytes high).
    static constexpr uint32_t kProducerSlotAddr = 0x30140FF8u;
    static constexpr uint32_t kProducerSlotMagic = 0x4950534Du;
    static constexpr uint32_t kPairMask = 0x1FFFFu; // 512 KiB MrAudio ring in stereo pairs
    std::atomic<bool> consumerCap_{false};
    std::atomic<uint64_t> audioGapPairs_{0};
    uint32_t readerPairs_ = 0;
    bool producerPairs(uint32_t& pairs) {
        const uint64_t v = *word(kProducerSlotAddr);
        if (static_cast<uint32_t>(v) != kProducerSlotMagic)
            return false;
        pairs = (static_cast<uint32_t>(v >> 32) >> 2) & kPairMask;
        return true;
    }

private:

    volatile uint8_t* regs_;
    volatile uint8_t* win_;
    volatile uint8_t* plx_;
    CapsConfig caps_{};
    static constexpr uint64_t kPlxfMagic = 0x504C5846u; // ddr_frame_store.sv:178 MAGIC_F
    static constexpr uint64_t kPlxdMagic = 0x504C5844u; // :179 MAGIC_D
    uint16_t framesDone_ = 0;
    uint8_t plxfSeq_ = 0;
    bool plxPublished_ = false;
    std::chrono::steady_clock::time_point nextPlx_{};
    std::atomic<uint32_t> buildId_{0};
    std::atomic<bool> restart_{true};
    std::atomic<uint64_t> strobes_{0};
    std::atomic<uint64_t> probes_{0};
    uint32_t lastGpo_ = 0;
    uint16_t spiResponse_ = 0;
    uint8_t spiIoCommand_ = 0;
    size_t confStrPos_ = 0;
    bool haveCtrl_ = false;
    bool epoch_ = false;
    bool lastPublishedEpoch_ = false;
    uint32_t producer_ = 0, consumer_ = 0;
    uint64_t session_ = 0;
    bool active_ = false, paused_ = false;
    uint32_t telemSeq_ = 0;
    uint32_t publication_ = 0;
    uint64_t nonce_ = 0;           // the last probe's: CAPS and MVPS carry it
    uint32_t presented_ = 0;       // this session's presentation count
    uint32_t frameSeq_ = 0;
    int64_t framePts_ = 0;
    uint32_t frameTbNum_ = 0, frameTbDen_ = 0;
    uint32_t presentationPub_ = 0;
    std::atomic<uint64_t> presentedTotal_{0};
};

} // namespace fake_core
