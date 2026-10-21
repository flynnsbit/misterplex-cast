#include "browse_render_test_common.hpp"
#include "r1_plxh_rtl_model.hpp"

#include "libmisterplex/browse_paint.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>

namespace {

class FakeTarget final : public misterplex::browse::BrowsePaintTarget {
public:
    explicit FakeTarget(misterplex::browse::IdleFbPath p) : path_(p), lateLatchRng(0x1A7E1004u) {
        bufs[0].assign(static_cast<std::size_t>(brtest::H) * brtest::Stride, 0);
        bufs[1].assign(static_cast<std::size_t>(brtest::H) * brtest::Stride, 0);
    }

    misterplex::browse::IdleFbPath path() const override { return path_; }

    misterplex::browse::BrowsePaintAck pollAck(std::uint32_t token, std::uint32_t) override {
        if (token == 0) {
            maybeLateLatch();
            return {true, false, true, shown};
        }
        if (timeoutNext) {
            timeoutNext = false;
            return {false, true, true, shown};
        }
        if (pendingToken != 0 && token == pendingToken && ((lateLatchRng() % 3u) != 0u)) {
            shown = pendingBuf;
            pendingToken = 0;
            ++acks;
            return {true, false, true, shown};
        }
        maybeLateLatch();
        if (pendingToken == 0 && shown == pendingBuf)
            return {true, false, true, shown};
        if (pendingToken != 0 && token == pendingToken)
            return {false, false, true, shown};
        brtest::require(token == pendingToken, "unexpected flip token");
        shown = pendingBuf;
        pendingToken = 0;
        ++acks;
        return {true, false, true, shown};
    }

    void writeLine(int idleBuf, int y, const std::uint8_t* rgb24Line) override {
        maybeLateLatch();
        brtest::require(idleBuf == 0 || idleBuf == 1, "bad idle buffer");
        brtest::require(!(idleBuf == shown), "wrote currently shown idle buffer");
        brtest::require(!(pendingToken != 0 && idleBuf == pendingBuf),
                        "wrote idle buffer with outstanding unlatch flip request");
        std::copy(rgb24Line, rgb24Line + brtest::Stride,
                  bufs[idleBuf].begin() + static_cast<std::ptrdiff_t>(y) * brtest::Stride);
        ++writes;
    }

    // One 16-bit token sequence, never 0, wrapping 0xFFFF -> 1 (the PLXH link's rule, review 19:01 A). It starts near
    // the top so the walk crosses the wrap.
    std::uint32_t requestFlip(int idleBuf) override {
        if (refuseNextFlip) {
            refuseNextFlip = false;
            ++refusedFlips;
            return 0;
        }
        lastToken = lastToken >= 0xFFFFu ? 1u : lastToken + 1u;
        if (lastToken == 1u)
            ++tokenWraps;
        pendingBuf = idleBuf;
        pendingToken = lastToken;
        ++flips;
        return lastToken;
    }

    bool flipAllowed() override {
        ++gateAsks;
        return allowFlips;
    }

    void paintAcked(int shownBuf) override {
        brtest::require(shownBuf == shown, "paintAcked buffer is not the shown idle buffer");
        ++paintAcks;
    }

    std::vector<std::uint8_t> bufs[2];
    int shown = -1;
    int pendingBuf = -1;
    std::uint32_t pendingToken = 0;
    std::uint32_t lastToken = 0xFFF0u;
    bool timeoutNext = false;
    bool allowFlips = true;
    bool refuseNextFlip = false;
    int acks = 0;
    int writes = 0;
    int flips = 0;
    int refusedFlips = 0;
    int tokenWraps = 0;
    int gateAsks = 0;
    int paintAcks = 0;

private:
    void maybeLateLatch() {
        if (pendingToken != 0 && (lateLatchRng() % 11u) == 0u) {
            shown = pendingBuf;
            pendingToken = 0;
            ++lateLatches;
        }
    }

