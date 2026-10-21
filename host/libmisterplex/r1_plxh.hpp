#pragma once
// R1 PLXH host side, the whole of it (review 19:01 A, 19:16 (1)(c), 19:27 F5; kit K2):
//
// 1. Write order (19:01 A). ddr_frame_store.sv:230-234 at r1-idlefb-mvpb 3a579e17 arms on the
//    magic word (DOUT[31:0] == 'PLXH') and takes token[63:48], buf_sel[33] and en[32] from the
//    same 64-bit poll beat. So the daemon stores the upper word {token, buf_sel, en} as ONE 32-bit
//    store at PLXH+4, then a barrier (dmb), then the constant magic at PLXH+0 LAST. Tokens are
//    16-bit and never 0; 0xFFFF wraps to 1. K2: PLXO is zeroed by one 64-bit store before the
//    first arm. PlxhLink owns the one token sequence; every PLXH writer goes through it.
//    Seed and prime (wW 20:05; review 20:23 RULING C). The RTL accepts only a token != the last
//    accepted one, and its first good-magic poll after a reset only snaps (accepts nothing), so a
//    cold slot would swallow the first request. A fresh link's tokens follow the PLXH slot's
//    (else PLXO's) and avoid both. The first request after start and after every core reload
//    (PlxPort::coreChanges) goes out twice with the SAME control word: T, a barrier, a >= 1 ms
//    settle, then T+1. Only T+1 is returned, so only its ACK counts; one log line per prime.
// 2. Session policy (19:16 (1)(c)). From the start of an FPGA session (before BEGIN) to its END,
//    no en=1 is written. On an R1 core, en=0 is written after the BEGIN ack and ACKed before
//    the first AU: PLXO token_latched == token with idle_shown 0. A mid-session
//    generation_clear then never selects the idle FB, so the FB geometry and ARX stay put.
//    requestIdle is the only en=1 path, and it is refused inside a session.
//    No-FB_VBL fallback (review 20:08 (a)-(c), N1). The RTL accepts PLXH off its free-running poll,
//    but registers it and writes PLXO only around an FB_VBL. So if PLXO stays bit-identical (to
//    its value right after the en=0 request) over the whole wait and at a re-read 50 ms later,
//    there was no FB_VBL: the session proceeds. The en=0 request stays armed in the slot (last
//    write wins) and latches at the first FB_VBL; inSession still blocks every en=1; the daemon
//    logs it once, and at END logs whether the en=0 ACK came late or never. A PLXO change that
//    is not the en=0 ACK, or an unreadable PLXO, still fails the session. An ACK at the re-read
//    is an ACK.
// 3. F5 pacing (19:27, lane9 Proposal A). On the /dev/mem path the next paint starts only after
//    TWO same-buffer no-op latches following paint n's ACK. A no-op is PLXH {buf_sel = the shown
//    buffer, en=1, fresh token}; it is ACKed by PLXO token_latched == token with idle_shown 1 and
//    idle_buf_shown unchanged. Each latch registers at an FB_VBL rise and PLXO is written
//    change-only, so 2 ACKs prove at least 2 display frames. Each no-op is issued only after the
//    previous ACK (last-write-wins, never queued). A latch not ACKed within 100 ms (no FB_VBL:
//    HDMI down, the core gone) ends the latch wait: the next paint then waits >= 50 ms of wall
//    time, logged once per streak. Never unbounded. Session edges reset the pacer (the en=0
//    write supersedes any unlatched no-op). The WC path is never paced.
#include "libmisterplex/r1_idle_fb.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace misterplex {
namespace r1 {

// The PLX page as the daemon maps it (0x300FF000 on the 480p layout). Tests fake it.
// store64 is ONLY for the K2 PLXO zero (review 20:00 N2). An STRD may land as two beats in
// either order, so a PLXH request must never go through it: the magic could arrive first. Every
// PLXH store is a store32 (upper word, then barrier, then magic); the concrete FpgaSpi port
// refuses a 64-bit store anywhere but PLXO.
class PlxPort {
public:
    virtual ~PlxPort() = default;
    virtual bool store32(uint32_t phys, uint32_t value) = 0;
    virtual bool store64(uint32_t phys, uint64_t value) = 0;
    virtual bool load64(uint32_t phys, uint64_t* value) = 0;
    virtual void barrier() = 0;   // dmb (__sync_synchronize on the ARM build)
    // Moves on every core reload the daemon sees (CoreGate::changes()): the link re-primes then.
    virtual uint64_t coreChanges() { return 0; }
    // The prime's settle between T and T+1: >= 1 ms (the phase-0 poll is ~2.9 us, plus the CDC).
    virtual void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
};

class PlxhLink {
public:
    // log: the daemon's log, for the one line per prime (review 20:23 P5).
    explicit PlxhLink(PlxPort& port, uint16_t lastToken = 0,
                      std::function<void(const std::string&)> log = {})
        : port_(port), token_(lastToken), log_(std::move(log)) {}

