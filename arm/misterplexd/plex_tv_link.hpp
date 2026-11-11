#pragma once

// Companion plex.tv/link PIN exchange. The code is published to the caller.
// The token is passed only to persist(). Nothing here writes either into a log line.

#include "libmisterplex/plex_link_label.hpp"
#include "libmisterplex/plex_web_client.hpp"
#include "libmisterplex/browse_fetch.hpp"
#include "libmisterplex/plexui/plexui_link.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace misterplex {

using PlexTvPostFn = std::function<BrowseHttpResult(
    const std::string& url, const std::string& contentType, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers, int connectMs, int timeoutMs,
    const BrowseCancelFn&)>;
using PlexTvHeaderGetFn = std::function<BrowseHttpResult(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    int connectMs, int timeoutMs, const BrowseCancelFn&)>;

inline bool writePlexTvLinkFile(const std::string& path, const std::string& code) {
    if (path.empty())
        return false;
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return false;
    const std::string payload = code.empty() ? std::string("https://plex.tv/link\n")
                                             : "https://plex.tv/link\n" + code + "\n";
    size_t off = 0;
    while (off < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + off, payload.size() - off);
        if (n < 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    if (::close(fd) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

inline bool sleepPlexTv(const std::function<bool()>& still, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (still && !still())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !still || still();
}

// How one runPlexTvLink call ended. Only the caller's backoff reads it.
enum class PlexTvLinkEnd { Signed, Stopped, Transient, Cap, TokenError };

struct PlexTvLinkResult {
    PlexTvLinkEnd end = PlexTvLinkEnd::Stopped;
    int retryAfterSec = -1;   // plex.tv Retry-After on a 429/503, else -1
};

using PlexTvSleepFn = std::function<bool(int ms)>;   // false: stop asked

// One PIN, then polls, then up to three replacements. True when persist stored a token.
// pollMs/pollMaxMs: poll gap starts at pollMs and doubles up to pollMaxMs (idle backoff).
inline bool runPlexTvLink(const PlexTvPostFn& post, const PlexTvHeaderGetFn& get,
                          const std::string& clientId, const std::function<bool()>& still,
                          const std::function<void(const std::string&)>& onCode,
                          const std::function<bool(const std::string&)>& persist,
                          const std::function<void(const std::string&)>& log,
                          PlexTvLinkResult* result = nullptr, int pollMs = 2000, int pollMaxMs = 2000,
                          const PlexTvSleepFn& sleepFn = {}) {
    const std::string ident = clientId.empty() ? "misterplex" : clientId;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/json"},
        {"X-Plex-Client-Identifier", ident},
        {"X-Plex-Product", "MiSTerPlex"},
        {"X-Plex-Version", kPlexWebVersion},
    };
    PlexTvLinkResult local;
    PlexTvLinkResult& res = result ? *result : local;
    res = PlexTvLinkResult{};
    auto say = [&](const std::string& line) {
        if (log)
            log(line);
    };
    auto publish = [&](const std::string& code) {
        if (onCode)
            onCode(code);
    };
    auto nap = [&](int ms) { return sleepFn ? sleepFn(ms) : sleepPlexTv(still, ms); };
    auto end = [&](PlexTvLinkEnd e, bool clear) {
        res.end = e;
        if (clear) {
#ifdef MUT_PLEXLINK_FAIL_CLEARS
            publish("");
#else
            // Signed/stopped: no row. Any other failure: "connecting, retrying" keeps the row.
            publish(e == PlexTvLinkEnd::Signed || e == PlexTvLinkEnd::Stopped ? std::string()
                                                                              : std::string(kPlexTvLinkPendingCode));
#endif
        }
        return e == PlexTvLinkEnd::Signed;
    };
    constexpr int kMaxCreates = 4;
    for (int created = 0; created < kMaxCreates;) {
        if (still && !still())
            return end(PlexTvLinkEnd::Stopped, false);
        ++created;
        BrowseHttpResult made;
        if (post)
            made = post(
#ifdef MUT_PLEXLINK_STRONG
                        "https://plex.tv/api/v2/pins?strong=true",
#else
                        "https://plex.tv/api/v2/pins?strong=false",   // 4-char PIN plex.tv/link accepts
#endif
                        "application/x-www-form-urlencoded", "", headers, 4000, 12000,
                        [&] { return still && !still(); });
        if (still && !still())
            return end(PlexTvLinkEnd::Stopped, true);
        if (made.tlsFailed) {
            say("companion: plex.tv link tls failed");
            return end(PlexTvLinkEnd::Transient, true);
        }
        const plexui::PlexPin pin = made.ok() ? plexui::parsePinResponse(made.body) : plexui::PlexPin{};
        if (!made.ok() || !pin.ok) {
            say("companion: plex.tv link pin request failed status=" + std::to_string(made.status));
            if (made.status == 429 || made.status == 503)
                res.retryAfterSec = made.retryAfterSec;
            return end(PlexTvLinkEnd::Transient, true);
        }
        publish(pin.code);
        say("companion: plex.tv link code ready");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(pin.expiresIn);
        const std::string pollUrl = "https://plex.tv/api/v2/pins/" + pin.id;
        int waitMs = 0;
        int gapMs = pollMs;
        while ((!still || still()) && std::chrono::steady_clock::now() < deadline) {
            if (waitMs > 0) {
                if (!nap(waitMs))
                    return end(PlexTvLinkEnd::Stopped, true);
                waitMs = 0;
            }
            BrowseHttpResult polled;
            if (get)
                polled = get(pollUrl, headers, 4000, 8000, [&] { return still && !still(); });
            if (still && !still())
                return end(PlexTvLinkEnd::Stopped, true);
            if (polled.tlsFailed) {
                say("companion: plex.tv link tls failed");
                return end(PlexTvLinkEnd::Transient, true);
            }
            if (polled.status == 404 || polled.status == 410) {
                say("companion: plex.tv link code expired");
                break;   // the next PIN replaces the code; no blank card in between
            }
            if (polled.ok()) {
                const plexui::PlexPin now = plexui::parsePinResponse(polled.body);
                if (plexui::pinAuthed(now) && !plexui::plexAccountTokenOk(now.authToken)) {
                    say("companion: plex.tv link token rejected");
                    return end(PlexTvLinkEnd::TokenError, true);
                }
                if (plexui::pinAuthed(now)) {
                    if (persist && !persist(now.authToken)) {
                        say("companion: plex.tv link token was not stored");
                        return end(PlexTvLinkEnd::TokenError, true);
                    }
                    say("companion: plex.tv signed in");
                    return end(PlexTvLinkEnd::Signed, true);
                }
                waitMs = gapMs;
#ifndef MUT_PLEXLINK_NO_IDLE_BACKOFF
                gapMs = std::min(pollMaxMs, std::max(pollMs, gapMs * 2));
#else
                (void)pollMaxMs;
#endif
                continue;
            }
            waitMs = (polled.status == 429 || polled.status >= 500) ? std::max(4000, gapMs) : gapMs;
            if (polled.status == 429 && polled.retryAfterSec > 0)
                waitMs = std::max(waitMs, std::min(polled.retryAfterSec, 3600) * 1000);
        }
    }
    say("companion: plex.tv link pin renewal cap");
    return end(PlexTvLinkEnd::Cap, false);   // the caller asks for the next PIN at once
}

// Backoff for the companion's link loop. sleep is a test hook; empty = real sleep.
struct PlexTvBackoff {
    int retryMs = 30000;          // first wait after a failed PIN request
    int maxRetryMs = 300000;      // failed-request backoff ceiling (5 min)
    int idlePollMaxMs = 60000;    // after the renewal cap the poll gap grows to this
    int tokenErrorLimit = 3;      // persist/token failures before the card shows the error
    int stalledRetryMs = 600000;  // after the limit, retry this often (10 min) instead of stopping
    PlexTvSleepFn sleep;
};

// #228: keep asking for a PIN until signed in or stopped. A failed request backs off
// 30 s doubling to 5 min (or plex.tv's Retry-After). After the 4-PIN cap the next PIN comes
// at once so a valid code stays on screen, but polls slow to one a minute. Token errors
// back off, log once, and end the loop after tokenErrorLimit.
inline bool runPlexTvLinkUntilSigned(const PlexTvPostFn& post, const PlexTvHeaderGetFn& get,
                                     const std::string& clientId, const std::function<bool()>& still,
                                     const std::function<void(const std::string&)>& onCode,
                                     const std::function<bool(const std::string&)>& persist,
                                     const std::function<void(const std::string&)>& log,
                                     const PlexTvBackoff& b = {}, int maxRounds = -1) {
    auto nap = [&](int ms) { return b.sleep ? b.sleep(ms) : sleepPlexTv(still, ms); };
    int failWait = b.retryMs;
    int tokenErrors = 0;
    bool afterCap = false;
    bool loggedToken = false;
    auto quiet = [&](const std::string& line) {   // token lines once, everything else as is
        const bool tokenLine = line.find("token was not stored") != std::string::npos ||
                               line.find("token rejected") != std::string::npos;
        if (tokenLine && loggedToken)
            return;
        if (tokenLine)
            loggedToken = true;
        if (log)
            log(line);
    };
#ifndef MUT_PLEXLINK_FAIL_CLEARS
    if (onCode)
        onCode(kPlexTvLinkPendingCode);   // armed, no PIN yet
#endif
    for (int round = 0; maxRounds < 0 || round < maxRounds; ++round) {
        if (still && !still())
            return false;
        PlexTvLinkResult res;
        if (runPlexTvLink(post, get, clientId, still, onCode, persist, quiet, &res, 2000,
                          afterCap ? b.idlePollMaxMs : 2000, b.sleep))
            return true;
        int waitMs = 0;
        switch (res.end) {
        case PlexTvLinkEnd::Signed: return true;
        case PlexTvLinkEnd::Stopped: return false;
        case PlexTvLinkEnd::Cap:
            afterCap = true;
            failWait = b.retryMs;
            continue;
        case PlexTvLinkEnd::Transient:
#ifdef MUT_PLEXLINK_NO_RETRY
            return false;
#endif
            waitMs = failWait;
#ifndef MUT_PLEXLINK_FLAT_RETRY
            failWait = std::min(b.maxRetryMs, failWait * 2);
            if (res.retryAfterSec > 0) {
                const int ra = std::min(res.retryAfterSec, 3600) * 1000;
#ifdef MUT_PLEXLINK_RA_REPLACES
                waitMs = ra;
#else
                waitMs = std::max(waitMs, ra);   // Retry-After is a floor, never a shortcut
#endif
            }
#endif
            if (log)
                log("companion: plex.tv link retry in " + std::to_string(waitMs / 1000) + " s");
            break;
        case PlexTvLinkEnd::TokenError:
            ++tokenErrors;
#ifndef MUT_PLEXLINK_TOKEN_LOOP
            if (tokenErrors >= b.tokenErrorLimit) {
                if (tokenErrors == b.tokenErrorLimit && log)
                    log("companion: plex.tv link failing on token errors; card shows error, retry every " +
                        std::to_string(b.stalledRetryMs / 1000) + " s");
#ifdef MUT_PLEXLINK_TOKEN_GIVEUP
                return false;
#endif
                if (onCode)
                    onCode(kPlexTvLinkErrorCode);
                if (!nap(b.stalledRetryMs))
                    return false;
                continue;
            }
#endif
            waitMs = std::min(b.maxRetryMs, b.retryMs << std::min(tokenErrors, 4));
            break;
        }
        if (!nap(waitMs))
            return false;
    }
    return false;
}

} // namespace misterplex
