#pragma once
// Back When Pluto TV live (PLUTO_LIVE): public boot.pluto.tv + stitcher HLS only.

#include "gcast/mini_json.hpp"
#include "libmisterplex/pluto_live.hpp"
#include "libmisterplex/h264_sps.hpp"
#include "libmisterplex/remote_h264_transcode.hpp"
#include "spelunkr/broadcast.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace misterplex {
namespace spelunkr {
namespace pluto_live {

inline constexpr const char* kPlutoBootStartUrl =
    "https://boot.pluto.tv/v4/start?appName=web&appVersion=1.0.0&clientModelNumber=na&deviceMake=Chrome&deviceModel=web"
    "&deviceType=web&deviceVersion=Chrome%20120.0.0.0";
// Unit tests may set MPX_PLUTO_BOOT_URL to https://boot.example.invalid/v4/start?... (synthetic).
inline std::string plutoBootBaseUrl() {
    if (const char* e = std::getenv("MPX_PLUTO_BOOT_URL"); e && e[0])
        return e;
    return kPlutoBootStartUrl;
}
// Slate fix (glass #685 80544cc5): the legacy `<stitcher>/stitch/hls/channel/<id>/master.m3u8` path now
// serves Pluto's takedown slate ("no longer available on this device", clip *ptv_takedownslates*) for every
// session; the same boot + bearer + stitcherParams on `/v2/stitch/hls/channel/...` returns the real channel
// (buildhost pluto-probe RESULT-SLATE.md). MUT_PLUTO_LEGACY_STITCH_PATH restores the legacy path.
#ifdef MUT_PLUTO_LEGACY_STITCH_PATH
inline constexpr const char* kPlutoStitchChannelPath = "/stitch/hls/channel/";
#else
inline constexpr const char* kPlutoStitchChannelPath = "/v2/stitch/hls/channel/";
#endif
// The /v2 stitcher requires the session on every request: master without it is 401, and the media playlist
// ffmpeg fetches (it gets NO Authorization header since ADDENDUM 8) is 401 unless the session rides in the
// URL. `jwt=<sessionToken>&masterJWTPassthrough=true` makes the stitcher copy the jwt into every media
// playlist URI (buildhost probe: slate_bearer.py / slate_jwtq.py). The jwt only goes to the *.pluto.tv stitcher
// host; segment/key hosts (plutotv.net) need no auth. Logs redact the `jwt` key (log_redact.hpp).
inline std::string plutoUrlEncodeComponent(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline std::string plutoStitchMasterUrl(const std::string& stitcher, const std::string& channelId,
                                        const std::string& stitcherParams, const std::string& sessionToken) {
    std::string u = stitcher + kPlutoStitchChannelPath + channelId + "/master.m3u8?" + stitcherParams;
#ifndef MUT_PLUTO_NO_JWT_QUERY
    if (!sessionToken.empty())
        u += "&jwt=" + plutoUrlEncodeComponent(sessionToken) + "&masterJWTPassthrough=true";
#else
    (void)sessionToken;
#endif
    return u;
}
inline constexpr size_t kPlutoMaxPlaylistBody = 128u << 10;
inline constexpr size_t kPlutoMaxBootBody = 512u << 10;
inline constexpr size_t kPlutoMaxProbeBytes = 400u << 10;
inline constexpr const char* kPlutoItemId = "pluto";
inline constexpr int64_t kPlutoBootCacheSec = 3600;

inline std::string asciiLowerCopy(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline int tagIntValue(const std::string& line, const char* key) {
    const auto p = line.find(key);
    if (p == std::string::npos)
        return 0;
    const char* start = line.c_str() + p + std::strlen(key);
    return static_cast<int>(std::strtol(start, nullptr, 10));
}

inline float tagFloatValue(const std::string& line, const char* key) {
    const auto p = line.find(key);
    if (p == std::string::npos)
        return 0.f;
    const char* start = line.c_str() + p + std::strlen(key);
    return static_cast<float>(std::strtod(start, nullptr));
}

inline std::vector<std::string> splitLines(const std::string& body) {
    std::vector<std::string> lines;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

inline bool plutoPlayUrlAllowed(const std::string& url) {
    if (!misterplex::plutoLiveEnabled())
        return false;
    if (url.rfind("https://", 0) != 0 || urlHasForbiddenChars(url) || !urlPortAllowed(url))
        return false;
    return hostIsPlutoOwned(urlHost(url));
}

// HLS key/segment URIs: *.pluto.tv or *.plutotv.net (exact suffix).
inline bool plutoHlsSubresourceFetchAllowed(const std::string& url) {
    if (url.rfind("https://", 0) != 0 || urlHasForbiddenChars(url) || !urlPortAllowed(url))
        return false;
    return hostIsPlutoOwned(urlHost(url));
}

// N12/N1: the master query (stitcherParams + the session jwt + masterJWTPassthrough) is copied onto a query-less
// variant URI only when that URI stays on a Pluto-owned host: a relative URI (resolves against the stitcher) or an
// absolute https URI whose host is exactly pluto.tv / plutotv.net or a subdomain (".pluto.tv" / ".plutotv.net"
// boundary, so evilpluto.tv, pluto.tv.evil.com and userinfo/backslash tricks are all off-domain).
// MUT_PLUTO_JWT_INHERIT_ANY_HOST restores the unconditional copy.
inline bool plutoVariantMayInheritMasterQuery(const std::string& uri) {
#ifdef MUT_PLUTO_JWT_INHERIT_ANY_HOST
    (void)uri;
    return true;
#else
    if (uri.empty())
        return false;
    if (uri.rfind("//", 0) == 0)
        return plutoHlsSubresourceFetchAllowed("https:" + uri);
    const auto colon = uri.find(':');
    const auto firstDelim = uri.find_first_of("/?#");
    if (colon == std::string::npos || (firstDelim != std::string::npos && firstDelim < colon))
        return !urlHasForbiddenChars(uri);  // relative reference: stays on the stitcher host
    return plutoHlsSubresourceFetchAllowed(uri);  // absolute: https + exact pluto suffix + port 443
#endif
}
inline std::string plutoMediaUrlForVariant(const std::string& variantUri, const std::string& masterQuery) {
    std::string u = variantUri;
    if (u.find('?') == std::string::npos && !masterQuery.empty() && plutoVariantMayInheritMasterQuery(u))
        u += "?" + masterQuery;
    return u;
}

// tlsVerifiedGet redirect hop (N2): drop Authorization if Location leaves Pluto HTTPS scope.
inline void plutoStripBearerOnRedirectOffScope(std::vector<std::pair<std::string, std::string>>& headers,
                                               const std::string& nextUrl) {
    if (!misterplex::plutoLiveEnabled())
        return;
    bool hasAuth = false;
    for (const auto& h : headers) {
        if (asciiLowerCopy(h.first) == "authorization") {
            hasAuth = true;
            break;
        }
    }
    if (!hasAuth)
        return;
#ifdef MUT_PLUTO_REDIRECT_KEEP_BEARER
    (void)nextUrl;
#else
    if (plutoPlayUrlAllowed(nextUrl))
        return;
#endif
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& h : headers) {
        if (asciiLowerCopy(h.first) != "authorization")
            out.push_back(h);
    }
    headers = std::move(out);
}

inline std::string randomUuidV4() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xffffffffu);
    auto hex = [](uint32_t v, int nibbles) {
        static const char* h = "0123456789abcdef";
        std::string s;
        for (int i = nibbles - 1; i >= 0; --i)
            s += h[(v >> (i * 4)) & 0xf];
        return s;
    };
    const uint32_t a = dis(gen);
    const uint32_t b = dis(gen);
    const uint32_t c = dis(gen);
    const uint32_t d = dis(gen);
    std::ostringstream o;
    o << hex(a, 8) << '-' << hex(b, 4) << '-' << hex((c & 0x0fffu) | 0x4000u, 4) << '-'
      << hex((d & 0x3fffu) | 0x8000u, 4) << '-' << hex(dis(gen), 8) << hex(dis(gen), 4);
    return o.str();
}

struct PlutoBootSession {
    std::string stitcher;
    std::string stitcherParams;
    std::string sessionToken;
    int64_t refreshAtSec = 0;
};

#ifdef MUT_PLUTO_NO_BOOT_CACHE
inline bool plutoBootSessionCacheEnabled() {
    return false;
}
#else
inline bool plutoBootSessionCacheEnabled() {
    return true;
}
#endif

struct PlutoBootCacheEntry {
    PlutoBootSession session;
    int64_t validUntilSec = 0;
};

inline std::mutex& plutoBootCacheMu() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<std::string, PlutoBootCacheEntry>& plutoBootCacheMap() {
    static std::unordered_map<std::string, PlutoBootCacheEntry> m;
    return m;
}

// In-memory session boot token (never logged to disk). Per-channel plus one live-session slot
// so zaps across Pluto channels reuse the same boot response.
inline std::string plutoBootCacheKey(const std::string& channelId) {
    return "pluto-boot:" + channelId;
}

inline constexpr const char* plutoBootLiveSessionKey() {
    return "pluto-boot:__live_session__";
}

inline void plutoBootCacheClearForTests() {
    std::lock_guard<std::mutex> g(plutoBootCacheMu());
    plutoBootCacheMap().clear();
}

inline bool plutoBootCacheGet(const std::string& channelId, int64_t nowSec, PlutoBootSession& out) {
    if (!plutoBootSessionCacheEnabled() || channelId.empty() || nowSec <= 0)
        return false;
    std::lock_guard<std::mutex> g(plutoBootCacheMu());
    auto lookup = [&](const std::string& key) -> bool {
        const auto it = plutoBootCacheMap().find(key);
        if (it == plutoBootCacheMap().end() || it->second.validUntilSec <= nowSec)
            return false;
        out = it->second.session;
        return !out.stitcher.empty() && !out.sessionToken.empty();
    };
  if (lookup(plutoBootCacheKey(channelId)))
    return true;
  return lookup(plutoBootLiveSessionKey());
}

// Boot-cache TTL from the boot's refreshInSec (N12/N3). A 1..59 s value is raised to kPlutoBootRefreshFloorSec
// (a tiny/hostile value must not make every tune hit the boot endpoint; N11 honoured 1 s as-is). Values are capped
// at 8 h; a missing/zero value falls back to kPlutoBootCacheSec (3600 s).
// MUT_PLUTO_REFRESH_NO_FLOOR: N11 behaviour (1..59 s honoured). MUT_PLUTO_REFRESH_FLOOR_3600: the pre-N11 bug
// (anything <= 60 s cached for 3600 s).
#ifdef MUT_PLUTO_REFRESH_NO_FLOOR
inline constexpr int64_t kPlutoBootRefreshFloorSec = 0;
#else
inline constexpr int64_t kPlutoBootRefreshFloorSec = 60;
#endif
inline int64_t plutoBootCacheTtlSec(int64_t refreshInSec) {
#ifdef MUT_PLUTO_REFRESH_FLOOR_3600
    return refreshInSec > 60 ? std::min<int64_t>(refreshInSec, 8 * 3600) : kPlutoBootCacheSec;
#else
    if (refreshInSec <= 0)
        return kPlutoBootCacheSec;
    return std::max<int64_t>(kPlutoBootRefreshFloorSec, std::min<int64_t>(refreshInSec, 8 * 3600));
#endif
}
inline void plutoBootCachePut(const std::string& channelId, const PlutoBootSession& boot, int64_t nowSec) {
    if (!plutoBootSessionCacheEnabled() || channelId.empty() || nowSec <= 0 || boot.stitcher.empty())
        return;
    const int64_t ttl = plutoBootCacheTtlSec(boot.refreshAtSec);
    PlutoBootCacheEntry e;
    e.session = boot;
    e.validUntilSec = nowSec + ttl;
    std::lock_guard<std::mutex> g(plutoBootCacheMu());
    plutoBootCacheMap()[plutoBootCacheKey(channelId)] = e;
    plutoBootCacheMap()[plutoBootLiveSessionKey()] = e;
}

struct PlutoVariant {
    int bandwidth = 0;
    int width = 0;
    int height = 0;
    float fps = 0.f;
    std::string codecs;
    std::string uri;
    // #686: EXT-X-STREAM-INF AUDIO="<group>" -- the video rung carries NO audio of its own when the
    // master points at a separate EXT-X-MEDIA TYPE=AUDIO rendition (Nickelodeon, VEVO, ...).
    std::string audioGroup;
};

struct PlutoStreamProbe {
    int width = 0;
    int height = 0;
    float fps = 0.f;
    std::string vcodec;
    bool ok = false;
};

inline bool plutoVariantNeedsProbe(const PlutoVariant& v) {
    return v.height <= 0 || v.codecs.empty();
}

inline bool plutoMasterNeedsSegmentProbe(const std::vector<PlutoVariant>& vars) {
    if (vars.empty())
        return true;
    for (const auto& v : vars) {
        if (!plutoVariantNeedsProbe(v))
            return false;
    }
    return true;
}

inline std::string avc1CodecFromSps(const SpsInfo& sps) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "avc1.%02x%02x%02x", sps.profile_idc, 0x40, sps.level_idc);
    return buf;
}

