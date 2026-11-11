#pragma once
#include "airplay/ns.hpp"
// Local YouTube resolve (no gcast-xcode). One resolve at a time: try the ready
// resident Android worker, then short-lived yt-dlp children on its client ladder.
// A timeout or RSS kill stops the ladder. A failed extractor may refresh the
// binary, but playback never waits on that refresh.
//
// Stock MiSTer Python and the release format (2026-10-19):
//   Current yt-dlp zipapp requires Python >= 3.10. That is
//   requires-python in https://github.com/yt-dlp/yt-dlp/blob/master/pyproject.toml
//   (fetched 2026-10-19). The zipapp is that program, run by /usr/bin/python3.
//   Stock MiSTer Linux does ship /usr/bin/python3. MiSTer FPGA Forum topic 6439
//   (aberu, 2023-04-25; morf77 the same week) says the image already supports
//   `#!/usr/bin/python3` and is kept small, so extra modules are refused.
//   MiSTer Downloader runs on that interpreter. Its PC launcher documents the
//   same codebase's floor as Python 3.9
//   (https://github.com/MiSTer-devel/Downloader_MiSTer/blob/main/docs/pc-launcher.md).
//   No stock linux.img release publishes Python >= 3.10. A 3.9 zipapp pin would
//   be an old yt-dlp, and YouTube breaks those quickly.
//   Upstream no longer ships a bare yt-dlp_linux_armv7l. The asset is
//   yt-dlp_linux_armv7l.zip, a PyInstaller onedir (binary + _internal/).
//   It bundles its own CPython and does not use the image interpreter.
//   Installed at /media/fat/linux/misterplex/yt-dlp/ with a shell launcher
//   (FAT has no symlinks). If a later image reports Python >= 3.10 and only
//   a zipapp is installed, chooseRelease will use the zipapp.
//
// Memory (512 MB MiSTer, gate 7, daemon + libav + framebuffer already resident):
//   One yt-dlp_linux_armv7l `-g` resolve is estimated at 80–120 MB peak RSS.
//   The onefile bootloader unpacks an embedded interpreter, then one extractor
//   runs. This tree does not measure that on a MiSTer (no board in this run);
//   the child is sampled from /proc/<pid>/status and the peak VmHWM is what
//   MEMSTAT logs as ytdlp_kb. Only one resolve runs at a time. The child is
//   SIGKILLed if it exceeds 96 MB VmRSS or the resolve timeout (90 s). A killed child is not retried.

#include "mem_stat.hpp"
#include "../log_redact.hpp"
#include "ca_bundle.hpp"
#include "wall_clock.hpp"
#include "gcast/yt_captions.hpp"
#include "yt_cache.hpp"
#include "yt_warm.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace misterplex::airplay::yt_dlp {

enum class ReleaseKind { None, StandaloneArmv7l, Zipapp };

// What this tree ships for a stock MiSTer. See the comment at the top.
inline constexpr ReleaseKind kStockMiSTerRelease = ReleaseKind::StandaloneArmv7l;
inline constexpr const char* kStandaloneAsset = "yt-dlp_linux_armv7l";
inline constexpr const char* kOnedirZipAsset = "yt-dlp_linux_armv7l.zip";
inline constexpr const char* kOnedirDir = "/media/fat/linux/misterplex/yt-dlp";
inline constexpr int kZipappPyMajor = 3;
inline constexpr int kZipappPyMinor = 10;
// #335: the ARM card needs 18-19 s for one -g even warm (PyInstaller start ~8 s,
// 1747 extractors ~3 s); under load it went past the old 20 s and every play died
// "yt-dlp timeout". 90 s total default; YT_RESOLVE_TIMEOUT (misterplex.conf, seconds) or
// MPX_YT_RESOLVE_TIMEOUT (env wins) override, clamped to [kResolveTimeoutMinMs, kResolveTimeoutMaxMs].
#ifdef MUT_335_OLD_TIMEOUT
inline constexpr int kResolveTimeoutMs = 20000;
#else
inline constexpr int kResolveTimeoutMs = 90000;
#endif
// #339 glass: under load (loadavg 5.7) the CPU-bound part of yt-dlp (~8 s Python
// start + ~3 s extractor load idle) stretches past 45 s. The resolve child runs at
// nice -5 so a play resolve gets ahead of background load; failure (non-root) is
// ignored. Only the child is boosted; the caption child keeps default.
// #339 review N1: execv keeps nice, so yt-dlp's own grandchildren (deno/node nsig,
// any ffmpeg it spawns) inherit it and cannot be reset from here. -10 outweighed a
// nice-0 decode/audio thread ~10:1 when a resolve overlaps playback; -5 is ~3:1.
// clampResolveNice() bounds a boost to [kResolveNiceMin, 0].
inline constexpr int kResolveNice = -5;
inline constexpr int kResolveNiceMin = -5;
inline constexpr int clampResolveNice(int n) {
#ifdef MUT_339_NO_NICE_CLAMP
    return n;
#else
    return n < kResolveNiceMin ? kResolveNiceMin : (n > 0 ? 0 : n);
#endif
}
inline constexpr int kResolveTimeoutMinMs = 10000;
inline constexpr int kResolveTimeoutMaxMs = 120000;
inline constexpr unsigned kRssCapKb = 96u * 1024u;
// Caption child only (--skip-download, one subtitle file). yt-dlp on the ARM
// card takes 25-40 s to extract before it writes s.en.vtt (glass 10/31); the
// 20 s media timeout killed every attempt ("no cues via=ytdlp"). A play
// resolve that queues behind it aborts the child (ResolveOptions::abort).
inline constexpr int kCaptionTimeoutMs = 75000;
// Whole ladder (all rungs, all exits) shares this budget: resolveMu is never
// held for more than one caption timeout (not 3 x 75 s on exit!=0 retries).
inline constexpr int kCaptionTotalBudgetMs = kCaptionTimeoutMs;

// Seconds ("60", "60s") to ms, clamped. Anything else keeps kResolveTimeoutMs.
inline int resolveTimeoutFromText(const char* e) {
    if (!e || !e[0])
        return kResolveTimeoutMs;
    long v = 0;
    std::size_t n = 0;
    for (; e[n] >= '0' && e[n] <= '9'; ++n) {
        v = v * 10 + (e[n] - '0');
        if (v > 100000)
            return kResolveTimeoutMs;
    }
    if (n == 0 || !(e[n] == 0 || ((e[n] == 's' || e[n] == 'S') && e[n + 1] == 0)))
        return kResolveTimeoutMs;
    long ms = v * 1000;
#ifndef MUT_335_NO_CLAMP
    if (ms < kResolveTimeoutMinMs)
        ms = kResolveTimeoutMinMs;
    if (ms > kResolveTimeoutMaxMs)
        ms = kResolveTimeoutMaxMs;
#endif
    return static_cast<int>(ms);
}

inline int resolveTimeoutMs() {
#ifdef MUT_335_IGNORE_CONF
    return kResolveTimeoutMs;
#else
    if (const char* e = std::getenv("MPX_YT_RESOLVE_TIMEOUT"); e && e[0])
        return resolveTimeoutFromText(e);
    return resolveTimeoutFromText(std::getenv("YT_RESOLVE_TIMEOUT"));
#endif
}

inline bool pythonMeetsZipapp(int major, int minor) {
    if (major != kZipappPyMajor)
        return major > kZipappPyMajor;
    return minor >= kZipappPyMinor;
}

