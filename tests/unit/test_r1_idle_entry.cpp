// R1 entry clear rows (review 20:18 ruling, STATE trigger; 20:12 N1): R-a..R-g against a model of what the
// RTL does with PLXH, a live decoded picture and the reader's clears. The model follows
// r1-idlefb-mvpb (4c210d99 / f203fc97):
//  - PLXH: the doorbell poll arms on the magic word and follows {token, buf_sel, en}; at every
//    FB_VBL rise idle_sel <= en && (!fb_live || generation_clear) (plex_fb480.sv:333), and PLXO is
//    published change-only on {idle_sel, idle_sel ? idle_buf : 0, token} (:340-346). K2's zero of
//    the PLXO word does not reset that key.
//  - fb_live rises with a decoded picture and drops on generation_clear (:248-250).
//  - The reader pulses out_flush on FLUSH and END (ddr_bitstream_reader.sv:1058-1059) and on an
//    epoch change (:776-786). out_flush -> stream_path video_clear (:213-214) ->
//    sys_generation_clear (Plex.sv:2115-2120).
//  - ARX:ARY (ar = Original) is 4:3 while idle_sel, else the session's source aspect while a
//    frame exists (Plex.sv:79-86).
// Mutants (each must FAIL): MUT_R1_NO_ENTRY_CLEAR (R-b), MUT_R1_ENTRY_CLEAR_IN_SESSION (R-e),
// MUT_R1_ENTRY_CLEAR_RETRY (R-d), MUT_R1_ENTRY_CLEAR_ANY_TOKEN (R-f), MUT_R1_NO_SUPERSEDE (R-h, R-i).
#include "browse_render_test_common.hpp"

#include "libmisterplex/idle_screen.hpp"
#include "libmisterplex/r1_idle_paint.hpp"
#include "playback_controls.hpp"   // TransportGenerations, stopClearGeneration (R-m m6)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using misterplex::IdleMode;
using misterplex::browse::IdleFbPath;
using misterplex::browse::PaintResult;
using misterplex::browse::PaintStep;
using misterplex::r1::kIdleBytes;
using misterplex::r1::kPlxhMagic;
using misterplex::r1::kPlxhPhys;
using misterplex::r1::kPlxoMagic;
using misterplex::r1::kPlxoPhys;

class Fb480Model final : public misterplex::r1::PlxPort {
public:
    // warm: the RTL has already polled a PLXH from an earlier core run (DDR keeps the slot across
    // reloads) and recorded it as its snap. cold: the slot has no magic (power-on), so the first
    // good-magic poll will be the snap and is not accepted.
    explicit Fb480Model(bool warm = true) {
        if (warm) {
            hi = uint32_t(0x7777u) << 16;   // {token 0x7777, buf 0, en 0}
            lo = kPlxhMagic;
            armed = true;
            ref = 0x7777;
        }
    }

    bool store32(uint32_t phys, uint32_t value) override {
        if (refuseStores)
            return false;   // R-l: a PLXH store the port refuses (D1's unreachable page, a closed gate)
        if (phys == kPlxhPhys) {
            lo = value;
            if (value == kPlxhMagic && (hi & 1u) != 0)
                note('R');   // an en=1 request armed
        } else if (phys == kPlxhPhys + 4) {
            hi = value;
        } else {
            return false;
        }
        poll();   // the doorbell poll is free-running (~2.9 us), far faster than the daemon's stores
        return true;
    }
    bool store64(uint32_t phys, uint64_t value) override {
        if (phys != kPlxoPhys)
            return false;
        plxo = value;   // K2; the RTL's change-only key is untouched
        return true;
    }
    bool load64(uint32_t phys, uint64_t* value) override {
        if (phys == kPlxoPhys) {
            *value = plxo;
            return true;
        }
        if (phys == kPlxhPhys) {   // the slot as FpgaSpi::plxLoad64 reads it
            *value = (uint64_t(hi) << 32) | lo;
            return true;
        }
        return false;
    }
    void barrier() override {}

    // ddr_frame_store.sv:1996-2013 (ac6c3c3b): the first good-magic poll after a reset records the
    // snap and accepts nothing; later polls accept a token != the last accepted one (or the snap).
    // Polling continues without FB_VBL; only the fb480 latch below needs a VBL.
    void poll() {
        if (lo != kPlxhMagic)
            return;
        const uint16_t t = static_cast<uint16_t>(hi >> 16);
        if (!armed) {
            armed = true;
            ref = t;
            return;
        }
        if (t == ref)
            return;
        ref = t;
        hEn = (hi & 1u) != 0;
        hBuf = static_cast<int>((hi >> 1) & 1u);
        hTok = t;
        hAccepted = true;
    }

    void vbl() {
        if (frozen)
            return;
        ++vbls;
        poll();
        idleSel = hEn && !fbLive;
        idleBuf = hBuf;
        if (!hAccepted)
            return;   // nothing accepted since the reset: no PLXO publish
        const uint64_t key = (uint64_t(hTok) << 2) | (uint64_t(idleSel ? idleBuf : 0) << 1) |
                             uint64_t(idleSel ? 1 : 0);
        if (!published || key != pubKey) {
            published = true;
            pubKey = key;
            plxo = (uint64_t(hTok) << 48) | (uint64_t(idleSel ? 0 : 1) << 34) |
                   (uint64_t(idleSel ? idleBuf : 0) << 33) | (uint64_t(idleSel ? 1 : 0) << 32) |
                   kPlxoMagic;
        }
    }

    void presentPicture() {
        fbLive = true;
        hasFrame = true;
    }
    void generationClear() {
        fbLive = false;
        note('G');
    }
    void flushRecord() { generationClear(); }
    void endRecord() { generationClear(); }
    bool epochReset() {
        ++epochResets;
        if (wedged)
            return false;   // the reader never ACKs the epoch: no out_flush
        note('C');
        generationClear();
        if (freezeOnClear)
            frozen = true;   // no FB_VBL after the clear (HDMI down)
        return true;
    }
    std::pair<int, int> arx() const {
        if (idleSel)
            return {4, 3};
        if (hasFrame && srcX && srcY)
            return {srcX, srcY};
        return {4, 3};
    }
    void note(char e) {
        if (events.empty() || events.back() != e)
            events.push_back(e);
    }