inline PlutoStreamProbe plutoProbeFromSegmentBytes(const std::string& body) {
    PlutoStreamProbe p;
    if (body.empty())
        return p;
    const SpsInfo sps = parseFirstSpsAnnexB(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    if (!sps.valid)
        return p;
    p.width = static_cast<int>(sps.width);
    p.height = static_cast<int>(sps.height);
    p.fps = 0.f;
    p.vcodec = avc1CodecFromSps(sps);
    p.ok = true;
    return p;
}

// Bandwidth-only masters with AES-128 media: raw segments are ciphertext, so SPS
// parse fails. Admit only ladder rungs whose declared bitrate fits a conservative 720p ceiling.
inline constexpr int kPlutoBandwidthAssume720Max = 2'000'000;

inline bool plutoMediaPlaylistAes128Only(const std::string& body) {
    bool sawAes128 = false;
    for (const auto& line : splitLines(body)) {
        if (line.rfind("#EXT-X-KEY", 0) != 0)
            continue;
        const std::string l = asciiLowerCopy(line);
        if (l.find("method=aes-128") != std::string::npos)
            sawAes128 = true;
        if (l.find("sample-aes") != std::string::npos || l.find("widevine") != std::string::npos ||
            l.find("fairplay") != std::string::npos || l.find("playready") != std::string::npos ||
            l.find("streamingkeydelivery") != std::string::npos)
            return false;
    }
    return sawAes128;
}

inline PlutoStreamProbe plutoProbeConservativeFromBandwidth(int bandwidth) {
    PlutoStreamProbe p;
    if (bandwidth <= 0 || bandwidth > kPlutoBandwidthAssume720Max)
        return p;
    p.width = 1280;
    p.height = 720;
    p.fps = 0.f;
    p.vcodec = "avc1";
    if (bandwidth <= 900'000) {
        p.width = 640;
        p.height = 360;
    }
    p.ok = true;
    return p;
}

inline bool plutoFetchUrlAllowed(const std::string& url) {
    if (plutoPlayUrlAllowed(url) || plutoHlsSubresourceFetchAllowed(url))
        return true;
    return urlHost(url) == urlHost(plutoBootBaseUrl());
}

inline bool plutoAttachSessionBearer(const std::string& url, const std::string& bearer) {
    return !bearer.empty() && hostIsPlutoOwned(urlHost(url));
}

inline std::string plutoFirstNonPlutoOwnedHlsHost(const std::string& body, const std::string& mediaUrl);
inline bool plutoMediaPlaylistSubhostsPlutoOwned(const std::string& body, const std::string& mediaUrl);
inline bool plutoSessionBearerAllowedForMedia(const std::string& mediaUrl, const std::string& playlistBody);

// libav probe: unknown / zero H.264 profile or level must not pass (no Main@L3.0 default).
inline bool plutoHlsCodecProfileLevelValid(int profile, int level) {
    return profile > 0 && level > 0;
}

struct PlutoHlsProbeRequest {
    std::string mediaUrl;
    std::string sessionToken;
    std::function<bool()> cancelled;
    bool allowSessionBearer = false;
};

PlutoStreamProbe plutoHlsProbeLibav(const PlutoHlsProbeRequest& req);

using PlutoHlsProbeOverrideFn = std::function<PlutoStreamProbe(const std::string& mediaUrl, const PlutoVariant& v)>;

inline PlutoHlsProbeOverrideFn& plutoHlsProbeOverride() {
    static PlutoHlsProbeOverrideFn fn;
    return fn;
}

inline bool plutoHlsProbeAttemptEnabled() {
#ifdef MUT_PLUTO_SKIP_PROBE
    return false;
#else
    return true;
#endif
}

enum class PlutoProbeSource { Hls, Segment, BandwidthFallback, None };

struct PlutoVariantProbeResult {
    PlutoStreamProbe probe;
    PlutoProbeSource source = PlutoProbeSource::None;
    std::string note;  // redacted; for stderr
};

struct PlutoMediaFetchResult {
    bool playlistOk = false;
    bool blocked = false;
    std::string playlistBody;
    std::string segmentBody;
};

// HLS demuxer probe first; raw segment SPS; bandwidth ladder last (AES-128 ciphertext).
template <typename MediaFetchFn>
inline PlutoVariantProbeResult plutoProbeVariantForCaps(const PlutoVariant& v, const std::string& mediaUrl,
                                                          const std::string& sessionToken,
                                                          const std::function<bool()>& cancelled,
                                                          MediaFetchFn fetchMedia) {
    PlutoVariantProbeResult out;
    const PlutoMediaFetchResult m = fetchMedia();
    const bool bearerScope = plutoSessionBearerAllowedForMedia(mediaUrl, m.playlistBody);
    if (plutoHlsProbeAttemptEnabled()) {
        if (plutoHlsProbeOverride()) {
            out.probe = plutoHlsProbeOverride()(mediaUrl, v);
            if (out.probe.ok)
                out.source = PlutoProbeSource::Hls;
        } else if (plutoPlayUrlAllowed(mediaUrl)) {
            PlutoHlsProbeRequest hreq;
            hreq.mediaUrl = mediaUrl;
            hreq.sessionToken = sessionToken;
            hreq.cancelled = cancelled;
            hreq.allowSessionBearer = bearerScope && !sessionToken.empty();
            if (!bearerScope && !sessionToken.empty()) {
                const std::string off = plutoFirstNonPlutoOwnedHlsHost(m.playlistBody, mediaUrl);
                if (!off.empty())
                    out.note = "pluto bearer withheld host=" + off + " reason=non-pluto subresource";
            }
            out.probe = plutoHlsProbeLibav(hreq);
            if (out.probe.ok)
                out.source = PlutoProbeSource::Hls;
        }
    }
    if (!out.probe.ok) {
        if (out.note.empty())
            out.note = "pluto hls probe unavailable";
        if (m.playlistOk && !m.blocked) {
            PlutoStreamProbe seg = plutoProbeFromSegmentBytes(m.segmentBody);
            if (seg.ok) {
                out.probe = seg;
                out.source = PlutoProbeSource::Segment;
                out.note.clear();
            }
        }
        if (!out.probe.ok && m.playlistOk && plutoMediaPlaylistAes128Only(m.playlistBody)) {
            PlutoStreamProbe bw = plutoProbeConservativeFromBandwidth(v.bandwidth);
            if (bw.ok) {
                out.probe = bw;
                out.source = PlutoProbeSource::BandwidthFallback;
                out.note = "pluto probe bandwidth fallback";
            }
        }
    }
    return out;
}

inline bool parsePlutoBoot(const std::string& body, PlutoBootSession& out) {
    out = PlutoBootSession();
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj)
        return false;
    const airplay::Json* servers = root.get("servers");
    if (!servers || servers->type != airplay::Json::Obj)
        return false;
    out.stitcher = jsonStr(servers->get("stitcher"));
    out.stitcherParams = jsonStr(root.get("stitcherParams"));
    out.sessionToken = jsonStr(root.get("sessionToken"));
    const int refresh = static_cast<int>(jsonInt(root.get("refreshInSec")));
    if (refresh > 0)
        out.refreshAtSec = static_cast<int64_t>(refresh);
    return !out.stitcher.empty() && !out.stitcherParams.empty() && !out.sessionToken.empty();
}