// Standalone wins when both are on disk: it does not need the image Python.
// Zipapp only when Python is new enough. Otherwise there is nothing to run.
inline ReleaseKind chooseRelease(int pyMajor, int pyMinor, bool haveStandalone, bool haveZipapp) {
    if (haveStandalone)
        return ReleaseKind::StandaloneArmv7l;
    if (haveZipapp && pythonMeetsZipapp(pyMajor, pyMinor))
        return ReleaseKind::Zipapp;
    return ReleaseKind::None;
}

inline std::string searchedPaths() {
    return std::string(kOnedirDir) + "/yt-dlp, " + kOnedirDir + "/yt-dlp_linux_armv7l, "
           "/media/fat/misterplex/bin/yt-dlp_linux_armv7l, "
           "/media/fat/misterplex/bin/yt-dlp, "
           "/media/fat/misterplex/yt-dlp";
}

inline bool fileExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline bool fileExecutable(const std::string& path) {
    return fileExists(path) && ::access(path.c_str(), X_OK) == 0;
}

inline bool looksElf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char b[4] = {};
    in.read(b, 4);
    return in.gcount() == 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

inline bool looksZipapp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char b[4] = {};
    in.read(b, 4);
    if (in.gcount() < 2)
        return false;
    if (b[0] == '#' && b[1] == '!')
        return true;
    return in.gcount() == 4 && b[0] == 'P' && b[1] == 'K' && static_cast<unsigned char>(b[2]) == 3 &&
           static_cast<unsigned char>(b[3]) == 4;
}

struct FoundBinary {
    std::string path;
    ReleaseKind kind = ReleaseKind::None;
};

inline const char* const* candidatePaths() {
    static const char* p[] = {
        "/media/fat/linux/misterplex/yt-dlp/yt-dlp",
        "/media/fat/linux/misterplex/yt-dlp/yt-dlp_linux_armv7l",
        "/media/fat/misterplex/bin/yt-dlp_linux_armv7l",
        "/media/fat/misterplex/bin/yt-dlp",
        "/media/fat/misterplex/yt-dlp",
        nullptr,
    };
    return p;
}

// A shell launcher (#!/bin/sh) is the onedir entry point. It is not a zipapp.
inline bool looksShellLauncher(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line))
        return false;
    if (line.size() < 2 || line[0] != '#' || line[1] != '!')
        return false;
    if (line.find("python") != std::string::npos)
        return false;
    return line.find("sh") != std::string::npos;
}

inline FoundBinary considerBinary(const std::string& path, int pyMajor, int pyMinor) {
    if (!fileExecutable(path))
        return {};
    if (looksElf(path) || looksShellLauncher(path))
        return FoundBinary{path, ReleaseKind::StandaloneArmv7l};
    if (looksZipapp(path) && pythonMeetsZipapp(pyMajor, pyMinor))
        return FoundBinary{path, ReleaseKind::Zipapp};
    return {};
}

// pyMajor/pyMinor describe the image interpreter. Pass <0 to skip the zipapp.
// preferred (or MPX_YTDLP_BIN) is a file or the onedir directory.
inline FoundBinary findBinary(int pyMajor, int pyMinor, const char* const* paths = candidatePaths(),
                              const char* preferred = nullptr) {
    std::string pref = preferred ? preferred : "";
    if (pref.empty()) {
        if (const char* e = std::getenv("MPX_YTDLP_BIN"))
            pref = e;
    }
    if (!pref.empty()) {
        struct stat st {};
        if (::stat(pref.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            FoundBinary hit = considerBinary(pref + "/yt-dlp", pyMajor, pyMinor);
            if (hit.kind == ReleaseKind::None)
                hit = considerBinary(pref + "/" + std::string(kStandaloneAsset), pyMajor, pyMinor);
            if (hit.kind != ReleaseKind::None)
                return hit;
        } else {
            FoundBinary hit = considerBinary(pref, pyMajor, pyMinor);
            if (hit.kind != ReleaseKind::None)
                return hit;
        }
    }
    FoundBinary zip;
    for (int i = 0; paths && paths[i]; ++i) {
        const std::string path = paths[i];
        if (!fileExecutable(path))
            continue;
        if (looksElf(path) || looksShellLauncher(path))
            return FoundBinary{path, ReleaseKind::StandaloneArmv7l};
        if (zip.path.empty() && looksZipapp(path))
            zip = FoundBinary{path, ReleaseKind::Zipapp};
    }
    if (zip.kind == ReleaseKind::Zipapp && pythonMeetsZipapp(pyMajor, pyMinor))
        return zip;
    return {};
}

inline bool executablePresent(int pyMajor = -1, int pyMinor = -1) {
    return findBinary(pyMajor, pyMinor).kind != ReleaseKind::None;
}

// The image interpreter. 0,0 when python3 is missing or does not answer.
inline void imagePython(int& major, int& minor) {
    major = minor = 0;
    FILE* f = ::popen("python3 -c 'import sys; print(\"%d %d\" % sys.version_info[:2])'", "r");
    if (!f)
        return;
    if (std::fscanf(f, "%d %d", &major, &minor) != 2)
        major = minor = 0;
    ::pclose(f);
}

struct ResolveResult {
    bool ok = false;
    std::string url;
    std::string audioUrl;   // set when -g prints a second line (DASH video + audio)
    std::string title;       // extract title when YT_CAST_TITLE is on; empty on the tip path
    std::string error;
    bool extractionError = false;
    bool timedOut = false;
    bool aborted = false;   // ResolveOptions::abort fired
    bool rssKilled = false;
    unsigned peakRssKb = 0;
    int exitCode = -1;
    int clientAttempts = 0;     // player_client ladder steps actually spawned
    std::string playerClient;   // "default", "mweb,web_safari", or "tv,web_embedded"
};

struct ResolveOptions {
    std::string bin;                 // executable (or zipapp) to spawn
    bool zipapp = false;            // bin is a zipapp: exec python3 bin ...
    std::string python = "python3";
    std::string videoId;
    int timeoutMs = resolveTimeoutMs();
    int nice = kResolveNice;         // setpriority() in the child; 0 leaves it alone; <0 clamped
    unsigned rssCapKb = kRssCapKb;
    std::vector<std::string> extraArgs;  // tests: replace the yt-dlp args. Non-empty skips the ladder.
    // Empty keeps kResolveFormat (VOD https). YT_LIVE sets a longer selector; the ladder still runs.
    std::string format;
    // Polled every 50 ms; true kills the child (ResolveResult::aborted).
    std::function<bool()> abort;
    // 403 refresh: skip warm Android and the android ladder rung (P2-c).
    bool skipWarm = false;
    bool skipAndroid = false;
};

// nullptr keeps kResolveFormat. MUT_YT_LIVE_DROP_FORMAT ignores a live selector and stays on VOD https.
inline const char* resolveFormatOf(const ResolveOptions& opt) {
#ifdef MUT_YT_LIVE_DROP_FORMAT
    (void)opt;
    return nullptr;
#else
    return opt.format.empty() ? nullptr : opt.format.c_str();
#endif
}

// Android historically did not need the JS player. In 2026 SABR-only android
// sessions omit https URLs; without player_skip yt-dlp still downloads the
// webpage + player JS and then hangs on nsig with no deno/node (MiSTer has
// none). player_skip=webpage,configs keeps the Innertube JSON path and
// returns progressive itag 18 when present. Try Android first, then default,
// mweb/web_safari and tv/web_embedded.
struct YtClientSpec {
    const char* name;
    const char* extractorArgs; // nullptr, or "youtube:player_client=..."
};

inline constexpr YtClientSpec kYtPlayerClientRetry[] = {
    {"android", "youtube:player_client=android;player_skip=webpage,configs"},
    {"default", nullptr},
    {"mweb,web_safari", "youtube:player_client=mweb,web_safari"},
    {"tv,web_embedded", "youtube:player_client=tv,web_embedded"},
};
inline constexpr int kYtPlayerClientRetryCount =
    static_cast<int>(sizeof(kYtPlayerClientRetry) / sizeof(kYtPlayerClientRetry[0]));

// Standalone -f (empty extraArgs). Progressive H.264/AAC <=720p is one URL.
// YouTube often has no progressive mux at that height; the tail is an AVC
// video plus AAC audio, capped at 30 fps (OB is real time at 720p30, not
// 720p60; 60 fps uploads offer 720p60 AVC first). Every selector, including
// each progressive b[...] option and each side of a DASH pair, requires
// protocol=https so yt-dlp cannot pick HLS (m3u8), which this player cannot
// demux (ARM libav has mpegts, not the hls demuxer). -g then prints one or
// two URLs. The player opens each one (yt-av playable). Nothing is remuxed
// onto the card. YT_LIVE (yt_live.hpp) appends m3u8 fallbacks; those URLs are
// remuxed to MPEG-TS and are not opened directly.
inline constexpr char kResolveFormat[] =
    "b[height<=720][vcodec^=avc][acodec^=mp4a][fps<=30][protocol=https]"
    "/b[height<=720][ext=mp4][fps<=30][protocol=https]"
    "/b[height<=720][fps<=30][protocol=https]"
    "/bv*[height<=720][vcodec^=avc][fps<=30][protocol=https]+ba[acodec^=mp4a][protocol=https]"
    "/bv*[height<=720][vcodec^=avc][fps<=30][protocol=https]+ba[ext=m4a][protocol=https]";

// YT_CAST_TITLE (misterplex.conf) / MPX_YT_CAST_TITLE (env wins). Default off.
// On: the same yt-dlp child that prints the media URL also prints the extract
// title. yt-dlp maps player videoDetails.title onto the info "title" field
// (a preferred-language translation only when one is configured; this argv
// does not set one). --print reads that field from the extract already in
// hand. It is not a second process and it does not request captions.
// Off: the -g argv is unchanged and the title stays empty. Unset is that tip path.
inline bool ytCastTitleEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n) {
        const unsigned char c = static_cast<unsigned char>(e[n]);
        buf[n] = static_cast<char>(std::tolower(c));
    }
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

