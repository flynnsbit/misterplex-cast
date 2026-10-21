#pragma once

#include "libmisterplex/h264_bitstream_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace misterplex::h264stream {

inline size_t annexBStartLen(const uint8_t* p, size_t n, size_t off = 0) {
    if (!p || off >= n)
        return 0;
    if (off + 4 <= n && p[off] == 0 && p[off + 1] == 0 && p[off + 2] == 0 && p[off + 3] == 1)
        return 4;
    if (off + 3 <= n && p[off] == 0 && p[off + 1] == 0 && p[off + 2] == 1)
        return 3;
    return 0;
}

inline uint8_t annexBNalType(const uint8_t* annexb, size_t len) {
    const size_t sc = annexBStartLen(annexb, len, 0);
    if (!sc || sc >= len)
        return 0;
    return annexb[sc] & 0x1f;
}

struct DispatchConfig {
    int max_full_retries = 50;
    int full_retry_sleep_ms = 2;
    std::function<void(int)> sleep_ms = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
};

struct DispatchStats {
    uint64_t nal_seen = 0;
    uint64_t nal_pushed = 0;
    uint64_t bytes_pushed = 0;
    uint64_t nal_dropped_paused = 0;
    uint64_t sps_replayed = 0;
    uint64_t pps_replayed = 0;
    uint64_t full_retries = 0;
    uint64_t full_escalations = 0;
    uint64_t desync_or_fatal = 0;
};

class AnnexBFramer {
public:
    enum class Error { None, InvalidInput, TooLarge, Malformed, CallbackRejected };

    explicit AnnexBFramer(size_t maxAccumBytes = 2 * 1024 * 1024) : max_acc_(maxAccumBytes) {}

    void reset() {
        acc_.clear();
        search_from_ = 0;
        started_ = false;
        error_ = Error::None;
    }

    Error error() const { return error_; }
    const char* errorText() const {
        switch (error_) {
        case Error::None: return "no framing error";
        case Error::InvalidInput: return "invalid Annex-B input or buffer limit";
        case Error::TooLarge: return "Annex-B NAL exceeds the buffer limit";
        case Error::Malformed: return "malformed or truncated Annex-B stream";
        case Error::CallbackRejected: return "Annex-B consumer rejected the NAL";
        }
        return "invalid framing error";
    }

    template <typename Fn>
    bool push(const uint8_t* data, size_t len, Fn&& onNal) {
        if (error_ != Error::None)
            return false;
        if ((!data && len) || max_acc_ == 0 ||
            max_acc_ > std::numeric_limits<size_t>::max() - 4)
            return fail(Error::InvalidInput);
        while (len != 0) {
            const size_t room = max_acc_ + 4 - acc_.size();
            if (room == 0)
                return fail(Error::TooLarge);
            const size_t count = std::min(room, len);
            acc_.insert(acc_.end(), data, data + count);
            data += count;
            len -= count;
            if (!emitComplete(false, onNal))
                return false;
        }
        return true;
    }

    template <typename Fn>
    bool finish(Fn&& onNal) {
        if (error_ != Error::None)
            return false;
        return emitComplete(true, onNal);
    }

private:
    bool fail(Error error) {
        error_ = error;
        return false;
    }

    template <typename Fn>
    bool emit(size_t len, Fn& onNal) {
        if (len > max_acc_)
            return fail(Error::TooLarge);
        if constexpr (std::is_void_v<std::invoke_result_t<Fn&, const uint8_t*, size_t>>) {
            onNal(acc_.data(), len);
        } else {
            if (!onNal(acc_.data(), len))
                return fail(Error::CallbackRejected);
        }
        return true;
    }

    template <typename Fn>
    bool emitComplete(bool final, Fn& onNal) {
        if (!started_) {
            size_t start = 0;
            while (start < acc_.size() &&
                   !annexBStartLen(acc_.data(), acc_.size(), start)) {
                if (acc_[start] != 0)
                    return fail(Error::Malformed);
                ++start;
            }
            if (start == acc_.size()) {
                // Leading/trailing zero bytes may straddle an input fragment.
                if (final)
                    acc_.clear();
                else if (acc_.size() > 3)
                    acc_.erase(acc_.begin(), acc_.end() - 3);
                return true;
            }
            acc_.erase(acc_.begin(), acc_.begin() + static_cast<std::ptrdiff_t>(start));
            started_ = true;
            search_from_ = 0;
        }
        while (!acc_.empty()) {
            const size_t sc = annexBStartLen(acc_.data(), acc_.size());
            if (acc_.size() <= sc)
                return final ? fail(Error::Malformed) : true;
            size_t next = std::max(sc, search_from_);
            while (next + 2 < acc_.size()) {
                if (annexBStartLen(acc_.data(), acc_.size(), next))
                    break;
                ++next;
            }
            if (next + 2 >= acc_.size()) {
                if (final) {
                    if (!emit(acc_.size(), onNal))
                        return false;
                    acc_.clear();
                    started_ = false;
                    search_from_ = 0;
                    return true;
                }
                search_from_ = std::max(sc, acc_.size() - 3);
                return true;
            }
            if (next <= sc)
                return fail(Error::Malformed);
            if (!emit(next, onNal))
                return false;
            acc_.erase(acc_.begin(), acc_.begin() + static_cast<std::ptrdiff_t>(next));
            search_from_ = 0;
        }
        return true;
    }