inline std::string plutoBootStartUrl(const std::string& deviceId) {
    std::string u = plutoBootBaseUrl();
    u += "&clientID=";
    u += deviceId;
    u += "&deviceId=";
    u += deviceId;
    return u;
}

inline std::string joinUrl(const std::string& base, const std::string& rel) {
    if (rel.rfind("https://", 0) == 0 || rel.rfind("http://", 0) == 0)
        return rel;
    if (rel.empty())
        return base;
    if (rel[0] == '/')
        return base.substr(0, base.find('/', 8)) + rel;
    const std::string dir = base.rfind('/') != std::string::npos ? base.substr(0, base.rfind('/') + 1) : base + "/";
    return dir + rel;
}

inline std::string plutoHlsQuotedAttr(const std::string& line, const char* key) {
    const std::string needle = std::string(asciiLowerCopy(key)) + "=\"";
    const std::string low = asciiLowerCopy(line);
    const auto p = low.find(needle);
    if (p == std::string::npos)
        return {};
    const size_t start = p + needle.size();
    const auto end = line.find('"', start);
    if (end == std::string::npos)
        return {};
    return line.substr(start, end - start);
}

inline std::string plutoFirstNonPlutoOwnedHlsHost(const std::string& body, const std::string& mediaUrl) {
    for (const auto& line : splitLines(body)) {
        std::string abs;
        if (line.rfind("#EXT-X-KEY", 0) == 0) {
            const std::string uri = plutoHlsQuotedAttr(line, "URI");
            if (uri.empty())
                continue;
            abs = joinUrl(mediaUrl, uri);
        } else if (!line.empty() && line[0] != '#') {
            abs = joinUrl(mediaUrl, line);
        } else {
            continue;
        }
        if (!plutoHlsSubresourceFetchAllowed(abs))
            return urlHost(abs);
        if (!hostIsPlutoOwned(urlHost(abs)))
            return urlHost(abs);
    }
    return {};
}