inline bool ytCastTitleEnabled() {
#ifdef MUT_YT_CAST_TITLE_IGNORED
    return false;
#elif defined(MUT_YT_CAST_TITLE_FORCED)
    return true;
#else
    if (const char* e = std::getenv("MPX_YT_CAST_TITLE"); e && e[0])
        return ytCastTitleEnvOn(e);
    return ytCastTitleEnvOn(std::getenv("YT_CAST_TITLE"));
#endif
}

// Prefixed so a title line cannot be collected as a media URL. The '%' makes
// yt-dlp treat this as an output template and fill %(title)s from the extract.
inline constexpr char kYtCastTitlePrefix[] = "MPX_TITLE:";
inline constexpr char kYtCastTitlePrint[] = "MPX_TITLE:%(title)s";
inline constexpr size_t kYtCastTitleMax = 120;

// Flag off, or an empty extract, keeps the tip string. Flag on with a title
// replaces it. The caller still runs that string through gcastNowPlaying.
inline std::string ytCastPlaybackTitle(bool enabled, const std::string& extractTitle,
                                      const std::string& fallback) {
#ifdef MUT_YT_CAST_TITLE_ALWAYS
    (void)enabled;
    if (!extractTitle.empty())
        return extractTitle;
    return fallback;
#else
    if (enabled && !extractTitle.empty())
        return extractTitle;
    return fallback;
#endif
}

// AirPlay keeps probe-or-fallback. YouTube uses the extract only when the flag
// is on and the title is non-empty.
inline std::string ytCastShownTitle(bool youtube, bool enabled, const std::string& extractTitle,
                                   const std::string& probeOrFallback) {
#ifdef MUT_YT_CAST_TITLE_ANY_OWNER
    (void)youtube;
    return ytCastPlaybackTitle(enabled, extractTitle, probeOrFallback);
#else
    if (!youtube)
        return probeOrFallback;
    return ytCastPlaybackTitle(enabled, extractTitle, probeOrFallback);
#endif
}

// GH#8 bot-check: a Netscape cookies file lets on-card yt-dlp pass "Sign in to
// confirm you're not a bot". Env wins; a file at the default path is used when
// present. A missing path is omitted (yt-dlp would fail the whole resolve).
inline const char* ytCookiesPathFromEnv() {
#ifdef MUT_GH8_NO_COOKIES
    return nullptr;
#else
    if (const char* v = std::getenv("AIRPLAY_YT_COOKIES"); v && v[0])
        return v;
    if (const char* v = std::getenv("GCAST_YT_COOKIES"); v && v[0])
        return v;
    if (const char* v = std::getenv("YT_COOKIES"); v && v[0])
        return v;
    return "/media/fat/misterplex/youtube.cookies";
#endif
}

inline bool ytCookiesFileOk(const char* path) {
    if (!path || !path[0])
        return false;
    struct stat st {};
    if (::stat(path, &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

inline void appendYtCookiesArgs(std::vector<std::string>& args) {
    const char* p = ytCookiesPathFromEnv();
    if (!ytCookiesFileOk(p))
        return;
    args.emplace_back("--cookies");
    args.emplace_back(p);
}

// Standalone -g argv. extractorArgs null is the default client (no extractor-args).
// #339 review N2: videoId is a bare video id only; the argv always ends in a
// watch?v= URL and --use-extractors youtube loads only the Youtube IE, so a playlist,
// channel or tab URL passed here would fail as unsupported.
// YT_CAST_TITLE inserts --print of the extract title immediately before the watch URL.
inline std::vector<std::string> localResolveArgs(const std::string& videoId, const char* extractorArgs,
                                                 const char* format = nullptr) {
    std::vector<std::string> args;
    args.emplace_back("-f");
    args.emplace_back(format && format[0] ? format : kResolveFormat);
    args.emplace_back("-g");
    args.emplace_back("--no-playlist");
    args.emplace_back("--no-warnings");
    appendYtCacheArgs(args);
#ifndef MUT_339_ALL_EXTRACTORS
    // #339: only the youtube extractors are matched/initialised (1747 load ~3 s idle on ARM).
    args.emplace_back("--use-extractors");
    args.emplace_back("youtube");
#endif
    if (extractorArgs && extractorArgs[0]) {
        args.emplace_back("--extractor-args");
        args.emplace_back(extractorArgs);
    }
    appendYtCookiesArgs(args);
    args.emplace_back(std::string("https://www.youtube.com/watch?v=") + videoId);
#ifndef MUT_YT_CAST_TITLE_DROP_PRINT
    if (ytCastTitleEnabled() && !args.empty()) {
        std::string watch = std::move(args.back());
        args.pop_back();
        args.emplace_back("--print");
        args.emplace_back(kYtCastTitlePrint);
        args.push_back(std::move(watch));
    }
#endif
    return args;
}

inline std::mutex& resolveMu() {
    static std::mutex m;
    return m;
}

// Play resolves waiting for resolveMu. A caption extract stops before its
// next ladder rung when a play is waiting (play start waits at most one rung).
inline std::atomic<int>& playResolveWaiters() {
    static std::atomic<int> n{0};
    return n;
}

inline unsigned readVmRssKb(int pid, unsigned* hwmKb) {
    if (hwmKb)
        *hwmKb = 0;
    const std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream in(path);
    if (!in)
        return 0;
    unsigned rss = 0;
    std::string line;
    while (std::getline(in, line)) {
        unsigned v = 0;
        if (std::sscanf(line.c_str(), "VmRSS: %u", &v) == 1)
            rss = v;
        else if (hwmKb && std::sscanf(line.c_str(), "VmHWM: %u", &v) == 1)
            *hwmKb = v;
    }
    return rss;
}

inline void notePeak(unsigned kb) {
    if (kb)
        mem_stat::noteYtdlpPeakKb(kb);
}

inline bool extractionText(const std::string& err) {
    return err.find("Unable to extract") != std::string::npos ||
           err.find("Unsupported URL") != std::string::npos ||
           err.find("Signature extraction") != std::string::npos ||
           err.find("nsig extraction") != std::string::npos;
}

inline void collectUrls(const std::string& text, std::string& first, std::string& second, int* count) {
    int n = 0;
    first.clear();
    second.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 7, "http://") == 0 || line.compare(0, 8, "https://") == 0) {
            if (n == 0)
                first = line;
            else if (n == 1)
                second = line;
            ++n;
        }
    }
    if (count)
        *count = n;
}

