#pragma once
// GCAST P2: airplay_fe's thread-safe core, free of UxPlay so the host tests build it alone.
// - EventQueue: libairplay's httpd thread only enqueues (never touches MediaPlayer). A new Play
//   drops queued Scrub/Rate/Stop of the old one (coalesce), and the queue is capped (sender caps).
// - PlaybackSnapshot: what GET /playback-info answers, fed by the arbiter's progress sink.
//   After an end (natural EOF, Plex preemption, local stop) the next poll answers duration -1
//   exactly once, which makes libairplay drop the sender session (RESET_TYPE_HLS_SHUTDOWN).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace misterplex {
namespace gcast {

enum class ApEventType : uint8_t { Play, Scrub, Rate, Stop };

struct ApEvent {
    ApEventType type = ApEventType::Stop;
    std::string url;
    double value = 0;   // Play: start seconds; Scrub: seconds; Rate: rate
};

class EventQueue {
public:
    static constexpr size_t kCap = 32;
    // false when dropped (cap reached for a non-Play/Stop event)
    bool push(ApEvent e) {
        std::lock_guard<std::mutex> lk(mu_);
#ifndef MUT_AP_NO_COALESCE
        if (e.type == ApEventType::Play)
            q_.clear();
#endif
        if (q_.size() >= kCap) {
            if (e.type != ApEventType::Play && e.type != ApEventType::Stop)
                return false;
            q_.pop_front();
        }
        q_.push_back(std::move(e));
        cv_.notify_one();
        return true;
    }
    bool pop(ApEvent& out, int ms) {   // false on timeout
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !q_.empty(); }))
            return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<ApEvent> q_;
};

struct PlaybackInfoOut {
    double duration = -1, position = -1, rate = 0;
    bool ready = false;
};

class PlaybackSnapshot {
public:
    enum class State : uint8_t { Idle, Loading, Playing, Paused, Ended };
    void loading(double startS) {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::Loading;
        pos_ = startS;
        dur_ = 0;
        endReported_ = false;
    }
    // MediaPlayer progress ("buffering"/"playing"/"paused"/"stopped"/"ended", ms)
    void progress(const std::string& st, int64_t tMs, int64_t dMs) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == State::Idle || state_ == State::Ended)
            return;
        if (dMs > 0)
            dur_ = dMs / 1000.0;
        if (tMs >= 0)
            pos_ = tMs / 1000.0;
        if (st == "playing")
            state_ = State::Playing;
        else if (st == "paused")
            state_ = State::Paused;
        else if (st == "stopped" || st == "ended")
            state_ = State::Ended;
    }
    void ended() {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != State::Idle)
            state_ = State::Ended;
    }
    void info(PlaybackInfoOut& o) {
        std::lock_guard<std::mutex> lk(mu_);
        o = PlaybackInfoOut{};
        if (state_ == State::Ended) {
#ifdef MUT_AP_END_FOREVER
            o.duration = -1;
            return;
#else
            if (!endReported_) {
                endReported_ = true;
                state_ = State::Idle;
                o.duration = -1;
                return;
            }
#endif
        }
        if (state_ == State::Idle) {
            o.duration = 0;   // position -1: libairplay answers "playback_info not available"
            return;
        }
        o.duration = dur_ > 0 ? dur_ : 0;
        o.position = pos_;
        o.rate = state_ == State::Playing ? 1.0 : 0.0;
        o.ready = state_ == State::Playing || state_ == State::Paused;
    }
    State state() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }

private:
    mutable std::mutex mu_;
    State state_ = State::Idle;
    double pos_ = 0, dur_ = 0;
    bool endReported_ = false;
};

}  // namespace gcast
}  // namespace misterplex