inline bool plutoMediaPlaylistSubhostsPlutoOwned(const std::string& body, const std::string& mediaUrl) {
    return plutoFirstNonPlutoOwnedHlsHost(body, mediaUrl).empty();
}

// Bearer when media + all HLS subhosts are Pluto-owned (pluto.tv + plutotv.net suffixes).
inline bool plutoSessionBearerAllowedForMedia(const std::string& mediaUrl, const std::string& playlistBody) {
    if (playlistBody.empty() || !plutoPlayUrlAllowed(mediaUrl))
        return false;
    return plutoMediaPlaylistSubhostsPlutoOwned(playlistBody, mediaUrl);
}

// Value of an HLS attribute (quoted or bare) on a tag line. The key must start an attribute (after ':' or ',')
// so AUDIO= does not match inside another attribute's value or name.
inline std::string plutoHlsAttrValue(const std::string& line, const char* key) {
    const std::string needle = std::string(key) + "=";
    size_t p = 0;
    while ((p = line.find(needle, p)) != std::string::npos) {
        if (p > 0 && line[p - 1] != ':' && line[p - 1] != ',') {
            p += needle.size();
            continue;
        }
        const size_t v = p + needle.size();
        if (v >= line.size())
            return {};
        if (line[v] == '"') {
            const auto end = line.find('"', v + 1);
            return end == std::string::npos ? std::string() : line.substr(v + 1, end - v - 1);
        }
        const auto end = line.find(',', v);
        return line.substr(v, end == std::string::npos ? std::string::npos : end - v);
    }
    return {};
}

