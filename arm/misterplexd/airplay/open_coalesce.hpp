#pragma once
#include "airplay/ns.hpp"
// A repeat play of the SAME stream must not abort the open already in flight.
//
// 2026-11-18 lab, AirPlay FoxOne: /ladder.ts takes 10-14 s to its first byte on a live HLS source. The phone
// re-sends /play (and a POST /stop) every 2-5 s while it waits. Each castPlay bumped playGen, which aborted the
// pending open ("Immediate exit requested" -> "open: stopped"), and the AirPlay Stop event retired it. So no open
// lived long enough to see a byte. The Fox URL also changes on every retry (a signed token path segment and the
// hdnts/traceid query), so "same stream" is the identity below, not the exact URL.
//
// 2026-11-23 lab v1.0.2 glass: the sender keeps re-sending /play (start=0, new URL) every 2-5 s after the picture
// is up too. Each one restarted the stream from 0 (39 plays, 39 first pictures, black dips), so the guard also
// holds the play once it is a picture, for as long as it lives.
//
// OpenGuard decides, for one AirPlay owner:
//   coalescePlay  - a repeat Play of the stream whose open is in flight, or of the stream already playing (and
//                   nothing newer took the glass): drop it, keep the running open / play. A different stream is
//                   never coalesced: it preempts. Neither is a Play at another position (a real seek / restart).
//   markPlaying   - the open of this generation became a picture: nothing is in flight (a Stop runs now) but the
//                   play is held. A late "playing" of an older generation is ignored.
//   settle        - ended / stopped / failed: nothing is in flight and nothing is held.
//   deferStop     - a Stop that arrives while the open is in flight is held for a short grace. A Play of the
//                   same stream inside the grace (the phone's stop-then-play) cancels it; any other Play too (it
//                   supersedes the stop). When the grace ends the stop runs as before.
//   takeNewSignature - a coalesced Play under another exact URL: the helper the open streams through must get it
//                   (GET /refresh). 2026-11-24 lab v1.0.3 glass: the held play kept the first Fox token, which
//                   expired at 75 s (2 x 403, PLAYBACK ERROR).
//   heldRefresh   - the helper's /refresh answer. Only "ok" means the running ffmpeg reloads its playlist under the
//                   new URL. A VOD playlist (#EXT-X-ENDLIST) is read once by the device ffmpeg 8.1.2, and a
//                   cookie-gated source reads the origin directly: the held play is then reopened at the playhead
//                   (beginReopen) before the URL it reads expires.
// Mutants: MUT_OPEN_NO_COALESCE, MUT_OPEN_EXACT_ID, MUT_OPEN_COALESCE_ANY, MUT_OPEN_NO_EXPIRE,
// MUT_OPEN_STOP_NOW, MUT_OPEN_STOP_NO_CANCEL, MUT_OPEN_SETTLE_IGNORED, MUT_OPEN_IGNORE_START,
// MUT_OPEN_NO_PLAYING_HOLD, MUT_OPEN_MARK_PLAYING_NOOP, MUT_OPEN_MARK_RESURRECTS, MUT_OPEN_HOLD_IGNORED,
// MUT_OPEN_SETTLE_KEEPS_HOLD, MUT_OPEN_BEGIN_KEEPS_HOLD, MUT_OPEN_PLAYING_EXPIRE, MUT_OPEN_HOLD_ANY_ID,
// MUT_OPEN_HOLD_IGNORE_START, MUT_OPEN_NO_PLAYHEAD, MUT_OPEN_MARK_IGNORE_GEN, MUT_OPEN_REFRESH_ID_ONLY,
// MUT_OPEN_REFRESH_SAME_URL, MUT_OPEN_BEGIN_KEEPS_URL, MUT_OPEN_HELPER_ANY_GEN, MUT_OPEN_HOLD_NOT_OK,
// MUT_OPEN_REOPEN_IN_FLIGHT, MUT_OPEN_REOPEN_NO_GAP, MUT_OPEN_REOPEN_ANY_TTL, MUT_OPEN_REOPEN_TAKES_PLAYHEAD.

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

enum OpenCoalesce { kOpenNotCoalesced = 0, kOpenCoalescedInFlight, kOpenCoalescedPlaying };

inline constexpr int64_t kHeldReopenTtlS = 30;       // reopen a held play whose URL in use expires within this
inline constexpr int64_t kHeldReopenGapMs = 20000;   // and at most once per this

enum HeldRefresh { kHeldKeep = 0, kHeldReopen };

// The helper's /refresh body: "ok ..." (the running ffmpeg reloads under the new URL), "vod ttl=N" / "cookie ttl=N"
// (it cannot; the URL it reads expires in N s, or ttl=none: it carries no expiry), "none ttl=none" (the helper
// streams nothing of that stream through its own playlist, e.g. a direct non-HLS input). Anything else is a failure.
inline bool refreshAnswerOk(const std::string& answer) { return answer.rfind("ok", 0) == 0; }

