#pragma once
#include "airplay/ns.hpp"
// A repeat play of the SAME stream must not abort the open already in flight.
//
// 2026-11-18  lab device, AirPlay FoxOne: /ladder.ts takes 10-14 s to its first byte on a live HLS source. The phone
// re-sends /play (and a POST /stop) every 2-5 s while it waits. Each castPlay bumped playGen, which aborted the
// pending open ("Immediate exit requested" -> "open: stopped"), and the AirPlay Stop event retired it. So no open
// lived long enough to see a byte. The Fox URL also changes on every retry (a signed token path segment and the
// hdnts/traceid query), so "same stream" is the identity below, not the exact URL.
//
// OpenGuard decides, for one AirPlay owner:
//   coalescePlay  - a repeat Play of the stream whose open is in flight (and nothing newer took the glass):
//                   drop it, keep the running open. A different stream is never coalesced: it preempts.
//   settle        - the open ended up as a picture / ended / stopped: nothing is in flight any more.
//   deferStop     - a Stop that arrives while the open is in flight is held for a short grace. A Play of the
//                   same stream inside the grace (the phone's stop-then-play) cancels it; any other Play too (it
//                   supersedes the stop). When the grace ends the stop runs as before.
// Mutants: MUT_OPEN_NO_COALESCE, MUT_OPEN_EXACT_ID, MUT_OPEN_COALESCE_ANY, MUT_OPEN_NO_EXPIRE,
// MUT_OPEN_STOP_NOW, MUT_OPEN_STOP_NO_CANCEL, MUT_OPEN_SETTLE_IGNORED, MUT_OPEN_IGNORE_START.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace misterplex {
namespace airplay {

inline constexpr int64_t kOpenCoalesceWindowMs = 25000;   // an open older than this is not "in flight" any more
inline constexpr int64_t kOpenStopGraceMs = 1500;         // a Stop during an open waits this long for the replay
inline constexpr double kOpenCoalesceStartSlopS = 5.0;    // a repeat play at another position is a real seek

namespace open_detail {
inline bool tokenSegment(const std::string& s) {
    if (s.size() < 40)
        return false;
    bool digit = false, upper = false;
    for (unsigned char c : s) {
        if (std::isdigit(c))
            digit = true;
        else if (std::isupper(c))
            upper = true;
        else if (!(std::islower(c) || c == '_' || c == '%' || c == '=' || c == '+' || c == '.' || c == '-'))
            return false;
    }
    return digit && upper;
}
inline bool volatileKey(std::string k) {
    for (auto& c : k)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const keys[] = {"hdnts", "traceid", "hmac", "exp", "expires", "acl", "token", "sig",
                                       "signature", "sid", "session", "sessionid", "wsid", "x-plex-token",
                                       "accesstoken", "auth", "policy", "key-pair-id", "nonce", "ts", "timestamp"};
    for (const char* v : keys)
        if (k == v)
            return true;
    return false;
}
inline std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace open_detail

// Same stream under another signature: scheme, host, the path without token segments, the query without the
// volatile signing keys (sorted). Everything else (channel, rendition, item id) stays. Mirrors
// airplay_xcode.py stream_identity (the helper keys its plan cache and orphaned ffmpeg by the same rule).
inline std::string streamIdentity(const std::string& url) {
#ifdef MUT_OPEN_EXACT_ID
    return url;
#else
    const bool http = url.rfind("http://", 0) == 0, https = url.rfind("https://", 0) == 0;
    if (!http && !https)
        return url;
    const size_t schemeEnd = url.find("://");
    std::string rest = url.substr(schemeEnd + 3);
    const size_t hash = rest.find('#');
    if (hash != std::string::npos)
        rest.resize(hash);
    std::string query;
    const size_t qm = rest.find('?');
    if (qm != std::string::npos) {
        query = rest.substr(qm + 1);
        rest.resize(qm);
    }
    std::string host = rest, path;
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    std::string outPath;
    {
        size_t i = 0;
        bool first = true;
        while (true) {
            const size_t j = path.find('/', i);
            const std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!open_detail::tokenSegment(seg)) {
                if (!first)
                    outPath += "/";
                outPath += seg;
                first = false;
            }
            if (j == std::string::npos)
                break;
            i = j + 1;
        }
    }
    std::vector<std::string> kept;
    size_t p = 0;
    while (p <= query.size() && !query.empty()) {
        const size_t amp = query.find('&', p);
        const std::string kv = query.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        const std::string key = kv.substr(0, kv.find('='));
        if (!kv.empty() && !open_detail::volatileKey(key))
            kept.push_back(kv);
        if (amp == std::string::npos)
            break;
        p = amp + 1;
    }
    std::sort(kept.begin(), kept.end());
    std::string out = open_detail::lower(url.substr(0, schemeEnd)) + "://" + open_detail::lower(host) + outPath + "?";
    for (size_t k = 0; k < kept.size(); ++k)
        out += (k ? "&" : "") + kept[k];
    return out;
#endif
}

// A Stop held for a grace, cancelled by the replay. One worker thread per schedule; cancel() wakes it.
class DeferredStop {
public:
    DeferredStop() : st_(std::make_shared<State>()) {}
    ~DeferredStop() { cancel(); }
    // Runs fn after graceMs unless cancel() ran first. Replaces a stop already waiting (the later one wins).
    void schedule(int64_t graceMs, std::function<void()> fn) {
        cancel();
        const uint64_t epoch = st_->epoch.load();
        std::shared_ptr<State> st = st_;
        st->pending.store(true);
        std::thread([st, epoch, graceMs, fn = std::move(fn)] {
            std::unique_lock<std::mutex> lk(st->mu);
            const bool cancelled = st->cv.wait_for(lk, std::chrono::milliseconds(graceMs),
                                                   [&] { return st->epoch.load() != epoch; });
            if (cancelled)
                return;
            st->pending.store(false);
            lk.unlock();
            fn();
        }).detach();
    }
    bool cancel() {
#ifdef MUT_OPEN_STOP_NO_CANCEL
        return false;
#else
        std::lock_guard<std::mutex> lk(st_->mu);
        const bool was = st_->pending.exchange(false);
        ++st_->epoch;
        st_->cv.notify_all();
        return was;
#endif
    }
    bool pending() const { return st_->pending.load(); }

private:
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<uint64_t> epoch{0};
        std::atomic<bool> pending{false};
    };
    std::shared_ptr<State> st_;
};

