#pragma once
#include "airplay/ns.hpp"
// YouTube livestream / in-progress premiere (YT_LIVE).
//
// The VOD selector (yt_dlp::kResolveFormat) requires protocol=https. A livestream
// has no progressive file; yt-dlp's playable is an HLS manifest. ARM libav in
// this daemon demuxes mpegts, not hls, so the player must not open that URL.
// When YT_LIVE is on, the same -g ladder gets m3u8 fallbacks after the https
// selectors. A manifest URL is copy-remuxed by the on-card helper ffmpeg
// (hls demuxer + mpegts muxer) onto 127.0.0.1. Unset / off does not change
// the VOD argv or the play URL.
//
// An upcoming premiere has no manifest: resolve fails and the cast ends.
// A premiere that has started is the same HLS shape as a livestream.
#include "gcast/yt_dlp.hpp"
#include "libmisterplex/live_disc_rebase.hpp"
#include "log_redact.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;

namespace misterplex {
namespace airplay {
namespace yt_live {

// Live HLS audio (itags 233/234 on ISS/ABC/Sky) reports acodec=unknown.
// acodec^=mp4a makes yt-dlp return no format, so the daemon never remuxes.
// Height, avc, fps, and m3u8 stay. The VOD selector is kResolveFormat and
// still requires acodec^=mp4a. MUT_YT_LIVE_REQUIRE_MP4A puts that filter back.
#ifdef MUT_YT_LIVE_REQUIRE_MP4A
inline constexpr char kLiveHlsFormat[] =
    "b[height<=720][vcodec^=avc][acodec^=mp4a][fps<=30][protocol^=m3u8]"
    "/bv*[height<=720][vcodec^=avc][fps<=30][protocol^=m3u8]+ba[acodec^=mp4a][protocol^=m3u8]";
#else
inline constexpr char kLiveHlsFormat[] =
    "b[height<=720][vcodec^=avc][fps<=30][protocol^=m3u8]"
    "/bv*[height<=720][vcodec^=avc][fps<=30][protocol^=m3u8]+ba[protocol^=m3u8]";
#endif

// Phone lounge duration when the player has none. The player itself stays at
// duration 0 so it does not end the cast at this mark. 48 h covers a live
// MPEG-TS PTS that started away from zero. A real duration is left alone.
inline constexpr int64_t kLiveLoungeDurationMs = 48LL * 3600 * 1000;
inline constexpr int kLiveFirstByteMs = 12000;

// N13 (#685 glass 10a21b2e). CLI repro (build-host pluto-probe RESULT-STABILITY.md): at a Pluto ad break the
// ffmpeg hls->mpegts remux emits NOTHING for 30-156 s while it keeps downloading the ad pod at real-time
// rate (the pod is a different MPEG-TS program/PIDs: "no program with id N found", a/v "timestamp
// discontinuity"); the playlist keeps refreshing 200, host and jwt never change. Regular content then
// resumes by itself. The player's 20 s libav rw_timeout fires first ("read: Connection timed out") and the
// job dies with SKIP FAILED: NO NEXT. ffmpeg can also exit early on a stitcher 5xx, which the relay turned
// into a 502 that the prefetch reported as "Server returned 5XX" with no retry.
// So the relay now: (1) keeps the player's socket alive with MPEG-TS null packets while the remux is
// silent, (2) kills and re-spawns an ffmpeg that stays silent for a long time (a wedge, not an ad break:
// a fresh ffmpeg joins at the live edge and its first byte arrives in about a second), (3) re-spawns an
// ffmpeg that exits, with -output_ts_offset so the timeline keeps moving forward, (4) retries a failed
// first spawn inside the first-byte budget. All bounded; every knob has an env override for the tests.
inline constexpr int kLiveKeepaliveMs = 1000;
inline constexpr int kLiveStallRestartMs = 120000;  // silent this long mid-stream: restart ffmpeg (ad pods stall 30-156 s)
inline constexpr int kLiveFirstStallRestartMs = 6000;  // no first byte yet: restart once
inline constexpr int kLiveRespawnMax = 10;          // spawns after the first, per window
inline constexpr int kLiveRespawnWindowMs = 120000;
inline constexpr int kLiveRespawnBackoffMs = 400;
inline constexpr int kLiveFirstSpawnsMax = 3;
// N16 (#685 glass 067d79f7): on dial 1062 the remux child died (rc=-6, a glibc early-init abort) about 1.5 s
// after each spawn, having written only PAT/PMT (376 B): seven spawns in 14 s, a storm. A child that did not
// stay up for kLiveHealthyMs is a FAILED spawn however many bytes it wrote: the back-off doubles per consecutive
// failure up to a cap, the failures are capped, and the stand-by card stays up the whole time (a short-lived
// child's bytes are not "the stream is back").
inline constexpr int kLiveHealthyMs = 3000;           // a child must stay up this long to count as healthy
inline constexpr int kLiveRespawnBackoffCapMs = 15000;  // exponential back-off cap
inline constexpr int kLiveRespawnFailMax = 8;          // consecutive failed spawns, then the relay ends
// N17 (#685, reviewer review of N16): a child that lives just over kLiveHealthyMs (3.2 s), dies, and repeats every ~12 s
// resets the consecutive-failure count each time and spends only 5 of the 10 respawns per 120 s window, so it
// is never capped. A slowly decaying failure score closes that gap: every death of a child that lived less than
// kLiveScoreShortMs adds 1, the score decays by 1 per kLiveScoreDecayMs, and at kLiveScoreMax the relay ends (the player
// then keeps the card and moves on to the next channel). A child that lives longer than kLiveScoreShortMs (a real
// ad-pod stall restart) adds nothing. With the defaults the 12 s cycle trips after ~10 deaths (~2 min); one
// short death every 30 s or slower never does.
inline constexpr int kLiveScoreShortMs = 30000;
inline constexpr int kLiveScoreDecayMs = 30000;
inline constexpr int kLiveScoreMax = 6;
// #686 x relay (build-host real Pluto, vevo-90s, 2026-11-18): with the audio rendition as a 2nd ffmpeg input, an ad boundary
// (the ad video segments use other PIDs / a reset timebase) makes ffmpeg stop emitting VIDEO for good while the AUDIO
// input keeps flowing (video 300 pkts/10 s -> 0, audio 469 pkts/10 s unbroken for 160 s). Bytes keep arriving, so the
// whole-stream silence watchdog never fires and the picture stays frozen with sound. For a split-audio relay the
// child is therefore also restarted when no VIDEO packet came out for this long. ffmpeg's mpegts muxer puts the first
// mapped stream (0:v:0) on PID 0x100. Normal video gaps are 3-11 s (one burst per 5 s segment).
inline constexpr int kLiveVideoStallRestartMs = 30000;
inline constexpr unsigned kLiveVideoPid = 0x100;
inline constexpr size_t kTsPacket = 188;

// N14 stand-by card trigger. The byte gap that means "ad pod dropped" is NOT 2 s: ffmpeg emits one
// burst per 5 s segment, so normal gaps are 3-11 s (build-host stability logs: 268 gaps >= 2 s in 24 min on
// 90s-kids, max normal 11.1 s) while every ad-pod stall was >= 30 s. 15 s sits in the empty 12-30 s
// band. An ffmpeg exit / re-spawn mid-stream shows the card at once.
inline constexpr int kLiveAdCardMs = 15000;
inline constexpr int kLiveAdCardHoldMs = 4000;  // after bytes resume: clear the card even if no frame is reported
enum class LiveGap { Silent, Resumed, Clear };
// N15: why the card went up / came down (the log line and the callback carry it).
enum class LiveGapWhy { FrameStall, RelaySilence, Respawn, PictureAdvance, HoldTimeout, RelayStop };
inline const char* liveGapWhyName(LiveGapWhy w) {
    switch (w) {
    case LiveGapWhy::FrameStall: return "frame_stall";
    case LiveGapWhy::RelaySilence: return "relay_silence";
    case LiveGapWhy::Respawn: return "respawn";
    case LiveGapWhy::PictureAdvance: return "picture_advance";
    case LiveGapWhy::HoldTimeout: return "hold_timeout";
    case LiveGapWhy::RelayStop: return "relay_stop";
    }
    return "?";
}

inline int liveKnobMs(const char* env, int def, int lo, int hi) {
    const char* e = std::getenv(env);
    if (!e || !e[0])
        return def;
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || *end != 0 || v < lo || v > hi)
        return def;
    return static_cast<int>(v);
}

inline bool ytLiveEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n) {
        unsigned char c = static_cast<unsigned char>(e[n]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        buf[n] = static_cast<char>(c);
    }
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// YT_LIVE (misterplex.conf) / MPX_YT_LIVE (env wins). Default off: the VOD
// selector and play URL stay on the tip path.
inline bool ytLiveEnabled() {
#ifdef MUT_YT_LIVE_IGNORED
    return false;
#else
    if (const char* e = std::getenv("MPX_YT_LIVE"); e && e[0])
        return ytLiveEnvOn(e);
    return ytLiveEnvOn(std::getenv("YT_LIVE"));
#endif
}

// https selectors first, then HLS. yt-dlp '/' is fallback: a VOD hit still
// matches kResolveFormat and never reaches the m3u8 selectors.
inline std::string liveEnabledFormat() {
    return std::string(yt_dlp::kResolveFormat) + "/" + kLiveHlsFormat;
}

inline bool plainHttpUrl(const std::string& u) {
    if (!(u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) || u.size() <= 8 || u.size() > 4096)
        return false;
    for (unsigned char c : u)
        if (c <= ' ' || c == 0x7f)
            return false;
    return true;
}

inline std::string asciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// YouTube live manifests are hls_variant / hls_playlist URLs, not always .m3u8.
inline bool hlsManifestUrl(const std::string& url) {
    if (!plainHttpUrl(url))
        return false;
    const std::string l = asciiLower(url);
    return l.find(".m3u8") != std::string::npos || l.find("/hls_variant/") != std::string::npos ||
           l.find("/hls_playlist/") != std::string::npos ||
           l.find("mime=application%2fx-mpegurl") != std::string::npos ||
           l.find("mime=application/x-mpegurl") != std::string::npos ||
           l.find("playlist_type=live") != std::string::npos;
}

// Live edge. A lounge currentTime is seconds-into-the-broadcast, not a file offset.
inline int64_t livePlayStartMs(bool live, int64_t loungeMs) {
#ifdef MUT_YT_LIVE_KEEP_OFFSET
    (void)live;
    return loungeMs;
#else
    if (live)
        return 0;
    return loungeMs;
#endif
}

// The player opens the loopback MPEG-TS, never the manifest.
inline std::string livePlayable(bool remux, const std::string& direct, const std::string& relay) {
#ifdef MUT_YT_LIVE_PLAY_DIRECT
    (void)remux;
    (void)relay;
    return direct;
#else
    if (remux && !relay.empty())
        return relay;
    return direct;
#endif
}

inline int64_t liveLoungeDurationMs(bool live, int64_t reported) {
#ifdef MUT_YT_LIVE_ZERO_LOUNGE_DUR
    (void)live;
    return reported;
#else
    if (live && reported <= 0)
        return kLiveLoungeDurationMs;
    return reported;
#endif
}

inline bool fileExecutable(const std::string& path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

// MPX_YT_LIVE_FFMPEG wins. Else the on-card helper binary (hls demuxer + mpegts muxer).
inline std::string findLiveFfmpeg() {
    if (const char* e = std::getenv("MPX_YT_LIVE_FFMPEG"); e && e[0])
        return fileExecutable(e) ? std::string(e) : std::string();
    if (const char* e = std::getenv("YT_LIVE_FFMPEG"); e && e[0])
        return fileExecutable(e) ? std::string(e) : std::string();
    const char* cands[] = {"/media/fat/misterplex/bin/ffmpeg", "/media/fat/linux/misterplex/bin/ffmpeg", nullptr};
    for (int i = 0; cands[i]; ++i)
        if (fileExecutable(cands[i]))
            return cands[i];
    return {};
}

struct LiveSource {
    std::string ffmpeg;
    std::string videoUrl;
    std::string audioUrl;
    // Optional HTTP headers for ffmpeg's -headers (e.g. Pluto stitcher Bearer).
    std::string ffmpegHeaders;
    // Tighter protocol whitelist (no cleartext http) for Pluto stitcher remux.
    bool httpsOnlyProto = false;
    bool plutoHlsRemux = false;
};

inline std::string liveFfmpegArgsLogLine(const std::vector<std::string>& a) {
    std::string line;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i)
            line += ' ';
        line += misterplex::redactUrlTokens(a[i]);
    }
    return line;
}

// Same whitelist the on-card helper uses for an http(s) input.
inline constexpr char kFfmpegNetProto[] = "http,https,tcp,tls,crypto";
inline constexpr char kFfmpegNetProtoHttps[] = "https,tcp,tls,crypto,hls";
inline constexpr char kFfmpegNetProtoPluto[] = "https,tcp,tls,crypto,hls";

// v1.0.1 glass (device, 2026-11-23 09:18 CT, Pluto 1062 MTV): a muxed Pluto variant whose live edge carried no
// audio stream made the REQUIRED `-map 0:a:0` (ec11e6bb) fatal: "Stream map '' matches no streams ...
// Failed to set value '0:a:0' for option 'map'", rc=234, sent=0 on every spawn, then 5XX / PREFETCH TIMEOUT.
// The audio map is a ladder per relay, chosen by the error the shipped ffmpeg 8.1.2 really prints (reviewer review
// of af87b021, F2):
//   * "Stream map '' matches no streams" (no audio stream in the input at all): a deeper probe never adds a
//     stream that only appears in a later segment, so go straight to the OPTIONAL map (video-only remux; the
//     relay lives). A missing audio stream must never kill the relay.
//   * "[mpegts] sample rate not set" before the first byte (ec11e6bb's case: the PMT declares AAC, its first
//     packet is late): retry the SAME required map with a deeper probe, which does find the late AAC.
// Split (#686) keeps `1:a:0?` at every step; YouTube keeps `0:a:0?`.
enum class LiveAudioMap { Required = 0, RequiredDeepProbe = 1, Optional = 2 };

inline const char* liveAudioMapName(LiveAudioMap m) {
    return m == LiveAudioMap::Required ? "required" : (m == LiveAudioMap::RequiredDeepProbe ? "required-deep-probe" : "optional");
}

// ffmpeg's option-parse error for a non-optional -map that names no stream of its input (rc 234 / -EINVAL).
inline bool liveErrAudioMapMissing(const std::string& errTail) {
#ifdef MUT_PLUTO_AMAP_NO_DETECT
    (void)errTail;
    return false;
#else
    if (errTail.find("matches no streams") == std::string::npos)
        return false;
    return errTail.find("'0:a:0'") != std::string::npos || errTail.find("'1:a:0'") != std::string::npos;
#endif
}

// The muxer's header failure when a mapped audio stream has no parameters yet (late first AAC packet, rc 234).
inline bool liveErrAudioRateUnset(const std::string& errTail) {
    return errTail.find("sample rate not set") != std::string::npos;
}

// Next rung after a pre-first-byte failure: `mapMissing` (no audio stream) -> Optional; `rateUnset` (late AAC) on the
// plain required map -> RequiredDeepProbe. Anything else keeps the map. Optional is terminal for the ladder (only
// the bounded audio re-check below goes back to Required).
inline LiveAudioMap nextLiveAudioMap(LiveAudioMap m, bool mapMissing, [[maybe_unused]] bool rateUnset) {
    if (mapMissing && m != LiveAudioMap::Optional) {
#ifdef MUT_PLUTO_AMAP_MISSING_VIA_DEEP
        if (m == LiveAudioMap::Required)
            return LiveAudioMap::RequiredDeepProbe;   // mutant: af87b021's rung 2 on an error it cannot help
#endif
        return LiveAudioMap::Optional;
    }
#ifndef MUT_PLUTO_AMAP_SKIP_DEEP_PROBE
    if (rateUnset && m == LiveAudioMap::Required)
        return LiveAudioMap::RequiredDeepProbe;
#endif
    return m;
}

// F1 (reviewer review of af87b021): an OPTIONAL-map child that started on an audio-less stretch stays video-only for
// life (ffmpeg cannot add an output stream after the header), so audio that comes back later is never remuxed. While
// the relay is on the optional map, a child that has put no packet on the muxer audio PID (0x101: the 2nd mapped
// stream) for kLiveAudioRecheckMs is killed and respawned ONCE with the required map. If the input still has no audio,
// that required spawn fails fast ("matches no streams") and the ladder drops back to optional. Bounded: at most
// kLiveAudioRecheckMax re-checks per relay, at least kLiveAudioRecheckGapMs apart, and only with 2 respawn slots left
// in the budget (the re-check never ends a working video-only relay).
// A re-check kill is intentional (not a failed spawn): it adds to neither the failure streak nor the failure score.
// A re-check child that meets late AAC ("sample rate not set") gets the deep-probe rung; one that dies young for any
// other reason (or whose deep probe fails too) goes straight back to the optional map, so a re-check can never end a
// working video-only relay through the failure caps (reviewer review of 139c3a35, finding 4).
inline constexpr unsigned kLiveAudioPid = 0x101;
inline constexpr int kLiveAudioRecheckMs = 10000;
inline constexpr int kLiveAudioRecheckGapMs = 30000;
inline constexpr int kLiveAudioRecheckMax = 3;

// Deeper probe on the muxed Pluto input after a "sample rate not set" failure: 10 s / 10 MB lets a late first ADTS frame
// (behind large Hybrik video PES) reach find_stream_info before the required map is checked.
inline constexpr const char* kLiveDeepProbeAnalyzeUs = "10000000";
inline constexpr const char* kLiveDeepProbeSizeBytes = "10000000";

// fix5 diag (glass ef980632 r1: 5 holes at pts_jump splices, no ffmpeg evidence at
// -loglevel error). The HLS demuxer's "Failed to open segment" skip is a WARNING.
// MPX_LIVE_FFMPEG_LOGLEVEL wins. A lab run (MPX_MEMSTAT_SEC set) gets warning.
// Otherwise it stays error.
inline const char* liveFfmpegLogLevel() {
    if (const char* e = std::getenv("MPX_LIVE_FFMPEG_LOGLEVEL"); e && (std::strcmp(e, "warning") == 0 ||
                                                                       std::strcmp(e, "info") == 0 ||
                                                                       std::strcmp(e, "verbose") == 0 ||
                                                                       std::strcmp(e, "error") == 0))
        return e;
    if (const char* m = std::getenv("MPX_MEMSTAT_SEC"); m && m[0] && std::strcmp(m, "0") != 0)
        return "warning";
    return "error";
}

// The ffmpeg stderr lines worth a daemon log line in a lab run (segment skips,
// reconnects, TLS pulls, timestamp discontinuities). Not the per-packet noise.
inline bool liveFfmpegDiagLine(const std::string& l) {
    static const char* const keys[] = {"Failed to open segment", "keepalive request failed", "Error in the pull",
                                       "skipping", "timestamp discontinuity", "Unable to open", "HTTP error",
                                       "expired from playlist", "Will reconnect", "Connection reset", "Invalid data",
                                       "End of file", "Immediate exit"};
    for (const char* k : keys)
        if (l.find(k) != std::string::npos)
            return true;
    return false;
}

// tsOffsetSec > 0 only on a re-spawn: the new ffmpeg starts its mpegts at 0, so the relay shifts it
// by the seconds already streamed to keep the player's timeline monotonic (MUT_LIVE_NO_TS_OFFSET).
inline std::vector<std::string> liveFfmpegArgs(const LiveSource& src, double tsOffsetSec = 0.0,
                                               LiveAudioMap audioMap = LiveAudioMap::Required) {
    std::vector<std::string> a;
    const char* proto = src.plutoHlsRemux     ? kFfmpegNetProtoPluto
                        : src.httpsOnlyProto ? kFfmpegNetProtoHttps
                                             : kFfmpegNetProto;
    const bool split = !src.audioUrl.empty() && src.audioUrl != src.videoUrl;
    a.push_back(src.ffmpeg);
    a.push_back("-nostdin");
    a.push_back("-hide_banner");
    a.push_back("-loglevel");
    a.push_back(liveFfmpegLogLevel());
    a.push_back("-protocol_whitelist");
    a.push_back(proto);
#ifndef MUT_LIVE_NO_DISC_REBASE
    // Play-all ads: keep remuxing through HLS discontinuities (new clip/program PTS)
    // instead of stalling the copy muxer. Player-side rebase still matches a tune.
    if (src.plutoHlsRemux) {
        a.push_back("-fflags");
        a.push_back("+genpts+discardcorrupt");
    }
#endif
    if (!src.ffmpegHeaders.empty()) {
        a.push_back("-headers");
        a.push_back(src.ffmpegHeaders);
    }
    // Per-input options (ffmpeg applies them to the next -i only): the Pluto audio rendition is a second HLS
    // input and needs the same reconnect/timeout guards as the video input (#686).
    auto plutoInputOpts = [&]() {
#ifndef MUT_PLUTO_NO_RECONNECT
        // Segment/playlist HTTP drops (CDN RemoteDisconnected seen in the CLI repro) reconnect inside
        // ffmpeg instead of ending the remux; rw_timeout bounds a dead socket.
        a.push_back("-reconnect");
        a.push_back("1");
        a.push_back("-reconnect_streamed");
        a.push_back("1");
        a.push_back("-reconnect_on_network_error");
        a.push_back("1");
        a.push_back("-reconnect_delay_max");
        a.push_back("4");
        a.push_back("-rw_timeout");
        a.push_back("30000000");
#endif
    };
    // Per-input: start both demuxed Pluto HLS playlists at the latest segment so a default
    // live_start_index=-3 cannot pick different program times (AVSYNC-702c3ce3-1120).
    auto plutoAvAlignInputOpts = [&]() {
#ifndef MUT_PLUTO_NO_AV_ALIGN
        if (src.plutoHlsRemux && split) {
            a.push_back("-live_start_index");
            a.push_back("-1");
        }
#endif
    };
    if (src.plutoHlsRemux)
        plutoInputOpts();
    plutoAvAlignInputOpts();
    if (src.plutoHlsRemux && !split && audioMap == LiveAudioMap::RequiredDeepProbe) {
        a.push_back("-analyzeduration");
        a.push_back(kLiveDeepProbeAnalyzeUs);
        a.push_back("-probesize");
        a.push_back(kLiveDeepProbeSizeBytes);
    }
    a.push_back("-i");
    a.push_back(src.videoUrl);
    if (split) {
        a.push_back("-protocol_whitelist");
        a.push_back(proto);
        if (src.plutoHlsRemux) {
            if (!src.ffmpegHeaders.empty()) {
                a.push_back("-headers");
                a.push_back(src.ffmpegHeaders);
            }
#ifndef MUT_PLUTO_AUDIO_INPUT_NO_OPTS
            plutoInputOpts();
#endif
        }
        plutoAvAlignInputOpts();
        a.push_back("-i");
        a.push_back(src.audioUrl);
    }
    a.push_back("-map");
    a.push_back("0:v:0");
    a.push_back("-map");
#ifdef MUT_PLUTO_MUXED_OPTIONAL_AUDIO
    a.push_back(split ? "1:a:0?" : "0:a:0?");
#else
    // Pluto muxed (Hybrik MPEG-TS HLS, MTV 1062): required 0:a:0. The optional `?` lets
    // ffmpeg start a video-only remux after a video-first HLS probe, so the player
    // loopback never sees AAC (picture living, HDMI stuck at the -81.9 floor). Split
    // keeps 1:a:0?. YouTube muxed keeps 0:a:0? (a livestream can be video-only).
    a.push_back(split ? "1:a:0?"
                      : (src.plutoHlsRemux && audioMap != LiveAudioMap::Optional ? "0:a:0" : "0:a:0?"));
#endif
    a.push_back("-sn");
    a.push_back("-dn");
    a.push_back("-c");
    a.push_back("copy");
    a.push_back("-bsf:v");
    a.push_back("h264_mp4toannexb");
    a.push_back("-f");
    a.push_back("mpegts");
    a.push_back("-muxdelay");
    a.push_back("0");
#ifndef MUT_PLUTO_NO_AV_ALIGN
    // Keep the common Pluto PTS/PCR timeline across the two live inputs, then rebase the
    // muxed output to 0. FIRST spawn only (tsOffsetSec == 0): Gate C Nick A/V needs this.
    // On a video_stall / TLS-EOF RESPAWN, copyts + -output_ts_offset together left glass Gate B
    // with a brief picture_advance then permanent freeze (no second ffmpeg gone; player socket
    // died on the remux). Drop copyts on respawn so each input zeroes independently and
    // -output_ts_offset can keep the player timeline monotonic (80bb5a58 recovery path).
    // Split interleave: FIRST spawn / zap is 0 so the first video AU beats the
    // 3000 ms RC_PIC watchdog (glass a1c0af44: 1 s wait, no picture, SIGKILL).
    // RESPAWN is 1 s: the 5.00 s HDMI starve grid is post-stall (glass B t=664+).
    // A 1 s bound still flushes at an ad-edge PTS jump. Muxed Pluto keeps 0 (GATE7).
    // Glass D 77fa2b7e: argv 0 on first spawn still left a 1.12 s HDMI hole at
    // remux ~70 s (soak 30.877). That cushion is player-side AFTER the first key
    // video AU (firstSpawnPaceAudioBytes, split-rendition only); do not put it on
    // this argv (C death / zap vs B). Not FPGA first_picture.
    // Glass F b36a6c5a: a blocking fill after the first key added zap latency
    // and left audio_bytes=0, so the next 1.39 s fetch hole at t=221.8 s hit HDMI.
    // G fills the watermark without holding video. Glass G then held PCM while
    // video played: HDMI idle floor from t=0. H FIFOs A+V after the first key.
    // Glass I (4beb780b r1): a Vevo track splice with no pts_jump left AAC/SWR
    // stale (HDMI silent from t=23.234); zap return killed /yt-live.ts while
    // the old session still read it. Player recovers the audio gap and stops
    // the reader before replacing the remux.
    if (src.plutoHlsRemux && split) {
#ifndef MUT_PLUTO_RESPAWN_KEEP_COPYTS
        if (tsOffsetSec <= 0.0)
#endif
        {
            a.push_back("-copyts");
            a.push_back("-avoid_negative_ts");
            a.push_back("make_zero");
            a.push_back("-muxpreload");
            a.push_back("0");
            a.push_back("-max_interleave_delta");
            a.push_back(misterplex::live_disc::muxInterleaveDeltaArgFirstSpawn());
        }
#ifndef MUT_PLUTO_RESPAWN_KEEP_COPYTS
        else {
            // Respawn drops copyts; keep the mpegts flush so GATE7 still sees AAC.
            // 1 s wait paces video to the ~5 s Pluto audio EXTINF fetch.
            a.push_back("-muxpreload");
            a.push_back("0");
            a.push_back("-max_interleave_delta");
            a.push_back(misterplex::live_disc::muxInterleaveDeltaArg());
        }
#endif
    }
#endif
#ifndef MUT_PLUTO_MUXED_NO_INTERLEAVE
    // Muxed Pluto (single -i, MTV 1062): flush audio immediately. Default mpegts
    // interleave holds AAC behind large Hybrik video PES; the player's GATE7
    // 64-packet / 1 s probe then opens video-only and never revisits audio.
    // Split already gets these flags on first spawn (with copyts). Apply on every
    // muxed spawn, including respawn (no copyts).
    if (src.plutoHlsRemux && !split) {
        a.push_back("-muxpreload");
        a.push_back("0");
        a.push_back("-max_interleave_delta");
        a.push_back("0");
    }
#endif
#ifndef MUT_LIVE_NO_TS_OFFSET
    if (tsOffsetSec > 0.0) {
        char off[32];
        std::snprintf(off, sizeof off, "%.3f", tsOffsetSec);
        a.push_back("-output_ts_offset");
        a.push_back(off);
    }
#else
    (void)tsOffsetSec;
#endif
    a.push_back("pipe:1");
    return a;
}

inline bool writeAllFd(int fd, const char* p, size_t n) {
    while (n) {
        const ssize_t w = ::write(fd, p, n);
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// #687 ( lab device, 2026-11-18): the card's ffmpeg is a STATIC glibc build (80baac9d). Its mpegts demuxer decodes the DVB SDT
// service name with iconv_open("UTF-8","ISO6937"), the static glibc then dlopen()s /usr/lib/gconv/ISO_6937.so, which pulls the
// card's dynamic libc 2.31 and aborts ("Fatal glibc error: dl-call-libc-early-init.c:37 ... sym != NULL", rc=-6) about 1.6 s
// after the spawn, before a single byte. Pluto's MTV clip library is transcoded by Hybrik and every such segment carries an SDT
// (service_provider "Hybrik", service_name "Service"), so channel 1062 (MTV) failed on every tune. GCONV_PATH makes glibc skip the
// system gconv cache and read this directory's gconv-modules FIRST; the alias lines below send every charset the mpegts
// demuxer can ask for to the BUILT-IN ASCII converter, so no gconv module is ever dlopen()ed. The service name is not used by
// anything (-map picks streams by index), so a lossy conversion costs nothing. Only the relay's ffmpeg gets the variable.
inline constexpr char kGconvStubModules[] =
    "# MisterPlex: route DVB SDT string charsets to the built-in ASCII converter (no gconv dlopen; see yt_live.hpp)\n"
    "alias\tISO6937//\tANSI_X3.4-1968//\n"
    "alias\tISO-6937//\tANSI_X3.4-1968//\n"
    "alias\tISO_6937//\tANSI_X3.4-1968//\n"
    "alias\tISO6937-2//\tANSI_X3.4-1968//\n"
    "alias\tISO-6937-2//\tANSI_X3.4-1968//\n"
    "alias\tISO_6937-2//\tANSI_X3.4-1968//\n"
    "alias\tUTF-16BE//\tANSI_X3.4-1968//\n"
    "alias\tUTF-16//\tANSI_X3.4-1968//\n"
    "alias\tGB2312//\tANSI_X3.4-1968//\n"
    "alias\tBIG5//\tANSI_X3.4-1968//\n"
    "alias\tEUC-KR//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-1//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-2//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-3//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-4//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-5//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-6//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-7//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-8//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-9//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-10//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-11//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-13//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-14//\tANSI_X3.4-1968//\n"
    "alias\tISO-8859-15//\tANSI_X3.4-1968//\n";

// Directory the stub lives in. MPX_GCONV_STUB_DIR overrides it (tests); "off" disables the stub.
inline std::string gconvStubDir() {
    const char* e = std::getenv("MPX_GCONV_STUB_DIR");
    if (e && e[0])
        return std::strcmp(e, "off") == 0 ? std::string() : std::string(e);
    return "/tmp/mpx-gconv-stub";
}

// browse-lane P2-1 (REVIEW-AV-MTV-1120): if the stub cannot be created, log once per 60 s so glass/ops
// can see MTV 1062 will still abort rc=-6. MPX_GCONV_STUB_DIR=off is an intentional disable and stays quiet.
inline void logGconvStubFailure(const char* why) {
    using clock = std::chrono::steady_clock;
    static std::mutex mu;
    static clock::time_point last{};
    const auto now = clock::now();
    std::lock_guard<std::mutex> lk(mu);
    if (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(60))
        return;
    last = now;
    std::fprintf(stderr, "misterplexd: live relay: gconv stub unavailable (%s); MTV 1062 may abort rc=-6\n",
                 why ? why : "unknown");
}

// Creates (or refreshes) <dir>/gconv-modules. Returns the directory, or "" when it cannot be made safely (the relay then
// runs ffmpeg exactly as before). A pre-existing path that is not a plain directory owned by this uid is refused.
inline std::string ensureGconvStub() {
#ifdef MUT_LIVE_NO_GCONV_STUB
    return std::string();
#else
    const std::string dir = gconvStubDir();
    if (dir.empty())
        return std::string();  // MPX_GCONV_STUB_DIR=off
    if (dir[0] != '/') {
        logGconvStubFailure("path not absolute");
        return std::string();
    }
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0) {
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            logGconvStubFailure("mkdir failed");
            return std::string();
        }
        if (::lstat(dir.c_str(), &st) != 0) {
            logGconvStubFailure("lstat failed");
            return std::string();
        }
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
        logGconvStubFailure("refused path (not a dir owned by this uid)");
        return std::string();
    }
    const std::string file = dir + "/gconv-modules";
    const std::string tmp = dir + "/.gconv-modules." + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        logGconvStubFailure("open failed");
        return std::string();
    }
    const bool ok = writeAllFd(fd, kGconvStubModules, sizeof kGconvStubModules - 1);
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), file.c_str()) != 0) {
        ::unlink(tmp.c_str());
        logGconvStubFailure("write/rename failed");
        return std::string();
    }
    return dir;
