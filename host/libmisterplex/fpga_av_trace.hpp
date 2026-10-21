#pragma once

#ifndef MPX_FPGA_AV_TRACE
#define MPX_FPGA_AV_TRACE 0
#endif

#if MPX_FPGA_AV_TRACE != 0 && MPX_FPGA_AV_TRACE != 1
#error MPX_FPGA_AV_TRACE must be 0 or 1
#endif

#if MPX_FPGA_AV_TRACE
#define MPX_AV_TRACE(...) __VA_ARGS__

#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <limits>
#include <string>
#include <time.h>
#include <type_traits>

namespace misterplex {

enum class FpgaAvLane : uint8_t { Audio, Video, Overlay };
enum class FpgaAvEvent : uint8_t {
    AudioDrain, AudioAdvance, AudioClock, AudioWrite, AudioWait, AudioEof,
    VideoOpen, VideoRead, VideoStatus, VideoPresentation, VideoClock,
    VideoCommit, VideoSubmit, VideoPace, VideoWait, OverlaySend, VideoDrain,
    AudioOpen, AudioExit, Quiesce, Release, Count
};
enum class FpgaAvWait : uint8_t {
    Paused, VideoNotStarted, PcmEmpty, ClockUnavailable, QueueHigh, AudibleDue,
    PresentationQueue, Pts, RingFull, FirstPresentation, EofDrain
};

inline bool fpgaAvMonoUs(int64_t& value) noexcept {
    const int savedErrno = errno;
    timespec time{};
    const bool ok = ::clock_gettime(CLOCK_MONOTONIC, &time) == 0;
    if (ok) value = int64_t(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
    errno = savedErrno;
    return ok;
}

struct FpgaAvStamp {
    int64_t mono = 0, active = 0;
    bool monoValid = false, activeValid = false;
};

// Published while the EXISTING pauseClockMu is held. Readers never take it
// or retry: a concurrent transition makes the active timestamp unavailable.
class FpgaAvPauseClock {
public:
    void beginUpdate() noexcept {
        version_.fetch_add(1, std::memory_order_acq_rel);
        // Pairs with the reader fence if any relaxed payload load sees this
        // update, so its final version observation cannot predate the update.
        std::atomic_thread_fence(std::memory_order_release);
    }
    void endUpdate(int64_t accumulated, int64_t started, bool held) noexcept {
        accumulated_.store(accumulated, std::memory_order_relaxed);
        started_.store(started, std::memory_order_relaxed);
        held_.store(held, std::memory_order_relaxed);
        version_.fetch_add(1, std::memory_order_release);
    }
    template<class Clock>
    FpgaAvStamp sample(Clock clock) const noexcept {
        FpgaAvStamp out;
        const auto before = version_.load(std::memory_order_acquire);
        const auto accumulated = accumulated_.load(std::memory_order_relaxed);
        const auto started = started_.load(std::memory_order_relaxed);
        const auto held = held_.load(std::memory_order_relaxed);
        out.monoValid = clock(out.mono);
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto after = version_.load(std::memory_order_relaxed);
        out.activeValid = out.monoValid && !(before & 1) && before == after;
        if (out.activeValid)
            out.active = out.mono - accumulated -
                (held && out.mono > started ? out.mono - started : 0);
        return out;
    }
    FpgaAvStamp sample() const noexcept { return sample(fpgaAvMonoUs); }
private:
    static_assert(std::atomic<int64_t>::is_always_lock_free,
                  "Trace requires nonblocking 64-bit atomics");
    std::atomic<uint32_t> version_{0};
    std::atomic<int64_t> accumulated_{0}, started_{0};
    std::atomic<bool> held_{false};
};

struct FpgaAvAnchor {
    int64_t monoBefore = 0, realtimeNs = 0, monoAfter = 0;
    uint8_t valid = 0;
    static FpgaAvAnchor capture() noexcept {
        const int savedErrno = errno;
        FpgaAvAnchor a;
        if (fpgaAvMonoUs(a.monoBefore)) a.valid |= 1;
        timespec real{};
        if (!::clock_gettime(CLOCK_REALTIME, &real)) {
            a.realtimeNs = int64_t(real.tv_sec) * 1000000000 + real.tv_nsec;
            a.valid |= 2;
        }
        if (fpgaAvMonoUs(a.monoAfter)) a.valid |= 4;
        errno = savedErrno;
        return a;
    }
};

struct FpgaAvRecord {
    enum : uint16_t {
        Begin = 1, End = 2, ActiveBegin = 4, ActiveEnd = 8,
        Acquired = 16, ServiceBegin = 32, ServiceEnd = 64, Released = 128,
        ApproximatePressure = 256, ApproximateWritten = 512
    };
    int64_t begin = 0, end = 0, activeBegin = 0, activeEnd = 0;
    int64_t acquired = 0, serviceBegin = 0, serviceEnd = 0, released = 0;
    std::array<uint64_t, 16> values{};
    uint16_t valid = 0, signedValues = 0, flags = 0;
    FpgaAvEvent event{};
    FpgaAvLane lane{};
};
static_assert(std::is_trivially_copyable<FpgaAvRecord>::value, "Numeric records only");

template<size_t Audio = 8192, size_t Video = 8192, size_t Overlay = 256>
class BasicFpgaAvTrace {
    static constexpr uint32_t kFrozen = 1u << 31;
    static constexpr size_t kEvents = static_cast<size_t>(FpgaAvEvent::Count);
    static constexpr std::array<size_t, 3> capacities_{{Audio, Video, Overlay}};
    static constexpr std::array<size_t, 3> offsets_{{0, Audio, Audio + Video}};
    struct alignas(64) Lane {
        std::atomic<uint64_t> attempted{0}, skipped{0}, reserved{0};
    };
public:
    class Writer {
    public:
        Writer(BasicFpgaAvTrace* trace, FpgaAvLane lane) noexcept : trace_(trace), lane_(lane) {
            if (trace_ && (trace_->producers_.fetch_add(1, std::memory_order_acq_rel) & kFrozen)) {
                trace_->producers_.fetch_sub(1, std::memory_order_release);
                trace_ = nullptr;
            }
        }
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        ~Writer() { finish(); }
        void finish() noexcept {
            if (trace_) {
                trace_->producers_.fetch_sub(1, std::memory_order_release);
                trace_ = nullptr;
            }
        }
        explicit operator bool() const noexcept { return trace_ != nullptr; }
        void record(FpgaAvRecord record, bool force = false) noexcept {
            if (!trace_) return;
            const size_t lane = static_cast<size_t>(lane_);
            const size_t kind = static_cast<size_t>(record.event);
            auto& counters = trace_->lanes_[lane];
            counters.attempted.fetch_add(1, std::memory_order_relaxed);
            const int64_t gap = record.event == FpgaAvEvent::AudioWait ||
                record.event == FpgaAvEvent::VideoWait ? 20000 : 5000;
            const bool timed = (record.flags & (FpgaAvRecord::Begin | FpgaAvRecord::End)) ==
                (FpgaAvRecord::Begin | FpgaAvRecord::End);
            const bool slow = timed && record.end - record.begin >= 1000;
            const bool hasOutcome = record.valid & 1;
            const bool changed = hasOutcome && outcomeSeen_[kind] &&
                outcome_[kind] != record.values[0];
            if (hasOutcome) { outcome_[kind] = record.values[0]; outcomeSeen_[kind] = true; }
            if (!force && !slow && !changed && timed && seen_[kind] &&
                record.end - last_[kind] < gap) {
                counters.skipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            last_[kind] = record.end;
            seen_[kind] = true;
            const uint64_t slot = counters.reserved.fetch_add(1, std::memory_order_relaxed);
            if (slot >= capacities_[lane]) return;
            record.lane = lane_;
            trace_->records_[offsets_[lane] + static_cast<size_t>(slot)] = record;
        }
    private:
        BasicFpgaAvTrace* trace_;
        FpgaAvLane lane_;
        std::array<int64_t, kEvents> last_{};
        std::array<bool, kEvents> seen_{};
        std::array<uint64_t, kEvents> outcome_{};
        std::array<bool, kEvents> outcomeSeen_{};
    };

    class Span {
    public:
        Span(Writer& writer, FpgaAvEvent event, const FpgaAvPauseClock& clock) noexcept
            : writer_(writer), clock_(clock), enabled_(bool(writer)) {
            record_.event = event;
            if (enabled_) {
                const auto s = clock_.sample();
                record_.begin = s.mono;
                record_.activeBegin = s.active;
                if (s.monoValid) record_.flags |= FpgaAvRecord::Begin;
                if (s.activeValid) record_.flags |= FpgaAvRecord::ActiveBegin;
            }
        }
        Span(const Span&) = delete;
        Span& operator=(const Span&) = delete;
        ~Span() {
            if (!enabled_) return;
            const auto s = clock_.sample();
            record_.end = s.mono;
            record_.activeEnd = s.active;
            if (s.monoValid) record_.flags |= FpgaAvRecord::End;
            if (s.activeValid) record_.flags |= FpgaAvRecord::ActiveEnd;
            writer_.record(record_, force_);
        }
        void value(size_t index, int64_t value) noexcept {
            uvalue(index, static_cast<uint64_t>(value));
            record_.signedValues |= uint16_t(1u << index);
        }
        void uvalue(size_t index, uint64_t value) noexcept {
            record_.values[index] = value;
            record_.valid |= uint16_t(1u << index);
            record_.signedValues &= uint16_t(~(1u << index));
        }
        void acquired() noexcept { stamp(record_.acquired, FpgaAvRecord::Acquired); }
        void serviceBegin() noexcept { stamp(record_.serviceBegin, FpgaAvRecord::ServiceBegin); }
        void serviceEnd() noexcept { stamp(record_.serviceEnd, FpgaAvRecord::ServiceEnd); }
        void released() noexcept { stamp(record_.released, FpgaAvRecord::Released); }
        void flags(uint16_t flags) noexcept { record_.flags |= flags; }
        void force() noexcept { force_ = true; }
    private:
        void stamp(int64_t& value, uint16_t flag) noexcept {
            if (enabled_ && fpgaAvMonoUs(value)) record_.flags |= flag;
        }
        Writer& writer_;
        const FpgaAvPauseClock& clock_;
        FpgaAvRecord record_{};
        bool enabled_, force_ = false;
    };

    BasicFpgaAvTrace(uint64_t epoch, uint64_t session)
        : epoch_(epoch), session_(session), begin_(FpgaAvAnchor::capture()) {}
    // Owner only, before audio/overlay producers are started.
    void bindNonce(uint64_t nonce) noexcept { nonce_ = nonce; nonceValid_ = nonce != 0; }
    bool freeze() noexcept {
        uint32_t expected = 0;
        if (!producers_.compare_exchange_strong(expected, kFrozen, std::memory_order_acq_rel))
            return false;
        end_ = FpgaAvAnchor::capture();
        return true;
    }
    bool frozen() const noexcept { return producers_.load() == kFrozen; }
    uint64_t attempted(size_t lane) const { return lanes_[lane].attempted.load(); }
    uint64_t skipped(size_t lane) const { return lanes_[lane].skipped.load(); }
    uint64_t retained(size_t lane) const {
        const auto n = lanes_[lane].reserved.load();
        return n < capacities_[lane] ? n : capacities_[lane];
    }
    uint64_t dropped(size_t lane) const {
        return lanes_[lane].reserved.load() - retained(lane);
    }
    const FpgaAvRecord& at(size_t lane, size_t slot) const {
        return records_[offsets_[lane] + slot];
    }
    static constexpr size_t storageBytes() { return sizeof(BasicFpgaAvTrace); }
    static constexpr size_t maxRows() { return Audio + Video + Overlay; }

    template<class Log>
    bool dump(Log log) const {
        if (!frozen()) return false;
        int64_t started = 0, finished = 0;
        const bool startValid = fpgaAvMonoUs(started);
        std::string header = "FPGA_AV_TRACE_BEGIN v=1 epoch=" + std::to_string(epoch_) +
            " session=" + std::to_string(session_) + " nonce=" +
            (nonceValid_ ? std::to_string(nonce_) : "unavailable") +
            " storage_bytes=" + std::to_string(storageBytes()) +
            " fast_sample_us=5000 wait_sample_us=20000 slow_keep_us=1000 keep_outcome_changes=1";
        auto anchor = [&](const char* name, const FpgaAvAnchor& a) {
            header += " " + std::string(name) + "=" + std::to_string(a.valid) + "," +
                (a.valid & 1 ? std::to_string(a.monoBefore) : "u") + "," +
                (a.valid & 2 ? std::to_string(a.realtimeNs) : "u") + "," +
                (a.valid & 4 ? std::to_string(a.monoAfter) : "u");
        };
        anchor("begin_anchor", begin_);
        anchor("end_anchor", end_);
        header += std::string(" truncated=") +
            (dropped(0) || dropped(1) || dropped(2) ? "1" : "0");
        for (size_t lane = 0; lane < 3; ++lane)
            header += " lane" + std::to_string(lane) + "=" +
                std::to_string(capacities_[lane]) + "," + std::to_string(attempted(lane)) + "," +
                std::to_string(skipped(lane)) + "," + std::to_string(retained(lane)) + "," +
                std::to_string(dropped(lane));
        log(header);
        std::string chunk;
        chunk.reserve(65536);
        uint64_t bytes = header.size(), chunks = 0;
        auto append = [&](auto value) {
            char text[32];
            const auto result = std::to_chars(text, text + sizeof(text), value);
            chunk.append(text, result.ptr);
        };
        auto field = [&](int64_t value, bool valid) {
            chunk += ',';
            if (valid) append(value); else chunk += 'u';
        };
        for (size_t lane = 0; lane < 3; ++lane) {
            for (size_t n = 0; n < retained(lane); ++n) {
                if (chunk.empty())
                    chunk = "FPGA_AV_TRACE_ROWS epoch=" + std::to_string(epoch_) +
                        " session=" + std::to_string(session_) + "\n";
                const auto& r = at(lane, n);
                append(lane); chunk += ','; append(n); chunk += ',';
                append(unsigned(r.event)); chunk += ','; append(r.flags);
                field(r.begin, r.flags & FpgaAvRecord::Begin);
                field(r.end, r.flags & FpgaAvRecord::End);
                field(r.activeBegin, r.flags & FpgaAvRecord::ActiveBegin);
                field(r.activeEnd, r.flags & FpgaAvRecord::ActiveEnd);
                field(r.acquired, r.flags & FpgaAvRecord::Acquired);
                field(r.serviceBegin, r.flags & FpgaAvRecord::ServiceBegin);
                field(r.serviceEnd, r.flags & FpgaAvRecord::ServiceEnd);
                field(r.released, r.flags & FpgaAvRecord::Released);
                for (size_t i = 0; i < r.values.size(); ++i) {
                    chunk += ',';
                    if (!(r.valid & (1u << i))) chunk += 'u';
                    else if (r.signedValues & (1u << i)) append(static_cast<int64_t>(r.values[i]));
                    else append(r.values[i]);
                }
                chunk += '\n';
                if (chunk.size() >= 49152) {
                    bytes += chunk.size(); ++chunks;
                    log(chunk);
                    chunk.clear();
                }
            }
        }
        if (!chunk.empty()) { bytes += chunk.size(); ++chunks; log(chunk); }
        const bool endValid = fpgaAvMonoUs(finished);
        log("FPGA_AV_TRACE_END epoch=" + std::to_string(epoch_) +
            " session=" + std::to_string(session_) + " bytes=" + std::to_string(bytes) +
            " chunks=" + std::to_string(chunks) + " serialize_emit_us=" +
            (startValid && endValid ? std::to_string(finished - started) : "unavailable"));
        return true;
    }
private:
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "Trace counters must not lock");
    uint64_t epoch_, session_, nonce_ = 0;
    bool nonceValid_ = false;
    FpgaAvAnchor begin_, end_{};
    std::atomic<uint32_t> producers_{0};
    std::array<Lane, 3> lanes_{};
    std::array<FpgaAvRecord, Audio + Video + Overlay> records_;
};

using FpgaAvTrace = BasicFpgaAvTrace<>;

} // namespace misterplex
#else
#define MPX_AV_TRACE(...)
#endif
