// R1 PLXH host side (host/libmisterplex/r1_plxh.hpp) against an RTL model of the PLX slots
// (tests/unit/r1_plxh_rtl_model.hpp). review 19:01 A (write order, tokens), 19:16 (1)(c)
// (en=0 from session start to END, ACKed before the first AU), 19:27 F5 (2 no-op latches),
// kit K2 (PLXO zeroed before the first arm), 20:08 (a)-(c), (e) (the no-FB_VBL fallback), wW 20:05
// (a fresh link's tokens follow the PLXH slot and PLXO: daemon restart, core reset).
// Mutants, each must FAIL this test: MUT_PLXH_MAGIC_FIRST, MUT_PLXH_EN1_IN_SESSION,
// MUT_F5_PAINT_AFTER_1_LATCH, MUT_PLXH_FALLBACK_ON_CHANGE, MUT_PLXH_NO_RECHECK, MUT_PLXH_NO_SEED,
// MUT_PLXH_NO_PRIME, MUT_PLXH_PRIME_EN0 (review 20:23 RULING C: the first request after start and
// after a core reload goes out as T then T+1 with the same control word).
#include "r1_plxh_rtl_model.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace misterplex::r1;
using r1test::Op;
using r1test::RtlPlxModel;

namespace {

int fails = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

// Every PLXH request in ops[from..] is: store32 upper @PLXH+4, barrier, store32 magic @PLXH (a
// prime's T is followed by one more barrier).
// The first request ever is preceded by the K2 zero: store64 0 @PLXO, barrier.
bool orderOk(const std::vector<Op>& ops, size_t from, bool k2First, std::string* why) {
    size_t i = from;
    if (k2First) {
        if (i + 1 >= ops.size() || ops[i].kind != 'W' || ops[i].phys != kPlxoPhys ||
            ops[i].value != 0 || ops[i + 1].kind != 'b') {
            *why = "K2: PLXO not zeroed by one 64-bit store before the first arm";
            return false;
        }
        i += 2;
    }
    while (i < ops.size()) {
        if (i + 2 >= ops.size() || ops[i].kind != 'w' || ops[i].phys != kPlxhPhys + 4 ||
            ops[i + 1].kind != 'b' || ops[i + 2].kind != 'w' || ops[i + 2].phys != kPlxhPhys ||
            ops[i + 2].value != kPlxhMagic) {
            *why = "request at op " + std::to_string(i) + " is not upper, dmb, magic";
            return false;
        }
        i += 3;
        if (i < ops.size() && ops[i].kind == 'b')
            ++i;   // the prime's barrier after T (review 20:23 P1)
    }
    return true;
}

// The en bit of every PLXH upper store in ops[from, to).
bool anyEn1(const std::vector<Op>& ops, size_t from, size_t to) {
    for (size_t i = from; i < to && i < ops.size(); ++i)
        if (ops[i].kind == 'w' && ops[i].phys == kPlxhPhys + 4 && (ops[i].value & 1u))
            return true;
    return false;
}

struct Rig {
    RtlPlxModel rtl;
    std::vector<std::string> linkLogs;   // the link's one line per prime (review 20:23 P5)
    PlxhLink link;
    int ms = 0;
    int ticks = 0;                      // policy wait ticks so far
    std::function<void(int)> onTick;    // runs at the start of tick n (before its VBL)
    std::vector<std::string> logs;
    PlxhSessionPolicy policy;
    PlxhF5Pacer pacer;
    explicit Rig(uint16_t lastToken = 0)
        : link(rtl, lastToken, [this](const std::string& s) { linkLogs.push_back(s); }),
          policy(link,
                 [this] {
                     ++ticks;
                     if (onTick)
                         onTick(ticks);
                     rtl.vbl();
                     ms += 17;
                 }),
          pacer(policy, link,
                [this] { return PlxhF5Pacer::Clock::time_point{} + std::chrono::milliseconds(ms); },
                [this](const std::string& s) { logs.push_back(s); }) {
        rtl.staleSlot(lastToken);   // an earlier daemon's last token, already snapped by the RTL
    }
    // A paint flip outside a session, latched on the next VBL.
    uint16_t paint(int buf) {
        uint16_t t = 0;
        std::string why;
        expect(policy.requestIdle(buf, &t, &why), "paint flip refused: " + why);
        rtl.vbl();
        ms += 17;
        expect(link.ackedIdle(t, buf), "paint flip not ACKed");
        return t;
    }
};

}  // namespace

