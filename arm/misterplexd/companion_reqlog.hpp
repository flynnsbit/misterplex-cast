// Companion HTTP request latency log (review 03:24, after a D52 L3 8 s /player/timeline/poll
// timeout; release 03:25). The companion HTTP server is one thread: a slow header read or a slow
// handler holds every later request, including Plex Web's polls, in the accept queue. One line
// per request names it:
//   companion: req recv=HH:MM:SS.mmm path=<METHOD /path> hdr_ms=<accept->headers> handler_ms=<headers->close>
//   companion: WARN slow req ...   when hdr_ms or handler_ms >= kCompanionSlowMs
// Poll paths (timeline poll, proxy timeline, mirror, resources/identity, clean closes) arrive about
// once a second or more often, so a fast one is folded into one summary line every
// kCompanionPollLogEveryMs.
// A slow poll is never folded: its WARN prints at once. Log only; nothing reads it back.
// The path never carries the query (tokens live there).
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

namespace misterplex {

constexpr int64_t kCompanionSlowMs = 1000;
constexpr int64_t kCompanionPollLogEveryMs = 10000;
constexpr size_t kCompanionPathMax = 96;

// "GET /player/timeline/poll?wait=1&X-Plex-Token=.. HTTP/1.1\r\n..." -> "GET /player/timeline/poll".
// Query and fragment dropped; at most kCompanionPathMax chars; bytes outside 0x21-0x7e -> '?'.
inline std::string companionRequestPath(const std::string& req) {
    const size_t eol = req.find_first_of("\r\n");
    const std::string line = req.substr(0, eol);
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos)
        return "(no request line)";
    size_t end = line.find_first_of(" ?#", sp1 + 1);
    if (end == std::string::npos)
        end = line.size();
    std::string out = line.substr(0, sp1) + " " + line.substr(sp1 + 1, end - sp1 - 1);
    if (out.size() > kCompanionPathMax)
        out = out.substr(0, kCompanionPathMax) + "...";
    for (char& ch : out) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (ch != ' ' && (u < 0x21 || u > 0x7e))
            ch = '?';
    }
    return out;
}

inline bool companionPollPath(const std::string& path) {
    return path.find("/player/timeline/poll") != std::string::npos ||
           path.find("/player/proxy/timeline") != std::string::npos ||
           path.find("/player/timeline/mirror") != std::string::npos ||
           path.rfind("GET /resources", 0) == 0 || path.rfind("GET /identity", 0) == 0 ||
           path == "(closed)";
}

inline std::string companionClock(int64_t wallMs) {
    const std::time_t s = static_cast<std::time_t>(wallMs / 1000);
    std::tm tm{};
    localtime_r(&s, &tm);
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(wallMs % 1000));
    return b;
}

class CompanionReqLog {
public:
    // One HTTP thread calls this; no locking. nowMs is a steady clock for the poll window.
    std::optional<std::string> observe(const std::string& path, int64_t wallMs, int64_t hdrMs,
                                       int64_t handlerMs, int64_t nowMs) {
        const std::string fields = "recv=" + companionClock(wallMs) + " path=" + path +
                                   " hdr_ms=" + std::to_string(hdrMs) +
                                   " handler_ms=" + std::to_string(handlerMs);
        const bool slow = hdrMs >= kCompanionSlowMs || handlerMs >= kCompanionSlowMs;
#ifdef MUT_COMPANION_SLOW_GT  // test-only mutant: threshold '>' instead of '>='
        const bool slowMut = hdrMs > kCompanionSlowMs || handlerMs > kCompanionSlowMs;
        if (slow != slowMut)
            return "companion: req " + fields;
#endif
#ifdef MUT_COMPANION_POLL_SWALLOWS_SLOW  // test-only mutant: the poll window hides a slow poll
        if (companionPollPath(path) && havePollLog_ &&
            nowMs - lastPollLogMs_ < kCompanionPollLogEveryMs)
            return std::nullopt;
#endif
        if (slow)
            return "companion: WARN slow req " + fields + " (>= " +
                   std::to_string(kCompanionSlowMs) + " ms on the single companion HTTP thread)";
        if (!companionPollPath(path))
            return "companion: req " + fields;
        ++pollN_;
        if (hdrMs > pollMaxHdr_)
            pollMaxHdr_ = hdrMs;
        if (handlerMs > pollMaxHandler_)
            pollMaxHandler_ = handlerMs;
        if (havePollLog_ && nowMs - lastPollLogMs_ < kCompanionPollLogEveryMs)
            return std::nullopt;
        std::string line = "companion: req " + fields + " polls=" + std::to_string(pollN_) +
                           " max_hdr_ms=" + std::to_string(pollMaxHdr_) +
                           " max_handler_ms=" + std::to_string(pollMaxHandler_) +
                           " (fast poll paths: one line per " +
                           std::to_string(kCompanionPollLogEveryMs / 1000) + " s)";
        havePollLog_ = true;
        lastPollLogMs_ = nowMs;
        pollN_ = 0;
        pollMaxHdr_ = 0;
        pollMaxHandler_ = 0;
        return line;
    }

private:
    bool havePollLog_ = false;
    int64_t lastPollLogMs_ = 0;
    int64_t pollN_ = 0;
    int64_t pollMaxHdr_ = 0;
    int64_t pollMaxHandler_ = 0;
};

}  // namespace misterplex