    // One PLXH request; returns its token, or 0 when the port refused a store. The first request
    // after start and after every core reload is primed (review 20:23 RULING C): it goes out twice,
    // as T and then T+1, and the token returned (the only one whose ACK counts) is T+1.
    uint16_t write(int bufSel, bool en) {
        std::string line;
        uint16_t token = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            token = writeLocked(bufSel, en, &line);
        }
        emit(line);
        return token;
    }

    Plxo plxo() {
        uint64_t word = 0;
        return plxoRaw(&word) ? decodePlxo(word) : Plxo{};
    }

    // The raw PLXO word (the no-FB_VBL proof compares bits); false when unreadable.
    bool plxoRaw(uint64_t* word) {
        if (!port_.load64(kPlxoPhys, word))
            return false;
        std::string line;
        {
            std::lock_guard<std::mutex> lock(mu_);
            observePrime(*word, &line);
        }
        emit(line);
        return true;
    }

    // en=0 ACK: `token` latched with the idle FB not shown.
    bool ackedOff(uint16_t token) {
        const Plxo o = plxo();
        return o.valid && o.tokenLatched == token && !o.idleShown;
    }

    // en=1 ACK: `token` latched with idle buffer `buf` shown (a paint flip or an F5 no-op).
    bool ackedIdle(uint16_t token, int buf) {
        const Plxo o = plxo();
        return o.valid && o.tokenLatched == token && o.idleShown && o.idleBufShown == buf;
    }

    uint16_t lastToken() const {
        std::lock_guard<std::mutex> lock(mu_);
        return token_;
    }

private:
    struct Prime {
        bool active = false;
        const char* reason = "";
        uint16_t t = 0, t1 = 0;
        const char* slot = "";   // cold (no magic: T is the RTL's post-reset snap), warm, unread
        bool sawT = false;       // a PLXO read showed T before T+1
    };

    uint16_t writeLocked(int bufSel, bool en, std::string* line) {
        const uint64_t changes = port_.coreChanges();
        if (!plxoZeroed_) {   // K2: the only 64-bit store (never a PLXH request, see PlxPort)
            if (!seedFromSlots())
                return 0;
            if (!port_.store64(kPlxoPhys, 0))
                return 0;
            port_.barrier();
            plxoZeroed_ = true;
            primeReason_ = "start";
            changesSeen_ = changes;
        } else if (changes != changesSeen_) {
            primeReason_ = "reload";
            changesSeen_ = changes;
        }
        if (prime_.active) {   // P5: the prime is still unresolved at the next request
            *line = primeLine("superseded before T+1 ACKed");
            prime_.active = false;
        }
#ifndef MUT_PLXH_NO_PRIME   // test-only mutant: no prime (a cold slot snaps the first request)
        if (primeReason_) {
            const uint16_t t1 = primeLocked(bufSel, en);
            if (t1)
                primeReason_ = nullptr;
            return t1;
        }
#endif
        return storeRequest(nextToken(token_), bufSel, en);
    }

