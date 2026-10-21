#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <string>
#include "mailbox_abi_spec.hpp"

namespace misterplex::ddr_bitstream_ring {

constexpr uint32_t kDataPhys = 0x30100000u;
constexpr uint32_t kCtrlPhys = mailbox_abi::kPlxbAddr;
constexpr uint32_t kReadPhys = 0x30140008u;
constexpr uint32_t kErrPhys = 0x30140010u;
constexpr uint32_t kStat0Phys = 0x30140018u;
constexpr uint32_t kStat1Phys = 0x30140020u;
constexpr uint32_t kStat2Phys = 0x30140028u;
constexpr uint32_t kStat3Phys = 0x30140030u;
constexpr uint32_t kStat4Phys = 0x30140038u;
constexpr uint32_t kStat5Phys = 0x30140040u;
constexpr uint32_t kStat6Phys = 0x30140048u;
constexpr size_t kRingBytes = 262144u;

constexpr uint32_t kCtrlMagic = mailbox_abi::kPlxbMagic;
constexpr uint32_t kReadMagic = 0x504C5852u; // PLXR, FPGA -> HPS
constexpr uint32_t kErrMagic = 0x504C5845u;  // PLXE, FPGA -> HPS
constexpr uint32_t kRecordMagic = 0x504C584Eu; // PLXN, record header
constexpr uint32_t kStat0Magic = 0x504C5854u; // PLXT, ring level
constexpr uint32_t kStat1Magic = 0x504C5855u; // PLXU, consumer seq
constexpr uint32_t kStat2Magic = 0x504C5856u; // PLXV, last bad seq
constexpr uint32_t kStat3Magic = 0x504C5857u; // PLXW, session low
constexpr uint32_t kStat4Magic = 0x504C5859u; // PLXY, session high
constexpr uint32_t kStat5Magic = 0x504C585Au; // PLXZ, underrun/overrun counts
constexpr uint32_t kStat6Magic = 0x504C5851u; // PLXQ, desync/state flags

constexpr size_t kRecordHeaderBytes = 32u;
constexpr size_t kAccessUnitMetadataBytes = 32u;
constexpr size_t kControlReserveBytes = kRecordHeaderBytes;
constexpr size_t kMaxAccessUnitBytes =
    kRingBytes - kRecordHeaderBytes - kAccessUnitMetadataBytes - kControlReserveBytes;
constexpr uint32_t kCountMask = 0x7fffffffu;
constexpr int64_t kNoTimestamp = std::numeric_limits<int64_t>::min();

constexpr int kErrTelemetrySeqShift = 32;
constexpr int kErrResetEpochBit = 40;
constexpr int kErrUnderrunStickyBit = 45;
constexpr int kErrOverrunStickyBit = 46;
constexpr int kErrActiveBit = 47;
constexpr int kErrUnderrunCountShift = 48;
constexpr int kErrOverrunCountShift = 56;

constexpr int kStat6StateFlagsShift = 32;
constexpr int kStat6UnderrunFlagBit = 6;
constexpr int kStat6OverrunFlagBit = 7;
constexpr int kStat6ActiveFlagBit = 8;
constexpr int kStat6PausedFlagBit = 9;
constexpr int kStat6DesyncFlagBit = 10;
constexpr int kStat6FatalFlagBit = 11;
constexpr int kStat6DesyncCountShift = 48;

enum class Event : uint8_t {
    Begin = 1,
    Nal = 2,
    Flush = 3,
    End = 4,
    Pause = 5,
    Resume = 6,
    Probe = 7,
    AccessUnit = 8,
    Drain = 9,
};

enum VideoFeature : uint32_t {
    AccessUnits = 1u << 0,
    PictureCommit = 1u << 1,
    H264Decode = 1u << 2,
    Color420 = 1u << 3,
    Intra = 1u << 4,
    Inter = 1u << 5,
    Deblock = 1u << 6,
    ConsumedAudioClock = 1u << 7,
    SeparateOverlay = 1u << 8,
    Native240 = 1u << 9,
    Native480i = 1u << 10,
    Progressive480 = 1u << 11,
    Hd720 = 1u << 12,
    PresentationFeedback = 1u << 13,
    AudioSessionControl = 1u << 14,
    FencedReset = 1u << 15,
};

constexpr uint32_t kRequiredVideoFeatures =
    AccessUnits | PictureCommit | H264Decode | Color420 | Intra | PresentationFeedback;
constexpr uint32_t kAccessUnitKeyframe = 1u;
constexpr uint32_t kAccessUnitKnownFlags = kAccessUnitKeyframe;

struct VideoCapabilities {
    uint16_t abi_version = 0;
    uint16_t layout_id = 0;
    uint32_t features = 0;
    uint16_t max_width = 0;
    uint16_t max_height = 0;
    uint32_t max_au_bytes = 0;
    uint32_t build_id = 0;
    uint64_t nonce = 0;
    uint32_t publication = 0;