// Drop controls, cap the length, and trim. "NA" is yt-dlp's empty-field marker.
inline std::string sanitizeCastTitle(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (out.size() >= kYtCastTitleMax)
            break;
        if (c == '\t') {
            if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
            continue;
        }
        if (c < 0x20 || c == 0x7f)
            continue;
        out.push_back(static_cast<char>(c));
    }
    size_t b = 0;
    while (b < out.size() && out[b] == ' ')
        ++b;
    size_t e = out.size();
    while (e > b && out[e - 1] == ' ')
        --e;
    out = out.substr(b, e - b);
    if (out == "NA")
        return {};
    return out;
}

// Pull MPX_TITLE: lines out of the child stdout before URL collection.
// Flag off leaves the buffer untouched (tip path). No prefix leaves it untouched.
inline std::string peelCastTitle(std::string& text) {
#ifdef MUT_YT_CAST_TITLE_DROP_PARSE
    (void)text;
    return {};
#else
    if (!ytCastTitleEnabled())
        return {};
    const std::string prefix = kYtCastTitlePrefix;
    if (text.find(prefix) == std::string::npos)
        return {};
    std::string title;
    std::string kept;
    std::istringstream in(text);
    std::string line;
    bool anyKept = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0) {
            if (title.empty())
                title = sanitizeCastTitle(line.substr(prefix.size()));
            continue;
        }
        if (anyKept)
            kept.push_back('\n');
        anyKept = true;
        kept += line;
    }
    text.swap(kept);
    return title;
#endif
}

// Spawn one child. Caller holds resolveMu when this is a YouTube resolve.
inline ResolveResult runResolveChild(const ResolveOptions& opt) {
    ResolveResult r;
    if (opt.bin.empty() || opt.videoId.empty()) {
        r.error = "yt-dlp bin or video id missing";
        return r;
    }
    ensureYtCacheDir();
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        r.error = "pipe failed";
        return r;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        r.error = "fork failed";
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        return r;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
#ifndef MUT_339_NO_PRIO
        if (opt.nice < 0)   // boost: clamped (#339 review N1)
            (void)::setpriority(PRIO_PROCESS, 0, clampResolveNice(opt.nice));
        else if (opt.nice > 0)   // lowering is always permitted (tests)
            (void)::setpriority(PRIO_PROCESS, 0, opt.nice);
#endif
        ::dup2(outPipe[1], 1);
        ::dup2(errPipe[1], 2);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        std::vector<std::string> store;
        if (opt.zipapp)
            store.push_back(opt.python);
        store.push_back(opt.bin);
        if (!opt.extraArgs.empty()) {
            store.insert(store.end(), opt.extraArgs.begin(), opt.extraArgs.end());
        } else {
            const std::vector<std::string> args = localResolveArgs(opt.videoId, nullptr, resolveFormatOf(opt));
            store.insert(store.end(), args.begin(), args.end());
        }
        std::vector<char*> argv;
        argv.reserve(store.size() + 1);
        for (auto& s : store)
            argv.push_back(s.data());
        argv.push_back(nullptr);
        ::execv(opt.zipapp ? opt.python.c_str() : opt.bin.c_str(), argv.data());
        std::fprintf(stderr, "exec failed: %s\n", std::strerror(errno));
        _exit(127);
    }
    ::setpgid(pid, pid);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);
    std::string out, err;
    const auto t0 = std::chrono::steady_clock::now();
    bool childDone = false;
    int status = 0;
    auto drain = [](int fd, std::string& dest) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0)
                dest.append(buf, buf + n);
            else
                break;
        }
    };
    while (!childDone) {
        pollfd pf[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
        ::poll(pf, 2, 50);
        drain(outPipe[0], out);
        drain(errPipe[0], err);
        unsigned hwm = 0;
        const unsigned rss = readVmRssKb(static_cast<int>(pid), &hwm);
        if (hwm > r.peakRssKb)
            r.peakRssKb = hwm;
        if (rss > r.peakRssKb)
            r.peakRssKb = rss;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (opt.rssCapKb > 0 && rss > opt.rssCapKb) {
            r.rssKilled = true;
            ::killpg(pid, SIGKILL);
        } else if (opt.timeoutMs > 0 && ms >= opt.timeoutMs) {
            r.timedOut = true;
            ::killpg(pid, SIGKILL);
        } else if (opt.abort && opt.abort()) {
            r.aborted = true;
            ::killpg(pid, SIGKILL);
        }
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid)
            childDone = true;
        else if (r.rssKilled || r.timedOut || r.aborted) {
            ::killpg(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            childDone = true;
        }
    }
    drain(outPipe[0], out);
    drain(errPipe[0], err);
    ::close(outPipe[0]);
    ::close(errPipe[0]);
    unsigned hwm = 0;
    readVmRssKb(static_cast<int>(pid), &hwm);
    if (hwm > r.peakRssKb)
        r.peakRssKb = hwm;
    notePeak(r.peakRssKb);
    if (WIFEXITED(status))
        r.exitCode = WEXITSTATUS(status);
    r.title = peelCastTitle(out);
    int urls = 0;
    std::string url, audio;
    collectUrls(out, url, audio, &urls);
    if (r.timedOut)
        r.error = "yt-dlp timed out";
    else if (r.rssKilled)
        r.error = "yt-dlp exceeded RSS cap";
    else if (r.exitCode != 0) {
        r.error = err.empty() ? "yt-dlp failed" : err;
        r.extractionError = extractionText(err);
    } else if (urls == 1) {
        r.ok = true;
        r.url = url;
    } else if (urls == 2) {
        // Video line then audio line. The player opens each URL whole and
        // seeks with avformat_seek_file. It does not append a byte offset.
        r.ok = true;
        r.url = url;
        r.audioUrl = audio;
    } else {
        r.error = urls == 0 ? "yt-dlp returned no url" : "yt-dlp returned more than a video and an audio url";
    }
    if (opt.extraArgs.empty()) {
        r.clientAttempts = 1;
        r.playerClient = "default";
    }
    return r;
}

