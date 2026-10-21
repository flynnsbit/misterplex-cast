#pragma once
// SPS-gate layer 3 (L3 paper v1 c1f55425; review 06:17 CLEAR TO CODE): FpgaPlayGate, the ONE decision owner of
// pre-reg v1.4.3 b57a87fb (B13). It turns the loaded core's CAPS verdict (gate::evaluateCaps) and the per-AU stream
// verdict (CompressedSyntax::inspect) into one action. Pure: no I/O, no daemon state, so every row is a unit tooth.
//
// Rows (L3 paper section 2, with Q1-Q4 as ruled by review 06:17):
//   R1   CAPS NoPlay (section 1 (i))                          -> Error on screen; no play, no fallback
//   R2   CAPS FallbackOnFloor, the item's first play, helper   -> Fallback, before the demux opens
//   R3   CAPS FallbackOnFloor, the item's fallback play        -> GateOnFloor: gate the helper stream on the floor
//   R4   CAPS Ok                                               -> GateOnCaps
//   R5   stream IN                                             -> Feed
//   R6   stream OUT or MALFORMED, 0 AUs written, first play, helper -> Fallback (MALFORMED routes like OUT, Q3)
//   R7   R6 or R2 with no helper configured                    -> Error on screen. Q2: the same verdict falls back
//                                                                 as soon as a helper is configured
//   R8   stream OUT or MALFORMED, 0 AUs written, fallback play -> Error: never a second fallback, never direct
//   R9   stream OUT or MALFORMED after an AU was written       -> StopLogged: today's stop plus the GATE line. L4
//                                                                 owns the mid-item flip (Q1)
//   R10  a non-gate failure (demux open, transport, HTTP)      -> Fail: today's stop, unchanged
//   R10h R10 on the item's fallback play                       -> Error on screen; never a direct retry (T7a)
// "The fallback play" in R3 and R8 also covers a first play whose resolved stream ALREADY comes from the ladder helper
// (helperStream: resolve built /ladder.ts, e.g. ladder480 on a 480p/480i conf with a helper). Falling back from the
// helper's stream would replay the same ladder, so it is R3/R8 (code-packet finding from L3 bar b1).
#include "libmisterplex/gate_caps.hpp"

#include <cstdint>
#include <string>