// #686: a separate audio rendition from the master (#EXT-X-MEDIA:TYPE=AUDIO,...,URI="..."). A rendition with no
// URI is muxed into the variant and is not listed.
struct PlutoAudioRendition {
    std::string groupId;
    std::string name;
    std::string uri;  // absolute (joined against the master URL)
    bool isDefault = false;
    bool autoselect = false;
};

inline std::vector<PlutoAudioRendition> parsePlutoAudioRenditions(const std::string& masterBody,
                                                                  const std::string& masterUrl) {
    std::vector<PlutoAudioRendition> out;
    for (const auto& line : splitLines(masterBody)) {
        if (line.rfind("#EXT-X-MEDIA:", 0) != 0)
            continue;
        if (plutoHlsAttrValue(line, "TYPE") != "AUDIO")
            continue;
        const std::string uri = plutoHlsAttrValue(line, "URI");
        if (uri.empty())
            continue;
        PlutoAudioRendition r;
        r.groupId = plutoHlsAttrValue(line, "GROUP-ID");
        r.name = plutoHlsAttrValue(line, "NAME");
        r.uri = joinUrl(masterUrl, uri);
        r.isDefault = plutoHlsAttrValue(line, "DEFAULT") == "YES";
        r.autoselect = plutoHlsAttrValue(line, "AUTOSELECT") == "YES";
        out.push_back(r);
    }
    return out;
}

// The audio rendition to remux with a chosen video rung: the variant's AUDIO group, DEFAULT=YES first, then
// AUTOSELECT=YES, then the first listed. nullptr = audio is muxed in the variant (or the group has no URI).
inline const PlutoAudioRendition* selectPlutoAudioRendition(const std::vector<PlutoAudioRendition>& rends,
                                                            const std::string& audioGroup) {
#ifdef MUT_PLUTO_NO_AUDIO_RENDITION
    (void)rends;
    (void)audioGroup;
    return nullptr;
#else
    if (audioGroup.empty())
        return nullptr;
    const PlutoAudioRendition* best = nullptr;
    int bestRank = -1;
    for (const auto& r : rends) {
        if (r.groupId != audioGroup)
            continue;
        const int rank = r.isDefault ? 2 : (r.autoselect ? 1 : 0);
        if (rank > bestRank) {
            best = &r;
            bestRank = rank;
        }
    }
    return best;
#endif
}

