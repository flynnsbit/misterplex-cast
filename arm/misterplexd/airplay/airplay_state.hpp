#pragma once
#include "airplay/ns.hpp"
// GCAST P2: airplay_fe's thread-safe core, free of UxPlay so the host tests build it alone.
// - EventQueue: libairplay's httpd thread only enqueues (never touches MediaPlayer). A new Play
//   drops queued Scrub/Rate/Stop of the old one (coalesce), and the queue is capped (sender caps).
// - PlaybackSnapshot: what GET /playback-info answers, fed by the arbiter's progress sink.
//   After an end (natural EOF, Plex preemption, local stop) the next poll answers duration -1
//   exactly once, which makes libairplay drop the sender session (RESET_TYPE_HLS_SHUTDOWN).
//   A re-sent /play that castPlay holds (the stream already on glass) undoes its loading(): the poll keeps
//   answering the play, not readyToPlay=0 at position 0. So does a held play castPlay reopens at its playhead
//   under the re-send's URL, until the reopen's first progress (playKeepsSnapshot).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace misterplex {
namespace airplay {

enum class ApEventType : uint8_t { Play, Scrub, Rate, Stop, Notice, MirrorStart, MirrorStop, AudioStart, AudioStop };

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
            if (e.type != ApEventType::Play && e.type != ApEventType::Stop &&
                e.type != ApEventType::Notice && e.type != ApEventType::MirrorStart &&
                e.type != ApEventType::MirrorStop && e.type != ApEventType::AudioStart &&
                e.type != ApEventType::AudioStop)
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
#ifndef MUT_AP_LOADING_NO_SAVE
#ifndef MUT_AP_LOADING_RESAVE
        if (state_ != State::Loading)   // a second /play before the first one's undo keeps the play it replaced
#endif
            saved_ = Saved{state_, pos_, dur_, endReported_};
#endif
        state_ = State::Loading;
        pos_ = startS;
        dur_ = 0;
        endReported_ = false;
    }
    // The /play behind the last loading() was held (the play on glass goes on): answer that play again. A
    // progress or end that landed since is newer and stays.
    void undoLoading() {
#ifndef MUT_AP_UNDO_NOOP
        std::lock_guard<std::mutex> lk(mu_);
#ifndef MUT_AP_UNDO_ANY_STATE
        if (state_ != State::Loading)
            return;
#endif
        state_ = saved_.state;
        pos_ = saved_.pos;
        dur_ = saved_.dur;
        endReported_ = saved_.endReported;
#endif
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
    struct Saved {
        State state = State::Idle;
        double pos = 0, dur = 0;
        bool endReported = false;
    };
    mutable std::mutex mu_;
    State state_ = State::Idle;
    double pos_ = 0, dur_ = 0;
    bool endReported_ = false;
    Saved saved_;   // what loading() replaced
};

// castPlay's /play goes on as the play on glass: it held a re-send while playing, or reopened that held play at its
// playhead. Then the Play event undoes the /play's loading() (a reopen otherwise reads position 0, ready 0).
inline bool playKeepsSnapshot(bool heldPlaying, bool reopenedAtPlayhead) {
#ifdef MUT_AP_REOPEN_NO_UNDO
    reopenedAtPlayhead = false;
#endif
    return heldPlaying || reopenedAtPlayhead;
}

}  // namespace airplay
}  // namespace misterplex