    uint32_t lo = 0, hi = 0;
    uint64_t plxo = 0xDEADBEEFCAFEF00Dull;
    bool armed = false;      // the post-reset snap has been taken
    uint16_t ref = 0;        // the snap, then the last accepted token
    bool hAccepted = false;  // a request has been accepted since the reset
    bool hEn = false;
    int hBuf = 0;
    uint16_t hTok = 0;
    bool idleSel = false;
    int idleBuf = 0;
    bool published = false;
    uint64_t pubKey = 0;
    bool fbLive = false;
    bool hasFrame = false;
    int srcX = 0, srcY = 0;
    bool frozen = false;
    bool freezeOnClear = false;
    bool wedged = false;
    bool refuseStores = false;
    int vbls = 0;
    int epochResets = 0;
    std::string events;   // P paint, R en=1 armed, G generation_clear, C entry clear
};

// One daemon process: its PLXH host side (w9 r1_plxh.hpp), the target and the planner, with the
// daemon hooks of media_player.cpp (stream clear = one ring epoch reset; entry = session ENDs).
struct Daemon {
    explicit Daemon(Fb480Model& model, bool f1 = false)
        : m(model),
          link(m),
          policy(link, [this] { m.vbl(); }),
          pacer(policy, link, {}, [this](const std::string& l) { logs.push_back(l); }),
          target(IdleFbPath::WriteCombining,
                 [this](int b) {
                     m.note('P');
                     return bufs[b].data();
                 },
                 link, policy, pacer,
                 misterplex::r1::IdleEntryClearHooks{
                     [this](std::string* why) {
                         if (m.epochReset())
                             return true;
                         *why = "reset_epoch not ACKed (reader wedged)";
                         return false;
                     },
                     [this] { return sessionEnds; }, [this] { return playing; }, f1},
                 [this](const std::string& l) { logs.push_back(l); }, [this] { m.vbl(); }) {
        bufs[0].assign(kIdleBytes, 0);
        bufs[1].assign(kIdleBytes, 0);
        planner.arm();
    }

    // fpgaThreadMain order: enterSession before BEGIN, en=0 ACKed after it, then pictures. `gen`:
    // the session's play generation (its enter takes over that request's hold, review 07:30 C1).
    void sessionWithPicture(uint64_t gen = 0) {
        playing = true;
        policy.enterSession(gen);
        std::string why;
        brtest::require(policy.beginSession(200, &why), "en=0 not ACKed: " + why);
        m.srcX = 16;
        m.srcY = 9;
        m.presentPicture();
        m.vbl();
    }
    // A normal stop or EOF: FLUSH then END (media_player.cpp:3887-3890), then the session END.
    void normalStop() {
        m.flushRecord();
        m.endRecord();
        policy.endSession();
        ++sessionEnds;
        playing = false;
    }
    int countLogs(const std::string& needle) const {
        return static_cast<int>(std::count_if(logs.begin(), logs.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        }));
    }

    Fb480Model& m;
    misterplex::r1::PlxhLink link;
    misterplex::r1::PlxhSessionPolicy policy;
    misterplex::r1::PlxhF5Pacer pacer;
    std::vector<uint8_t> bufs[2];
    uint64_t sessionEnds = 0;
    bool playing = false;
    std::vector<std::string> logs;
    misterplex::r1::R1IdleFbTarget target;
    misterplex::browse::BrowsePaintPlanner planner;
};

PaintResult pumpUntil(Daemon& d, PaintStep want, int maxPumps, const std::string& where) {
    for (int i = 0; i < maxPumps; ++i) {
        const PaintResult r = d.planner.pump(d.target);
        if (r.step == want)
            return r;
        d.m.vbl();
    }
    brtest::require(false, where + ": step never came");
    return {};
}

// A daemon that painted the S0 idle screen (en=1), started a session (en=0 ACKed, a live 16:9
// picture) and died mid-session: no FLUSH, no END, no endSession.
void oldDaemonDiesMidSession(Fb480Model& m) {
    Daemon old(m);
    std::vector<uint8_t> f(kIdleBytes);
    misterplex::renderIdleRgb24(f.data(), 640, 480, IdleMode::Logo, 0);
    old.planner.submitFrame(f);
    pumpUntil(old, PaintStep::Started, 8, "old daemon S0 paint");
    pumpUntil(old, PaintStep::Acked, 8, "old daemon S0 ack");
    old.sessionWithPicture();
}

void expectIdleShows(Daemon& d, const std::vector<uint8_t>& want, const std::string& where) {
    d.m.vbl();
    brtest::require(d.m.idleSel, where + ": idle FB not selected (the last frame holds)");
    brtest::require(d.m.arx() == std::make_pair(4, 3), where + ": ARX is not 4:3");
    brtest::require(d.bufs[d.m.idleBuf] == want, where + ": shown idle buffer is not the paint");
}

// R-a (the R1 merge-gate row): IdleMode LastFrame, normal stop, then browse. END already cleared,
// so the browse shows idle + 4:3 with 0 entry clears.
void rowA() {
    Fb480Model m;
    Daemon d(m);
    d.sessionWithPicture();
    brtest::require(m.fbLive && !m.idleSel && m.arx() == std::make_pair(16, 9), "R-a: no live 16:9 picture");
    d.normalStop();
    m.vbl();
    brtest::require(!m.fbLive && !m.idleSel, "R-a: a normal stop left the decoded picture live");
    // LastFrame paints nothing until browse opens (media_player paintIdle).
    d.planner.submit(brtest::s3Root());
    pumpUntil(d, PaintStep::Started, 8, "R-a paint");
    pumpUntil(d, PaintStep::Acked, 8, "R-a ack");
    expectIdleShows(d, brtest::render(brtest::s3Root()), "R-a");
    brtest::require(d.target.entryClears() == 0 && m.epochResets == 0, "R-a: an entry clear after a normal stop");
    std::cout << "R-a PASS LastFrame stop -> browse: idle_sel=1 ARX 4:3, entry clears 0\n";
}

