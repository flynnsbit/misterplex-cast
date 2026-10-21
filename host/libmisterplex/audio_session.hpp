#pragma once

#include <array>
#include <cstdint>
#include "mailbox_abi_spec.hpp"

namespace misterplex::audio_session {

// Begin/Reset require a quiescent HPS PCM producer. The DMA consumer retires
// its outstanding DDR read, aligns rptr to the stable kernel producer position
// carried by the request, discards prefetch/old PCM, and zeros the sample clock.
// Begin leaves an active PAUSED session for priming; Reset leaves it inactive.
// Pause acknowledges only at a sample boundary after DDR retirement, preserving
// all unplayed samples. Resume also carries the current quiesced producer
// position and waits until actual SPI DMA metadata agrees before consuming any
// sample. It continues the retained position; it does not flush.
// If local video reset erases its identity, Reset alone may recover the
// coherently reported actual consumer epoch/nonce while video is inactive.
// That recovery never authorizes another opcode or overrides active video.
enum class Control : uint8_t { Begin = 1, Pause = 2, Resume = 3, Reset = 4 };
constexpr uint8_t kActive = 1u << 0;
constexpr uint8_t kPaused = 1u << 1;
// rptr already advances when this read starts; neither flag is consumption.
constexpr uint8_t kReadPending = 1u << 2;
constexpr uint8_t kPrefetched = 1u << 3;
constexpr uint8_t kSupported = 1u << 4;
constexpr uint8_t kKnownFlags = 0x1f;

struct Status {
    uint64_t session_id = 0; // Actual consumer session, retained after Reset.
    uint64_t nonce = 0;      // Probe nonce bound to that consumer session.
    uint64_t token = 0;      // Echo of the latest acknowledged command token.
    // Valid stereo pairs assigned to PCM at 48 kHz, NOT DMA requests/writes.
    // Frozen while paused, inactive, or empty; cleared only by Begin/Reset.
    uint64_t samples_consumed = 0;
    uint64_t ack_session_id = 0; // Request identity, including rejected commands.
    uint64_t ack_nonce = 0;
    uint32_t publication = 0;
    Control command = Control::Reset;
    uint8_t error = 0; // 0: applied; 1: rejected opcode/identity/token/state.
    bool active = false;
    bool paused = true;
    bool read_pending = false;
    bool prefetched = false;
    bool supported = false;
};

inline bool validControl(Control command) {
    return command >= Control::Begin && command <= Control::Reset;
}

// Serialization only: zero is a literal ring position, NEVER "unknown".
// Production Begin/Reset/Resume must supply a successfully observed pointer;
// Pause ignores this field. FpgaSpi obtains and validates it before publishing.
inline std::array<uint64_t, 6> encodeControl(uint64_t session, uint64_t nonce,
                                            uint64_t token, Control command,
                                            uint32_t publication,
                                            uint32_t producerPosition = 0) {
    return {{
        mailbox_abi::kAudioControlMagic |
            (uint64_t(mailbox_abi::kAudioSessionAbiVersion) << 32) |
            (uint64_t(command) << 48),
        session, nonce, token, producerPosition,
        mailbox_abi::kAudioControlCommitMagic | (uint64_t(publication) << 32),
    }};
}

inline bool decodeStatus(const std::array<uint64_t, 8>& words, Status& out) {
    if (uint32_t(words[0]) != mailbox_abi::kAudioStatusMagic ||
        uint8_t(words[0] >> 32) != mailbox_abi::kAudioSessionAbiVersion ||
        uint32_t(words[7]) != mailbox_abi::kAudioStatusCommitMagic ||
        uint32_t(words[7] >> 32) == 0)
        return false;
    const uint8_t flags = uint8_t(words[0] >> 40);
    if ((flags & ~kKnownFlags) != 0)
        return false;
    Status status;
    status.session_id = words[1];
    status.nonce = words[2];
    status.token = words[3];
    status.samples_consumed = words[4];
    status.ack_session_id = words[5];
    status.ack_nonce = words[6];
    status.publication = uint32_t(words[7] >> 32);
    status.command = Control(uint8_t(words[0] >> 48));
    status.error = uint8_t(words[0] >> 56);
    status.active = (flags & kActive) != 0;
    status.paused = (flags & kPaused) != 0;
    status.read_pending = (flags & kReadPending) != 0;
    status.prefetched = (flags & kPrefetched) != 0;
    status.supported = (flags & kSupported) != 0;
    out = status;
    return true;
}

inline bool matchesAck(const Status& status, uint64_t session, uint64_t nonce,
                       uint64_t token, Control command) {
    return session != 0 && nonce != 0 && token != 0 && status.supported &&
           status.ack_session_id == session && status.ack_nonce == nonce &&
           status.session_id == session && status.nonce == nonce &&
           status.token == token && status.command == command;
}

inline bool completed(const Status& status, Control command) {
    if (status.error != 0)
        return false;
    switch (command) {
    case Control::Begin:
        return status.active && status.paused && !status.read_pending &&
               !status.prefetched && status.samples_consumed == 0;
    case Control::Pause: return status.active && status.paused && !status.read_pending;
    case Control::Resume: return status.active && !status.paused;
    case Control::Reset:
        return !status.active && status.paused && !status.read_pending &&
               !status.prefetched && status.samples_consumed == 0;
    }
    return false;
}

} // namespace misterplex::audio_session
