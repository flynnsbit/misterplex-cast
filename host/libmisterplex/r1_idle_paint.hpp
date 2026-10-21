#pragma once
// R1 phase 2: the concrete idle FB paint target. BrowsePaintPlanner (browse_paint.hpp) paints into
// the two 640x480 RGB24 idle buffers (IdleFbMap, 0x31600000 / 0x31700000) and flips them through
// the PLXH host side (r1_plxh.hpp, w9: one token sequence, session en=0 policy, F5 pacer).
//
// Entry clear (review 20:18 ruling: STATE trigger, no IdleMode trigger; wX premise check).
// PLXH en=1 does not override a live decoded picture: plex_fb480.sv:333
// idle_sel <= h_en_sys && (!fb_live || generation_clear), registered at each FB_VBL rise. A normal
// stop or EOF already drops fb_live, because the reader pulses out_flush on FLUSH and END
// (ddr_bitstream_reader.sv:1058-1059) -> stream_path video_clear -> sys_generation_clear. Abort
// does the same through a ring epoch reset. A decoded picture can still own scan-out outside a
// session only when no END or epoch reset reached the reader, for example a daemon restart
// mid-session with an unmanaged core gate (a managed gate's verify probe resets the ring itself).
// Then the daemon issues ONE stream clear (a ring epoch reset) for that entry:
//  (1) only when PLXO shows the CURRENT en=1 token latched with idle_shown == 0: the token of this
//      target's latest flip, which is also the link's latest write. Never on an unACKed or stale
//      token;
//  (2) at most once per entry (an entry ends at a session END), logged, no retry loop;
//  (3) refused in a session, with a BEGIN pending (the policy is in session from before BEGIN), or
//      while the daemon reports session work pending;
//  (4) order: paint -> requestIdle -> ACK check -> clear. The last frame goes straight to the idle
//      FB at the next FB_VBL, with no black gap.
#include "libmisterplex/browse_paint.hpp"
#include "libmisterplex/r1_plxh.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace misterplex {
namespace r1 {

struct IdleEntryClearHooks {
    // One stream clear: a bitstream ring epoch reset (reader out_flush -> generation_clear). False
    // with *why when refused or not ACKed. The daemon runs it under its own session guard.
    std::function<bool(std::string* why)> streamClear;
    // Changes at every session END. At most one clear per value (condition 2).
    std::function<uint64_t()> entryGeneration;
    // The daemon's own "a session is starting or running" view (playing, FPGA session).
    std::function<bool()> sessionPending;
    // F1 (PLEX_IDLE_FB_240P; fpga_build_ids.hpp idleBeatPlxl): the 240p core shows the idle FB only
    // once it has qualified the daemon's PLXL beat (F1-PREREG v2.1 + code delta D1), so a latched
    // en=1 token with idle_shown == 0 is that hysteresis, not a decoded picture owning scan-out. No
    // entry clear: the flip completes when the core turns the FB on (the planner retries after an
    // ACK timeout).
    bool coreQualifiedOn = false;
};

class R1IdleFbTarget final : public browse::BrowsePaintTarget {
public:
    // Extra ticks a pollAck waits after a clear, so the clear reaches the next FB_VBL (1 tick = 1 ms).
    static constexpr uint32_t kClearSettleTicks = 100;

    R1IdleFbTarget(browse::IdleFbPath path, std::function<uint8_t*(int)> buffer, PlxhLink& link,
                   PlxhSessionPolicy& policy, PlxhF5Pacer& pacer, IdleEntryClearHooks hooks,
                   std::function<void(const std::string&)> log, std::function<void()> tick = {})
        : path_(path), buffer_(std::move(buffer)), link_(link), policy_(policy), pacer_(pacer),
          hooks_(std::move(hooks)), log_(std::move(log)), tick_(std::move(tick)) {
        if (!tick_)
            tick_ = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
    }

    browse::IdleFbPath path() const override { return path_; }