// R-b: a daemon restart mid-session (no END reaches the reader). The first en=1 latches with
// idle_shown 0; one clear; the next FB_VBL shows idle + 4:3.
void rowB() {
    Fb480Model m;
    oldDaemonDiesMidSession(m);
    Daemon d(m);
    m.vbl();
    brtest::require(m.fbLive && !m.idleSel, "R-b: the last frame is not live after the restart");
    const std::size_t base = m.events.size();   // the new daemon's events start here
    d.planner.submit(brtest::s3Root());
    pumpUntil(d, PaintStep::Started, 8, "R-b paint");
    pumpUntil(d, PaintStep::Acked, 8, "R-b ack");
    expectIdleShows(d, brtest::render(brtest::s3Root()), "R-b");
    brtest::require(d.target.entryClears() == 1 && m.epochResets == 1, "R-b: not exactly one entry clear");
    brtest::require(d.countLogs("r1: idle entry clear reason=latched_not_shown") == 1 &&
                        d.countLogs("result=ok") == 1,
                    "R-b: the clear is not logged once");
    // review 20:18 (4): paint -> requestIdle -> ACK check -> clear.
    const std::size_t p = m.events.find('P', base), r = m.events.find('R', p), c = m.events.find('C', base);
    brtest::require(p != std::string::npos && r != std::string::npos && c != std::string::npos && p < r && r < c,
                    "R-b: order is not paint -> requestIdle -> clear (events " + m.events + ")");
    // The next paint needs no clear.
    d.planner.submit(brtest::s4Movies(false));
    pumpUntil(d, PaintStep::Started, 8, "R-b repaint");
    pumpUntil(d, PaintStep::Acked, 8, "R-b repaint ack");
    expectIdleShows(d, brtest::render(brtest::s4Movies(false)), "R-b repaint");
    brtest::require(d.target.entryClears() == 1, "R-b: a second clear");
    std::cout << "R-b PASS restart mid-session -> browse: 1 entry clear (" << m.events
              << "), idle_sel=1 ARX 4:3\n";
}

// R-c: in a session (from before BEGIN) the planner writes nothing, requests no en=1 and clears
// nothing.
void rowC() {
    Fb480Model m;
    Daemon d(m);
    d.sessionWithPicture();
    const std::string before = m.events;
    d.planner.submit(brtest::s3Root());
    for (int i = 0; i < 5; ++i)
        brtest::require(d.planner.pump(d.target).step == PaintStep::Refused, "R-c: a paint started in a session");
    brtest::require(m.events == before, "R-c: paint or en=1 in a session (events " + m.events + ")");
    brtest::require(d.target.entryClears() == 0 && m.epochResets == 0, "R-c: a clear in a session");
    brtest::require(m.fbLive && !m.idleSel, "R-c: the session picture lost scan-out");
    std::cout << "R-c PASS in session: no paint, no en=1, no clear\n";
}

// R-d: the clear fails (a wedged reader). One attempt per entry, logged once, no retry loop; the
// next entry (after a session END) may try once more.
void rowD() {
    Fb480Model m;
    oldDaemonDiesMidSession(m);
    m.wedged = true;
    Daemon d(m);
    d.planner.submit(brtest::s3Root());
    for (int i = 0; i < 300; ++i) {
        d.planner.pump(d.target);
        m.vbl();
    }
    brtest::require(d.target.entryClears() == 1 && m.epochResets == 1,
                    "R-d: " + std::to_string(m.epochResets) + " clear attempts in one entry (want 1)");
    brtest::require(d.countLogs("result=failed") == 1, "R-d: the failure is not logged once");
    brtest::require(!m.idleSel, "R-d: idle selected without a clear");
    ++d.sessionEnds;   // a session END starts the next entry
    for (int i = 0; i < 300; ++i) {
        d.planner.pump(d.target);
        m.vbl();
    }
    brtest::require(m.epochResets == 2, "R-d: not one attempt in the next entry");
    std::cout << "R-d PASS wedged reader: 1 clear attempt per entry, logged once, bounded\n";
}

// R-e: refused in a session or with a BEGIN pending, even when the trigger holds.
void rowE() {
    Fb480Model m;
    oldDaemonDiesMidSession(m);
    Daemon d(m);
    const uint32_t t = d.target.requestFlip(0);   // outside a session
    brtest::require(t != 0, "R-e: en=1 refused outside a session");
    m.vbl();
    brtest::require(m.hTok == t && !m.idleSel, "R-e: the trigger does not hold");
    d.policy.enterSession();   // a session starts: BEGIN pending, en=0 not written yet
    d.target.pollAck(t, 20);
    brtest::require(d.target.entryClears() == 0 && m.epochResets == 0, "R-e: entry clear with a BEGIN pending");
    d.policy.endSession();
    d.playing = true;   // the daemon's own view: play() started, the FPGA thread not yet in session
    d.target.pollAck(t, 20);
    brtest::require(d.target.entryClears() == 0 && m.epochResets == 0, "R-e: entry clear while playing");
    brtest::require(d.target.entryClearRefusals() > 0 && d.countLogs("r1: idle entry clear refused") == 1,
                    "R-e: the refusal is not logged once");
    std::cout << "R-e PASS refused in session / BEGIN pending / playing\n";
}

// R-f: condition (1). No clear on an unACKed token, nor on a stale one that is not the link's
// latest request.
void rowF() {
    Fb480Model m;
    oldDaemonDiesMidSession(m);
    Daemon d(m);
    m.frozen = true;
    const uint32_t t1 = d.target.requestFlip(0);
    d.target.pollAck(t1, 10);   // never latches (no FB_VBL)
    brtest::require(m.epochResets == 0, "R-f: clear on an unACKed token");
    m.frozen = false;
    m.vbl();   // t1 latches with idle_shown 0
    m.frozen = true;
    const uint32_t t2 = d.target.requestFlip(1);   // newer request, not latched yet
    brtest::require(t2 != t1, "R-f: no fresh token");
    d.target.pollAck(t1, 10);   // PLXO shows t1 latched, idle_shown 0, but t1 is stale
    brtest::require(m.epochResets == 0, "R-f: clear on a stale token");
    m.frozen = false;
    std::cout << "R-f PASS no clear on an unACKed or stale token\n";
}