#endif
}

// The relay ffmpeg's environment: the daemon's own, with GCONV_PATH pointing at the stub (any inherited GCONV_PATH is dropped).
inline std::vector<std::string> liveFfmpegEnv(const std::string& gconvDir) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp(*e, "GCONV_PATH=", 11) != 0)
            env.emplace_back(*e);
    if (!gconvDir.empty())
        env.push_back("GCONV_PATH=" + gconvDir);
    return env;
}

// Glass 47abfbbb: SO_SNDTIMEO is 1 s. After a disc rebase the HDMI client can pause
// (ring Full / decoder resync) longer than that; a single timed-out send used to
// end the relay with client_write and leave a permanent dead decoder. Retry on
// EAGAIN/EWOULDBLOCK/ETIMEDOUT/EINTR up to this budget. EPIPE/ECONNRESET still
// end the client at once (the listen loop then accepts a reconnect).
inline constexpr int kLiveClientWriteRetryMs = 20000;

inline bool writeAll(int fd, const char* p, size_t n) {
#ifdef MUT_LIVE_NO_CLIENT_RETRY
    while (n) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
#else
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kLiveClientWriteRetryMs);
    while (n) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w > 0) {
            p += w;
            n -= static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            pollfd pf{};
            pf.fd = fd;
            pf.events = POLLOUT;
            ::poll(&pf, 1, 100);
            continue;
        }
        return false;  // EPIPE / ECONNRESET: genuine client_gone
    }
    return true;