namespace misterplex::gate {

enum class GateRow { R1, R2, R3, R4, R5, R6, R6p, R7, R8, R9, R10, R10h };

enum class GateAction {
    GateOnCaps,   // R4
    GateOnFloor,  // R3
    Feed,         // R5
    Fallback,     // R2, R6: end this play with 0 AUs written; re-resolve the SAME item once, forceLadder
    Error,        // R1, R7, R8, R10h: the on-screen error, then the play ends
    StopLogged,   // R9
    Fail,         // R10
};

struct GateDecision {
    GateAction action;
    GateRow row;
};

// Per play request. fallbackUsed: this play IS the item's one fallback (forceLadder). helperStream: the resolved
// stream already comes from the ladder helper. helperConfigured: a ladder helper is configured
// (MPX_REMOTE_LADDER_PROXY, else REMOTE_LADDER_PROXY), the same read resolve makes.
struct GatePlayContext {
    bool fallbackUsed = false;
    bool helperStream = false;
    bool helperConfigured = false;
    // NO-REQUIRED-PROFILE: the stream is a PMS universal transcode that has not had its one lowered retry
    // (row R6p: the same item re-requested with lowerPmsLadder, when no ladder helper is configured).
    bool pmsRetryAvailable = false;
};

// The stream being gated is the ladder helper's: a second fallback would replay the same ladder.
inline bool onHelperStream(const GatePlayContext& ctx) {
    return ctx.fallbackUsed || ctx.helperStream;
}

inline const char* rowName(GateRow r) {
    switch (r) {
    case GateRow::R1: return "R1";
    case GateRow::R2: return "R2";
    case GateRow::R3: return "R3";
    case GateRow::R4: return "R4";
    case GateRow::R5: return "R5";
    case GateRow::R6: return "R6";
    case GateRow::R6p: return "R6p";
    case GateRow::R7: return "R7";
    case GateRow::R8: return "R8";
    case GateRow::R9: return "R9";
    case GateRow::R10: return "R10";
    case GateRow::R10h: return "R10h";
    }
    return "R?";
}

inline const char* actionName(GateAction a) {
    switch (a) {
    case GateAction::GateOnCaps: return "gate-on-caps";
    case GateAction::GateOnFloor: return "gate-on-floor";
    case GateAction::Feed: return "feed";
    case GateAction::Fallback: return "fallback";
    case GateAction::Error: return "error";
    case GateAction::StopLogged: return "stop-logged";
    case GateAction::Fail: return "fail";
    }
    return "?";
}

inline const char* capsOutcomeName(CapsOutcome o) {
    return o == CapsOutcome::Ok ? "Ok" : o == CapsOutcome::FallbackOnFloor ? "FallbackOnFloor" : "NoPlay";
}

// The CAPS decision, once per play, after the build refusal and before the demux opens.
inline GateDecision decideCaps(CapsOutcome caps, const GatePlayContext& ctx) {
    if (caps == CapsOutcome::NoPlay)
        return {GateAction::Error, GateRow::R1};
    if (caps == CapsOutcome::FallbackOnFloor) {
        if (onHelperStream(ctx))
            return {GateAction::GateOnFloor, GateRow::R3};
        if (ctx.helperConfigured)
            return {GateAction::Fallback, GateRow::R2};
        return {GateAction::Error, GateRow::R7};
    }
    return {GateAction::GateOnCaps, GateRow::R4};
}

// The stream decision, per AU, before that AU's ring write. auWritten: AccessUnit records already written this play.
inline GateDecision decideStream(StreamOutcome s, uint64_t auWritten, const GatePlayContext& ctx) {
    if (s == StreamOutcome::In)
        return {GateAction::Feed, GateRow::R5};
    if (auWritten > 0)
        return {GateAction::StopLogged, GateRow::R9};
    if (onHelperStream(ctx))
        return {GateAction::Error, GateRow::R8};
    if (ctx.helperConfigured)
        return {GateAction::Fallback, GateRow::R6};
#ifndef MUT_NO_LOWERED_RETRY   // test-only mutant: a refused PMS stream errors at once (no lowered retry)
    if (ctx.pmsRetryAvailable)
        return {GateAction::Fallback, GateRow::R6p};
#endif
    return {GateAction::Error, GateRow::R7};
}

// A non-gate failure: the demux open, an HTTP read, or the transport.
inline GateDecision decideFailure(const GatePlayContext& ctx) {
    if (ctx.fallbackUsed)
        return {GateAction::Error, GateRow::R10h};
    return {GateAction::Fail, GateRow::R10};
}

// "<field> <value> (limit <limit>)": neutral wording, because limit is a maximum for a <= rule and the required
// value for an == rule; -1 marks a set-membership rule.
inline std::string verdictPhrase(const StreamVerdict& v) {
    if (v.outcome == StreamOutcome::Malformed)
        return std::string("malformed ") + v.field + (v.why[0] != '\0' ? std::string(" (") + v.why + ")" : "");
    std::string s = std::string(v.field) + " " + std::to_string(v.value);
    s += v.limit < 0 ? std::string(" not supported") : " (limit " + std::to_string(v.limit) + ")";
    if (v.why[0] != '\0')
        s += std::string(": ") + v.why;
    return s;
}

inline std::string streamLogLine(const StreamVerdict& v, GateDecision d, uint32_t features, uint32_t buildId,
                                 uint64_t auWritten, uint64_t auBytes) {
    return std::string("GATE ") + streamOutcomeName(v.outcome) + " field=" + v.field +
           " value=" + std::to_string(v.value) + " limit=" + std::to_string(v.limit) +
           " why=" + (v.why[0] != '\0' ? v.why : "-") + " caps=" + detail::hex32(features) +
           " build=" + detail::hex32(buildId) + " au_written=" + std::to_string(auWritten) +
           " au_bytes=" + std::to_string(auBytes) + " row=" + rowName(d.row) + " action=" + actionName(d.action);
}

inline std::string capsLogLine(const CapsVerdict& c, GateDecision d, uint32_t features, uint32_t buildId) {
    return std::string("GATE CAPS ") + capsOutcomeName(c.outcome) + " row=" + rowName(d.row) +
           " action=" + actionName(d.action) + " caps=" + detail::hex32(features) + " build=" +
           detail::hex32(buildId) + " detail=" + (c.detail.empty() ? "-" : c.detail);
}

// The reason resolve prints after "gate fallback: " (forceLadder r.detail, T1f).
inline std::string fallbackReason(const StreamVerdict& v) {
    return verdictPhrase(v);
}

inline std::string fallbackReason(const CapsVerdict& c) {
    return c.detail;
}

// The ONE error text: the painter, lastError_ and the terminal player_error= line all carry it.
inline std::string errorText(GateRow row, const std::string& reason, uint32_t buildId) {
    const std::string core = " (core " + detail::hex32(buildId) + ")";
    switch (row) {
    case GateRow::R7:
        return "Can't play here: " + reason + core + ". No ladder helper configured.";
    case GateRow::R8:
        return "Can't play here: the ladder helper's stream has " + reason + core + ".";
    case GateRow::R10h:
        return "Can't play here: the ladder helper failed: " + reason + core + ".";
    default:
        return "Can't play here: " + reason + core + ".";
    }
}

} // namespace misterplex::gate