// R-g (review 20:12 N1): the clear is ACKed but no FB_VBL follows (HDMI down), so idle_shown
// stays 0 through the settle window. That is logged once, and the entry stops: no second clear.
void rowG() {
    Fb480Model m;
    oldDaemonDiesMidSession(m);
    m.freezeOnClear = true;
    Daemon d(m);
    d.planner.submit(brtest::s3Root());
    for (int i = 0; i < 50; ++i) {
        d.planner.pump(d.target);
        m.vbl();
    }
    brtest::require(m.epochResets == 1 && d.target.entryClears() == 1, "R-g: not exactly one clear");
    brtest::require(d.target.entryClearsWithoutEffect() >= 1 && d.countLogs("took no effect") == 1,
                    "R-g: the no-effect window is not logged exactly once");
    brtest::require(!m.idleSel, "R-g: idle selected without an FB_VBL");
    std::cout << "R-g PASS clear without effect (no FB_VBL): logged once, no second clear\n";
}

// R-b2 (review 20:12 N2, on w9's token-restart fix 9afa5d4c and the ruling C prime e9393b7e): a cold
// boot (no PLXH magic in the slot), a LastFrame daemon whose only request was its first session's
// en=0, then a crash mid-session. The prime sends that en=0 as T (taken as the RTL's post-reset
// snap) and T+1 (accepted and ACKed). The restarted link seeds past the slot token, so its first
// en=1 is accepted and latches with idle_shown 0, and the one entry clear shows idle + 4:3.
void rowB2() {
    Fb480Model m(/*warm=*/false);
    uint16_t oldSessionToken = 0;
    {
        Daemon old(m);
        old.sessionWithPicture();
        oldSessionToken = old.policy.sessionToken();
    }
    const misterplex::r1::Plxo acked = misterplex::r1::decodePlxo(m.plxo);
    brtest::require(m.armed && m.hAccepted && m.hTok == oldSessionToken && !m.hEn && acked.valid &&
                        acked.tokenLatched == oldSessionToken && !acked.idleShown,
                    "R-b2: the primed en=0 (T+1) was not accepted and ACKed on the cold slot");
    Daemon d(m);
    m.vbl();
    brtest::require(m.fbLive && !m.idleSel, "R-b2: the last frame is not live after the restart");
    d.planner.submit(brtest::s3Root());
    pumpUntil(d, PaintStep::Started, 8, "R-b2 paint");
    pumpUntil(d, PaintStep::Acked, 8, "R-b2 ack");
    expectIdleShows(d, brtest::render(brtest::s3Root()), "R-b2");
    brtest::require(d.target.entryClears() == 1 && m.epochResets == 1, "R-b2: not exactly one entry clear");
    std::cout << "R-b2 PASS cold boot (primed en=0 ACKed), one-request daemon crash -> browse: 1 entry clear, "
                 "idle_sel=1 ARX 4:3\n";
}

// R-h: a paint times out with no FB_VBL (HDMI off). A cast then runs and stops: its en=0 is the
// newest request and latches once FB_VBL is back. After END the dead paint token resolves as
// superseded and the browse paints. MUT_R1_NO_SUPERSEDE waits on the dead token forever: RED.
void rowH() {
    Fb480Model m;
    Daemon d(m);
    m.frozen = true;
    d.planner.submit(brtest::s3Root());
    pumpUntil(d, PaintStep::Started, 4, "R-h paint");
    pumpUntil(d, PaintStep::AckTimeout, 4, "R-h timeout");
    m.frozen = false;
    d.sessionWithPicture();
    d.normalStop();
    d.planner.submit(brtest::s4Movies(false));
    pumpUntil(d, PaintStep::Superseded, 4, "R-h dead paint token");
    pumpUntil(d, PaintStep::Started, 8, "R-h repaint");
    pumpUntil(d, PaintStep::Acked, 8, "R-h ack");
    expectIdleShows(d, brtest::render(brtest::s4Movies(false)), "R-h");
    brtest::require(d.target.entryClears() == 0, "R-h: an entry clear after a normal stop");
    std::cout << "R-h PASS paint timed out (no FB_VBL), session, stop -> superseded, browse shows\n";
}

// R-i: a paint is in flight when a session starts; the session's en=0 latches first. After END the
// in-flight token resolves as superseded, both buffers are repainted and the paint shows.
// MUT_R1_NO_SUPERSEDE: RED.
void rowI() {
    Fb480Model m;
    Daemon d(m);
    d.planner.submit(brtest::s3Root());
    pumpUntil(d, PaintStep::Started, 4, "R-i paint");
    d.sessionWithPicture();
    d.normalStop();
    const PaintResult s = pumpUntil(d, PaintStep::Superseded, 4, "R-i dead in-flight token");
    const PaintResult r = pumpUntil(d, PaintStep::Started, 8, "R-i repaint");
    brtest::require(r.linesCopied >= brtest::H, "R-i: the repaint after a superseded flip was not full");
    pumpUntil(d, PaintStep::Acked, 8, "R-i ack");
    expectIdleShows(d, brtest::render(brtest::s3Root()), "R-i");
    std::cout << "R-i PASS in-flight paint superseded by a session -> full repaint shows (token " << s.token
              << ")\n";
}

// S0 IDLE through the same planner: the renderIdleRgb24 chevron is what the idle FB shows.
void rowS0() {
    Fb480Model m;
    Daemon d(m);
    std::vector<uint8_t> f(kIdleBytes);
    misterplex::renderIdleRgb24(f.data(), 640, 480, IdleMode::Logo, 0);
    d.planner.submitFrame(f);
    pumpUntil(d, PaintStep::Started, 8, "S0 paint");
    pumpUntil(d, PaintStep::Acked, 8, "S0 ack");
    expectIdleShows(d, f, "S0");
    brtest::require(d.target.entryClears() == 0, "S0: an entry clear at boot");
    d.planner.submitFrame(std::vector<uint8_t>(10));   // a wrong size is ignored
    brtest::require(!d.planner.busy(), "S0: a wrong-size frame was queued");
    std::cout << "S0 PASS chevron frame painted through the planner\n";
}