    misterplex::browse::IdleFbPath path_;
    std::mt19937 lateLatchRng;
    int lateLatches = 0;
};

brtest::RenderModel mutate(brtest::RenderModel m, std::mt19937& rng) {
    const int op = static_cast<int>(rng() % 6u);
    if (op == 0) {
        m.cursorIndex = std::min<int>(static_cast<int>(m.rows.size()) - 1, m.cursorIndex + 1);
    } else if (op == 1) {
        m.cursorIndex = std::max(0, m.cursorIndex - 1);
    } else if (op == 2) {
        if (!m.rows.empty()) {
            auto& row = m.rows[static_cast<std::size_t>(rng() % m.rows.size())];
            row.label += ".";
        }
    } else if (op == 3) {
        m.footerText = "tick " + std::to_string(rng() % 10000u);
    } else if (op == 4) {
        m.viewportTop = static_cast<int>(rng() % 12u);
    } else if (!m.rows.empty()) {
        auto& row = m.rows[static_cast<std::size_t>(rng() % m.rows.size())];
        row.pct = row.pct < 0 ? static_cast<int>(rng() % 101u) : -1;
    }
    return m;
}

void expectShownEquals(FakeTarget& target, const brtest::RenderModel& model, const std::string& where) {
    brtest::require(target.shown == 0 || target.shown == 1, "nothing shown at " + where);
    const auto want = brtest::render(model);
    brtest::require(target.bufs[target.shown] == want, "shown buffer mismatch at " + where);
}

misterplex::browse::PaintResult pumpUntil(misterplex::browse::BrowsePaintPlanner& planner, FakeTarget& target,
                                          misterplex::browse::PaintStep want, const std::string& where) {
    for (int i = 0; i < 64; ++i) {
        const auto r = planner.pump(target);
        if (r.step == want)
            return r;
        brtest::require(r.step == misterplex::browse::PaintStep::Idle, "unexpected step at " + where);
    }
    brtest::require(false, "step never came at " + where);
    return {};
}

// --- w9 F5 rows (review 19:27): the planner over the REAL PLXH stack -------------------------
// BrowsePaintTarget on the /dev/mem path over r1_plxh.hpp (PlxhLink, PlxhSessionPolicy,
// PlxhF5Pacer) and the RTL model of the PLX slots (r1_plxh_rtl_model.hpp). Separate objects: the
// walk above is untouched.
class R1StackTarget final : public misterplex::browse::BrowsePaintTarget {
public:
    explicit R1StackTarget(std::uint16_t lastToken = 0)
        : link(rtl, lastToken),
          policy(link, [this] { vbl(); }),
          pacer(policy, link, [this] {
              return misterplex::r1::PlxhF5Pacer::Clock::time_point{} + std::chrono::milliseconds(ms);
          }) {
        rtl.staleSlot(lastToken);   // an earlier daemon's last token, already snapped by the RTL
        bufs[0].assign(static_cast<std::size_t>(brtest::H) * brtest::Stride, 0);
        bufs[1].assign(static_cast<std::size_t>(brtest::H) * brtest::Stride, 0);
    }

    misterplex::browse::IdleFbPath path() const override { return misterplex::browse::IdleFbPath::DevMem; }

    misterplex::browse::BrowsePaintAck pollAck(std::uint32_t token, std::uint32_t) override {
        const auto o = link.plxo();
        const bool idle = o.valid && o.idleShown;
        const int shown = idle ? o.idleBufShown : -1;
        if (token == 0)
            return {true, false, idle, shown};
        if (o.valid && o.tokenLatched == token) {
            pendingToken = 0;
            return {true, false, idle, shown};
        }
        return {false, false, idle, shown};
    }

    void writeLine(int idleBuf, int y, const std::uint8_t* rgb24Line) override {
        const auto o = link.plxo();
        brtest::require(!(o.valid && o.idleShown && o.idleBufShown == idleBuf), "R1 stack: wrote the shown buffer");
        brtest::require(!(pendingToken != 0 && idleBuf == pendingBuf), "R1 stack: wrote a buffer with a flip pending");
        std::copy(rgb24Line, rgb24Line + brtest::Stride,
                  bufs[idleBuf].begin() + static_cast<std::ptrdiff_t>(y) * brtest::Stride);
    }

    std::uint32_t requestFlip(int idleBuf) override {
        std::uint16_t t = 0;
        if (!policy.requestIdle(idleBuf, &t, nullptr))
            return 0;
        pendingBuf = idleBuf;
        pendingToken = t;
        flipTokens.push_back(t);
        return t;
    }

