#pragma once
// Network-drop resume policy (design rev1, tipA-net-drop-resume). Pure: no I/O.
// NET_RESUME stays off until glass. The session calls this only when the flag is on.
//
// Shipped here: classifyFault, backoff (tries, wall clock, jitter, flap, clean-play
// reset), resumeTargetMs, stall tick, give-up text.
// Not in this slice: /identity probe, tier-2 re-resolve, timeStalled, libav reconnect*.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace misterplex::netresume {

enum class Fault : uint8_t { None, Network, Stall, EarlyEof, ServerGone, Fatal };

// AVERROR_EXIT = -MKTAG('E','X','I','T'). A stall abort uses this code.
constexpr int kStallAbortAv = -1414092869;
constexpr int64_t kStallShowMs = 1500;
constexpr int64_t kStallAbortMs = 8000;
constexpr int64_t kEarlyEofGapMs = 30000;
constexpr int64_t kFlapWindowMs = 600000;   // 10 min
constexpr int kFlapMax = 5;                 // the 6th fault in the window gives up
constexpr int64_t kCleanResetMs = 30000;
constexpr int kEarlyEofGiveUp = 2;

struct Policy {
    int maxTries = 8;
    int64_t maxMs = 120000;
    int64_t openMs = 10000;
    int64_t rewindMs = 1000;
};

inline bool isRecoverable(Fault f) {
    return f == Fault::Network || f == Fault::Stall || f == Fault::EarlyEof || f == Fault::ServerGone;
}

inline bool isNetErrno(int avErr) {
    const int e = avErr < 0 ? -avErr : avErr;
    if (e <= 0 || e > 10000)
        return false;
    switch (e) {
    case EIO:
    case ETIMEDOUT:
    case ECONNRESET:
    case ECONNREFUSED:
    case EHOSTUNREACH:
    case ENETUNREACH:
        return true;
    default:
        return false;
    }
}

// A 3-digit status as its own token (not a substring of a longer number).
inline bool hasHttpToken(const std::string& s, const char* code) {
    const size_t n = std::strlen(code);
    for (size_t p = s.find(code); p != std::string::npos; p = s.find(code, p + 1)) {
        const bool left = p == 0 || !std::isdigit(static_cast<unsigned char>(s[p - 1]));
        const bool right = p + n >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p + n]));
        if (left && right)
            return true;
    }
    return false;
}

inline int httpStatusFromText(const std::string& s) {
    if (hasHttpToken(s, "401"))
        return 401;
    if (hasHttpToken(s, "403"))
        return 403;
    if (hasHttpToken(s, "404"))
        return 404;
    for (int code = 500; code <= 599; ++code) {
        const std::string t = std::to_string(code);
        if (hasHttpToken(s, t.c_str()))
            return code;
    }
    return 0;
}

inline int errnoFromText(const std::string& s) {
    if (s.find("Immediate exit requested") != std::string::npos)
        return kStallAbortAv;
    if (s.find("Connection reset") != std::string::npos)
        return -ECONNRESET;
    if (s.find("timed out") != std::string::npos || s.find("Timed out") != std::string::npos)
        return -ETIMEDOUT;
    if (s.find("Connection refused") != std::string::npos)
        return -ECONNREFUSED;
    if (s.find("No route to host") != std::string::npos)
        return -EHOSTUNREACH;
    if (s.find("Network is unreachable") != std::string::npos)
        return -ENETUNREACH;
    if (s.find("Input/output error") != std::string::npos || s.find("I/O error") != std::string::npos)
        return -EIO;
    return 0;
}

// #231 Glass Run G: the TRON-542 reopen after a drop got libav "open: Connection refused"
// (AVERROR(ECONNREFUSED), a REJECT rule's TCP reset) and the play ended SourceError. A reopen
// that fails on transport (refused, reset, no route, net unreachable, timed out, or the range
// reader's "connect failed") is retried in the 20 s window. HTTP status, EIO, token and
// format errors are not.
inline bool reopenRetryable(const std::string& err) {
#ifdef MUT_REOPEN_REFUSED_FATAL
    if (err.find("refused") != std::string::npos)
        return false;
#endif
    switch (-errnoFromText(err)) {
    case ECONNREFUSED:
    case ECONNRESET:
    case EHOSTUNREACH:
    case ENETUNREACH:
    case ETIMEDOUT:
        return true;
    default:
        break;
    }
    return err.find("connect failed") != std::string::npos;
}

