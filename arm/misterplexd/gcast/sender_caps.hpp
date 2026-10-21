#pragma once
// GCAST P6 sender caps: what one LAN sender can make the receivers hold.
//  - DIAL: one connection is served for at most kDialConnMs wall clock (the accept loop is serial, so a
//    slow sender trickling bytes under the 3 s socket timeout would otherwise hold it for minutes).
//  - pairing codes: at most kMaxPairCodes queued, a repeat of a queued code is ignored, the oldest drops.
//  - commands: DIAL launches go through the same latest-play-wins, kMaxCmds-capped queue as Lounge.
#include <chrono>
#include <deque>
#include <string>

namespace misterplex {
namespace gcast {
namespace caps {

constexpr int kDialConnMs = 3000;
constexpr size_t kMaxPairCodes = 4;
constexpr size_t kMaxCmds = 32;

inline bool pushPairCode(std::deque<std::string>& q, const std::string& code) {
    if (code.empty() || code.size() > 64)
        return false;
    for (const auto& c : q)
        if (c == code)
            return false;
#ifndef MUT_CAPS_NO_PAIRCAP
    while (q.size() >= kMaxPairCodes)
        q.pop_front();
#endif
    q.push_back(code);
    return true;
}

class Deadline {
public:
    explicit Deadline(int ms) : end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)) {}
    bool expired() const {
#ifdef MUT_CAPS_NO_DEADLINE
        return false;
#else
        return std::chrono::steady_clock::now() >= end_;
#endif
    }
    // ms left, at least 1 while not expired (for SO_RCVTIMEO / poll)
    int leftMs() const {
        const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - std::chrono::steady_clock::now()).count();
        return d > 0 ? static_cast<int>(d) : 0;
    }

private:
    std::chrono::steady_clock::time_point end_;
};

}  // namespace caps
}  // namespace gcast
}  // namespace misterplex