    bool flipAllowed() override { return !policy.inSession() && pacer.ready(); }
    void paintAcked(int shownBuf) override { pacer.paintAcked(shownBuf, true); }

    void vbl() {
        rtl.vbl();
        ms += 17;
    }
    int shownBuf() {
        const auto o = link.plxo();
        return o.valid && o.idleShown ? o.idleBufShown : -1;
    }
    int noopLatchesSince(std::size_t mark, int buf) const {
        int n = 0;
        for (std::size_t i = mark; i < rtl.latches.size(); ++i) {
            const auto& l = rtl.latches[i];
            const bool isFlip = std::find(flipTokens.begin(), flipTokens.end(), l.token) != flipTokens.end();
            n += (l.en && l.sel == buf && !isFlip) ? 1 : 0;
        }
        return n;
    }

    r1test::RtlPlxModel rtl;
    misterplex::r1::PlxhLink link;
    int ms = 0;
    misterplex::r1::PlxhSessionPolicy policy;
    misterplex::r1::PlxhF5Pacer pacer;
    std::vector<std::uint8_t> bufs[2];
    int pendingBuf = -1;
    std::uint32_t pendingToken = 0;
    std::vector<std::uint16_t> flipTokens;
};

misterplex::browse::PaintResult pumpVbl(misterplex::browse::BrowsePaintPlanner& planner, R1StackTarget& t,
                                        misterplex::browse::PaintStep want, const std::string& where) {
    using misterplex::browse::PaintStep;
    for (int i = 0; i < 64; ++i) {
        const auto r = planner.pump(t);
        if (r.step == want)
            return r;
        brtest::require(r.step == PaintStep::Idle || r.step == PaintStep::Refused, "unexpected step at " + where);
        t.vbl();
    }
    brtest::require(false, "step never came at " + where);
    return {};
}

void expectR1Shown(R1StackTarget& t, const brtest::RenderModel& model, const std::string& where) {
    const int shown = t.shownBuf();
    brtest::require(shown == 0 || shown == 1, "R1 stack: nothing shown at " + where);
    brtest::require(t.bufs[shown] == brtest::render(model), "R1 stack: shown buffer mismatch at " + where);
}

// One paint ACK, then the next paint: it must come only after exactly 2 no-op latches on the
// shown buffer (MUT_F5_PAINT_AFTER_1_LATCH: after 1, RED). Returns the latches seen.
int f5PaintPair(misterplex::browse::BrowsePaintPlanner& planner, R1StackTarget& t,
                const brtest::RenderModel& next, const std::string& where) {
    using misterplex::browse::PaintStep;
    const int shownA = t.shownBuf();
    const std::size_t mark = t.rtl.latches.size();
    planner.submit(next);
    pumpVbl(planner, t, PaintStep::Started, where + " start");
    const int noops = t.noopLatchesSince(mark, shownA);
    brtest::require(noops == 2, where + ": next paint after " + std::to_string(noops) +
                                    " no-op latches, F5 wants 2 (review 19:27)");
    pumpVbl(planner, t, PaintStep::Acked, where + " ack");
    expectR1Shown(t, next, where);
    return noops;
}

void f5Rows() {
    using misterplex::browse::PaintStep;
    // F5-1: no paint until 2 no-op latches after the paint ACK; each no-op only after the
    // previous ACK; the shown buffer never changes under the no-ops.
    R1StackTarget t;
    misterplex::browse::BrowsePaintPlanner planner;
    planner.arm();
    planner.submit(brtest::s2Servers());
    pumpVbl(planner, t, PaintStep::Started, "f5 first paint");
    pumpVbl(planner, t, PaintStep::Acked, "f5 first ack");
    expectR1Shown(t, brtest::s2Servers(), "f5 first");
    f5PaintPair(planner, t, brtest::s3Root(), "f5 second paint");
    f5PaintPair(planner, t, brtest::s4Movies(false), "f5 third paint");
    brtest::require(t.rtl.mixedLatches == 0, "f5: a mixed latch");

    // F5-2: session start closes the gate (no write, no flip, no en=1 store); en=0 is ACKed;
    // after END the first paint does not wait on the dead no-op token.
    planner.submit(brtest::s3Root());
    t.policy.enterSession();
    const std::size_t ops0 = t.rtl.ops.size();
    for (int i = 0; i < 3; ++i) {
        brtest::require(planner.pump(t).step == PaintStep::Refused, "f5: session did not refuse");
        t.vbl();
    }
    std::string why;
    brtest::require(t.policy.beginSession(10, &why), "f5: en=0 not ACKed: " + why);
    for (std::size_t i = ops0; i < t.rtl.ops.size(); ++i)
        brtest::require(!(t.rtl.ops[i].kind == 'w' && t.rtl.ops[i].phys == misterplex::r1::kPlxhPhys + 4 &&
                          (t.rtl.ops[i].value & 1u)),
                        "f5: an en=1 PLXH store inside the session");
    t.policy.endSession();
    pumpVbl(planner, t, PaintStep::Started, "f5 paint after END");
    pumpVbl(planner, t, PaintStep::Acked, "f5 ack after END");
    expectR1Shown(t, brtest::s3Root(), "f5 after END");

    // F5-3: the token sequence wraps 0xFFFF -> 1 through flips and no-ops.
    R1StackTarget w(0xFFF8);
    misterplex::browse::BrowsePaintPlanner wp;
    wp.arm();
    wp.submit(brtest::s2Servers());
    pumpVbl(wp, w, PaintStep::Started, "f5 wrap first");
    pumpVbl(wp, w, PaintStep::Acked, "f5 wrap first ack");
    f5PaintPair(wp, w, brtest::s3Root(), "f5 wrap 2");
    f5PaintPair(wp, w, brtest::s4Movies(false), "f5 wrap 3");
    f5PaintPair(wp, w, brtest::s2Servers(), "f5 wrap 4");
    brtest::require(w.link.lastToken() < 0xFFF8u && w.link.lastToken() != 0, "f5: the token sequence did not wrap");
    brtest::require(w.rtl.mixedLatches == 0, "f5 wrap: a mixed latch");
}

} // namespace