    bool supportsVideo() const {
        return abi_version == mailbox_abi::kFpgaVideoAbiVersion &&
               layout_id == mailbox_abi::kFpgaVideoLayoutId &&
               (features & kRequiredVideoFeatures) == kRequiredVideoFeatures &&
               max_width != 0 && max_height != 0 &&
               max_au_bytes != 0 && max_au_bytes <= kMaxAccessUnitBytes &&
               build_id != 0 && nonce != 0 && publication != 0;
    }
};

struct AccessUnit {
    uint64_t session_id = 0;
    uint32_t seq = 0;
    const uint8_t* annexb = nullptr;
    size_t len = 0;
    int64_t pts = kNoTimestamp;
    int64_t duration = 0;
    uint32_t timebase_num = 0;
    uint32_t timebase_den = 0;
    uint32_t flags = 0;
};

constexpr uint32_t kVideoActive = 1u << 0;
constexpr uint32_t kVideoHasFrame = 1u << 1;
constexpr uint32_t kVideoPaused = 1u << 2;
constexpr uint32_t kVideoBuffering = 1u << 3;
constexpr uint32_t kVideoAudioClock = 1u << 4;
constexpr uint32_t kVideoError = 1u << 5;
constexpr uint32_t kVideoKnownStateFlags = 0x3fu;

struct VideoPresentation {
    uint64_t session_id = 0;
    uint32_t seq = 0;
    int64_t pts = kNoTimestamp;
    uint32_t timebase_num = 0;
    uint32_t timebase_den = 0;
    uint32_t presentation_count = 0;
    uint64_t audio_samples_consumed = 0; // Frozen at this frame's actual display ACK.
    uint64_t nonce = 0;
    uint32_t error_code = 0;
    uint32_t publication = 0;
    bool active = false;
    bool has_frame = false;
    bool paused = false;
    bool buffering = false;
    bool has_audio_clock = false;
    bool error = false;
};

inline bool decodeVideoPresentation(const std::array<uint64_t, 9>& words,
                                    uint64_t expectedSession, uint64_t expectedNonce,
                                    VideoPresentation& out) {
    if (static_cast<uint32_t>(words[0]) != mailbox_abi::kVideoPresentationMagic ||
        static_cast<uint32_t>(words[8]) != mailbox_abi::kVideoPresentationCommitMagic ||
        (words[8] >> 32) == 0 || static_cast<uint32_t>(words[7]) != 0 ||
        expectedSession == 0 || words[1] != expectedSession ||
        expectedNonce == 0 || words[6] != expectedNonce)
        return false;
    const uint32_t flags = static_cast<uint32_t>(words[0] >> 32);
    if ((flags & ~kVideoKnownStateFlags) != 0)
        return false;
    VideoPresentation status;
    status.session_id = words[1];
    status.pts = static_cast<int64_t>(words[2]);
    status.timebase_num = static_cast<uint32_t>(words[3]);
    status.timebase_den = static_cast<uint32_t>(words[3] >> 32);
    status.seq = static_cast<uint32_t>(words[4]);
    status.presentation_count = static_cast<uint32_t>(words[4] >> 32);
    status.audio_samples_consumed = words[5];
    status.nonce = words[6];
    status.error_code = static_cast<uint32_t>(words[7] >> 32);
    status.publication = static_cast<uint32_t>(words[8] >> 32);
    status.active = (flags & kVideoActive) != 0;
    status.has_frame = (flags & kVideoHasFrame) != 0;
    status.paused = (flags & kVideoPaused) != 0;
    status.buffering = (flags & kVideoBuffering) != 0;
    status.has_audio_clock = (flags & kVideoAudioClock) != 0;
    status.error = (flags & kVideoError) != 0;
    if (status.has_frame &&
        (status.pts == kNoTimestamp || status.timebase_num == 0 ||
         status.timebase_num > 0x7fffffffu || status.timebase_den == 0 ||
         status.timebase_den > 0x7fffffffu || status.presentation_count == 0))
        return false;
    out = status;
    return true;
}

// Core decode_error named for the AU push-reject session end (T-DAEMON-MVPS, review 18:34).
// The MVPS error_code is the core's 8-bit decode_error (Plex.sv codec_error path). Empty unless the
// presentation reports an error with a nonzero code, so a fault without a code keeps the old message.
inline std::string decodeErrorNote(const VideoPresentation& presentation) {
    if (!presentation.error || presentation.error_code == 0)
        return {};
    std::string note = " decode_error=" + std::to_string(presentation.error_code);
    if (presentation.error_code == 22)
        note += " (sub-8x8 unsupported)";   // h264_mb_ctrl H_SUB: P sub_mb_type != 0 refused
    return note;
}

inline bool validAccessUnit(const AccessUnit& au, size_t maxBytes = kMaxAccessUnitBytes) {
    return au.session_id != 0 && au.annexb != nullptr && au.len != 0 &&
           au.len <= maxBytes && au.len <= kMaxAccessUnitBytes &&
           au.pts != kNoTimestamp && au.duration >= 0 &&
           au.timebase_num != 0 && au.timebase_num <= 0x7fffffffu &&
           au.timebase_den != 0 && au.timebase_den <= 0x7fffffffu &&
           (au.flags & ~kAccessUnitKnownFlags) == 0;
}

inline uint32_t countDistance(uint32_t newer, uint32_t older) {
    return (newer - older) & kCountMask;
}

inline void copyToDeviceBytes(volatile uint8_t* destination, const uint8_t* source,
                              size_t bytes) {
    // Records may start at any byte offset in a /dev/mem device mapping.
    // Volatile byte stores prevent memcpy/vectorization into unaligned wide stores.
    for (size_t i = 0; i < bytes; ++i)
        destination[i] = source[i];
}

inline void putLe32(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void putLe64(uint8_t* out, uint64_t value) {
    putLe32(out, static_cast<uint32_t>(value));
    putLe32(out + 4, static_cast<uint32_t>(value >> 32));
}

inline std::array<uint8_t, kAccessUnitMetadataBytes> encodeAccessUnitMetadata(
    const AccessUnit& au) {
    std::array<uint8_t, kAccessUnitMetadataBytes> bytes{};
    putLe64(bytes.data(), static_cast<uint64_t>(au.pts));
    putLe32(bytes.data() + 8, au.timebase_num);
    putLe32(bytes.data() + 12, au.timebase_den);
    putLe64(bytes.data() + 16, static_cast<uint64_t>(au.duration));
    putLe32(bytes.data() + 24, au.flags);
    return bytes;
}

inline bool decodeVideoCapabilities(const std::array<uint64_t, 8>& words,
                                    uint64_t expectedNonce,
                                    VideoCapabilities& out) {
    constexpr std::array<uint32_t, 8> magics{{
        mailbox_abi::kVideoCapsMagic, mailbox_abi::kVideoFeaturesMagic,
        mailbox_abi::kVideoDimensionsMagic, mailbox_abi::kVideoAuLimitMagic,
        mailbox_abi::kVideoBuildMagic, mailbox_abi::kVideoNonceLowMagic,
        mailbox_abi::kVideoNonceHighMagic, mailbox_abi::kVideoCapsCommitMagic,
    }};
    for (size_t i = 0; i < words.size(); ++i) {
        if (static_cast<uint32_t>(words[i]) != magics[i])
            return false;
    }
    VideoCapabilities caps;
    caps.abi_version = static_cast<uint16_t>(words[0] >> 32);
    caps.layout_id = static_cast<uint16_t>(words[0] >> 48);
    caps.features = static_cast<uint32_t>(words[1] >> 32);
    caps.max_width = static_cast<uint16_t>(words[2] >> 32);
    caps.max_height = static_cast<uint16_t>(words[2] >> 48);
    caps.max_au_bytes = static_cast<uint32_t>(words[3] >> 32);
    caps.build_id = static_cast<uint32_t>(words[4] >> 32);
    caps.nonce = (words[5] >> 32) | (words[6] & 0xffffffff00000000ull);
    caps.publication = static_cast<uint32_t>(words[7] >> 32);
    if (expectedNonce == 0 || caps.nonce != expectedNonce || caps.publication == 0)
        return false;
    out = caps;
    return true;
}

enum class PushResult {
    Ok,
    Full,
    Desync,
    Fatal,
};

struct Nal {
    uint64_t session_id = 0;
    uint32_t seq = 0;
    uint8_t nal_type = 0;
    const uint8_t* annexb = nullptr;
    size_t len = 0;
};

struct Status {
    uint32_t ring_level = 0;
    uint32_t ring_capacity = static_cast<uint32_t>(kRingBytes);
    uint32_t producer_count = 0;
    uint32_t consumer_count = 0;
    uint32_t consumer_seq = 0;
    uint16_t underrun_count = 0;
    uint16_t overrun_count = 0;
    uint16_t desync_count = 0;
    uint32_t last_bad_seq = 0;
    uint64_t session_id = 0;
    bool active = false;
    bool paused = false;
    bool underrun = false;
    bool overrun = false;
    bool desync = false;
    bool fatal = false;
    bool reset_epoch = false;
};

inline bool decodeErrStatusWord(uint64_t word, Status& status) {
    if (static_cast<uint32_t>(word) != kErrMagic)
        return false;
    status.underrun = ((word >> kErrUnderrunStickyBit) & 1u) != 0;
    status.overrun = ((word >> kErrOverrunStickyBit) & 1u) != 0;
    status.active = ((word >> kErrActiveBit) & 1u) != 0;
    status.reset_epoch = ((word >> kErrResetEpochBit) & 1u) != 0;
    status.underrun_count = static_cast<uint8_t>(word >> kErrUnderrunCountShift);
    status.overrun_count = static_cast<uint8_t>(word >> kErrOverrunCountShift);
    return true;
}

inline bool decodeStat5StatusWord(uint64_t word, Status& status) {
    if (static_cast<uint32_t>(word) != kStat5Magic)
        return false;
    const uint32_t counts = static_cast<uint32_t>(word >> 32);
    status.overrun_count = static_cast<uint16_t>(counts);
    status.underrun_count = static_cast<uint16_t>(counts >> 16);
    return true;
}

inline bool decodeStat6StatusWord(uint64_t word, Status& status) {
    if (static_cast<uint32_t>(word) != kStat6Magic)
        return false;
    const uint32_t ds = static_cast<uint32_t>(word >> 32);
    status.desync_count = static_cast<uint16_t>(ds >> 16);
    const uint16_t flags = static_cast<uint16_t>(ds);
    status.underrun = status.underrun || ((flags >> kStat6UnderrunFlagBit) & 1u);
    status.overrun = status.overrun || ((flags >> kStat6OverrunFlagBit) & 1u);
    status.active = status.active || ((flags >> kStat6ActiveFlagBit) & 1u);
    status.paused = ((flags >> kStat6PausedFlagBit) & 1u) != 0;
    status.desync = ((flags >> kStat6DesyncFlagBit) & 1u) != 0;
    status.fatal = ((flags >> kStat6FatalFlagBit) & 1u) != 0;
    return true;
}

// CTRL @ kCtrlPhys:
//   [31:0]  PLXB
//   [62:32] absolute producer byte count modulo 2^31
//   [63]    reset/flush epoch toggle
// READ @ kReadPhys:
//   [31:0]  PLXR
//   [63:32] absolute FPGA consumer byte count
// ERR @ kErrPhys:
//   [31:0]  PLXE
//   [39:32] telemetry publish seq
//   [40]    acknowledged CTRL reset epoch (ABI v2)
//   [45]    underrun sticky
//   [46]    overrun sticky
//   [47]    active
//   [55:48] saturated underrun count low byte
//   [63:56] saturated overrun count low byte
// RECORD header, little-endian, exactly kRecordHeaderBytes:
//   [0..3]   PLXN
//   [4]      Event
//   [5]      nal_type (Event::Nal only)
//   [6..7]   ABI version (2 for Probe/AccessUnit/Drain, zero for legacy records)
//   [8..15]  session_id
//   [16..19] seq (Event::Nal continuity is checked FPGA-side)
//   [20..23] payload length in bytes
//   [24..31] reserved, must be zero
//
// Probe: inactive-session request; session_id is a fresh nonzero challenge,
// seq is the requested layout ID, payload is empty. MPC0..MPC7 are written
// in order, with the nonzero publication sequence in MPC7 written last.
// AccessUnit: payload starts with 32-byte metadata, followed by complete
// Annex-B AU bytes. Metadata is little-endian:
//   [0..7] PTS (signed), [8..11] timebase numerator, [12..15] denominator,
//   [16..23] duration (signed, >=0), [24..27] flags, [28..31] reserved zero.
// Drain: no payload; acknowledges only after the final AU/picture has drained.
// Data records always leave one header of capacity for Resume/control traffic.
//
// MVPS presentation block (nine little-endian qwords, commit invalidated first):
//   0 flags[63:32] | MVPS magic[31:0]
//   1 session/epoch; 2 signed original PTS
//   3 timebase_den[63:32] | timebase_num[31:0]
//   4 unique presentation_count[63:32] | AU sequence[31:0]
//   5 consumed 48 kHz samples at this display ACK (valid only with AudioClock flag)
//   6 nonce from the acknowledged Probe, not an old DDR session
//   7 error_code[63:32] | reserved zero[31:0]
//   8 nonzero publication[63:32] | MVPC commit magic[31:0]
// A displayed-frame event, not ingress or reconstruction alone, advances
// presentation_count. Scheduled raster repeats do not increment it.
// STAT0..6 expose ring level/capacity-derived status, consumer seq,
// underrun/overrun/desync counters, last_bad_seq, active/paused/fatal flags,
// and the FPGA-visible session_id for low-cost host polling.

} // namespace misterplex::ddr_bitstream_ring