    // RULING C: T with the SAME control word as the request (P2), a barrier, the settle, then T+1
    // (P1). Cold slot: the RTL snaps T and accepts T+1. Warm slot: it accepts T (the intended
    // content) and T+1 is a same-content latch whose tok_r change ACKs. Callers get T+1 only (P3).
    // T and T+1 both follow the seed rule against the slot and PLXO as they are now (P4).
    uint16_t primeLocked(int bufSel, bool en) {
        uint64_t slot = 0, out = 0;
        if (!port_.load64(kPlxoPhys, &out))
            return 0;
        const bool slotRead = port_.load64(kPlxhPhys, &slot);
        const bool slotArmed = slotRead && static_cast<uint32_t>(slot) == kPlxhMagic;
        const Plxo o = decodePlxo(out);
        avoidTokens(slotArmed ? static_cast<uint16_t>(slot >> 48) : 0, o.valid ? o.tokenLatched : 0);
        const uint16_t t = nextToken(token_);
#ifdef MUT_PLXH_PRIME_EN0   // test-only mutant: a throwaway en=0 prime (blanks a warm idle FB)
        if (!storeRequest(t, 0, false))
            return 0;
#else
        if (!storeRequest(t, bufSel, en))
            return 0;
#endif
        port_.barrier();   // T is out before the settle, so T+1 cannot pass it
        port_.settle();
        const uint16_t t1 = storeRequest(nextToken(t), bufSel, en);
        if (!t1)
            return 0;
        prime_ = {true, primeReason_, t, t1, !slotRead ? "unread" : slotArmed ? "warm" : "cold", false};
        return t1;
    }

    // One request: the upper word {token, buf_sel, en}, a barrier, then the magic LAST (19:01 A).
    uint16_t storeRequest(uint16_t token, int bufSel, bool en) {
        const uint64_t word = encodePlxh(token, bufSel, en);
        const uint32_t upper = static_cast<uint32_t>(word >> 32);
        const uint32_t magic = static_cast<uint32_t>(word);
#ifdef MUT_PLXH_MAGIC_FIRST   // test-only mutant: the magic beat before the upper word
        if (!port_.store32(kPlxhPhys, magic))
            return 0;
        port_.barrier();
        if (!port_.store32(kPlxhPhys + 4, upper))
            return 0;
#else
        if (!port_.store32(kPlxhPhys + 4, upper))
            return 0;
        port_.barrier();
        if (!port_.store32(kPlxhPhys, magic))
            return 0;
#endif
        token_ = token;
        return token;
    }

    // The next two tokens differ from a and b (the slot's and PLXO's; 0 = absent, and tokens are
    // never 0).
    void avoidTokens(uint16_t a, uint16_t b) {
#ifndef MUT_PLXH_NO_SEED   // test-only mutant: a fresh link starts at the given token (wW R-b)
        const auto bad = [a, b](uint16_t t) { return t == a || t == b; };
        while (bad(nextToken(token_)) || bad(nextToken(nextToken(token_))))
            token_ = nextToken(token_);
#else
        (void)a;
        (void)b;
#endif
    }

    // A fresh link (a daemon restart, or after a core reload) must not reuse the token the RTL
    // last accepted or snapped (the PLXH slot: accept needs token != the last accepted one, or the
    // post-reset snap) nor the one it last published (PLXO is change-only on {idle_sel,
    // idle_sel ? idle_buf : 0, tok_r}, plex_fb480.sv:340-346, and K2 is about to zero the DDR
    // word). So the tokens follow the slot's (else PLXO's) and differ from both (wW, R1 entry-clear
    // row R-b). With neither valid the RTL has accepted nothing since its reset (accept needs the
    // magic in the slot; reset clears tok_r), and the sequence starts as given. An unreadable PLXH
    // slot counts as absent (PLXO alone seeds); an unreadable PLXO refuses.
    bool seedFromSlots() {
        uint64_t slot = 0, out = 0;
        if (!port_.load64(kPlxoPhys, &out))
            return false;
        const bool slotArmed =
            port_.load64(kPlxhPhys, &slot) && static_cast<uint32_t>(slot) == kPlxhMagic;
#ifndef MUT_PLXH_NO_SEED
        const uint16_t slotToken = slotArmed ? static_cast<uint16_t>(slot >> 48) : 0;
        const Plxo o = decodePlxo(out);
        const uint16_t outToken = o.valid ? o.tokenLatched : 0;
        if (slotArmed)
            token_ = slotToken;
        else if (o.valid)
            token_ = outToken;
        avoidTokens(slotToken, outToken);
#else
        (void)slotArmed;
#endif
        return true;
    }

