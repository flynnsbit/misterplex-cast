#pragma once
// POST /misterplex/queue/add?ratingKey=<digits>&mode=next|append|end  (frontier item 2, wiring). Pure parts: the strict
// request parse and the executor that drives queue_add.hpp over an injected transport. The companion calls it behind
// the SEC-M2/M5 guard (peer ACL, Host, Origin, cross-site fetch, no-Origin policy); nothing here touches the network.
#include "queue_add.hpp"

#include <functional>
#include <string>

namespace misterplex {
namespace qadd {

struct Reply {
    int http = 0;  // 0 = no response
    std::string body;
};
// method is "GET" or "PUT"; path is the PMS path plus query. The daemon adds base URL and the X-Plex-Token header.
using Transport = std::function<Reply(const std::string& method, const std::string& path)>;

struct HttpParse {
    bool matched = false;  // the request line names exactly this endpoint
    int code = 0;          // 0 = valid, else the status to answer (405 / 400)
    Mode mode = Mode::PlayNext;
    std::string ratingKey;
};

inline bool queryOnce(const std::string& q, const std::string& key, std::string& out) {
    int hits = 0;
    size_t i = 0;
    while (i <= q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos)
            amp = q.size();
        const std::string kv = q.substr(i, amp - i);
        if (kv.compare(0, key.size() + 1, key + "=") == 0) {
            out = kv.substr(key.size() + 1);
            ++hits;
        }
        i = amp + 1;
    }
    return hits == 1;  // missing or repeated = ambiguous
}

inline HttpParse parseQueueAddRequest(const std::string& req) {
    HttpParse p;
    const std::string line = req.substr(0, req.find("\r\n"));
    const size_t sp1 = line.find(' ');
    const size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1)
        return p;
    const std::string method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t qm = target.find('?');
    if (target.substr(0, qm) != "/misterplex/queue/add")
        return p;
    p.matched = true;
#ifndef MUT_QADD_HTTP_ANY_METHOD
    if (method != "POST") {
        p.code = 405;
        return p;
    }
#endif
    const std::string q = qm == std::string::npos ? std::string() : target.substr(qm + 1);
    std::string mode;
    if (!queryOnce(q, "ratingKey", p.ratingKey) || !allDigits(p.ratingKey) || !queryOnce(q, "mode", mode) ||
#ifdef MUT_QADD_HTTP_NO_END
        (mode != "next" && mode != "append")) {
#else
        (mode != "next" && mode != "append" && mode != "end")) {  // "end" = append
#endif
        p.code = 400;
        return p;
    }
    p.mode = mode == "next" ? Mode::PlayNext : Mode::Append;
    return p;
}

struct Result {
    int code = 0;
    std::string text;  // one of the fixed osdMessage texts, never PMS text
};

inline Result mapOutcome(Outcome o, Mode m) {
    Result r;
    r.text = osdMessage(o, m);
    switch (o) {
    case Outcome::Ok: r.code = 200; break;
    case Outcome::NoChange: r.code = 409; break;
    case Outcome::Refused: r.code = 409; break;
    case Outcome::ServerError: r.code = 502; break;
    default: r.code = 504; break;
    }
#ifdef MUT_QADDX_STATUS_OK_ALWAYS
    r.code = 200;
#endif
    return r;
}

// The one on-screen toast line for a finished queue request (frontier item 2): only the fixed texts above/below pass,
// so PMS text, ids and tokens can never reach the TV. Empty = no toast.
inline std::string noticeText(const Result& r) {
#ifdef MUT_QADDN_RAW_TEXT
    return r.text;
#else
    static const char* const kFixed[] = {"Playing next",   "Added to queue", "Queue unchanged", "Queue refused",
                                         "Server error",   "No connection",  "Not playing"};
    for (const char* f : kFixed)
        if (r.text == f)
            return r.text;
    return std::string();
#endif
}

// GET the queue for its version, then PUT the add; the PMS stays the owner. Nothing is sent unless the plan is
// AddToQueue; a failed or version-less GET stops before the PUT; no retries.
inline Result runQueueAdd(const Context& c, Mode m, const Transport& t) {
    const Plan plan = planAdd(c, m);
#ifndef MUT_QADDX_PUT_WHEN_REFUSED
    if (plan == Plan::Refuse)
        return mapOutcome(Outcome::Refused, m);
    if (plan == Plan::PlayNow)
        return Result{409, "Not playing"};  // idle play-now is not wired over this endpoint
#endif
    if (!t)
        return Result{503, "No connection"};
    const Reply before = t("GET", "/playQueues/" + c.playQueueId);
    int64_t v0 = replyVersion(before.body);
#ifndef MUT_QADDX_DEDUPE_NOT_CHECKED
    // A repeat of an item already pending after the playing one changes nothing: say so, send no PUT.
    if (classifyStatus(before.http) == Outcome::Ok && v0 != 0 && alreadyPending(before.body, c.ratingKey))
        return mapOutcome(Outcome::NoChange, m);
#endif
#ifndef MUT_QADDX_BEFORE_FAIL_IGNORED
    if (classifyStatus(before.http) != Outcome::Ok)
        return mapOutcome(classifyStatus(before.http), m);
    if (v0 == 0)
        return mapOutcome(Outcome::Refused, m);  // not a play-queue document
#endif
#ifdef MUT_QADDX_NO_BEFORE_VERSION
    v0 = 0;
#endif
    const Reply put = t("PUT", addPath(c, m));
    return mapOutcome(finalOutcome(put.http, put.body, v0), m);
}

}  // namespace qadd
}  // namespace misterplex