#endif
}

// One loopback MPEG-TS. The manifest stays in this process; the player URL
// does not carry it. One ffmpeg at a time. stop() kills the process group.
class LiveRelay {
public:
    std::string start(const LiveSource& src, std::string& err) {
        stop();
        err.clear();
        if (!hlsManifestUrl(src.videoUrl)) {
            err = "not an hls manifest";
            return {};
        }
        if (!src.audioUrl.empty() && !plainHttpUrl(src.audioUrl)) {
            err = "bad audio url";
            return {};
        }
        if (!fileExecutable(src.ffmpeg)) {
            err = "ffmpeg not executable";
            return {};
        }
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            err = "socket failed";
            return {};
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 8) != 0) {
            ::close(fd);
            err = "bind failed";
            return {};
        }
        sockaddr_in bound{};
        socklen_t bl = sizeof bound;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bl) != 0) {
            ::close(fd);
            err = "getsockname failed";
            return {};
        }
        const int port = ntohs(bound.sin_port);
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_.store(false);
            listenFd_ = fd;
            src_ = src;
            servedHeader_ = false;  // relay thread not running (stop() joined it)
        }
        misterplex::live_disc::splitLiveRemuxActive().store(!src.audioUrl.empty() &&
                                                            src.audioUrl != src.videoUrl);
        thread_ = std::thread([this] { loop(); });
        return "http://127.0.0.1:" + std::to_string(port) + "/yt-live.ts";
    }

    void stop() {
        std::thread joining;
        int listenFd = -1;
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_.store(true);
            listenFd = listenFd_;
            listenFd_ = -1;
            pid = ffmpegPid_.exchange(-1);
            if (thread_.joinable())
                joining = std::move(thread_);
        }
        if (listenFd >= 0) {
            ::shutdown(listenFd, SHUT_RDWR);
            ::close(listenFd);
        }
        if (pid > 0)
            ::killpg(pid, SIGKILL);
        if (joining.joinable())
            joining.join();
        pid = ffmpegPid_.exchange(-1);
        if (pid > 0)
            ::killpg(pid, SIGKILL);
        misterplex::live_disc::splitLiveRemuxActive().store(false);
    }

    ~LiveRelay() { stop(); }

    // N14: the daemon shows / clears the stand-by card from these events (called on the relay thread).
    void setGapCallback(std::function<void(LiveGap, LiveGapWhy)> cb) {
        std::lock_guard<std::mutex> lk(cbMu_);
        gapCb_ = std::move(cb);
    }
    // N15: the player's picture clock (media position, ms; -1 = not watching: paused / not playing). The glass
    // of N14 showed frozen ads with the relay still alive; the card also goes up when this clock does not move
    // for MPX_AD_CARD_MS while bytes flow, and (once armed by a first advance) clears only when it moves again.
    void setFrameProbe(std::function<int64_t()> probe) {
        std::lock_guard<std::mutex> lk(cbMu_);
        frameProbe_ = std::move(probe);
    }
    // Fired on the relay thread after a successful ffmpeg respawn (not the first spawn).
    // MediaPlayer::rejoinLiveRelayAudio re-Begins MrAudio / keeps OwenbRing PCM attached.
    void setRespawnCallback(std::function<void()> cb) {
        std::lock_guard<std::mutex> lk(cbMu_);
        respawnCb_ = std::move(cb);
    }

