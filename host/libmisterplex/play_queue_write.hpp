#pragma once
// Issue #56: mark a library title Play next or Add to queue from Browse.
//
// The live queue is the PMS playQueue the daemon already refetches in queue_step.hpp
// (skipNext / auto-next). This header only builds the write and decides whether to
// PUT, POST, or play. It does not touch a cast session's companion fields.
// A refused write (4xx/5xx, empty document, version that did not change, or the
// ratingKey missing from the document) leaves playback alone. One attempt, no retry.
// The token is a header at the call site, never part of these URLs or bodies.
// PMS returns HTTP 400 when POST /playQueues carries type/continuous/uri as a
// form body (glass on Native, reviewer, and reviewer). The same fields on the query
// string return 200. Create is a query POST with an empty body. PUT was already
// a query.
// Pre-roll clips (type=clip) stay in the document; queue_step.hpp still owns the
// natural-EOF rule. The footer skips a clip when it names "Up next".

#include "../../arm/misterplexd/log_redact.hpp"
#include "libmisterplex/plexui/plexui_tv.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace misterplex {
namespace plexui {

enum class QueueMark : uint8_t { Append = 0, PlayNext = 1 };

struct QueueBinding {
    bool bound = false;     // a live playQueue id for this server
    bool playing = false;   // something is on screen
    std::string id;
    std::string version;
};

enum class QueueWriteKind : uint8_t { Create, Put, PlayNow };

struct QueueWriteDecision {
    QueueWriteKind kind = QueueWriteKind::Create;
    bool next = false; // PUT ?next=1
};

// Bound queue → PUT (next=1 only for Play next). Nothing playing + Play next →
// just play the title. Otherwise POST /playQueues.
inline QueueWriteDecision decideQueueWrite(QueueMark mark, const QueueBinding& b) {
#ifdef MUT_PQ_WRITE_IGNORED
    (void)mark;
    (void)b;
    QueueWriteDecision d;
    d.kind = QueueWriteKind::PlayNow;
    d.next = false;
    return d;
#else
    if (mark == QueueMark::PlayNext && !b.playing) {
        QueueWriteDecision d;
        d.kind = QueueWriteKind::PlayNow;
        return d;
    }
    if (!b.bound || b.id.empty()) {
        QueueWriteDecision d;
        d.kind = QueueWriteKind::Create;
        return d;
    }
    QueueWriteDecision d;
    d.kind = QueueWriteKind::Put;
#ifdef MUT_PQ_PUT_NO_NEXT
    d.next = false;
#else
    d.next = mark == QueueMark::PlayNext;
#endif
    return d;
#endif
}

inline std::string queueQueryEscape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

inline std::string queueLibraryUri(const std::string& machineId, const std::string& ratingKey) {
    return "server://" + machineId + "/com.plexapp.plugins.library/library/metadata/" + ratingKey;
}

struct QueueHttpRequest {
    std::string method; // "POST" or "PUT"
    std::string url;    // no token; create and update params live in the query
    std::string body;   // empty. A form body on POST /playQueues is HTTP 400.
};

inline std::string queueBase(std::string baseUrl) {
    while (!baseUrl.empty() && baseUrl.back() == '/')
        baseUrl.pop_back();
    return baseUrl;
}

inline QueueHttpRequest buildQueuePut(const std::string& baseUrl, const std::string& queueId,
                                      const std::string& machineId, const std::string& ratingKey,
                                      bool next) {
    QueueHttpRequest r;
    r.method = "PUT";
    r.url = queueBase(baseUrl) + "/playQueues/" + queueQueryEscape(queueId) + "?uri=" +
            queueQueryEscape(queueLibraryUri(machineId, ratingKey));
    if (next)
        r.url += "&next=1";
    return r;
}

inline QueueHttpRequest buildQueueCreate(const std::string& baseUrl, const std::string& machineId,
                                         const std::string& ratingKey) {
    QueueHttpRequest r;
    r.method = "POST";
    const std::string query =
        "type=video&continuous=1&uri=" + queueQueryEscape(queueLibraryUri(machineId, ratingKey));
#ifdef MUT_PQ_POST_BODY
    // Glass: this form body is HTTP 400. The query POST is 200.
    r.url = queueBase(baseUrl) + "/playQueues";
    r.body = query;
#else
    r.url = queueBase(baseUrl) + "/playQueues?" + query;
#endif
    return r;
}

// 4xx/5xx, a document we cannot use, an unchanged playQueueVersion, or a missing ratingKey.
inline bool queueHttpRefused(int httpStatus, bool parsedOk, const std::string& sentVersion,
                             const std::string& returnedVersion, bool containsRatingKey) {
#ifdef MUT_PQ_ACCEPT_REFUSED
    (void)httpStatus;
    (void)parsedOk;
    (void)sentVersion;
    (void)returnedVersion;
    (void)containsRatingKey;
    return false;
#else
    if (httpStatus < 200 || httpStatus >= 300)
        return true;
    if (!parsedOk || !containsRatingKey)
        return true;
    if (returnedVersion.empty())
        return true;
    if (!sentVersion.empty() && returnedVersion == sentVersion)
        return true;
    return false;
#endif
}

inline const char* queueRefusedLine() { return "QUEUE REFUSED"; }

// Depth, and the next title that is not a pre-roll clip. A clip stays in the queue.
inline std::string queueFooterFor(const PlexPlayQueue& q, int index) {
    if (!q.ok || q.items.empty())
        return {};
    std::string up;
    const int start = index < 0 ? 0 : index + 1;
    for (int i = start; i < static_cast<int>(q.items.size()); ++i) {
        const PlexQueueItem& it = q.items[static_cast<std::size_t>(i)];
        if (it.type == "clip")
            continue;
        up = it.title;
        break;
    }
    if (up.size() > 32)
        up.resize(32);
    const int depth = static_cast<int>(q.items.size());
    if (!up.empty())
        return "Queue " + std::to_string(depth) + "  Up next: " + up;
    return "Queue " + std::to_string(depth);
}

struct QueueTransportResult {
    int status = 0;
    bool cancelled = false;
    std::string body;
};

using QueueTransportFn = std::function<QueueTransportResult(const QueueHttpRequest&)>;

struct QueueApplyResult {
    bool applied = false;
    bool refused = false;
    bool playNow = false;
    bool cancelled = false;
    int httpStatus = 0;
    PlexPlayQueue queue;
    std::string osd;
};

inline QueueApplyResult applyQueueMark(const QueueTransportFn& transport, const std::string& baseUrl,
                                       const std::string& machineId, const std::string& ratingKey,
                                       const QueueBinding& binding, QueueMark mark) {
    QueueApplyResult out;
    const QueueWriteDecision d = decideQueueWrite(mark, binding);
    if (d.kind == QueueWriteKind::PlayNow) {
        out.playNow = true;
        return out;
    }
    if (ratingKey.empty() || baseUrl.empty() || !transport) {
        out.refused = true;
        out.osd = queueRefusedLine();
        return out;
    }
    const QueueHttpRequest req = d.kind == QueueWriteKind::Put
                                     ? buildQueuePut(baseUrl, binding.id, machineId, ratingKey, d.next)
                                     : buildQueueCreate(baseUrl, machineId, ratingKey);
    if (misterplex::hasPlexTokenKey(req.url) ||
        misterplex::hasPlexTokenKey(req.body)) {
        out.refused = true;
        out.osd = queueRefusedLine();
        return out;
    }
    const QueueTransportResult r = transport(req);
    out.httpStatus = r.status;
    if (r.cancelled) {
        out.cancelled = true;
        return out;
    }
    PlexPlayQueue q = parsePlayQueueXml(r.body);
    // queueIndexFor falls back to the selected or first row. Presence is an exact key.
    const bool hasKey = queueExactIndex(q, ratingKey) >= 0;
    const std::string sent = d.kind == QueueWriteKind::Put ? binding.version : std::string();
    if (queueHttpRefused(r.status, q.ok, sent, q.version, hasKey)) {
        out.refused = true;
        out.osd = queueRefusedLine();
        return out;
    }
    out.applied = true;
    out.queue = std::move(q);
    const int idx = queueExactIndex(out.queue, ratingKey);
    out.osd = queueFooterFor(out.queue, idx);
    return out;
}

} // namespace plexui
} // namespace misterplex