inline const char* refreshAnswerKind(const std::string& answer) {
    if (refreshAnswerOk(answer))
        return "live";
    if (answer.rfind("vod", 0) == 0)
        return "vod";
    if (answer.rfind("cookie", 0) == 0)
        return "cookie";
    if (answer.rfind("none", 0) == 0)
        return "not a helper playlist";
    return "no helper answer";
}

// Seconds left on the URL the helper's ffmpeg reads: >= 0 known, -1 ttl=none, -2 not given.
inline int64_t refreshAnswerTtlS(const std::string& answer) {
    const size_t at = answer.find("ttl=");
    if (at == std::string::npos)
        return -2;
    if (answer.compare(at + 4, 4, "none") == 0)
        return -1;
    int64_t v = 0;
    size_t i = at + 4;
    if (i >= answer.size() || !std::isdigit(static_cast<unsigned char>(answer[i])))
        return -2;
    for (; i < answer.size() && std::isdigit(static_cast<unsigned char>(answer[i])) && v < 100000000; ++i)
        v = v * 10 + (answer[i] - '0');
    return v;
}

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
#ifndef MUT_OPEN_BEGIN_KEEPS_HOLD
        playing_ = false;
#endif
#ifndef MUT_OPEN_BEGIN_KEEPS_URL
        url_ = url;
        helper_.clear();
