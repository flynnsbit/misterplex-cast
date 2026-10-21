// YouTube Lounge (BrowserChannel) protocol pieces for dial_lounge_fe (GCAST P3).
// Pure and network-free so it can be unit-tested and fuzzed: chunk framing,
// batch decoding, SID/gsessionid/AID bookkeeping, command -> cast event mapping,
// and outgoing form encoding. Transport lives in yt_fe.cpp.
#pragma once
#include "gcast/mini_json.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace misterplex {
namespace gcast {
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
enum class Ev { None, Play, Pause, Resume, Seek, Stop, NowPlaying, Volume, Subtitles, RemoteJoin, RemoteLeave };
struct Event {
    Ev kind = Ev::None;
    std::string videoId, listId, remoteName;
    long long ms = 0;  // start or seek target
    int index = 0;
    std::vector<std::string> videoIds;
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
    if (m.cmd == "getVolume" || m.cmd == "setVolume") { e.kind = Ev::Volume; return e; }
    if (m.cmd == "getSubtitlesTrack") { e.kind = Ev::Subtitles; return e; }
    if (m.cmd == "remoteConnected") { e.kind = Ev::RemoteJoin; e.remoteName = argOr(m, "name"); return e; }
    if (m.cmd == "remoteDisconnected") { e.kind = Ev::RemoteLeave; e.remoteName = argOr(m, "name"); return e; }
    return e;
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
// Glass KI-7: after setPlaylist the bind socket stayed ESTABLISHED, outbound
// onStateChange kept flowing on the other connection, and no noop or command
// arrived, so the phone's stopVideo was never read. YouTube then dropped new
// senders. 15s is inside a held poll and well under the 300s socket timeout.
constexpr int kLoungePollIdleSec = 15;

enum class PollNext { Stay, Retry, RebindKeepReady, RebindDropReady };

// err == "idle" is a poll that parsed its headers (often status 200) and then
// received no body bytes. Rebind without clearing the ready flag. A 410/401/403
// still drops the ready flag; that path is the separate KI-2 flap.
inline PollNext nextPoll(bool ok, int status, const std::string& err, int failsAfter) {
    if (err == "idle") return PollNext::RebindKeepReady;
    if (ok && status == 200) return PollNext::Stay;
    if (status == 400 || status == 410 || status == 401 || status == 403)
        return PollNext::RebindDropReady;
    if (failsAfter > 3) return PollNext::RebindDropReady;
    return PollNext::Retry;
}

inline std::string bindSendPath(const BindIds& b, const Channel& c, long rid, const std::string& zx) {
    return bindBase(b) + "&SID=" + formEncode(c.sid) + "&RID=" + std::to_string(rid) + "&AID=" + std::to_string(c.aid) +
           "&gsessionid=" + formEncode(c.gsid) + "&zx=" + zx + "&t=1";
}

}  // namespace lounge
}  // namespace gcast
}  // namespace misterplex