inline bool cacheableUrl(const std::string& u);

inline std::optional<ResolveResult> resolveViaWarm(const ResolveOptions& opt) {
    // Custom/live selectors and caption/test argv keep the existing extractor contract.
    // skipWarm: 403 refresh must not re-mint another android URL (P2-c).
    if (opt.skipWarm || !opt.extraArgs.empty() || !opt.format.empty() || (opt.abort && opt.abort()))
        return {};
    auto warm = yt_warm::process().resolve(opt.videoId, std::min(opt.timeoutMs / 4, 5000), opt.abort);
    if (!warm)
        return {};
    if (!cacheableUrl(warm->url) || (!warm->audioUrl.empty() && !cacheableUrl(warm->audioUrl))) {
        std::fprintf(stderr, "misterplexd: yt-warm returned invalid media URLs; using one-shot\n");
        return {};
    }
    ResolveResult r;
    r.ok = true;
    r.url = std::move(warm->url);
    r.audioUrl = std::move(warm->audioUrl);
    if (ytCastTitleEnabled())
        r.title = std::move(warm->title);
    r.exitCode = 0;
    r.playerClient = "warm-android";
    r.clientAttempts = 1;
    return r;
}

// One resolve, holding the single-flight lock for every client it tries.
// extraArgs (tests) is one child. All clients share one timeout budget.
// Stop on the first URL; timeout, RSS kill or cancellation stops the ladder.
inline ResolveResult resolveVideo(const ResolveOptions& opt) {
    const auto t0 = std::chrono::steady_clock::now();
    playResolveWaiters().fetch_add(1);
    std::unique_lock<std::mutex> lk(resolveMu());
    playResolveWaiters().fetch_sub(1);
    if (auto warm = resolveViaWarm(opt))
        return *warm;
    if (!opt.extraArgs.empty() || opt.bin.empty() || opt.videoId.empty())
        return runResolveChild(opt);
#ifdef MUT_YT_NO_CLIENT_RETRY
    return runResolveChild(opt);
#else
    ResolveResult last;
    const int ladderStart = opt.skipAndroid ? 1 : 0; // index 0 is android (P2-c)
    for (int i = ladderStart; i < kYtPlayerClientRetryCount; ++i) {
        const YtClientSpec& spec = kYtPlayerClientRetry[i];
        ResolveOptions attempt = opt;
        if (opt.timeoutMs > 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (spent >= opt.timeoutMs) {
                last.ok = false;
                last.timedOut = true;
                last.error = "yt-dlp timed out";
                return last;
            }
            attempt.timeoutMs = opt.timeoutMs - static_cast<int>(spent);
        }
        attempt.extraArgs = localResolveArgs(opt.videoId, spec.extractorArgs, resolveFormatOf(opt));
        last = runResolveChild(attempt);
        last.clientAttempts = (i - ladderStart) + 1;
        last.playerClient = spec.name ? spec.name : "";
        if (last.timedOut || last.rssKilled || last.aborted)
            return last;
        if (!last.ok)
            std::fprintf(stderr, "misterplexd: yt-dlp no url client=%s attempts=%d\n", spec.name, i + 1);
#ifndef MUT_YT_CLIENT_RETRY_WALK
        if (last.ok)
            return last;
#endif
    }

    return last;
#endif
}

inline std::string resolveFailureIdleLine(const ResolveResult& r) {
    if (r.timedOut || r.error.find("timed out") != std::string::npos)
        return "YouTube lookup timed out";
    std::istringstream words(misterplex::redactSensitive(r.error));
    std::string text, word;
    while (words >> word) {
        if (word.find("://") != std::string::npos || word.find("www.") != std::string::npos)
            continue;
        for (unsigned char c : word) {
            if (c >= 32 && c < 127)
                text.push_back(static_cast<char>(c));
        }
        text.push_back(' ');
    }
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    if (text.empty())
        return "YouTube lookup failed";
    return "YouTube: " + text.substr(0, 64);
}

// Subtitles for YT_CAPTIONS. Same binary, RSS cap, timeout, and player-client
// ladder as resolveVideo. yt-dlp writes the signed subtitle file; we read that
// file. An unsigned /api/timedtext URL is refused (HTTP 429, no cues).
// Holds the single-flight lock so a caption extract does not overlap -g.
inline std::string resolveCaptionBody(const std::string& bin, bool zipapp, const std::string& python,
                                      const yt_captions::YtCaptionRequest& req,
                                      const std::function<bool()>& stillWanted = {},
                                      std::string* why = nullptr,
                                      int totalBudgetMs = kCaptionTotalBudgetMs) {
    if (why)
        why->clear();
    if (!lounge::ytCaptionsEnabled() || bin.empty())
        return {};
    if (!yt_captions::captionArgsAreYtdlp(yt_captions::captionYtdlpArgs(req, "out", nullptr)))
        return {};
    std::lock_guard<std::mutex> lk(resolveMu());
    char tmpl[] = "/tmp/mpx-ytcap-XXXXXX";
    if (!::mkdtemp(tmpl))
        return {};
    const std::string dir = tmpl;
    const std::string stem = dir + "/" + req.videoId;
    std::string body;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kYtPlayerClientRetryCount; ++i) {
        // Superseded request (newer toggle): no yt-dlp child.
        if (stillWanted && !stillWanted())
            break;
#ifndef MUT_LP_CAPTIONS_IGNORE_PLAY_WAITER
        if (i > 0 && playResolveWaiters().load() > 0)
            break;
#endif
        const long spent = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
#ifdef MUT_LP_CAPTIONS_NO_BUDGET
        const long left = kCaptionTimeoutMs;
        (void)spent;
        (void)totalBudgetMs;
#else
        const long left = long(totalBudgetMs) - spent;
#endif
        if (left < 1000) {
            if (why)
                *why = "budget";
            break;
        }
        const YtClientSpec& spec = kYtPlayerClientRetry[i];
        ResolveOptions opt;
        opt.bin = bin;
        opt.zipapp = zipapp;
        opt.python = python.empty() ? "python3" : python;
        opt.videoId = req.videoId;
        opt.extraArgs = yt_captions::captionYtdlpArgs(req, stem, spec.extractorArgs);
#ifdef MUT_LP_CAPTIONS_SHORT_TIMEOUT
        opt.timeoutMs = 20000;
#else
        opt.timeoutMs = int(left < kCaptionTimeoutMs ? left : kCaptionTimeoutMs);
#endif
        opt.rssCapKb = kRssCapKb;
        opt.nice = 0;   // captions are not on the play path; do not outrank decode
        opt.abort = [&stillWanted] {
#ifdef MUT_LP_CAPTIONS_IGNORE_PLAY_WAITER
            return stillWanted && !stillWanted();
#else
            return (stillWanted && !stillWanted()) || playResolveWaiters().load() > 0;
#endif
        };
        if (!yt_captions::captionArgsAreYtdlp(opt.extraArgs))
            break;
        const ResolveResult r = runResolveChild(opt);
        body = yt_captions::captionTextInDir(dir);
        if (!body.empty())
            break;
        if (why)
            *why = r.timedOut ? "timeout" : r.rssKilled ? "rss" : r.aborted ? "aborted"
                 : ("exit=" + std::to_string(r.exitCode) + " client=" + (spec.name ? spec.name : "-"));
        if (r.timedOut || r.rssKilled || r.aborted)
            break;
    }
    yt_captions::removeCaptionDir(dir);
    return body;
}

