// fr-resume-1025: the remote (WAN) PMS transcode start probe, as pure logic.
//
// Glass 1025 ( lab, reviewer): "remote start probe ms=24101 http=200 bytes=0" = the probe's 12 s status GET
// plus its 12 s body GET, then "remote universal start dead" and a fall to the multi-GB direct Part. The
// repeat of the same play from idle reached first picture +31.7 s after playMedia, so a COLD remote
// transcoder needs longer than the 24 s the probe gave it [inf: the cause of the dead start is not proven;
// stale state from the YouTube session was looked for in the log and not found]. The probe now retries
// inside a budget, and an abandoned start is stopped on the server it started on.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace misterplex {
namespace remote_start {

constexpr int kStepSeconds = 12;       // one attempt's GET timeout, as before
constexpr int kDefaultBudgetS = 48;    // MPX_REMOTE_START_BUDGET_S default
constexpr int kMinBudgetS = 12;
constexpr int kMaxBudgetS = 120;
#ifdef MUT_RSP_NO_PAUSE   // test-only mutant (review M3): no pause between retries = a probe flood
constexpr int kRetryPauseMs = 0;
#else
constexpr int kRetryPauseMs = 500;   // pinned by test_remote_start_probe B2 (literal 500, not this name)
#endif
constexpr int kMaxProbeCapKb = 4096;

struct Attempt {
    int http = 0;          // 0 = no answer (timeout / connect failure)
    size_t bytes = 0;
    bool alive = false;    // a usable start: ok status, not 204, a media-sized body
};

// MPX_REMOTE_START_BUDGET_S: 12..120, a bad value keeps the default.
inline int budgetSeconds(const char* env) {
#ifdef MUT_RSP_NO_BUDGET
    (void)env;
    return kStepSeconds;     // mutant: the old single attempt
#endif
    if (!env || !*env)
        return kDefaultBudgetS;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end == env || *end != '\0' || v < kMinBudgetS || v > kMaxBudgetS)
        return kDefaultBudgetS;
    return static_cast<int>(v);
}

// MPX_REMOTE_PROBE_CAP_KB (rs9): unset, empty or invalid => kDefaultProbeCapKb: one byte-capped GET that returns
// at headers + 64 KiB of media. "0" => the legacy drain probe (12 s each GET; 24 s with MPX_PREWARM=0), kept as
// the rollback knob. strtol(..., 10): leading zeros are decimal ("007" -> 7, "00" -> 0). Clamp to 4096 KiB.
constexpr int kDefaultProbeCapKb = 64;
inline int probeCapKb(const char* env) {
#ifdef MUT_RS9_CAP_DEFAULT_OFF   // test-only mutant: the PR255 default (unset = legacy 24 s probe)
    if (!env || !*env)
        return 0;
#endif
    if (!env || !*env)
        return kDefaultProbeCapKb;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
#ifdef MUT_RS9_NEG_CAP   // test-only mutant (review M1): a negative cap passes through
    if (end == env || *end != '\0')
        return kDefaultProbeCapKb;
#else
    // A negative value is invalid (not the rollback): size_t(-1) * 1024 would be a near-endless "cap".
    if (end == env || *end != '\0' || v < 0)
        return kDefaultProbeCapKb;
#endif
#ifdef MUT_PROBE_NO_CLAMP
    return static_cast<int>(v);
#else
    if (v > kMaxProbeCapKb)
        return kMaxProbeCapKb;
    return static_cast<int>(v);
#endif
}

// curl -s -D - (headers then body, no -w): pick status from the last non-1xx/non-redirect block.
struct CurlHeaderDump {
    int httpStatus = 0;
    std::string body;
};

inline bool parseHttpStatusAt(const std::string& raw, size_t blockStart, int& codeOut) {
    size_t lineEnd = raw.find('\n', blockStart);
    if (lineEnd == std::string::npos)
        lineEnd = raw.size();
    std::string line = raw.substr(blockStart, lineEnd - blockStart);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.size() < 12 || line.compare(0, 5, "HTTP/") != 0)
        return false;
    const size_t sp = line.find(' ');
    if (sp == std::string::npos || sp + 3 >= line.size())
        return false;
    const char* p = line.c_str() + sp + 1;
    for (int i = 0; i < 3; ++i) {
        if (p[i] < '0' || p[i] > '9')
            return false;
    }
    if (p[3] != ' ' && p[3] != '\0')
        return false;
    codeOut = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    return true;
}

inline bool skipProbeHeaderBlock(int code) {
    return (code >= 100 && code < 200) || (code >= 300 && code < 400);
}