private:
    void fireRespawn() {
#ifndef MUT_HDMI_AUDIO_NO_RESPAWN_CB
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(cbMu_);
            cb = respawnCb_;
        }
        if (cb)
            cb();
#endif
    }
    void loop() {
        for (;;) {
            int listenFd = -1;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_.load())
                    return;
                listenFd = listenFd_;
            }
            if (listenFd < 0)
                return;
            pollfd pf{};
            pf.fd = listenFd;
            pf.events = POLLIN;
            const int pr = ::poll(&pf, 1, 200);
            if (pr <= 0)
                continue;
            const int client = ::accept(listenFd, nullptr, nullptr);
            if (client < 0)
                continue;
            serve(client);
            ::close(client);
        }
    }

    void serve(int client) {
        timeval snd{};
        snd.tv_sec = 1;
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192 && !stop_.load()) {
            pollfd pf{};
            pf.fd = client;
            pf.events = POLLIN;
            if (::poll(&pf, 1, 200) <= 0)
                continue;
            const ssize_t n = ::recv(client, buf, sizeof buf, 0);
            if (n <= 0)
                return;
            req.append(buf, buf + n);
        }
        if (req.compare(0, 4, "GET ") != 0) {
            static const char k[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(client, k, sizeof k - 1);
            return;
        }
        const auto sp = req.find(' ', 4);
        std::string path = sp == std::string::npos ? std::string() : req.substr(4, sp - 4);
        const auto q = path.find('?');
        if (q != std::string::npos)
            path.resize(q);
        if (path != "/yt-live.ts") {
            static const char k[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(client, k, sizeof k - 1);
            return;
        }
        LiveSource src;
        {
            std::lock_guard<std::mutex> lk(mu_);
            src = src_;
        }
        runRemux(client, src);
    }

    struct Child {
        pid_t pid = -1;
        int out = -1;
        int err = -1;
        std::string errTail;
        std::string errLine;  // fix5 diag: partial stderr line
        int diagLines = 0;
        std::chrono::steady_clock::time_point started{};
    };

    static std::string redactTail(const std::string& raw) {
        // ffmpeg -loglevel error lines can quote a URL (jwt in the query). Keep only a short,
        // URL-free tail for the daemon log.
        std::string t;
        for (size_t i = 0; i < raw.size() && t.size() < 200; ++i) {
            if (raw.compare(i, 8, "https://") == 0 || raw.compare(i, 7, "http://") == 0) {
                while (i < raw.size() && raw[i] != ' ' && raw[i] != '\'' && raw[i] != '"' && raw[i] != '\n')
                    ++i;
                t += "<url>";
                if (i >= raw.size())
                    break;
            }
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            t += (c < 0x20 || c > 0x7e) ? ' ' : static_cast<char>(c);
        }
        return t;
    }

    bool spawnChild(const LiveSource& src, double tsOffsetSec, Child& c,
                    LiveAudioMap audioMap = LiveAudioMap::Required) {
        int outPipe[2] = {-1, -1};
        int errPipe[2] = {-1, -1};
        if (::pipe(outPipe) != 0)
            return false;
        if (::pipe(errPipe) != 0) {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            return false;
        }
        // Build argv before fork. The child of a multithreaded process must not allocate.
        const std::vector<std::string> store = liveFfmpegArgs(src, tsOffsetSec, audioMap);
        std::vector<char*> argv;
        argv.reserve(store.size() + 1);
        for (const auto& s : store)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        // #687: the static-glibc ffmpeg aborts in iconv on a DVB SDT; give it the GCONV_PATH stub (see ensureGconvStub).
        const std::vector<std::string> envStore = liveFfmpegEnv(ensureGconvStub());
        std::vector<char*> envp;
        envp.reserve(envStore.size() + 1);
        for (const auto& s : envStore)
            envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            ::close(errPipe[0]);
            ::close(errPipe[1]);
            return false;
        }
        if (pid == 0) {
            ::setpgid(0, 0);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::dup2(errPipe[1], STDERR_FILENO);
            for (int fd = 3; fd < 256; ++fd)
                ::close(fd);
            ::execve(src.ffmpeg.c_str(), argv.data(), envp.data());
            _exit(127);
        }
        ::setpgid(pid, pid);
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
        ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);
        ::fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);
        c.pid = pid;
        c.out = outPipe[0];
        c.err = errPipe[0];
        c.errTail.clear();
        c.started = std::chrono::steady_clock::now();
        ffmpegPid_.store(pid);
        return true;
    }

    // Returns the exit status (or -1 when it was killed / unknown).
    int reapChild(Child& c) {
        int rc = -1;
        if (c.pid > 0) {
            ::killpg(c.pid, SIGKILL);
            int status = 0;
            if (::waitpid(c.pid, &status, 0) == c.pid)
                rc = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        }
        if (c.out >= 0)
            ::close(c.out);
        if (c.err >= 0)
            ::close(c.err);
        c.pid = -1;
        c.out = -1;
        c.err = -1;
        ffmpegPid_.store(-1);
        return rc;
    }

    void drainErr(Child& c) {
        if (c.err < 0)
            return;
        char b[512];
        for (int i = 0; i < 8; ++i) {
            const ssize_t n = ::read(c.err, b, sizeof b);
            if (n <= 0)
                break;
            c.errTail.append(b, b + n);
            if (c.errTail.size() > 600)
                c.errTail.erase(0, c.errTail.size() - 600);
            for (ssize_t i = 0; i < n; ++i) {
                if (b[i] != '\n' && b[i] != '\r') {
                    if (c.errLine.size() < 400)
                        c.errLine.push_back(b[i]);
                    continue;
                }
                if (!c.errLine.empty() && c.diagLines < 400 && liveFfmpegDiagLine(c.errLine)) {
                    ++c.diagLines;
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch()).count();
                    std::fprintf(stderr, "misterplexd: live relay ffmpeg: wall_ms=%lld %s\n",
                                 static_cast<long long>(ms), redactTail(c.errLine).c_str());
                }
                c.errLine.clear();
            }
        }
    }

    static int64_t msSince(std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
    }

    // One client, a supervised ffmpeg. The player's socket outlives a silent or exited ffmpeg.
    void runRemux(int client, const LiveSource& src) {
        const int keepaliveMs = liveKnobMs("MPX_LIVE_KEEPALIVE_MS", kLiveKeepaliveMs, 50, 10000);
        const int stallMs = liveKnobMs("MPX_LIVE_STALL_RESTART_MS", kLiveStallRestartMs, 200, 120000);
        const int firstStallMs = liveKnobMs("MPX_LIVE_FIRST_STALL_MS", kLiveFirstStallRestartMs, 100, 60000);
        const int videoStallMs = liveKnobMs("MPX_LIVE_VIDEO_STALL_MS", kLiveVideoStallRestartMs, 200, 120000);
        [[maybe_unused]] const int audioRecheckMs = liveKnobMs("MPX_LIVE_AUDIO_RECHECK_MS", kLiveAudioRecheckMs, 200, 600000);
        [[maybe_unused]] const int audioRecheckGapMs = liveKnobMs("MPX_LIVE_AUDIO_RECHECK_GAP_MS", kLiveAudioRecheckGapMs, 100, 3600000);
        [[maybe_unused]] const int audioRecheckMax = liveKnobMs("MPX_LIVE_AUDIO_RECHECK_MAX", kLiveAudioRecheckMax, 0, 100);
        const bool splitAudio = !src.audioUrl.empty() && src.audioUrl != src.videoUrl;
        const int firstByteMs = liveKnobMs("MPX_LIVE_FIRST_BYTE_MS", kLiveFirstByteMs, 200, 60000);
        const int backoffMs = liveKnobMs("MPX_LIVE_RESPAWN_BACKOFF_MS", kLiveRespawnBackoffMs, 0, 10000);
        const int respawnMax = liveKnobMs("MPX_LIVE_RESPAWN_MAX", kLiveRespawnMax, 0, 100);
        const int windowMs = liveKnobMs("MPX_LIVE_RESPAWN_WINDOW_MS", kLiveRespawnWindowMs, 100, 3600000);
#ifdef MUT_N16_DYING_CHILD_HEALTHY
        const int healthyMs = 0;   // mutant: a child that dies young still counts as healthy (the N15 behaviour)
#else
        const int healthyMs = liveKnobMs("MPX_LIVE_HEALTHY_MS", kLiveHealthyMs, 50, 600000);
#endif
        const int backoffCapMs = liveKnobMs("MPX_LIVE_RESPAWN_BACKOFF_CAP_MS", kLiveRespawnBackoffCapMs, 0, 120000);
        const int failMax = liveKnobMs("MPX_LIVE_RESPAWN_FAIL_MAX", kLiveRespawnFailMax, 0, 1000);
        const int scoreShortMs = liveKnobMs("MPX_LIVE_SCORE_SHORT_MS", kLiveScoreShortMs, 50, 600000);
        const int scoreDecayMs = liveKnobMs("MPX_LIVE_SCORE_DECAY_MS", kLiveScoreDecayMs, 10, 3600000);
        const int scoreMax = liveKnobMs("MPX_LIVE_SCORE_MAX", kLiveScoreMax, 1, 1000);
#ifdef MUT_ADBREAK_THRESHOLD_0
        const int adCardMs = 0;   // mutant: a short hiccup flashes the card
#else
        const int adCardMs = liveKnobMs("MPX_AD_CARD_MS", kLiveAdCardMs, 50, 120000);
#endif
        const bool plutoGap = src.plutoHlsRemux;   // only the Pluto relay drives the stand-by card
        const int adHoldMs = liveKnobMs("MPX_AD_CARD_HOLD_MS", kLiveAdCardHoldMs, 50, 60000);
        using Clock = std::chrono::steady_clock;
        const auto tServe = Clock::now();
        Child ch;
        std::vector<Clock::time_point> respawnTimes;  // spawns after the first
        bool headerSent = false;
        bool havePrev = false;  // a first spawn happened
        uint64_t sent = 0;      // TS bytes written after the header
        std::string pend;       // tail of a packet not yet complete (< kTsPacket bytes), held back (N1)
        Clock::time_point tFirstData{};
        Clock::time_point lastReal = tServe;
        Clock::time_point lastData = tServe;   // last real ffmpeg byte (not reset by a spawn)
        Clock::time_point lastVideo = tServe;  // last whole TS packet on the muxer video PID (reset by a spawn)
        std::function<int64_t()> probe;
        {
            std::lock_guard<std::mutex> lk(cbMu_);
            probe = frameProbe_;
        }
        int64_t framePos = -1;                 // last picture clock seen
        Clock::time_point framePosAt = tServe; // when it last moved
        bool frameArmed = false;               // it moved at least once since this relay started
        int64_t posAtOn = -1;                  // the frozen clock the card must see leave
        LiveGapWhy gapWhy = LiveGapWhy::RelaySilence;
        Clock::time_point gapOnAt = tServe;
        bool gapOn = false;                    // card requested
        bool gapResumed = false;               // bytes flow again, waiting out the hold
        bool gapBytes = false;                 // the current child wrote a byte since the card went up
        Clock::time_point gapResumedAt{};
        Clock::time_point lastKeep = tServe;
        Clock::time_point nextSpawn = tServe;
        int spawns = 0;
        int consecutiveFails = 0;
        LiveAudioMap audioMap = LiveAudioMap::Required;   // v1.0.1 1062: escalates on a no-audio map failure
        bool childAudio = false;                          // this child put a packet on kLiveAudioPid
        [[maybe_unused]] int audioRechecks = 0;           // F1: optional -> required re-checks so far
        Clock::time_point lastAudioRecheck{};
        bool recheckSpawn = false;                        // the current required/deep child came from an F1 re-check
        uint64_t childBytes = 0;                          // media bytes the current child wrote (reset by a spawn)
        double failScore = 0;                       // N17: decaying count of short-lived deaths
        Clock::time_point failScoreAt = tServe;
        static const unsigned char kNull[kTsPacket] = {
            0x47, 0x1f, 0xff, 0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff};
        static const char kHdr[] =
            "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        auto send502 = [&] {
            static const char k[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(client, k, sizeof k - 1);
        };
        // Re-spawn budget: at most respawnMax spawns after the first in any windowMs.
        [[maybe_unused]] auto budgetOk = [&] {
#ifdef MUT_LIVE_RESPAWN_UNBOUNDED
            return true;
#else
            const auto now = Clock::now();
            std::vector<Clock::time_point> keep;
            for (const auto& t : respawnTimes)
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count() < windowMs)
                    keep.push_back(t);
            respawnTimes.swap(keep);
            return static_cast<int>(respawnTimes.size()) < respawnMax;
#endif
        };
        // The child ended (exit, EOF, or killed for a stall). Decide: re-spawn, or end the response.
        // Returns false when the response is over.
        auto childGone = [&](const char* why) -> bool {
            drainErr(ch);
            const int64_t liveMs = ch.pid > 0 ? msSince(ch.started) : 0;
            const std::string tail = redactTail(ch.errTail);
            const bool audioMapMissing = src.plutoHlsRemux && !splitAudio && liveErrAudioMapMissing(ch.errTail);
            // "sample rate not set" (late AAC) gets the deep-probe rung whenever the dying child wrote no media
            // byte: the first open, an F1 re-check child (reviewer review of 139c3a35, finding 4), and a reconnect
            // whose 200 header went out at accept (finding 7). The 200 header alone says nothing about this child.
#if defined(MUT_PLUTO_AMAP_RECHECK_NO_RECOVER)
            const bool rateGate = !headerSent;                  // mutant: 139c3a35's gate
#elif defined(MUT_PLUTO_AMAP_RATE_GATE_HEADER)
            const bool rateGate = !headerSent || recheckSpawn;  // mutant: the header, not this child's bytes
#else
            const bool rateGate = childBytes == 0 || recheckSpawn;
#endif
            const bool audioRateUnset =
                src.plutoHlsRemux && !splitAudio && rateGate && liveErrAudioRateUnset(ch.errTail);
            const int rc = reapChild(ch);
            std::fprintf(stderr,
                         "misterplexd: live relay: ffmpeg gone why=%s rc=%d after_ms=%lld sent=%llu spawns=%d "
                         "err='%s'\n",
                         why, rc, static_cast<long long>(liveMs), static_cast<unsigned long long>(sent), spawns,
                         tail.c_str());
            if (stop_.load())
                return false;
            [[maybe_unused]] const LiveAudioMap mapWas = audioMap;
            if (audioMapMissing || audioRateUnset) {
                const LiveAudioMap was = audioMap;
                audioMap = nextLiveAudioMap(audioMap, audioMapMissing, audioRateUnset);
                if (audioMap != was)
                    std::fprintf(stderr,
                                 "misterplexd: live relay: %s for the audio map (%s); respawn with audio map %s "
                                 "spawns=%d\n",
                                 audioMapMissing ? "input has no audio stream" : "input audio has no sample rate yet",
                                 liveAudioMapName(was), liveAudioMapName(audioMap), spawns);
            }
            // An F1 re-check child (required, then the deep probe on "sample rate not set") that dies young for any
            // other reason goes back to the working video-only map instead of retrying required until the failure
            // caps end the relay (reviewer review of 139c3a35, finding 4; their candidate fix).
#ifndef MUT_PLUTO_AMAP_RECHECK_NO_RECOVER
            if (recheckSpawn && std::strcmp(why, "audio_recheck") != 0 && audioMap != LiveAudioMap::Optional &&
                liveMs < healthyMs && !(audioRateUnset && mapWas == LiveAudioMap::Required)) {
                audioMap = LiveAudioMap::Optional;
                std::fprintf(stderr, "misterplexd: live relay: audio re-check failed (%s); back to audio map optional\n",
                             liveAudioMapName(mapWas));
            }
#endif
            if (audioMap == LiveAudioMap::Optional)
                recheckSpawn = false;
            if (!headerSent) {
#ifdef MUT_LIVE_NO_FIRST_BYTE_RETRY
                return false;
#else
                if (spawns >= kLiveFirstSpawnsMax || msSince(tServe) >= firstByteMs)
                    return false;
                ++consecutiveFails;
                nextSpawn = Clock::now() + std::chrono::milliseconds(backoffMs);
                return true;
#endif
            }
#ifdef MUT_LIVE_NO_RESPAWN
            return false;
#else
            if (!budgetOk()) {
                std::fprintf(stderr, "misterplexd: live relay: respawn budget exhausted (%d in %d ms); ending\n",
                             respawnMax, windowMs);
                return false;
            }
#ifdef MUT_ADBREAK_DROP_AT_RESPAWN
            if (gapOn) {   // mutant: the card is dropped when ffmpeg is killed / re-spawned
                gapOn = false;
                gapResumed = false;
                emitGap(LiveGap::Clear, LiveGapWhy::Respawn);
            }
#endif
            // browse-lane P2-2: long-lived video_stall kill is intentional restart, not a failed spawn.
            const bool intentionalVideoStall =
                std::strcmp(why, "video_stall") == 0 && liveMs >= scoreShortMs;
            const bool audioRecheckKill = std::strcmp(why, "audio_recheck") == 0;   // F1: intentional, bounded
            if (!intentionalVideoStall && !audioRecheckKill)
                ++consecutiveFails;
#ifndef MUT_N16_NO_FAIL_CAP
            if (consecutiveFails > failMax) {
                std::fprintf(stderr,
                             "misterplexd: live relay: respawn capped after %d consecutive failed spawns; ending\n",
                             failMax);
                return false;
            }
#endif
            // N17: cumulative, decaying cap. The consecutive count above is reset by every child that reaches
            // healthyMs, so a child that lives just past it, dies and repeats would never trip it.
            {
                const auto nowS = Clock::now();
#ifndef MUT_N17_NO_DECAY
                failScore -= static_cast<double>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(nowS - failScoreAt).count()) /
                             scoreDecayMs;
                if (failScore < 0)
                    failScore = 0;
#endif
                failScoreAt = nowS;
                if (!audioRecheckKill) {
#ifndef MUT_N17_COUNT_LONG
                    if (liveMs < scoreShortMs)
#endif
                        failScore += 1.0;
                }
#ifndef MUT_N17_NO_SCORE_CAP
                if (failScore >= scoreMax) {
                    std::fprintf(stderr,
                                 "misterplexd: live relay: respawn capped: failure score %.1f >= %d (deaths of "
                                 "children that lived < %d ms, decaying 1 per %d ms); ending\n",
                                 failScore, scoreMax, scoreShortMs, scoreDecayMs);
                    return false;
                }
#endif
            }
            // Exponential back-off (base, 2x, 4x ... cap) per consecutive failed spawn.
            int backoff = backoffMs;
