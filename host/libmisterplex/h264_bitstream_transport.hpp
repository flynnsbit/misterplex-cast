#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace misterplex::h264stream {

enum class PushResult : uint8_t { Ok, Full, Desync, Fatal };
enum class ControlResult : uint8_t { Ok, ActiveSession, NoSession, Desync, Fatal };

inline const char* toString(PushResult r) {
    switch (r) {
    case PushResult::Ok: return "Ok";
    case PushResult::Full: return "Full";
    case PushResult::Desync: return "Desync";
    case PushResult::Fatal: return "Fatal";
    }
    return "?";
}

inline const char* toString(ControlResult r) {
    switch (r) {
    case ControlResult::Ok: return "Ok";
    case ControlResult::ActiveSession: return "ActiveSession";
    case ControlResult::NoSession: return "NoSession";
    case ControlResult::Desync: return "Desync";
    case ControlResult::Fatal: return "Fatal";
    }
    return "?";
}

struct NalView {
    uint64_t session_id = 0;
    uint32_t seq = 0;
    uint8_t nal_type = 0;
    const uint8_t* annexb = nullptr; // Includes the Annex-B start code.
    size_t len = 0;
};

struct Telemetry {
    uint64_t session_id = 0;
    size_t ring_level_bytes = 0;
    size_t ring_capacity_bytes = 0;
    uint32_t producer_seq = 0;
    uint32_t consumer_seq = 0;
    uint64_t bytes_accepted = 0;
    uint64_t nal_accepted = 0;
    uint64_t underrun_count = 0;
    uint64_t overrun_count = 0;
    uint64_t desync_count = 0;
    uint32_t last_bad_seq = 0;
    bool active = false;
    bool paused = false;
};

class IBitstreamProducer {
public:
    virtual ~IBitstreamProducer() = default;

    // Synchronous: returns only after the consumer/ring is reset for session_id.
    // begin() while a session is active must return ActiveSession; call end() first.
    virtual ControlResult begin(uint64_t session_id) = 0;

    // Copy-on-push: on Ok, bytes have been copied/accepted before this returns;
    // caller may immediately reuse or free nal.annexb.
    virtual PushResult pushNal(const NalView& nal) = 0;

    // Synchronous: returns only when no partial NAL from this session remains visible.
    virtual ControlResult flush(uint64_t session_id) = 0;
    virtual ControlResult end(uint64_t session_id) = 0;

    // Pause freezes the consumer on the last decoded frame. The source may keep
    // the HTTP demux alive and stop feeding NALs until resume().
    virtual ControlResult pause(uint64_t session_id) = 0;
    virtual ControlResult resume(uint64_t session_id) = 0;

    virtual Telemetry status() const = 0;
};

// Producers whose consumer takes whole records (for example the owenb h264ring v1.x core) rather
// than a NAL stream. On these producers, pushNal() carries ONE complete Annex-B access unit per
// call with no timestamp. pushAccessUnit() carries the same AU plus its PTS and record flags.
// pushPcm() carries interleaved s16le stereo audio, so its length must be a non-zero multiple
// of 4. Sequence and session rules are the same as pushNal(): seq counts AUs, and PCM records do
// not take a seq number.
inline constexpr int64_t kNoPts90k = -1;

struct AccessUnitView {
    uint64_t session_id = 0;
    uint32_t seq = 0;
    const uint8_t* annexb = nullptr;   // the whole AU, start codes included
    size_t len = 0;
    int64_t pts90k = kNoPts90k;        // 90 kHz; kNoPts90k = not valid
    bool discontinuity = false;        // the decoder resynchronises at this AU (seek)
    bool end_of_stream = false;        // the last AU: the core holds this picture
    bool low_latency = false;          // present as soon as decoded (no reorder delay)
};

struct PcmView {
    uint64_t session_id = 0;
    const uint8_t* s16le = nullptr;    // 48 kHz stereo, L/R interleaved
    size_t len = 0;                    // bytes; a non-zero multiple of 4
    int64_t pts90k = kNoPts90k;
};