inline size_t headerBodyOffset(const std::string& raw, size_t blockStart) {
    const size_t crlf = raw.find("\r\n\r\n", blockStart);
    if (crlf != std::string::npos)
        return crlf + 4;
    const size_t lf = raw.find("\n\n", blockStart);
    if (lf != std::string::npos)
        return lf + 2;
    return std::string::npos;
}

inline CurlHeaderDump parseCurlHeaderDump(const std::string& raw) {
    CurlHeaderDump out;
    struct Block {
        size_t start;
        int code;
    };
    std::vector<Block> blocks;
    if (raw.size() < 5 || raw.compare(0, 5, "HTTP/") != 0)
        return out;
    for (size_t b = 0; b != std::string::npos;) {
        int code = 0;
        if (!parseHttpStatusAt(raw, b, code))
            break;
        blocks.push_back({b, code});
        const size_t bodyAt = headerBodyOffset(raw, b);
        if (bodyAt == std::string::npos)
            break;
        if (bodyAt + 5 <= raw.size() && raw.compare(bodyAt, 5, "HTTP/") == 0)
            b = bodyAt;
        else
            break;
    }
    if (blocks.empty())
        return out;
#ifdef MUT_PROBE_FIRST_BLOCK
    const Block pick = blocks.front();
#else
    Block pick{};
    bool have = false;
    for (const Block& bl : blocks) {
        if (skipProbeHeaderBlock(bl.code))
            continue;
        pick = bl;
        have = true;
    }
    if (!have)
        return out;
#endif
    const size_t bodyAt = headerBodyOffset(raw, pick.start);
    if (bodyAt == std::string::npos)
        return out;
    out.httpStatus = pick.code;
    out.body = raw.substr(bodyAt);
    return out;
}

inline int attemptsForBudget(int budgetS) {
    if (budgetS < kStepSeconds)
        budgetS = kStepSeconds;
    return (budgetS + kStepSeconds - 1) / kStepSeconds;
}

// A cold transcoder answers late or empty: retry a timeout, a 204, and an ok status with no media body. A
// hard 4xx/5xx (no transcoder, bad request) is dead at once; waiting would only delay the fallback.
inline bool retryable(const Attempt& a) {
    if (a.alive)
        return false;
    if (a.http == 0 || a.http == 204)
        return true;
    return a.http >= 200 && a.http < 300;
}

struct Outcome {
    Attempt last;
    int attempts = 0;
    bool alive = false;
};

// once() -> Attempt; pauseMs(int) waits between attempts (injected so a test needs no clock).
template <class Once, class Pause>
Outcome runProbe(int attempts, Once once, Pause pauseMs) {
    Outcome o;
    if (attempts < 1)
        attempts = 1;
    for (int i = 0; i < attempts; ++i) {
        o.last = once();
        ++o.attempts;
        if (o.last.alive) {
            o.alive = true;
            return o;
        }
#ifndef MUT_RSP_RETRY_HARD
        if (!retryable(o.last))
            return o;
#endif
        if (i + 1 < attempts)
            pauseMs(kRetryPauseMs);
    }
    return o;
}

// GET {base}/video/:/transcode/universal/stop?session=<id> for a start the probe gave up on. The token goes in
// a header (never this URL). Empty when the base is not http(s) or the id is empty or not a plain id.
inline std::string abandonedStopUrl(const std::string& base, const std::string& session) {
    if (session.empty() || session.size() > 128)
        return {};
    for (char c : session) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
                        c == '_' || c == '.';
        if (!ok)
            return {};
    }
    if (base.compare(0, 7, "http://") != 0 && base.compare(0, 8, "https://") != 0)
        return {};
    std::string b = base;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    if (b.find('?') != std::string::npos || b.find('#') != std::string::npos)
        return {};
#ifdef MUT_RSP_STOP_TOKEN_URL
    return b + "/video/:/transcode/universal/stop?session=" + session + "&X-Plex-Token=leak";
#endif
    return b + "/video/:/transcode/universal/stop?session=" + session;
}

// The session id the heartbeat pings and stops: only when the play IS that transcode. A direct-Part
// fallback after a dead start carries the abandoned session's id (already stopped by the resolve).
inline std::string heartbeatSession(bool transcoded, const std::string& sessionId) {
#ifdef MUT_RSP_PING_DIRECT
    (void)transcoded;
    return sessionId;
#endif
    return transcoded ? sessionId : std::string();
}

}  // namespace remote_start
}  // namespace misterplex