#ifdef MUT_N16_NO_BACKOFF
            (void)backoffCapMs;   // mutant: a flat delay, a crash storm hammers at the base rate
#else
            // Intentional long-lived video_stall keeps streak 0: use base backoff only (no << -1).
            if (!intentionalVideoStall && consecutiveFails > 0) {
                backoff = static_cast<int>(std::min<int64_t>(
                    static_cast<int64_t>(backoffMs) << std::min(consecutiveFails - 1, 16), 600000));
#ifndef MUT_N16_NO_BACKOFF_CAP
                backoff = std::min(backoff, backoffCapMs);
#endif
            }
#endif
            std::fprintf(stderr, "misterplexd: live relay: respawn in %d ms (failed spawns in a row %d/%d)\n", backoff,
                         consecutiveFails, failMax);
            nextSpawn = Clock::now() + std::chrono::milliseconds(backoff);
            // A killed/dead ffmpeg may have left half a TS packet: it is dropped (held back, never sent) so the
            // player's demuxer does not lose sync. MUT_LIVE_NO_PAD: send the partial packet raw instead.
#ifdef MUT_LIVE_NO_PAD
            if (!pend.empty()) {
                if (!writeAll(client, pend.data(), pend.size()))
                    return false;
                sent += pend.size();
            }