    // P5: one line per prime, when PLXO shows T+1 (or at the next request, unresolved).
    void observePrime(uint64_t word, std::string* line) {
        if (!prime_.active)
            return;
        const Plxo o = decodePlxo(word);
        if (!o.valid)
            return;
        if (o.tokenLatched == prime_.t) {
            prime_.sawT = true;
        } else if (o.tokenLatched == prime_.t1) {
            *line = primeLine("T+1 ACKed");
            prime_.active = false;
        }
    }

    std::string primeLine(const char* outcome) const {
        return std::string("r1: PLXH prime (") + prime_.reason + "): T=" + std::to_string(prime_.t) +
               " T+1=" + std::to_string(prime_.t1) + ", slot " + prime_.slot + ", " + outcome +
               (prime_.sawT ? "; PLXO showed T first" : "; PLXO never showed T");
    }

    void emit(const std::string& line) {
        if (!line.empty() && log_)
            log_(line);
    }

    PlxPort& port_;
    mutable std::mutex mu_;
    uint16_t token_ = 0;
    bool plxoZeroed_ = false;
    const char* primeReason_ = nullptr;   // "start" or "reload" while a prime is owed
    uint64_t changesSeen_ = 0;
    Prime prime_;
    const std::function<void(const std::string&)> log_;
};

class PlxhF5Pacer;

class PlxhSessionPolicy {
public:
    // tick: one wait step while the en=0 ACK is pending (1 ms sleep by default; tests advance a VBL).
    explicit PlxhSessionPolicy(PlxhLink& link, std::function<void()> tick = {})
        : link_(link), tick_(std::move(tick)) {
        if (!tick_)
            tick_ = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
    }

    void attachPacer(PlxhF5Pacer* pacer) { pacer_ = pacer; }

    // Session start (before BEGIN, on any core): no en=1 from here to endSession(). `gen` is the
    // session's play generation (0: none, e.g. the lab --play-file); the session takes over the
    // PLAY-request hold of that generation or an older one, never a newer request's.
    inline void enterSession(uint64_t gen = 0);

    // After the BEGIN ack on an R1 core, before the first AU: PLXH en=0, ACKed by PLXO within
    // `ticks` waits, or (review 20:08) PLXO bit-identical over them and at a re-read after
    // `recheckTicks` more: no FB_VBL, so proceed with en0Pending(). On true, *why is empty after
    // a prompt ACK, or says how the wait ended (ACK at the re-read, or the no-FB_VBL proof); on
    // false it says what PLXO showed.
    inline bool beginSession(int ticks, std::string* why, int recheckTicks = 50);

    // Ends the session (en=1 allowed again). When the session proceeded without the en=0 ACK,
    // logs and returns the END line (review 20:08 (c)): ACKed late, or never ACKed. Else empty.
    inline std::string endSession();

    // Where endSession's END line goes (the daemon's log).
    void setLog(std::function<void(const std::string&)> log) {
        std::lock_guard<std::mutex> lock(mu_);
        log_ = std::move(log);
    }

    bool inSession() const { return inSession_.load(); }
    uint16_t sessionToken() const { return sessionToken_.load(); }
    // The session proceeded on the no-FB_VBL proof; its en=0 is armed but not yet ACKed.
    bool en0Pending() const { return en0Pending_.load(); }