inline std::vector<PlutoVariant> parsePlutoMaster(const std::string& masterBody, const std::string& masterUrl) {
    std::vector<PlutoVariant> out;
    PlutoVariant cur;
    bool haveInf = false;
    std::istringstream in(masterBody);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            haveInf = true;
            cur = PlutoVariant();
            cur.bandwidth = tagIntValue(line, "BANDWIDTH=");
            const auto res = line.find("RESOLUTION=");
            if (res != std::string::npos) {
                const auto x = line.find('x', res + 11);
                if (x != std::string::npos) {
                    cur.width = static_cast<int>(
                        std::strtol(line.c_str() + res + 11, nullptr, 10));
                    cur.height = static_cast<int>(
                        std::strtol(line.c_str() + x + 1, nullptr, 10));
                }
            }
            cur.fps = tagFloatValue(line, "FRAME-RATE=");
            cur.audioGroup = plutoHlsAttrValue(line, "AUDIO");
            const auto cd = line.find("CODECS=\"");
            if (cd != std::string::npos) {
                const auto end = line.find('"', cd + 8);
                if (end != std::string::npos)
                    cur.codecs = line.substr(cd + 8, end - cd - 8);
            }
            continue;
        }
        if (haveInf && !line.empty() && line[0] != '#') {
            cur.uri = joinUrl(masterUrl, line);
            out.push_back(cur);
            haveInf = false;
        }
    }
    return out;
}

inline bool variantOverCap(const PlutoVariant& v) {
#ifdef MUT_PLUTO_IGNORE_CAP
    (void)v;
    return false;
#else
    if (v.height > 0 && (v.height > 720 || v.width > 1280))
        return true;
    if (v.fps > 30.5f)
        return true;
    if (v.width > 0 && v.height > 0) {
        const int64_t mbs = ((int64_t(v.width) + 15) / 16) * ((int64_t(v.height) + 15) / 16);
        if (mbs > kRemoteDirectMaxMbs)
            return true;
    }
    return false;
#endif
}

inline bool variantCodecAllowed(const PlutoVariant& v) {
    if (v.codecs.empty())
        return true;
    const std::string c = asciiLowerCopy(v.codecs);
    if (c.find("hev") != std::string::npos || c.find("hvc") != std::string::npos || c.find("vp9") != std::string::npos)
        return false;
    if (c.find("avc") != std::string::npos || c.find("mp4a") != std::string::npos || c.find("aac") != std::string::npos)
        return true;
    return c.find("avc1") != std::string::npos;
}

inline std::string plutoFirstSegmentUrl(const std::string& mediaPlaylist, const std::string& mediaUrl) {
    for (const auto& line : splitLines(mediaPlaylist)) {
        if (line.empty() || line[0] == '#')
            continue;
        return joinUrl(mediaUrl, line);
    }
    return {};
}

inline void plutoMergeProbe(PlutoVariant& v, const PlutoStreamProbe& p) {
    if (!p.ok)
        return;
    v.width = p.width;
    v.height = p.height;
    if (p.fps > 0.f)
        v.fps = p.fps;
    if (!p.vcodec.empty())
        v.codecs = p.vcodec;
}

inline bool plutoProbeFitsCaps(const PlutoStreamProbe& p, int bandwidth) {
    if (!p.ok)
        return false;
    PlutoVariant v;
    v.width = p.width;
    v.height = p.height;
    v.fps = p.fps;
    v.bandwidth = bandwidth > 0 ? bandwidth : 0;
    v.codecs = p.vcodec.empty() ? "avc1" : p.vcodec;
    if (!variantCodecAllowed(v) || variantOverCap(v))
        return false;
    const int64_t mbs = ((int64_t(v.width) + 15) / 16) * ((int64_t(v.height) + 15) / 16);
    const int fpsNum = p.fps > 0.f ? static_cast<int>(p.fps * 1000.f + 0.5f) : 0;
    if (mbs > 0 && fpsNum > 0 && mbs * fpsNum > int64_t(kRemoteDirectMaxMbps) * 1000)
        return false;
    return true;
}

inline const PlutoVariant* selectPlutoVariant(const std::vector<PlutoVariant>& vars, std::string& reason) {
    reason.clear();
    if (vars.empty()) {
        reason = "UNSUPPORTED: Pluto no HLS variants";
        return nullptr;
    }
    std::vector<const PlutoVariant*> ok;
    for (const auto& v : vars) {
        if (!variantCodecAllowed(v)) {
            reason = "UNSUPPORTED: Pluto non-H.264 variant";
            continue;
        }
        if (variantOverCap(v))
            continue;
        ok.push_back(&v);
    }
    if (ok.empty()) {
        if (reason.empty())
            reason = "UNSUPPORTED: Pluto no variant under cap";
        return nullptr;
    }
    std::sort(ok.begin(), ok.end(), [](const PlutoVariant* a, const PlutoVariant* b) {
        if (a->height != b->height)
            return a->height < b->height;
        return a->bandwidth < b->bandwidth;
    });
    const PlutoVariant* best = ok.back();
    for (auto it = ok.rbegin(); it != ok.rend(); ++it) {
        if ((*it)->height > 0 && (*it)->height <= 720) {
            best = *it;
            break;
        }
    }
    return best;
}