// --- resolved-URL reuse (YouTube seek) --------------------------------------
// A YouTube seek is a re-play of the same video (yt.ts / ladder are not demux
// seekable), so castPlay used to run a full yt-dlp resolve again: 19-22 s on the
// card, enough to time out the seek. The googlevideo URLs it printed are still
// valid for hours. This keeps the last few resolved pairs per videoId and reuses
// one until shortly before its own expire= stamp (or a short default when the URL
// carries none). The cache never replaces the player's own checks: the yt-av
// allowlist (media_source_policy.hpp) still gates every playable, and a 403, an
// open failure or an expiry falls back to a real resolve.
inline constexpr int kCacheMarginSec = 120;     // stop reusing this long before expire=
inline constexpr int kCacheDefaultTtlSec = 300; // URL with no expire= stamp
inline constexpr int kCacheMaxTtlSec = 1800;    // never trust one entry longer than this
inline constexpr size_t kCacheMaxEntries = 4;

// expire=<epoch> (query) or /expire/<epoch>/ (path). 0 when absent or not a plain number.
inline int64_t urlExpireEpoch(const std::string& url) {
    for (const char* key : {"expire=", "/expire/"}) {
        size_t p = url.find(key);
        while (p != std::string::npos) {
            const bool query = key[0] == 'e';
            const bool okStart = !query || p == 0 || url[p - 1] == '?' || url[p - 1] == '&';
            size_t q = p + std::strlen(key);
            int64_t v = 0;
            size_t nd = 0;
            while (q < url.size() && url[q] >= '0' && url[q] <= '9' && nd < 12) {
                v = v * 10 + (url[q] - '0');
                ++q;
                ++nd;
            }
            if (okStart && nd > 0 && (q >= url.size() || !(url[q] >= '0' && url[q] <= '9')))
                return v;
            p = url.find(key, p + 1);
        }
    }
    return 0;
}

struct CachedResolve {
    std::string url;
    std::string audioUrl;
    std::time_t validUntil = 0;   // epoch seconds
    std::string title;            // empty when the resolve did not print one
};

// Plain http(s), no space or control byte (same shape rule as media_source_policy.hpp).
inline bool cacheableUrl(const std::string& u) {
    if (!(u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) || u.size() <= 8)
        return false;
    for (unsigned char c : u)
        if (c <= ' ' || c == 0x7f)
            return false;
    return true;
}

class ResolveCache {
public:
    // Expiry a result would get at `now`: the earliest expire= of its URLs minus the margin,
    // capped by kCacheMaxTtlSec; kCacheDefaultTtlSec when no URL has a stamp.
    static std::time_t validUntilFor(const ResolveResult& r, std::time_t now) {
        std::time_t until = now + kCacheDefaultTtlSec;
        bool stamped = false;
        for (const std::string* u : {&r.url, &r.audioUrl}) {
            if (u->empty())
                continue;
            const int64_t e = urlExpireEpoch(*u);
            if (e <= 0)
                continue;
            const std::time_t t = static_cast<std::time_t>(e) - kCacheMarginSec;
            if (!stamped || t < until)
                until = t;
            stamped = true;
        }
        if (until > now + kCacheMaxTtlSec)
            until = now + kCacheMaxTtlSec;
        return until;
    }
    bool get(const std::string& id, std::time_t now, ResolveResult& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(id);
        if (it == map_.end())
            return false;
        if (now >= it->second.validUntil) {
            map_.erase(it);
            return false;
        }
        out = ResolveResult();
        out.ok = true;
        out.url = it->second.url;
        out.audioUrl = it->second.audioUrl;
        out.title = it->second.title;
        return true;
    }
    // Stores only a successful result whose URLs are plain http(s). Returns whether it was stored.
    bool put(const std::string& id, const ResolveResult& r, std::time_t now) {
        if (id.empty() || !r.ok || !cacheableUrl(r.url) || (!r.audioUrl.empty() && !cacheableUrl(r.audioUrl)))
            return false;
        const std::time_t until = validUntilFor(r, now);
        if (until <= now)
            return false;
        std::lock_guard<std::mutex> lk(mu_);
        if (map_.size() >= kCacheMaxEntries && !map_.count(id))
            map_.erase(map_.begin());
        map_[id] = CachedResolve{r.url, r.audioUrl, until, r.title};
        return true;
    }
    void invalidate(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        map_.erase(id);
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.size();
    }
private:
    std::mutex mu_;
    std::map<std::string, CachedResolve> map_;
};

// Any http(s):// span (to the next whitespace, quote or bracket) becomes "<url>". The log redactor does not
// know googlevideo's sig/lsig keys, so a quoted URL is dropped whole.
inline std::string stripUrls(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        const bool h = in.compare(i, 7, "http://") == 0 || in.compare(i, 8, "https://") == 0;
        if (!h) { out.push_back(in[i++]); continue; }
        out += "<url>";
        while (i < in.size() && !std::strchr(" \t\r\n\"'<>()[]", in[i])) ++i;
    }
    return out;
}

// The one stderr line for a resolve. It never carries the resolved URL (a signed googlevideo URL is a
// bearer token); a failure text is passed through the log redactor because yt-dlp errors can quote one.
inline std::string resolveLogLine(const ResolveResult& r, bool cacheHit) {
    std::string s = std::string("misterplexd: yt-dlp resolve ok=") + (r.ok ? "1" : "0") +
                    " cache=" + (cacheHit ? "hit" : "miss") + " peak_rss_kb=" + std::to_string(r.peakRssKb);
    if (r.clientAttempts > 0) {
        std::string client = r.playerClient.empty() ? std::string("default") : r.playerClient;
        for (char c : client) {
            const bool okCh = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ',' || c == '_' || c == '-';
            if (!okCh) {
                client = "invalid";
                break;
            }
        }
        s += " attempts=" + std::to_string(r.clientAttempts) + " client=" + client;
    }
    if (!r.ok)
        s += " " + misterplex::redactUrlTokens(stripUrls(r.error));
    return s;
}

inline ResolveCache& resolveCache() {
    static ResolveCache c;
    return c;
}

// Cached resolve. `hit` (optional) says whether a cached pair was returned. refresh=true skips the
// lookup (403 / open failure / caller wants a new URL), runs a real resolve and stores the result.
inline ResolveResult resolveVideoCached(const ResolveOptions& opt, ResolveCache& cache, std::time_t now,
                                        bool refresh = false, bool* hit = nullptr) {
    if (hit)
        *hit = false;
    if (refresh)
        cache.invalidate(opt.videoId);
    else {
        ResolveResult c;
        if (cache.get(opt.videoId, now, c)) {
            if (hit)
                *hit = true;
            return c;
        }
    }
    ResolveResult r = resolveVideo(opt);
    if (r.ok)
        cache.put(opt.videoId, r, now);
    return r;
}

// --- checksummed update ---------------------------------------------------