    // review 06:57 D3: the en=1 gate closes at the PLAY request, not at BEGIN (the session
    // enters only after the resolve). review 07:30 C1: the hold belongs to one request, keyed by
    // its play generation, so a stale request's end can never release a newer request's hold. It
    // ends when that request's session enters (enterSession(gen), atomically with inSession), when
    // that request ends without a session (playRequestEnded(gen)), or when a later STOP ends it
    // (playRequestsEndedBefore). review 08:17 (2): while that request's doPlay runs it OWNS the
    // hold (PlayRequestHold) and the hold does not expire; doPlay's exit (return or throw) releases
    // or disowns it. A hold nobody owns (begun, dispatch pending; or left by a doPlay that a seek's
    // replay replaced) expires kPlayRequestHoldMs after it lost its owner. kPlayRequestOwnedMaxMs only
    // guards a wedged doPlay: a resolve is curl-bounded per step (connect 6 s, max-time 12-20 s,
    // about 6 steps per server, the plex.tv lookup 15 s) and this bound is above that.
    static constexpr int64_t kPlayRequestHoldMs = 30000;
    static constexpr int64_t kPlayRequestOwnedMaxMs = 300000;
    // Every play-generation bump that is a PLAY request goes through here: bump() runs under the
    // hold lock, so no release can land between the new generation and its hold. bump() returns
    // the new generation, or 0 for no request (a lost compare-exchange), and then holds nothing.
    uint64_t beginPlayRequest(const std::function<uint64_t()>& bump) {
        std::lock_guard<std::mutex> hold(holdMu_);
        const uint64_t gen = bump();
        if (gen)
            holdLocked(gen, false);
        return gen;
    }
    // doPlay's start (PlayRequestHold): (re)arms and owns the hold for its generation (a seek's
    // replay, a bump that was not a begin) unless a newer request was begun or noted, or a later
    // session or STOP came, since. True when this doPlay now owns the hold.
    bool notePlayRequest(uint64_t gen) {
        std::lock_guard<std::mutex> hold(holdMu_);
        if (!gen || gen < newestGen_)
            return false;
        holdLocked(gen, true);
        return true;
    }
    // Request `gen` ended without a session (refused, failed, stopped): compare-and-clear, so only
    // its own hold goes.
    void playRequestEnded(uint64_t gen) {
        std::lock_guard<std::mutex> hold(holdMu_);
        releaseLocked(holdGen_ == gen);
    }
    // doPlay `gen` exited and left the hold to its session's enter or to a seek's replay: it no
    // longer owns it, so the hold now expires kPlayRequestHoldMs from here unless taken over.
    void playRequestDone(uint64_t gen) {
        std::lock_guard<std::mutex> hold(holdMu_);
        if (holdGen_ == gen && holdOwned_) {
            holdOwned_ = false;
            holdMs_ = std::max<int64_t>(1, nowMs());
        }
    }
    // A STOP at play generation `gen`: every request before it is over; a later one keeps its hold.
    void playRequestsEndedBefore(uint64_t gen) {
        std::lock_guard<std::mutex> hold(holdMu_);
        releaseLocked(holdGen_ != 0 && holdGen_ < gen);
        newestGen_ = std::max(newestGen_, gen);
    }
    bool playRequestPending() const {
        std::lock_guard<std::mutex> hold(holdMu_);
        return pendingLocked();
    }
    // The pending hold's play generation (0: none).
    uint64_t playRequestGeneration() const {
        std::lock_guard<std::mutex> hold(holdMu_);
        return pendingLocked() ? holdGen_ : 0;
    }
    // Tests drive the hold's clock (ms, monotonic).
    void setClockMs(std::function<int64_t()> nowMs) { nowMs_ = std::move(nowMs); }

