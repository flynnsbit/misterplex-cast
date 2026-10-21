#pragma once
// Safety net for a YouTube open that dies before the first byte.
//
// The lab-pc gcast-xcode helper (tools/gcast/gcast_xcode.py, lab is fixing the
// copy on lab-pc) used to start a remux at off>0 by feeding ffmpeg -ss in front
// of the itag pipes. For a split DASH pair the audio itag (140) is not a
// container that can be sliced at a byte offset, so ffmpeg exits 183 and the
// helper answers HTTP 200 with a zero-length body. libav then reports
// "[http] Stream ends prematurely at 0" and "open: Input/output error", the
// OB play ends, and the idle card comes back. A helper 5xx is the same class
// of failure: the previous ffmpeg is still dying, or the seeked remux has
// not produced a packet yet.
//
// This retry stays on the play that already owns the generation. A newer cast
// sets the cancel predicate and the loop stops without reporting ended or
// idle. While it waits it reports BUFFERING. Backoff is 300 ms, 800 ms,
// 1.5 s, 3 s (about 8 s once the failed opens are counted). A seek that is
// still empty after that one more pass opens at previousMs.

#include <cstdint>
#include <string>

namespace misterplex {
namespace gcast {

inline constexpr int kGcastOpenBackoffMs[] = {300, 800, 1500, 3000};
inline constexpr int kGcastOpenAttempts = 5;   // first try plus the four waits

struct OpenClass {
    int httpStatus = 0;
    size_t bodyBytes = 0;
    std::string error;
    bool ok = false;
};

// A cast open armed by armCastOpenRetry ends on the idle chevron instead of
// the PLAYBACK ERROR card. A library Plex SourceError is not armed, so it
// still paints the error.
inline bool castOpenFailEndsIdle(bool armedFlag, bool sourceError) {
#ifndef MUT_CAST_OPEN_ERROR_CARD
    return armedFlag && sourceError;
#else
    (void)armedFlag;
    return false;
#endif
}

inline bool gcastOpenRetryable(const OpenClass& c) {
    if (c.ok && c.bodyBytes > 0 && (c.httpStatus == 0 || (c.httpStatus >= 200 && c.httpStatus < 400)))
        return false;
    if (c.httpStatus >= 500 && c.httpStatus <= 599)
        return true;
    if (c.httpStatus >= 200 && c.httpStatus < 300 && c.bodyBytes == 0)
        return true;
    const std::string& e = c.error;
    if (e.find("Input/output error") != std::string::npos)
        return true;
    if (e.find("prematurely") != std::string::npos)
        return true;
    if (e.find("Server returned 5") != std::string::npos)
        return true;
    if (e.find("5XX") != std::string::npos || e.find("HTTP error 5") != std::string::npos)
        return true;
    return false;
}

inline bool gcastOpenUsable(const OpenClass& c) {
    return c.ok && c.bodyBytes > 0 && (c.httpStatus == 0 || (c.httpStatus >= 200 && c.httpStatus < 400));
}

// Rewrite a helper URL's start_ms=. Leaves every other byte, including a
// googlevideo query, alone. Seeking is not done by splicing a byte range
// onto the audio URL.
inline std::string urlWithStartMs(const std::string& url, int64_t ms) {
    const std::string key = "start_ms=";
    const auto at = url.find(key);
    if (at == std::string::npos)
        return url;
    const auto end = url.find('&', at);
    const std::string neu = key + std::to_string(ms > 0 ? ms : 0);
    if (end == std::string::npos)
        return url.substr(0, at) + neu;
    return url.substr(0, at) + neu + url.substr(end);
}

struct RetryHit {
    bool played = false;
    bool superseded = false;
    bool resumedPrevious = false;
    int64_t playedMs = 0;
};

// tryAt(ms, class) fills the class for that position.
// still() is false when a newer cast owns the generation.
// buffering(ms) is the lounge/overlay report for the wait. It must not
// report idle or ended.
// sleepFor(ms) waits, or returns early when still() has gone false.
template <class Try, class Still, class Buffer, class Sleep>
RetryHit retryOpens(Try tryAt, int64_t startMs, int64_t previousMs, Still still, Buffer buffering, Sleep sleepFor) {
    auto pass = [&](int64_t ms, bool resume) -> RetryHit {
        for (int attempt = 0; attempt < kGcastOpenAttempts; ++attempt) {
            if (!still())
                return RetryHit{false, true, false, ms};
            OpenClass c;
            tryAt(ms, c);
            if (gcastOpenUsable(c))
                return RetryHit{true, false, resume, ms};
            if (!gcastOpenRetryable(c) || attempt + 1 == kGcastOpenAttempts)
                return {};
            buffering(ms);
            sleepFor(kGcastOpenBackoffMs[attempt]);
        }
        return {};
    };
    RetryHit hit = pass(startMs, false);
    if (hit.played || hit.superseded)
        return hit;
    if (previousMs >= 0 && previousMs != startMs) {
        buffering(previousMs);
        hit = pass(previousMs, true);
    }
    return hit;
}

} // namespace gcast
} // namespace misterplex