// eof: the read returned end, not an error. durationMs <= 0 means the length is unknown,
// so an EOF is the natural end. A 404 on a transcode is the killed session (recoverable);
// a 404 on a direct part is fatal. Anything else with a code we do not list is fatal
// (token, decode, format). No code and no eof is None: the caller did not pass a fault.
inline Fault classifyFault(int avErr, int httpStatus, bool isTranscode, bool eof, int64_t lastGoodMs,
                           int64_t durationMs) {
    Fault f = Fault::None;
    if (eof) {
        if (durationMs > 0 && lastGoodMs + kEarlyEofGapMs < durationMs)
            f = Fault::EarlyEof;
    } else if (httpStatus == 401 || httpStatus == 403) {
        f = Fault::Fatal;
    } else if (httpStatus == 404) {
        f = isTranscode ? Fault::ServerGone : Fault::Fatal;
    } else if (httpStatus >= 500 && httpStatus <= 599) {
        f = Fault::Network;
    } else if (avErr == kStallAbortAv) {
        f = Fault::Stall;
    } else if (isNetErrno(avErr)) {
        f = Fault::Network;
    } else if (avErr != 0 || httpStatus >= 400) {
        f = Fault::Fatal;
    }
#ifdef MUT_NET_RESUME_FATAL_OK
    if (f == Fault::Fatal)
        f = Fault::Network;   // mutant: a token or decode error is retried
#endif
    return f;
}

// Attempt index 0 reopens at once. Then 1s, 2s, 4s, 8s, and 15s thereafter.
inline int64_t delayBaseMs(int episodeIndex) {
    static constexpr int64_t k[] = {0, 1000, 2000, 4000, 8000, 15000};
    if (episodeIndex <= 0)
        return 0;
    if (episodeIndex >= 5)
        return 15000;
    return k[episodeIndex];
}

// unitThousand is 0..1000. 0 is -20%, 1000 is +20%, 500 is the base.
inline int64_t applyJitter(int64_t base, uint32_t unitThousand) {
    if (base <= 0)
        return 0;
    const int64_t span = base / 5;
    if (span <= 0)
        return base;
    const uint32_t u = unitThousand % 1001u;
    const int64_t delta = (static_cast<int64_t>(u) * (2 * span)) / 1000 - span;
    return base + delta;
}

// Installed on the demuxer only while NET_RESUME is on. armed is false while
// paused or while the ring still holds a video AU. onShow fires once per quiet
// stretch at showMs; the read is failed at abortMs.
struct StallWatch {
    bool enabled = false;
    int64_t showMs = kStallShowMs;
    int64_t abortMs = kStallAbortMs;
    std::function<int64_t()> nowMs;
    std::function<bool()> armed;
    std::function<void()> onShow;
    std::function<void()> onClear;
};

enum class StallTick : uint8_t { Wait, Show, Abort };

// armed: not paused, and the ring is below one video AU (empty). quietMs is time
// since the last byte. Under showMs the ring hides the blip. Show keeps waiting.
// Abort fails the read so it is classified as a stall.
inline StallTick stallTick(bool armed, int64_t quietMs, int64_t showMs = kStallShowMs,
                           int64_t abortMs = kStallAbortMs) {
    if (!armed || quietMs < showMs)
        return StallTick::Wait;
    if (quietMs >= abortMs)
        return StallTick::Abort;
    return StallTick::Show;
}

// Phase 0/1 (a seek in flight): the pending seek target. Phase 2: mediaMs().
// Cap at the last video AU pushed, then step back rewindMs. A negative cap means
// no AU has been pushed, so there is nothing to cap.
inline int64_t resumeTargetMs(int64_t mediaMs, uint32_t stamp, int64_t pendingSeekMs, int64_t lastPushedAuMs,
                              int64_t rewindMs) {
    const unsigned phase = stamp & 0xffu;
    int64_t base = mediaMs;
    if (phase < 2 && pendingSeekMs >= 0)
        base = pendingSeekMs;
    if (base < 0)
        base = 0;
    if (lastPushedAuMs >= 0 && base > lastPushedAuMs)
        base = lastPushedAuMs;
    if (rewindMs > 0)
        base -= rewindMs;
    return base < 0 ? 0 : base;
}