    void writeLine(int idleBuf, int y, const uint8_t* rgb24Line) override {
        if ((idleBuf != 0 && idleBuf != 1) || y < 0 || y >= static_cast<int>(kIdleH))
            return;
        uint8_t* base = buffer_ ? buffer_(idleBuf) : nullptr;
        if (base)
            std::memcpy(base + static_cast<size_t>(y) * kIdleStride, rgb24Line, kIdleStride);
    }

    uint32_t requestFlip(int idleBuf) override {
        uint16_t t = 0;
        std::string why;
        if (!policy_.requestIdle(idleBuf, &t, &why)) {
            if (!flipRefusalLogged_)
                log("r1: idle flip refused: " + why);
            flipRefusalLogged_ = true;
            return 0;
        }
        flipRefusalLogged_ = false;
        flipToken_ = t;
        flipBuf_ = idleBuf;
        return t;
    }

    // D3 (review 06:57): no flip from the PLAY request on, not only from BEGIN.
    bool flipAllowed() override {
        return !policy_.inSession() && !policy_.playRequestPending() && pacer_.ready();
    }

    void paintAcked(int shownBuf) override {
        pacer_.paintAcked(shownBuf, path_ == browse::IdleFbPath::DevMem);
    }

    browse::BrowsePaintAck pollAck(uint32_t token, uint32_t timeoutMs) override {
        browse::BrowsePaintAck a{};
        if (token == 0) {   // a peek at what PLXO shows
            const Plxo o = link_.plxo();
            a.complete = true;
            a.idleShown = o.valid && o.idleShown;
            a.idleBufShown = a.idleShown ? o.idleBufShown : -1;
            return a;
        }
        const uint16_t tok = static_cast<uint16_t>(token);
        const int want = tok == flipToken_ ? flipBuf_ : -1;
        uint32_t budget = timeoutMs;
        bool clearedHere = false;
        for (uint32_t waited = 0;; ++waited) {
            const Plxo o = link_.plxo();
#ifndef MUT_R1_NO_SUPERSEDE   // test-only mutant: a dead request is awaited forever
            // The link's newest request (any writer) has latched and it is not this one: the slot
            // held the newer request when the RTL accepted, so this one can never latch.
            if (o.valid && o.tokenLatched != tok && o.tokenLatched == link_.lastToken()) {
                a.superseded = true;
                a.idleShown = o.idleShown;
                a.idleBufShown = o.idleShown ? o.idleBufShown : -1;
                return a;
            }
#endif
            if (o.valid && o.tokenLatched == tok) {
                if (o.idleShown && (want < 0 || o.idleBufShown == want)) {
                    a.complete = true;
                    a.idleShown = true;
                    a.idleBufShown = o.idleBufShown;
                    return a;
                }
                if (!o.idleShown && maybeEntryClear(tok)) {
                    budget = waited + kClearSettleTicks;
                    clearedHere = true;
                }
            }
            if (waited >= budget) {
                if (clearedHere && !(o.valid && o.idleShown))
                    noteClearNoEffect(tok);
                a.timedOut = true;
                a.idleShown = o.valid && o.idleShown;
                a.idleBufShown = a.idleShown ? o.idleBufShown : -1;
                return a;
            }
            tick_();
        }
    }

