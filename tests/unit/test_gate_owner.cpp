// SPS-gate layer 3 (L3 paper v1 c1f55425; review 06:17 CLEAR TO CODE): unit rows R1-R10h of gate::decideCaps,
// gate::decideStream and gate::decideFailure (host/libmisterplex/gate_owner.hpp), with a NEG for every row (each row
// is checked in every context, and only its own row may come out), the Q2 pair, the Q3 equivalence, and the shared
// strings. Pure: no daemon, no fake core. The rows run in a fixed order, so a mutant's first FAIL line is
// deterministic (tests/unit/test_gate_owner_mutants.sh pins it).
#include "libmisterplex/gate_owner.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace misterplex;
using gate::CapsOutcome;
using gate::GateAction;
using gate::GateRow;
using gate::StreamOutcome;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL gate owner: %s\n", msg.c_str());
        ++fails;
    }
}

std::string show(GateRow r, GateAction a) {
    return std::string(gate::rowName(r)) + "/" + gate::actionName(a);
}

void row(const std::string& label, gate::GateDecision got, GateRow wantRow, GateAction wantAction) {
    expect(got.row == wantRow && got.action == wantAction,
           label + ": want " + show(wantRow, wantAction) + ", got " + show(got.row, got.action));
}

struct Ctx {
    const char* name;
    gate::GatePlayContext ctx;
};

const Ctx kFirstHelper{"first play + helper", {false, false, true}};
const Ctx kFirstNoHelper{"first play, no helper", {false, false, false}};
const Ctx kFallbackHelper{"fallback play + helper", {true, false, true}};
const Ctx kFallbackNoHelper{"fallback play, no helper", {true, false, false}};
// A first play whose resolved stream already comes from the helper (/ladder.ts; ladder480 on a 480p conf).
const Ctx kHelperStream{"first play on the helper's stream", {false, true, true}};
const Ctx kAll[] = {kFirstHelper, kFirstNoHelper, kFallbackHelper, kFallbackNoHelper, kHelperStream};

gate::StreamVerdict verdict(StreamOutcome o, const char* field, int64_t value, int64_t limit, const char* why = "") {
    gate::StreamVerdict v;
    v.outcome = o;
    v.field = field;
    v.value = value;
    v.limit = limit;
    v.why = why;
    return v;
}

} // namespace

