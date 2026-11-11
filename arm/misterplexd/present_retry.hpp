// #342 (follow-up to #328): if initPresent() fails at start (core gate still closed; PlexOB
// answers CAPS later), the paint/idle/OSD/input workers never started. PresentRetry schedules
// bounded re-tries from the main loop tick: exponential backoff (base..cap), a hard attempt
// limit, no busy loop (due() is false until the deadline). First success calls
// startPresentWorkers(). No strings: no token can be logged. Closes #342.
#pragma once

#include <chrono>
#include <cstdint>

namespace misterplex {

class PresentRetry {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int64_t kBaseMs = 1000;
    static constexpr int64_t kCapMs = 30000;
    static constexpr int kMaxAttempts = 40;   // 1,2,4,8,16 s then 30 s: ~18 min in all

    PresentRetry(bool armed, Clock::time_point now) : armed_(armed), next_(now + ms(kBaseMs)) {}

    bool active() const { return armed_ && !done_ && attempts_ < kMaxAttempts; }
    bool due(Clock::time_point now) const { return active() && now >= next_; }
    int attempts() const { return attempts_; }
    bool succeeded() const { return done_; }
    bool stoodDown() const { return stoodDown_; }
    // #344 glass: a verified PlexOB core owns the display (OwenbRing session). The legacy
    // present path can never come up there and each failed try runs FpgaSpi::close(), which
    // tears down the DDR/mailbox/bitstream maps the OB session uses. Stop for good.
    void standDown() { stoodDown_ = true; armed_ = false; }
    static int64_t delayMsFor(int attempt) {   // wait before 1-based attempt `attempt`
        int64_t d = kBaseMs;
        for (int i = 1; i < attempt && d < kCapMs; ++i)
            d *= 2;
        return d < kCapMs ? d : kCapMs;
    }
    // One attempt's outcome. Returns ms until the next try, 0 when finished.
    int64_t record(bool ok, Clock::time_point now) {
        ++attempts_;
        if (ok) {
            done_ = true;
            return 0;
        }
        if (attempts_ >= kMaxAttempts)
            return 0;
        const int64_t d = delayMsFor(attempts_ + 1);
        next_ = now + ms(d);
        return d;
    }

private:
    static Clock::duration ms(int64_t v) { return std::chrono::milliseconds(v); }
    bool armed_;
    bool done_ = false;
    bool stoodDown_ = false;
    int attempts_ = 0;
    Clock::time_point next_;
};

}  // namespace misterplex
