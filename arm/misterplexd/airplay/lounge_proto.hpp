// YouTube Lounge (BrowserChannel) protocol pieces for dial_lounge_fe (GCAST P3).
// Pure and network-free so it can be unit-tested and fuzzed: chunk framing,
// batch decoding, SID/gsessionid/AID bookkeeping, command -> cast event mapping,
// and outgoing form encoding. Transport lives in yt_fe.cpp.
#pragma once
#include "airplay/ns.hpp"
#include "gcast/mini_json.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace misterplex {
namespace airplay {
namespace lounge {

constexpr size_t kMaxChunk = 1u << 20;  // a single framed payload above 1 MiB is rejected

// "<len>\n<payload>" framing of the bind long-poll body. Feed bytes as they arrive.
class ChunkParser {
public:
    // Returns false once the stream is malformed; the caller should reconnect.
    bool feed(const char* p, size_t n, std::vector<std::string>& out) {
        if (bad_) return false;
        buf_.append(p, n);
        for (;;) {
            if (need_ == 0) {
                size_t nl = buf_.find('\n');
                if (nl == std::string::npos) {
                    if (buf_.size() > 16) { bad_ = true; return false; }
                    return true;
                }
                std::string head = buf_.substr(0, nl);
                while (!head.empty() && (head.back() == '\r' || head.back() == ' ')) head.pop_back();
                if (head.empty() || head.size() > 8) { bad_ = true; return false; }
                for (char c : head) if (c < '0' || c > '9') { bad_ = true; return false; }
                size_t len = std::strtoul(head.c_str(), nullptr, 10);
#ifndef MUT_LP_NO_CAP
                if (len > kMaxChunk) { bad_ = true; return false; }
#endif
                if (len == 0) { buf_.erase(0, nl + 1); continue; }
                need_ = len;
                buf_.erase(0, nl + 1);
            }
            if (buf_.size() < need_) return true;
            out.push_back(buf_.substr(0, need_));
            buf_.erase(0, need_);
            need_ = 0;
        }
    }
    bool bad() const { return bad_; }
private:
    std::string buf_;
    size_t need_ = 0;
    bool bad_ = false;
};

struct Message {
    long index = -1;
    std::string cmd;
    std::map<std::string, std::string> args;  // first-arg object, scalars only
    std::vector<std::string> strArgs;         // positional scalar args (c / S)
};

// Payload is a JSON array of [index, [cmd, args...]].
inline bool decodeBatch(const std::string& payload, std::vector<Message>& out) {
    Json root;
    if (!parseJson(payload, root) || root.type != Json::Arr) return false;
    for (auto& item : root.arr) {
        if (item.type != Json::Arr || item.arr.size() < 2) continue;
        if (item.arr[0].type != Json::Num || item.arr[1].type != Json::Arr || item.arr[1].arr.empty()) continue;
        const Json& body = item.arr[1];
        if (body.arr[0].type != Json::Str) continue;
        Message m;
        m.index = long(item.arr[0].num);
        m.cmd = body.arr[0].str;
        for (size_t i = 1; i < body.arr.size(); ++i) {
            const Json& a = body.arr[i];
            if (a.type == Json::Obj) {
                if (m.args.empty())
                    for (auto& kv : a.obj)
                        if (kv.second.type != Json::Arr && kv.second.type != Json::Obj) m.args[kv.first] = kv.second.scalar();
            } else if (a.type == Json::Str || a.type == Json::Num) {
                m.strArgs.push_back(a.scalar());
            }
        }
        out.push_back(std::move(m));
    }
    return true;
}

// Channel bookkeeping: c -> SID, S -> gsessionid, AID = last seen index.
struct Channel {
    std::string sid, gsid;
    long aid = -1;
    // Returns true if the message is new and should be dispatched to the app.
    bool accept(const Message& m) {
#ifndef MUT_LP_NO_DEDUP
        if (m.index <= aid) return false;
#endif
        aid = m.index;
        if (m.cmd == "c") { if (!m.strArgs.empty()) sid = m.strArgs[0]; return false; }
        if (m.cmd == "S") { if (!m.strArgs.empty()) gsid = m.strArgs[0]; return false; }
        if (m.cmd == "noop") return false;
        return true;
    }
    void reset() { sid.clear(); gsid.clear(); aid = -1; }
};

// ---- command -> cast event ----
enum class Ev { None, Play, Pause, Resume, Seek, Stop, NowPlaying, Volume, Subtitles, RemoteJoin, RemoteLeave, Queue, Next, Prev };
// How a Queue event edits the id list. It does not start playback.
enum class QueueOp { None, Replace, Append, InsertNext, Remove, Clear };
struct Event {
    Ev kind = Ev::None;
    std::string videoId, listId, remoteName;
    long long ms = 0;  // start or seek target
    int index = 0;
    std::vector<std::string> videoIds;
    QueueOp qop = QueueOp::None;
    // setVolume only. -1 and hasVolume false mean getVolume, a rejected arg,
    // or YT_VOLUME off (the arg is not read).
    int volume = -1;
    bool hasVolume = false;
    // setSubtitlesTrack only, and only when YT_CAPTIONS is on. Flag off leaves
    // that command unparsed. getSubtitlesTrack is Ev::Subtitles with captionSet
    // false (the tip ack: video id only, nothing shown).
    bool captionSet = false;
    bool captionShow = false;
    std::string captionLang, captionVss, captionName, captionKind;
};

inline bool validVideoId(const std::string& v) {
    if (v.size() != 11) return false;
    for (char c : v)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
// "12.5" -> 12500. Rejects negatives, junk, NaN and absurd values (> 1 week).
inline bool secondsToMs(const std::string& s, long long& ms) {
    if (s.empty() || s.size() > 24) return false;
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0' || !(d >= 0.0) || d > 604800.0) return false;
    ms = (long long)(d * 1000.0 + 0.5);
    return true;
}
inline std::vector<std::string> splitIds(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == ',') { if (validVideoId(cur)) v.push_back(cur); cur.clear(); } else cur += c; }
    if (validVideoId(cur)) v.push_back(cur);
    if (v.size() > 200) v.resize(200);
    return v;
}
inline std::string argOr(const Message& m, const char* k) {
    auto it = m.args.find(k); return it == m.args.end() ? std::string() : it->second;
}

// YT_NEXT_PREV (misterplex.conf) / MPX_YT_NEXT_PREV (env wins). Default off.
// On: lounge "next" / "previous" skip within the current queue. Off: those
// tokens stay Ev::None, the same as any other unparsed lounge command.
// Natural-end advance does not read this flag.
inline bool ytNextPrevEnvOn(const char* e) {
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

inline bool ytNextPrevEnabled() {
#ifdef MUT_LP_NEXT_PREV_IGNORED
    return false;
#elif defined(MUT_LP_NEXT_PREV_FORCED)
    return true;
#else
    if (const char* e = std::getenv("MPX_YT_NEXT_PREV"); e && e[0])
        return ytNextPrevEnvOn(e);
    return ytNextPrevEnvOn(std::getenv("YT_NEXT_PREV"));
#endif
}

// YT_VOLUME (misterplex.conf) / MPX_YT_VOLUME (env wins). Default off.
// On: setVolume with an integer 0-100 is a level for the player, and
// getVolume replies the last applied level (100 until a set). Off: the
// volume argument is not read. The reply stays onVolumeChanged 100 and
// the player volume path is not used. Unset is that tip path.
inline bool ytVolumeEnvOn(const char* e) {
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

inline bool ytVolumeEnabled() {
#ifdef MUT_LP_VOLUME_IGNORED
    return false;
#elif defined(MUT_LP_VOLUME_FORCED)
    return true;
#else
    if (const char* e = std::getenv("MPX_YT_VOLUME"); e && e[0])
        return ytVolumeEnvOn(e);
    return ytVolumeEnvOn(std::getenv("YT_VOLUME"));
#endif
}

// "0".."100" only. Decimals, signs, junk, and anything above 100 are not a set.
inline bool parseVolumePercent(const std::string& s, int& out) {
    if (s.empty() || s.size() > 3)
        return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
#ifndef MUT_LP_VOLUME_OVER
    if (v > 100)
        return false;
#endif
    out = v;
    return true;
}

// Level to put in onVolumeChanged. Flag off is always 100 (tip path).
// Flag on with a parsed setVolume is that level. Otherwise the remembered level.
inline int loungeVolumeReply(bool enabled, const Event& e, int current) {
#ifdef MUT_LP_VOLUME_ALWAYS_100
    (void)enabled;
    (void)e;
    (void)current;
    return 100;
#else
    if (!enabled)
        return 100;
    int v = e.hasVolume ? e.volume : current;
    if (v < 0)
        return 0;
    if (v > 100)
        return 100;
    return v;
#endif
}

// True when this event should call the player setVolumePercent.
inline bool loungeVolumeApplies(bool enabled, const Event& e) {
#ifdef MUT_LP_VOLUME_NO_APPLY
    (void)enabled;
    (void)e;
    return false;
#else
    return enabled && e.hasVolume && e.volume >= 0 && e.volume <= 100;
#endif
}

// YT_CAPTIONS (misterplex.conf) / MPX_YT_CAPTIONS (env wins). Default off.
// On: setSubtitlesTrack with a language shows that track on the HDMI caption
// band, and an empty track hides it. Off: setSubtitlesTrack stays unparsed.
// getSubtitlesTrack still replies onSubtitlesTrackChanged with the video id
// only, and no caption text is painted. Unset is that tip path.
inline bool ytCaptionsEnabled() {
#ifdef MUT_LP_CAPTIONS_IGNORED
    return false;
#elif defined(MUT_LP_CAPTIONS_FORCED)
    return true;
#else
    if (const char* e = std::getenv("MPX_YT_CAPTIONS"); e && e[0])
        return ytVolumeEnvOn(e);
    return ytVolumeEnvOn(std::getenv("YT_CAPTIONS"));
#endif
}

// BCP-47-ish language the timedtext query can carry. Two or three letters,
// optional -tag. Nothing else, so the URL cannot grow a second parameter.
inline bool captionLangOk(const std::string& s) {
    if (s.size() < 2 || s.size() > 16)
        return false;
    size_t i = 0;
    int letters = 0;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
        ++letters;
        ++i;
    }
    if (letters < 2 || letters > 3)
        return false;
    if (i == s.size())
        return true;
    if (s[i] != '-')
        return false;
    ++i;
    int tail = 0;
    while (i < s.size()) {
        if (!std::isalnum(static_cast<unsigned char>(s[i])))
            return false;
        ++tail;
        ++i;
    }
    return tail >= 2 && tail <= 8;
}

inline bool captionVssOk(const std::string& s) {
    if (s.empty() || s.size() > 32)
        return false;
    bool letter = false;
    for (unsigned char c : s) {
        if (std::isalpha(c))
            letter = true;
        else if (!(std::isdigit(c) || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return letter;
}

inline std::string langFromCaptionVss(const std::string& vss) {
    std::string s = vss;
    if (s.size() >= 2 && s[0] == 'a' && s[1] == '.')
        s = s.substr(2);
    else if (!s.empty() && s[0] == '.')
        s = s.substr(1);
    return captionLangOk(s) ? s : std::string();
}

inline std::string captionKindOf(const std::string& kind, const std::string& vss) {
    std::string k;
    k.reserve(kind.size());
    for (unsigned char c : kind)
        k.push_back(static_cast<char>(std::tolower(c)));
    if (k == "asr")
        return "asr";
    if (vss.size() >= 2 && vss[0] == 'a' && vss[1] == '.')
        return "asr";
    return {};
}

inline std::string captionNameClean(const std::string& name) {
    std::string o;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f)
            continue;
        o.push_back(static_cast<char>(c));
        if (o.size() >= 64)
            break;
    }
    return o;
}

// True when this setSubtitlesTrack should change what HDMI shows (show or hide).
inline bool loungeCaptionApplies(bool enabled, const Event& e) {
#ifdef MUT_LP_CAPTIONS_NO_APPLY
    (void)enabled;
    (void)e;
    return false;
#else
    return enabled && e.kind == Ev::Subtitles && e.captionSet;
#endif
}

// Queue edits that arrive after the first setPlaylist/setVideo. Those
// commands used to fall through as Ev::None, so natural end kept walking
// the id list from that first play. A queue edit must not be Ev::Play:
// Play restarts the current video. MUT_LP_DROP_QUEUE restores the drop.
inline Event queueChangeEvent(const Message& m) {
    Event e;
#ifndef MUT_LP_DROP_QUEUE
    if (m.cmd == "updatePlaylist") {
        e.videoIds = splitIds(argOr(m, "videoIds"));
        if (e.videoIds.empty()) return Event{};
        e.kind = Ev::Queue;
        e.qop = QueueOp::Replace;
        e.listId = argOr(m, "listId");
        return e;
    }
    if (m.cmd == "addVideo" || m.cmd == "insertVideo" || m.cmd == "removeVideo") {
        const std::string vid = argOr(m, "videoId");
        if (!validVideoId(vid)) return Event{};
        e.kind = Ev::Queue;
        e.videoId = vid;
        e.videoIds = {vid};
        if (m.cmd == "addVideo") e.qop = QueueOp::Append;
        else if (m.cmd == "insertVideo") e.qop = QueueOp::InsertNext;
        else e.qop = QueueOp::Remove;
        return e;
    }
    if (m.cmd == "clearPlaylist") {
        e.kind = Ev::Queue;
        e.qop = QueueOp::Clear;
        return e;
    }
#else
    (void)m;
#endif
    return e;
}

inline Event toEvent(const Message& m) {
    Event e;
    if (m.cmd == "setPlaylist" || m.cmd == "setVideo") {
        e.videoIds = splitIds(argOr(m, "videoIds"));
        std::string vid = argOr(m, "videoId");
        long long idx = 0;
        std::string ci = argOr(m, "currentIndex");
        if (!ci.empty()) { char* end = nullptr; idx = std::strtoll(ci.c_str(), &end, 10); if (!end || *end) idx = 0; }
        if (vid.empty() && idx >= 0 && idx < (long long)e.videoIds.size()) vid = e.videoIds[size_t(idx)];
#ifndef MUT_LP_NO_VIDCHECK
        if (!validVideoId(vid)) return Event{};
#endif
        e.kind = Ev::Play; e.videoId = vid; e.index = int(idx < 0 ? 0 : idx);
        e.listId = argOr(m, "listId");
        long long ms = 0;
        e.ms = secondsToMs(argOr(m, "currentTime"), ms) ? ms : 0;
        return e;
    }
    if (m.cmd == "pause") { e.kind = Ev::Pause; return e; }
    if (m.cmd == "play") { e.kind = Ev::Resume; return e; }
    if (m.cmd == "stopVideo") { e.kind = Ev::Stop; return e; }
    if (m.cmd == "seekTo") {
        long long ms = 0;
        if (!secondsToMs(argOr(m, "newTime"), ms)) return Event{};
        e.kind = Ev::Seek; e.ms = ms; return e;
    }
    if (m.cmd == "getNowPlaying") { e.kind = Ev::NowPlaying; return e; }
    if (m.cmd == "getVolume" || m.cmd == "setVolume") {
        e.kind = Ev::Volume;
        // Flag off does not read the argument (tip path: reply stays 100).
#ifndef MUT_LP_VOLUME_DROP_ARG
        if (m.cmd == "setVolume" && ytVolumeEnabled()) {
            int v = 0;
            if (parseVolumePercent(argOr(m, "volume"), v)) {
                e.volume = v;
                e.hasVolume = true;
            }
        }
#endif
#ifdef MUT_LP_VOLUME_GET_SETS
        if (m.cmd == "getVolume") {
            e.volume = 0;
            e.hasVolume = true;
        }
#endif
        return e;
    }
    if (m.cmd == "getSubtitlesTrack") { e.kind = Ev::Subtitles; return e; }
    if (m.cmd == "setSubtitlesTrack") {
        // Flag off: same as any other unparsed lounge command (tip path).
        if (!ytCaptionsEnabled())
            return Event{};
        e.kind = Ev::Subtitles;
        e.captionSet = true;
#ifndef MUT_LP_CAPTIONS_DROP_ARG
        const std::string vid = argOr(m, "videoId");
        if (validVideoId(vid))
            e.videoId = vid;
        std::string lang = argOr(m, "languageCode");
        if (!captionLangOk(lang))
            lang.clear();
        std::string vss = argOr(m, "vssId");
        if (vss.empty())
            vss = argOr(m, "vss_id");
        if (!captionVssOk(vss))
            vss.clear();
        e.captionVss = vss;
        e.captionLang = !lang.empty() ? lang : langFromCaptionVss(vss);
        e.captionName = captionNameClean(argOr(m, "trackName"));
        if (e.captionName.empty())
            e.captionName = captionNameClean(argOr(m, "languageName"));
        e.captionKind = captionKindOf(argOr(m, "kind"), e.captionVss);
        e.captionShow = !e.captionLang.empty();
#endif
#ifdef MUT_LP_CAPTIONS_ALWAYS_SHOW
        e.captionShow = true;
        if (e.captionLang.empty())
            e.captionLang = "en";
#endif
        return e;
    }
    if (m.cmd == "remoteConnected") { e.kind = Ev::RemoteJoin; e.remoteName = argOr(m, "name"); return e; }
    if (m.cmd == "remoteDisconnected") { e.kind = Ev::RemoteLeave; e.remoteName = argOr(m, "name"); return e; }
    if (m.cmd == "updatePlaylist" || m.cmd == "addVideo" || m.cmd == "insertVideo" ||
        m.cmd == "removeVideo" || m.cmd == "clearPlaylist")
        return queueChangeEvent(m);
    // Phone skip. Flag off leaves the token unparsed (Ev::None).
    if (m.cmd == "next" || m.cmd == "previous") {
        if (!ytNextPrevEnabled())
            return Event{};
#ifdef MUT_LP_NEXT_PREV_SWAP
        e.kind = m.cmd == "next" ? Ev::Prev : Ev::Next;
#else
        e.kind = m.cmd == "next" ? Ev::Next : Ev::Prev;
#endif
        return e;
    }
    return e;
}

// The id list natural end walks, in queue order. index is the playing id when
// it is still in the list. endPlaysFirst means that id was removed or the
// replacement list does not contain it, so the next video is ids[0].
struct IdList {
    std::vector<std::string> ids;
    size_t index = 0;
    bool endPlaysFirst = false;
};

inline bool naturalEndNext(const IdList& q, size_t& nextIndex) {
#ifdef MUT_LP_NEXT_PREV_GATES_EOF
    if (!ytNextPrevEnabled()) return false;
#endif
    if (q.endPlaysFirst) {
        if (q.ids.empty()) return false;
        nextIndex = 0;
        return true;
    }
    if (q.index + 1 < q.ids.size()) {
        nextIndex = q.index + 1;
        return true;
    }
    return false;
}

// Phone next walks the same list natural end walks. Phone previous steps
// back one id. A miss (no neighbor, or the playing id was removed and only
// a forward arm remains) leaves the current video playing.
enum class SkipDir { Next, Prev };

inline bool queueSkip(const IdList& q, SkipDir dir, size_t& at) {
    if (dir == SkipDir::Next) {
#ifdef MUT_LP_NEXT_PREV_WRAP
        if (!naturalEndNext(q, at)) {
            if (q.ids.empty()) return false;
            at = 0;
            return true;
        }
        return true;
#else
        return naturalEndNext(q, at);
#endif
    }
    if (q.endPlaysFirst || q.ids.empty() || q.index >= q.ids.size())
        return false;
#ifdef MUT_LP_NEXT_PREV_RESTART
    if (q.index == 0) { at = 0; return true; }
#endif
    if (q.index == 0)
        return false;
    at = q.index - 1;
    return true;
}

// Apply one queue edit. currentId is the video that is already playing.
// A hit keeps that video where it now sits so natural end walks forward
// in this list. A miss arms the first id for the natural end.
inline bool applyLoungeQueue(IdList& q, const Event& e, const std::string& currentId) {
    if (e.kind != Ev::Queue) return false;
    auto findId = [&](const std::string& id) -> size_t {
        if (id.empty()) return q.ids.size();
        for (size_t i = 0; i < q.ids.size(); ++i) if (q.ids[i] == id) return i;
        return q.ids.size();
    };
    auto retarget = [&]() -> bool {
        const size_t at = findId(currentId);
        if (at == q.ids.size()) return false;
        q.index = at;
        return true;
    };
    auto cap = [&]() {
        if (q.ids.size() > 200) q.ids.resize(200);
        if (!q.ids.empty() && q.index >= q.ids.size()) q.index = q.ids.size() - 1;
    };
    auto oneId = [&]() -> std::string {
        if (validVideoId(e.videoId)) return e.videoId;
        if (!e.videoIds.empty() && validVideoId(e.videoIds[0])) return e.videoIds[0];
        return {};
    };
    switch (e.qop) {
    case QueueOp::Replace:
        q.ids = e.videoIds;
        q.index = 0;
        q.endPlaysFirst = false;
        if (!retarget() && !q.ids.empty()) q.endPlaysFirst = true;
        cap();
        return true;
    case QueueOp::Append:
    case QueueOp::InsertNext: {
        const std::string vid = oneId();
        if (!validVideoId(vid)) return false;
        if (q.ids.empty() && validVideoId(currentId)) {
            q.ids.push_back(currentId);
            q.index = 0;
        }
        const bool pinned = retarget();
        const size_t at = (e.qop == QueueOp::InsertNext && pinned) ? q.index + 1 : q.ids.size();
        q.ids.insert(q.ids.begin() + static_cast<std::vector<std::string>::difference_type>(at), vid);
        q.endPlaysFirst = false;
        if (!retarget() && !q.ids.empty()) q.endPlaysFirst = true;
        cap();
        return true;
    }
    case QueueOp::Remove: {
        const std::string vid = oneId();
        if (!validVideoId(vid)) return false;
        retarget();
        size_t at = q.ids.size();
        for (size_t i = 0; i < q.ids.size(); ++i) {
            if (q.ids[i] != vid) continue;
            if (at == q.ids.size()) at = i;
            if (i == q.index) { at = i; break; }
        }
        if (at >= q.ids.size()) return true;
        const bool droppedCurrent = at == q.index && q.ids[at] == currentId;
        q.ids.erase(q.ids.begin() + static_cast<std::vector<std::string>::difference_type>(at));
        if (droppedCurrent) {
            if (at < q.ids.size()) {
                if (at == 0) { q.endPlaysFirst = true; q.index = 0; }
                else { q.endPlaysFirst = false; q.index = at - 1; }
            } else {
                q.endPlaysFirst = false;
                q.index = q.ids.empty() ? 0 : q.ids.size() - 1;
            }
        } else if (at < q.index) {
            --q.index;
            q.endPlaysFirst = false;
        }
        return true;
    }
    case QueueOp::Clear:
        q.ids.clear();
        q.index = 0;
        q.endPlaysFirst = false;
        if (validVideoId(currentId)) q.ids.push_back(currentId);
        return true;
    case QueueOp::None:
        return false;
    }
    return false;
}

// ---- outgoing ----
struct Out {
    std::string cmd;
    std::vector<std::pair<std::string, std::string>> kv;
};
inline std::string formEncode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}
// count=N&ofs=K&req0__sc=cmd&req0_key=val...
inline std::string formBody(const std::vector<Out>& outs, long ofs) {
    std::string b = "count=" + std::to_string(outs.size()) + "&ofs=" + std::to_string(ofs);
    for (size_t i = 0; i < outs.size(); ++i) {
        std::string p = "req" + std::to_string(i) + "_";
        b += "&" + p + "_sc=" + formEncode(outs[i].cmd);
        for (auto& kv : outs[i].kv) b += "&" + p + formEncode(kv.first) + "=" + formEncode(kv.second);
    }
    return b;
}
// Lounge player states, same codes as the YouTube IFrame API:
// -1 unstarted, 0 ended, 1 playing, 2 paused, 3 buffering, 5 cued.
enum State { Unstarted = -1, Ended = 0, Playing = 1, Paused = 2, Buffering = 3, Cued = 5 };
inline std::string secs(long long ms) {
    char b[32]; std::snprintf(b, sizeof b, "%.3f", double(ms < 0 ? 0 : ms) / 1000.0); return b;
}
inline Out onStateChange(int state, long long posMs, long long durMs, const std::string& cpn) {
    Out o{"onStateChange", {}};
    o.kv = {{"currentTime", secs(posMs)}, {"duration", secs(durMs)}, {"cpn", cpn}, {"loadedTime", secs(durMs)},
            {"seekableStartTime", "0"}, {"seekableEndTime", secs(durMs)}, {"state", std::to_string(state)}};
    return o;
}
inline Out nowPlaying(const std::string& videoId, const std::string& listId, int index,
                      int state, long long posMs, long long durMs, const std::string& cpn) {
    Out o{"nowPlaying", {}};
    if (videoId.empty()) return o;  // empty nowPlaying == idle
    o.kv = {{"videoId", videoId}, {"currentTime", secs(posMs)}, {"duration", secs(durMs)}, {"cpn", cpn},
            {"loadedTime", secs(durMs)}, {"seekableStartTime", "0"}, {"seekableEndTime", secs(durMs)},
            {"state", std::to_string(state)}, {"currentIndex", std::to_string(index)}};
    if (!listId.empty()) o.kv.push_back({"listId", listId});
    return o;
}
inline Out volumeChanged(int vol) { return Out{"onVolumeChanged", {{"volume", std::to_string(vol)}, {"muted", "false"}}}; }
inline Out subtitlesChanged(const std::string& videoId) { return Out{"onSubtitlesTrackChanged", {{"videoId", videoId}}}; }

// Track the phone should paint as selected. Flag off, or CC off, stays the
// video-id-only ack (the tip reply).
struct CaptionEcho {
    bool show = false;
    std::string lang, name, vss, kind;
};
inline Out captionLoungeReply(bool enabled, const std::string& videoId, const CaptionEcho& cur) {
    if (!enabled || !cur.show || cur.lang.empty())
        return subtitlesChanged(videoId);
    Out o = subtitlesChanged(videoId);
    o.kv.push_back({"languageCode", cur.lang});
    if (!cur.name.empty()) {
        o.kv.push_back({"languageName", cur.name});
        o.kv.push_back({"trackName", cur.name});
    }
    if (!cur.vss.empty())
        o.kv.push_back({"vssId", cur.vss});
    if (cur.kind == "asr")
        o.kv.push_back({"kind", "asr"});
    return o;
}

// ---- bind URLs (host www.youtube.com, these are the path+query) ----
struct BindIds { std::string screenUuid, name, loungeToken; };
inline std::string bindBase(const BindIds& b) {
    return "/api/lounge/bc/bind?device=LOUNGE_SCREEN&id=" + formEncode(b.screenUuid) + "&name=" + formEncode(b.name) +
           "&app=lb-v4&theme=cl&capabilities=" + formEncode("dsp,mic,dpa") + "&mdxVersion=2&loungeIdToken=" +
           formEncode(b.loungeToken) + "&VER=8&v=2";
}
inline std::string bindInitialPath(const BindIds& b, long rid, const std::string& zx) {
    return bindBase(b) + "&RID=" + std::to_string(rid) + "&CVER=1&zx=" + zx + "&t=1";
}
inline std::string bindPollPath(const BindIds& b, const Channel& c, const std::string& zx) {
    return bindBase(b) + "&RID=rpc&SID=" + formEncode(c.sid) + "&CI=0&AID=" + std::to_string(c.aid) +
           "&gsessionid=" + formEncode(c.gsid) + "&TYPE=xmlhttp&zx=" + zx + "&t=1";
}
// Glass KI-7: after setPlaylist the bind socket stayed ESTABLISHED and outbound
// onStateChange kept flowing, but the phone's stopVideo never arrived. YouTube
// then dropped a new sender about 7s after setPlaylist ("screen disconnected").
// The long-poll has to come back with that command's AID before then. Silence
// on a held poll is not a dead screen: poll again on the same SID, and only
// after several quiet polls open a fresh bind. Ready stays up either way.
constexpr int kLoungeSenderDropSec = 7;
constexpr int kLoungePollIdleSec = 4;
constexpr int kLoungeRebindGraceMs = 20000;
// Glass window for the first stop after a cold boot. YouTube drops the screen
// about kLoungeSenderDropSec after setPlaylist if the long-poll never comes
// back with the new AID. A stop inside this window has to be on that new poll.
constexpr int kColdBootStopWindowMs = 5000;

enum class PollNext { Stay, PollAgain, Retry, RebindKeepReady, RebindDropReady };

// A long-poll that already delivered a message (AID moved) must end so the next
// GET carries the new AID. Bind-init passes endAfterAid false and keeps reading.
// The caller still reconnects on a framing error; that is not an AID advance.
inline bool endPollForNewAid(bool aidAdvanced, bool endAfterAid) {
    return endAfterAid && aidAdvanced;
}

// Milliseconds from setPlaylist until the next GET can carry stopVideo.
// An AID advance ends the read now (0). A silent socket waits out the idle
// gap. The old 15s idle was past both this window and the sender drop, so the
// first stop never arrived and the socket stayed ESTABLISHED.
inline int repollDelayMs(bool aidAdvanced, bool endAfterAid, int idleSec) {
    if (endPollForNewAid(aidAdvanced, endAfterAid))
        return 0;
    return idleSec > 0 ? idleSec * 1000 : 15000;
}

// True when stopVideo that arrives stopAtMs after setPlaylist is accepted on a
// poll that is already open, and that poll opened before the screen is dropped.
inline bool coldBootStopDelivered(int stopAtMs, bool aidAdvanced, int idleSec, bool stopAccepted) {
    if (!stopAccepted || stopAtMs < 0)
        return false;
    const int ready = repollDelayMs(aidAdvanced, true, idleSec);
    if (ready >= kColdBootStopWindowMs || idleSec >= kLoungeSenderDropSec)
        return false;
    return stopAtMs >= ready && stopAtMs <= kColdBootStopWindowMs;
}

// err == "idle": headers arrived (often 200) and then no body bytes for
// kLoungePollIdleSec. PollAgain keeps the SID. The third quiet poll rebinds
// and still keeps Ready. 410 is the gracefulReconnect close: rebind, keep
// Ready for the grace window (KI-2). 401/403 reject the lounge token.
inline PollNext nextPoll(bool ok, int status, const std::string& err, int failsAfter) {
    if (err == "idle")
        return failsAfter >= 3 ? PollNext::RebindKeepReady : PollNext::PollAgain;
    if (ok && status == 200) return PollNext::Stay;
#ifdef MUT_LP_410_DROP
    if (status == 410) return PollNext::RebindDropReady;
#else
    if (status == 410) return PollNext::RebindKeepReady;
#endif
    if (status == 400 || status == 401 || status == 403)
        return PollNext::RebindDropReady;
    if (failsAfter > 3) return PollNext::RebindDropReady;
    return PollNext::Retry;
}

inline std::string bindSendPath(const BindIds& b, const Channel& c, long rid, const std::string& zx) {
    return bindBase(b) + "&SID=" + formEncode(c.sid) + "&RID=" + std::to_string(rid) + "&AID=" + std::to_string(c.aid) +
           "&gsessionid=" + formEncode(c.gsid) + "&zx=" + zx + "&t=1";
}

}  // namespace lounge
}  // namespace airplay
}  // namespace misterplex