// R-j (review 06:57 D2): PLAY -> stop on glass left PLXO live=1 for 2+ min: the session's en=0
// took the idle FB off screen behind the planner, and the unchanged S0 frame was NoChange after the
// END, so no en=1. paintIdleR1 re-arms the planner per session END (R1SessionEndRearm): the same
// frame is repainted and flipped, en=1 ACKed and idle_shown=1 within 24 FB_VBLs (400 ms at 60 Hz).
void rowJ() {
    Fb480Model m;
    Daemon d(m);
    misterplex::r1::R1SessionEndRearm rearm;
    std::vector<uint8_t> f(kIdleBytes);
    misterplex::renderIdleRgb24(f.data(), 640, 480, IdleMode::Logo, 0);
    const auto paintIdleR1 = [&] {   // media_player paintIdleR1's planner steps
        if (rearm.takeSessionEnd(d.sessionEnds))
            d.planner.arm();
        d.planner.submitFrame(f);
    };
    paintIdleR1();
    pumpUntil(d, PaintStep::Started, 8, "R-j first paint");
    pumpUntil(d, PaintStep::Acked, 8, "R-j first ack");
    expectIdleShows(d, f, "R-j before PLAY");
    d.sessionWithPicture();
    m.vbl();
    brtest::require(!m.idleSel && m.fbLive, "R-j: the session's en=0 left the idle FB on screen");
    d.normalStop();
    paintIdleR1();
    bool acked = false;
    int vbls = 0;
    for (; vbls < 24 && !acked; ++vbls) {
        const PaintResult r = d.planner.pump(d.target);
        brtest::require(r.step != PaintStep::NoChange,
                        "R-j: the unchanged idle frame was NoChange after the session END (no en=1)");
        acked = r.step == PaintStep::Acked;
        if (!acked)
            m.vbl();
    }
    brtest::require(acked, "R-j: no en=1 ACK within 24 FB_VBLs (400 ms) of the session END");
    expectIdleShows(d, f, "R-j after stop");
    brtest::require(d.target.entryClears() == 0, "R-j: an entry clear after a normal stop");
    std::cout << "R-j PASS PLAY -> stop: the same S0 frame re-shown, en=1 ACKed and idle_sel=1 after " << vbls
              << " FB_VBLs of the session END (<= 24)\n";
}

// R-k (review 06:57 D3): on glass token 17 en=1 landed 1.5 s after the PLAY request, before BEGIN:
// the gate closed only at enterSession. beginPlayRequest (main.cpp: every PLAY bump of playGen) and
// notePlayRequest (every doPlay) close it at the request: an idle repaint in the resolve window flips
// nothing and stores no PLXH en=1. The session's enter takes the hold over; after the END the gate is
// open again. A request that never reaches a session is released by playRequestEnded(gen), else
// after kPlayRequestHoldMs.
void rowK() {
    Fb480Model m;
    Daemon d(m);
    int64_t now = 1000;
    d.policy.setClockMs([&] { return now; });
    uint64_t playGen = 0;
    const auto bump = [&] { return ++playGen; };
    std::vector<uint8_t> f(kIdleBytes);
    misterplex::renderIdleRgb24(f.data(), 640, 480, IdleMode::Logo, 0);
    d.planner.submitFrame(f);
    pumpUntil(d, PaintStep::Started, 8, "R-k idle paint");
    pumpUntil(d, PaintStep::Acked, 8, "R-k idle ack");
    expectIdleShows(d, f, "R-k before PLAY");
    const uint64_t g1 = d.policy.beginPlayRequest(bump);   // the PLAY request (setPlayQueued)
    d.policy.notePlayRequest(g1);                          // its doPlay
    const auto enOneStores = [&] { return std::count(m.events.begin(), m.events.end(), 'R'); };
    const auto storesBefore = enOneStores();
    const uint16_t tokenBefore = d.link.lastToken();
    d.planner.submit(brtest::s3Root());   // the library hides, the idle thread repaints
    for (int i = 0; i < 24; ++i, now += 17) {
        const PaintResult r = d.planner.pump(d.target);
        brtest::require(r.step != PaintStep::Started && r.step != PaintStep::Acked,
                        "R-k: an idle flip between the PLAY request and BEGIN");
        m.vbl();
    }
    uint16_t t = 0;
    std::string why;
    brtest::require(!d.policy.requestIdle(0, &t, &why) && why.find("PLAY request is pending") != std::string::npos,
                    "R-k: requestIdle allowed between the PLAY request and BEGIN (" + why + ")");
    brtest::require(enOneStores() == storesBefore && d.link.lastToken() == tokenBefore,
                    "R-k: a PLXH en=1 store between the PLAY request and BEGIN");
    d.sessionWithPicture(g1);   // enterSession(g1), BEGIN, en=0 ACKed
    brtest::require(!d.policy.playRequestPending(), "R-k: the hold outlived the session's enter");
    d.normalStop();
    pumpUntil(d, PaintStep::Started, 8, "R-k paint after END");
    pumpUntil(d, PaintStep::Acked, 8, "R-k ack after END");
    expectIdleShows(d, brtest::render(brtest::s3Root()), "R-k after END");
    const uint64_t g2 = d.policy.beginPlayRequest(bump);
    brtest::require(d.policy.playRequestPending(), "R-k: no hold after a second PLAY request");
    d.policy.playRequestEnded(g2);   // doPlay ended without a session (stopped, refused, failed)
    brtest::require(!d.policy.playRequestPending(), "R-k: playRequestEnded did not release the hold");
    d.policy.beginPlayRequest(bump);
    now += misterplex::r1::PlxhSessionPolicy::kPlayRequestHoldMs;
    brtest::require(!d.policy.playRequestPending(), "R-k: an orphaned hold does not expire");
    std::cout << "R-k PASS PLAY request -> BEGIN: 24 FB_VBLs of repaint, 0 flips, 0 PLXH en=1 stores; "
                 "the session's enter, playRequestEnded and the 30 s expiry each release the hold\n";
}