// Bandwidth-high-first when master tags are missing; caller supplies segment probes.
template <typename ProbeFn>
inline const PlutoVariant* selectPlutoVariantViaProbe(const std::vector<PlutoVariant>& vars, std::string& reason,
                                                      ProbeFn probeOne) {
    reason.clear();
    if (vars.empty()) {
        reason = "UNSUPPORTED: Pluto no HLS variants";
        return nullptr;
    }
    std::vector<const PlutoVariant*> order;
    order.reserve(vars.size());
    for (const auto& v : vars)
        order.push_back(&v);
    std::sort(order.begin(), order.end(), [](const PlutoVariant* a, const PlutoVariant* b) {
        return a->bandwidth > b->bandwidth;
    });
    for (const PlutoVariant* v : order) {
        PlutoStreamProbe pr = probeOne(*v);
        if (!pr.ok)
            continue;
        PlutoVariant merged = *v;
        plutoMergeProbe(merged, pr);
        if (!variantCodecAllowed(merged) || variantOverCap(merged))
            continue;
        const PlutoStreamProbe capProbe{merged.width, merged.height, merged.fps, merged.codecs, true};
        if (!plutoProbeFitsCaps(capProbe, merged.bandwidth))
            continue;
        for (const auto& cand : vars) {
            if (cand.uri == v->uri)
                return &cand;
        }
    }
    reason = "UNSUPPORTED: Pluto no variant under cap";
    return nullptr;
}

inline std::string plutoPlaylistBlockReason(const std::string& body) {
    for (const auto& line : splitLines(body)) {
        if (line.rfind("#EXT-X-KEY", 0) != 0)
            continue;
        const std::string l = asciiLowerCopy(line);
        if (l.find("widevine") != std::string::npos || l.find("fairplay") != std::string::npos ||
            l.find("playready") != std::string::npos || l.find("sample-aes") != std::string::npos ||
            l.find("streamingkeydelivery") != std::string::npos)
            return "UNSUPPORTED: Pluto DRM";
        // Plain AES-128 from the public stitcher playlist is allowed (ffmpeg decrypts in-session).
    }
    return {};
}

// Set during resolvePlutoChannelPlay so archiveGet can attach Authorization.
inline std::string& plutoAuthHeaderForThread() {
    thread_local std::string auth;
    return auth;
}

// Stitcher session token for ffmpeg remux (same thread as resolve + play job).
inline std::string& plutoRemuxSessionToken() {
    thread_local std::string tok;
    return tok;
}

// FINDINGS.md / Stage B: stitcher + siloh CDN fetch with or without bearer; ffmpeg must not
// attach Authorization via -headers (redirect could replay it off-domain).
inline bool plutoFfmpegRemuxUsesSessionBearer() {
#ifdef MUT_PLUTO_FFMPEG_KEEP_BEARER
    return true;
#else
    return false;
#endif
}

inline std::string plutoFfmpegRemuxHeaders(const std::string& sessionToken) {
    if (!plutoFfmpegRemuxUsesSessionBearer() || sessionToken.empty())
        return {};
    return "Authorization: Bearer " + sessionToken + "\r\n";
}

// loopback ffmpeg MPEG-TS from LiveRelay (YouTube + Pluto remux); not seekable.
inline bool plutoSpelunkrLiveRemuxUrl(const std::string& url) {
    return url.rfind("http://127.0.0.1:", 0) == 0 && url.find("/yt-live.ts") != std::string::npos;
}

inline int64_t plutoSpelunkrOpenStartMs(const std::string& playableUrl, int64_t scheduleOffsetMs) {
#ifdef MUT_PLUTO_LIVE_SEEK
    (void)playableUrl;
    return scheduleOffsetMs < 0 ? 0 : scheduleOffsetMs;
#else
    if (plutoSpelunkrLiveRemuxUrl(playableUrl))
        return 0;
    return scheduleOffsetMs < 0 ? 0 : scheduleOffsetMs;
#endif
}

inline bool plutoSpelunkrOpenContainerSeek(const std::string& playableUrl, int64_t startMs) {
#ifdef MUT_PLUTO_LIVE_SEEK
    (void)playableUrl;
    return startMs > 0;
#else
    if (plutoSpelunkrLiveRemuxUrl(playableUrl))
        return false;
    return startMs > 0;
#endif
}


// Spelunkr must stop the shared yt live ffmpeg relay when leaving a Pluto channel (B2 pin).
inline bool plutoSpelunkrStopsLiveRemuxOnTeardown() {
#ifdef MUT_PLUTO_SKIP_RELAY_STOP
    return false;
#else
    return true;
#endif
}

// N13 (#685 glass 1033): the first open of a Pluto live remux that fails with a relay/stitcher 5xx
// ("open: Server returned 5XX Server Error reply": the relay 502s when ffmpeg exited or stayed
// silent) used to end the tune with no retry. Bounded retry with backoff, then the existing
// terminal. MUT_PLUTO_OPEN_NO_RETRY: no retry (46a3ed51). MUT_PLUTO_OPEN_RETRY_UNBOUNDED: no cap.
// MUT_PLUTO_OPEN_RETRY_ANY: retries non-transient failures (unsupported resolution, non-H.264...).
inline constexpr int kPlutoOpenRetryMax = 2;

