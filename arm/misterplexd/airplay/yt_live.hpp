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
#include "libmisterplex/pluto_ad_skip.hpp"
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
#include <memory>
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

// N13 (#685 glass 10a21b2e). CLI repro (buildhost pluto-probe RESULT-STABILITY.md): at a Pluto ad break the
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
// No first byte yet: restart once. A Pluto remux open does not get its first byte "in about a second": v1.0.2 glass
// (lab 2026-11-23, 1066 resume after Plex->Live TV) had the good open's first byte at 5959 ms, while the 6000 ms
// default killed two earlier healthy children at 6032-6034 ms (sent=0). Each kill cost a prefetch timeout and a
// metadata retry: first_picture took 54405 ms. Above the legacy 8 s remux-open cap
// (MPX_PLUTO_REMUX_OPEN_MS=8000) and at most kLiveFirstByteMs, so a silent child still gets one restart on the
// other paths. The remux-open default is now 15 s, so this 10 s stall is the tighter first-byte limit: a 10.1 s
// ARM tail is still killed here (Finding 5). The remux cap itself no longer kills an 8–10 s first byte.
#ifdef MUT_LIVE_FIRST_STALL_TOO_SHORT
inline constexpr int kLiveFirstStallRestartMs = 6000;   // mutant: the v1.0.2 default
#else
inline constexpr int kLiveFirstStallRestartMs = 10000;
#endif
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
// N17 (#685, FPGA Bug review of N16): a child that lives just over kLiveHealthyMs (3.2 s), dies, and repeats every ~12 s
// resets the consecutive-failure count each time and spends only 5 of the 10 respawns per 120 s window, so it
// is never capped. A slowly decaying failure score closes that gap: every death of a child that lived less than
// kLiveScoreShortMs adds 1, the score decays by 1 per kLiveScoreDecayMs, and at kLiveScoreMax the relay ends (the player
// then keeps the card and moves on to the next channel). A child that lives longer than kLiveScoreShortMs (a real
// ad-pod stall restart) adds nothing. With the defaults the 12 s cycle trips after ~10 deaths (~2 min); one
// short death every 30 s or slower never does.
inline constexpr int kLiveScoreShortMs = 30000;
inline constexpr int kLiveScoreDecayMs = 30000;
inline constexpr int kLiveScoreMax = 6;
// #686 x relay (buildhost real Pluto, vevo-90s, 2026-11-18): with the audio rendition as a 2nd ffmpeg input, an ad boundary
// (the ad video segments use other PIDs / a reset timebase) makes ffmpeg stop emitting VIDEO for good while the AUDIO
// input keeps flowing (video 300 pkts/10 s -> 0, audio 469 pkts/10 s unbroken for 160 s). Bytes keep arriving, so the
// whole-stream silence watchdog never fires and the picture stays frozen with sound. For a split-audio relay the
// child is therefore also restarted when no VIDEO packet came out for this long. ffmpeg's mpegts muxer puts the first
// mapped stream (0:v:0) on PID 0x100. Normal video gaps are 3-11 s (one burst per 5 s segment).
inline constexpr int kLiveVideoStallRestartMs = 30000;
inline constexpr unsigned kLiveVideoPid = 0x100;
inline constexpr size_t kTsPacket = 188;