// R-l (review 06:57 D1): a refused flip is never terminal. The planner keeps the frame pending, and
// the idle thread retries an owed flip within 250 ms (idleRepaintStepMs), not the 30 s static net.
void rowL() {
    using misterplex::r1::idleRepaintStepMs;
    brtest::require(idleRepaintStepMs(false, true) <= 250,
                    "R-l: an owed flip waits " + std::to_string(idleRepaintStepMs(false, true)) + " ms");
    brtest::require(idleRepaintStepMs(false, false) == 30000, "R-l: the static safety net is not 30 s");
    brtest::require(idleRepaintStepMs(true, false) == 100 && idleRepaintStepMs(true, true) == 100,
                    "R-l: the screensaver step is not 100 ms");
    Fb480Model m;
    Daemon d(m);
    std::vector<uint8_t> f(kIdleBytes);
    misterplex::renderIdleRgb24(f.data(), 640, 480, IdleMode::Logo, 0);
    m.refuseStores = true;   // the start-up store refusal
    d.planner.submitFrame(f);
    const PaintResult refused = d.planner.pump(d.target);
    brtest::require(refused.step == PaintStep::Refused && d.planner.busy(),
                    "R-l: a refused flip left nothing pending to retry");
    m.refuseStores = false;
    pumpUntil(d, PaintStep::Started, 8, "R-l retry");
    pumpUntil(d, PaintStep::Acked, 8, "R-l retry ack");
    expectIdleShows(d, f, "R-l after the retry");
    brtest::require(d.countLogs("r1: idle flip refused: PLXH store refused") == 1, "R-l: the refusal is not logged once");
    std::cout << "R-l PASS a refused store stays pending and the retry flips; idle retry step "
              << idleRepaintStepMs(false, true) << " ms (static 30000, screensaver 100)\n";
}

// R-m (review 07:30 C1): the PLAY-request hold is token-matched. On 818d5780 setPlayQueued noted
// the hold and THEN bumped playGen, and doPlay A's end (R1PlayHold: gen == current, then release)
// could land between B's note and B's increment and wipe B's hold, reopening the D3 window; an older
// session's enter cleared any hold too. Now a PLAY's generation and its hold are one step
// (beginPlayRequest runs the bump under the hold lock) and every release names a generation. Each
// case starts with the idle FB shown and a repaint owed; while B's request is pending, 6 FB_VBLs of
// pumps flip nothing, requestIdle is refused and no PLXH en=1 is stored; B's own end, or B's session
// END, then lets the owed repaint flip and ACK. Mutant MUT_R1_HOLD_RELEASE_ANY (any end releases
// the hold, the pre-C1 rule) must FAIL, in each case alone too (R1_HOLD_CASE=m1..m5): m1/m2 the
// release, m4 the STOP's clear, m5 the session enter.
struct HoldCase {
    Fb480Model m;
    Daemon d{m};
    misterplex::r1::R1SessionEndRearm rearm;
    int64_t now = 1000;
    uint64_t playGen = 0;
    std::string name;
    int frame = 0;

    explicit HoldCase(std::string n) : name(std::move(n)) {
        d.policy.setClockMs([this] { return now; });
        owe();
        pumpUntil(d, PaintStep::Started, 8, "R-m " + name + " idle paint");
        pumpUntil(d, PaintStep::Acked, 8, "R-m " + name + " idle ack");
        owe();   // a repaint is owed from here on
    }
    // media_player paintIdleR1's planner steps: re-arm per session END, then the next idle frame
    // (Logo and Black alternate, so each owed frame differs from the one shown).
    void owe() {
        if (rearm.takeSessionEnd(d.sessionEnds))
            d.planner.arm();
        std::vector<uint8_t> f(kIdleBytes);
        misterplex::renderIdleRgb24(f.data(), 640, 480, (frame++ % 2) ? IdleMode::Black : IdleMode::Logo, 0);
        d.planner.submitFrame(f);
    }
    uint64_t begin() {
        return d.policy.beginPlayRequest([this] { return ++playGen; });
    }
    long enOneStores() const { return static_cast<long>(std::count(m.events.begin(), m.events.end(), 'R')); }
    void expectHeld(uint64_t gen, const std::string& at) {
        const std::string w = "R-m " + name + " (" + at + ")";
        brtest::require(d.policy.playRequestGeneration() == gen,
                        w + ": the hold is generation " + std::to_string(d.policy.playRequestGeneration()) +
                            ", want " + std::to_string(gen));
        const long stores = enOneStores();
        for (int i = 0; i < 6; ++i, now += 17) {
            const PaintResult r = d.planner.pump(d.target);
            brtest::require(r.step != PaintStep::Started && r.step != PaintStep::Acked,
                            w + ": an idle flip while the PLAY request is pending");
            m.vbl();
        }
        uint16_t t = 0;
        std::string why;
        brtest::require(!d.policy.requestIdle(0, &t, &why), w + ": requestIdle allowed while the PLAY request is pending");
        brtest::require(enOneStores() == stores, w + ": a PLXH en=1 store while the PLAY request is pending");
    }
    void expectReleased(const std::string& at) {
        const std::string w = "R-m " + name + " (" + at + ")";
        brtest::require(!d.policy.playRequestPending(), w + ": the hold was not released");
        pumpUntil(d, PaintStep::Started, 8, w + " flip");
        pumpUntil(d, PaintStep::Acked, 8, w + " ack");
    }
};