int main() {
    try {
        misterplex::browse::BrowsePaintPlanner planner;
        FakeTarget target(misterplex::browse::IdleFbPath::WriteCombining);
        planner.arm();

        brtest::RenderModel current = brtest::s4Movies(false);
        brtest::RenderModel pending = current;
        brtest::RenderModel inFlightModel = current;
        bool haveInFlightModel = false;
        std::mt19937 rng(0x1004BEEF);
        int starts = 0;
        int acked = 0;
        int timeouts = 0;

        for (int step = 0; step < 500; ++step) {
            const int burst = 1 + static_cast<int>(rng() % 4u);
            for (int i = 0; i < burst; ++i) {
                current = mutate(current, rng);
                planner.submit(current);
                pending = current;
            }

            if (!planner.inFlight()) {
                const auto r = planner.pump(target);
                if (r.step == misterplex::browse::PaintStep::Started) {
                    ++starts;
                    inFlightModel = pending;
                    haveInFlightModel = true;
                }
            }

            if (planner.inFlight()) {
                if (step % 37 == 11) {
                    target.timeoutNext = true;
                    const auto r = planner.pump(target);
                    brtest::require(r.step == misterplex::browse::PaintStep::AckTimeout, "expected timeout");
                    ++timeouts;
                    haveInFlightModel = false;
                } else {
                    const auto r = planner.pump(target);
                    if (r.step == misterplex::browse::PaintStep::Acked) {
                        brtest::require(haveInFlightModel, "acked without model");
                        expectShownEquals(target, inFlightModel, "walk step " + std::to_string(step));
                        ++acked;
                        haveInFlightModel = false;
                    }
                }
            }
        }

        for (int drain = 0; drain < 20; ++drain) {
            if (!planner.inFlight()) {
                const auto r = planner.pump(target);
                if (r.step == misterplex::browse::PaintStep::Started) {
                    ++starts;
                    inFlightModel = pending;
                    haveInFlightModel = true;
                }
            }
            if (planner.inFlight()) {
                const auto r = planner.pump(target);
                if (r.step == misterplex::browse::PaintStep::Acked) {
                    expectShownEquals(target, inFlightModel, "drain");
                    ++acked;
                    haveInFlightModel = false;
                }
            }
        }

        brtest::require(target.paintAcks == acked, "paintAcked not called exactly once per paint ACK");
        brtest::require(target.tokenWraps >= 1, "walk did not cross the 0xFFFF -> 1 token wrap");

        using misterplex::browse::PaintStep;
        FakeTarget devmem(misterplex::browse::IdleFbPath::DevMem);
        misterplex::browse::BrowsePaintPlanner devPlanner;
        devPlanner.arm();
        brtest::require(std::string(devPlanner.pathLogLine(devmem.path())) == "r1: idlefb path=devmem",
                        "devmem path log line changed");
        devPlanner.submit(brtest::s2Servers());
        pumpUntil(devPlanner, devmem, PaintStep::Started, "gate first paint");
        pumpUntil(devPlanner, devmem, PaintStep::Acked, "gate first ack");
        brtest::require(devmem.paintAcks == 1, "first paint ACK not reported");

        // Gate row: the target's gate (session, or its F5 pacer) is asked on an idle pump with nothing pending, and
        // while it is closed a pending model writes nothing and requests nothing.
        devmem.allowFlips = false;
        const int asks = devmem.gateAsks;
        brtest::require(devPlanner.pump(devmem).step == PaintStep::Idle, "idle pump did something");
        brtest::require(devmem.gateAsks == asks + 1, "gate not asked on an idle pump (pacer would stall)");
        devPlanner.submit(brtest::s3Root());
        const int writes = devmem.writes;
        const int flips = devmem.flips;
        for (int i = 0; i < 3; ++i)
            brtest::require(devPlanner.pump(devmem).step == PaintStep::Refused, "closed gate did not refuse");
        brtest::require(devmem.writes == writes && devmem.flips == flips, "closed gate still wrote or flipped");
        expectShownEquals(devmem, brtest::s2Servers(), "closed gate");
        devmem.allowFlips = true;
        pumpUntil(devPlanner, devmem, PaintStep::Started, "gate reopened");
        pumpUntil(devPlanner, devmem, PaintStep::Acked, "gate reopened ack");
        expectShownEquals(devmem, brtest::s3Root(), "gate reopened");
        brtest::require(devmem.paintAcks == 2, "second paint ACK not reported");

        // Refused after the copy (a session began between the gate and the request): nothing is shown, and the
        // next paints copy both buffers in full.
        devPlanner.submit(brtest::s4Movies(false));
        devmem.refuseNextFlip = true;
        brtest::require(pumpUntil(devPlanner, devmem, PaintStep::Refused, "refuse after copy").linesCopied == 0,
                        "refused paint reported lines");
        brtest::require(devmem.refusedFlips == 1, "flip was not refused");
        expectShownEquals(devmem, brtest::s3Root(), "refused flip");
        const auto full1 = pumpUntil(devPlanner, devmem, PaintStep::Started, "full after refuse");
        brtest::require(full1.linesCopied == brtest::H, "paint after a refused flip was not full");
        pumpUntil(devPlanner, devmem, PaintStep::Acked, "full after refuse ack");
        expectShownEquals(devmem, brtest::s4Movies(false), "full after refuse");
        const auto full2 = pumpUntil(devPlanner, devmem, PaintStep::Started, "second full after refuse");
        brtest::require(full2.linesCopied == brtest::H, "other buffer not repainted in full after a refused flip");
        pumpUntil(devPlanner, devmem, PaintStep::Acked, "second full after refuse ack");
        expectShownEquals(devmem, brtest::s4Movies(false), "second full after refuse");
        brtest::require(devmem.paintAcks == 4, "paint ACKs after refuse not reported");

        f5Rows();

        std::cout << "RG-2 PASS steps=500 starts=" << starts << " acks=" << acked
                  << " timeouts=" << timeouts << " token_wraps=" << target.tokenWraps
                  << " gate=ok refuse_after_copy=ok f5=ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "RG-2 FAIL: " << e.what() << "\n";
        return 1;
    }
}