#endif
            pend.clear();
            // (Only reachable unaligned when hold-back is off.) Pad to a packet boundary.
#ifndef MUT_LIVE_NO_PAD
            if (sent % kTsPacket != 0) {
                unsigned char pad[kTsPacket];
                std::memset(pad, 0xff, sizeof pad);
                const size_t n = kTsPacket - static_cast<size_t>(sent % kTsPacket);
                if (!writeAll(client, reinterpret_cast<const char*>(pad), n))
                    return false;
                sent += n;
            }
#endif
            return true;
#endif
        };
        // Glass 40c9feb4 r1: after `ending why=client_write` the player's libav
        // reconnect GET got no header until this new child's first byte
        // (2.5-4.5 s on Pluto). The player's cover wake aborts a blocked read
        // after 100 ms, so 17 reconnects died in the header read ("Error
        // reading HTTP response: Immediate exit requested", relay sent=0) and
        // the picture froze at 6629 for the rest of the soak. The 200 header is
        // only held back on the first response, where a dead first spawn can
        // still turn into a 502. A reconnect gets it at once and the keepalive
        // null packets carry it to the first byte.
        if (servedHeader_ && misterplex::live_disc::liveReconnectHeaderAtAccept()) {
            if (!writeAll(client, kHdr, sizeof kHdr - 1)) {
                std::fprintf(stderr, "misterplexd: live relay: ending why=client_write sent=0 spawns=0\n");
                return;
            }
            headerSent = true;
            tFirstData = Clock::now();
            std::fprintf(stderr, "misterplexd: live relay: reconnect header at accept\n");
        }
        auto forward = [&](const char* p, size_t n) -> bool {
            if (!headerSent) {
                if (!writeAll(client, kHdr, sizeof kHdr - 1))
                    return false;
                headerSent = true;
                servedHeader_ = true;
                tFirstData = Clock::now();
            }
            childBytes += n;
#ifdef MUT_LIVE_NO_HOLDBACK
            if (!writeAll(client, p, n))
                return false;
            sent += n;
            return true;
#else
            // N1: only whole 188-byte packets go out. A pipe read can end mid-packet; the tail waits here, so
            // `sent` stays on a packet boundary and a keepalive null packet can go out during ANY stall
            // (a stall > 20 s with an unaligned `sent` used to suppress it and trip the player's rw_timeout).
            pend.append(p, n);
            const size_t whole = pend.size() / kTsPacket * kTsPacket;
            if (whole > 0) {
                if (!writeAll(client, pend.data(), whole))
                    return false;
                for (size_t i = 0; i + kTsPacket <= whole; i += kTsPacket)
                    if (static_cast<unsigned char>(pend[i]) == 0x47 &&
                        ((static_cast<unsigned>(static_cast<unsigned char>(pend[i + 1]) & 0x1f) << 8) |
                         static_cast<unsigned char>(pend[i + 2])) == kLiveVideoPid) {
                        lastVideo = Clock::now();
                        break;
                    }
                if (!childAudio && audioMap == LiveAudioMap::Optional)
                    for (size_t i = 0; i + kTsPacket <= whole; i += kTsPacket)
                        if (static_cast<unsigned char>(pend[i]) == 0x47 &&
                            ((static_cast<unsigned>(static_cast<unsigned char>(pend[i + 1]) & 0x1f) << 8) |
                             static_cast<unsigned char>(pend[i + 2])) == kLiveAudioPid) {
                            childAudio = true;
                            break;
                        }
                sent += whole;
                pend.erase(0, whole);
            }
            return true;
#endif
        };

        bool alive = true;

        // browse-lane P1-2 / GLASS-AV-MTV-1120: client write failures used to reap with no log.
        // Always print ending why= and ffmpeg gone why=client_gone when a live child is torn down.
        auto endRelay = [&](const char* why) {
            std::fprintf(stderr, "misterplexd: live relay: ending why=%s sent=%llu spawns=%d\n", why,
                         static_cast<unsigned long long>(sent), spawns);
            if (ch.pid > 0) {
                drainErr(ch);
                const int64_t liveMs = msSince(ch.started);
                const std::string tail = redactTail(ch.errTail);
                const int rc = reapChild(ch);
                std::fprintf(stderr,
                             "misterplexd: live relay: ffmpeg gone why=client_gone rc=%d after_ms=%lld sent=%llu "
                             "spawns=%d end=%s err='%s'\n",
                             rc, static_cast<long long>(liveMs), static_cast<unsigned long long>(sent), spawns, why,
                             tail.c_str());
            } else {
                reapChild(ch);
            }
            alive = false;
        };
        while (alive && !stop_.load()) {
            const auto now = Clock::now();
            if (ch.pid < 0) {
                if (!headerSent && msSince(tServe) >= firstByteMs) {
                    send502();
                    break;
                }
                if (now >= nextSpawn) {
                    double off = 0.0;
                    if (havePrev && headerSent)
                        off = std::chrono::duration<double>(now - tFirstData).count();
                    if (havePrev && headerSent)
                        respawnTimes.push_back(now);
                    if (!spawnChild(src, off, ch, audioMap)) {
                        std::fprintf(stderr, "misterplexd: live relay: spawn failed\n");
                        if (!headerSent)
                            send502();
                        break;
                    }
                    ++spawns;
                    havePrev = true;
                    gapBytes = false;
                    childAudio = false;
                    childBytes = 0;
                    lastReal = Clock::now();
                    lastVideo = lastReal;
                    // Header already sent: this is a video_stall / TLS-EOF / stall respawn.
                    // Rejoin HDMI PCM so remux AAC is not stuck at the grabber floor (-81.9).
                    // (spawns > 1: a reconnect's header-at-accept is not a respawn.)
                    if (headerSent && spawns > 1) {
                        std::fprintf(stderr, "misterplexd: live relay: ffmpeg respawn spawns=%d\n", spawns);
                        fireRespawn();
                    }
                } else {
                    // between spawns: keep the socket warm
                    ::usleep(50 * 1000);
#ifdef MUT_N16_CARD_DROP_IN_BACKOFF
                    if (gapOn) {   // mutant: the card is dropped while the next spawn waits out its back-off
                        gapOn = false;
                        gapResumed = false;
                        emitGap(LiveGap::Clear, LiveGapWhy::Respawn);
                    }
#endif
                }
            }
            pollfd pf[2];
            int np = 0;
            if (ch.out >= 0) {
                pf[np] = {ch.out, POLLIN, 0};
                ++np;
            }
            if (ch.err >= 0) {
                pf[np] = {ch.err, POLLIN, 0};
                ++np;
            }
            if (np > 0)
                ::poll(pf, static_cast<nfds_t>(np), 100);
            if (ch.pid > 0) {
                drainErr(ch);
                bool eof = false;
                for (int i = 0; i < 64 && ch.out >= 0; ++i) {
                    char b[8192];
                    const ssize_t n = ::read(ch.out, b, sizeof b);
                    if (n > 0) {
                        if (!forward(b, static_cast<size_t>(n))) {
                            endRelay("client_write");
                            break;
                        }
                        lastReal = Clock::now();
                        lastData = lastReal;
                        if (msSince(ch.started) >= healthyMs) {
                            consecutiveFails = 0;   // only a child that stayed up ends a failure streak
                            recheckSpawn = false;
                        }
                        if (gapOn)
                            gapBytes = true;
                        continue;
                    }
                    if (n == 0)
                        eof = true;
                    break;
                }
                if (!alive)
                    break;
                if (eof) {
                    if (!childGone("exit")) {
                        if (!headerSent)
                            send502();
                        break;
                    }
                    continue;
                }
                // silent child
                const int64_t idle = msSince(lastReal);
#ifndef MUT_LIVE_NO_STALL_RESTART
                const int limit = headerSent ? stallMs : firstStallMs;
                if (idle >= limit && (headerSent || msSince(tServe) < firstByteMs)) {
                    if (!childGone("stall")) {
                        if (!headerSent)
                            send502();
                        break;
                    }
                    continue;
                }
#endif
#ifndef MUT_LIVE_NO_VIDEO_STALL
                if (headerSent && splitAudio && msSince(lastVideo) >= videoStallMs) {
                    if (!childGone("video_stall")) {
                        break;
                    }
                    continue;
                }
#endif
#ifndef MUT_PLUTO_AMAP_NO_AUDIO_RECHECK
                // F1: an optional-map child with no audio output for audioRecheckMs -> respawn once with the required
                // map (audio may be back). Bounded per relay; see kLiveAudioRecheckMax.
                if (headerSent && src.plutoHlsRemux && !splitAudio && audioMap == LiveAudioMap::Optional &&
                    !childAudio && msSince(ch.started) >= audioRecheckMs &&
                    // never spend the last respawn slots on a re-check: a working video-only relay must not end on it
                    budgetOk() && static_cast<int>(respawnTimes.size()) + 2 <= respawnMax &&
#ifdef MUT_PLUTO_AMAP_RECHECK_UNBOUNDED
                    true) {
#else
                    audioRechecks < audioRecheckMax &&
                    (audioRechecks == 0 || msSince(lastAudioRecheck) >= audioRecheckGapMs)) {
#endif
                    ++audioRechecks;
                    lastAudioRecheck = Clock::now();
                    audioMap = LiveAudioMap::Required;
                    recheckSpawn = true;
                    std::fprintf(stderr,
                                 "misterplexd: live relay: optional audio map sent no audio for %lld ms; respawn with "
                                 "audio map required (audio re-check %d/%d)\n",
                                 static_cast<long long>(msSince(ch.started)), audioRechecks, audioRecheckMax);
                    if (!childGone("audio_recheck"))
                        break;
                    continue;
                }
#endif
                if (!headerSent && msSince(tServe) >= firstByteMs) {
                    reapChild(ch);
                    send502();
                    break;
                }
            }
            if (plutoGap && headerSent) {
                // N15: the picture clock. -1 (paused / not playing) = nothing to watch: the timer restarts.
                const int64_t p = probe ? probe() : -1;
                if (p < 0) {
                    framePosAt = Clock::now();
                } else if (framePos < 0) {
                    framePos = p;
                    framePosAt = Clock::now();
                } else if (p != framePos) {
                    framePos = p;
                    framePosAt = Clock::now();
                    frameArmed = true;
                }
#ifdef MUT_ADBREAK_NO_FRAME_STALL
                const bool frameStalled = false;   // mutant: relay silence is the only trigger (N14)
#else
                // AD-REC A play-all: a picture hiccup while bytes still flow is a splice, not a
                // dropped ad pod. Do not snap back to the ad-break card mid-ad.
                const bool frameStalled = frameArmed && p >= 0 && msSince(framePosAt) >= adCardMs &&
                                          misterplex::live_disc::playAllShowsFrameStallCard();
#endif
                if (!gapOn) {
                    // card on: ffmpeg being re-spawned (at once), no real byte for adCardMs, or no new picture for
                    // adCardMs while bytes flow. Play-all keeps FrameStall and RelaySilence off (the black
                    // card is what killed HDMI on 47abfbbb). Respawn (child actually gone) stays.
                    const bool respawning = ch.pid < 0;
                    const bool silent = msSince(lastData) >= adCardMs &&
                                        misterplex::live_disc::playAllShowsRelaySilenceCard();
                    if (respawning || silent || frameStalled) {
                        gapOn = true;
                        gapResumed = false;
                        gapBytes = false;
                        gapWhy = respawning ? LiveGapWhy::Respawn
                                            : (silent ? LiveGapWhy::RelaySilence : LiveGapWhy::FrameStall);
                        posAtOn = p >= 0 ? p : framePos;
                        gapOnAt = Clock::now();
                        emitGap(LiveGap::Silent, gapWhy);
                    }
                } else {
                    // "Bytes flow again" = the child wrote something AND has stayed up healthyMs: a respawned child
                    // that dies after its PAT/PMT (N16 glass, 1062) never ends the card.
                    if (!gapResumed && gapBytes && ch.pid > 0 && msSince(ch.started) >= healthyMs) {
                        gapResumed = true;
                        gapResumedAt = Clock::now();
                        emitGap(LiveGap::Resumed, gapWhy);
                    }
                    // The clock is rebased until real bytes flow again: frames still draining out of the player's
                    // buffer after the stall began are not "the picture is back".
                    if (!gapResumed && p >= 0)
                        posAtOn = p;
                    const bool probeLive = frameArmed && p >= 0;
#ifdef MUT_ADBREAK_CLEAR_ON_BYTES
                    if (gapResumed) {   // mutant: bytes flowing again clears it, a picture is not required
                        gapOn = false;
                        gapResumed = false;
                        emitGap(LiveGap::Clear, LiveGapWhy::PictureAdvance);
                    }
#else
                    if (gapResumed && probeLive && p != posAtOn) {
                        gapOn = false;
                        gapResumed = false;
                        emitGap(LiveGap::Clear, LiveGapWhy::PictureAdvance);
                    } else if (gapResumed && !probeLive && msSince(gapResumedAt) >= adHoldMs) {
                        // no picture clock to ask (never armed / paused): the hold is the only evidence left
                        gapOn = false;
                        gapResumed = false;
                        emitGap(LiveGap::Clear, LiveGapWhy::HoldTimeout);
                    }
#endif
                }
            }
#ifndef MUT_LIVE_NO_KEEPALIVE
            if (headerSent && msSince(lastReal) >= keepaliveMs && msSince(lastKeep) >= keepaliveMs &&
                sent % kTsPacket == 0) {
                if (!writeAll(client, reinterpret_cast<const char*>(kNull), kTsPacket)) {
                    endRelay("keepalive");
                    break;
                }
                sent += kTsPacket;
                lastKeep = Clock::now();
            }
#endif
        }
        if (stop_.load() && ch.pid > 0) {
            std::fprintf(stderr, "misterplexd: live relay: ending why=stop sent=%llu spawns=%d\n",
                         static_cast<unsigned long long>(sent), spawns);
            drainErr(ch);
            const int64_t liveMs = msSince(ch.started);
            const std::string tail = redactTail(ch.errTail);
            const int rc = reapChild(ch);
            std::fprintf(stderr,
                         "misterplexd: live relay: ffmpeg gone why=client_gone rc=%d after_ms=%lld sent=%llu "
                         "spawns=%d end=stop err='%s'\n",
                         rc, static_cast<long long>(liveMs), static_cast<unsigned long long>(sent), spawns,
                         tail.c_str());
        } else {
            reapChild(ch);
        }
        if (gapOn)
            emitGap(LiveGap::Clear, LiveGapWhy::RelayStop);
        gapOn = false;
    }

    // One log line per card event (no URL, no jwt): "ad card shown why=<frame_stall|relay_silence|respawn>" and
    // "ad card cleared why=<picture_advance|hold_timeout|relay_stop> shown_ms=<n>".
    void emitGap(LiveGap g, LiveGapWhy why) {
#ifdef MUT_ADBREAK_NO_SIGNAL
        if (g == LiveGap::Silent)
            return;
#endif
        if (g == LiveGap::Silent) {
            cardShownAt_ = std::chrono::steady_clock::now();
#ifndef MUT_ADBREAK_NO_LOG
            std::fprintf(stderr, "misterplexd: ad card shown why=%s\n", liveGapWhyName(why));
#endif
        } else if (g == LiveGap::Clear) {
#ifndef MUT_ADBREAK_NO_LOG
            std::fprintf(stderr, "misterplexd: ad card cleared why=%s shown_ms=%lld\n", liveGapWhyName(why),
                         static_cast<long long>(msSince(cardShownAt_)));
#endif
        }
        std::function<void(LiveGap, LiveGapWhy)> cb;
        {
            std::lock_guard<std::mutex> lk(cbMu_);
            cb = gapCb_;
        }
        if (cb)
            cb(g, why);
    }

    std::chrono::steady_clock::time_point cardShownAt_{};
    std::mutex cbMu_;
    std::function<void(LiveGap, LiveGapWhy)> gapCb_;
    std::function<int64_t()> frameProbe_;
    std::function<void()> respawnCb_;
    std::mutex mu_;
    std::atomic<bool> stop_{true};
    std::atomic<pid_t> ffmpegPid_{-1};
    int listenFd_ = -1;
    LiveSource src_;
    bool servedHeader_ = false;  // relay thread: a response header went out since start()
    std::thread thread_;
};

inline LiveRelay& liveRelay() {
    static LiveRelay r;
    return r;
}

}  // namespace yt_live
}  // namespace airplay
}  // namespace misterplex