    int entryClears() const { return clears_; }
    int entryClearRefusals() const { return refusals_; }
    int entryClearsWithoutEffect() const { return noEffect_; }

private:
    // review 20:12 N1: a clear that was ACKed but left idle_shown 0 through the settle window
    // is logged once, and the entry stops there (no further clear until the next entry).
    void noteClearNoEffect(uint16_t tok) {
        ++noEffect_;
        if (!noEffectLogged_)
            log("r1: idle entry clear took no effect: idle_shown still 0 " +
                std::to_string(kClearSettleTicks) + " ticks after the clear (token " + std::to_string(tok) +
                "); no further clear this entry");
        noEffectLogged_ = true;
    }
    // The latched-but-not-shown case: a decoded picture still owns scan-out. True when a clear
    // was issued and ACKed.
    bool maybeEntryClear(uint16_t tok) {
#ifndef MUT_F1_R1_ENTRY_CLEAR   // test-only mutant: an F1 row clears as R1 does
        if (hooks_.coreQualifiedOn)
            return false;
#endif
#ifndef MUT_R1_ENTRY_CLEAR_ANY_TOKEN   // test-only mutant: drops condition (1)
        if (tok != flipToken_ || tok != link_.lastToken())
            return false;
#endif
#ifndef MUT_R1_ENTRY_CLEAR_IN_SESSION   // test-only mutant: drops condition (3)
        if (policy_.inSession() || policy_.playRequestPending() ||
            (hooks_.sessionPending && hooks_.sessionPending())) {
            ++refusals_;
            if (!clearRefusalLogged_)
                log("r1: idle entry clear refused: session or BEGIN pending (token " +
                    std::to_string(tok) + ")");
            clearRefusalLogged_ = true;
            return false;
        }
#endif
        const uint64_t gen = hooks_.entryGeneration ? hooks_.entryGeneration() : 0;
#ifndef MUT_R1_ENTRY_CLEAR_RETRY   // test-only mutant: drops condition (2)
        if (clearTried_ && gen == clearGen_)
            return false;
#endif
        clearTried_ = true;
        clearGen_ = gen;
        clearRefusalLogged_ = false;
        noEffectLogged_ = false;
#ifdef MUT_R1_NO_ENTRY_CLEAR   // test-only mutant: never clears
        return false;
#else
        std::string why;
        const bool ok = hooks_.streamClear && hooks_.streamClear(&why);
        ++clears_;
        log("r1: idle entry clear reason=latched_not_shown token=" + std::to_string(tok) +
            " buf=" + std::to_string(flipBuf_) + " entry=" + std::to_string(gen) +
            (ok ? " result=ok" : " result=failed: " + why + " (no retry this entry)"));
        return ok;
#endif
    }

    void log(const std::string& line) {
        if (log_)
            log_(line);
    }

    browse::IdleFbPath path_;
    std::function<uint8_t*(int)> buffer_;
    PlxhLink& link_;
    PlxhSessionPolicy& policy_;
    PlxhF5Pacer& pacer_;
    IdleEntryClearHooks hooks_;
    std::function<void(const std::string&)> log_;
    std::function<void()> tick_;
    uint16_t flipToken_ = 0;
    int flipBuf_ = -1;
    bool flipRefusalLogged_ = false;
    bool clearTried_ = false;
    uint64_t clearGen_ = 0;
    bool clearRefusalLogged_ = false;
    bool noEffectLogged_ = false;
    int clears_ = 0;
    int refusals_ = 0;
    int noEffect_ = 0;
};

// review 06:57 D2: a session's en=0 takes the idle FB off screen behind the planner's back, so the
// first idle paint after every session END repaints in full and flips, even an unchanged frame (the
// planner would call it NoChange), and owes one log line: re-shown, or why not.
class R1SessionEndRearm {
public:
    // True once per new session END count: the caller arms the planner.
    bool takeSessionEnd(uint64_t sessionEnds) {
        if (sessionEnds == seen_)
            return false;
        seen_ = sessionEnds;
#ifdef MUT_R1_NO_SESSION_REARM   // test-only mutant: the pre-D2 behaviour, no re-arm at a session END
        return false;
#else
        return true;
#endif
    }

private:
    uint64_t seen_ = 0;
};

// review 06:57 D1: an idle flip that is still owed (refused, not ACKed in the 400 ms bound, or due
// after a session END) is retried by the idle thread within kFlipRetryMs, never left to the 30 s
// static-screen safety net.
constexpr int kIdleStaticRepaintMs = 30000;
constexpr int kIdleMovingRepaintMs = 100;
constexpr int kFlipRetryMs = 250;
inline int idleRepaintStepMs(bool moving, bool flipOwed) {
    if (moving)
        return kIdleMovingRepaintMs;
#ifdef MUT_R1_NO_FLIP_RETRY   // test-only mutant: an owed flip waits for the 30 s safety net
    (void)flipOwed;
    return kIdleStaticRepaintMs;
#else
    return flipOwed ? kFlipRetryMs : kIdleStaticRepaintMs;
#endif
}

}  // namespace r1
}  // namespace misterplex