// N14 stand-by card trigger. The byte gap that means "ad pod dropped" is NOT 2 s: ffmpeg emits one
// burst per 5 s segment, so normal gaps are 3-11 s (buildhost stability logs: 268 gaps >= 2 s in 24 min on
// 90s-kids, max normal 11.1 s) while every ad-pod stall was >= 30 s. 15 s sits in the empty 12-30 s
// band. An ffmpeg exit / re-spawn mid-stream shows the card at once.
inline constexpr int kLiveAdCardMs = 15000;
inline constexpr int kLiveAdCardHoldMs = 4000;  // after bytes resume: clear the card even if no frame is reported
// Pluto ad skip (libmisterplex/pluto_ad_skip.hpp). Playlist poll period; how long the picture sits still after
// the reserve ran dry before the card goes up (no picture clock: kPlutoAdSkipNoProbeCardMs); the longest hold
// before a forced live-edge resume; tune-in-pod arm time with no picture clock; and how long a child may stream
// without any "Opening" line before the cut falls back to the last clean poll.
inline constexpr int kPlutoAdSkipPollMs = 2000;
inline constexpr int kPlutoAdSkipFreezeMs = 500;
inline constexpr int kPlutoAdSkipNoProbeCardMs = 1500;
inline constexpr int kPlutoAdSkipHoldCapMs = 600000;
inline constexpr int kPlutoAdSkipArmMs = 3000;
inline constexpr int kPlutoAdSkipOpenProbeMs = 10000;
inline constexpr int kLiveReserveLogMs = 30000;
enum class LiveGap { Silent, Resumed, Clear };
// N15: why the card went up / came down (the log line and the callback carry it).
enum class LiveGapWhy { FrameStall, RelaySilence, Respawn, PictureAdvance, HoldTimeout, RelayStop, AdSkip, ContentReturn };
inline const char* liveGapWhyName(LiveGapWhy w) {
    switch (w) {
    case LiveGapWhy::FrameStall: return "frame_stall";
    case LiveGapWhy::RelaySilence: return "relay_silence";
    case LiveGapWhy::Respawn: return "respawn";
    case LiveGapWhy::PictureAdvance: return "picture_advance";
    case LiveGapWhy::HoldTimeout: return "hold_timeout";
    case LiveGapWhy::RelayStop: return "relay_stop";
    case LiveGapWhy::AdSkip: return "ad_skip";
    case LiveGapWhy::ContentReturn: return "content_return";
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
    // Pluto split remux only (the relay sets these per spawn): -live_start_index per input
    // and info-level "Opening '...'" lines that let ad skip cut before an ad segment.
    int liveStartIndexVideo = -1;
    int liveStartIndexAudio = -1;
    bool logSegmentOpens = false;
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

// tsOffsetSec > 0 only on a re-spawn: the new ffmpeg starts its mpegts at 0, so the relay shifts it
// by the seconds already streamed to keep the player's timeline monotonic (MUT_LIVE_NO_TS_OFFSET).
inline std::vector<std::string> liveFfmpegArgs(const LiveSource& src, double tsOffsetSec = 0.0) {
    std::vector<std::string> a;
    const char* proto = src.plutoHlsRemux     ? kFfmpegNetProtoPluto
                        : src.httpsOnlyProto ? kFfmpegNetProtoHttps
                                             : kFfmpegNetProto;
    const bool split = !src.audioUrl.empty() && src.audioUrl != src.videoUrl;
    a.push_back(src.ffmpeg);
    a.push_back("-nostdin");
    a.push_back("-hide_banner");
    const bool openLines = src.logSegmentOpens && src.plutoHlsRemux && split;
    a.push_back("-loglevel");
    a.push_back(openLines ? "info" : "error");
    if (openLines)
        a.push_back("-nostats");
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
    // The relay's live reserve starts both at the same older index; a resume after an ad pod
    // starts both at the oldest media sequence the two windows share.
    auto plutoAvAlignInputOpts = [&](int startIndex) {
#ifndef MUT_PLUTO_NO_AV_ALIGN
        if (src.plutoHlsRemux && split) {
            a.push_back("-live_start_index");
            a.push_back(std::to_string(startIndex));
        }
#else
        (void)startIndex;
#endif
        // A persistent HTTP connection reuses the socket without an "Opening" line.
        if (openLines) {
            a.push_back("-http_persistent");
            a.push_back("0");
        }
    };
    if (src.plutoHlsRemux)
        plutoInputOpts();
    plutoAvAlignInputOpts(src.liveStartIndexVideo);
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
        plutoAvAlignInputOpts(src.liveStartIndexAudio);
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
    a.push_back(split ? "1:a:0?" : (src.plutoHlsRemux ? "0:a:0" : "0:a:0?"));
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

// #687 (lab, 2026-11-18): the card's ffmpeg is a STATIC glibc build (80baac9d). Its mpegts demuxer decodes the DVB SDT
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

// First-picture watchdog exemption. True only while an ad hold is actually
// active: tune-in-pod, or Phase::Holding. Resume (content_return / hold_cap)
// leaves Holding, so the 3 s timer starts on the next poll. A player that
// stops during Resuming must not sit on the ad card until the 660 s ceiling.
// ads=false (MPX_PLUTO_ADSKIP=0) is never an exemption.
// Unset: start the ad-skip playlist poll when the first remux byte is written
// (headerSent), so it does not compete with ARM ffmpeg's first-byte TLS.
// MPX_PLUTO_POLL_AT_START=1 restores today's poll at serve start (t=0).
// tune_in_pod is driven by ffmpeg "Opening" lines and already lets bytes flow
// until playerArmed(), which requires headerSent. The no-Opening poll cut
// already requires headerSent plus the open-probe wait, so the first poll
// still lands before that cut. childGone's pod hold is after the !headerSent
// first-byte retry, so a pre-header death never held for ads.
// MUT_PLUTO_POLL_AT_START always starts the poller at t=0.
inline bool plutoAdSkipPollAtServeStart() {
#ifdef MUT_PLUTO_POLL_AT_START
    return true;
#else
    const char* e = std::getenv("MPX_PLUTO_POLL_AT_START");
    return e && e[0] == '1' && e[1] == '\0';
#endif
}

inline bool adHoldExemptsWatchdog(bool ads, bool tunePod, misterplex::pluto_adskip::Phase phase) {
#ifdef MUT_FPW_AD_HOLD_THROUGH_RESUMING
    // pre-fix: Resuming is treated as a hold until content is on screen
    return ads && (tunePod || phase != misterplex::pluto_adskip::Phase::Content);
#else
    return ads && (tunePod || phase == misterplex::pluto_adskip::Phase::Holding);
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
#ifdef MUT_TUNE_BLOCKS_INPUT
        {
            PlaylistFetcher f;
            {
                std::lock_guard<std::mutex> lk(cbMu_);
                f = fetcher_;
            }
            std::string body;
            if (f && src.plutoHlsRemux)
                f(src.videoUrl, [] { return false; }, body);
        }
#endif
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
            nextStartV_ = 0;
            nextStartA_ = 0;
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
        adHoldActive_.store(false, std::memory_order_release);
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
    // Optional clean player reopen on Pluto ad-skip content return. Returning true
    // closes this relay connection (keeping the ad card up) so the player reconnects
    // as a fresh first spawn with -copyts, avStartGate skip_gop, and ring flush.
    void setAdReturnReopenCallback(std::function<bool()> cb) {
        std::lock_guard<std::mutex> lk(cbMu_);
        adReturnReopenCb_ = std::move(cb);
    }
    // Pluto ad skip polls the two media playlists with this (on a detached thread; it must
    // not log the URL and should give up when `cancelled` turns true). No fetcher = no ad skip.
    using PlaylistFetcher =
        std::function<bool(const std::string& url, const std::function<bool()>& cancelled, std::string& body)>;
    void setPlaylistFetcher(PlaylistFetcher f) {
        std::lock_guard<std::mutex> lk(cbMu_);
        fetcher_ = std::move(f);
    }
    // Bytes held in the Pluto live reserve (not yet written to the player).
    size_t reserveBytes() const { return reserveBytes_.load(); }

    // True while Pluto ad-skip is holding (tune-in-pod or Holding) and until the
    // resumed child's bytes are content on screen. False when ad skip is off,
    // and cleared by stop() / a new start().
    bool adHoldActive() const { return adHoldActive_.load(std::memory_order_acquire); }

private:
    struct PollShared {
        std::mutex mu;
        std::atomic<bool> stop{false};
        uint64_t n = 0;
        std::chrono::steady_clock::time_point startedAt{};
        misterplex::pluto_adskip::Window v;
        misterplex::pluto_adskip::Window a;
    };

    static void pollPlaylists(const std::shared_ptr<PollShared>& sh, const PlaylistFetcher& fetch,
                              const std::string& vUrl, const std::string& aUrl, int pollMs) {
        namespace pa = misterplex::pluto_adskip;
        const std::function<bool()> cancelled = [sh] { return sh->stop.load(); };
        while (!sh->stop.load()) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string vb;
            std::string ab;
            const bool vOk = fetch(vUrl, cancelled, vb);
            const bool aOk = !sh->stop.load() && fetch(aUrl, cancelled, ab);
            pa::Window v = vOk ? pa::parseMediaPlaylist(vb) : pa::Window{};
            pa::Window a = aOk ? pa::parseMediaPlaylist(ab) : pa::Window{};
            {
                std::lock_guard<std::mutex> lk(sh->mu);
                sh->v = std::move(v);
                sh->a = std::move(a);
                sh->startedAt = t0;
                ++sh->n;
            }
            while (!sh->stop.load() && msSince(t0) < pollMs)
                ::usleep(20 * 1000);
        }
    }

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

    bool spawnChild(const LiveSource& src, double tsOffsetSec, Child& c) {
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
        const std::vector<std::string> store = liveFfmpegArgs(src, tsOffsetSec);
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
        namespace pa = misterplex::pluto_adskip;
        // Pluto split remux only: a reserve between ffmpeg and the player, and ad skip by segment path.
        const bool plutoSplit = src.plutoHlsRemux && splitAudio;
        const int reserveS = plutoSplit ? pa::liveReserveTargetS() : 0;
        const bool rsv = reserveS > 0;
        PlaylistFetcher fetcher;
        {
            std::lock_guard<std::mutex> lk(cbMu_);
            fetcher = fetcher_;
        }
        const bool ads = plutoSplit && fetcher && pa::adSkipEnabled();
        const int pollMs = liveKnobMs("MPX_PLUTO_ADSKIP_POLL_MS", kPlutoAdSkipPollMs, 50, 60000);
        const int adFreezeMs = liveKnobMs("MPX_PLUTO_ADSKIP_FREEZE_MS", kPlutoAdSkipFreezeMs, 50, 60000);
        const int holdCapMs = liveKnobMs("MPX_PLUTO_ADSKIP_HOLD_CAP_MS", kPlutoAdSkipHoldCapMs, 1000, 3600000);
        const int openProbeMs = liveKnobMs("MPX_PLUTO_ADSKIP_OPEN_PROBE_MS", kPlutoAdSkipOpenProbeMs, 500, 600000);
        const int pollHoldbackMs = liveKnobMs("MPX_PLUTO_ADSKIP_POLL_HOLDBACK_MS", pollMs + 1500, 0, 60000);
        LiveSource spawnSrc = src;
        spawnSrc.logSegmentOpens = ads;
        spawnSrc.liveStartIndexVideo = pa::firstSpawnStartIndex(reserveS);
        spawnSrc.liveStartIndexAudio = spawnSrc.liveStartIndexVideo;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (nextStartV_ < 0 && nextStartA_ < 0) {
                spawnSrc.liveStartIndexVideo = nextStartV_;
                spawnSrc.liveStartIndexAudio = nextStartA_;
            }
            nextStartV_ = 0;
            nextStartA_ = 0;
        }
        bool reopenHandoff = false;
        using Clock = std::chrono::steady_clock;
        const auto tServe = Clock::now();
        Child ch;
        std::vector<Clock::time_point> respawnTimes;  // spawns after the first
        bool headerSent = false;
        bool havePrev = false;  // a first spawn happened
        uint64_t sent = 0;      // TS bytes written after the header
        uint64_t realSent = 0;  // of those, ffmpeg bytes (not keepalive / pad)
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
        double failScore = 0;                       // N17: decaying count of short-lived deaths
        Clock::time_point failScoreAt = tServe;
        pa::ByteReserve reserve;
        size_t capBytes = pa::reserveCapBytes(reserveS, 0);
        int64_t rateBps = 0;                        // measured remux rate, 0 until rateWarmMs of data
        uint64_t rateFrom = 0;
        Clock::time_point rateAt = tServe;
        Clock::time_point lastSend = tServe;        // last byte written to the player
        Clock::time_point lastRsvLog = tServe;
        int64_t rsvPeakMs = 0;
        std::vector<std::pair<Clock::time_point, uint64_t>> inPosAt;  // reserve input position history
        pa::Machine adm;
        std::shared_ptr<PollShared> polls;
        std::thread pollThread;
        uint64_t pollSeen = 0;
        std::string errLine;                        // partial ffmpeg stderr line (open scan)
        bool opensSeen = false;                     // some child logged an "Opening" line
        bool pollOnly = false;                      // it never did: cut by poll, hold fresh bytes back
        int contentOpens = 0;
        bool tunePod = false;                       // tuned inside a pod: ad bytes until the player is armed
        bool haveCleanPos = false;
        uint64_t cleanPos = 0;
        Clock::time_point dryAt{};
        bool dry = false;
        int64_t posDry = -1;
        Clock::time_point posDryAt{};
        bool adCard = false;                        // ad skip owns the card
        bool adCardDropped = false;
        Clock::time_point adCardAt{};
        uint64_t resumeMark = 0;                    // client stream position where the resumed child begins
        bool resumeBytes = false;
        Clock::time_point resumeBytesAt{};
        int64_t posAtResume = -1;
        auto startAdSkipPoll = [&] {
            if (!ads || polls)
                return;
            polls = std::make_shared<PollShared>();
            auto pollFn = [polls, fetcher, v = src.videoUrl, a = src.audioUrl, pollMs] {
                pollPlaylists(polls, fetcher, v, a, pollMs);
            };
#ifdef MUT_TUNE_BLOCKS_INPUT
            pollThread = std::thread(pollFn);
#else
            std::thread(pollFn).detach();
#endif
            std::fprintf(stderr, "misterplexd: pluto adskip armed poll_ms=%d\n", pollMs);
        };
        if (plutoAdSkipPollAtServeStart())
            startAdSkipPoll();
        if (rsv)
            std::fprintf(stderr, "misterplexd: live reserve target_s=%d start_index=%d cap_bytes=%zu\n", reserveS,
                         spawnSrc.liveStartIndexVideo, capBytes);
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
        // Reserve mode: whole packets queue in the reserve and go to the player without blocking.
        auto ingest = [&](const char* p, size_t n) -> bool {
            if (!headerSent) {
                if (!writeAll(client, kHdr, sizeof kHdr - 1))
                    return false;
                headerSent = true;
                tFirstData = Clock::now();
                lastSend = tFirstData;
                startAdSkipPoll();
            }
            pend.append(p, n);
            const size_t whole = pend.size() / kTsPacket * kTsPacket;
            if (whole > 0) {
                for (size_t i = 0; i + kTsPacket <= whole; i += kTsPacket)
                    if (static_cast<unsigned char>(pend[i]) == 0x47 &&
                        ((static_cast<unsigned>(static_cast<unsigned char>(pend[i + 1]) & 0x1f) << 8) |
                         static_cast<unsigned char>(pend[i + 2])) == kLiveVideoPid) {
                        lastVideo = Clock::now();
                        break;
                    }
                reserve.append(pend.data(), whole);
                pend.erase(0, whole);
            }
            return true;
        };
        // Poll-only ad skip: a byte leaves only after a poll that started later could have seen its segment.
        auto sendLimit = [&]() -> uint64_t {
            if (!pollOnly)
                return reserve.inPos();
            const auto edge = Clock::now() - std::chrono::milliseconds(pollHoldbackMs);
            for (auto it = inPosAt.rbegin(); it != inPosAt.rend(); ++it)
                if (it->first <= edge)
                    return it->second;
            return reserve.outPos();
        };
        auto flushReserve = [&]() -> bool {
            const uint64_t limit = sendLimit();
            for (int i = 0; i < 256 && reserve.outPos() < limit; ++i) {
                size_t n = 0;
                const char* p = reserve.front(n);
                if (!n)
                    break;
                n = static_cast<size_t>(std::min<uint64_t>(n, limit - reserve.outPos()));
                const ssize_t w = ::send(client, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (w > 0) {
                    reserve.consume(static_cast<size_t>(w));
                    sent += static_cast<uint64_t>(w);
                    lastSend = Clock::now();
                    continue;
                }
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    break;
                return false;
            }
            reserveBytes_.store(reserve.size());
            return true;
        };
        // Stream position of the next ffmpeg byte (reserve input, or what went straight to the player).
        auto streamPos = [&]() -> uint64_t { return rsv ? reserve.inPos() : realSent; };
        // Position the player has been sent up to.
        auto playerPos = [&]() -> uint64_t { return rsv ? reserve.outPos() : realSent; };
        auto playerArmed = [&]() -> bool {
            if (frameArmed)
                return true;
            return headerSent && (!probe || framePos < 0) && msSince(tFirstData) >= kPlutoAdSkipArmMs;
        };
        // Reads all pending ffmpeg stderr. True when an open line says cut before the bytes that follow it.
        auto scanOpens = [&]() -> bool {
            bool cut = false;
            char b[1024];
            for (int i = 0; i < 256 && ch.err >= 0; ++i) {
                const ssize_t n = ::read(ch.err, b, sizeof b);
                if (n <= 0)
                    break;
                errLine.append(b, b + n);
                size_t nl;
                while ((nl = errLine.find('\n')) != std::string::npos) {
                    const std::string line = errLine.substr(0, nl);
                    errLine.erase(0, nl + 1);
                    pa::SegKind k = pa::SegKind::Unknown;
                    const bool open = pa::parseOpenLine(line, k);
                    // Info level quotes every URL: only other lines reach the exit log, each redacted whole so a
                    // tail cut cannot start inside a query string.
                    if (!open && line.find("Opening '") == std::string::npos) {
                        ch.errTail += redactTail(line);
                        ch.errTail += ' ';
                        if (ch.errTail.size() > 600)
                            ch.errTail.erase(0, ch.errTail.size() - 600);
                    }
                    if (!open)
                        continue;
                    opensSeen = true;
                    if (k == pa::SegKind::Content) {
                        ++contentOpens;
                        tunePod = false;
                    }
                    if (adm.cutOnOpen(k))
                        cut = true;
                }
                if (errLine.size() > 16384)
                    errLine.clear();
            }
            return cut;
        };
        auto drainChildErr = [&]() {
            if (ads)
                scanOpens();
            else
                drainErr(ch);
        };
        Clock::time_point holdT{};
        // Cut the stream at `pos` (nothing this child wrote after it reaches the player), reap the child, hold.
        auto holdAt = [&](uint64_t pos, const char* why) {
            pend.clear();
            if (rsv) {
                const uint64_t out = reserve.outPos();
                if (pos < out)
                    pos = (out + kTsPacket - 1) / kTsPacket * kTsPacket;
                reserve.truncateTo(pos);
                reserveBytes_.store(reserve.size());
            }
            if (ch.pid > 0) {
                const int64_t liveMs = msSince(ch.started);
                const int rc = reapChild(ch);
                std::fprintf(stderr,
                             "misterplexd: live relay: ffmpeg gone why=ad_skip rc=%d after_ms=%lld sent=%llu "
                             "spawns=%d\n",
                             rc, static_cast<long long>(liveMs), static_cast<unsigned long long>(sent), spawns);
            }
            errLine.clear();
            tunePod = false;
            dry = false;
            if (gapOn && !adCard) {
                adCard = true;
                adCardAt = gapOnAt;
            }
            if (adm.phase() != pa::Phase::Holding) {
                holdT = Clock::now();
                std::fprintf(stderr, "misterplexd: pluto adskip hold why=%s reserve_bytes=%zu\n", why,
                             rsv ? reserve.size() : size_t(0));
            }
            adm.hold(msSince(tServe));
        };
        auto onAdOpen = [&](uint64_t pos) -> bool {
            if (contentOpens == 0 && !playerArmed()) {
                if (!tunePod)
                    std::fprintf(stderr, "misterplexd: pluto adskip tune_in_pod\n");
                tunePod = true;
                return false;
            }
            holdAt(pos, "path_ad");
            return true;
        };
        bool respawnCbPending = false;
        uint64_t respawnMark = 0;
        auto reserveTick = [&]() {
            if (!rsv || !headerSent)
                return;
            const auto now = Clock::now();
            const bool steady = adm.phase() == pa::Phase::Content && ch.pid > 0 && msSince(ch.started) >= 20000;
            if (!steady) {
                rateFrom = reserve.inPos();
                rateAt = now;
            } else if (msSince(rateAt) >= 10000) {
                const int64_t el = msSince(rateAt);
                const int64_t bps = static_cast<int64_t>(reserve.inPos() - rateFrom) * 8000 / el;
                rateBps = std::clamp<int64_t>(rateBps ? (rateBps * 3 + bps) / 4 : bps, 250000, 40000000);
                capBytes = pa::reserveCapBytes(reserveS, rateBps);
                rateFrom = reserve.inPos();
                rateAt = now;
            }
            const int64_t bps = rateBps ? rateBps : pa::kReserveDefaultBps;
            const int64_t readyMs = static_cast<int64_t>(reserve.size()) * 8000 / bps;
            rsvPeakMs = std::max(rsvPeakMs, readyMs);
            if (msSince(lastRsvLog) >= kLiveReserveLogMs) {
                std::fprintf(stderr,
                             "misterplexd: live reserve target_s=%d ready_ms=%lld cap_bytes=%zu bytes=%zu bps=%lld "
                             "peak_ms=%lld\n",
                             reserveS, static_cast<long long>(readyMs), capBytes, reserve.size(),
                             static_cast<long long>(rateBps), static_cast<long long>(rsvPeakMs));
                lastRsvLog = now;
            }
            if (ads && (inPosAt.empty() ||
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - inPosAt.back().first).count() >=
                            100)) {
                inPosAt.emplace_back(now, reserve.inPos());
                if (inPosAt.size() > 1200)
                    inPosAt.erase(inPosAt.begin(), inPosAt.begin() + 600);
            }
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
            drainChildErr();
            const int64_t liveMs = ch.pid > 0 ? msSince(ch.started) : 0;
            const std::string tail = redactTail(ch.errTail);
            const int rc = reapChild(ch);
            std::fprintf(stderr,
                         "misterplexd: live relay: ffmpeg gone why=%s rc=%d after_ms=%lld sent=%llu spawns=%d "
                         "err='%s'\n",
                         why, rc, static_cast<long long>(liveMs), static_cast<unsigned long long>(sent), spawns,
                         tail.c_str());
            if (stop_.load())
                return false;
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
            // A child lost while the window still holds an ad (video_stall at the splice, exit) must not respawn
            // into the pod: hold until the windows are all content.
            if (ads && adm.phase() != pa::Phase::Holding && adm.podInWindow()) {
                holdAt(streamPos(), why);
                return true;
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
            if (!intentionalVideoStall)
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
#ifndef MUT_N17_COUNT_LONG
                if (liveMs < scoreShortMs)
#endif
                    failScore += 1.0;
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
            if (!rsv && !pend.empty()) {
                if (!writeAll(client, pend.data(), pend.size()))
                    return false;
                sent += pend.size();
            }
#endif
            pend.clear();
            // (Only reachable unaligned when hold-back is off.) Pad to a packet boundary. The reserve holds whole
            // packets only; its partly sent packet finishes from the reserve.
#ifndef MUT_LIVE_NO_PAD
            if (!rsv && sent % kTsPacket != 0) {
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
        auto forward = [&](const char* p, size_t n) -> bool {
            if (!headerSent) {
                if (!writeAll(client, kHdr, sizeof kHdr - 1))
                    return false;
                headerSent = true;
                tFirstData = Clock::now();
                startAdSkipPoll();
            }
#ifdef MUT_LIVE_NO_HOLDBACK
            if (!writeAll(client, p, n))
                return false;
            sent += n;
            realSent += n;
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
                sent += whole;
                realSent += whole;
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
                drainChildErr();
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
        adHoldActive_.store(false, std::memory_order_release);
        auto publishAdHold = [&] {
            // Only the ad-skip machine (MPX_PLUTO_ADSKIP on), and only while the
            // hold is active. Resuming is not a hold: the watchdog counts from
            // the next poll after resume spawn / content_return / hold_cap.
            adHoldActive_.store(adHoldExemptsWatchdog(ads, tunePod, adm.phase()),
                                std::memory_order_release);
        };
        while (alive && !stop_.load()) {
            publishAdHold();
            const auto now = Clock::now();
            if (polls) {
                pa::Window pv;
                pa::Window pva;
                Clock::time_point pAt{};
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> lk(polls->mu);
                    if (polls->n != pollSeen) {
                        pollSeen = polls->n;
                        pv = polls->v;
                        pva = polls->a;
                        pAt = polls->startedAt;
                        fresh = true;
                    }
                }
                if (fresh) {
                    const pa::Machine::PollResult r = adm.onPoll(
                        pv, pva, std::chrono::duration_cast<std::chrono::milliseconds>(pAt - tServe).count());
                    if (r.podEntered)
                        std::fprintf(stderr, "misterplexd: pluto adskip in_ad why=path_ad seq=%lld phase=%s\n",
                                     static_cast<long long>(r.adSeq), pa::phaseName(adm.phase()));
                    if (rsv && pa::windowAllContent(pv) && pa::windowAllContent(pva)) {
                        for (auto it = inPosAt.rbegin(); it != inPosAt.rend(); ++it)
                            if (it->first <= pAt) {
                                cleanPos = it->second;
                                haveCleanPos = true;
                                break;
                            }
                    }
                }
                // No child ever logged an "Opening" line: cut at the last position a clean poll vouched for, and
                // hold fresh bytes back long enough for a poll to see their segment first.
                const bool noOpens = !opensSeen && headerSent && msSince(tFirstData) >= openProbeMs;
                if (noOpens && rsv && !pollOnly)
                    std::fprintf(stderr, "misterplexd: pluto adskip no_open_lines holdback_ms=%d\n", pollHoldbackMs);
                pollOnly = noOpens && rsv;
                if (noOpens && ch.pid > 0 && adm.phase() != pa::Phase::Holding && adm.podInWindow())
                    holdAt(haveCleanPos ? cleanPos : streamPos(), "poll_ad");
                if (tunePod && ch.pid > 0 && playerArmed())
                    holdAt(playerPos(), "tune_in_pod");
            }
            if (ch.pid < 0 && ads && adm.phase() == pa::Phase::Holding) {
                const bool capped = msSince(holdT) >= holdCapMs;
                if (adm.resumeReady() || capped) {
                    const pa::StartIndex si = adm.resumeStart();
                    const bool aligned = rsv && !capped && si.ok;
                    std::function<bool()> adReturnReopen;
                    {
                        std::lock_guard<std::mutex> lk(cbMu_);
                        adReturnReopen = adReturnReopenCb_;
                    }
                    if (adReturnReopen) {
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            nextStartV_ = aligned ? si.video : -1;
                            nextStartA_ = aligned ? si.audio : -1;
                        }
                        if (adReturnReopen()) {
                            std::fprintf(stderr,
                                         "misterplexd: pluto adskip resume why=%s seq=%lld start_v=%d start_a=%d "
                                         "held_ms=%lld reopen=1\n",
                                         capped ? "hold_cap" : "content_return",
                                         static_cast<long long>(aligned ? si.seq : -1),
                                         aligned ? si.video : -1, aligned ? si.audio : -1,
                                         static_cast<long long>(msSince(holdT)));
                            reopenHandoff = true;
                            alive = false;
                            break;
                        }
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            nextStartV_ = 0;
                            nextStartA_ = 0;
                        }
                    }
                    spawnSrc.liveStartIndexVideo = aligned ? si.video : -1;
                    spawnSrc.liveStartIndexAudio = aligned ? si.audio : -1;
                    const double off = std::chrono::duration<double>(now - tFirstData).count();
                    if (!spawnChild(spawnSrc, off, ch)) {
                        std::fprintf(stderr, "misterplexd: live relay: spawn failed\n");
                        break;
                    }
                    ++spawns;
                    consecutiveFails = 0;
                    gapBytes = false;
                    lastReal = Clock::now();
                    lastVideo = lastReal;
                    resumeMark = streamPos();
                    resumeBytes = false;
                    adm.resumed();
                    std::fprintf(stderr,
                                 "misterplexd: pluto adskip resume why=%s seq=%lld start_v=%d start_a=%d held_ms=%lld\n",
                                 capped ? "hold_cap" : "content_return", static_cast<long long>(aligned ? si.seq : -1),
                                 spawnSrc.liveStartIndexVideo, spawnSrc.liveStartIndexAudio,
                                 static_cast<long long>(msSince(holdT)));
                    if (rsv) {
                        respawnCbPending = true;
                        respawnMark = resumeMark;
                    } else {
                        fireRespawn();
                    }
                }
            } else if (ch.pid < 0) {
                if (!headerSent && msSince(tServe) >= firstByteMs) {
                    send502();
                    break;
                }
                if (now >= nextSpawn) {
                    double off = 0.0;
                    if (havePrev && headerSent)
                        off = std::chrono::duration<double>(now - tFirstData).count();
                    if (havePrev && headerSent) {
                        respawnTimes.push_back(now);
                        spawnSrc.liveStartIndexVideo = -1;
                        spawnSrc.liveStartIndexAudio = -1;
                        if (rsv)
                            pa::onContentRespawn(reserve);
                    }
                    if (!spawnChild(spawnSrc, off, ch)) {
                        std::fprintf(stderr, "misterplexd: live relay: spawn failed\n");
                        if (!headerSent)
                            send502();
                        break;
                    }
                    ++spawns;
                    havePrev = true;
                    gapBytes = false;
                    lastReal = Clock::now();
                    lastVideo = lastReal;
                    // Header already sent: this is a video_stall / TLS-EOF / stall respawn.
                    // Rejoin HDMI PCM so remux AAC is not stuck at the grabber floor (-81.9).
                    if (headerSent) {
                        std::fprintf(stderr, "misterplexd: live relay: ffmpeg respawn spawns=%d\n", spawns);
                        // Reserve: the player still plays the old child's bytes; rejoin when the new ones reach it.
                        if (rsv) {
                            respawnCbPending = true;
                            respawnMark = reserve.inPos();
                        } else {
                            fireRespawn();
                        }
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
            const bool full = rsv && reserve.size() >= capBytes;
            pollfd pf[3];
            int np = 0;
            if (ch.out >= 0 && !full) {
                pf[np] = {ch.out, POLLIN, 0};
                ++np;
            }
            if (ch.err >= 0) {
                pf[np] = {ch.err, POLLIN, 0};
                ++np;
            }
            if (rsv && headerSent && reserve.outPos() < sendLimit()) {
                pf[np] = {client, POLLOUT, 0};
                ++np;
            }
            if (np > 0)
                ::poll(pf, static_cast<nfds_t>(np), 100);
            else if (ads && adm.phase() == pa::Phase::Holding)
                ::usleep(50 * 1000);
            if (ch.pid > 0) {
                if (ads) {
                    if (scanOpens())
                        onAdOpen(streamPos());
                } else {
                    drainErr(ch);
                }
                bool eof = false;
                for (int i = 0; ch.pid > 0 && i < 64 && ch.out >= 0; ++i) {
                    if (rsv && reserve.size() >= capBytes) {
                        // Reserve full: ffmpeg waits on its pipe; that is not a stall.
                        lastReal = Clock::now();
                        lastVideo = lastReal;
                        break;
                    }
                    char b[8192];
                    const ssize_t n = ::read(ch.out, b, sizeof b);
                    if (n > 0) {
                        // An open line seen after this read may belong to bytes inside it: cut at its start.
                        if (ads && scanOpens() && onAdOpen(streamPos()))
                            break;
                        if (!(rsv ? ingest(b, static_cast<size_t>(n)) : forward(b, static_cast<size_t>(n)))) {
                            endRelay("client_write");
                            break;
                        }
                        lastReal = Clock::now();
                        lastData = lastReal;
                        if (msSince(ch.started) >= healthyMs)
                            consecutiveFails = 0;   // only a child that stayed up ends a failure streak
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
                if (ch.pid < 0)
                    continue;   // cut before an ad: holding
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
                if (!headerSent && msSince(tServe) >= firstByteMs) {
                    reapChild(ch);
                    send502();
                    break;
                }
            }
            if (rsv && headerSent) {
                if (!flushReserve()) {
                    endRelay("client_write");
                    break;
                }
                if (respawnCbPending && reserve.outPos() > respawnMark) {
                    respawnCbPending = false;
                    fireRespawn();
                }
                reserveTick();
            }
            if (plutoGap && headerSent && ads && (adCard || adm.phase() != pa::Phase::Content)) {
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
                const bool probeLive = frameArmed && p >= 0;
                if (adm.phase() == pa::Phase::Resuming && playerPos() > resumeMark) {
                    if (!resumeBytes) {
                        resumeBytes = true;
                        resumeBytesAt = Clock::now();
                        posAtResume = p;
                    }
                    const bool onScreen = probeLive ? p != posAtResume : msSince(resumeBytesAt) >= adHoldMs;
                    if (!gapOn || onScreen) {
                        if (gapOn) {
                            gapOn = false;
                            gapResumed = false;
                            emitGap(LiveGap::Clear, LiveGapWhy::ContentReturn);
                        }
                        adCard = false;
                        adCardDropped = false;
                        dry = false;
                        adm.contentOnScreen();
                    }
                } else if (!gapOn) {
                    // Holding (or a resumed child not on the wire yet): finish the buffered content first. The card
                    // goes up once nothing is left to send and the picture has stopped.
                    if (rsv && reserve.size() > 0) {
                        dry = false;
                    } else if (!dry) {
                        dry = true;
                        dryAt = Clock::now();
                        posDry = p;
                        posDryAt = dryAt;
                    } else {
                        if (p != posDry) {
                            posDry = p;
                            posDryAt = Clock::now();
                        }
                        const bool frozen = probeLive ? msSince(posDryAt) >= adFreezeMs
                                                      : msSince(dryAt) >= kPlutoAdSkipNoProbeCardMs;
                        if (frozen && !adCardDropped) {
                            gapOn = true;
                            gapResumed = false;
                            gapBytes = false;
                            gapWhy = LiveGapWhy::AdSkip;
                            gapOnAt = Clock::now();
                            adCard = true;
                            adCardAt = gapOnAt;
                            emitGap(LiveGap::Silent, LiveGapWhy::AdSkip);
                        }
                    }
                } else if (adm.phase() == pa::Phase::Holding &&
                           !pa::Machine::cardHeld(msSince(adCardAt), adHoldMs)) {
                    gapOn = false;
                    adCardDropped = true;
                    emitGap(LiveGap::Clear, LiveGapWhy::HoldTimeout);
                }
            } else if (plutoGap && headerSent) {
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
                    // With a reserve the player keeps playing through a respawn: the card waits until nothing is
                    // left to send and the picture has stopped (socket and player buffers hold more than the reserve).
                    const bool respawning =
                        ch.pid < 0 && (!rsv || (reserve.size() == 0 && ((frameArmed && p >= 0)
                                                                              ? msSince(framePosAt) >= adFreezeMs
                                                                              : msSince(lastSend) >= kPlutoAdSkipNoProbeCardMs)));
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
            if (rsv) {
                // Never blocks: a full socket means the player has plenty to read.
                if (headerSent && reserve.size() == 0 && msSince(lastSend) >= keepaliveMs &&
                    msSince(lastKeep) >= keepaliveMs && sent % kTsPacket == 0) {
                    const ssize_t w = ::send(client, kNull, kTsPacket, MSG_NOSIGNAL | MSG_DONTWAIT);
                    if (w > 0 && static_cast<size_t>(w) < kTsPacket &&
                        !writeAll(client, reinterpret_cast<const char*>(kNull) + w, kTsPacket - static_cast<size_t>(w))) {
                        endRelay("keepalive");
                        break;
                    }
                    if (w > 0) {
                        sent += kTsPacket;
                        lastKeep = Clock::now();
                    } else if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                        endRelay("keepalive");
                        break;
                    }
                }
            } else if (headerSent && msSince(lastReal) >= keepaliveMs && msSince(lastKeep) >= keepaliveMs &&
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
        adHoldActive_.store(false, std::memory_order_release);
        if (stop_.load() && ch.pid > 0) {
            std::fprintf(stderr, "misterplexd: live relay: ending why=stop sent=%llu spawns=%d\n",
                         static_cast<unsigned long long>(sent), spawns);
            drainChildErr();
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
        if (polls)
            polls->stop.store(true);
        if (pollThread.joinable())
            pollThread.join();
        reserveBytes_.store(0);
        if (gapOn && !reopenHandoff)
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
    std::function<bool()> adReturnReopenCb_;
    PlaylistFetcher fetcher_;
    std::atomic<size_t> reserveBytes_{0};
    std::mutex mu_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> adHoldActive_{false};
    std::atomic<pid_t> ffmpegPid_{-1};
    int listenFd_ = -1;
    int nextStartV_ = 0;
    int nextStartA_ = 0;
    LiveSource src_;
    std::thread thread_;
};

inline LiveRelay& liveRelay() {
    static LiveRelay r;
    return r;
}

}  // namespace yt_live
}  // namespace airplay
}  // namespace misterplex