    size_t max_acc_ = 0;
    std::vector<uint8_t> acc_;
    size_t search_from_ = 0;
    bool started_ = false;
    Error error_ = Error::None;
};

class NalDispatcher {
public:
    NalDispatcher(IBitstreamProducer& producer, DispatchConfig cfg = {})
        : producer_(producer), cfg_(std::move(cfg)) {}

    ControlResult begin(uint64_t session_id) {
        const auto r = producer_.begin(session_id);
        if (r != ControlResult::Ok)
            return r;
        session_id_ = session_id;
        seq_ = 0;
        active_ = true;
        paused_ = false;
        sps_delivered_ = false;
        pps_delivered_ = false;
        sps_.clear();
        pps_.clear();
        stats_ = {};
        return ControlResult::Ok;
    }

    ControlResult flushForSeek(uint64_t new_session_id) {
        if (active_) {
            const auto f = producer_.flush(session_id_);
            if (f != ControlResult::Ok)
                return f;
            const auto e = producer_.end(session_id_);
            if (e != ControlResult::Ok)
                return e;
            active_ = false;
        }
        return begin(new_session_id);
    }

    ControlResult end() {
        if (!active_)
            return ControlResult::NoSession;
        const auto r = producer_.end(session_id_);
        if (r == ControlResult::Ok)
            active_ = false;
        return r;
    }

    ControlResult pause() {
        if (!active_)
            return ControlResult::NoSession;
        const auto r = producer_.pause(session_id_);
        if (r == ControlResult::Ok)
            paused_ = true;
        return r;
    }

    ControlResult resume() {
        if (!active_)
            return ControlResult::NoSession;
        const auto r = producer_.resume(session_id_);
        if (r == ControlResult::Ok)
            paused_ = false;
        return r;
    }

    PushResult handleNal(const uint8_t* annexb, size_t len) {
        if (!active_ || !annexb || !len)
            return PushResult::Fatal;
        if (paused_)
            return PushResult::Full;
        const uint8_t type = annexBNalType(annexb, len);
        if (type == 0)
            return PushResult::Fatal;
        ++stats_.nal_seen;

        if (type == 7)
            sps_.assign(annexb, annexb + len);
        else if (type == 8)
            pps_.assign(annexb, annexb + len);

        if (type == 1 || type == 5) {
            const auto pr = replayParametersIfNeeded();
            if (pr != PushResult::Ok)
                return pr;
        }

        const auto r = pushWithBackpressure(annexb, len, type, false);
        if (r == PushResult::Ok) {
            if (type == 7)
                sps_delivered_ = true;
            else if (type == 8)
                pps_delivered_ = true;
        }
        return r;
    }

    const DispatchStats& stats() const { return stats_; }
    uint64_t sessionId() const { return session_id_; }
    uint32_t nextSeq() const { return seq_; }

private:
    PushResult replayParametersIfNeeded() {
        if (!sps_delivered_) {
            if (sps_.empty())
                return PushResult::Desync;
            const auto r = pushWithBackpressure(sps_.data(), sps_.size(), 7, true);
            if (r != PushResult::Ok)
                return r;
            sps_delivered_ = true;
            ++stats_.sps_replayed;
        }
        if (!pps_delivered_) {
            if (pps_.empty())
                return PushResult::Desync;
            const auto r = pushWithBackpressure(pps_.data(), pps_.size(), 8, true);
            if (r != PushResult::Ok)
                return r;
            pps_delivered_ = true;
            ++stats_.pps_replayed;
        }
        return PushResult::Ok;
    }

    PushResult pushWithBackpressure(const uint8_t* data, size_t len, uint8_t type, bool replay) {
        for (int attempt = 0; attempt <= cfg_.max_full_retries; ++attempt) {
            NalView nal{session_id_, seq_, type, data, len};
            const auto r = producer_.pushNal(nal);
            if (r == PushResult::Ok) {
                ++seq_;
                ++stats_.nal_pushed;
                stats_.bytes_pushed += len;
                (void)replay;
                return r;
            }
            if (r != PushResult::Full) {
                ++stats_.desync_or_fatal;
                return r;
            }
            ++stats_.full_retries;
            if (attempt == cfg_.max_full_retries)
                break;
            if (cfg_.sleep_ms && cfg_.full_retry_sleep_ms > 0)
                cfg_.sleep_ms(cfg_.full_retry_sleep_ms);
        }
        ++stats_.full_escalations;
        return PushResult::Full;
    }

    IBitstreamProducer& producer_;
    DispatchConfig cfg_;
    uint64_t session_id_ = 0;
    uint32_t seq_ = 0;
    bool active_ = false;
    bool paused_ = false;
    bool sps_delivered_ = false;
    bool pps_delivered_ = false;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    DispatchStats stats_;
};

} // namespace misterplex::h264stream