// N2 (FPGA Bug review of a310e5c3): a 4XX (expired jwt 401/403, unknown channel 404, 410, 429) is the stitcher's
// answer, not a blip: retrying it only delays the terminal and re-sends a rejected jwt. A 4XX anywhere in the
// reason is terminal even when the message also carries a transient needle ("... 4XX Client Error ... End of
// file"). MUT_PLUTO_OPEN_RETRY_4XX: the 4XX guard is gone and "Server returned 4" counts as transient.
inline bool plutoPrefetchFail4xx(const std::string& why) {
    static const char* kNeedles[] = {"Server returned 4", "HTTP error 4", "4XX Client Error"};
    for (const char* n : kNeedles)
        if (why.find(n) != std::string::npos)
            return true;
    return false;
}

// Glass 85629316 zap4: remux SIGKILL before codecpar is known. "unknown resolution
// (remux stall)" is the new label; leftover "unsupported resolution 0x0 fps=0/0"
// is the same stall. Real oversize (1920x1080 fps=30/1) is not unknown.
// MUT_LIVE_NO_NONEXT_RETRY: treat 0-format as a hard caps reject (tests FAIL).
inline bool plutoPrefetchUnknownFormat(const std::string& why) {
#ifdef MUT_LIVE_NO_NONEXT_RETRY
    (void)why;
    return false;
#else
    if (why.find("unknown resolution") != std::string::npos)
        return true;
    int w = 0, h = 0, n = 0, d = 0;
    if (std::sscanf(why.c_str(), "unsupported resolution %dx%d fps=%d/%d", &w, &h, &n, &d) == 4)
        return w <= 0 || h <= 0 || n <= 0 || d <= 0;
    return false;
#endif
}

inline bool plutoPrefetchFailTransient(const std::string& why) {
#ifdef MUT_PLUTO_OPEN_RETRY_ANY
    return !why.empty();
#elif defined(MUT_PLUTO_OPEN_RETRY_4XX)
    static const char* kNeedles[] = {"Server returned 5XX", "Server returned 4", "HTTP error 4", "Connection timed out",
                                     "Connection reset", "Connection refused", "Input/output error", "End of file"};
    for (const char* n : kNeedles)
        if (why.find(n) != std::string::npos)
            return true;
    return false;
#else
    if (plutoPrefetchFail4xx(why))
        return false;
    if (plutoPrefetchUnknownFormat(why))
        return true;
    // libav strings for the loopback relay / network: 5xx, reset, timeout, EOF/EIO before first data.
    // Unable to open resource / prefetch timeout: glass ff11ef73 zap02 crypto
    // stall sent=0 then PREFETCH TIMEOUT: NO NEXT (retry before 1-item fail).
    // Glass a6bfe8e0 #668: remux first-spawn pace with audio_bytes=0 then ffmpeg SIGKILL
    // (rc=-9, err='') does not match crypto needles; treat zap-window cancel and relay
    // teardown as retriable like prefetch timeout.
    // Glass 85629316 zap4: unknown/zero format after remux SIGKILL is a remux stall.
    static const char* kNeedles[] = {"Server returned 5XX", "Connection timed out", "Connection reset",
                                     "Connection refused", "Input/output error", "End of file",
                                     "Unable to open resource", "prefetch timeout", "prefetch cancelled",
                                     "ffmpeg gone", "first-spawn pace", "unknown resolution"};
    for (const char* n : kNeedles)
        if (why.find(n) != std::string::npos)
            return true;
    return false;
#endif
}

// Build the remux-retry reason string. A deferred advance timeout cancels prefetch with
// err=prefetch cancelled (not timedOut=true), so advanceTimeoutDeferred adds the same
// prefetch timeout tag the libav timed-out path uses.
inline std::string plutoOpenRetryWhy(const std::string& prefetchFail, bool prefetchTimedOut,
                                     bool advanceTimeoutDeferred) {
    std::string retryWhy = prefetchFail;
    if ((prefetchTimedOut || advanceTimeoutDeferred) &&
        retryWhy.find("prefetch timeout") == std::string::npos) {
        retryWhy = retryWhy.empty() ? std::string("prefetch timeout")
                                    : retryWhy + " prefetch timeout";
    }
    return retryWhy;
}

inline bool plutoOpenRetryAllowed(const std::string& why, int retriesSoFar) {
#ifdef MUT_PLUTO_OPEN_NO_RETRY
    (void)why;
    (void)retriesSoFar;
    return false;
#elif defined(MUT_PLUTO_OPEN_RETRY_UNBOUNDED)
    return plutoPrefetchFailTransient(why);
#else
    return retriesSoFar < kPlutoOpenRetryMax && plutoPrefetchFailTransient(why);
#endif
}

// 1 s, 2 s (retry number n >= 1).
inline int64_t plutoOpenRetryBackoffMs(int retryNo) {
    if (retryNo < 1)
        retryNo = 1;
    if (retryNo > 4)
        retryNo = 4;
    return 1000LL * retryNo;
}

}  // namespace pluto_live
}  // namespace spelunkr
}  // namespace misterplex