void rowM() {
    // R1_HOLD_CASE=m2 (test-only) runs one case, so each interleave's mutant failure can be shown.
    const char* onlyEnv = std::getenv("R1_HOLD_CASE");
    const std::string only = onlyEnv ? onlyEnv : "";
    const auto run = [&](const char* name) { return only.empty() || only == name; };
    if (run("m1")) {   // m1: A's end check (gen == current) passes before B's begin; its release lands after it.
        HoldCase c("m1");
        const uint64_t a = c.begin();   // PLAY A (setPlayQueued)
        c.d.policy.notePlayRequest(a);  // A's doPlay start
        const bool aCurrent = a == c.playGen;
        const uint64_t b = c.begin();   // PLAY B lands between A's check and A's release
        if (aCurrent)
            c.d.policy.playRequestEnded(a);
        c.expectHeld(b, "A's release after B's begin");
        c.d.policy.notePlayRequest(b);  // B's doPlay start
        c.d.policy.playRequestEnded(b); // B fails without a session: its own end releases
        c.expectReleased("B's own end");
    }
    if (run("m2")) {   // m2: A's release lands inside B's begin, after B's increment and before B's hold.
        HoldCase c("m2");
        const uint64_t a = c.begin();
        std::atomic<bool> attempting{false}, released{false};
        bool releasedInsideBegin = true;
        std::thread endA;
        const uint64_t b = c.d.policy.beginPlayRequest([&] {
            const uint64_t g = ++c.playGen;   // B's increment: playGen moved, B's hold not yet set
            endA = std::thread([&] {
                attempting.store(true);
                c.d.policy.playRequestEnded(a);   // A's R1PlayHold release
                released.store(true);
            });
            while (!attempting.load())
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            releasedInsideBegin = released.load();
            return g;
        });
        endA.join();
        brtest::require(!releasedInsideBegin,
                        "R-m m2: A's release completed between B's increment and B's hold");
        c.expectHeld(b, "A's release between B's increment and B's hold");
        c.d.policy.playRequestEnded(b);
        c.expectReleased("B's own end");
    }
    if (run("m3")) {   // m3: A's late doPlay start (a stale note), then A's session entering after B's request and
        // ending (D2 re-arm owed): B's hold survives both; B's session takes it over at its enter.
        HoldCase c("m3");
        const uint64_t a = c.begin();
        const uint64_t b = c.begin();   // B before A's doPlay even ran
        c.d.policy.notePlayRequest(a);  // A's doPlay start: stale
        c.expectHeld(b, "A's stale note after B's begin");
        c.d.policy.playRequestEnded(a); // A's doPlay ends, superseded
        c.expectHeld(b, "A's end after its stale note");
        c.d.sessionWithPicture(a);      // A passed its last check before B's begin; its thread enters later
        brtest::require(c.d.policy.playRequestGeneration() == b, "R-m m3: A's session enter took B's hold");
        c.d.normalStop();               // A's END: the D2 re-arm makes a repaint owed
        c.owe();
        c.expectHeld(b, "A's session enter and END after B's begin");
        c.d.sessionWithPicture(b);
        brtest::require(!c.d.policy.playRequestPending(), "R-m m3: B's session enter did not take B's hold over");
        c.d.normalStop();
        c.owe();
        c.expectReleased("B's session END");
    }
    if (run("m4")) {   // m4: a STOP ends the holds before it, not a PLAY begun after it; a late note stays stale.
        HoldCase c("m4");
        const uint64_t a = c.begin();
        c.d.policy.notePlayRequest(a);
        ++c.playGen;                    // STOP: TransportGenerations::accept bumps playGen
        const uint64_t b = c.begin();   // PLAY B before the STOP handler's clear() runs
        c.d.policy.playRequestsEndedBefore(c.playGen);   // clear(): r1PlayRequestsEndedBefore(playGen)
        c.expectHeld(b, "a STOP's clear after a newer PLAY");
        c.d.policy.playRequestEnded(b);
        c.expectReleased("B's own end");
        c.owe();
        const uint64_t a2 = c.begin();
        ++c.playGen;                    // STOP during A2's resolve
        c.d.policy.playRequestsEndedBefore(c.playGen);
        c.expectReleased("a STOP during A2's resolve");
        c.d.policy.notePlayRequest(a2); // A2's doPlay starts after the STOP: stale
        brtest::require(!c.d.policy.playRequestPending(), "R-m m4: a stale note after the STOP re-armed the hold");
    }
    if (run("m5")) {   // m5: the enter path alone. A's doPlay started its session (so it releases
        // nothing) and A's thread enters after B's begin: the enter takes over A's generation only;
        // A's END re-arms a repaint that waits for B; B's session enter takes B's hold over.
        HoldCase c("m5");
        const uint64_t a = c.begin();
        c.d.policy.notePlayRequest(a);
        const uint64_t b = c.begin();
        c.d.sessionWithPicture(a);
        brtest::require(c.d.policy.playRequestGeneration() == b, "R-m m5: A's session enter took B's hold");
        c.d.normalStop();
        c.owe();
        c.expectHeld(b, "A's session enter and END after B's begin");
        c.d.sessionWithPicture(b);
        brtest::require(!c.d.policy.playRequestPending(), "R-m m5: B's session enter did not take B's hold over");
        c.d.normalStop();
        c.owe();
        c.expectReleased("B's session END");
    }
    using misterplex::r1::PlayRequestHold;
    if (run("m6")) {   // m6 (review 08:17 (1)): a STOP clears with its OWN generation. PLAY A, the
        // STOP is accepted, PLAY B, a seek on B bumps playGen before its replay notes; then the STOP
        // handler's clear() runs: B's hold must survive (with the live playGen it would not).
        HoldCase c("m6");
        std::atomic<uint64_t> play{0}, seek{0};
        misterplex::TransportGenerations gens{play, seek};
        const auto bump = [&] { return ++play; };
        const uint64_t a = c.d.policy.beginPlayRequest(bump);
        c.d.policy.notePlayRequest(a);
        const misterplex::TransportRequest stop = gens.accept(misterplex::TransportCommand::Stop, 0);
        const uint64_t b = c.d.policy.beginPlayRequest(bump);
        (void)gens.accept(misterplex::TransportCommand::Seek, 0);   // replay not yet noted
        c.d.policy.playRequestsEndedBefore(misterplex::stopClearGeneration(stop, gens));   // clear()
        c.expectHeld(b, "the STOP's clear after a PLAY and a seek");
        c.d.policy.notePlayRequest(play.load());   // the seek's replay doPlay
        c.d.policy.playRequestEnded(play.load());
        c.expectReleased("the replay's own end");
    }
    if (run("m7")) {   // m7 (08:17 (2)): a doPlay that owns the hold keeps it through a 45 s resolve
        // and releases it on exit; a doPlay replaced by a seek's replay only disowns it, and that
        // orphan expires kPlayRequestHoldMs later.
        HoldCase c("m7");
        const uint64_t a = c.begin();
        {
            PlayRequestHold h(c.d.policy, a, [] { return true; });
            c.now += 45000;   // the resolve: 45 s, past the orphan expiry
            c.expectHeld(a, "45 s into its own doPlay's resolve");
        }
        c.expectReleased("the doPlay's exit after 45 s");
        c.owe();
        const uint64_t b = c.begin();
        {
            PlayRequestHold h(c.d.policy, b, [] { return false; });   // superseded by a seek's replay
            c.now += 45000;
        }
        c.now += misterplex::r1::PlxhSessionPolicy::kPlayRequestHoldMs - 1000;
        c.expectHeld(b, "an orphan 29 s after its doPlay's exit");
        c.now += 2000;
        c.expectReleased("an orphan past kPlayRequestHoldMs");
    }
    if (run("m8")) {   // m8 (08:17 (2)): a throw inside doPlay releases the hold (RAII).
        HoldCase c("m8");
        const uint64_t a = c.begin();
        bool caught = false;
        try {
            PlayRequestHold h(c.d.policy, a, [] { return true; });
            c.expectHeld(a, "inside doPlay before the throw");
            throw std::runtime_error("resolve threw");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        brtest::require(caught, "R-m m8: the throw was not caught");
        c.expectReleased("a throw inside doPlay");
    }
    if (run("m9")) {   // m9 (08:17 C1): the library overlay play bumps playGen twice (the request, then
        // doPlay's own bump 50 ms later); the hold moves to the final generation with no gap.
        HoldCase c("m9");
        const uint64_t lib = c.begin();   // setLibraryPlay: the request's bump
        c.expectHeld(lib, "the library request, before its doPlay");
        const uint64_t own = c.begin();   // doPlay (dispatchGeneration 0): the final bump
        brtest::require(own == lib + 1, "R-m m9: the final bump is not the next generation");
        c.expectHeld(own, "the library doPlay's own bump");
        {
            PlayRequestHold h(c.d.policy, own, [] { return true; });
            c.d.policy.playRequestEnded(lib);   // a stale end for the first generation
            c.expectHeld(own, "a stale end for the request's first generation");
        }
        c.expectReleased("the library doPlay's exit");
    }
    if (run("m10")) {   // m10: a doPlay that started its session only disowns the hold at exit; the
        // session's enter takes it over, and the END re-shows the idle FB.
        HoldCase c("m10");
        const uint64_t a = c.begin();
        {
            PlayRequestHold h(c.d.policy, a, [] { return true; });
            h.started();
        }
        c.expectHeld(a, "after a started doPlay's exit, before its session's enter");
        c.d.sessionWithPicture(a);
        brtest::require(!c.d.policy.playRequestPending(), "R-m m10: the session's enter did not take the hold over");
        c.d.normalStop();
        c.owe();
        c.expectReleased("the session END");
    }
    if (!only.empty()) {
        std::cout << "R-m PASS case " << only << "\n";
        return;
    }
    std::cout << "R-m PASS token-matched hold: A's release after B's begin (m1) or inside it (m2, waited "
                 "for the hold lock), A's stale note, A's session enter and END (m3), a STOP's clear after "
                 "B (m4), A's session enter alone (m5), the STOP's own generation (m6), an owned 45 s "
                 "resolve and an orphan's expiry (m7), a throw (m8), the library double bump (m9), a "
                 "started doPlay's exit (m10)\n";
}

// R-f1 (F1, PLEX_IDLE_FB_240P; fpga_build_ids.hpp idleBeatPlxl): R-b's scene on an F1 daemon. A
// latched en=1 with idle_shown 0 is the 240p core's beat hysteresis (the FB stays off until the core
// qualifies the PLXL beat), not a decoded picture owning scan-out: no ring epoch reset, no clear line,
// and the flip waits (ACK timeout; the planner retries). MUT_F1_R1_ENTRY_CLEAR must FAIL here.
void rowF1() {
    Fb480Model m;
    oldDaemonDiesMidSession(m);
    Daemon d(m, /*f1=*/true);
    m.vbl();
    brtest::require(m.fbLive && !m.idleSel, "R-f1: the scene has no latched-not-shown state");
    d.planner.submit(brtest::s3Root());
    pumpUntil(d, PaintStep::Started, 8, "R-f1 paint");
    pumpUntil(d, PaintStep::AckTimeout, 8, "R-f1 ack timeout");
    brtest::require(d.target.entryClears() == 0 && m.epochResets == 0 && d.countLogs("idle entry clear") == 0,
                    "R-f1: an F1 row issued an entry clear");
    std::cout << "R-f1 PASS F1 row: latched-not-shown -> no entry clear (epoch resets 0), the flip waits\n";
}

}  // namespace

int main() {
    try {
        if (std::getenv("R1_HOLD_CASE")) {   // one R-m case only (test-only filter)
            rowM();
            return 0;
        }
        rowA();
        rowB();
        rowB2();
        rowC();
        rowD();
        rowE();
        rowF();
        rowG();
        rowH();
        rowI();
        rowS0();
        rowJ();
        rowK();
        rowL();
        rowM();
        rowF1();
        std::cout << "R1 ENTRY CLEAR PASS R-a R-b R-b2 R-c R-d R-e R-f R-g R-h R-i S0 R-j R-k R-l R-m\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "R1 ENTRY CLEAR FAIL: " << e.what() << "\n";
        return 1;
    }
}