int main() {
    // 1. K2 + write order (19:01 A). review 20:00 N2: the only 64-bit store is the K2 PLXO zero; no
    //    PLXH request ever goes through store64.
    {
        Rig r;
        r.paint(0);
        r.paint(1);
        std::string why;
        expect(orderOk(r.rtl.ops, 0, true, &why), "order: " + why);
        expect(r.rtl.mixedLatches == 0, "order: a mixed latch");
        int store64s = 0;
        for (const auto& op : r.rtl.ops)
            store64s += op.kind == 'W' ? 1 : 0;
        expect(store64s == 1 && r.rtl.store64Violations == 0,
               "store64 used " + std::to_string(store64s) + "x (K2 PLXO zero only)");
        // review 20:23 P1/P2: the first request is primed, T then T+1 with the same control word.
        const auto& o = r.rtl.ops;
        expect(o.size() > 8 && o[2].phys == kPlxhPhys + 4 && o[5].kind == 'b' &&
                   o[6].phys == kPlxhPhys + 4 && (o[2].value & 0xFFFFu) == (o[6].value & 0xFFFFu) &&
                   nextToken(static_cast<uint16_t>(o[2].value >> 16)) ==
                       static_cast<uint16_t>(o[6].value >> 16) && r.rtl.settles == 1,
               "prime: T and T+1 do not carry the same control word, or no settle between");
    }
    // 2. A VBL between the two beats of a request: the slot already holds the magic (steady state),
    //    so the poll sees the new upper word as one complete beat; never a mixed latch.
    {
        Rig r;
        r.rtl.vblAfterStore32 = 1;
        uint16_t t = 0;
        expect(r.policy.requestIdle(1, &t, nullptr), "first arm refused");
        expect(r.rtl.mixedLatches == 0, "VBL between beats latched a stale/torn upper word");
        r.rtl.vbl();
        expect(r.link.ackedIdle(t, 1), "first arm not latched on the VBL after the magic");
        expect(r.rtl.mixedLatches == 0, "first arm: a mixed latch");
    }
    // 3. Tokens: 16-bit, never 0, 0xFFFF wraps to 1, fresh every request (the first paint is
    //    primed: T = 0xFFFD, T+1 = 0xFFFE).
    {
        Rig r(0xFFFC);
        const uint16_t a = r.paint(0), b = r.paint(1), c = r.paint(0);
        expect(a == 0xFFFE && b == 0xFFFF && c == 1,
               "token wrap: got " + std::to_string(a) + "," + std::to_string(b) + "," +
                   std::to_string(c));
    }
    // 4. Session (19:16 (1)(c)): idle shown -> session start blocks en=1 -> en=0 written and
    //    ACKed before the first AU -> no en=1 until END -> en=1 again after END.
    {
        Rig r;
        r.paint(1);
        expect(r.link.plxo().idleShown, "idle not shown before the session");
        const size_t start = r.rtl.ops.size();
        r.policy.enterSession();   // before BEGIN
        expect(!r.policy.requestIdle(0, nullptr, nullptr), "en=1 not refused after session start");
        std::string why;
        expect(r.policy.beginSession(10, &why), "en=0 not ACKed: " + why);
        // The first AU would be sent here.
        const Plxo atFirstAu = r.link.plxo();
        expect(atFirstAu.valid && atFirstAu.tokenLatched == r.policy.sessionToken() &&
                   !atFirstAu.idleShown,
               "PLXO at the first AU does not show the en=0 token with idle_shown 0");
        expect(!r.policy.requestIdle(1, nullptr, nullptr), "en=1 not refused mid-session");
        r.rtl.vbl();   // a mid-session VBL (a generation_clear would not select idle: en=0)
        expect(!r.link.plxo().idleShown, "idle shown mid-session");
        const size_t end = r.rtl.ops.size();
        r.policy.endSession();
        expect(!anyEn1(r.rtl.ops, start, end), "an en=1 PLXH store between session start and END");
        r.paint(0);
        expect(r.link.plxo().idleShown, "idle not shown after END");
        std::string orderWhy;
        expect(orderOk(r.rtl.ops, 0, true, &orderWhy), "session order: " + orderWhy);
    }
    // 5. review 20:08 (a)-(c), (e): en=0 not ACKed with no FB_VBL. PLXO bit-identical over the wait
    //    and at the re-read: the session proceeds, en=0 stays armed in the slot, en=1 is blocked,
    //    the wait is bounded, and END reports the late ACK (5b) or its absence (5a).
    {
        Rig r;
        r.paint(0);
        r.rtl.frozen = true;
        std::string why;
        const int t0 = r.ticks;
        expect(r.policy.beginSession(5, &why, 3), "5a: no-FB_VBL fallback refused: " + why);
        expect(r.ticks - t0 == 5 + 3,
               "5a: waited " + std::to_string(r.ticks - t0) + " ticks, want exactly 5 + 3");
        expect(r.policy.en0Pending() && why.find("no FB_VBL") != std::string::npos,
               "5a: not the no-FB_VBL fallback: " + why);
        const uint16_t tok = r.policy.sessionToken();
        expect(r.rtl.lo == kPlxhMagic && (r.rtl.hi >> 16) == tok && !(r.rtl.hi & 1u),
               "5a: the en=0 request is not left armed in the PLXH slot");
        expect(!r.policy.requestIdle(1, nullptr, nullptr), "5a: en=1 not blocked after the fallback");
        std::vector<std::string> endLogs;
        r.policy.setLog([&endLogs](const std::string& s) { endLogs.push_back(s); });
        const std::string note = r.policy.endSession();
        expect(note.find("never ACKed") != std::string::npos, "5a: END line: " + note);
        expect(!r.policy.en0Pending() && r.policy.endSession().empty() && endLogs.size() == 1 &&
                   endLogs[0] == note,
               "5a: the END line is not logged once per session");
    }
    {   // 5b: FB_VBL resumes in the session: the armed en=0 latches at the first VBL; END says late.
        Rig r;
        r.paint(1);
        r.rtl.frozen = true;
        std::string why;
        expect(r.policy.beginSession(5, &why, 3) && r.policy.en0Pending(), "5b: fallback: " + why);
        r.rtl.frozen = false;
        r.rtl.vbl();
        expect(r.link.ackedOff(r.policy.sessionToken()),
               "5b: the armed en=0 did not latch at the first FB_VBL");
        const std::string note = r.policy.endSession();
        expect(note.find("ACKed late") != std::string::npos, "5b: END line: " + note);
        r.paint(0);
        expect(r.link.plxo().idleShown, "5b: idle not shown after END");
    }
    {   // 5c: PLXO changes in the window without this en=0's ACK: fail (MUT_PLXH_FALLBACK_ON_CHANGE).
        Rig r;
        r.paint(0);
        r.rtl.frozen = true;
        r.onTick = [&r](int t) {
            if (t == 2)
                r.rtl.plxo ^= uint64_t(1) << 33;   // a PLXO write that is not the en=0 ACK
        };
        std::string why;
        expect(!r.policy.beginSession(5, &why, 3) && !r.policy.en0Pending(),
               "5c: PLXO changed in the window, yet the session proceeded");
        expect(why.find("changed") != std::string::npos, "5c: reason: " + why);
    }
    {   // 5d: PLXO changes after the window, before the re-read: fail (MUT_PLXH_NO_RECHECK RED).
        Rig r;
        r.paint(0);
        r.rtl.frozen = true;
        r.onTick = [&r](int t) {
            if (t == 6)
                r.rtl.plxo ^= uint64_t(1) << 33;
        };
        std::string why;
        expect(!r.policy.beginSession(5, &why, 3), "5d: a change before the re-read was missed");
        expect(why.find("changed") != std::string::npos, "5d: reason: " + why);
    }
    {   // 5e: FB_VBL resumes before the re-read: an ACK, not the fallback (MUT_PLXH_NO_RECHECK RED).
        Rig r;
        r.paint(0);
        r.rtl.frozen = true;
        r.onTick = [&r](int t) {
            if (t == 6)
                r.rtl.frozen = false;
        };
        std::string why;
        expect(r.policy.beginSession(5, &why, 3) && !r.policy.en0Pending() &&
                   why.find("re-read") != std::string::npos &&
                   r.link.ackedOff(r.policy.sessionToken()),
               "5e: the ACK at the re-read is not an ACK: " + why);
        expect(r.policy.endSession().empty(), "5e: an END line after an ACKed en=0");
    }
    {   // 5f: PLXO unreadable (the core gate closed): fail, never the fallback.
        Rig r;
        r.paint(0);
        r.rtl.frozen = true;
        r.onTick = [&r](int t) {
            if (t == 1)
                r.rtl.loadFails = true;
        };
        std::string why;
        expect(!r.policy.beginSession(5, &why, 3) && why.find("unreadable") != std::string::npos,
               "5f: an unreadable PLXO taken as no FB_VBL: " + why);
    }
    // 6. F5: paint ACK -> 2 same-buffer no-op latches, one at a time -> ready.
    {
        Rig r;
        r.paint(0);
        r.pacer.paintAcked(0, true);
        const size_t from = r.rtl.latches.size();
        const size_t ops0 = r.rtl.ops.size();
        expect(!r.pacer.ready(), "ready before any latch");
        const uint16_t n1 = r.pacer.inFlightToken();
        expect(n1 != 0 && r.rtl.ops.size() == ops0 + 3, "no-op 1 not issued as one request");
        expect(!r.pacer.ready() && r.rtl.ops.size() == ops0 + 3,
               "a second no-op issued before the first ACK (queued)");
        r.rtl.vbl();
        r.ms += 17;
        expect(!r.pacer.ready(), "ready after 1 latch");
        const uint16_t n2 = r.pacer.inFlightToken();
        expect(n2 != 0 && n2 != n1, "no-op 2 token not fresh");
        r.rtl.vbl();
        r.ms += 17;
        expect(r.pacer.ready(), "not ready after 2 latches");
        int noops = 0;
        for (size_t i = from; i < r.rtl.latches.size(); ++i)
            noops += (r.rtl.latches[i].en && r.rtl.latches[i].sel == 0) ? 1 : 0;
        expect(noops == 2, "F5 saw " + std::to_string(noops) + " no-op latches, want 2");
        expect(r.link.ackedIdle(n2, 0), "the shown buffer changed under the no-ops");
        expect(r.pacer.ready() && r.rtl.ops.size() == ops0 + 6,
               "pacer issued more than 2 no-ops or is not idempotent when ready");
        std::string why;
        expect(orderOk(r.rtl.ops, 0, true, &why), "F5 order: " + why);
    }
    // 7. F5 timeout: no FB_VBL -> 100 ms per latch -> >= 50 ms of wall time -> ready, logged once.
    {
        Rig r;
        r.paint(1);
        r.pacer.paintAcked(1, true);
        r.rtl.frozen = true;
        expect(!r.pacer.ready(), "ready with no latch");
        r.ms += 99;
        expect(!r.pacer.ready(), "timed out before 100 ms");
        r.ms += 1;
        expect(!r.pacer.ready(), "ready at the timeout, before the 50 ms pacing");
        r.ms += 49;
        expect(!r.pacer.ready(), "pacing shorter than 50 ms");
        r.ms += 1;
        expect(r.pacer.ready(), "not ready after 100 + 50 ms (unbounded?)");
        expect(r.logs.size() == 1, "timeout logged " + std::to_string(r.logs.size()) + " times");
        // The 2nd no-op times out: one latch, then 100 + 50 ms, then ready (wW note 7).
        r.rtl.frozen = false;
        r.paint(0);
        r.pacer.paintAcked(0, true);
        expect(!r.pacer.ready(), "ready before the first latch (2nd timeout row)");
        r.rtl.vbl();
        expect(!r.pacer.ready(), "ready after one latch (2nd timeout row)");
        r.rtl.frozen = true;
        r.ms += 100;
        expect(!r.pacer.ready(), "2nd no-op: ready at its timeout");
        r.ms += 50;
        expect(r.pacer.ready(), "2nd no-op: not ready after 100 + 50 ms");
        expect(r.pacer.timeouts() == 2, "timeouts counted " + std::to_string(r.pacer.timeouts()));
    }
    // 8. Token wrap through the pacer's no-op pair.
    {
        Rig r(0xFFFC);
        r.paint(0);   // primed: T = 0xFFFD, T+1 = 0xFFFE
        r.pacer.paintAcked(0, true);
        r.pacer.ready();   // no-op 1: 0xFFFF
        r.rtl.vbl();
        r.pacer.ready();   // no-op 2: 1
        expect(r.pacer.inFlightToken() == 1, "no-op 2 did not wrap to 1");
        r.rtl.vbl();
        expect(r.pacer.ready(), "not ready across the token wrap");
        expect(r.rtl.mixedLatches == 0, "wrap: a mixed latch");
    }
    // 9. WC path: paintAcked is ignored; never paced, never writes.
    {
        Rig r;
        r.paint(0);
        const size_t ops0 = r.rtl.ops.size();
        r.pacer.paintAcked(0, false);
        expect(r.pacer.ready() && r.rtl.ops.size() == ops0, "WC path paced or wrote");
    }
    // 10. Session edges reset the pacer: the en=0 write supersedes the unlatched no-op, and the
    //     first paint after END does not wait on that dead token.
    {
        Rig r;
        r.paint(1);
        r.pacer.paintAcked(1, true);
        expect(!r.pacer.ready(), "pacer not armed");
        std::string why;
        expect(r.policy.beginSession(10, &why), "en=0 not ACKed (reset row): " + why);
        r.policy.endSession();
        expect(r.pacer.ready() && !r.pacer.armed(), "pacer still waits on a dead no-op after END");
    }
    // 11. Restart (wW R-b): an old link's only PLXH was one primed en=0 (T 1, T+1 2); the daemon
    //     restarts and K2 zeroes PLXO. The fresh link's first request must ACK: from token 1 again
    //     its T+1 (2) repeats the published key {0, 0, 2}, so no PLXO (MUT_PLXH_NO_SEED RED).
    {
        RtlPlxModel rtl;
        rtl.staleSlot(0);
        {
            PlxhLink old(rtl);
            const uint16_t t = old.write(0, false);
            rtl.vbl();
            expect(t == 2 && old.ackedOff(t), "11: the old link's en=0 not ACKed");
        }
        PlxhLink fresh(rtl);
        const uint16_t t = fresh.write(0, false);
        rtl.vbl();
        expect(t == 4 && fresh.ackedOff(t),
               "11: the fresh link's first en=0 (token " + std::to_string(t) +
                   ") not ACKed after a restart");
        const uint16_t t1 = fresh.write(1, true);
        rtl.vbl();
        expect(fresh.ackedIdle(t1, 1), "11: en=1 after the restart not ACKed");
    }
    // 11b. The same restart through a port that reads PLXO only (an unreadable PLXH slot counts as
    //      absent): PLXO's token seeds (MUT_PLXH_NO_SEED RED).
    {
        RtlPlxModel rtl;
        rtl.staleSlot(0);
        {
            PlxhLink old(rtl);
            old.write(0, false);
            rtl.vbl();
        }
        rtl.plxhLoadFails = true;
        PlxhLink fresh(rtl);
        const uint16_t t = fresh.write(0, false);
        rtl.vbl();
        expect(t == 4 && fresh.ackedOff(t),
               "11b: PLXO-only seed: the fresh first en=0 (token " + std::to_string(t) +
                   ") not ACKed");
    }
    // 12. Restart after a no-FB_VBL session: the slot's en=0 token (3) was accepted but never
    //     latched (PLXO shows 2). The fresh link follows the slot's token: T = 4, T+1 = 5. From
    //     PLXO's token its T would repeat the accepted 3 (P4).
    {
        RtlPlxModel rtl;
        rtl.staleSlot(0);
        {
            PlxhLink old(rtl);
            old.write(1, true);   // primed: T 1, T+1 2, latched
            rtl.vbl();
            rtl.frozen = true;
            old.write(0, false);   // token 3, accepted, not latched; the daemon dies
            rtl.vbl();
        }
        PlxhLink fresh(rtl);
        const uint16_t t = fresh.write(0, true);
        rtl.frozen = false;
        rtl.vbl();
        expect(t == 5 && fresh.ackedIdle(t, 0),
               "12: the fresh link's first en=1 (token " + std::to_string(t) + ") not ACKed");
    }
    // 13. Core reset with a stale slot (token 2): the first poll snaps it and never accepts it. The
    //     fresh link's T and T+1 follow it (3, 4).
    {
        RtlPlxModel rtl;
        rtl.staleSlot(0);
        {
            PlxhLink old(rtl);
            old.write(1, true);
            rtl.vbl();
        }
        rtl.coreReset();
        rtl.poll();
        PlxhLink fresh(rtl);
        const uint16_t t = fresh.write(1, true);
        rtl.vbl();
        expect(t == 4 && fresh.ackedIdle(t, 1),
               "13: the first request after a core reset (token " + std::to_string(t) +
                   ") not ACKed");
    }
    // 14. review 20:23 RULING C, cold slot (no magic since the reset): the RTL snaps the prime's T
    //     and accepts T+1, so the first paint ACKs on T+1; one prime line (MUT_PLXH_NO_PRIME RED:
    //     the lone first request is the snap and never ACKs).
    {
        Rig r;
        r.rtl.lo = r.rtl.hi = 0;
        r.rtl.armed = false;
        const uint16_t t = r.paint(1);
        expect(t == 2 && r.rtl.armed && r.rtl.snap == 1,
               "14: the cold first paint did not ACK on T+1 with T snapped (token " +
                   std::to_string(t) + ")");
        expect(r.linkLogs.size() == 1 &&
                   r.linkLogs[0].find("prime (start): T=1 T+1=2, slot cold, T+1 ACKed") !=
                       std::string::npos,
               "14: prime line: " + (r.linkLogs.empty() ? std::string("none") : r.linkLogs[0]));
    }
    // 14b. The first session en=0 after a cold boot ACKs, no fallback (MUT_PLXH_NO_PRIME RED).
    {
        Rig r;
        r.rtl.lo = r.rtl.hi = 0;
        r.rtl.armed = false;
        std::string why;
        expect(r.policy.beginSession(5, &why, 3) && !r.policy.en0Pending() &&
                   r.link.ackedOff(r.policy.sessionToken()),
               "14b: the cold first en=0 did not ACK (fallback?): " + why);
        expect(r.policy.endSession().empty(), "14b: an END line after an ACKed en=0");
    }
    // 14c. Warm slot with idle up (an earlier daemon's idle FB on buffer 0), restart, first flip
    //      to buffer 1, with an FB_VBL inside the settle so T itself registers: idle_sel stays 1
    //      across T and T+1, no blank (MUT_PLXH_PRIME_EN0 RED: its en=0 T blanks the idle FB).
    {
        RtlPlxModel rtl;
        rtl.staleSlot(0);
        {
            PlxhLink old(rtl);
            const uint16_t t = old.write(0, true);
            rtl.vbl();
            expect(old.ackedIdle(t, 0), "14c: idle not up before the restart");
        }
        rtl.vblInSettle = true;
        const size_t mark = rtl.latches.size();
        std::vector<std::string> logs;
        PlxhLink fresh(rtl, 0, [&logs](const std::string& s) { logs.push_back(s); });
        const uint16_t t = fresh.write(1, true);
        rtl.vbl();
        expect(fresh.ackedIdle(t, 1), "14c: the warm first flip not ACKed on T+1");
        bool blank = false;
        for (size_t i = mark; i < rtl.latches.size(); ++i)
            blank = blank || !rtl.latches[i].en;
        expect(!blank && rtl.latches.size() - mark == 2,
               "14c: the prime blanked the idle FB, or T did not register in the settle");
        expect(logs.size() == 1 && logs[0].find("slot warm, T+1 ACKed") != std::string::npos,
               "14c: prime line: " + (logs.empty() ? std::string("none") : logs[0]));
    }
    // 14d. A core reload mid-run (CoreGate changes() moves) re-primes the next request; here the
    //      reload also left the slot cold (MUT_PLXH_NO_PRIME RED).
    {
        Rig r;
        r.paint(0);
        r.rtl.coreReset();
        r.rtl.lo = r.rtl.hi = 0;
        const uint16_t t = r.paint(1);
        expect(r.rtl.settles == 2 && r.linkLogs.size() == 2 &&
                   r.linkLogs[1].find("prime (reload)") != std::string::npos &&
                   r.linkLogs[1].find("slot cold, T+1 ACKed") != std::string::npos,
               "14d: no re-prime after the core reload (token " + std::to_string(t) + ")");
        const uint16_t t2 = r.paint(0);
        expect(r.rtl.settles == 2 && r.linkLogs.size() == 2 && t2 == nextToken(t),
               "14d: a request after the re-prime was primed again");
    }

    if (fails) {
        std::printf("FAIL r1 PLXH (%d failures)\n", fails);
        return 1;
    }
    std::printf("PASS r1 PLXH (K2 + upper/dmb/magic order; VBL between beats; token wrap; "
                "session en=0 ACKed before the first AU, no en=1 to END; no-FB_VBL fallback "
                "5a-5f; F5 2 latches, 100+50 ms timeouts, wrap, WC unpaced, session reset; "
                "restart/reload token seed 11, 11b, 12, 13; prime 14, 14b, 14c, 14d)\n");
    return 0;
}