class IAvRecordProducer : public IBitstreamProducer {
public:
    virtual PushResult pushAccessUnit(const AccessUnitView& au) = 0;
    virtual PushResult pushPcm(const PcmView& pcm) = 0;
};

// Host-only copy-ring implementation used by source-side unit tests and as an
// executable contract for the w-b1 transport. It validates copy-on-push,
// sequence continuity, Full/overrun, underrun telemetry, and session ownership.
class CopyRingBitstreamProducer final : public IBitstreamProducer {
public:
    explicit CopyRingBitstreamProducer(size_t capacityBytes) : capacity_(capacityBytes) {}

    ControlResult begin(uint64_t session_id) override {
        if (active_)
            return ControlResult::ActiveSession;
        ring_.clear();
        session_id_ = session_id;
        producer_seq_ = 0;
        consumer_seq_ = 0;
        active_ = true;
        paused_ = false;
        return ControlResult::Ok;
    }

    PushResult pushNal(const NalView& nal) override {
        if (!active_ || nal.session_id != session_id_ || !nal.annexb || nal.len == 0)
            return PushResult::Fatal;
        if (nal.seq != producer_seq_) {
            ++desync_count_;
            last_bad_seq_ = nal.seq;
            return PushResult::Desync;
        }
        if (ring_.size() + nal.len > capacity_) {
            ++overrun_count_;
            return PushResult::Full;
        }
        for (size_t i = 0; i < nal.len; ++i)
            ring_.push_back(nal.annexb[i]);
        ++producer_seq_;
        bytes_accepted_ += nal.len;
        ++nal_accepted_;
        return PushResult::Ok;
    }

    ControlResult flush(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return ControlResult::NoSession;
        ring_.clear();
        consumer_seq_ = producer_seq_;
        return ControlResult::Ok;
    }

    ControlResult end(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return ControlResult::NoSession;
        ring_.clear();
        active_ = false;
        paused_ = false;
        return ControlResult::Ok;
    }

    ControlResult pause(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return ControlResult::NoSession;
        paused_ = true;
        return ControlResult::Ok;
    }

    ControlResult resume(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return ControlResult::NoSession;
        paused_ = false;
        return ControlResult::Ok;
    }

    Telemetry status() const override {
        Telemetry t;
        t.session_id = session_id_;
        t.ring_level_bytes = ring_.size();
        t.ring_capacity_bytes = capacity_;
        t.producer_seq = producer_seq_;
        t.consumer_seq = consumer_seq_;
        t.bytes_accepted = bytes_accepted_;
        t.nal_accepted = nal_accepted_;
        t.underrun_count = underrun_count_;
        t.overrun_count = overrun_count_;
        t.desync_count = desync_count_;
        t.last_bad_seq = last_bad_seq_;
        t.active = active_;
        t.paused = paused_;
        return t;
    }

    size_t consumeBytes(size_t n) {
        const size_t got = std::min(n, ring_.size());
        for (size_t i = 0; i < got; ++i)
            ring_.pop_front();
        if (got == 0)
            ++underrun_count_;
        if (got != 0 && ring_.empty())
            consumer_seq_ = producer_seq_;
        return got;
    }

    std::vector<uint8_t> snapshot() const { return {ring_.begin(), ring_.end()}; }

private:
    size_t capacity_ = 0;
    std::deque<uint8_t> ring_;
    uint64_t session_id_ = 0;
    uint32_t producer_seq_ = 0;
    uint32_t consumer_seq_ = 0;
    uint64_t bytes_accepted_ = 0;
    uint64_t nal_accepted_ = 0;
    uint64_t underrun_count_ = 0;
    uint64_t overrun_count_ = 0;
    uint64_t desync_count_ = 0;
    uint32_t last_bad_seq_ = 0;
    bool active_ = false;
    bool paused_ = false;
};

} // namespace misterplex::h264stream