class OpenGuard {
public:
    explicit OpenGuard(int64_t windowMs = kOpenCoalesceWindowMs) : windowMs_(windowMs) {}

    static int64_t steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // A real castPlay began an open for url (after its generation bump).
    void beginOpen(const std::string& url, double startS, uint64_t gen, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mu_);
        id_ = streamIdentity(url);
        startS_ = startS;
        gen_ = gen;
        beganMs_ = nowMs;
        pending_ = true;
    }

    // True when this Play is a repeat of the open in flight: drop it. liveOwner: the arbiter still names this
    // owner and the player still has the play (a failed open, or a newer cast, makes it false and the play runs).
    bool coalescePlay(const std::string& url, double startS, bool liveOwner, uint64_t currentGen, int64_t nowMs) {
#ifdef MUT_OPEN_NO_COALESCE
        (void)url; (void)startS; (void)liveOwner; (void)currentGen; (void)nowMs;
        return false;
#else
        std::lock_guard<std::mutex> lk(mu_);
        if (!pending_ || !liveOwner || currentGen != gen_)
            return false;
#ifndef MUT_OPEN_NO_EXPIRE
        if (nowMs - beganMs_ > windowMs_)
            return false;
#endif
#ifndef MUT_OPEN_COALESCE_ANY
        if (streamIdentity(url) != id_)
            return false;
#endif
#ifndef MUT_OPEN_IGNORE_START
        if (startS - startS_ > kOpenCoalesceStartSlopS || startS_ - startS > kOpenCoalesceStartSlopS)
            return false;
#endif
        ++coalesced_;
        return true;
#endif
    }

    // The open is no longer in flight (picture shown, ended, stopped, failed).
    void settle() {
#ifndef MUT_OPEN_SETTLE_IGNORED
        std::lock_guard<std::mutex> lk(mu_);
        pending_ = false;
#endif
    }

    // True when a Stop right now should wait for the replay.
    bool shouldDeferStop(bool liveOwner, int64_t nowMs) {
#ifdef MUT_OPEN_STOP_NOW
        (void)liveOwner; (void)nowMs;
        return false;
#else
        std::lock_guard<std::mutex> lk(mu_);
        return pending_ && liveOwner && nowMs - beganMs_ <= windowMs_;
#endif
    }

    DeferredStop& stop() { return stop_; }
    uint64_t coalesced() const {
        std::lock_guard<std::mutex> lk(mu_);
        return coalesced_;
    }
    bool pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_;
    }
    int64_t beganMs() const {
        std::lock_guard<std::mutex> lk(mu_);
        return beganMs_;
    }

private:
    mutable std::mutex mu_;
    int64_t windowMs_;
    std::string id_;
    double startS_ = 0.0;
    uint64_t gen_ = 0;
    int64_t beganMs_ = 0;
    bool pending_ = false;
    uint64_t coalesced_ = 0;
    DeferredStop stop_;
};

}  // namespace airplay
}  // namespace misterplex