    // The only en=1 path (paint flips and F5 no-ops): refused from the PLAY request to the END. The
    // hold lock spans the check and the store, so a PLAY request begun meanwhile comes after it.
    bool requestIdle(int bufSel, uint16_t* token, std::string* why) {
        std::lock_guard<std::mutex> lock(mu_);
#ifndef MUT_PLXH_EN1_IN_SESSION
        if (inSession_.load())
            return fail(why, "PLXH en=1 refused inside a session (review 19:16 (1)(c))");
#endif
        std::lock_guard<std::mutex> hold(holdMu_);
        if (pendingLocked())
            return fail(why, "PLXH en=1 refused: a PLAY request is pending, its session not yet entered "
                             "(review 06:57 D3)");
        const uint16_t t = link_.write(bufSel, true);
        if (!t)
            return fail(why, "PLXH store refused");
        if (token)
            *token = t;
        return true;
    }

private:
    static bool fail(std::string* why, const std::string& text) {
        if (why)
            *why = text;
        return false;
    }
    int64_t nowMs() const {
        if (nowMs_)
            return nowMs_();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    // The PLAY-request hold (holdMu_ held).
    void holdLocked(uint64_t gen, bool owned) {
#ifndef MUT_R1_GATE_AT_BEGIN   // test-only mutant: the gate closes only at the session's BEGIN
        holdGen_ = gen;
        holdMs_ = std::max<int64_t>(1, nowMs());
        holdOwned_ = owned;
#else
        (void)owned;
#endif
        newestGen_ = std::max(newestGen_, gen);
    }
    void releaseLocked(bool own) {
#ifdef MUT_R1_HOLD_RELEASE_ANY   // test-only mutant: any end releases the hold (the pre-C1 rule)
        own = true;
#endif
        if (own) {
            holdGen_ = 0;
            holdMs_ = 0;
            holdOwned_ = false;
        }
    }
    bool pendingLocked() const {
        if (holdGen_ == 0)
            return false;
#ifndef MUT_R1_HOLD_EXPIRES_OWNED   // test-only mutant: an owned hold expires like an orphan
        if (holdOwned_)
            return nowMs() - holdMs_ < kPlayRequestOwnedMaxMs;
#endif
        return nowMs() - holdMs_ < kPlayRequestHoldMs;
    }

    PlxhLink& link_;
    std::function<void()> tick_;
    std::mutex mu_;
    std::atomic<bool> inSession_{false};
    std::atomic<uint16_t> sessionToken_{0};
    std::atomic<bool> en0Pending_{false};
    mutable std::mutex holdMu_;   // the PLAY-request hold below; a leaf lock (mu_ -> holdMu_)
    uint64_t holdGen_ = 0;        // the holding request's play generation (0: none)
    int64_t holdMs_ = 0;          // owned: since the owner noted it; else since it lost its owner
    bool holdOwned_ = false;      // a doPlay of holdGen_ is running (PlayRequestHold)
    uint64_t newestGen_ = 0;      // the newest generation begun, noted, entered or stopped
    std::function<int64_t()> nowMs_;
    std::function<void(const std::string&)> log_;
    PlxhF5Pacer* pacer_ = nullptr;
};

// doPlay's PLAY-request guard (review 06:57 D3, 07:30 C1, 08:17 (2)). From doPlay's start it owns
// its generation's hold (the hold does not expire while it runs). On every exit, return or throw:
// a play that started no session and was not replaced by a seek's replay (`releasable`: still the
// current request, or stopped) releases its own generation's hold; otherwise the hold is only
// disowned and the session's enter or the replay takes it over (an orphan expires after
// kPlayRequestHoldMs).
class PlayRequestHold {
public:
    PlayRequestHold(PlxhSessionPolicy& policy, uint64_t gen, std::function<bool()> releasable)
        : policy_(policy), gen_(gen), releasable_(std::move(releasable)) {
        policy_.notePlayRequest(gen_);
    }
    ~PlayRequestHold() {
        if (!started_ && releasable_ && releasable_())
            policy_.playRequestEnded(gen_);
        else
            policy_.playRequestDone(gen_);
    }
    // player.play() started this generation's session: its enter takes the hold over.
    void started() { started_ = true; }
    PlayRequestHold(const PlayRequestHold&) = delete;
    PlayRequestHold& operator=(const PlayRequestHold&) = delete;

private:
    PlxhSessionPolicy& policy_;
    uint64_t gen_;
    std::function<bool()> releasable_;
    bool started_ = false;
};

class PlxhF5Pacer {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kLatchTimeout{100};
    static constexpr std::chrono::milliseconds kTimePacing{50};
#ifdef MUT_F5_PAINT_AFTER_1_LATCH   // test-only mutant: the next paint after one latch
    static constexpr int kLatches = 1;
#else
    static constexpr int kLatches = 2;
#endif

    PlxhF5Pacer(PlxhSessionPolicy& policy, PlxhLink& link, std::function<Clock::time_point()> now = {},
                std::function<void(const std::string&)> log = {})
        : policy_(policy), link_(link), now_(std::move(now)), log_(std::move(log)) {
        if (!now_)
            now_ = [] { return Clock::now(); };
        policy_.attachPacer(this);
    }