#endif
        lastReopenMs_ = kNoReopen;
    }

    // castPlay reopens the held play at playheadS under url (heldRefresh said so), generation gen. As beginOpen, but
    // the sender's own start stays the coalesce anchor: its next re-send (start=0) still joins this open.
    void beginReopen(const std::string& url, double playheadS, uint64_t gen, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_OPEN_REOPEN_TAKES_PLAYHEAD
        startS_ = playheadS;
#else
        (void)playheadS;
#endif
        id_ = streamIdentity(url);
        gen_ = gen;
        beganMs_ = nowMs;
        pending_ = true;
        playing_ = false;
        url_ = url;
        helper_.clear();
    }

    // The helper base the open of gen streams through (its /ladder.ts); empty when the play uses none.
    // castPlay(AirPlay) runs on AirplayFrontend::worker() only, so this has run before the next /play is processed.
    void noteHelper(uint64_t gen, const std::string& base) {
        std::lock_guard<std::mutex> lk(mu_);
#ifndef MUT_OPEN_HELPER_ANY_GEN
        if (gen != gen_)
            return;
#endif
        helper_ = base;
    }

    // url came with a coalesced Play. Returns the helper base to send it to (GET /refresh?src=), or empty when the
    // helper already has this exact URL or the play has no helper. A returned URL counts as sent.
    std::string takeNewSignature(const std::string& url) {
        std::lock_guard<std::mutex> lk(mu_);
#if defined(MUT_OPEN_REFRESH_ID_ONLY)
        const bool same = streamIdentity(url) == streamIdentity(url_);
#elif defined(MUT_OPEN_REFRESH_SAME_URL)
        const bool same = false;
#else
        const bool same = url == url_;
#endif
        if (same || helper_.empty())
            return {};
        url_ = url;
        return helper_;
    }

    // answer: the helper's /refresh body for a Play coalesced as held (empty on an HTTP failure). Reopen only once
    // the play is a picture (an open in flight is never aborted: lab FoxOne), when the URL in use expires within
    // kHeldReopenTtlS or the answer does not say, and at most once per kHeldReopenGapMs: the sender re-sends a new
    // URL every 2-5 s for the whole play (lab v1.0.2: a reopen each time was 39 black dips).
    HeldRefresh heldRefresh(const std::string& answer, OpenCoalesce held, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_OPEN_HOLD_NOT_OK
        (void)answer; (void)held; (void)nowMs;
        return kHeldKeep;
#else
        if (refreshAnswerOk(answer))
            return kHeldKeep;
#ifndef MUT_OPEN_REOPEN_IN_FLIGHT
        if (held != kOpenCoalescedPlaying)
            return kHeldKeep;
#else
        (void)held;
#endif
#ifndef MUT_OPEN_REOPEN_ANY_TTL
        const int64_t ttl = refreshAnswerTtlS(answer);
        if (ttl == -1 || ttl > kHeldReopenTtlS)
            return kHeldKeep;
#endif
#ifndef MUT_OPEN_REOPEN_NO_GAP
        if (lastReopenMs_ != kNoReopen && nowMs - lastReopenMs_ < kHeldReopenGapMs)
            return kHeldKeep;
#endif
        lastReopenMs_ = nowMs;
        return kHeldReopen;
#endif
    }

    // Non-zero when this Play is a repeat of the open in flight or of the play already on glass: drop it.
    // liveOwner: the arbiter still names this owner and the player still has the play (a failed open, or a newer
    // cast, makes it false and the play runs). playheadS: the player's position now (< 0 unknown); once playing, a
    // start near it is a refresh too. A start far from both the original start and the playhead is a real seek.
    OpenCoalesce coalescePlay(const std::string& url, double startS, bool liveOwner, uint64_t currentGen,
                              int64_t nowMs, double playheadS = -1.0) {
#ifdef MUT_OPEN_NO_COALESCE
        (void)url; (void)startS; (void)liveOwner; (void)currentGen; (void)nowMs; (void)playheadS;
        return kOpenNotCoalesced;
#else
        std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_OPEN_HOLD_IGNORED
        const bool held = false;
#else
        const bool held = playing_;
#endif
        if (!(pending_ || held) || !liveOwner || currentGen != gen_)
            return kOpenNotCoalesced;
#if !defined(MUT_OPEN_NO_EXPIRE) && !defined(MUT_OPEN_PLAYING_EXPIRE)
        if (!held && nowMs - beganMs_ > windowMs_)
            return kOpenNotCoalesced;
#elif defined(MUT_OPEN_PLAYING_EXPIRE)
        if (nowMs - beganMs_ > windowMs_)
            return kOpenNotCoalesced;
#endif
#if defined(MUT_OPEN_HOLD_ANY_ID)
        if (!held && streamIdentity(url) != id_)
            return kOpenNotCoalesced;
#elif !defined(MUT_OPEN_COALESCE_ANY)
        if (streamIdentity(url) != id_)
            return kOpenNotCoalesced;
#endif
        auto near = [](double a, double b) {
            return a - b <= kOpenCoalesceStartSlopS && b - a <= kOpenCoalesceStartSlopS;
        };
        bool refresh = near(startS, startS_);
#ifndef MUT_OPEN_NO_PLAYHEAD
        if (held && playheadS >= 0)
            refresh = refresh || near(startS, playheadS);
#endif
#ifdef MUT_OPEN_IGNORE_START
        if (!held)
            refresh = true;
#endif
#ifdef MUT_OPEN_HOLD_IGNORE_START
        if (held)
            refresh = true;
#endif
        if (!refresh)
            return kOpenNotCoalesced;
        ++coalesced_;
        return held ? kOpenCoalescedPlaying : kOpenCoalescedInFlight;
#endif
    }

    // The open became a picture: nothing is in flight any more (a Stop runs now), the play is held until settle.
    // currentGen: the generation the "playing" progress belongs to (captured before it was dispatched).
    void markPlaying(uint64_t currentGen) {
#ifndef MUT_OPEN_MARK_PLAYING_NOOP
        std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_OPEN_MARK_IGNORE_GEN
        (void)currentGen;
#else
        if (currentGen != gen_)
            return;   // an older open's "playing" must not hold a newer open
#endif
#if defined(MUT_OPEN_MARK_RESURRECTS)
        playing_ = true;
#elif !defined(MUT_OPEN_NO_PLAYING_HOLD)
        playing_ = playing_ || pending_;   // a late "playing" after a stop does not bring the hold back
#endif
        pending_ = false;
#else
        (void)currentGen;
#endif
    }

    // Nothing is in flight and nothing is held (ended, stopped, failed).
    void settle() {
#ifndef MUT_OPEN_SETTLE_IGNORED
        std::lock_guard<std::mutex> lk(mu_);
        pending_ = false;
#ifndef MUT_OPEN_SETTLE_KEEPS_HOLD
        playing_ = false;
#endif
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
    bool holding() const {
        std::lock_guard<std::mutex> lk(mu_);
        return playing_;
    }
    int64_t beganMs() const {
        std::lock_guard<std::mutex> lk(mu_);
        return beganMs_;
    }

private:
    static constexpr int64_t kNoReopen = INT64_MIN;
    mutable std::mutex mu_;
    int64_t windowMs_;
    std::string id_;
    std::string url_;      // the newest exact URL the helper has for this open
    std::string helper_;   // the open's helper base, empty until noteHelper
    double startS_ = 0.0;
    uint64_t gen_ = 0;
    int64_t beganMs_ = 0;
    int64_t lastReopenMs_ = kNoReopen;   // the last held-play reopen of this stream (beginReopen keeps it)
    bool pending_ = false;   // the open is in flight
    bool playing_ = false;   // the open became a picture and still plays
    uint64_t coalesced_ = 0;
    DeferredStop stop_;
};

}  // namespace airplay
}  // namespace misterplex