inline std::string clockHms(int64_t ms) {
    const int64_t s = ms < 0 ? 0 : ms / 1000;
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", static_cast<long long>(s / 3600),
                  static_cast<long long>(s / 60 % 60), static_cast<long long>(s % 60));
    return b;
}

inline std::string lostConnectionText(int64_t lastGoodMs) {
    return "LOST CONNECTION AT " + clockHms(lastGoodMs);
}

// Explicit on only. Missing, 0, "off", or anything else stays off.
inline bool enabledFromEnv(const char* v) {
    if (!v || !*v)
        return false;
    return std::strcmp(v, "1") == 0 || std::strcmp(v, "on") == 0 || std::strcmp(v, "true") == 0 ||
           std::strcmp(v, "yes") == 0;
}

// One play's retry budget. The schedule restarts each fault (reopen at once). The
// attempt budget carries across faults until 30s of clean play. Five successful
// recoveries in 10 minutes make the next fault a give-up.
class Backoff {
public:
    explicit Backoff(const Policy& p, uint32_t seed) : policy_(p), rng_(seed ? seed : 0xA5A5u) {}

    int maxTries() const { return policy_.maxTries; }
    int attempt() const { return lastAttempt_; }
    const Policy& policy() const { return policy_; }

    // False: the flap guard says this fault is the one that gives up.
    bool beginFault(int64_t nowMs) {
        cleanSince_ = -1;
        faultStart_ = nowMs;
        episodeTries_ = 0;
        if (flapLimitHit(nowMs))
            return false;
        return true;
    }

    // Delay before the next attempt. nullopt: tries or wall clock, give up.
    // The first call of an episode returns 0 (reopen at once).
    std::optional<int64_t> nextDelayMs(int64_t nowMs) {
        if (faultStart_ < 0)
            faultStart_ = nowMs;
#ifndef MUT_NET_RESUME_NO_GIVEUP
        if (nowMs - faultStart_ >= policy_.maxMs)
            return std::nullopt;
        if (budgetUsed_ >= policy_.maxTries)
            return std::nullopt;
#endif
        const int64_t base = delayBaseMs(episodeTries_);
        ++episodeTries_;
        ++budgetUsed_;
        lastAttempt_ = budgetUsed_;
        return applyJitter(base, nextUnit());
    }

    void noteRecovered(int64_t nowMs) {
        recoveries_.push_back(nowMs);
        faultStart_ = -1;
        episodeTries_ = 0;
        cleanSince_ = nowMs;
    }

    // After 30s of clean play the shared attempt budget is full again.
    void onCleanPlay(int64_t nowMs) {
        if (cleanSince_ < 0)
            cleanSince_ = nowMs;
        if (nowMs - cleanSince_ >= kCleanResetMs) {
            budgetUsed_ = 0;
            episodeTries_ = 0;
            faultStart_ = -1;
            earlyEof_ = 0;
            cleanSince_ = nowMs;
        }
    }

    bool flapLimitHit(int64_t nowMs) const {
        int n = 0;
        for (int64_t t : recoveries_) {
            if (nowMs >= t && nowMs - t <= kFlapWindowMs)
                ++n;
        }
        return n >= kFlapMax;
    }

    // True when this early EOF is the one that gives up ("Server ended the stream").
    bool noteEarlyEof() { return ++earlyEof_ >= kEarlyEofGiveUp; }
    void clearEarlyEof() { earlyEof_ = 0; }

    // A user seek during recovery starts the budget over. Flap history stays.
    void resetBudget() {
        budgetUsed_ = 0;
        episodeTries_ = 0;
        faultStart_ = -1;
        earlyEof_ = 0;
        lastAttempt_ = 0;
        cleanSince_ = -1;
    }

private:
    uint32_t nextUnit() {
        uint32_t x = rng_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng_ = x ? x : 0xA5A5u;
        return rng_ % 1001u;
    }

    Policy policy_{};
    uint32_t rng_ = 1;
    int budgetUsed_ = 0;
    int episodeTries_ = 0;
    int lastAttempt_ = 0;
    int earlyEof_ = 0;
    int64_t faultStart_ = -1;
    int64_t cleanSince_ = -1;
    std::vector<int64_t> recoveries_;
};

} // namespace misterplex::netresume