struct UpdateConfig {
    std::string baseUrl;   // no trailing slash; http://127.0.0.1:port in tests
    std::string asset = kOnedirZipAsset;
    std::string dest = kOnedirDir; // onedir directory (a file path for a non-zip asset)
    std::string stampPath = "/media/fat/linux/misterplex/yt-dlp.update-stamp";
    std::string caFile; // --cacert for https; empty uses mpx::caBundlePath()
    int intervalSec = 86400;
};

inline UpdateConfig configFromEnv() {
    UpdateConfig c;
    if (const char* b = std::getenv("MPX_YTDLP_RELEASE_BASE"))
        c.baseUrl = b;
    else
        c.baseUrl = "https://github.com/yt-dlp/yt-dlp/releases/latest/download";
    if (const char* d = std::getenv("MPX_YTDLP_DEST"))
        c.dest = d;
    if (const char* s = std::getenv("MPX_YTDLP_STAMP"))
        c.stampPath = s;
    if (const char* a = std::getenv("MPX_YTDLP_ASSET"))
        c.asset = a;
    c.caFile = mpx::caBundlePath();
    return c;
}

struct UpdateResult {
    bool ok = false;
    bool swapped = false;
    std::string error;
};

inline bool claimDailyUpdate(const std::string& stampPath, std::time_t now, int intervalSec) {
    if (stampPath.empty() || intervalSec <= 0)
        return false;
    std::ifstream in(stampPath);
    long prev = 0;
    if (in >> prev) {
        if (now >= prev && now - prev < intervalSec)
            return false;
    }
    std::ofstream out(stampPath, std::ios::trunc);
    if (!out)
        return false;
    out << now << "\n";
    return static_cast<bool>(out);
}

// Compact SHA-256 (FIPS 180-4). Public domain style, no OpenSSL link.
inline std::string sha256Hex(const uint8_t* data, size_t n) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    std::vector<uint8_t> msg(data, data + n);
    const uint64_t bitLen = static_cast<uint64_t>(n) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t)msg[off + i * 4] << 24 | (uint32_t)msg[off + i * 4 + 1] << 16 |
                   (uint32_t)msg[off + i * 4 + 2] << 8 | (uint32_t)msg[off + i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }
    const uint32_t hs[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    char hex[65];
    for (int i = 0; i < 8; ++i)
        std::snprintf(hex + i * 8, 9, "%08x", hs[i]);
    return hex;
}

inline std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sha256Hex(buf.data(), buf.size());
}

// SHA2-256SUMS lines: "<hex><spaces><filename>". Filename is the last field.
inline bool sumsMatch(const std::string& sums, const std::string& filename, const std::string& hex) {
    std::istringstream in(sums);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string got, name;
        if (!(ls >> got >> name))
            continue;
        if (name == filename && got.size() == hex.size()) {
            bool eq = true;
            for (size_t i = 0; i < got.size(); ++i) {
                char a = got[i], b = hex[i];
                if (a >= 'A' && a <= 'F')
                    a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'F')
                    b = static_cast<char>(b - 'A' + 'a');
                if (a != b)
                    eq = false;
            }
            if (eq)
                return true;
        }
    }
    return false;
}

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path;
    bool https = false;
    bool ok = false;
};

inline ParsedUrl parseHttpUrl(const std::string& url) {
    ParsedUrl u;
    const std::string http = "http://";
    const std::string https = "https://";
    std::string rest;
    if (url.compare(0, http.size(), http) == 0) {
        rest = url.substr(http.size());
        u.port = 80;
    } else if (url.compare(0, https.size(), https) == 0) {
        rest = url.substr(https.size());
        u.https = true;
        u.port = 443;
    } else {
        return u;
    }
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(':') == colon) {
        u.host = hostport.substr(0, colon);
        u.port = std::atoi(hostport.c_str() + colon + 1);
    } else {
        u.host = hostport;
    }
    u.ok = !u.host.empty() && u.port > 0;
    return u;
}

inline bool writeAllFd(int fd, const char* p, size_t n) {
    while (n) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// HTTP GET. https is not spoken here; the caller uses curl for https.
inline bool httpGet(const std::string& url, std::string& body, std::string& err, size_t capBytes = 80u * 1024u * 1024u) {
    body.clear();
    const ParsedUrl u = parseHttpUrl(url);
    if (!u.ok || u.https) {
        err = "httpGet only speaks http";
        return false;
    }
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port = std::to_string(u.port);
    if (getaddrinfo(u.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "dns failed";
        return false;
    }
    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        err = "socket failed";
        return false;
    }
    timeval tv {};
    tv.tv_sec = 10;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ::close(fd);
        freeaddrinfo(res);
        err = "connect failed";
        return false;
    }
    freeaddrinfo(res);
    const std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host + "\r\nConnection: close\r\n\r\n";
    if (!writeAllFd(fd, req.data(), req.size())) {
        ::close(fd);
        err = "send failed";
        return false;
    }
    std::string raw;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            ::close(fd);
            err = "recv failed";
            return false;
        }
        if (n == 0)
            break;
        if (raw.size() + static_cast<size_t>(n) > capBytes + 4096) {
            ::close(fd);
            err = "response too large";
            return false;
        }
        raw.append(buf, buf + n);
    }
    ::close(fd);
    const auto hdr = raw.find("\r\n\r\n");
    if (hdr == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) {
        err = "bad http response";
        return false;
    }
    const auto sp = raw.find(' ');
    const int code = sp == std::string::npos ? 0 : std::atoi(raw.c_str() + sp + 1);
    if (code != 200) {
        err = "http status " + std::to_string(code);
        return false;
    }
    body = raw.substr(hdr + 4);
    if (body.size() > capBytes) {
        err = "body too large";
        body.clear();
        return false;
    }
    return true;
}

inline bool downloadUrl(const std::string& url, const std::string& dest, std::string& err,
                       const std::string& caFile = "") {
    const ParsedUrl u = parseHttpUrl(url);
    if (!u.ok) {
        err = "bad url";
        return false;
    }
    if (!u.https) {
        std::string body;
        if (!httpGet(url, body, err))
            return false;
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            err = "write failed";
            return false;
        }
        return true;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        err = "fork curl failed";
        return false;
    }
    if (pid == 0) {
        const std::string ca = !caFile.empty() ? caFile : mpx::caBundlePath();
        if (ca.empty()) {
            std::fprintf(stderr, "yt-dlp: no CA bundle for %s\n", misterplex::logUrl(url).c_str());
            _exit(60);
        }
        ::execlp("curl", "curl", "-q", "-fsSL", "--cacert", ca.c_str(), "--max-time", "120", "-o", dest.c_str(),
                 url.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err = "curl download failed";
        return false;
    }
    return true;
}

inline bool atomicInstall(const std::string& tmp, const std::string& dest, std::string& err) {
    const std::string prev = dest + ".prev";
    ::chmod(tmp.c_str(), 0755);
    if (fileExists(dest)) {
        ::unlink(prev.c_str());
        if (::rename(dest.c_str(), prev.c_str()) != 0) {
            err = "could not keep previous version";
            return false;
        }
    }
    if (::rename(tmp.c_str(), dest.c_str()) != 0) {
        if (fileExists(prev))
            ::rename(prev.c_str(), dest.c_str());
        err = "atomic replace failed";
        return false;
    }
    return true;
}