    // Once per paint ACK, with the buffer that ACK made shown. Only the /dev/mem path is paced.
    void paintAcked(int shownBuf, bool devMem) {
        std::lock_guard<std::mutex> lock(mu_);
        syncReset();
        if (!devMem || (shownBuf != 0 && shownBuf != 1))
            return;
        state_ = State::Latching;
        shownBuf_ = shownBuf;
        acked_ = 0;
        token_ = 0;
    }

    // Non-blocking; cheap and idempotent when not armed. True when the next paint may start.
    bool ready() {
        std::lock_guard<std::mutex> lock(mu_);
        syncReset();
        if (state_ == State::Idle)
            return true;
        const Clock::time_point now = now_();
        if (state_ == State::TimePaced) {
            if (now < paceUntil_)
                return false;
            state_ = State::Idle;
            return true;
        }
        if (token_ == 0)
            return issue(now);
        if (link_.ackedIdle(token_, shownBuf_)) {
            ++acked_;
            ++latches_;
            token_ = 0;
            if (streakTimeouts_ > 0) {
                log("r1: F5 latch ACK resumed after " + std::to_string(streakTimeouts_) +
                    " time-paced paint(s)");
                streakTimeouts_ = 0;
            }
            if (acked_ >= kLatches) {
                state_ = State::Idle;
                return true;
            }
            return issue(now);
        }
        if (now - issuedAt_ >= kLatchTimeout) {
            ++timeouts_;
            if (streakTimeouts_++ == 0)
                log("r1: F5 no-op latch token " + std::to_string(token_) +
                    " not ACKed in 100 ms (no FB_VBL?): time pacing >= 50 ms");
            token_ = 0;
            state_ = State::TimePaced;
            paceUntil_ = now + kTimePacing;
        }
        return false;
    }

    // Session edges (start and END): lock-free, so the session thread never waits on a painter.
    void reset() { resetEpoch_.fetch_add(1); }

    bool armed() {
        std::lock_guard<std::mutex> lock(mu_);
        syncReset();
        return state_ != State::Idle;
    }
    int latchesAcked() const { return latches_; }
    int timeouts() const { return timeouts_; }
    uint16_t inFlightToken() const { return token_; }

private:
    enum class State { Idle, Latching, TimePaced };

    void syncReset() {
        const uint32_t e = resetEpoch_.load();
        if (e != seenEpoch_) {
            seenEpoch_ = e;
            state_ = State::Idle;
            token_ = 0;
            acked_ = 0;
        }
    }

    bool issue(Clock::time_point now) {
        uint16_t t = 0;
        if (!policy_.requestIdle(shownBuf_, &t, nullptr))
            return false;   // inside a session: the session edge resets us
        token_ = t;
        issuedAt_ = now;
        return false;
    }

    void log(const std::string& line) {
        if (log_)
            log_(line);
    }

    PlxhSessionPolicy& policy_;
    PlxhLink& link_;
    std::function<Clock::time_point()> now_;
    std::function<void(const std::string&)> log_;
    std::mutex mu_;
    std::atomic<uint32_t> resetEpoch_{0};
    uint32_t seenEpoch_ = 0;
    State state_ = State::Idle;
    int shownBuf_ = -1;
    int acked_ = 0;
    uint16_t token_ = 0;
    Clock::time_point issuedAt_{};
    Clock::time_point paceUntil_{};
    int latches_ = 0;
    int timeouts_ = 0;
    int streakTimeouts_ = 0;
};

inline void PlxhSessionPolicy::enterSession(uint64_t gen) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        inSession_.store(true);
        en0Pending_.store(false);
        std::lock_guard<std::mutex> hold(holdMu_);   // D3/C1: takes over its own request's hold
        releaseLocked(holdGen_ != 0 && holdGen_ <= gen);
        newestGen_ = std::max(newestGen_, gen);
    }
    if (pacer_)
        pacer_->reset();
}

