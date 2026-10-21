#pragma once
// GCAST P1: SessionArbiter -- one owner of the player at a time, last caster wins.
// Owners: Plex (companion/OSD library), AirPlay (airplay_fe), YouTube (dial_lounge_fe).
// claim() hands back a token; the previous owner's retire hook runs (outside the lock) so it
// can tell its sender the session ended (Plex: closedStop + timeline end; AirPlay: playback-info
// duration -1; YouTube: Lounge onStateChange stopped). Progress from MediaPlayer::setProgress is
// dispatched to the current owner's sink. With GAIRPLAY/GYT off nothing ever claims a non-Plex
// owner, so main.cpp keeps its Plex path byte-for-byte (gcastEnabled() == false skips the arbiter).
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace misterplex {
namespace gcast {

enum class Owner : uint8_t { None = 0, Plex = 1, AirPlay = 2, YouTube = 3 };
constexpr int kOwnerCount = 4;

inline const char* ownerName(Owner o) {
    switch (o) {
    case Owner::Plex: return "Plex";
    case Owner::AirPlay: return "AirPlay";
    case Owner::YouTube: return "YouTube";
    default: return "None";
    }
}

class SessionArbiter {
public:
    using Retire = std::function<void(Owner by)>;
    using Progress = std::function<void(const std::string& state, int64_t timeMs, int64_t durationMs)>;

    void setRetire(Owner o, Retire fn) {
        std::lock_guard<std::mutex> lk(mu_);
        retire_[idx(o)] = std::move(fn);
    }
    void setProgress(Owner o, Progress fn) {
        std::lock_guard<std::mutex> lk(mu_);
        progress_[idx(o)] = std::move(fn);
    }

    // New session for o. Retires the previous owner when it differs (a new session by the same
    // owner is that front end's own business: it already replaced its sender state).
    uint64_t claim(Owner o) {
        Retire fn;
        Owner prev;
        uint64_t tok;
        {
            std::lock_guard<std::mutex> lk(mu_);
            prev = owner_;
            owner_ = o;
            tok = ++token_;
#ifndef MUT_ARB_NO_RETIRE
            if (prev != o && prev != Owner::None)
                fn = retire_[idx(prev)];
#endif
        }
        if (fn)
            fn(o);
        return tok;
    }

    // Owner o ends its own session. Stale tokens (a later claim happened) are ignored.
    bool release(Owner o, uint64_t tok) {
        std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_ARB_RELEASE_STALE
        (void)tok;
        if (owner_ != o)
            return false;
#else
        if (owner_ != o || tok != token_)
            return false;
#endif
        owner_ = Owner::None;
        return true;
    }

    Owner owner() const {
        std::lock_guard<std::mutex> lk(mu_);
        return owner_;
    }
    uint64_t token() const {
        std::lock_guard<std::mutex> lk(mu_);
        return token_;
    }
    bool current(Owner o, uint64_t tok) const {
        std::lock_guard<std::mutex> lk(mu_);
        return owner_ == o && token_ == tok;
    }

    // True when a non-Plex owner took the progress; false means "Plex path" (caller's own code).
    bool dispatchProgress(const std::string& st, int64_t t, int64_t d) {
        Progress fn;
        {
            std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_ARB_PROGRESS_TO_PLEX
            return false;
#endif
            if (owner_ == Owner::Plex || owner_ == Owner::None)
                return false;
            fn = progress_[idx(owner_)];
        }
        if (fn)
            fn(st, t, d);
        return true;
    }

private:
    static int idx(Owner o) { return static_cast<int>(o) & 3; }
    mutable std::mutex mu_;
    Owner owner_ = Owner::None;
    uint64_t token_ = 0;
    Retire retire_[kOwnerCount];
    Progress progress_[kOwnerCount];
};

}  // namespace gcast
}  // namespace misterplex