inline bool restorePrevious(const std::string& dest, std::string& err) {
    const std::string prev = dest + ".prev";
    if (!fileExists(prev)) {
        err = "no previous version";
        return false;
    }
    const std::string tmp = dest + ".rollback";
    if (fileExists(dest) && ::rename(dest.c_str(), tmp.c_str()) != 0) {
        err = "could not move current aside";
        return false;
    }
    if (::rename(prev.c_str(), dest.c_str()) != 0) {
        if (fileExists(tmp))
            ::rename(tmp.c_str(), dest.c_str());
        err = "rollback rename failed";
        return false;
    }
    if (fileExists(tmp))
        ::rename(tmp.c_str(), prev.c_str());
    return true;
}

inline bool endsWithZip(const std::string& asset) {
    return asset.size() >= 4 && asset.compare(asset.size() - 4, 4, ".zip") == 0;
}

// Unzip an onedir archive into destDir. Previous tree is kept as destDir.prev.
// Writes a shell launcher; does not create a symlink.
inline bool installOnedirZip(const std::string& zipPath, const std::string& destDir, std::string& err) {
    if (destDir.empty()) {
        err = "empty yt-dlp dest";
        return false;
    }
    const auto slash = destDir.find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string(".") : destDir.substr(0, slash);
    ::mkdir(parent.c_str(), 0755);
    const std::string stage = parent + "/.yt-dlp-new." + std::to_string(::getpid());
    ::mkdir(stage.c_str(), 0755);
    const pid_t pid = ::fork();
    if (pid < 0) {
        err = "fork unzip failed";
        return false;
    }
    if (pid == 0) {
        ::execlp("unzip", "unzip", "-o", "-q", zipPath.c_str(), "-d", stage.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err = "unzip failed";
        return false;
    }
    std::string bin;
    std::function<void(const std::string&)> walk = [&](const std::string& dir) {
        if (!bin.empty())
            return;
        DIR* d = ::opendir(dir.c_str());
        if (!d)
            return;
        while (dirent* ent = ::readdir(d)) {
            if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
                continue;
            const std::string path = dir + "/" + ent->d_name;
            if (std::strcmp(ent->d_name, kStandaloneAsset) == 0 && fileExists(path)) {
                bin = path;
                break;
            }
            struct stat st {};
            if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                walk(path);
        }
        ::closedir(d);
    };
    walk(stage);
    if (bin.empty()) {
        err = "zip has no yt-dlp_linux_armv7l";
        return false;
    }
    const auto bslash = bin.find_last_of('/');
    const std::string src = bslash == std::string::npos ? stage : bin.substr(0, bslash);
    const std::string flat = parent + "/.yt-dlp-flat." + std::to_string(::getpid());
    ::mkdir(flat.c_str(), 0755);
    const pid_t cp = ::fork();
    if (cp == 0) {
        ::execlp("cp", "cp", "-a", (src + "/.").c_str(), flat.c_str(), nullptr);
        _exit(127);
    }
    int cpStatus = 0;
    if (cp > 0)
        ::waitpid(cp, &cpStatus, 0);
    if (cp < 0 || !WIFEXITED(cpStatus) || WEXITSTATUS(cpStatus) != 0) {
        err = "copy onedir failed";
        return false;
    }
    {
        std::ofstream launcher(flat + "/yt-dlp", std::ios::trunc);
        launcher << "#!/bin/sh\n"
                    "DIR=$(CDPATH= cd -- \"$(dirname \"$0\")\" && pwd)\n"
                    "exec \"$DIR/yt-dlp_linux_armv7l\" \"$@\"\n";
        if (!launcher) {
            err = "launcher write failed";
            return false;
        }
    }
    ::chmod((flat + "/yt-dlp").c_str(), 0755);
    ::chmod((flat + "/yt-dlp_linux_armv7l").c_str(), 0755);
    const std::string prev = destDir + ".prev";
    // best-effort remove of an old rollback tree
    const pid_t rm = ::fork();
    if (rm == 0) {
        ::execlp("rm", "rm", "-rf", prev.c_str(), nullptr);
        _exit(127);
    }
    if (rm > 0) {
        int rmStatus = 0;
        ::waitpid(rm, &rmStatus, 0);
    }
    struct stat st {};
    if (::stat(destDir.c_str(), &st) == 0) {
        if (::rename(destDir.c_str(), prev.c_str()) != 0) {
            err = "could not keep previous yt-dlp";
            return false;
        }
    }
    if (::rename(flat.c_str(), destDir.c_str()) != 0) {
        ::rename(prev.c_str(), destDir.c_str());
        err = "atomic replace failed";
        return false;
    }
    const pid_t rmStage = ::fork();
    if (rmStage == 0) {
        ::execlp("rm", "rm", "-rf", stage.c_str(), nullptr);
        _exit(127);
    }
    if (rmStage > 0) {
        int rmStatus = 0;
        ::waitpid(rmStage, &rmStatus, 0);
    }
    return true;
}

inline UpdateResult runVerifiedUpdate(const UpdateConfig& cfg) {
    UpdateResult r;
    if (cfg.baseUrl.empty() || cfg.dest.empty() || cfg.asset.empty()) {
        r.error = "update config incomplete";
        return r;
    }
    std::string base = cfg.baseUrl;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    std::string sums, err;
    const std::string sumsUrl = base + "/SHA2-256SUMS";
    const ParsedUrl su = parseHttpUrl(sumsUrl);
    if (su.https) {
        const std::string tmpSums = cfg.dest + ".sums";
        if (!downloadUrl(sumsUrl, tmpSums, err, cfg.caFile)) {
            r.error = err;
            return r;
        }
        std::ifstream in(tmpSums);
        sums.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        ::unlink(tmpSums.c_str());
    } else if (!httpGet(sumsUrl, sums, err, 1024u * 1024u)) {
        r.error = err;
        return r;
    }
    const std::string tmp = cfg.dest + ".tmp";
    if (!downloadUrl(base + "/" + cfg.asset, tmp, err, cfg.caFile)) {
        r.error = err;
        ::unlink(tmp.c_str());
        return r;
    }
    const std::string hex = sha256File(tmp);
    if (!sumsMatch(sums, cfg.asset, hex)) {
        r.error = "checksum mismatch";
        ::unlink(tmp.c_str());
        return r;
    }
    if (endsWithZip(cfg.asset)) {
        if (!installOnedirZip(tmp, cfg.dest, err)) {
            r.error = err;
            ::unlink(tmp.c_str());
            return r;
        }
        ::unlink(tmp.c_str());
        r.ok = true;
        r.swapped = true;
        return r;
    }
    if (!atomicInstall(tmp, cfg.dest, err)) {
        r.error = err;
        ::unlink(tmp.c_str());
        return r;
    }
    r.ok = true;
    r.swapped = true;
    return r;
}

// Returns as soon as the daily claim is decided. The download runs elsewhere.
inline bool scheduleDailyUpdate(const UpdateConfig& cfg) {
    const std::time_t now = std::time(nullptr);
    if (!wallTimePlausible(now))
        return false;
    if (!claimDailyUpdate(cfg.stampPath, now, cfg.intervalSec))
        return false;
    const UpdateConfig copy = cfg;
    std::thread([copy] { runVerifiedUpdate(copy); }).detach();
    return true;
}

inline bool noteResolveFailure(const ResolveResult& r, const UpdateConfig& cfg) {
    if (!r.extractionError)
        return false;
    return scheduleDailyUpdate(cfg);
}

}  // namespace misterplex::airplay::yt_dlp