int main() {
    // --- CAPS rows ---
    for (const auto& c : kAll)
        row(std::string("R1 ") + c.name + ", CAPS NoPlay", gate::decideCaps(CapsOutcome::NoPlay, c.ctx), GateRow::R1,
            GateAction::Error);
    row("R2 first play + helper, CAPS FallbackOnFloor", gate::decideCaps(CapsOutcome::FallbackOnFloor, kFirstHelper.ctx),
        GateRow::R2, GateAction::Fallback);
    row("R7 first play, no helper, CAPS FallbackOnFloor",
        gate::decideCaps(CapsOutcome::FallbackOnFloor, kFirstNoHelper.ctx), GateRow::R7, GateAction::Error);
    row("R3 fallback play + helper, CAPS FallbackOnFloor",
        gate::decideCaps(CapsOutcome::FallbackOnFloor, kFallbackHelper.ctx), GateRow::R3, GateAction::GateOnFloor);
    row("R3 fallback play, no helper, CAPS FallbackOnFloor",
        gate::decideCaps(CapsOutcome::FallbackOnFloor, kFallbackNoHelper.ctx), GateRow::R3, GateAction::GateOnFloor);
    row("R3 first play on the helper's stream, CAPS FallbackOnFloor",
        gate::decideCaps(CapsOutcome::FallbackOnFloor, kHelperStream.ctx), GateRow::R3, GateAction::GateOnFloor);
    for (const auto& c : kAll)
        row(std::string("R4 ") + c.name + ", CAPS Ok", gate::decideCaps(CapsOutcome::Ok, c.ctx), GateRow::R4,
            GateAction::GateOnCaps);

    // --- stream rows ---
    for (const auto& c : kAll)
        for (uint64_t au : {uint64_t{0}, uint64_t{5}})
            row(std::string("R5 ") + c.name + ", IN after " + std::to_string(au) + " AU",
                gate::decideStream(StreamOutcome::In, au, c.ctx), GateRow::R5, GateAction::Feed);
    row("R6 first play + helper, OUT", gate::decideStream(StreamOutcome::Out, 0, kFirstHelper.ctx), GateRow::R6,
        GateAction::Fallback);
    row("R6 first play + helper, MALFORMED", gate::decideStream(StreamOutcome::Malformed, 0, kFirstHelper.ctx),
        GateRow::R6, GateAction::Fallback);
    row("R7 first play, no helper, OUT", gate::decideStream(StreamOutcome::Out, 0, kFirstNoHelper.ctx), GateRow::R7,
        GateAction::Error);
    row("R7 first play, no helper, MALFORMED", gate::decideStream(StreamOutcome::Malformed, 0, kFirstNoHelper.ctx),
        GateRow::R7, GateAction::Error);
    for (const Ctx* c : {&kFallbackHelper, &kFallbackNoHelper, &kHelperStream}) {
        row(std::string("R8 ") + c->name + ", OUT", gate::decideStream(StreamOutcome::Out, 0, c->ctx), GateRow::R8,
            GateAction::Error);
        row(std::string("R8 ") + c->name + ", MALFORMED", gate::decideStream(StreamOutcome::Malformed, 0, c->ctx),
            GateRow::R8, GateAction::Error);
    }
    for (const auto& c : kAll) {
        row(std::string("R9 ") + c.name + ", OUT after 1 AU", gate::decideStream(StreamOutcome::Out, 1, c.ctx),
            GateRow::R9, GateAction::StopLogged);
        row(std::string("R9 ") + c.name + ", MALFORMED after 1 AU",
            gate::decideStream(StreamOutcome::Malformed, 1, c.ctx), GateRow::R9, GateAction::StopLogged);
    }

    // --- non-gate failures ---
    row("R10 first play + helper", gate::decideFailure(kFirstHelper.ctx), GateRow::R10, GateAction::Fail);
    row("R10 first play, no helper", gate::decideFailure(kFirstNoHelper.ctx), GateRow::R10, GateAction::Fail);
    // L3 scope: R10h is the fallback play only; a helper-stream first play keeps today's R10 (no new paint).
    row("R10 first play on the helper's stream", gate::decideFailure(kHelperStream.ctx), GateRow::R10, GateAction::Fail);
    row("R10h fallback play + helper", gate::decideFailure(kFallbackHelper.ctx), GateRow::R10h, GateAction::Error);
    row("R10h fallback play, no helper", gate::decideFailure(kFallbackNoHelper.ctx), GateRow::R10h, GateAction::Error);

    // --- Q2 pair (review 06:17): the SAME verdict is an on-screen error with no helper and falls back with one ---
    {
        const auto none = gate::decideStream(StreamOutcome::Out, 0, kFirstNoHelper.ctx);
        const auto helper = gate::decideStream(StreamOutcome::Out, 0, kFirstHelper.ctx);
        expect(none.action == GateAction::Error && helper.action == GateAction::Fallback,
               "Q2 pair: OUT at AU0 must be Error without a helper and Fallback with one");
        const auto capsNone = gate::decideCaps(CapsOutcome::FallbackOnFloor, kFirstNoHelper.ctx);
        const auto capsHelper = gate::decideCaps(CapsOutcome::FallbackOnFloor, kFirstHelper.ctx);
        expect(capsNone.action == GateAction::Error && capsHelper.action == GateAction::Fallback,
               "Q2 pair: CAPS FallbackOnFloor must be Error without a helper and Fallback with one");
        const auto onHelper = gate::decideStream(StreamOutcome::Out, 0, kHelperStream.ctx);
        expect(onHelper.action == GateAction::Error && onHelper.row == GateRow::R8,
               "helper stream: OUT at AU0 on the helper's own stream must be R8, never a fallback to the same ladder");
    }

    // --- Q3 (review 06:17): MALFORMED routes exactly like OUT, in every context, at AU0 and after it ---
    for (const auto& c : kAll)
        for (uint64_t au : {uint64_t{0}, uint64_t{1}}) {
            const auto o = gate::decideStream(StreamOutcome::Out, au, c.ctx);
            const auto m = gate::decideStream(StreamOutcome::Malformed, au, c.ctx);
            expect(o.row == m.row && o.action == m.action,
                   std::string("Q3 ") + c.name + " after " + std::to_string(au) + " AU: MALFORMED " +
                       show(m.row, m.action) + " != OUT " + show(o.row, o.action));
        }

    // --- the shared strings: one text for the log, lastError_, the painter and resolve's detail ---
    {
        const auto refs = verdict(StreamOutcome::Out, "max_num_ref_frames", 3, 1);
        const auto p3 = verdict(StreamOutcome::Out, "num_ref_idx_active", 2, 1, "active > max_num_ref_frames");
        const auto set = verdict(StreamOutcome::Out, "slice_type", 1, -1);
        const auto bad = verdict(StreamOutcome::Malformed, "sps", 0, 0, "truncated");
        expect(gate::verdictPhrase(refs) == "max_num_ref_frames 3 (limit 1)", "phrase refs: " + gate::verdictPhrase(refs));
        expect(gate::verdictPhrase(p3) == "num_ref_idx_active 2 (limit 1): active > max_num_ref_frames",
               "phrase p3: " + gate::verdictPhrase(p3));
        expect(gate::verdictPhrase(set) == "slice_type 1 not supported", "phrase set: " + gate::verdictPhrase(set));
        expect(gate::verdictPhrase(bad) == "malformed sps (truncated)", "phrase malformed: " + gate::verdictPhrase(bad));
        const auto r6 = gate::decideStream(StreamOutcome::Out, 0, kFirstHelper.ctx);
        const std::string log = gate::streamLogLine(refs, r6, 0x0000E1BFu, 0x48005D52u, 0, 0);
        expect(log == "GATE OUT field=max_num_ref_frames value=3 limit=1 why=- caps=0x0000E1BF build=0x48005D52 "
                      "au_written=0 au_bytes=0 row=R6 action=fallback",
               "stream log line: " + log);
        const auto r9 = gate::decideStream(StreamOutcome::Malformed, 2, kFirstHelper.ctx);
        const std::string log9 = gate::streamLogLine(bad, r9, 0x0000E1BFu, 0x48005D52u, 2, 14382);
        expect(log9 == "GATE MALFORMED field=sps value=0 limit=0 why=truncated caps=0x0000E1BF build=0x48005D52 "
                       "au_written=2 au_bytes=14382 row=R9 action=stop-logged",
               "stream log line R9: " + log9);
        gate::CapsVerdict cv;
        cv.outcome = CapsOutcome::FallbackOnFloor;
        cv.detail = "gate caps: unknown DCAP bits 0x40000000";
        const auto r2 = gate::decideCaps(cv.outcome, kFirstHelper.ctx);
        const std::string clog = gate::capsLogLine(cv, r2, 0x4000E1BFu, 0x48005D52u);
        expect(clog == "GATE CAPS FallbackOnFloor row=R2 action=fallback caps=0x4000E1BF build=0x48005D52 "
                       "detail=gate caps: unknown DCAP bits 0x40000000",
               "caps log line: " + clog);
        expect(gate::fallbackReason(refs) == "max_num_ref_frames 3 (limit 1)", "fallback reason (stream)");
        expect(gate::fallbackReason(cv) == "gate caps: unknown DCAP bits 0x40000000", "fallback reason (caps)");
        const std::string e7 = gate::errorText(GateRow::R7, gate::verdictPhrase(refs), 0x48005D52u);
        expect(e7 == "Can't play here: max_num_ref_frames 3 (limit 1) (core 0x48005D52). No ladder helper configured.",
               "error text R7: " + e7);
        const std::string e8 = gate::errorText(GateRow::R8, gate::verdictPhrase(refs), 0x48005D52u);
        expect(e8 == "Can't play here: the ladder helper's stream has max_num_ref_frames 3 (limit 1) (core 0x48005D52).",
               "error text R8: " + e8);
        const std::string e10 = gate::errorText(GateRow::R10h, "connection refused", 0x48005D52u);
        expect(e10 == "Can't play here: the ladder helper failed: connection refused (core 0x48005D52).",
               "error text R10h: " + e10);
        const std::string e1 = gate::errorText(GateRow::R1, "gate caps (i): supportsVideo false", 0x48005D52u);
        expect(e1 == "Can't play here: gate caps (i): supportsVideo false (core 0x48005D52).", "error text R1: " + e1);
    }

    if (fails) {
        std::fprintf(stderr, "test_gate_owner: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_gate_owner: OK (R1-R10h with NEGs, Q2 pair, Q3, strings)\n");
    return 0;
}