inline bool PlxhSessionPolicy::beginSession(int ticks, std::string* why, int recheckTicks) {
    enterSession();
    if (why)
        why->clear();
#ifdef MUT_PLXH_EN1_IN_SESSION   // test-only mutant: en=1 left on (no en=0 at BEGIN)
    (void)ticks;
    (void)recheckTicks;
    return true;
#else
    uint16_t token = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        token = link_.write(0, false);
    }
    if (!token)
        return fail(why, "PLXH store refused");
    const auto acked = [token](uint64_t w) {
        const Plxo o = decodePlxo(w);
        return o.valid && o.tokenLatched == token && !o.idleShown;
    };
    const auto hex = [](uint64_t w) {
        char s[24];
        std::snprintf(s, sizeof s, "0x%016llx", static_cast<unsigned long long>(w));
        return std::string(s);
    };
    // review 20:08 (a): the reference is PLXO right after the en=0 request (after any K2 zero). Only
    // an FB_VBL can change it.
    uint64_t ref = 0;
    bool readable = link_.plxoRaw(&ref);
    bool unchanged = true;
    for (int i = 0; i <= ticks; ++i) {
        uint64_t w = 0;
        const bool r = link_.plxoRaw(&w);
        if (r && acked(w)) {
            sessionToken_.store(token);
            return true;
        }
        readable = readable && r;
        unchanged = unchanged && w == ref;
        if (i < ticks)
            tick_();
    }
#ifndef MUT_PLXH_NO_RECHECK   // test-only mutant: no re-read after the window
    for (int i = 0; i < recheckTicks; ++i)
        tick_();
    {
        uint64_t w = 0;
        const bool r = link_.plxoRaw(&w);
        if (r && acked(w)) {
            sessionToken_.store(token);
            if (why)
                *why = "ACKed at the re-read " + std::to_string(recheckTicks) +
                       " ticks after the window";
            return true;
        }
        readable = readable && r;
        unchanged = unchanged && w == ref;
    }
#else
    (void)recheckTicks;
#endif
#ifdef MUT_PLXH_FALLBACK_ON_CHANGE   // test-only mutant: the fallback ignores a PLXO change
    readable = unchanged = true;
#endif
    if (!readable)
        return fail(why, "PLXO unreadable while waiting for the PLXH en=0 token " +
                             std::to_string(token) + " ACK");
    if (!unchanged) {
        const Plxo o = link_.plxo();
        return fail(why, "PLXO changed without ACKing PLXH en=0 token " + std::to_string(token) +
                             " (plxo valid=" + std::to_string(o.valid ? 1 : 0) +
                             " token_latched=" + std::to_string(o.tokenLatched) +
                             " idle_shown=" + std::to_string(o.idleShown ? 1 : 0) + ", was " +
                             hex(ref) + ")");
    }
    // (b): no FB_VBL. The en=0 request stays armed in the slot and latches at the first FB_VBL;
    // inSession blocks every en=1 until END.
    sessionToken_.store(token);
    en0Pending_.store(true);
    if (why)
        *why = "no FB_VBL: PLXO " + hex(ref) + " bit-identical over " + std::to_string(ticks) +
               " ticks and at the re-read " + std::to_string(recheckTicks) + " later";
    return true;
#endif
}

inline std::string PlxhSessionPolicy::endSession() {
    std::string note;
    std::function<void(const std::string&)> log;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (en0Pending_.exchange(false)) {   // review 20:08 (c)
            const uint16_t token = sessionToken_.load();
            const Plxo o = link_.plxo();
            if (o.valid && o.tokenLatched == token && !o.idleShown)
                note = "r1: PLXH en=0 token " + std::to_string(token) +
                       " ACKed late, after the first AU (FB_VBL resumed in the session)";
            else
                note = "r1: PLXH en=0 token " + std::to_string(token) +
                       " never ACKed in the session (plxo valid=" +
                       std::to_string(o.valid ? 1 : 0) +
                       " token_latched=" + std::to_string(o.tokenLatched) +
                       " idle_shown=" + std::to_string(o.idleShown ? 1 : 0) + ")";
        }
        inSession_.store(false);
        if (!note.empty())
            log = log_;
    }
    if (pacer_)
        pacer_->reset();
    if (log)
        log(note);
    return note;
}

}  // namespace r1
}  // namespace misterplex
