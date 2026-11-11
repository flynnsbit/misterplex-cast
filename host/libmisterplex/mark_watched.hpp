#pragma once
// FEATURE-GAP E17: mark watched, mark unwatched, and remove from Continue
// Watching for the focused or playing item.
//
// MARK_WATCHED (misterplex.conf) / MPX_MARK_WATCHED (env wins). Default off.
// Off: build/apply refuse and the program menu does not grow these rows, so
// playback and /:/timeline stay on the tip path. On: a separate PMS request
// (GET /:/scrobble, GET /:/unscrobble, or PUT /actions/removeFromContinueWatching).
// This header never builds /:/timeline and never reports progress.

#include "../../arm/misterplexd/log_redact.hpp"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {

enum class MarkWatchedOp : uint8_t { None = 0, Watched = 1, Unwatched = 2, DropContinue = 3 };

struct MarkTarget {
    std::string baseUrl;
    std::string token;
    std::string ratingKey;
    std::string clientId;
};

struct MarkFocus {
    bool playing = false;
    std::string playingKey;
    std::string playingBase;
    std::string playingToken;
    bool focused = false;
    std::string focusKey;
    std::string focusBase;
    std::string focusToken;
};

struct MarkHttpRequest {
    std::string method; // "GET" or "PUT"
    std::string url;    // no token
    std::vector<std::pair<std::string, std::string>> headers;
};

struct MarkTransportResult {
    int status = 0;
};

using MarkTransportFn = std::function<MarkTransportResult(const MarkHttpRequest&)>;

struct MarkApplyResult {
    bool sent = false;
    bool refused = false;
    int httpStatus = 0;
    std::string osd;
};

inline bool markWatchedEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// Default off. MPX_MARK_WATCHED wins over MARK_WATCHED when it is set.
inline bool markWatchedEnabled() {
#ifdef MUT_MW_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_MARK_WATCHED"); e && e[0])
        return markWatchedEnvOn(e);
    return markWatchedEnvOn(std::getenv("MARK_WATCHED"));
#endif
}

inline bool markRatingKeyOk(const std::string& key) {
    if (key.empty() || key.size() > 16)
        return false;
    for (unsigned char c : key) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

inline std::string markClientId(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return "misterplex";
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '-' && c != '_')
            return "misterplex";
    }
    return id;
}

inline std::string markBaseUrl(std::string base) {
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0)
        return {};
    if (base.find(' ') != std::string::npos || base.find('\r') != std::string::npos ||
        base.find('\n') != std::string::npos)
        return {};
    return base;
}

// Playing item wins while a play is up. Otherwise the focused browse leaf.
inline MarkTarget selectMarkTarget(const MarkFocus& f) {
    MarkTarget t;
#ifdef MUT_MW_PLAYING_IGNORED
    if (f.focused) {
        t.baseUrl = f.focusBase;
        t.token = f.focusToken;
        t.ratingKey = f.focusKey;
        return t;
    }
#endif
    if (f.playing) {
        t.baseUrl = f.playingBase;
        t.token = f.playingToken;
        t.ratingKey = f.playingKey;
        return t;
    }
    if (f.focused) {
        t.baseUrl = f.focusBase;
        t.token = f.focusToken;
        t.ratingKey = f.focusKey;
    }
    return t;
}

inline bool markTokenOk(const std::string& token) {
    if (token.empty() || token.size() > 256)
        return false;
    return token.find('\r') == std::string::npos && token.find('\n') == std::string::npos &&
           token.find(' ') == std::string::npos;
}

// False when the flag is irrelevant here: this only builds the request.
// Callers that send must go through applyMarkWatched, which honors the flag.
inline bool buildMarkWatchedRequest(MarkWatchedOp op, const MarkTarget& target, MarkHttpRequest& out) {
    out = {};
    if (op != MarkWatchedOp::Watched && op != MarkWatchedOp::Unwatched &&
        op != MarkWatchedOp::DropContinue)
        return false;
    if (!markRatingKeyOk(target.ratingKey) || !markTokenOk(target.token))
        return false;
    const std::string base = markBaseUrl(target.baseUrl);
    if (base.empty())
        return false;
    if (op == MarkWatchedOp::Watched) {
#ifdef MUT_MW_WATCHED_IS_TIMELINE
        out.method = "GET";
        out.url = base + "/:/timeline?ratingKey=" + target.ratingKey + "&state=stopped";
#else
        out.method = "GET";
        out.url = base + "/:/scrobble?key=" + target.ratingKey +
                  "&identifier=com.plexapp.plugins.library";
#endif
    } else if (op == MarkWatchedOp::Unwatched) {
        out.method = "GET";
#ifdef MUT_MW_UNWATCHED_IS_SCROBBLE
        out.url = base + "/:/scrobble?key=" + target.ratingKey +
                  "&identifier=com.plexapp.plugins.library";
#else
        out.url = base + "/:/unscrobble?key=" + target.ratingKey +
                  "&identifier=com.plexapp.plugins.library";
#endif
    } else {
#ifdef MUT_MW_DROP_IS_SCROBBLE
        out.method = "GET";
        out.url = base + "/:/scrobble?key=" + target.ratingKey +
                  "&identifier=com.plexapp.plugins.library";
#else
        out.method = "PUT";
        out.url = base + "/actions/removeFromContinueWatching?ratingKey=" + target.ratingKey;
#endif
    }
#ifdef MUT_MW_TOKEN_IN_URL
    out.url += "&X-Plex-Token=" + target.token;
#endif
    if (out.url.find("/:/timeline") != std::string::npos || misterplex::hasPlexTokenKey(out.url) ||
        out.url.find(target.token) != std::string::npos)
        return false;
    out.headers = {
        {"X-Plex-Token", target.token},
        {"X-Plex-Client-Identifier", markClientId(target.clientId)},
        {"X-Plex-Product", "MiSTerPlex"},
        {"Accept", "application/xml"},
    };
    return out.method == "GET" || out.method == "PUT";
}

inline const char* markWatchedOsd(MarkWatchedOp op) {
    if (op == MarkWatchedOp::Watched)
        return "MARKED WATCHED";
    if (op == MarkWatchedOp::Unwatched)
        return "MARKED UNWATCHED";
    if (op == MarkWatchedOp::DropContinue)
        return "REMOVED FROM CONTINUE";
    return "MARK REFUSED";
}

// Flag off: no transport call. A timeline URL is refused so progress reporting
// stays on PmsTimelineReporter.
inline MarkApplyResult applyMarkWatched(const MarkTransportFn& transport, MarkWatchedOp op,
                                        const MarkTarget& target) {
    MarkApplyResult r;
#ifndef MUT_MW_SEND_WHEN_OFF
    if (!markWatchedEnabled()) {
        r.refused = true;
        return r;
    }
#endif
    MarkHttpRequest req;
    if (!transport || !buildMarkWatchedRequest(op, target, req) ||
        req.url.find("/:/timeline") != std::string::npos) {
        r.refused = true;
        r.osd = "MARK REFUSED";
        return r;
    }
    const MarkTransportResult tr = transport(req);
    r.sent = true;
    r.httpStatus = tr.status;
    if (tr.status < 200 || tr.status >= 300) {
        r.refused = true;
        r.osd = "MARK REFUSED";
        return r;
    }
    r.osd = markWatchedOsd(op);
    return r;
}

} // namespace misterplex
