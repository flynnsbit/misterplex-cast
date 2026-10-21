#pragma once
// Seek on a live remote universal transcode: reuse the session and its decision (lead a lab MiSTer A3 1011:
// a 4-seek burst recovered in ~55 s, two back-to-back ~24 s WAN resolves; one superseded). A seek on
// the SAME item, SAME server, SAME token, SAME clamped ladder and SAME core replays the committed
// start.mp4 with only &offset= changed: same session id, same transcode params, same FFmpeg headers,
// no metadata/decision/alive-probe round trips. Anything else (a ladder-proxy play, a direct Part, a
// changed AU-safe clamp, another key/server/core) takes the full resolve as before.
#include "companion.hpp"
#include "plex_resolve.hpp"

#include <cstdint>
#include <string>

namespace misterplex {

struct SeekReuseEntry {
    bool valid = false;
    std::string key;
    std::string server; // protocol|address|port of the cast
    std::string token;
    std::string ladderSig;
    uint32_t buildId = 0;
    ResolveResult resolved;
    std::string base;
};

inline std::string seekReuseServer(const PlayRequest& r) {
    return r.protocol + "|" + r.address + "|" + r.port;
}

inline std::string seekReuseLadderSig(const WeakLadder& w) {
    return w.videoResolution + "|" + std::to_string(w.maxVideoBitrateKbps) + "|" +
           std::to_string(w.videoQuality) + "|" + w.h264Profile + "|" + std::to_string(w.h264Level) +
           "|" + w.fpgaProfileVariant + "|" + std::to_string(w.fpgaFpsNum) + "/" +
           std::to_string(w.fpgaFpsDen) + "|" + (w.burnSubtitles ? "burn" : "-") + "|" +
           std::to_string(w.subtitleStreamId);
}

inline bool seekReuseEligible(const ResolveResult& r) {
    return r.ok && r.transcoded && !r.viaLadderProxy &&
           r.playable.find("/video/:/transcode/universal/start") != std::string::npos &&
           r.playable.find("session=") != std::string::npos;
}

// The start URL at a new offset (PMS universal offset is seconds; 0 drops the parameter).
inline std::string universalStartWithOffset(const std::string& url, int64_t offsetMs) {
    std::string u = url;
    const std::string kOff = "&offset=";
    size_t p = u.find(kOff);
    if (p != std::string::npos) {
        size_t e = u.find('&', p + 1);
        u.erase(p, (e == std::string::npos ? u.size() : e) - p);
    }
    const int64_t sec = universalOffsetSeconds(offsetMs);
    if (sec <= 0)
        return u;
    const std::string ins = kOff + std::to_string(sec);
    const size_t tok = u.find("&X-Plex-Token=");
    if (tok != std::string::npos)
        u.insert(tok, ins);
    else
        u += ins;
    return u;
}

inline SeekReuseEntry makeSeekReuseEntry(const PlayRequest& bound, const WeakLadder& w, uint32_t buildId,
                                         const ResolveResult& r, const std::string& base) {
    SeekReuseEntry e;
    if (!seekReuseEligible(r) || bound.key.empty())
        return e;
    e.valid = true;
    e.key = bound.key;
    e.server = seekReuseServer(bound);
    e.token = bound.token;
    e.ladderSig = seekReuseLadderSig(w);
    e.buildId = buildId;
    e.resolved = r;
    e.base = base;
    return e;
}

// True (and out/base filled) when this seek may replay the committed session at offsetMs.
inline bool seekReuseFor(const SeekReuseEntry& e, const PlayRequest& req, const WeakLadder& w,
                         uint32_t buildId, int64_t offsetMs, ResolveResult* out, std::string* base,
                         std::string* why) {
    auto no = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
#ifdef MUT_SEEK_NO_REUSE   // test-only mutant: every seek re-resolves (the A3 55 s burst)
    return no("mutant");
#endif
    if (!req.seekReplay) return no("not a seek");
    if (!e.valid) return no("no committed universal session");
    if (req.key != e.key) return no("another item");
    if (seekReuseServer(req) != e.server) return no("another server");
    if (req.token != e.token) return no("another token");
    if (buildId != e.buildId) return no("another core");
    if (seekReuseLadderSig(w) != e.ladderSig) return no("the ladder changed (AU-safe clamp)");
    *out = e.resolved;
    out->playable = universalStartWithOffset(e.resolved.playable, offsetMs);
    *base = e.base;
    if (why) *why = "same session";
    return true;
}

} // namespace misterplex
