#pragma once
// Play next / add to queue from the device (frontier item 2). Pure helpers: plan, request path, result.
// PMS API (Plex Play Queue API, "Add a generator or playlist to a play queue"):
//   PUT /playQueues/{id}?uri=<server://machineId/com.plexapp.plugins.library/library/metadata/RK>&next=1|0
// next=1 puts the item right after the current one, next=0 (default) at the end of the manual queue; the reply is
// the modified queue and playQueueVersion increases. Tokens are never part of anything built here: the caller
// sends X-Plex-Token as a header and never logs it (log_redact.hpp).
#include "url_query_codec.hpp"

#include <cctype>
#include <cstdint>
#include <string>

namespace misterplex {
namespace qadd {

enum class Mode { PlayNext, Append };
enum class Plan { AddToQueue, PlayNow, Refuse };
enum class Outcome { Ok, NoChange, Refused, ServerError, NetError };

struct Context {
    bool playing = false;
    std::string playQueueId;  // the queue the current play is bound to, empty = none (never invented)
    std::string machineId;    // the PMS machine identifier (library URI authority)
    std::string ratingKey;    // the item to add
};

inline bool allDigits(const std::string& s, size_t maxLen = 12) {
    if (s.empty() || s.size() > maxLen)
        return false;
    for (const char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

inline bool machineIdOk(const std::string& s) {
    if (s.empty() || s.size() > 64)
        return false;
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
            return false;
    return true;
}

// Idle + PlayNext = play it now. Idle + Append, no bound queue, or any bad id = refuse (nothing is guessed).
inline Plan planAdd(const Context& c, Mode m) {
    if (!allDigits(c.ratingKey) || !machineIdOk(c.machineId))
        return Plan::Refuse;
    if (!c.playing)
#ifdef MUT_QADD_IDLE_APPEND_PLAYS
        return Plan::PlayNow;
#else
        return m == Mode::PlayNext ? Plan::PlayNow : Plan::Refuse;
#endif
#ifdef MUT_QADD_INVENT_QUEUE
    return Plan::AddToQueue;
#else
    return allDigits(c.playQueueId) ? Plan::AddToQueue : Plan::Refuse;
#endif
}

inline std::string libraryUri(const std::string& machineId, const std::string& ratingKey) {
    return "server://" + machineId + "/com.plexapp.plugins.library/library/metadata/" + ratingKey;
}

// The request path (with query) for the add; empty when the plan is not AddToQueue.
inline std::string addPath(const Context& c, Mode m) {
    if (planAdd(c, m) != Plan::AddToQueue)
        return std::string();
#ifdef MUT_QADD_NO_ENCODE
    const std::string uri = libraryUri(c.machineId, c.ratingKey);
#else
    const std::string uri = urlEncodeQueryComponent(libraryUri(c.machineId, c.ratingKey));
#endif
#ifdef MUT_QADD_NEXT_SWAPPED
    const char* next = m == Mode::PlayNext ? "0" : "1";
#else
    const char* next = m == Mode::PlayNext ? "1" : "0";
#endif
    return "/playQueues/" + c.playQueueId + "?uri=" + uri + "&next=" + next;
}

// HTTP status -> outcome. 2xx = the PMS took it; 4xx (gone item, bad uri, stale queue) = refused; 5xx = server error;
// anything else (0 = no connection, 3xx) = network. No retry is built on any of these.
inline Outcome classifyStatus(int http) {
    if (http >= 200 && http < 300)
        return Outcome::Ok;
#ifdef MUT_QADD_5XX_REFUSED
    if (http >= 400 && http < 600)
        return Outcome::Refused;
#else
    if (http >= 400 && http < 500)
        return Outcome::Refused;
    if (http >= 500 && http < 600)
        return Outcome::ServerError;
#endif
    return Outcome::NetError;
}

// playQueueVersion="N" from the reply body, 0 when absent.
inline int64_t replyVersion(const std::string& body) {
    const std::string key = "playQueueVersion=\"";
    const size_t p = body.find(key);
    if (p == std::string::npos)
        return 0;
    int64_t v = 0;
    size_t i = p + key.size();
    int n = 0;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9' && n < 15) {
        v = v * 10 + (body[i] - '0');
        ++i;
        ++n;
    }
    return n > 0 ? v : 0;
}

// Is the item already queued AFTER the selected (playing) item? body = the GET /playQueues/{id} reply. The PMS accepts
// a duplicate add (version still goes up), so the version check alone cannot see a repeat; the item's own ratingKey is
// compared with the pending items' ratingKey attribute (exact attribute, not parentRatingKey). When the
// selected item is unknown nothing is claimed (false): the add goes ahead.
namespace detail {
inline size_t tagEnd(const std::string& s, size_t from) {  // index of the closing '>' outside quotes
    char q = 0;
    for (size_t i = from; i < s.size(); ++i) {
        if (q) {
            if (s[i] == q)
                q = 0;
        } else if (s[i] == '"' || s[i] == '\'') {
            q = s[i];
        } else if (s[i] == '>') {
            return i;
        }
    }
    return std::string::npos;
}
inline std::string attrOf(const std::string& tag, const std::string& name) {
    const std::string key = " " + name + "=\"";
    const size_t p = tag.find(key);
    if (p == std::string::npos)
        return std::string();
    const size_t b = p + key.size();
    const size_t e = tag.find('"', b);
    return e == std::string::npos ? std::string() : tag.substr(b, e - b);
}
}  // namespace detail

inline bool alreadyPending(const std::string& body, const std::string& ratingKey) {
#ifdef MUT_QADD_DEDUPE_OFF
    (void)body;
    (void)ratingKey;
    return false;
#else
    if (!allDigits(ratingKey))
        return false;
    const size_t mc = body.find("<MediaContainer");
    if (mc == std::string::npos)
        return false;
    const size_t mce = detail::tagEnd(body, mc);
    if (mce == std::string::npos)
        return false;
    const std::string selected = detail::attrOf(body.substr(mc, mce - mc), "playQueueSelectedItemID");
#ifndef MUT_QADD_DEDUPE_ANY_POSITION
    if (selected.empty())
        return false;
    bool after = false;
#else
    bool after = true;  // mutant: items before and including the playing one count too
#endif
    size_t i = mce;
    while ((i = body.find('<', i + 1)) != std::string::npos) {
        const bool item = body.compare(i, 6, "<Video") == 0 || body.compare(i, 6, "<Track") == 0 ||
                          body.compare(i, 6, "<Photo") == 0;
        if (!item)
            continue;
        const size_t e = detail::tagEnd(body, i);
        if (e == std::string::npos)
            break;
        const std::string tag = body.substr(i, e - i);
#ifndef MUT_QADD_DEDUPE_ANY_POSITION
        if (!selected.empty() && detail::attrOf(tag, "playQueueItemID") == selected) {
            after = true;
            i = e;
            continue;
        }
#endif
        if (after && detail::attrOf(tag, "ratingKey") == ratingKey)
            return true;
        i = e;
    }
    return false;
#endif
}

// A 2xx whose version did not increase changed nothing (PMS did not add the item): report it as NoChange.
inline Outcome finalOutcome(int http, const std::string& body, int64_t versionBefore) {
    const Outcome o = classifyStatus(http);
    if (o != Outcome::Ok)
        return o;
#ifdef MUT_QADD_VERSION_IGNORED
    return Outcome::Ok;
#else
    return replyVersion(body) > versionBefore ? Outcome::Ok : Outcome::NoChange;
#endif
}

// Short fixed OSD text (no PMS text, no ids, no token; fits a 240p card).
inline const char* osdMessage(Outcome o, Mode m) {
    switch (o) {
    case Outcome::Ok: return m == Mode::PlayNext ? "Playing next" : "Added to queue";
    case Outcome::NoChange: return "Queue unchanged";
    case Outcome::Refused: return "Queue refused";
    case Outcome::ServerError: return "Server error";
    default: return "No connection";
    }
}

}  // namespace qadd
}  // namespace misterplex
