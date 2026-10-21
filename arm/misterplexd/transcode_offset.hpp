#pragma once
// Start-offset helpers for PMS universal and lab-ladder URLs: the URL query that carries a
// play's start (PMS universal offset= in whole seconds, lab ladder offset_ms= for the proxy's
// FFmpeg -ss). Shared by MediaPlayer (seek restarts) and doPlay (continue-watching start) so
// both bake the start into the URL the same way; the FPGA demuxer cannot seek a live TS.
// Moved verbatim from media_player.cpp (dbcc42fa), then the ladder retarget fix below.
// Review follow-up (review, c65b6078): doPlay start bounds (clampStartMs), overflow-free
// whole-second rounding, and an offset reader that agrees with the lab proxy's parse_int on
// duplicate, blank and malformed values.
#include <cstdint>
#include <string>

#include "libmisterplex/p720_transcode_vf.hpp"  // misterplex::isUniversalTranscodeUrl
#include "url_query_codec.hpp"                   // the encoder resolve builds src= with (F-B)

namespace misterplex {
namespace transcode_offset {

// doPlay start bounds (main.cpp): [0, duration] when the duration is known. An unknown duration
// (0) caps the start at one week, far past any title, so a bogus cast offset or viewOffset cannot
// overflow the whole-second rounding or the position arithmetic downstream.
constexpr int64_t kMaxUnknownDurationStartMs = 7LL * 24 * 3600 * 1000;

inline int64_t clampStartMs(int64_t startMs, int64_t durationMs) {
    if (startMs < 0)
        return 0;
    const int64_t cap = durationMs > 0 ? durationMs : kMaxUnknownDurationStartMs;
    return startMs > cap ? cap : startMs;
}

// PMS universal offset= is whole seconds rounded half-up: (ms + 500) / 1000 without the signed
// overflow near INT64_MAX (MediaPlayer::seekMs does not clamp above when the duration is unknown).
inline int64_t roundMsToSec(int64_t ms) {
    return ms <= 0 ? 0 : ms / 1000 + (ms % 1000 >= 500 ? 1 : 0);
}

inline bool urlHasUniversalOffset(const std::string& url) {
    auto q = url.find('?');
    if (q == std::string::npos)
        return false;
    const std::string qs = url.substr(q + 1);
    // PMS universal bakes offset= (seconds). Lab ladder proxy bakes offset_ms=
    // so FFmpeg -ss runs on the remote Part — live mpegts is not demux-seekable.
    if (isUniversalTranscodeUrl(url) && qs.find("offset=") != std::string::npos)
        return true;
    if (url.find("/ladder.ts") != std::string::npos &&
        qs.find("offset_ms=") != std::string::npos)
        return true;
    return false;
}

// Replace or append a query key with value (no encoding). Preserves #fragment.
inline std::string withQueryParam(const std::string& url, const std::string& key,
                                  const std::string& value) {
    const std::string assignment = key + "=" + value;
    const auto q = url.find('?');
    const auto hash = url.find('#');
    const auto end = (hash == std::string::npos) ? url.size() : hash;
    if (q == std::string::npos || q > end) {
        return url.substr(0, end) + "?" + assignment +
               (hash == std::string::npos ? std::string() : url.substr(hash));
    }
    auto pos = q + 1;
    while ((pos = url.find(key + "=", pos)) != std::string::npos && pos < end) {
        const bool atKey = pos == q + 1 || url[pos - 1] == '&';
        if (atKey) {
            auto valEnd = url.find('&', pos);
            if (valEnd == std::string::npos || valEnd > end)
                valEnd = end;
            return url.substr(0, pos) + assignment + url.substr(valEnd);
        }
        pos += key.size() + 1;
    }
    return url.substr(0, end) + "&" + assignment +
           (hash == std::string::npos ? std::string() : url.substr(hash));
}

inline std::string withUniversalOffset(const std::string& url, int64_t offsetMs) {
    if (!isUniversalTranscodeUrl(url))
        return url;
    return withQueryParam(url, "offset", std::to_string(roundMsToSec(offsetMs)));
}

// ---- F-B: a live ladder src carries its own start (T-BROWSE-DOUBLE-SEEK fix F-B, PREREG-FB rev1.1) ----
// The lab ladder proxy's offset_ms is an input -ss (remote_fpga_ladder_proxy e1c0abf0 :415-418). It
// works on a seekable Part and does nothing on a live src: ds3 logged 'could not seek' for every
// transcode or relay src, and each then played from its own start. So a live src keeps its start
// in its own carrier, and the outer URL carries no offset_ms:
//   R0  the src kind comes from the decoded src= value (ladderLiveSrc);
//   R1  seek and retarget set the carrier to the target and remove every outer offset_ms
//       (withLiveLadderStart); nothing else changes, inner or outer;
//   R2  retargetStartUrl keeps the URL iff the carrier is already the target and no offset_ms is
//       present;
//   R3  a Part src keeps the offset_ms behaviour below, byte for byte.
enum class LadderSrcKind { Part, Relay, Universal, YtRemux };

struct LadderLiveSrc {
    LadderSrcKind kind = LadderSrcKind::Part;
    size_t valStart = 0;  // the encoded src= value is url[valStart, valEnd)
    size_t valEnd = 0;
    std::string src;          // decoded
    size_t carrierPos = 0;    // RELAY / YTREMUX: the o<ms> / start_ms= digits are src[carrierPos, carrierPos + carrierLen)
    size_t carrierLen = 0;
    int64_t carrierMs = -1;   // RELAY: the start in the path; -1 if it does not fit
};

inline int64_t queryParamInt(const std::string& url, const std::string& key);

// The first src= value of a /ladder.ts URL (the key starts the query or follows '&'), still
// encoded: up to the next '&' (urlEncodeQueryComponent never writes a raw '&') or the #fragment.
inline bool ladderSrcValue(const std::string& url, size_t* valStart, size_t* valEnd) {
    if (url.find("/ladder.ts") == std::string::npos)
        return false;
    const auto q = url.find('?');
    const auto hash = url.find('#');
    const auto end = (hash == std::string::npos) ? url.size() : hash;
    if (q == std::string::npos || q > end)
        return false;
    auto pos = q + 1;
    while ((pos = url.find("src=", pos)) != std::string::npos && pos < end) {
        if (pos == q + 1 || url[pos - 1] == '&') {
            auto v = url.find('&', pos);
            if (v == std::string::npos || v > end)
                v = end;
            *valStart = pos + 4;
            *valEnd = v;
            return true;
        }
        pos += 4;
    }
    return false;
}

// The helper's relay route: mpx_menu.py :1142 @675c9b09 matches ^/menu/tsrc/[a-z0-9]+/\d+/o\d+$
// against urlparse(...).path (:1111, :1116-1117), i.e. the path with the query and fragment split
// off; the scheme, host and port do not matter (TSRC_BASE may be any base, :1004). So a query after
// o<ms> is still a relay (the helper ignores it), while a trailing '/', an upper-case slug or a
// non-digit rk is not (the helper answers 404) and stays PART. Not mirrored: urlparse also splits
// ';params' off the last segment; such a src stays PART (the helper never builds one, :1004).
inline bool relayStartDigits(const std::string& src, size_t* pos, size_t* len) {
    const auto scheme = src.find("://");
    if (scheme == std::string::npos)
        return false;
    const auto p = src.find_first_of("/?#", scheme + 3);
    if (p == std::string::npos || src[p] != '/')
        return false;
    auto pathEnd = src.find_first_of("?#", p);
    if (pathEnd == std::string::npos)
        pathEnd = src.size();
    static const std::string kRoute = "/menu/tsrc/";
    if (src.compare(p, kRoute.size(), kRoute) != 0)
        return false;
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    size_t i = p + kRoute.size();
    const size_t slug = i;
    while (i < pathEnd && ((src[i] >= 'a' && src[i] <= 'z') || isDigit(src[i])))
        ++i;
    if (i == slug || i >= pathEnd || src[i] != '/')
        return false;
    const size_t rk = ++i;
    while (i < pathEnd && isDigit(src[i]))
        ++i;
    if (i == rk || i >= pathEnd || src[i] != '/')
        return false;
    if (++i >= pathEnd || src[i] != 'o')
        return false;
    const size_t digits = ++i;
    while (i < pathEnd && isDigit(src[i]))
        ++i;
    if (i == digits || i != pathEnd)
        return false;
    *pos = digits;
    *len = i - digits;
    return true;
}

// GCAST YouTube: gcast-xcode's /yt.ts?v=<id>&start_ms=<ms> is a live remux that starts at
// start_ms (the ladder's -ss cannot seek it). The carrier is the start_ms= digits; the path must be
// exactly /yt.ts and the field must be a whole query field (start of query or after '&').
inline bool ytRemuxStartDigits(const std::string& src, size_t* pos, size_t* len) {
    const auto scheme = src.find("://");
    if (scheme == std::string::npos)
        return false;
    const auto p = src.find_first_of("/?#", scheme + 3);
    if (p == std::string::npos || src[p] != '/')
        return false;
    const auto q = src.find('?', p);
    const auto hash = src.find('#', p);
    if (q == std::string::npos || (hash != std::string::npos && hash < q) || src.compare(p, q - p, "/yt.ts") != 0)
        return false;
    const auto end = hash == std::string::npos ? src.size() : hash;
    static const std::string kKey = "start_ms=";
    size_t f = q + 1;
    while (f < end) {
        auto amp = src.find('&', f);
        if (amp == std::string::npos || amp > end)
            amp = end;
        if (src.compare(f, kKey.size(), kKey) == 0) {
            const size_t d = f + kKey.size();
            size_t i = d;
            while (i < amp && src[i] >= '0' && src[i] <= '9')
                ++i;
            if (i == d || i != amp)
                return false;
            *pos = d;
            *len = i - d;
            return true;
        }
        f = amp + 1;
    }
    return false;
}

// R0. True for a /ladder.ts URL whose src is live: the helper's relay (start in its path, ms) or a
// PMS universal transcode (start in its offset= query key, whole seconds; matched by exact name,
// so viewOffset= or startOffset= is never the carrier). The UNIVERSAL arm is PROVISIONAL (F-B
// commit B): it is pushed only if ds4 D2 shows PMS honouring a new offset= on the same session.
// Only a src that re-encodes to the very same bytes is taken (resolve's own encoder built it, so
// that always holds for resolve's URLs); anything else stays PART, i.e. today's behaviour.
inline bool ladderLiveSrc(const std::string& url, LadderLiveSrc* out) {
    size_t vs = 0, ve = 0;
    if (!ladderSrcValue(url, &vs, &ve))
        return false;
    const std::string enc = url.substr(vs, ve - vs);
    std::string src;
    if (!urlDecodeQueryComponent(enc, &src) || urlEncodeQueryComponent(src) != enc)
        return false;
    LadderLiveSrc live;
    live.valStart = vs;
    live.valEnd = ve;
    if (relayStartDigits(src, &live.carrierPos, &live.carrierLen)) {
        live.kind = LadderSrcKind::Relay;
        live.carrierMs = live.carrierLen <= 18 ? std::stoll(src.substr(live.carrierPos, live.carrierLen))
                                               : -1;
#ifndef MUT_TO_NO_YTREMUX
    } else if (ytRemuxStartDigits(src, &live.carrierPos, &live.carrierLen)) {
        live.kind = LadderSrcKind::YtRemux;
        live.carrierMs = live.carrierLen <= 18 ? std::stoll(src.substr(live.carrierPos, live.carrierLen))
                                               : -1;
#endif
    } else if (isUniversalTranscodeUrl(src)) {
        live.kind = LadderSrcKind::Universal;
    } else {
        return false;
    }
    live.src = src;
    *out = live;
    return true;
}

// GCAST: the ladder's src is gcast-xcode's /yt.ts live remux (its start is start_ms=, never a demux seek).
inline bool ladderYtRemuxSrc(const std::string& url) {
    LadderLiveSrc live;
    return ladderLiveSrc(url, &live) && live.kind == LadderSrcKind::YtRemux;
}

// The URL already carries this play's start: PMS universal offset= (seconds),
// a ladder offset_ms, a live ladder src (relay / universal / yt.ts), or
// gcast-xcode start_ms=. A second demux seek on that live TS yields no frame.
inline bool liveStartBaked(const std::string& url) {
    if (urlHasUniversalOffset(url))
        return true;
    if (url.find("/yt.ts") != std::string::npos || url.find("start_ms=") != std::string::npos)
        return true;
    LadderLiveSrc live;
    return ladderLiveSrc(url, &live);
}

// Timeline stays in milliseconds. The demux / container seek is 0 when the URL
// already starts there, so NeedsTranscode's retry (2217 ms -> offset=2) does
// not av_seek_frame the live transcode as well.
inline int64_t demuxSeekMs(const std::string& url, int64_t timelineMs) {
    if (timelineMs <= 0 || liveStartBaked(url))
        return 0;
    return timelineMs;
}

// Every key= field of the query removed, whatever its position (first, middle or last); the other
// fields keep their bytes and order. A query left empty loses its '?'. The #fragment is kept.
inline std::string withoutQueryParam(const std::string& url, const std::string& key) {
    const auto q = url.find('?');
    const auto hash = url.find('#');
    const auto end = (hash == std::string::npos) ? url.size() : hash;
    if (q == std::string::npos || q > end)
        return url;
    const std::string qs = url.substr(q + 1, end - q - 1);
    const std::string needle = key + "=";
    std::string kept;
    bool any = false;
    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        if (amp == std::string::npos)
            amp = qs.size();
        const std::string field = qs.substr(pos, amp - pos);
        if (field.compare(0, needle.size(), needle) != 0) {
            kept += (any ? "&" : "") + field;
            any = true;
        }
        pos = amp + 1;
    }
    return url.substr(0, q) + (any ? "?" + kept : std::string()) +
           (hash == std::string::npos ? std::string() : url.substr(hash));
}

// R1. The live src with its carrier set to startMs (a negative start counts as 0), re-encoded with
// the encoder resolve used, and no outer offset_ms.
inline std::string withLiveLadderStart(const std::string& url, const LadderLiveSrc& live, int64_t startMs) {
    if (startMs < 0)
        startMs = 0;
    std::string src = live.src;
    if (live.kind == LadderSrcKind::Relay || live.kind == LadderSrcKind::YtRemux)
        src.replace(live.carrierPos, live.carrierLen, std::to_string(startMs));
    else if (live.kind == LadderSrcKind::Universal)
        src = withQueryParam(src, "offset", std::to_string(roundMsToSec(startMs)));
    const std::string out =
        url.substr(0, live.valStart) + urlEncodeQueryComponent(src) + url.substr(live.valEnd);
    return withoutQueryParam(out, "offset_ms");
}

// R2. The live src already starts at startMs. A universal src without offset= starts at 0.
inline bool liveLadderAtStart(const LadderLiveSrc& live, int64_t startMs) {
    if (live.kind == LadderSrcKind::Universal)
        return queryParamInt(live.src, "offset") == roundMsToSec(startMs);
    return (live.kind == LadderSrcKind::Relay || live.kind == LadderSrcKind::YtRemux) && live.carrierMs == startMs;
}

// Seek restarts often land on a large IDR that exceeds the FPGA 8KiB AU/RBSP
// budget at the default 4Mbps 240p profile. Clamp bitrate/quality on the
// universal URL so PMS emits smaller AUs while keeping the same resolution.
inline std::string withSeekSafeTranscodeParams(const std::string& url, int64_t offsetMs) {
    {
        LadderLiveSrc live;
        if (ladderLiveSrc(url, &live))
            return withLiveLadderStart(url, live, offsetMs);  // F-B R1
    }
    std::string out = withUniversalOffset(url, offsetMs);
    // Lab ladder: bake offset_ms for FFmpeg -ss (resume/scrub). Live TS is not seekable.
    // Retarget it whenever the URL already carries one (a resumed or seeked session): the
    // restart must start at the new target, and a seek to 0 sets offset_ms=0 (the proxy's
    // default) instead of keeping the old start. (dbcc42fa kept the first offset_ms.)
    if (out.find("/ladder.ts") != std::string::npos &&
        (offsetMs > 0 || urlHasUniversalOffset(out)))
        out = withQueryParam(out, "offset_ms", std::to_string(offsetMs > 0 ? offsetMs : 0));
    if (!isUniversalTranscodeUrl(out))
        return out;
    constexpr int kSeekMaxBitrateKbps = 350;
    constexpr int kSeekMaxQuality = 18;
    auto clampParam = [&](const char* key, int ceiling) {
        const auto q = out.find('?');
        if (q == std::string::npos)
            return;
        const auto hash = out.find('#');
        const auto end = (hash == std::string::npos) ? out.size() : hash;
        const std::string needle = std::string(key) + "=";
        auto pos = q + 1;
        while ((pos = out.find(needle, pos)) != std::string::npos && pos < end) {
            const bool atKey = pos == q + 1 || out[pos - 1] == '&';
            if (!atKey) {
                pos += needle.size();
                continue;
            }
            const auto valStart = pos + needle.size();
            auto valEnd = out.find('&', valStart);
            if (valEnd == std::string::npos || valEnd > end)
                valEnd = end;
            int cur = 0;
            try {
                cur = std::stoi(out.substr(valStart, valEnd - valStart));
            } catch (...) {
                cur = ceiling + 1;
            }
            if (cur > ceiling)
                out = withQueryParam(out, key, std::to_string(ceiling));
            return;
        }
        out = withQueryParam(out, key, std::to_string(ceiling));
    };
    clampParam("maxVideoBitrate", kSeekMaxBitrateKbps);
    clampParam("videoQuality", kSeekMaxQuality);
    out = withQueryParam(out, "fastSeek", "1");
    return out;
}

// The first value of a query key; true when the key is present (the value may be blank). Same
// key matching as withQueryParam: the key starts the query or follows '&', #fragment excluded.
inline bool findQueryParam(const std::string& url, const std::string& key, std::string* value) {
    const auto q = url.find('?');
    const auto hash = url.find('#');
    const auto end = (hash == std::string::npos) ? url.size() : hash;
    if (q == std::string::npos || q > end)
        return false;
    auto pos = q + 1;
    while ((pos = url.find(key + "=", pos)) != std::string::npos && pos < end) {
        if (pos == q + 1 || url[pos - 1] == '&') {
            const auto valStart = pos + key.size() + 1;
            auto valEnd = url.find('&', valStart);
            if (valEnd == std::string::npos || valEnd > end)
                valEnd = end;
            if (value)
                *value = url.substr(valStart, valEnd - valStart);
            return true;
        }
        pos += key.size() + 1;
    }
    return false;
}

// The value of a query key (empty when absent or blank).
inline std::string queryParamValue(const std::string& url, const std::string& key) {
    std::string v;
    findQueryParam(url, key, &v);
    return v;
}

// The first value of a query key as an integer: 0 when the key is absent (the lab proxy's
// offset_ms default), -1 when it is present but blank, non-numeric or followed by junk ("5000x",
// "12.5"). -1 never equals a start (>= 0), so retargetStartUrl rewrites the key with a clean
// value. The lab proxy's parse_int also reads the first value only, but falls back to 0 on junk
// and skips a blank first value (parse_qs), so a prefix parse (std::stoll alone: "5000x" ->
// 5000) or blank -> 0 could leave a URL the proxy starts somewhere else.
inline int64_t queryParamInt(const std::string& url, const std::string& key) {
    std::string v;
    if (!findQueryParam(url, key, &v))
        return 0;
    try {
        size_t used = 0;
        const long long n = std::stoll(v, &used, 10);
        if (used == v.size())
            return n;
    } catch (...) {
    }
    return -1;
}

// doPlay: the play starts at startMs (the cast offset, the PMS continue-watching viewOffset when the
// cast omitted offset=, or a scrub that landed during resolve), but the resolved URL carries the
// start it was built for. When the two differ, bake startMs into the URL the way a seek restart
// does; otherwise the FPGA demuxer is asked to av_seek_frame a live transcode / ladder TS while
// the timeline reports startMs. Compared against the start the URL actually carries (lab ladder
// offset_ms exact, PMS universal offset= in whole seconds rounded half-up), so a URL that already
// starts there keeps its parameters. Other URLs (direct Part / file) are returned unchanged: the
// demuxer seeks those. On a ladder with a Part src only offset_ms changes: the proxy bounds a
// resumed AU itself (crf rungs: vbv-bufsize 900 kbit; other rungs: br <= 900 whenever
// offset_ms > 0). On a ladder with a live src (F-B) the start is the src's own carrier, and the
// URL keeps its parameters iff that carrier is already startMs and no offset_ms is present.
inline std::string retargetStartUrl(const std::string& url, int64_t startMs) {
    if (startMs < 0)
        startMs = 0;
    if (url.find("/ladder.ts") != std::string::npos) {
        LadderLiveSrc live;
        if (ladderLiveSrc(url, &live)) {  // F-B R2, else R1
            if (liveLadderAtStart(live, startMs) && !findQueryParam(url, "offset_ms", nullptr))
                return url;
            return withLiveLadderStart(url, live, startMs);
        }
        if (queryParamInt(url, "offset_ms") == startMs)
            return url;
        return withSeekSafeTranscodeParams(url, startMs);
    }
    if (isUniversalTranscodeUrl(url)) {
        const int64_t wantSec = roundMsToSec(startMs);
        const bool hasOffset = !queryParamValue(url, "offset").empty();
        if (hasOffset && queryParamInt(url, "offset") == wantSec)
            return url;
        if (!hasOffset && wantSec == 0) {
            // A start under half a second: PMS starts at 0 either way. Say so (offset=0, no seek
            // clamps) so the demuxer does not av_seek_frame the live TS to startMs.
            return startMs > 0 ? withUniversalOffset(url, startMs) : url;
        }
        // A universal start that moved (continue-watching viewOffset without the lab proxy, or a
        // scrub during resolve) takes the seek-restart clamps (maxVideoBitrate <= 350,
        // videoQuality <= 18, fastSeek=1) for this transcode session, as a seek restart does: the
        // first IDR must fit the FPGA AU budget. New with 6c0c6af5 for no-proxy continue-watching
        // starts, which before kept the cast-offset URL and av_seek_frame'd the live TS. Prod casts
        // go through the lab ladder proxy, where only offset_ms changes.
        return withSeekSafeTranscodeParams(url, startMs);
    }
    return url;
}

}  // namespace transcode_offset
}  // namespace misterplex
